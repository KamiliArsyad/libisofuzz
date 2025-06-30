# libisofuzz

`libisofuzz` is a standalone, generic greybox fuzzing library designed to detect concurrency-related isolation level bugs in database management systems (DBMS).

Inspired by academic research in formal methods and concurrency testing (e.g., RFF, Elle), this library provides a framework for developers to systematically explore unexpected and bug-inducing interleavings of fine-grained database operations.

## Core Philosophy

Verifying isolation level correctness is notoriously difficult. Black-box testing often misses subtle, storage-level race conditions, while full white-box instrumentation can be prohibitively complex.

`libisofuzz` takes a balanced "greybox" approach:

1.  **High-Level Semantic Instrumentation:** Instead of hooking low-level system calls or memory accesses, the DBMS developer instruments the source code at logically significant points: the start and end of a transaction, and the physical read, write, insert, or delete of an individual row version.
2.  **Generic Adapter-Based Design:** The library itself is database-agnostic. The developer writes a thin "adapter" layer to translate the DBMS's internal structures (like transaction handles and object identifiers) into the generic API provided by `libisofuzz`.
3.  **Proactive Interleaving Exploration:** The core of the library is a scheduler designed to find bugs. It deliberately introduces small delays to batch concurrent operations, then executes them in a randomized order to uncover race conditions that would not appear in a typical, high-throughput execution.

## How it Works: The Epoch-Based Scheduler

The central challenge in high-level semantic fuzzing is creating meaningful interleavings from workloads that might appear sequential. `libisofuzz` solves this with an **Epoch-Based Centralized Scheduler**.

-   **`COLLECTING` Phase:** For a brief, configurable period (e.g., 5ms), the scheduler simply collects all incoming operation requests from all transaction threads and adds them to a pending queue. This allows a "batch" of concurrent operations to build up.
-   **`DRAINING` Phase:** After the collecting period, the scheduler takes the entire batch, shuffles it into a randomized priority queue, and begins executing the operations one by one.

This model is deadlock-free by design and is highly effective at turning seemingly serial request streams into highly concurrent test scenarios, dramatically increasing the chances of finding subtle isolation bugs.

## Runner
Please check the README in `/scripts` to understand how to run the `libisofuzz`-instrumented DBMS through the fuzzing cycle.

## Features

-   **Generic by Design:** Can be integrated into any thread-per-connection DBMS (like MySQL, PostgreSQL, etc.) by implementing a simple adapter.
-   **Fine-Grained Scheduling:** Interleaves individual row-level operations, not just entire SQL statements, allowing it to find deep storage-level anomalies.
-   **Deadlock-Free Scheduler:** The epoch-based model is inherently safe from self-deadlocks.
-   **Reproducibility:** Test runs can be made deterministic by providing a seed via an environment variable.
-   **Tunable:** The scheduling behavior can be tuned via environment variables to suit different workloads.

## Prerequisites

-   **CMake** (version 3.18 or higher)
-   A **C++20** compliant compiler (e.g., GCC 10+, Clang 12+)

## Building the Library

You can build `libisofuzz` as a static library (`libisofuzz.a`) using the standard CMake workflow.

```bash
# 1. Clone the repository (if you haven't already)
git clone git@github.com:KamiliArsyad/libisofuzz.git 
# cd libisofuzz

# 2. Create a build directory
mkdir build
cd build

# 3. Configure the project with CMake
cmake ..

# 4. Compile the library
make
```

Upon successful compilation, the static library `libisofuzz.a` will be located in the `build/` directory.

## Installation

To use `libisofuzz` in another project (like your MySQL fork), you can install it to a local prefix. This is the recommended approach for development.

From within the `build` directory, run the following command:

```bash
# Install the library, headers, and CMake config files to a local 'install' directory
cmake --install . --prefix ./install
```

After running this command, your `build/` directory will contain an `install/` folder with the following structure, ready to be used by another CMake project:

```
install/
├── include/
│   └── isofuzz.h
├── lib/
│   ├── libisofuzz.a
│   └── cmake/
│       └── isofuzz/
│           ├── isofuzz-targets.cmake
│           └── ...
└── ...
```

## Integration

To use `libisofuzz` in your DBMS project:

1.  **Link the Library:** In your DBMS's `CMakeLists.txt`, you need to find the `libisofuzz` package and link against it.
2.  **Create an Adapter:** Write a thin C++ adapter that includes `isofuzz.h` and the necessary internal headers from your DBMS. This adapter will translate calls.
3.  **Instrument the Code:** At the desired instrumentation points, call your adapter functions.

### Key API Functions

Your adapter will primarily use the following functions from `isofuzz.h`:

-   `isofuzz_init() / isofuzz_shutdown()`: Manage the library lifecycle.
-   `isofuzz_trx_begin()`: Starts a new transaction, schedules, and logs the `BEGIN` event.
-   `isofuzz_trx_commit(handle)`: Schedules and logs the `COMMIT` event.
-   `isofuzz_trx_end(handle)`: Cleans up the transaction's resources *after* it has been committed/aborted.
-   `isofuzz_schedule_op(handle, intent)`: The primary function for scheduling a fine-grained data operation. This blocks.
-   `isofuzz_log_op(handle, ...)`: The primary function for logging the details of a data operation *after* `isofuzz_schedule_op` has returned.

## Configuration

The library's behavior is controlled by the following environment variables:

| Variable           | Description                                                                                             | Default |
| ------------------ | ------------------------------------------------------------------------------------------------------- | ------- |
| `RANDOM_SEED`      | An integer seed for the random number generator to ensure reproducible test runs.                         | `42`      |
| `OUT_FILE`         | The absolute or relative path to the output log file where the execution trace will be written.           | `stdout`  |
| `ISOFUZZ_EPOCH_MS` | The duration in milliseconds for the scheduler's `COLLECTING` phase. Higher values create larger batches. | `5`       |
