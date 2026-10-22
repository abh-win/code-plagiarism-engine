// plag: find likely-plagiarised pairs among C/C++ source files.
//
//   plag [--tokenizer ast|lex|raw] [--k 10] [--w 6] [--threshold 0.55]
//        [--threads N] [--max-df-frac 0.5] [--top 20] [--csv pairs.csv]
//        [--explain] [--brute-force] [--cache FILE] PATH...
//
// PATHs may be files or directories (searched recursively).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "plag/engine.h"

using namespace plag;

namespace {

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: plag [--tokenizer ast|lex|raw] [--k N] [--w N] [--threshold J] [--threads N]\n"
               "            [--max-df-frac F] [--top N] [--csv FILE] [--explain] [--brute-force] [--cache FILE] PATH...\n"
               "       plag [--tokenizer ast|lex|raw] --dump FILE\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  EngineOptions opts;
  std::vector<std::string> paths;
  size_t top = 20;
  std::string csv;
  bool explain = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--tokenizer") opts.mode = parseTokenizerMode(next());
    else if (a == "--k") opts.fp.k = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--w") opts.fp.w = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--threshold") opts.threshold = std::stod(next());
    else if (a == "--threads") opts.threads = std::stoul(next());
    else if (a == "--max-df-frac") opts.max_df_frac = std::stod(next());
    else if (a == "--top") top = std::stoul(next());
    else if (a == "--csv") csv = next();
    else if (a == "--explain") explain = true;
    else if (a == "--brute-force") opts.brute_force = true;
    else if (a == "--cache") opts.cache_path = next();
    else if (a == "--dump") {
      // Print the normalized token stream of one file and exit.
      TokenizeResult tr = tokenizeFile(next(), opts.mode);
      std::printf("ok=%d clang_errors=%u tokens=%zu\n", tr.ok, tr.errors, tr.tokens.size());
      for (const auto& d : tr.diagnostics) std::printf("  %s\n", d.c_str());
      uint32_t line = 0;
      for (const auto& t : tr.tokens) {
        if (t.line != line) std::printf("\n%4u: ", line = t.line);
        std::printf("%s ", tokenString(t.id).c_str());
      }
      std::printf("\n");
      return 0;
    }
    else if (a == "-h" || a == "--help") usage();
    else if (a.rfind("--", 0) == 0) usage();
    else paths.push_back(a);
  }
  if (paths.empty()) usage();

  Engine engine(opts);
  engine.addPaths(paths);
  RunResult r = engine.run();
  const auto& docs = engine.documents();

  std::printf("files %zu  tokens %zu  fingerprints %zu  (tokenizer=%s k=%u w=%u -> guaranteed match length %u tokens)\n",
              docs.size(), r.total_tokens, r.total_fingerprints, toString(opts.mode), opts.fp.k,
              opts.fp.w, opts.fp.k + opts.fp.w - 1);
  if (!opts.brute_force)
    std::printf("index: %zu hashes, %zu boilerplate hashes dropped\n", r.indexed_hashes, r.dropped_hashes);
  if (r.failed_docs) std::printf("warning: %zu files could not be parsed\n", r.failed_docs);
  if (!opts.cache_path.empty())
    std::printf("token cache: %zu of %zu files reused from %s\n", r.cache_hits, docs.size(), opts.cache_path.c_str());
  if (r.docs_with_errors)
    std::printf("note: %zu files parsed with recovered Clang errors\n", r.docs_with_errors);
  std::printf("time: tokenize %.3fs | fingerprint %.3fs  index %.3fs  score %.3fs  (compare phase %.3fs) | total %.3fs\n",
              r.timings.tokenize, r.timings.fingerprint, r.timings.index, r.timings.score,
              r.timings.comparePhase(), r.timings.total());
  std::printf("pairs with Jaccard >= %.2f: %zu\n\n", opts.threshold, r.pairs.size());

  for (size_t i = 0; i < std::min(top, r.pairs.size()); ++i) {
    const PairScore& p = r.pairs[i];
    std::printf("%6.3f  %4u shared  %s  <->  %s\n", p.jaccard, p.shared, docs[p.a].path.c_str(),
                docs[p.b].path.c_str());
    if (explain) {
      for (const auto& m : engine.explain(p.a, p.b))
        std::printf("          lines %u-%u  ~  %u-%u\n", m.a_first, m.a_last, m.b_first, m.b_last);
    }
  }

  if (!csv.empty()) {
    std::ofstream out(csv);
    out << "a,b,jaccard,shared\n";
    for (const auto& p : r.pairs)
      out << docs[p.a].path << "," << docs[p.b].path << "," << p.jaccard << "," << p.shared << "\n";
  }
  return 0;
}
