#include <gtest/gtest.h>

#include <atomic>
#include <numeric>

#include "plag/thread_pool.h"

using namespace plag;

TEST(ThreadPool, SubmitReturnsResults) {
  ThreadPool pool(4);
  std::vector<std::future<int>> fs;
  for (int i = 0; i < 100; ++i) fs.push_back(pool.submit([i] { return i * i; }));
  for (int i = 0; i < 100; ++i) EXPECT_EQ(fs[i].get(), i * i);
}

TEST(ThreadPool, ParallelForVisitsEachIndexOnce) {
  ThreadPool pool(8);
  std::vector<std::atomic<int>> hits(10007);
  pool.parallelFor(0, hits.size(), [&](size_t i) { hits[i]++; });
  for (auto& h : hits) EXPECT_EQ(h.load(), 1);
}

TEST(ThreadPool, WorkerSlotsAreExclusive) {
  ThreadPool pool(4);
  std::vector<long> per_slot(pool.size(), 0);
  pool.parallelFor(0, 100000, [&](size_t i, size_t slot) { per_slot[slot] += static_cast<long>(i); });
  EXPECT_EQ(std::accumulate(per_slot.begin(), per_slot.end(), 0L), 99999L * 100000L / 2);
}

TEST(ThreadPool, PropagatesExceptions) {
  ThreadPool pool(2);
  EXPECT_THROW(pool.parallelFor(0, 100, [](size_t i) { if (i == 42) throw std::runtime_error("x"); }),
               std::runtime_error);
  auto f = pool.submit([]() -> int { throw std::logic_error("y"); });
  EXPECT_THROW(f.get(), std::logic_error);
}
