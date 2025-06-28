#include "scheduler.h"

#include <atomic>
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

// --- Global Scheduler State ---
static std::thread scheduler_thread;
static std::atomic<bool> scheduler_running(false);
static std::mutex scheduler_global_mutex;
static std::condition_variable scheduler_wakeup_cv;

// Map from a transaction's library ID to its personal wait info.
static std::unordered_map<uint64_t, std::unique_ptr<TrxWaitInfo>> trx_wait_map;

// The main priority queue of transactions waiting for their turn.
static std::priority_queue<TrxPriority, std::vector<TrxPriority>, CompareTrxPriority> scheduler_queue;

// --- RNG for random priorities ---
static std::mt19937 rng;

static void init_rng()
{
  int seed = 42; // Default seed
  const char* seed_str = std::getenv("RANDOM_SEED");
  if (seed_str != nullptr)
  {
    try
    {
      seed = std::stoi(seed_str);
    }
    catch (const std::exception&)
    {
      // Keep default seed if conversion fails
    }
  }
  rng.seed(seed);
}

static int get_random_priority()
{
  std::uniform_int_distribution<int> dist(0, 1000000);
  return dist(rng);
}

// --- Main Scheduler Thread Logic ---
static void trx_scheduler_run()
{
  while (scheduler_running.load(std::memory_order_acquire))
  {
    std::unique_ptr<TrxWaitInfo> wait_info_ptr;
    uint64_t next_trx_id = 0;

    {
      std::unique_lock lock(scheduler_global_mutex);
      scheduler_wakeup_cv.wait(lock, []
      {
        return !scheduler_running.load(std::memory_order_acquire) || !scheduler_queue.empty();
      });

      if (!scheduler_running.load(std::memory_order_acquire))
      {
        break;
      }

      if (!scheduler_queue.empty())
      {
        next_trx_id = scheduler_queue.top().second;
        scheduler_queue.pop();

        auto it = trx_wait_map.find(next_trx_id);
        if (it != trx_wait_map.end())
        {
          wait_info_ptr = std::move(it->second);
          trx_wait_map.erase(it);
        }
      }
    }

    if (wait_info_ptr && next_trx_id != 0)
    {
      std::unique_lock trx_lock(wait_info_ptr->mtx);
      wait_info_ptr->is_ready = true;
      trx_lock.unlock();
      wait_info_ptr->cv.notify_one();
    }
  }
}

// --- Public Scheduler API Implementation ---

void scheduler_init()
{
  bool already_running = scheduler_running.exchange(true, std::memory_order_acq_rel);
  if (!already_running)
  {
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

void scheduler_request(uint64_t trx_lib_id, IsoFuzzSchedulerIntent /* intent */)
{
  // The 'intent' parameter is currently unused, but is available for future
  // feedback-driven scheduling logic where priorities could be assigned
  // based on the intended operation.

  auto wait_info = std::make_unique<TrxWaitInfo>();
  TrxWaitInfo* wait_info_ptr = wait_info.get();

  {
    std::lock_guard lock(scheduler_global_mutex);
    scheduler_queue.push({get_random_priority(), trx_lib_id});
    trx_wait_map[trx_lib_id] = std::move(wait_info);
  }

  scheduler_wakeup_cv.notify_one();

  std::unique_lock trx_lock(wait_info_ptr->mtx);
  wait_info_ptr->cv.wait(trx_lock, [wait_info_ptr] { return wait_info_ptr->is_ready; });
}
