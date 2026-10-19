# Source Code Plagiarism Engine

A MOSS-style plagiarism detector for C/C++, written in C++17. Source is normalised through the
**Clang AST** (via libclang), so renaming, reformatting, rewriting loops and inserting junk code
no longer hide a copy. Documents are fingerprinted with **Rabin–Karp + Winnowing**, and an
**inverted index** finds candidate pairs without the naive O(N²) comparison. Pairs are scored by
**Jaccard similarity** in parallel on a custom thread pool.

```
 source ──► libclang single-file parse ──► AST walk + normalisation ──► token stream
            (std stub as precompiled hdr)    (DCE, canonical forms)         │
                                                                            ▼
 report ◄── Jaccard per candidate pair ◄── inverted index ◄── winnowed Rabin–Karp fingerprints
            (thread pool, dense counters)   (hash → docs, boilerplate filter)
```

## How it works

**1. AST tokenizer** (`src/clang_tokenizer.cpp`). Each file is parsed with
`CXTranslationUnit_SingleFileParse`, so `#include`s are not followed and system headers cost
nothing. A small prelude declaring common standard-library names (`src/std_stub.cpp`; vector,
string, map, cin/cout, algorithm, …) is compiled once into a **precompiled header** and loaded
with `-include-pch`, so code written against `<bits/stdc++.h>` still gets a complete AST. The
pre-order walk emits one token per node *kind*, never its spelling. Normalisations:

| obfuscation | normalisation |
|---|---|
| renaming variables / functions / types | identifiers, type names and literal values are not emitted (`REF`, `VAR`, `NUM`, …) |
| reformatting, comments, `#include` changes | invisible to the AST |
| `for` ↔ `while` ↔ `do` ↔ range-for | `LOOP`; a `for(init;cond;inc) body` is emitted in while-order `init LOOP cond body inc` (clauses located by the `;` positions, since libclang omits absent ones) |
| `if` ↔ `?:` | `BRANCH` |
| `a < b` ↔ `b > a` | `CMP` |
| `i++` ↔ `++i` ↔ `i += 1` ↔ `i = i + 1` | `INC` |
| `x = x + y` ↔ `x += y` | `ASSIGN_OP+` |
| extra parentheses, braces, implicit casts, `int a, b;` vs `int a; int b;`, typedefs, stray `;` | dropped |
| **dead-code insertion** | per-function dead-code elimination: a local variable whose every statement mentions nothing else (no other variable, call, return) is removed along with those statements |

`--tokenizer lex` (Clang lexer, identifiers → `ID`) and `--tokenizer raw` (verbatim tokens) are
there as baselines.

**2. Fingerprinting** (`src/fingerprint.cpp`). Every k-gram of tokens gets a Rabin–Karp rolling
hash, mod the Mersenne prime 2⁶¹−1, in O(1) per step. Winnowing (Schleimer, Wilkerson & Aiken,
2003) keeps the rightmost minimum of each window of w hashes, using a monotonic deque so the whole
pass is O(n). Any shared run of at least `w + k − 1` tokens is guaranteed to produce a shared
fingerprint (a property-tested invariant). Token hashes come from token *text*, so results are
identical across runs.

**3. Inverted index** (`src/index.cpp`). This is an `unordered_map<hash, posting list>`, giving
O(1) expected lookups. Fingerprints that occur in more than `--max-df-frac` of the documents
(starter or skeleton code) are dropped and excluded from Jaccard.

**4. Candidate scoring.** `ThreadPool::parallelFor` hands documents to workers. Each worker keeps
a dense per-document counter array; for document *i* it walks the postings of *i*'s fingerprints
and counts co-occurrences with every *j > i*. Only pairs that share a fingerprint are ever
touched. Jaccard = |A∩B| / |A∪B|.

**5. Token cache** (`--cache FILE`). Token streams are cached, keyed by a hash of file content
plus tokenizer mode, so incremental runs only re-parse new files.

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j   # needs clang/libclang
./build/plag_tests                                        # 21 GoogleTest cases

