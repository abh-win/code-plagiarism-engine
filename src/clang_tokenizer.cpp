#include <clang-c/Index.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "plag/tokenizer.h"

namespace plag {
namespace {

// ------------------------------------------------------------------ interner

class Interner {
 public:
  uint32_t intern(const std::string& s) {
    {
      std::shared_lock<std::shared_mutex> r(mu_);
      auto it = ids_.find(s);
      if (it != ids_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> w(mu_);
    auto [it, inserted] = ids_.emplace(s, static_cast<uint32_t>(strings_.size()));
    if (inserted) strings_.push_back(s);
    return it->second;
  }
  std::string str(uint32_t id) {
    std::shared_lock<std::shared_mutex> r(mu_);
    return id < strings_.size() ? strings_[id] : std::string("?");
  }

 private:
  std::shared_mutex mu_;
  std::unordered_map<std::string, uint32_t> ids_;
  std::vector<std::string> strings_;
};

Interner& interner() {
  static Interner i;
  return i;
}

// Per-thread front cache so hot tokens never touch the shared lock.
uint32_t internCached(const std::string& s) {
  thread_local std::unordered_map<std::string, uint32_t> cache;
  auto it = cache.find(s);
  if (it != cache.end()) return it->second;
  uint32_t id = interner().intern(s);
  cache.emplace(s, id);
  return id;
}

// ------------------------------------------------------------------ libclang plumbing

struct IndexHolder {
  CXIndex index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0, /*displayDiagnostics=*/0);
  ~IndexHolder() { clang_disposeIndex(index); }
};

CXIndex threadIndex() {
  thread_local IndexHolder holder;
  return holder.index;
}

std::string toStd(CXString s) {
  const char* c = clang_getCString(s);
  std::string out = c ? c : "";
  clang_disposeString(s);
  return out;
}

unsigned countLines(const std::string& s) {
  return static_cast<unsigned>(std::count(s.begin(), s.end(), '\n'));
}

// ------------------------------------------------------------------ AST walk

// Set of cursors keyed by clang_hashCursor, compared with clang_equalCursors.
class CursorSet {
 public:
  void insert(CXCursor c) { buckets_[clang_hashCursor(c)].push_back(c); }
  bool contains(CXCursor c) const {
    auto it = buckets_.find(clang_hashCursor(c));
    if (it == buckets_.end()) return false;
    for (const CXCursor& x : it->second)
      if (clang_equalCursors(x, c)) return true;
    return false;
  }
  bool empty() const { return buckets_.empty(); }

