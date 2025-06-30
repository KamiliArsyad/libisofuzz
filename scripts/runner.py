#!/usr/bin/env python3

"""
runner.py - The main fuzzing loop orchestrator for IsoFuzz.

This script orchestrates the entire fuzzing process by executing a series of
commands defined in a configuration file. It is designed to be flexible,
allowing users to specify full command prefixes with arguments in the config.
"""

import os
import sys
import subprocess as sp
import random
import json
import time
from tqdm import tqdm
from datetime import datetime
from pathlib import Path
from typing import Optional, Tuple

# --- Constants ---
# Classification results from Elle
ELLE_OK = "OK"
ELLE_VIOLATION = "VIOLATION"
ELLE_REALTIME_VIOLATION = "RT"
ELLE_ERROR = "ERROR"
ELLE_ANOMALY_KEYWORD = "false"  # In Elle's output, 'false' means no anomalies were found.
ELLE_REALTIME_KEYWORD = "realtime"  # Substring in violation filenames for realtime anomalies.


# --- Configuration Class ---
class FuzzConfig:
    """Encapsulates all configuration for the fuzzing run, loaded from a JSON file."""

    def __init__(self, config_path: str):
        print(f"Loading configuration from: {config_path}")
        with open(config_path, 'r') as f:
            config_data = json.load(f)

        # --- Mandatory command prefixes. These are treated as raw strings and can contain arguments. ---
        self.prog_bin: str = config_data['prog-bin']
        self.elle_bin: str = config_data['elle-bin']
        self.edn_maker_bin: str = config_data['edn-maker-bin']
        self.workload_bin: str = config_data['workload-bin']
        self.shutdown_cmd: str = config_data['shutdown-cmd']
        self.check_ready_cmd: str = config_data['check-ready-cmd']

        # --- Fuzzing parameters with sane defaults. ---
        self.iterations: int = config_data.get('iterations', 100)
        self.base_log_dir: Path = Path(config_data.get('base_log_dir', './fuzz_logs'))
        self.server_ready_timeout: int = config_data.get('server_ready_timeout', 30)

        # --- Mutation strategy ---
        self.should_mutate: bool = config_data.get('mutate', False)
        self.max_mutate_budget: int = config_data.get('max_mutate_budget', 16)

        # NOTE: We no longer validate path existence here, as the values can be
        # full commands with arguments. The responsibility is on the user
        # to ensure the commands in the config file are correct.


# --- State Management Class ---
class FuzzState:
    """Encapsulates the mutable state and statistics of the fuzzing run."""

    def __init__(self):
        self.start_time: str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self.iteration_logs: list[str] = []
        self.counts: dict[str, int] = {
            ELLE_OK: 0,
            ELLE_VIOLATION: 0,
            ELLE_REALTIME_VIOLATION: 0,
            ELLE_ERROR: 0
        }
        self.mutation_stats: dict[int, int] = {}


