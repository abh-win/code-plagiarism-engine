#include "plag/token_cache.h"

#include <cstring>
#include <fstream>

namespace plag {
namespace {

constexpr char kMagic[8] = {'P', 'L', 'A', 'G', 'T', 'C', '0', '1'};
constexpr uint64_t kVersion = 3;  // bump when tokenizer output changes

template <typename T>
void writePod(std::ofstream& o, T v) {
  o.write(reinterpret_cast<const char*>(&v), sizeof v);
}

template <typename T>
bool readPod(std::ifstream& in, T* v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(v), sizeof *v));
}

}  // namespace

uint64_t TokenCache::key(const std::string& content, TokenizerMode mode) {
  uint64_t h = 1469598103934665603ull ^ (kVersion << 8) ^ static_cast<uint64_t>(mode);  // FNV-1a
  for (unsigned char c : content) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

bool TokenCache::get(uint64_t k, std::vector<Token>* out) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = entries_.find(k);
  if (it == entries_.end()) return false;
  *out = it->second;
  ++hits_;
  return true;
}

void TokenCache::put(uint64_t k, const std::vector<Token>& tokens) {
  std::lock_guard<std::mutex> l(mu_);
  entries_[k] = tokens;
}

bool TokenCache::save(const std::string& path) const {
  std::lock_guard<std::mutex> l(mu_);
  // Re-number the process-local token ids densely.
  std::unordered_map<uint32_t, uint32_t> local;
  std::vector<std::string> strings;
  for (const auto& [k, toks] : entries_)
    for (const auto& t : toks)
      if (local.emplace(t.id, static_cast<uint32_t>(strings.size())).second)
        strings.push_back(tokenString(t.id));

  std::string tmp = path + ".tmp";
  std::ofstream o(tmp, std::ios::binary);
  if (!o) return false;
  o.write(kMagic, sizeof kMagic);
  writePod<uint32_t>(o, static_cast<uint32_t>(strings.size()));
  for (const auto& s : strings) {
    writePod<uint32_t>(o, static_cast<uint32_t>(s.size()));
    o.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  writePod<uint32_t>(o, static_cast<uint32_t>(entries_.size()));
  for (const auto& [k, toks] : entries_) {
    writePod<uint64_t>(o, k);
    writePod<uint32_t>(o, static_cast<uint32_t>(toks.size()));
    for (const auto& t : toks) {
      writePod<uint32_t>(o, local[t.id]);
      writePod<uint32_t>(o, t.line);
    }
  }
  o.close();
  return o.good() && std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool TokenCache::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[8];
  if (!in.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return false;
  uint32_t ns;
  if (!readPod(in, &ns)) return false;
  std::vector<uint32_t> ids(ns);
  for (uint32_t i = 0; i < ns; ++i) {
    uint32_t len;
    if (!readPod(in, &len) || len > (1u << 20)) return false;
    std::string s(len, '\0');
    if (!in.read(s.data(), len)) return false;
    ids[i] = internToken(s);
  }
  uint32_t ne;
  if (!readPod(in, &ne)) return false;
  std::unordered_map<uint64_t, std::vector<Token>> entries;
  entries.reserve(ne);
  for (uint32_t e = 0; e < ne; ++e) {
    uint64_t k;
    uint32_t n;
    if (!readPod(in, &k) || !readPod(in, &n)) return false;
    std::vector<Token> toks(n);
    for (auto& t : toks) {
      uint32_t s, line;
      if (!readPod(in, &s) || !readPod(in, &line) || s >= ns) return false;
      t = {ids[s], line};
    }
    entries.emplace(k, std::move(toks));
  }
  std::lock_guard<std::mutex> l(mu_);
  entries_ = std::move(entries);
  return true;
}

}  // namespace plag
