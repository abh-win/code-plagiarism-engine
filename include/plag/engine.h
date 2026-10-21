#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "plag/fingerprint.h"
#include "plag/index.h"
#include "plag/tokenizer.h"

namespace plag {

struct EngineOptions {
  TokenizerMode mode = TokenizerMode::AST;
  FingerprintParams fp;
  double threshold = 0.55;     // report pairs with Jaccard >= threshold
  double max_df_frac = 0.5;    // drop fingerprints in more than this fraction of docs...
  uint32_t min_max_df = 10;    // ...but never below this absolute document count
  size_t threads = 0;          // 0 = hardware concurrency
  bool brute_force = false;    // O(N^2) pairwise scoring instead of the index
  std::string cache_path;      // token cache file (empty = no cache)
};

struct Document {
  std::string path;
  std::vector<Token> tokens;
  std::vector<Fingerprint> fps;
  std::vector<uint64_t> set;
  unsigned clang_errors = 0;
  bool ok = true;
};

struct Timings {
  double tokenize = 0, fingerprint = 0, index = 0, score = 0;
  double comparePhase() const { return fingerprint + index + score; }
  double total() const { return tokenize + comparePhase(); }
};

struct RunResult {
  std::vector<PairScore> pairs;  // sorted by Jaccard, descending
  Timings timings;
  size_t total_tokens = 0;
  size_t total_fingerprints = 0;
  size_t indexed_hashes = 0;
  size_t dropped_hashes = 0;
  size_t failed_docs = 0;
  size_t docs_with_errors = 0;  // parsed, but Clang reported (recovered) errors
  size_t cache_hits = 0;
};

struct MatchRegion {
  uint32_t a_first, a_last;  // 1-based line ranges
  uint32_t b_first, b_last;
};

class Engine {
 public:
  explicit Engine(EngineOptions opts) : opts_(opts) {}

  // Expands directories recursively (C/C++ source extensions only).
  void addPaths(const std::vector<std::string>& paths);
  // In-memory documents (tests, benchmarks).
  void addSource(const std::string& name, const std::string& source);

  RunResult run();

  const std::vector<Document>& documents() const { return docs_; }

  // Line ranges in a and b covered by shared fingerprints, merged.
  std::vector<MatchRegion> explain(uint32_t a, uint32_t b) const;

 private:
  EngineOptions opts_;
  std::vector<std::string> paths_;
  std::vector<std::pair<std::string, std::string>> sources_;
  std::vector<Document> docs_;
};

}  // namespace plag
