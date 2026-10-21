#pragma once

// Inverted index from fingerprint hash to the documents containing it, and
// candidate-pair scoring that never enumerates all N^2 pairs: a pair is only
// examined if the two documents share at least one indexed fingerprint.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace plag {

class ThreadPool;

struct PairScore {
  uint32_t a, b;    // document indices, a < b
  uint32_t shared;  // |A ∩ B| over indexed fingerprints
  double jaccard;   // |A ∩ B| / |A ∪ B|
};

class InvertedIndex {
 public:
  // `sets[d]` is the sorted unique fingerprint set of document d.
  // Fingerprints occurring in more than `max_df` documents are treated as
  // boilerplate (e.g. a common `main` skeleton or template code) and dropped
  // from the index; they still count in |A ∪ B|.
  void build(const std::vector<std::vector<uint64_t>>& sets, uint32_t max_df);

  // O(1) expected lookup. Returns nullptr when the hash is absent/dropped.
  const std::vector<uint32_t>* postings(uint64_t hash) const {
    auto it = map_.find(hash);
    return it == map_.end() ? nullptr : &it->second;
  }

  size_t numHashes() const { return map_.size(); }
  size_t numDropped() const { return dropped_; }
  size_t numPostings() const { return total_postings_; }

 private:
  std::unordered_map<uint64_t, std::vector<uint32_t>> map_;
  size_t dropped_ = 0;
  size_t total_postings_ = 0;
};

// Scores every candidate pair with Jaccard >= threshold, in parallel over
// documents. Each worker owns a dense counter array; for document i it walks
// the postings of i's fingerprints, counting co-occurrences with j > i.
std::vector<PairScore> scoreCandidates(const std::vector<std::vector<uint64_t>>& sets,
                                       const InvertedIndex& index, double threshold,
                                       ThreadPool& pool);

// O(N^2) reference used by tests and the --brute-force benchmark baseline.
std::vector<PairScore> scoreAllPairs(const std::vector<std::vector<uint64_t>>& sets,
                                     double threshold, ThreadPool& pool);

double jaccard(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, uint32_t* shared);

}  // namespace plag
