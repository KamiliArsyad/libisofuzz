#include "scheduler.h"

#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// State for a single waiting transaction thread.
// The TrxWaitInfo object is now a plain C++ object. Its lifetime will be
// managed manually by the requesting thread.
struct TrxWaitInfo
{
  std::mutex mtx;
  std::condition_variable cv;
  bool is_ready = false;
};

// The scheduler queue holds pairs of <priority, library_trx_id>.
using TrxPriority = std::pair<int, uint64_t>;

struct CompareTrxPriority
{
  bool operator()(const TrxPriority& a, const TrxPriority& b) const
  {
    return a.first > b.first; // Min-heap on priority
  }
};

// --- NEW: Epoch-based state ---
enum class EpochState { COLLECTING, DRAINING };

static std::atomic<EpochState> g_epoch_state(EpochState::COLLECTING);
static std::chrono::milliseconds EPOCH_DURATION_MS(5);

// --- Global Scheduler State ---
static std::thread scheduler_thread;
static std::atomic<bool> scheduler_running(false);

// This mutex protects the main scheduler_queue and the trx_wait_map.
static std::mutex scheduler_global_mutex;
static std::condition_variable scheduler_wakeup_cv;

// This mutex protects the new pending_requests queue.
static std::mutex pending_queue_mutex;
static std::queue<TrxPriority> pending_requests;

// Map from a transaction's library ID to its personal wait info.
// Note: The trx_wait_map no longer holds owning unique_ptrs. It now holds
// non-owning raw pointers. The responsibility for deleting the TrxWaitInfo
// object now rests entirely with the thread that created it.
static std::unordered_map<uint64_t, TrxWaitInfo*> trx_wait_map;

// The main priority queue of transactions waiting for their turn.
static std::priority_queue<TrxPriority, std::vector<TrxPriority>, CompareTrxPriority> scheduler_queue;

// --- RNG for random priorities ---
static std::mt19937 rng;

static void init_rng()
{
  int seed = 42;
  const char* seed_str = std::getenv("RANDOM_SEED");
  if (seed_str != nullptr)
  {
    try
    {
      seed = std::stoi(seed_str);
    }
    catch (const std::exception&)
    {
    }
  }
  rng.seed(seed);
}

static int get_random_priority()
{
  std::uniform_int_distribution<int> dist(0, 1000000);
  return dist(rng);
}

// --- Main Scheduler Thread Logic (State Machine) ---
static void trx_scheduler_run()
{
  while (scheduler_running.load(std::memory_order_acquire))
  {
    if (g_epoch_state.load(std::memory_order_relaxed) == EpochState::COLLECTING)
    {
      std::this_thread::sleep_for(EPOCH_DURATION_MS);

      std::queue<TrxPriority> local_pending;
      {
        std::lock_guard lock(pending_queue_mutex);
        if (pending_requests.empty())
        {
          continue;
        }
        local_pending.swap(pending_requests);
      }

      {
        std::lock_guard lock(scheduler_global_mutex);
        while (!local_pending.empty())
        {
          scheduler_queue.push(local_pending.front());
          local_pending.pop();
        }
        g_epoch_state.store(EpochState::DRAINING, std::memory_order_relaxed);
        scheduler_wakeup_cv.notify_one();
      }
    }
    else
    {
      // DRAINING state
      std::unique_lock lock(scheduler_global_mutex);

      if (scheduler_queue.empty())
      {
        g_epoch_state.store(EpochState::COLLECTING, std::memory_order_relaxed);
        lock.unlock();
        continue;
      }

      uint64_t next_trx_id = scheduler_queue.top().second;
      scheduler_queue.pop();

      auto it = trx_wait_map.find(next_trx_id);
      if (it != trx_wait_map.end())
      {
        // --- CRITICAL CHANGE ---
        // We retrieve the raw pointer but DO NOT take ownership.
        TrxWaitInfo* wait_info_ptr = it->second;

        // We are done with this entry in the map, so we erase it.
        // This prevents other threads from ever seeing a stale pointer.
        // The worker thread still holds the valid pointer and is responsible for deletion.
        trx_wait_map.erase(it);

        // Release the global lock before notifying the specific thread.
        lock.unlock();

        // Wake up the worker thread.
        std::unique_lock trx_lock(wait_info_ptr->mtx);
        wait_info_ptr->is_ready = true;
        trx_lock.unlock();
        wait_info_ptr->cv.notify_one();
      }
      else
      {
        // This should not happen if the logic is correct.
        // It means a request was scheduled for which we have no waiter info.
        assert(false && "Scheduler found a transaction ID with no waiter info.");
        lock.unlock();
      }
    }
  }
}

// --- Public Scheduler API Implementation ---

void scheduler_init()
{
  bool already_running = scheduler_running.exchange(true, std::memory_order_acq_rel);
  if (!already_running)
  {
    const char* epoch_ms_str = std::getenv("ISOFUZZ_EPOCH_MS");
    if (epoch_ms_str)
    {
      try
      {
        long ms = std::stol(epoch_ms_str);
        if (ms > 0)
        {
          EPOCH_DURATION_MS = std::chrono::milliseconds(ms);
        }
      }
      catch (const std::exception&)
      {
      }
    }
    init_rng();
    scheduler_thread = std::thread(trx_scheduler_run);
  }
}

void scheduler_shutdown()
{
  if (scheduler_running.exchange(false, std::memory_order_acq_rel))
  {
    scheduler_wakeup_cv.notify_one();
    if (scheduler_thread.joinable())
    {
      scheduler_thread.join();
    }
    // Clean up any remaining waiters to prevent deadlocks on shutdown.
    std::lock_guard lock(scheduler_global_mutex);
    for (auto& pair : trx_wait_map)
    {
      pair.second->is_ready = true;
      pair.second->cv.notify_all();
    }
    trx_wait_map.clear();

    // --- INVARIANT CHECK ON SHUTDOWN ---
    // On a clean shutdown, the wait map should be empty. If it's not,
    // it implies some transactions were blocked and never cleaned up,
    // which would lead to a memory leak.
    assert(trx_wait_map.empty() && "trx_wait_map not empty on shutdown; memory will be leaked.");
  }
}

// This function now just adds requests to the pending queue.
void scheduler_request(uint64_t trx_lib_id, IsoFuzzSchedulerIntent /* intent */) {
  // Step 1: The worker thread allocates its own waiter object on the heap.
  TrxWaitInfo* wait_info_ptr = new TrxWaitInfo();

  TrxPriority priority_entry = {get_random_priority(), trx_lib_id};

  // Step 2: Add the request to the pending queue for the scheduler thread.
  {
    std::lock_guard lock(pending_queue_mutex);
    pending_requests.push(priority_entry);
  }

  // Step 3: Add the NON-OWNING pointer to the global map so the scheduler can find it.
  {
    std::lock_guard lock(scheduler_global_mutex);
    trx_wait_map[trx_lib_id] = wait_info_ptr;
  }

  // Step 4: The worker thread blocks, waiting for the scheduler to signal its CV.
  {
    std::unique_lock trx_lock(wait_info_ptr->mtx);
    wait_info_ptr->cv.wait(trx_lock, [wait_info_ptr] { return wait_info_ptr->is_ready; });
  }

  // Step 5: CRITICAL CLEANUP
  // Once woken up, the worker thread is now responsible for deleting the
  // waiter object it created. This happens on the same thread that called `new`,
  // eliminating the cross-thread destruction and the heap corruption bug.
  delete wait_info_ptr;
}
