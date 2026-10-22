#include <gtest/gtest.h>

#include <random>

#include "plag/index.h"
#include "plag/thread_pool.h"

using namespace plag;

namespace {
std::vector<std::vector<uint64_t>> randomSets(size_t n, std::mt19937_64& rng) {
  std::vector<std::vector<uint64_t>> sets(n);
  for (auto& s : sets) {
    size_t sz = 5 + rng() % 60;
    for (size_t i = 0; i < sz; ++i) s.push_back(rng() % 2000);  // overlapping universe
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
  }
  return sets;
}
}  // namespace

TEST(Jaccard, Basic) {
  uint32_t shared;
  EXPECT_DOUBLE_EQ(jaccard({1, 2, 3}, {2, 3, 4}, &shared), 0.5);
  EXPECT_EQ(shared, 2u);
  EXPECT_DOUBLE_EQ(jaccard({1}, {2}, &shared), 0.0);
  EXPECT_DOUBLE_EQ(jaccard({}, {}, &shared), 0.0);
}

TEST(Index, CandidatesMatchBruteForceWithoutDfCutoff) {
  std::mt19937_64 rng(11);
  auto sets = randomSets(400, rng);
  ThreadPool pool(4);
  InvertedIndex idx;
  idx.build(sets, UINT32_MAX);
  for (double thr : {0.0, 0.05, 0.2}) {
    auto fast = scoreCandidates(sets, idx, thr, pool);
    auto slow = scoreAllPairs(sets, thr, pool);
    if (thr == 0.0) {
      // Brute force skips pairs with no overlap; so does the index.
      EXPECT_EQ(fast.size(), slow.size());
    }
    ASSERT_EQ(fast.size(), slow.size()) << thr;
    for (size_t i = 0; i < fast.size(); ++i) {
      EXPECT_EQ(fast[i].a, slow[i].a);
      EXPECT_EQ(fast[i].b, slow[i].b);
      EXPECT_EQ(fast[i].shared, slow[i].shared);
      EXPECT_DOUBLE_EQ(fast[i].jaccard, slow[i].jaccard);
    }
  }
}

TEST(Index, DropsBoilerplateFingerprints) {
  std::vector<std::vector<uint64_t>> sets = {{1, 100}, {1, 200}, {1, 300}, {1, 100}};
  InvertedIndex idx;
  idx.build(sets, 2);  // hash 1 is in 4 docs -> dropped
  EXPECT_EQ(idx.postings(1), nullptr);
  ASSERT_NE(idx.postings(100), nullptr);
  EXPECT_EQ(*idx.postings(100), (std::vector<uint32_t>{0, 3}));
  ThreadPool pool(2);
  auto pairs = scoreCandidates(sets, idx, 0.0, pool);
  ASSERT_EQ(pairs.size(), 1u);  // only docs 0 and 3 share a non-boilerplate hash
  EXPECT_EQ(pairs[0].a, 0u);
  EXPECT_EQ(pairs[0].b, 3u);
  EXPECT_DOUBLE_EQ(pairs[0].jaccard, 1.0 / 3.0);  // boilerplate still counts in the union
}
