#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <random>
#include <set>

#include "plag/fingerprint.h"

using namespace plag;

namespace {
std::vector<uint32_t> randomIds(size_t n, uint32_t alphabet, std::mt19937& rng) {
  std::vector<uint32_t> v(n);
  for (auto& x : v) x = rng() % alphabet;
  return v;
}
}  // namespace

TEST(RabinKarp, RollingEqualsRecomputed) {
  std::mt19937 rng(1);
  auto ids = randomIds(500, 40, rng);
  const uint32_t k = 9;
  auto rolled = kgramHashes(ids, k);
  ASSERT_EQ(rolled.size(), ids.size() - k + 1);
  for (size_t i = 0; i < rolled.size(); i += 37) {
    std::vector<uint32_t> window(ids.begin() + i, ids.begin() + i + k);
    EXPECT_EQ(kgramHashes(window, k)[0], rolled[i]) << i;
  }
}

TEST(RabinKarp, EqualKgramsEqualHashesAndShortInput) {
  std::vector<uint32_t> ids = {1, 2, 3, 4, 1, 2, 3, 4};
  auto h = kgramHashes(ids, 4);
  EXPECT_EQ(h[0], h[4]);
  EXPECT_NE(h[0], h[1]);
  EXPECT_TRUE(kgramHashes({1, 2}, 4).empty());
}

TEST(Winnow, PicksRightmostMinimumPerWindow) {
  std::vector<uint64_t> h = {77, 74, 42, 17, 98, 50, 17, 98, 8, 88, 67, 39, 77, 74, 42, 17, 98};
  // Example from Schleimer et al. (w = 4): selected 17 17 8 39 17.
  auto fps = winnow(h, 4);
  std::vector<uint64_t> got;
  for (auto& f : fps) got.push_back(f.hash);
  EXPECT_EQ(got, (std::vector<uint64_t>{17, 17, 8, 39, 17}));
}

TEST(Winnow, ShortSequenceStillFingerprinted) {
  auto fps = winnow({5, 3, 9}, 8);
  ASSERT_EQ(fps.size(), 1u);
  EXPECT_EQ(fps[0].hash, 3u);
}

TEST(Winnow, DensityNearTwoOverWPlusOne) {
  std::mt19937_64 rng(3);
  std::vector<uint64_t> h(200000);
  for (auto& x : h) x = rng();
  for (uint32_t w : {4u, 8u, 16u}) {
    double density = static_cast<double>(winnow(h, w).size()) / h.size();
    EXPECT_NEAR(density, 2.0 / (w + 1), 0.15 * 2.0 / (w + 1)) << w;
  }
}

// Core guarantee: any common substring of >= w+k-1 tokens shares a fingerprint.
TEST(Winnow, GuaranteeThreshold) {
  std::mt19937 rng(7);
  FingerprintParams p{6, 5};
  const uint32_t t = p.k + p.w - 1;
  for (int trial = 0; trial < 300; ++trial) {
    auto shared = randomIds(t, 1000, rng);
    auto a = randomIds(rng() % 200, 1000, rng);
    auto b = randomIds(rng() % 200, 1000, rng);
    size_t ia = rng() % (a.size() + 1), ib = rng() % (b.size() + 1);
    a.insert(a.begin() + ia, shared.begin(), shared.end());
    b.insert(b.begin() + ib, shared.begin(), shared.end());
    std::vector<Token> ta, tb;
    for (auto x : a) ta.push_back({x, 1});
    for (auto x : b) tb.push_back({x, 1});
    auto sa = fingerprintSet(fingerprintTokens(ta, p));
    auto sb = fingerprintSet(fingerprintTokens(tb, p));
    std::vector<uint64_t> inter;
    std::set_intersection(sa.begin(), sa.end(), sb.begin(), sb.end(), std::back_inserter(inter));
    ASSERT_FALSE(inter.empty()) << "trial " << trial;
  }
}
