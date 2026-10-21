#pragma once

// Persistent cache of token streams keyed by (file content, tokenizer mode).
// Parsing dominates the pipeline (~8 ms/file for the AST tokenizer), so an
// incremental run over a mostly unchanged corpus only parses new files.
//
// File format (little endian):
//   "PLAGTC01" | u32 n_strings | n_strings * (u32 len, bytes)
//              | u32 n_entries | n_entries * (u64 key, u32 n, n * (u32 str, u32 line))

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "plag/tokenizer.h"

namespace plag {

class TokenCache {
 public:
  static uint64_t key(const std::string& content, TokenizerMode mode);

  bool load(const std::string& path);  // false if missing/corrupt (cache starts empty)
  bool save(const std::string& path) const;

  bool get(uint64_t key, std::vector<Token>* out) const;
  void put(uint64_t key, const std::vector<Token>& tokens);

  size_t size() const { return entries_.size(); }
  size_t hits() const { return hits_; }

 private:
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, std::vector<Token>> entries_;  // token ids are process-local
  mutable size_t hits_ = 0;
};

}  // namespace plag