# --- Main Runner Class ---
class FuzzRunner:
    """Orchestrates the main fuzzing loop."""

    def __init__(self, config: FuzzConfig):
        self.config = config
        self.state = FuzzState()
        random.seed(42)  # Initialize for reproducibility
        self.config.base_log_dir.mkdir(parents=True, exist_ok=True)
        print("FuzzRunner initialized. Log directory:", self.config.base_log_dir)

    def _run_command(self, cmd: str, env: Optional[dict] = None, timeout: Optional[int] = None) -> sp.CompletedProcess:
        """Helper to run a command with shell=True, capturing output."""
        full_env = os.environ.copy()
        if env:
            full_env.update({k: str(v) for k, v in env.items()})
        return sp.run(cmd, shell=True, capture_output=True, text=True, env=full_env, timeout=timeout)

    def _wait_for_server_ready(self) -> bool:
        """Wait for the DBMS server to be ready for connections."""
        start = time.time()
        while time.time() - start < self.config.server_ready_timeout:
            result = self._run_command(self.config.check_ready_cmd)
            if result.returncode == 0:
                return True
            time.sleep(0.5)
        return False

    def _shutdown_server_with_retries(self, iteration: int):
        """Attempt to shut down the server, retrying with exponential backoff."""
        for i in range(6):
            try:
                result = self._run_command(self.config.shutdown_cmd, timeout=10)
                if result.returncode == 0:
                    return
            except sp.TimeoutExpired:
                print(f"Shutdown command timed out at iteration {iteration}. Retrying ({i + 1}/6)...")
            time.sleep(2 ** i)
        print(f"FATAL: Could not shut down server after multiple retries. Exiting.")
        sys.exit(1)

    def _run_server_workload(self, run_dir: Path, iteration: int, seed: int) -> Optional[Path]:
        """Run the server and workload, return the path to the raw log file."""
        raw_log_file = run_dir / f"out_raw_{iteration}.log"
        env = {
            "RANDOM_SEED": str(seed),
            "OUT_FILE": str(raw_log_file)
        }

        # The user provides the full command for prog_bin, including arguments.
        # We use Popen as it's a long-running background process.
        server_process = sp.Popen(self.config.prog_bin, shell=True, env=env,
                                  stdout=sp.DEVNULL, stderr=sp.DEVNULL)

        try:
            if not self._wait_for_server_ready():
                print(f"Server did not become ready within timeout at iteration {iteration}")
                return None

            # The user provides the full command for workload_bin, including arguments.
            # We append the -L flag for per-iteration log directories.
            workload_cmd = f"{self.config.workload_bin} -L {run_dir / f'workload_output_{iteration}'}"
            result = self._run_command(workload_cmd, env={"RANDOM_SEED": seed})
            if result.returncode != 0:
                print(f"Workload failed at iteration {iteration} with exit code {result.returncode}")
                return None
        finally:
            self._shutdown_server_with_retries(iteration)
            try:
                server_process.wait(timeout=10)  # Wait for the process to terminate
            except sp.TimeoutExpired:
                print(f"WARN: Server process did not terminate gracefully. Killing.")
                server_process.kill()

        return raw_log_file

    def _run_elle_check(self, edn_file: Path, out_dir: Path) -> str:
        """Run Elle and classify the result."""
        result_file = out_dir / "elle_result.txt"

        # User provides the base elle command prefix. We append dynamic args.
        elle_cmd = f"{self.config.elle_bin} {edn_file} --directory {out_dir}"

        with open(result_file, 'w') as f:
            sp.run(elle_cmd, shell=True, stdout=f, stderr=f)

        with open(result_file, 'r') as f:
            content = f.read()

        if ELLE_ANOMALY_KEYWORD not in content.lower():
            return ELLE_OK

        for v_file in out_dir.iterdir():
            if v_file.suffix == ".txt" and ELLE_REALTIME_KEYWORD not in v_file.name.lower():
                return ELLE_VIOLATION

        return ELLE_REALTIME_VIOLATION

    def _process_trace(self, log_file: Path, run_dir: Path, iteration: int) -> Tuple[str, Optional[int]]:
        """Translate, optionally mutate, and verify a raw trace file."""
        edn_path = run_dir / f"out_translated_{iteration}.edn"
        elle_out_dir = run_dir / f"elle_output_{iteration}"
        elle_out_dir.mkdir(exist_ok=True)

        translator_cmd = f"{self.config.edn_maker_bin} {log_file} {edn_path}"
        result = self._run_command(translator_cmd)
        if result.returncode != 0:
            print(f"Translator failed at iteration {iteration}:\n{result.stderr}")
            return ELLE_ERROR, None

        classification = self._run_elle_check(edn_path, elle_out_dir)
        if classification == ELLE_VIOLATION or not self.config.should_mutate:
            return classification, None

        mutation_count = 1
        current_budget = 1
        while current_budget <= self.config.max_mutate_budget:
            mut_edn_path = run_dir / f"out_mutated_{iteration}_{mutation_count}.edn"
            mut_elle_dir = run_dir / f"elle_mutated_{iteration}_{mutation_count}"
            mut_elle_dir.mkdir(exist_ok=True)

            mutator_cmd = f"{self.config.edn_maker_bin} {log_file} {mut_edn_path} --mutate {current_budget}"
            result = self._run_command(mutator_cmd)
            if result.returncode != 0:
                print(f"Mutator failed for iteration {iteration} (budget {current_budget}):\n{result.stderr}")
                return ELLE_ERROR, mutation_count

            classification = self._run_elle_check(mut_edn_path, mut_elle_dir)
            if classification == ELLE_VIOLATION:
                return classification, mutation_count

            mutation_count += 1
            current_budget *= 2

        return classification, mutation_count

    def _update_summary(self, iteration: int, classification: str, seed: int, mutation_count: Optional[int]):
        """Update the summary file with results from the current iteration."""
        self.state.counts[classification] += 1
        if classification == ELLE_VIOLATION and mutation_count:
            self.state.mutation_stats[iteration] = mutation_count

        log_entry = f"Iteration {iteration:04d}: {classification:<9} (seed: {seed}"
        if mutation_count:
            log_entry += f", mutations: {mutation_count})"
        else:
            log_entry += ")"
        self.state.iteration_logs.append(log_entry)

        summary_file_path = self.config.base_log_dir / "summary.txt"
        header = f"Fuzzing run summary\nStarted at: {self.state.start_time}\nConfig: {json.dumps(vars(self.config), default=str, indent=2)}\n"
        stats_list = [
            f"Iterations completed: {iteration + 1}",
            f"{ELLE_OK}: {self.state.counts[ELLE_OK]}",
            f"{ELLE_VIOLATION}: {self.state.counts[ELLE_VIOLATION]}",
            f"{ELLE_REALTIME_VIOLATION}: {self.state.counts[ELLE_REALTIME_VIOLATION]}",
            f"{ELLE_ERROR}: {self.state.counts[ELLE_ERROR]}",
        ]
        stats = "\n--- Statistics ---\n" + "\n".join(stats_list)
        if self.state.mutation_stats:
            stats += (
                f"\n--- Mutation Statistics ---\n"
                f"Average mutations to find violation: {sum(self.state.mutation_stats.values()) / len(self.state.mutation_stats):.2f}\n"
            )

        body = "\n--- Iteration Log ---\n" + "\n".join(self.state.iteration_logs)
        with open(summary_file_path, 'w') as f:
            f.write(header + stats + body)

    def run(self):
        """The main fuzzing loop."""
        for i in tqdm(range(self.config.iterations), desc="Fuzzing", unit="iterations"):
            run_dir = self.config.base_log_dir / f"run_{i:04d}"
            run_dir.mkdir(exist_ok=True)
            run_seed = random.randint(0, 2 ** 32 - 1)

            raw_log_file = self._run_server_workload(run_dir, i, run_seed)
            if not raw_log_file:
                self._update_summary(i, ELLE_ERROR, run_seed, None)
                continue

            classification, mutation_count = self._process_trace(raw_log_file, run_dir, i)
            if classification == ELLE_VIOLATION:
                print(f"\nVIOLATION found at iteration {i}!")

            self._update_summary(i, classification, run_seed, mutation_count)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: python3 {sys.argv[0]} <path_to_config.json>")
        sys.exit(1)

    try:
        config = FuzzConfig(sys.argv[1])
        runner = FuzzRunner(config)
        runner.run()
    except (FileNotFoundError, KeyError) as e:
        print(f"Configuration Error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
