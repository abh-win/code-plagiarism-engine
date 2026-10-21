#pragma once

// Fixed-size thread pool with a shared FIFO task queue.
//
//   ThreadPool pool(8);
//   auto f = pool.submit([] { return 42; });
//   pool.parallelFor(0, n, [&](size_t i) { ... });   // dynamic chunked scheduling

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace plag {

class ThreadPool {
 public:
  explicit ThreadPool(size_t threads = std::thread::hardware_concurrency()) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  size_t size() const { return workers_.size(); }

  template <typename F>
  auto submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    std::future<R> fut = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mu_);
      tasks_.emplace([task] { (*task)(); });
    }
    cv_.notify_one();
    return fut;
  }

  // Calls fn(i) for i in [begin, end). Indices are claimed in chunks from a
  // shared atomic counter, so uneven per-item cost balances itself. `fn` may
  // also take (i, worker_slot) to index per-worker scratch space.
  template <typename F>
  void parallelFor(size_t begin, size_t end, F&& fn, size_t chunk = 0) {
    if (begin >= end) return;
    size_t n = end - begin;
    size_t slots = std::min(size(), n);
    if (chunk == 0) chunk = std::max<size_t>(1, n / (slots * 16));
    std::atomic<size_t> next{begin};
    std::vector<std::future<void>> futs;
    futs.reserve(slots);
    for (size_t s = 0; s < slots; ++s) {
      futs.push_back(submit([&, s] {
        while (true) {
          size_t lo = next.fetch_add(chunk);
          if (lo >= end) break;
          size_t hi = std::min(end, lo + chunk);
          for (size_t i = lo; i < hi; ++i) {
            if constexpr (std::is_invocable_v<F, size_t, size_t>) fn(i, s);
            else fn(i);
          }
        }
      }));
    }
    for (auto& f : futs) f.get();  // rethrows the first task exception
  }

 private:
  void workerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
};

}  // namespace plag
