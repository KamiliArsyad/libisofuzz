#ifndef ISOFUZZ_CTX_H
#define ISOFUZZ_CTX_H

#include "isofuzz.h" // For public enums and IsoFuzzObject

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// The internal, concrete implementation of the opaque IsoFuzzTrx handle.
struct IsoFuzzTrxImpl
{
  // A unique ID assigned by this library upon creation. It's used for scheduling
  // and as the transaction's identifier *before* it is promoted.
  const uint64_t lib_id;

  // The permanent ID assigned by the DBMS. It is 0 until isofuzz_trx_promote is called.
  std::atomic<uint64_t> dbms_id;

  // The thread ID that began this transaction.
  const std::thread::id thread_id;

  IsoFuzzTrxImpl(uint64_t lib_id, std::thread::id thread_id)
    : lib_id(lib_id), dbms_id(0), thread_id(thread_id)
  {
  }
};

// A singleton context to hold all global state for the library.
class IsoFuzzContext
{
public:
  static IsoFuzzContext& getInstance()
  {
    static IsoFuzzContext instance;
    return instance;
  }

  // Prohibit copy/move operations
  IsoFuzzContext(const IsoFuzzContext&) = delete;
  void operator=(const IsoFuzzContext&) = delete;

  // Transaction Management
  isofuzz_trx_t begin_trx();
  void end_trx(isofuzz_trx_t handle);
  IsoFuzzTrxImpl* get_trx(isofuzz_trx_t handle);

private:
  IsoFuzzContext() : next_lib_id(1)
  {
  }

  ~IsoFuzzContext() = default;

  std::mutex m_trx_map_mutex;
  std::unordered_map<uint64_t, std::unique_ptr<IsoFuzzTrxImpl>> m_transactions;
  std::atomic<uint64_t> next_lib_id;
};

// Helper function to convert op enums to strings for logging
const char* op_type_to_string(IsoFuzzOpType op_type);

#endif // ISOFUZZ_CTX_H