 private:
  std::unordered_map<unsigned, std::vector<CXCursor>> buckets_;
};

struct WalkContext {
  CXTranslationUnit tu;
  unsigned prelude_lines;  // lines occupied by the prelude in the parse buffer
  std::vector<Token>* out;
  CursorSet skip;          // dead code found in the current function
};

// Returns the canonical token name for a cursor, or "" to emit nothing (the
// node's children are still visited).
std::string canonicalName(CXCursor c) {
  CXCursorKind k = clang_getCursorKind(c);
  switch (k) {
    // Pure syntax / sugar that obfuscation commonly adds or removes.
    case CXCursor_UnexposedExpr:      // implicit casts, temporaries
    case CXCursor_UnexposedStmt:
    case CXCursor_ParenExpr:
    case CXCursor_CompoundStmt:       // braces around single statements
    case CXCursor_DeclStmt:           // `int a, b;` == `int a; int b;`
    case CXCursor_NullStmt:           // stray semicolons
    case CXCursor_TypeRef:
    case CXCursor_TemplateRef:
    case CXCursor_NamespaceRef:
    case CXCursor_OverloadedDeclRef:
    case CXCursor_UsingDirective:
    case CXCursor_UsingDeclaration:
    case CXCursor_UnexposedDecl:
    case CXCursor_LinkageSpec:
    case CXCursor_Namespace:
    case CXCursor_TypedefDecl:        // type aliases are renaming in disguise
    case CXCursor_TypeAliasDecl:
    case CXCursor_TemplateTypeParameter:
    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_MacroDefinition:
    case CXCursor_MacroExpansion:
    case CXCursor_InclusionDirective:
    case CXCursor_CXXAccessSpecifier:
    case CXCursor_CXXBaseSpecifier:
    case CXCursor_FirstAttr:
      return "";

    case CXCursor_ForStmt:
    case CXCursor_WhileStmt:
    case CXCursor_DoStmt:
    case CXCursor_CXXForRangeStmt:
      return "LOOP";
    case CXCursor_IfStmt:
    case CXCursor_ConditionalOperator:
      return "BRANCH";
    case CXCursor_SwitchStmt: return "SWITCH";
    case CXCursor_CaseStmt: return "CASE";
    case CXCursor_DefaultStmt: return "DEFAULT";
    case CXCursor_BreakStmt: return "BREAK";
    case CXCursor_ContinueStmt: return "CONTINUE";
    case CXCursor_ReturnStmt: return "RETURN";
    case CXCursor_GotoStmt: return "GOTO";
    case CXCursor_LabelStmt: return "";

    case CXCursor_DeclRefExpr: return "REF";
    case CXCursor_MemberRefExpr: return "MEMBER";
    case CXCursor_CallExpr: return "CALL";
    case CXCursor_ArraySubscriptExpr: return "INDEX";
    case CXCursor_IntegerLiteral:
    case CXCursor_FloatingLiteral:
      return "NUM";
    case CXCursor_StringLiteral:
    case CXCursor_CharacterLiteral:
      return "STR";
    case CXCursor_CXXBoolLiteralExpr: return "BOOL";
    case CXCursor_CXXNullPtrLiteralExpr:
    case CXCursor_GNUNullExpr:
      return "NULLPTR";
    case CXCursor_CStyleCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXFunctionalCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXConstCastExpr:
      return "CAST";
    case CXCursor_InitListExpr: return "INITLIST";
    case CXCursor_LambdaExpr: return "LAMBDA";
    case CXCursor_CXXNewExpr: return "NEW";
    case CXCursor_CXXDeleteExpr: return "DELETE";
    case CXCursor_CXXThisExpr: return "THIS";

    case CXCursor_BinaryOperator: {
      std::string op = toStd(clang_getBinaryOperatorKindSpelling(clang_getCursorBinaryOperatorKind(c)));
      // a > b  <->  b < a : relational direction is a trivial rewrite.
      if (op == "<" || op == ">" || op == "<=" || op == ">=") return "CMP";
      return "OP" + op;
    }
    case CXCursor_CompoundAssignOperator: {
      std::string op = toStd(clang_getBinaryOperatorKindSpelling(clang_getCursorBinaryOperatorKind(c)));
      return "ASSIGN_OP" + op.substr(0, op.size() - 1);  // "+=" -> ASSIGN_OP+
    }
    case CXCursor_UnaryOperator: {
      CXUnaryOperatorKind u = clang_getCursorUnaryOperatorKind(c);
      switch (u) {
        case CXUnaryOperator_PostInc:
        case CXUnaryOperator_PreInc:
          return "INC";  // i++ == ++i as a statement
        case CXUnaryOperator_PostDec:
        case CXUnaryOperator_PreDec:
          return "DEC";
        default:
          return "UOP" + toStd(clang_getUnaryOperatorKindSpelling(u));
      }
    }

    case CXCursor_VarDecl: return "VAR";
    case CXCursor_ParmDecl: return "PARAM";
    case CXCursor_FieldDecl: return "FIELD";
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_FunctionTemplate:
      return "FUNC";
    case CXCursor_Constructor: return "CTOR";
    case CXCursor_Destructor: return "DTOR";
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_UnionDecl:
      return "RECORD";
    case CXCursor_EnumDecl: return "ENUM";
    case CXCursor_EnumConstantDecl: return "ENUMCONST";
    default:
      return toStd(clang_getCursorKindSpelling(k));
  }
}

std::vector<CXCursor> children(CXCursor c) {
  std::vector<CXCursor> out;
  clang_visitChildren(
      c,
      [](CXCursor child, CXCursor, CXClientData d) {
        static_cast<std::vector<CXCursor>*>(d)->push_back(child);
        return CXChildVisit_Continue;
      },
      &out);
  return out;
}

// Looks through implicit casts and parentheses.
CXCursor stripSugar(CXCursor c) {
  while (true) {
    CXCursorKind k = clang_getCursorKind(c);
    if (k != CXCursor_UnexposedExpr && k != CXCursor_ParenExpr) return c;
    auto kids = children(c);
    if (kids.size() != 1) return c;
    c = kids[0];
  }
}

bool sameVariable(CXCursor a, CXCursor b) {
  a = stripSugar(a);
  b = stripSugar(b);
  if (clang_getCursorKind(a) != CXCursor_DeclRefExpr || clang_getCursorKind(b) != CXCursor_DeclRefExpr)
    return false;
  CXCursor ra = clang_getCursorReferenced(a), rb = clang_getCursorReferenced(b);
  return !clang_Cursor_isNull(ra) && clang_equalCursors(ra, rb);
}

bool isLiteralOne(CXCursor c) {
  c = stripSugar(c);
  if (clang_getCursorKind(c) != CXCursor_IntegerLiteral) return false;
  CXEvalResult r = clang_Cursor_Evaluate(c);
  if (r == nullptr) return false;
  bool one = clang_EvalResult_getKind(r) == CXEval_Int && clang_EvalResult_getAsLongLong(r) == 1;
  clang_EvalResult_dispose(r);
  return one;
}

std::string binaryOpSpelling(CXCursor c) {
  return toStd(clang_getBinaryOperatorKindSpelling(clang_getCursorBinaryOperatorKind(c)));
}

unsigned fileOffset(CXSourceLocation loc) {
  unsigned off = 0;
  clang_getFileLocation(loc, nullptr, nullptr, nullptr, &off);
  return off;
}

void walk(CXCursor c, WalkContext* ctx);

// ------------------------------------------------------------------ dead-code elimination
//
// A local variable is *dead* when every statement that mentions it mentions
// nothing else: no other variable, no call (operator<< included), no return.
// Such a variable cannot influence the program's output, so its declaration
// and all those statements are dropped before tokenizing - this neutralises
// the classic "insert junk statements" obfuscation, e.g.
//     int t = 17;  if (t < 0) t = -t;      long long z = 0;  z += 4;      (void)q;
// "Statement" means the innermost enclosing statement that is a direct child
// of a { } block (or the function body).

struct DceStmt {
  CXCursor cursor;
  std::vector<CXCursor> refs;  // referenced declarations
  bool effects = false;        // contains a call / return / throw / new / delete
};

struct DceVar {
  CXCursor decl;
  int decl_stmt = -1;
  std::vector<int> uses;
};

struct DceState {
  CXCursor func;
  std::vector<DceStmt> stmts;
  std::vector<DceVar> vars;
};

int findVar(const DceState& st, CXCursor decl) {
  for (size_t i = 0; i < st.vars.size(); ++i)
    if (clang_equalCursors(st.vars[i].decl, decl)) return static_cast<int>(i);
  return -1;
}

bool isLocalOf(CXCursor decl, CXCursor func) {
  return clang_getCursorKind(decl) == CXCursor_VarDecl &&
         clang_equalCursors(clang_getCursorSemanticParent(decl), func);
}

void dceScan(CXCursor c, int top, bool parent_is_block, DceState* st) {
  CXCursorKind k = clang_getCursorKind(c);
  if (parent_is_block) {
    st->stmts.push_back({c, {}, false});
    top = static_cast<int>(st->stmts.size()) - 1;
  }
  if (top >= 0) {
    switch (k) {
      case CXCursor_CallExpr:
      case CXCursor_ReturnStmt:
      case CXCursor_CXXThrowExpr:
      case CXCursor_CXXNewExpr:
      case CXCursor_CXXDeleteExpr:
      case CXCursor_BreakStmt:
      case CXCursor_ContinueStmt:
      case CXCursor_GotoStmt:
        st->stmts[top].effects = true;
        break;
      case CXCursor_DeclRefExpr:
      case CXCursor_MemberRefExpr: {
        CXCursor ref = clang_getCursorReferenced(c);
        if (clang_Cursor_isNull(ref)) {
          st->stmts[top].effects = true;  // unresolved: be conservative
          break;
        }
        st->stmts[top].refs.push_back(ref);
        if (isLocalOf(ref, st->func)) {
          int v = findVar(*st, ref);
          if (v >= 0) st->vars[v].uses.push_back(top);
        }
        break;
      }
      case CXCursor_VarDecl:
        if (isLocalOf(c, st->func)) st->vars.push_back({c, top, {}});
        break;
      default:
        break;
    }
  }
  bool is_block = k == CXCursor_CompoundStmt;
  for (CXCursor child : children(c)) dceScan(child, top, is_block, st);
}

void findDeadCode(CXCursor func, CursorSet* skip) {
  DceState st{func, {}, {}};
  for (CXCursor child : children(func)) dceScan(child, -1, false, &st);
  auto only_mentions = [&](const DceStmt& s, CXCursor v) {
    if (s.effects) return false;
    for (const CXCursor& r : s.refs)
      if (!clang_equalCursors(r, v)) return false;
    return true;
  };
  for (const DceVar& v : st.vars) {
    if (v.decl_stmt < 0 || !only_mentions(st.stmts[v.decl_stmt], v.decl)) continue;
    bool dead = true;
    for (int u : v.uses) dead = dead && only_mentions(st.stmts[u], v.decl);
    if (!dead) continue;
    skip->insert(v.decl);
    for (int u : v.uses) skip->insert(st.stmts[u].cursor);
  }
}

void emit(WalkContext* ctx, const std::string& name, unsigned line) {
  ctx->out->push_back({internCached(name), line - ctx->prelude_lines});
}

// for (init; cond; inc) body   is emitted in while-loop order
//   init LOOP cond body inc
// so that a for loop and its hand-written while equivalent tokenize alike.
// libclang omits absent parts, so each child's role is recovered from its
// position relative to the two top-level ';' inside the for-parentheses.
void walkFor(CXCursor c, unsigned line, WalkContext* ctx) {
  auto kids = children(c);
  CXToken* toks = nullptr;
  unsigned n = 0;
  clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
  unsigned semi1 = 0, semi2 = 0, rparen = 0;
  int depth = 0, semis = 0;
  for (unsigned i = 0; i < n && rparen == 0; ++i) {
    if (clang_getTokenKind(toks[i]) != CXToken_Punctuation) continue;
    std::string sp = toStd(clang_getTokenSpelling(ctx->tu, toks[i]));
    unsigned off = fileOffset(clang_getTokenLocation(ctx->tu, toks[i]));
    if (sp == "(") {
      ++depth;
    } else if (sp == ")") {
      if (--depth == 0) rparen = off;
    } else if (sp == ";" && depth == 1) {
      (semis++ == 0 ? semi1 : semi2) = off;
    }
  }
  clang_disposeTokens(ctx->tu, toks, n);

  std::vector<CXCursor> init, cond, inc, body;
  if (semis == 2 && rparen != 0) {
    for (CXCursor k : kids) {
      unsigned off = fileOffset(clang_getRangeStart(clang_getCursorExtent(k)));
      if (off < semi1) init.push_back(k);
      else if (off < semi2) cond.push_back(k);
      else if (off < rparen) inc.push_back(k);
      else body.push_back(k);
    }
  } else {
    body = kids;  // macro-generated or malformed: keep source order
  }
  for (CXCursor k : init) walk(k, ctx);
  emit(ctx, "LOOP", line);
  for (CXCursor k : cond) walk(k, ctx);
  for (CXCursor k : body) walk(k, ctx);
  for (CXCursor k : inc) walk(k, ctx);
}

void walk(CXCursor c, WalkContext* ctx) {
  CXSourceLocation loc = clang_getCursorLocation(c);
  if (!clang_Location_isFromMainFile(loc)) return;
  unsigned line = 0;
  clang_getExpansionLocation(loc, nullptr, &line, nullptr, nullptr);
  if (line <= ctx->prelude_lines) return;  // stub declarations

  if (!ctx->skip.empty() && ctx->skip.contains(c)) return;

  CXCursorKind kind = clang_getCursorKind(c);
  if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
      kind == CXCursor_FunctionTemplate || kind == CXCursor_Constructor) {
    CursorSet saved = std::move(ctx->skip);
    ctx->skip = CursorSet();
    findDeadCode(c, &ctx->skip);
    emit(ctx, canonicalName(c), line);
    for (CXCursor k : children(c)) walk(k, ctx);
    ctx->skip = std::move(saved);
    return;
  }
  if (kind == CXCursor_ForStmt) {
    walkFor(c, line, ctx);
    return;
  }
  if (kind == CXCursor_CompoundAssignOperator) {
    // x += 1  ->  INC x
    std::string op = binaryOpSpelling(c);
    auto kids = children(c);
    if ((op == "+=" || op == "-=") && kids.size() == 2 && isLiteralOne(kids[1])) {
      emit(ctx, op == "+=" ? "INC" : "DEC", line);
      walk(kids[0], ctx);
      return;
    }
  }
  if (kind == CXCursor_BinaryOperator && binaryOpSpelling(c) == "=") {
    // x = x op y  ->  ASSIGN_OP(op) x y ;  x = x + 1  ->  INC x
    auto kids = children(c);
    if (kids.size() == 2) {
      CXCursor rhs = stripSugar(kids[1]);
      if (clang_getCursorKind(rhs) == CXCursor_BinaryOperator) {
        std::string op = binaryOpSpelling(rhs);
        auto rk = children(rhs);
        static const char* kArith[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
        bool arith = std::find(std::begin(kArith), std::end(kArith), op) != std::end(kArith);
        if (arith && rk.size() == 2 && sameVariable(kids[0], rk[0])) {
          if ((op == "+" || op == "-") && isLiteralOne(rk[1])) {
            emit(ctx, op == "+" ? "INC" : "DEC", line);
            walk(kids[0], ctx);
          } else {
            emit(ctx, "ASSIGN_OP" + op, line);
            walk(kids[0], ctx);
            walk(rk[1], ctx);
          }
          return;
        }
      }
    }
  }

  std::string name = canonicalName(c);
  if (!name.empty()) emit(ctx, name, line);
  for (CXCursor k : children(c)) walk(k, ctx);
}

// ------------------------------------------------------------------ lexer modes

void lexTokens(CXTranslationUnit tu, const std::string& fname, size_t user_offset, size_t total,
               unsigned prelude_lines, bool raw, std::vector<Token>* out) {
  CXFile file = clang_getFile(tu, fname.c_str());
  CXSourceRange range =
      clang_getRange(clang_getLocationForOffset(tu, file, static_cast<unsigned>(user_offset)),
                     clang_getLocationForOffset(tu, file, static_cast<unsigned>(total)));
  CXToken* toks = nullptr;
  unsigned n = 0;
  clang_tokenize(tu, range, &toks, &n);
  out->reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    CXTokenKind kind = clang_getTokenKind(toks[i]);
    if (kind == CXToken_Comment) continue;
    unsigned line = 0;
    clang_getSpellingLocation(clang_getTokenLocation(tu, toks[i]), nullptr, &line, nullptr, nullptr);
    std::string sp = toStd(clang_getTokenSpelling(tu, toks[i]));
    if (sp == "#" && i + 1 < n) {
      // Skip preprocessor directive lines entirely (#include, #define ...).
      unsigned l2 = line;
      while (i + 1 < n) {
        unsigned nl = 0;
        clang_getSpellingLocation(clang_getTokenLocation(tu, toks[i + 1]), nullptr, &nl, nullptr, nullptr);
        if (nl != l2) break;
        ++i;
      }
      continue;
    }
    if (!raw) {
      if (kind == CXToken_Identifier) sp = "ID";
      else if (kind == CXToken_Literal) sp = (sp[0] == '"' || sp[0] == '\'') ? "STR" : "NUM";
    }
    out->push_back({internCached(sp), line > prelude_lines ? line - prelude_lines : 1});
  }
  clang_disposeTokens(tu, toks, n);
}

}  // namespace

uint32_t internToken(const std::string& s) { return internCached(s); }
std::string tokenString(uint32_t id) { return interner().str(id); }

TokenizerMode parseTokenizerMode(const std::string& s) {
  if (s == "ast") return TokenizerMode::AST;
  if (s == "lex" || s == "lexical") return TokenizerMode::Lexical;
  if (s == "raw") return TokenizerMode::Raw;
  throw std::invalid_argument("tokenizer must be ast, lex or raw: " + s);
}

const char* toString(TokenizerMode m) {
  switch (m) {
    case TokenizerMode::AST: return "ast";
    case TokenizerMode::Lexical: return "lex";
    case TokenizerMode::Raw: return "raw";
  }
  return "?";
}

namespace {

// The prelude is compiled once into a precompiled header so each file parse
// only loads it (mmap) instead of re-parsing ~200 lines of declarations.
// Returns "" if PCH generation failed; callers then prepend the text.
const std::string& stubPchPath() {
  static std::string path;
  static std::once_flag once;
  std::call_once(once, [] {
    std::string dir = std::filesystem::temp_directory_path().string();
    std::string base = dir + "/plag_stub_" + std::to_string(::getpid());
    std::string header = base + ".hpp", pch = base + ".pch";
    {
      std::ofstream out(header);
      out << stdStubPrelude();
    }
    CXIndex idx = clang_createIndex(0, 0);
    const char* args[] = {"-x", "c++-header", "-std=c++17", "-w"};
    CXTranslationUnit tu = clang_parseTranslationUnit(idx, header.c_str(), args, 4, nullptr, 0,
                                                      CXTranslationUnit_ForSerialization);
    if (tu != nullptr && clang_saveTranslationUnit(tu, pch.c_str(), clang_defaultSaveOptions(tu)) ==
                             CXSaveError_None) {
      path = pch;
      std::atexit([] {
        std::remove(stubPchPath().c_str());
        std::remove((stubPchPath().substr(0, stubPchPath().size() - 4) + ".hpp").c_str());
      });
    }
    if (tu) clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
  });
  return path;
}

}  // namespace

TokenizeResult tokenizeSource(const std::string& source, const std::string& filename,
                              TokenizerMode mode) {
  TokenizeResult res;
  const std::string& pch = mode == TokenizerMode::AST ? stubPchPath() : std::string();
  const bool use_prelude = mode == TokenizerMode::AST && pch.empty();
  const std::string& prelude = stdStubPrelude();
  std::string buffer;
  buffer.reserve(prelude.size() + source.size() + 2);
  if (use_prelude) buffer = prelude + "\n";
  size_t user_offset = buffer.size();
  unsigned prelude_lines = use_prelude ? countLines(buffer) : 0;
  buffer += source;

  // Clang decides the language from the extension; force C++ either way.
  std::string fname = filename.empty() ? "input.cpp" : filename;
  CXUnsavedFile unsaved{fname.c_str(), buffer.data(), static_cast<unsigned long>(buffer.size())};
  std::vector<const char*> args = {"-x", "c++", "-std=c++17", "-w", "-ferror-limit=0",
                                   "-fno-spell-checking"};
  if (!pch.empty()) {
    args.push_back("-include-pch");
    args.push_back(pch.c_str());
  }
  unsigned flags = CXTranslationUnit_SingleFileParse | CXTranslationUnit_KeepGoing |
                   CXTranslationUnit_Incomplete;
  if (mode != TokenizerMode::AST) flags |= CXTranslationUnit_SkipFunctionBodies;

  CXTranslationUnit tu = nullptr;
  CXErrorCode err = clang_parseTranslationUnit2(threadIndex(), fname.c_str(), args.data(),
                                                static_cast<int>(args.size()), &unsaved, 1, flags, &tu);
  if (err != CXError_Success || tu == nullptr) {
    res.ok = false;
    res.message = "libclang failed to parse " + fname;
    return res;
  }
  unsigned ndiag = clang_getNumDiagnostics(tu);
  for (unsigned i = 0; i < ndiag; ++i) {
    CXDiagnostic d = clang_getDiagnostic(tu, i);
    if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
      res.errors++;
      if (res.diagnostics.size() < 5)
        res.diagnostics.push_back(toStd(clang_formatDiagnostic(d, CXDiagnostic_DisplaySourceLocation)));
    }
    clang_disposeDiagnostic(d);
  }

  if (mode == TokenizerMode::AST) {
    WalkContext ctx{tu, prelude_lines, &res.tokens, {}};
    for (CXCursor top : children(clang_getTranslationUnitCursor(tu))) walk(top, &ctx);
  } else {
    lexTokens(tu, fname, user_offset, buffer.size(), prelude_lines, mode == TokenizerMode::Raw,
              &res.tokens);
  }
  clang_disposeTranslationUnit(tu);
  return res;
}

TokenizeResult tokenizeFile(const std::string& path, TokenizerMode mode) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    TokenizeResult r;
    r.ok = false;
    r.message = "cannot read " + path;
    return r;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  // The unsaved-file name must be unique per thread only; keep the real
  // extension so diagnostics stay readable.
  return tokenizeSource(ss.str(), path, mode);
}

}  // namespace plag
