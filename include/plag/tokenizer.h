#pragma once

// Source -> normalized token stream, using libclang.
//
// Modes
//   AST      Pre-order walk of the Clang AST of the file. Each node becomes
//            one token naming its *kind*, never its spelling: identifiers,
//            types and literal values disappear, so renaming variables or
//            changing constants has no effect. Equivalent constructs are
//            canonicalised (for/while/do/range-for -> LOOP, if/?: -> BRANCH,
//            x+=y -> ASSIGN_OP(+)), and pure syntax that obfuscators add or
//            remove (parentheses, implicit casts, braces/compound blocks) is
//            dropped.
//   Lexical  Clang lexer tokens with identifiers -> ID and literals -> NUM/STR;
//            keywords and punctuation kept. Renaming-proof but not
//            structure-aware.
//   Raw      Clang lexer tokens verbatim (baseline; defeated by renaming).
//
// Files are parsed with CXTranslationUnit_SingleFileParse (includes are not
// followed, so no system headers are parsed); a small prelude declaring the
// common standard-library names is prepended so student-style code using
// std::vector / cin / cout still yields a complete AST.

#include <cstdint>
#include <string>
#include <vector>

namespace plag {

enum class TokenizerMode { AST, Lexical, Raw };

struct Token {
  uint32_t id;    // interned token string
  uint32_t line;  // 1-based line in the original file
};

struct TokenizeResult {
  std::vector<Token> tokens;
  bool ok = true;
  unsigned errors = 0;  // Clang error diagnostics (recovered)
  std::vector<std::string> diagnostics;  // first few error messages
  std::string message;
};

// Thread-safe. Each calling thread lazily creates its own CXIndex.
TokenizeResult tokenizeSource(const std::string& source, const std::string& filename,
                              TokenizerMode mode);
TokenizeResult tokenizeFile(const std::string& path, TokenizerMode mode);

// Global, thread-safe string interner shared by all tokenizers.
uint32_t internToken(const std::string& s);
std::string tokenString(uint32_t id);

TokenizerMode parseTokenizerMode(const std::string& s);
const char* toString(TokenizerMode m);

// The prelude that is prepended before parsing (exposed for tests).
const std::string& stdStubPrelude();

}  // namespace plag
