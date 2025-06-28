#ifndef ISOFUZZ_H
#define ISOFUZZ_H

#include <cstdint>

// Forward-declare the main handle struct to keep it opaque to the user.
// The adapter will only ever see a pointer to this.
struct IsoFuzzTrx;
using isofuzz_trx_t = IsoFuzzTrx*;

// The type of scheduler request. This allows for future feedback-driven
// scheduling by letting the scheduler know the context of a request.
enum class IsoFuzzSchedulerIntent
{
  TXN_BEGIN,
  TXN_COMMIT,
  TXN_ABORT,
  OP_READ,
  OP_WRITE // A generic write, covers INSERT, UPDATE, DELETE
};

// The specific type of data operation, used for precise logging.
enum class IsoFuzzOpType
{
  READ,
  WRITE_UPDATE,
  WRITE_INSERT,
  WRITE_DELETE,
  // This event logs the promotion of a read-only transaction to read-write,
  // capturing the point where it receives a permanent DBMS-assigned ID.
  TXN_PROMOTE
};

// Generic description of a data object involved in an operation.
struct IsoFuzzObject
{
  const char* table_name;
  const char* column_name; // Optional, can be null
  uint64_t row_identifier;
};

/*
 * ========================================================================
 * Library Lifecycle
 * ========================================================================
 */

/**
 * @brief Initializes the IsoFuzz library, scheduler, and logger.
 * Must be called once before any other IsoFuzz function. Reads environment
 * variables like RANDOM_SEED and OUT_FILE to configure its state.
 */
void isofuzz_init();

/**
 * @brief Shuts down the IsoFuzz library.
 * Stops the scheduler thread, joins it, flushes all pending log entries,
 * and cleans up resources. Should be called during the DBMS's graceful
 * shutdown sequence.
 */
void isofuzz_shutdown();


/*
 * ========================================================================
 * Transaction Management
 * ========================================================================
 */

/**
 * @brief Notifies the library that a new transaction is starting.
 * The library creates an internal representation for the transaction and assigns
 * it a temporary, library-local ID, which is essential for tracking read-only
 * transactions that do not yet have a DBMS-assigned ID.
 * @return An opaque handle to the transaction.
 */
isofuzz_trx_t isofuzz_trx_begin();

/**
 * @brief Notifies the library that a read-only transaction has been promoted to
 * a read-write transaction and now has a permanent, DBMS-assigned ID.
 * This function logs the promotion event, associating the library's temporary ID
 * with the new permanent ID. This is critical for reconstructing correct
 * dependency graphs.
 * @param trx_handle The existing opaque handle for the transaction.
 * @param new_dbms_id The new, permanent transaction ID assigned by the DBMS.
 */
void isofuzz_trx_promote(isofuzz_trx_t trx_handle, uint64_t new_dbms_id);

/**
 * @brief Notifies the library that a transaction has ended (committed or aborted).
 * The library will clean up any internal state associated with this handle.
 * @param trx_handle The handle of the transaction that is ending.
 */
void isofuzz_trx_end(isofuzz_trx_t trx_handle);


/*
 * ========================================================================
 * Scheduling and Logging
 * ========================================================================
 */

/**
 * @brief Submits a request to the scheduler and blocks until released.
 * This function ONLY handles scheduling and does NOT produce any log output.
 *
 * @param trx_handle The handle for the current transaction.
 * @param intent The purpose of the scheduling request (e.g., about to read).
 */
void isofuzz_schedule_op(isofuzz_trx_t trx_handle, IsoFuzzSchedulerIntent intent);

/**
 * @brief Logs the details of a specific data operation.
 * This function ONLY handles logging and does NOT block or schedule.
 *
 * @param trx_handle The handle for the current transaction.
 * @param op_type The specific operation type for logging.
 * @param object The data object being acted upon.
 * @param last_writer_trx_id For READ/UPDATE/DELETE, the ID of the transaction
 *                           that wrote the version being accessed.
 */
void isofuzz_log_op(isofuzz_trx_t trx_handle, IsoFuzzOpType op_type,
                    const IsoFuzzObject& object, uint64_t last_writer_trx_id);

#endif // ISOFUZZ_H
