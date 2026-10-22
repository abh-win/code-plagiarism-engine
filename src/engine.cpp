#include "plag/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "plag/thread_pool.h"
#include "plag/token_cache.h"

namespace plag {
namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

bool isSourceFile(const std::filesystem::path& p) {
  static const char* kExt[] = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".h"};
  std::string e = p.extension().string();
  return std::any_of(std::begin(kExt), std::end(kExt), [&](const char* x) { return e == x; });
}

}  // namespace

void Engine::addPaths(const std::vector<std::string>& paths) {
  namespace fs = std::filesystem;
  std::vector<std::string> found;
  for (const auto& p : paths) {
    if (fs::is_directory(p)) {
      for (const auto& e : fs::recursive_directory_iterator(p))
        if (e.is_regular_file() && isSourceFile(e.path())) found.push_back(e.path().string());
    } else {
      found.push_back(p);
    }
  }
  std::sort(found.begin(), found.end());
  paths_.insert(paths_.end(), found.begin(), found.end());
}

void Engine::addSource(const std::string& name, const std::string& source) {
  sources_.emplace_back(name, source);
}

RunResult Engine::run() {
  RunResult r;
  ThreadPool pool(opts_.threads ? opts_.threads : std::thread::hardware_concurrency());

  // 1. Tokenize (dominant cost: one libclang parse per file).
  auto t0 = Clock::now();
  const size_t n_files = paths_.size();
  docs_.assign(n_files + sources_.size(), Document{});
  TokenCache cache;
  if (!opts_.cache_path.empty()) cache.load(opts_.cache_path);
  pool.parallelFor(0, docs_.size(), [&](size_t i) {
    Document& d = docs_[i];
    d.path = i < n_files ? paths_[i] : sources_[i - n_files].first;
    std::string content;
    if (i < n_files) {
      std::ifstream in(paths_[i], std::ios::binary);
      if (!in) {
        d.ok = false;
        return;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      content = ss.str();
    } else {
      content = sources_[i - n_files].second;
    }
    uint64_t key = TokenCache::key(content, opts_.mode);
    if (!opts_.cache_path.empty() && cache.get(key, &d.tokens)) return;
    TokenizeResult tr = tokenizeSource(content, d.path, opts_.mode);
    d.tokens = std::move(tr.tokens);
    d.clang_errors = tr.errors;
    d.ok = tr.ok;
    if (!opts_.cache_path.empty() && tr.ok) cache.put(key, d.tokens);
  }, 4);
  r.cache_hits = cache.hits();
  if (!opts_.cache_path.empty()) cache.save(opts_.cache_path);
  r.timings.tokenize = since(t0);

  // 2. Fingerprint (Rabin–Karp + winnowing), in parallel.
  t0 = Clock::now();
  pool.parallelFor(0, docs_.size(), [&](size_t i) {
    docs_[i].fps = fingerprintTokens(docs_[i].tokens, opts_.fp);
    docs_[i].set = fingerprintSet(docs_[i].fps);
  });
  r.timings.fingerprint = since(t0);

  std::vector<std::vector<uint64_t>> sets;
  sets.reserve(docs_.size());
  for (const auto& d : docs_) {
    sets.push_back(d.set);
    r.total_tokens += d.tokens.size();
    r.total_fingerprints += d.set.size();
    r.failed_docs += d.ok ? 0 : 1;
    r.docs_with_errors += d.clang_errors > 0 ? 1 : 0;
  }

  if (opts_.brute_force) {
    t0 = Clock::now();
    r.pairs = scoreAllPairs(sets, opts_.threshold, pool);
    r.timings.score = since(t0);
    return r;
  }

  // 3. Inverted index.
  t0 = Clock::now();
  InvertedIndex index;
  uint32_t max_df = std::max<uint32_t>(
      opts_.min_max_df, static_cast<uint32_t>(std::ceil(opts_.max_df_frac * docs_.size())));
  index.build(sets, max_df);
  r.indexed_hashes = index.numHashes();
  r.dropped_hashes = index.numDropped();
  r.timings.index = since(t0);

  // 4. Candidate scoring. Boilerplate fingerprints are removed from the sets
  // too, so Jaccard is computed over distinctive fingerprints only (an exact
  // copy of a file full of template code still scores 1.0).
  t0 = Clock::now();
  pool.parallelFor(0, sets.size(), [&](size_t i) {
    auto& s = sets[i];
    s.erase(std::remove_if(s.begin(), s.end(), [&](uint64_t h) { return index.postings(h) == nullptr; }),
            s.end());
  });
  r.pairs = scoreCandidates(sets, index, opts_.threshold, pool);
  r.timings.score = since(t0);
  return r;
}

std::vector<MatchRegion> Engine::explain(uint32_t a, uint32_t b) const {
  const Document& A = docs_.at(a);
  const Document& B = docs_.at(b);
  std::unordered_map<uint64_t, std::vector<uint32_t>> bpos;
  for (const auto& f : B.fps) bpos[f.hash].push_back(f.pos);

  auto lineOf = [](const Document& d, uint32_t tok) {
    return d.tokens[std::min<size_t>(tok, d.tokens.size() - 1)].line;
  };
  const uint32_t k = opts_.fp.k;

  // Each shared fingerprint covers k tokens in both documents.
  std::vector<MatchRegion> raw;
  for (const auto& f : A.fps) {
    auto it = bpos.find(f.hash);
    if (it == bpos.end()) continue;
    for (uint32_t pb : it->second)
      raw.push_back({lineOf(A, f.pos), lineOf(A, f.pos + k - 1), lineOf(B, pb), lineOf(B, pb + k - 1)});
  }
  std::sort(raw.begin(), raw.end(), [](const MatchRegion& x, const MatchRegion& y) {
    return x.a_first != y.a_first ? x.a_first < y.a_first : x.b_first < y.b_first;
  });
  // Merge regions that overlap (or touch) in both files.
  std::vector<MatchRegion> merged;
  for (const auto& m : raw) {
    if (!merged.empty()) {
      MatchRegion& last = merged.back();
      bool a_overlap = m.a_first <= last.a_last + 1;
      bool b_overlap = m.b_first <= last.b_last + 1 && m.b_last + 1 >= last.b_first;
      if (a_overlap && b_overlap) {
        last.a_last = std::max(last.a_last, m.a_last);
        last.b_first = std::min(last.b_first, m.b_first);
        last.b_last = std::max(last.b_last, m.b_last);
        continue;
      }
    }
    merged.push_back(m);
  }
  return merged;
}

}  // namespace plag
