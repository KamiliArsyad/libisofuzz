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
  return IsoFuzzContext::getInstance().begin_trx();
}

void isofuzz_trx_promote(isofuzz_trx_t trx_handle, uint64_t new_dbms_id)
{
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;

  trx->dbms_id.store(new_dbms_id, std::memory_order_relaxed);

  // Manually log the promotion event. This does not go through the scheduler.
  std::stringstream ss;
  ss << trx->thread_id << "\t"
    << new_dbms_id << "\t" // Log with the NEW permanent ID
    << op_type_to_string(IsoFuzzOpType::TXN_PROMOTE) << "\t"
    << "N/A\tN/A\t" << trx->lib_id; // Log the OLD library ID for correlation
  logger_log_line(ss.str());
}

void isofuzz_trx_end(isofuzz_trx_t trx_handle)
{
  IsoFuzzContext::getInstance().end_trx(trx_handle);
}

void isofuzz_schedule_op(isofuzz_trx_t trx_handle, IsoFuzzSchedulerIntent intent) {
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;

  // Call the scheduler and block. That's it.
  scheduler_request(trx->lib_id, intent);
}

void isofuzz_log_op(isofuzz_trx_t trx_handle, IsoFuzzOpType op_type,
                    const IsoFuzzObject& object, uint64_t last_writer_trx_id) {
  IsoFuzzTrxImpl* trx = IsoFuzzContext::getInstance().get_trx(trx_handle);
  if (!trx) return;

  // This function only builds the log line and writes it.
  std::stringstream ss;
  uint64_t effective_trx_id = trx->dbms_id.load(std::memory_order_relaxed);
  if (effective_trx_id == 0) {
    effective_trx_id = trx->lib_id;
  }

  ss << trx->thread_id << "\t"
     << effective_trx_id << "\t"
     << op_type_to_string(op_type) << "\t"
     << object.table_name << "\t"
     << (object.column_name ? object.column_name : "N/A") << "\t"
     << object.row_identifier << "\t";

  if (op_type == IsoFuzzOpType::READ || op_type == IsoFuzzOpType::WRITE_UPDATE || op_type == IsoFuzzOpType::WRITE_DELETE) {
    ss << last_writer_trx_id;
  } else {
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
  default: return "UNKNOWN";
  }
}
