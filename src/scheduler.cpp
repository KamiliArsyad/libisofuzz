#include "scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// State for a single waiting transaction thread.
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
static std::unordered_map<uint64_t, std::unique_ptr<TrxWaitInfo>> trx_wait_map;

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
      // Phase 1: Collect requests for a fixed duration.
      std::this_thread::sleep_for(EPOCH_DURATION_MS);

      std::queue<TrxPriority> local_pending;
      {
        // Atomically grab all pending requests.
        std::lock_guard lock(pending_queue_mutex);
        if (pending_requests.empty())
        {
          continue; // Nothing to do, start another collecting phase.
        }
        local_pending.swap(pending_requests);
      }

      // Transition to Draining phase.
      {
        std::lock_guard lock(scheduler_global_mutex);
        while (!local_pending.empty())
        {
          scheduler_queue.push(local_pending.front());
          local_pending.pop();
        }
        g_epoch_state.store(EpochState::DRAINING, std::memory_order_relaxed);
        // Wake up the scheduler thread in case it was waiting on an empty queue.
        scheduler_wakeup_cv.notify_one();
      }
    }
    else
    {
      // DRAINING state
      // Phase 2: Process the collected batch.
      std::unique_lock lock(scheduler_global_mutex);

      if (scheduler_queue.empty())
      {
        // Batch is drained, switch back to collecting.
        g_epoch_state.store(EpochState::COLLECTING, std::memory_order_relaxed);
        lock.unlock();
        continue;
      }

      // Get next transaction from the randomized queue.
      uint64_t next_trx_id = scheduler_queue.top().second;
      scheduler_queue.pop();

      auto it = trx_wait_map.find(next_trx_id);
      if (it != trx_wait_map.end())
      {
        std::unique_ptr<TrxWaitInfo> wait_info_ptr = std::move(it->second);
        trx_wait_map.erase(it);

        // Release the global lock before notifying the specific thread.
        lock.unlock();

        std::unique_lock trx_lock(wait_info_ptr->mtx);
        wait_info_ptr->is_ready = true;
        trx_lock.unlock();
        wait_info_ptr->cv.notify_one();
      }
      else
      {
        // Should not happen, but unlock if it does.
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
  }
}

// This function now just adds requests to the pending queue.
void scheduler_request(uint64_t trx_lib_id, IsoFuzzSchedulerIntent /* intent */)
{
  auto wait_info = std::make_unique<TrxWaitInfo>();
  TrxWaitInfo* wait_info_ptr = wait_info.get();
  TrxPriority priority_entry = {get_random_priority(), trx_lib_id};

  {
    // Add request to the pending queue.
    std::lock_guard lock(pending_queue_mutex);
    pending_requests.push(priority_entry);
  }

  {
    // Add the waiter information to the map.
    std::lock_guard lock(scheduler_global_mutex);
    trx_wait_map[trx_lib_id] = std::move(wait_info);
  }

  // Block this thread on its personal waiting room until the scheduler releases it.
  std::unique_lock trx_lock(wait_info_ptr->mtx);
  wait_info_ptr->cv.wait(trx_lock, [wait_info_ptr] { return wait_info_ptr->is_ready; });
}
