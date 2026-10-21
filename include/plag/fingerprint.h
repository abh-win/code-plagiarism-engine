#pragma once

// Document fingerprinting (Schleimer, Wilkerson & Aiken, "Winnowing: Local
// Algorithms for Document Fingerprinting", SIGMOD 2003 - the MOSS algorithm).
//
// 1. Rabin–Karp: hash every k-gram of the token stream with a rolling
//    polynomial hash mod the Mersenne prime 2^61-1, O(1) per step.
// 2. Winnowing: slide a window of w consecutive k-gram hashes and keep the
//    minimum of each window (rightmost on ties), recording it once.
//
// Guarantee: if two documents share a run of at least t = w + k - 1 tokens,
// they share at least one fingerprint. Matches shorter than k tokens are
// never reported (noise threshold). Expected density is 2/(w+1).

#include <cstdint>
#include <vector>

#include "plag/tokenizer.h"

namespace plag {

struct Fingerprint {
  uint64_t hash;
  uint32_t pos;  // index of the first token of the k-gram
};

struct FingerprintParams {
  uint32_t k = 10;  // k-gram length in tokens
  uint32_t w = 6;   // winnowing window (in k-grams)
};

// Rolling hashes of every k-gram; result size = max(0, n - k + 1).
std::vector<uint64_t> kgramHashes(const std::vector<uint32_t>& token_ids, uint32_t k);

// Robust winnowing over a hash sequence. If there are fewer than w hashes the
// overall minimum is selected (so short documents still get a fingerprint).
std::vector<Fingerprint> winnow(const std::vector<uint64_t>& hashes, uint32_t w);

std::vector<Fingerprint> fingerprintTokens(const std::vector<Token>& tokens,
                                           const FingerprintParams& p);

// Sorted, de-duplicated hash set of a fingerprint list.
std::vector<uint64_t> fingerprintSet(const std::vector<Fingerprint>& fps);

}  // namespace plag