./build/plag submissions/ --top 20 --explain              # report + matching line ranges
./build/plag submissions/ --csv pairs.csv --threshold 0.4 --cache .plagcache
./build/plag --dump file.cpp                              # show the normalised token stream
```

Example `--explain` output for an original vs. its L3-obfuscated copy (renamed, reordered,
loops rewritten, dead code inserted):

```
 0.793    69 shared  p00007_copy0_L1.cpp  <->  p00007_copy2_L3.cpp
          lines 12-31  ~  42-61
          lines 32-57  ~  13-41
          ...
```

## Evaluation

### Corpus (`scripts/gen_corpus.py`)

The corpus has 10,601 C++ files and 10,692 labelled plagiarised pairs, and every file compiles
with g++. It was built in three steps:

1. **Originals.** 4,200 original programs, each assembled from 4–6 of 30 algorithm snippets
   (sorting, search, DP, graphs, strings, number theory; most snippets have two implementation
   variants). Each original has its own identifiers, constants, formatting and author-specific
   incidental code (debug output, validation).
2. **Overlap guard.** Two independent originals may share at most 3 implementations.
3. **Copies.** Each original gets 0–3 copies at three obfuscation levels:
   - **L1**: rename everything and reformat.
   - **L2**: L1, plus reorder functions, for↔while, `i++`/`++i`/`i+=1`, `a<b`→`b>a`, and `x+=y`↔`x=x+y`.
   - **L3**: L2, plus dead-code insertion, merged declarations and redundant parentheses.

### Results

From `scripts/evaluate.py data/corpus` (`results/evaluation.txt`), with the best threshold per
configuration:

| tokenizer | k / w | precision | recall | **F1** | recall L1 | recall L2 | recall L3 |
|---|---|---|---|---|---|---|---|
| **AST** (default) | 10 / 6 | 0.981 | 0.963 | **0.972** | 1.000 | 0.964 | 0.941 |
| AST | 8 / 4 | 0.981 | 0.968 | 0.974 | 1.000 | 0.964 | 0.952 |
| AST | 12 / 8 | 0.975 | 0.954 | 0.964 | 1.000 | 0.948 | 0.930 |
| lexical (identifiers→ID) | 10 / 6 | 0.835 | 0.631 | 0.719 | 1.000 | 0.753 | 0.308 |
| raw tokens | 10 / 6 | – | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

Each normalisation was added in response to a measured failure on this corpus:

- **Dead-code elimination:** with the same k/w and corpus generator (dead extras), L3 recall at
  the best threshold went from 0.55 to 0.81.
- **for→while canonicalisation and INC/ASSIGN_OP rewriting:** these closed the gap between
  originals and L2 copies that had made L2 recall collapse in the first runs.

### Speed

10,601 files on a 12-thread i5-1235U laptop (2 P-cores + 8 E-cores):

| phase | time |
|---|---|
| Clang parse + AST normalisation (cold) | 9.1–9.4 s (≈ 8 ms/file/thread) |
| token cache hit (warm) | 0.39 s |
| fingerprint + index + Jaccard scoring | **0.38 s** at threshold 0.56 (1.7 s when emitting every pair ≥ 0.15 for evaluation) |
| same scoring, brute-force O(N²) all pairs (`--brute-force`) | 6.33 s, same 10,708 pairs |

The indexed comparison is about 17× faster than brute force at this size, and the gap grows
quadratically. End to end with a warm cache, the run takes 0.76 s.

## Layout

```
include/plag/   tokenizer.h  fingerprint.h  index.h  engine.h  thread_pool.h  token_cache.h
src/            clang_tokenizer.cpp  std_stub.cpp  fingerprint.cpp  index.cpp  engine.cpp  token_cache.cpp
tools/plag.cpp  CLI
tests/          fingerprint (rolling hash, winnowing guarantee/density), index (vs brute force),
                thread pool, tokenizer (renaming invariance, loop canonicalisation, DCE, line mapping)
scripts/        gen_corpus.py  evaluate.py
```

## Limitations

- Function-level cross-file refactorings (inlining, splitting helpers) still reduce the score.
  Fingerprint sets are order-insensitive, so reordering costs nothing, but restructuring does.
- The std prelude covers common competitive-programming and student code. Exotic library usage
  parses with recovered errors, and the affected statements drop out of the token stream.
- The labelled corpus is synthetic, so numbers on real course submissions will differ. The engine
  runs on any directory of C/C++ files.
