# IsoFuzz Runner

This script (`runner.py`) is the main orchestrator for the IsoFuzz testing framework. It automates the process of running the instrumented database, executing workloads, and analyzing the resulting traces for concurrency anomalies.

## How It Works

The runner executes a fuzzing loop. In each iteration, it performs the following steps:

1.  **Start Server**: Launches the instrumented DBMS server with a unique random seed.
2.  **Run Workload**: Executes a client workload (e.g., a shell script running SQL commands) against the server. This interaction generates a raw execution trace file (`out_raw_*.log`).
3.  **Translate Trace**: The raw log file is processed by a translator (`edn-maker-bin`) which converts it into the EDN (Extensible Data Notation) format required by the Elle verifier.
4.  **Verify**: The Elle verifier (`elle-bin`) is run on the EDN file. Elle analyzes the history for serializability violations.
5.  **Mutate (Optional)**: If the initial trace is valid but the `mutate` option is enabled in the config, the runner will repeatedly call the translator with an increasing mutation budget. This perturbs the schedule in the trace file to explore nearby execution states.
6.  **Log Results**: The outcome of the iteration (OK, VIOLATION, etc.) is logged to a central `summary.txt` file in the log directory.
7.  **Repeat**: The loop continues for the configured number of iterations.

## Prerequisites

Before running, ensure you have:
- A build of your instrumented DBMS.
- A compiled `edn-maker-bin` that can translate your raw logs.
- The `elle-cli.jar` verifier.
- A workload script that can be executed to stress the database.

## Configuration

The runner is configured via a single JSON file. You must create this file and pass its path as a command-line argument.

**The values for binary paths and commands are treated as raw strings. This means you can and should include any necessary command-line arguments directly in the string.**

**Example `config.json`:**
```json
{
  "prog-bin": "/path/to/mysqld --defaults-file=/path/my.cnf",
  "elle-bin": "java -jar /path/to/elle-cli.jar --model list-append",
  "edn-maker-bin": "python3 /path/to/translate_to_elle.py",
  "workload-bin": "/path/to/run-test.sh -r 2",
  "shutdown-cmd": "/path/to/mysqladmin -u root --socket=/path/mysql.sock shutdown",
  "check-ready-cmd": "/path/to/mysqladmin -u root --socket=/path/mysql.sock ping",
  "iterations": 1000,
  "base_log_dir": "/home/arkamili/isofuzz_runs",
  "server_ready_timeout": 45,
  "mutate": true,
  "max_mutate_budget": 32
}
```

I have also attached an example config that I usually use in `/configs`.

### Configuration Options:

- **`prog-bin`**: (string, required) The full command prefix to start the DBMS server executable.
- **`elle-bin`**: (string, required) The full command prefix to run the Elle verifier.
- **`edn-maker-bin`**: (string, required) The full command prefix for your trace-to-EDN translator.
- **`workload-bin`**: (string, required) The full command prefix for the script that runs the client workload. The runner will append a `-L <log_dir>` argument to this command.
- **`shutdown-cmd`**: (string, required) The full command used to gracefully shut down the DBMS server.
- **`check-ready-cmd`**: (string, required) The full command used to check if the DBMS server is ready. It should exit with code 0 on success.
- **`iterations`**: (integer, optional) The total number of fuzzing iterations to run. Defaults to `100`.
- **`base_log_dir`**: (string, optional) The path to the directory where all logs will be stored. Defaults to `./fuzz_logs`.
- **`server_ready_timeout`**: (integer, optional) Max seconds to wait for the server to become ready. Defaults to `30`.
- **`mutate`**: (boolean, optional) If `true`, enables the mutation strategy. Defaults to `false`.
- **`max_mutate_budget`**: (integer, optional) The highest mutation budget to try. Defaults to `16`.

## How to Run

Execute the script from your terminal, passing the path to your configuration file:

```bash
python3 runner.py /path/to/your/config.json
```