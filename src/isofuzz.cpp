#include "../include/isofuzz.h"
#include "isofuzz_ctx.h"
#include "logger.h"
#include "scheduler.h"

#include <sstream>
#include <thread>

// --- IsoFuzzContext Implementation ---

isofuzz_trx_t IsoFuzzContext::begin_trx()
{
  std::lock_guard lock(m_trx_map_mutex);
  uint64_t id = next_lib_id.fetch_add(1, std::memory_order_relaxed);
  auto trx = std::make_unique<IsoFuzzTrxImpl>(id, std::this_thread::get_id());
  IsoFuzzTrxImpl* trx_ptr = trx.get();
  m_transactions[id] = std::move(trx);
  return reinterpret_cast<isofuzz_trx_t>(trx_ptr);
}

void IsoFuzzContext::end_trx(isofuzz_trx_t handle)
{
  if (!handle) return;
  IsoFuzzTrxImpl* trx = reinterpret_cast<IsoFuzzTrxImpl*>(handle);
  std::lock_guard lock(m_trx_map_mutex);
  m_transactions.erase(trx->lib_id);
}

IsoFuzzTrxImpl* IsoFuzzContext::get_trx(isofuzz_trx_t handle)
{
  if (!handle) return nullptr;
  return reinterpret_cast<IsoFuzzTrxImpl*>(handle);
}

// --- Public API Implementation ---

static void log_generic_op(isofuzz_trx_t trx_handle, IsoFuzzOpType op_type,
                           const IsoFuzzObject* object, uint64_t last_writer_trx_id);


void isofuzz_init()
{
  logger_init();
  scheduler_init();
}

void isofuzz_shutdown()
{
  scheduler_shutdown();
  logger_shutdown();
}

isofuzz_trx_t isofuzz_trx_begin()
{
  // 1. Create the internal handle.
  isofuzz_trx_t handle = IsoFuzzContext::getInstance().begin_trx();
  if (!handle) return nullptr;

  // 2. Schedule the BEGIN event.
  // isofuzz_schedule_op(handle, IsoFuzzSchedulerIntent::TXN_BEGIN);
  // Note: I removed the line above as this is causing deadlock for critical operations.
  //       same reasoning for the commit.

  // 3. Log the BEGIN event.
  log_generic_op(handle, IsoFuzzOpType::TXN_BEGIN, nullptr, 0);

  return handle;
}

void isofuzz_trx_commit(isofuzz_trx_t trx_handle)
{
  if (!trx_handle) return;

  // 1. Schedule the COMMIT event.
  // isofuzz_schedule_op(trx_handle, IsoFuzzSchedulerIntent::TXN_COMMIT);

  // 2. Log the COMMIT event.
  log_generic_op(trx_handle, IsoFuzzOpType::TXN_COMMIT, nullptr, 0);
}

void isofuzz_trx_promote(isofuzz_trx_t trx_handle, uint64_t new_dbms_id)
{
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;
  trx->dbms_id.store(new_dbms_id, std::memory_order_relaxed);
  log_generic_op(trx_handle, IsoFuzzOpType::TXN_PROMOTE, nullptr, trx->lib_id);
}

void isofuzz_trx_end(isofuzz_trx_t trx_handle)
{
  // This function is now ONLY for resource cleanup.
  IsoFuzzContext::getInstance().end_trx(trx_handle);
}

/*
 * ========================================================================
 * Data Operation API
 * ========================================================================
 */
void isofuzz_schedule_op(isofuzz_trx_t trx_handle, IsoFuzzSchedulerIntent intent)
{
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;
  scheduler_request(trx->lib_id, intent);
}

void isofuzz_log_op(isofuzz_trx_t trx_handle, IsoFuzzOpType op_type,
                    const IsoFuzzObject& object, uint64_t last_writer_trx_id)
{
  log_generic_op(trx_handle, op_type, &object, last_writer_trx_id);
}

/*
 * ========================================================================
 * Internal Logging Helper
 * ========================================================================
 */
static void log_generic_op(isofuzz_trx_t trx_handle, IsoFuzzOpType op_type,
                           const IsoFuzzObject* object, uint64_t last_writer_trx_id)
{
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;

  std::stringstream ss;
  uint64_t effective_trx_id = trx->dbms_id.load(std::memory_order_relaxed);
  if (effective_trx_id == 0)
  {
    effective_trx_id = trx->lib_id;
  }

  ss << trx->thread_id << "\t"
    << effective_trx_id << "\t"
    << op_type_to_string(op_type) << "\t";

  if (object)
  {
    ss << object->table_name << "\t"
      << (object->column_name ? object->column_name : "N/A") << "\t"
      << object->row_identifier << "\t";
  }
  else
  {
    ss << "N/A\tN/A\tN/A\t";
  }

  if (op_type == IsoFuzzOpType::TXN_PROMOTE)
  {
    // For PROMOTE, the last_writer_trx_id field is repurposed to log the old temp ID.
    ss << last_writer_trx_id;
  }
  else if (op_type == IsoFuzzOpType::READ || op_type == IsoFuzzOpType::WRITE_UPDATE || op_type ==
    IsoFuzzOpType::WRITE_DELETE)
  {
    ss << last_writer_trx_id;
  }
  else
  {
    ss << "0";
  }
  logger_log_line(ss.str());
}


// --- Helper Implementation ---
const char* op_type_to_string(IsoFuzzOpType op_type)
{
  switch (op_type)
  {
  case IsoFuzzOpType::READ: return "READ";
  case IsoFuzzOpType::WRITE_UPDATE: return "UPDATE";
  case IsoFuzzOpType::WRITE_INSERT: return "INSERT";
  case IsoFuzzOpType::WRITE_DELETE: return "DELETE";
  case IsoFuzzOpType::TXN_PROMOTE: return "PROMOTE";
  case IsoFuzzOpType::TXN_BEGIN: return "BEGIN";
  case IsoFuzzOpType::TXN_COMMIT: return "COMMIT";
  default: return "UNKNOWN";
  }
}
