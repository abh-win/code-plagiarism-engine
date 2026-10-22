#include "plag/fingerprint.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace plag {
namespace {

constexpr uint64_t kMod = (1ull << 61) - 1;
constexpr uint64_t kBase = 0x1f3d5b79a1c3e5f7ull % kMod;

uint64_t mulmod(uint64_t a, uint64_t b) {
  __uint128_t r = static_cast<__uint128_t>(a) * b;
  uint64_t lo = static_cast<uint64_t>(r & kMod);
  uint64_t hi = static_cast<uint64_t>(r >> 61);
  uint64_t s = lo + hi;
  return s >= kMod ? s - kMod : s;
}

uint64_t addmod(uint64_t a, uint64_t b) {
  uint64_t s = a + b;
  return s >= kMod ? s - kMod : s;
}

uint64_t submod(uint64_t a, uint64_t b) { return a >= b ? a - b : a + kMod - b; }

// Token ids are small dense integers; spread them over the field first so
// the polynomial hash behaves well (splitmix64 finaliser).
uint64_t scramble(uint32_t id) {
  uint64_t z = id + 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z ^= z >> 31;
  return z % kMod;
}

}  // namespace

std::vector<uint64_t> kgramHashes(const std::vector<uint32_t>& ids, uint32_t k) {
  std::vector<uint64_t> out;
  if (k == 0 || ids.size() < k) return out;
  out.reserve(ids.size() - k + 1);
  // base^(k-1), the weight of the outgoing token.
  uint64_t top = 1;
  for (uint32_t i = 1; i < k; ++i) top = mulmod(top, kBase);

  uint64_t h = 0;
  for (uint32_t i = 0; i < k; ++i) h = addmod(mulmod(h, kBase), scramble(ids[i]));
  out.push_back(h);
  for (size_t i = k; i < ids.size(); ++i) {
    h = submod(h, mulmod(scramble(ids[i - k]), top));
    h = addmod(mulmod(h, kBase), scramble(ids[i]));
    out.push_back(h);
  }
  return out;
}

std::vector<Fingerprint> winnow(const std::vector<uint64_t>& hashes, uint32_t w) {
  std::vector<Fingerprint> out;
  if (hashes.empty()) return out;
  if (w <= 1) {
    out.reserve(hashes.size());
    for (size_t i = 0; i < hashes.size(); ++i) out.push_back({hashes[i], static_cast<uint32_t>(i)});
    return out;
  }
  if (hashes.size() < w) {
    // Rightmost minimum of the whole (short) sequence.
    size_t best = 0;
    for (size_t i = 1; i < hashes.size(); ++i)
      if (hashes[i] <= hashes[best]) best = i;
    out.push_back({hashes[best], static_cast<uint32_t>(best)});
    return out;
  }
  // Monotonic deque of candidate indices: hashes increasing from front to
  // back; equal values keep the newer (rightmost) index. O(n) overall.
  std::deque<size_t> dq;
  size_t last_selected = static_cast<size_t>(-1);
  for (size_t i = 0; i < hashes.size(); ++i) {
    while (!dq.empty() && hashes[dq.back()] >= hashes[i]) dq.pop_back();
    dq.push_back(i);
    if (i + 1 >= w) {
      size_t window_start = i + 1 - w;
      while (dq.front() < window_start) dq.pop_front();
      size_t m = dq.front();
      if (m != last_selected) {
        out.push_back({hashes[m], static_cast<uint32_t>(m)});
        last_selected = m;
      }
    }
  }
  return out;
}

std::vector<Fingerprint> fingerprintTokens(const std::vector<Token>& tokens,
                                           const FingerprintParams& p) {
  // Interned ids depend on the order threads first saw each token, so hash
  // the token *text* instead: fingerprints are then identical across runs.
  thread_local std::unordered_map<uint32_t, uint32_t> stable;
  std::vector<uint32_t> ids;
  ids.reserve(tokens.size());
  for (const auto& t : tokens) {
    auto it = stable.find(t.id);
    if (it == stable.end()) {
      uint32_t h = 2166136261u;  // FNV-1a
      for (unsigned char c : tokenString(t.id)) h = (h ^ c) * 16777619u;
      it = stable.emplace(t.id, h).first;
    }
    ids.push_back(it->second);
  }
  return winnow(kgramHashes(ids, p.k), p.w);
}

std::vector<uint64_t> fingerprintSet(const std::vector<Fingerprint>& fps) {
  std::vector<uint64_t> s;
  s.reserve(fps.size());
  for (const auto& f : fps) s.push_back(f.hash);
  std::sort(s.begin(), s.end());
  s.erase(std::unique(s.begin(), s.end()), s.end());
  return s;
}

}  // namespace plag
