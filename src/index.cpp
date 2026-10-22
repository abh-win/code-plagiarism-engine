#include "plag/index.h"

#include <algorithm>
#include <mutex>

#include "plag/thread_pool.h"

namespace plag {

void InvertedIndex::build(const std::vector<std::vector<uint64_t>>& sets, uint32_t max_df) {
  map_.clear();
  dropped_ = 0;
  total_postings_ = 0;
  size_t total = 0;
  for (const auto& s : sets) total += s.size();
  map_.reserve(total);
  for (uint32_t d = 0; d < sets.size(); ++d)
    for (uint64_t h : sets[d]) map_[h].push_back(d);  // ascending d by construction
  for (auto it = map_.begin(); it != map_.end();) {
    if (it->second.size() > max_df) {
      ++dropped_;
      it = map_.erase(it);
    } else {
      total_postings_ += it->second.size();
      ++it;
    }
  }
}

double jaccard(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, uint32_t* shared) {
  size_t i = 0, j = 0;
  uint32_t inter = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) ++i;
    else if (a[i] > b[j]) ++j;
    else {
      ++inter;
      ++i;
      ++j;
    }
  }
  if (shared) *shared = inter;
  size_t uni = a.size() + b.size() - inter;
  return uni == 0 ? 0.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

std::vector<PairScore> scoreCandidates(const std::vector<std::vector<uint64_t>>& sets,
                                       const InvertedIndex& index, double threshold,
                                       ThreadPool& pool) {
  const uint32_t n = static_cast<uint32_t>(sets.size());
  struct Scratch {
    std::vector<uint32_t> count;
    std::vector<uint32_t> touched;
    std::vector<PairScore> out;
  };
  std::vector<Scratch> scratch(pool.size());
  for (auto& s : scratch) s.count.assign(n, 0);

  pool.parallelFor(0, n, [&](size_t i, size_t slot) {
    Scratch& s = scratch[slot];
    for (uint64_t h : sets[i]) {
      const auto* post = index.postings(h);
      if (post == nullptr) continue;
      // Postings are sorted: only visit documents after i.
      auto it = std::upper_bound(post->begin(), post->end(), static_cast<uint32_t>(i));
      for (; it != post->end(); ++it) {
        if (s.count[*it]++ == 0) s.touched.push_back(*it);
      }
    }
    const double size_i = static_cast<double>(sets[i].size());
    for (uint32_t j : s.touched) {
      uint32_t inter = s.count[j];
      s.count[j] = 0;
      double uni = size_i + static_cast<double>(sets[j].size()) - inter;
      double jac = uni > 0 ? inter / uni : 0.0;
      if (jac >= threshold) s.out.push_back({static_cast<uint32_t>(i), j, inter, jac});
    }
    s.touched.clear();
  });

  std::vector<PairScore> all;
  for (auto& s : scratch) all.insert(all.end(), s.out.begin(), s.out.end());
  std::sort(all.begin(), all.end(), [](const PairScore& x, const PairScore& y) {
    if (x.jaccard != y.jaccard) return x.jaccard > y.jaccard;
    return x.a != y.a ? x.a < y.a : x.b < y.b;
  });
  return all;
}

std::vector<PairScore> scoreAllPairs(const std::vector<std::vector<uint64_t>>& sets,
                                     double threshold, ThreadPool& pool) {
  const uint32_t n = static_cast<uint32_t>(sets.size());
  std::vector<std::vector<PairScore>> per(pool.size());
  pool.parallelFor(0, n, [&](size_t i, size_t slot) {
    for (uint32_t j = static_cast<uint32_t>(i) + 1; j < n; ++j) {
      uint32_t inter;
      double jac = jaccard(sets[i], sets[j], &inter);
      if (inter > 0 && jac >= threshold) per[slot].push_back({static_cast<uint32_t>(i), j, inter, jac});
    }
  });
  std::vector<PairScore> all;
  for (auto& v : per) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end(), [](const PairScore& x, const PairScore& y) {
    if (x.jaccard != y.jaccard) return x.jaccard > y.jaccard;
    return x.a != y.a ? x.a < y.a : x.b < y.b;
  });
  return all;
}

}  // namespace plag
