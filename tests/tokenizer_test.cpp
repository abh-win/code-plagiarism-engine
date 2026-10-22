#include <gtest/gtest.h>

#include <algorithm>

#include "plag/tokenizer.h"

using namespace plag;

namespace {

const char* kOriginal = R"(#include <bits/stdc++.h>
using namespace std;

int sumOfSquares(const vector<int>& values) {
  int total = 0;
  for (int i = 0; i < (int)values.size(); i++) {
    total += values[i] * values[i];
  }
  return total;
}

int main() {
  int n;
  cin >> n;
  vector<int> v(n);
  for (int i = 0; i < n; ++i) cin >> v[i];
  cout << sumOfSquares(v) << endl;
  return 0;
}
)";

// Same program: identifiers renamed, reformatted, comments added, for -> while,
// i++ -> ++i, a < b -> b > a, extra parentheses/braces.
const char* kObfuscated = R"(#include <iostream>
#include <vector>
using namespace std;
// helper
int f(const vector<int>& xs)
{
    int acc = 0;   /* accumulator */
    int j = 0;
    while ((int)xs.size() > j)
    {
        acc += (xs[j] * xs[j]);
        ++j;
    }
    return acc;
}
int main()
{
    int count; cin >> count;
    vector<int> data(count);
    for (int k = 0; k < count; ++k) { cin >> data[k]; }
    cout << f(data) << endl;
    return 0;
}
)";

std::vector<std::string> names(const TokenizeResult& r) {
  std::vector<std::string> out;
  for (const auto& t : r.tokens) out.push_back(tokenString(t.id));
  return out;
}

std::string renameAll(std::string s) {
  auto rep = [&](const std::string& a, const std::string& b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
  };
  rep("sumOfSquares", "zz_q");
  rep("values", "arr");
  rep("total", "t_");
  return s;
}

}  // namespace

TEST(Tokenizer, AstIsInvariantToRenaming) {
  auto a = tokenizeSource(kOriginal, "a.cpp", TokenizerMode::AST);
  auto b = tokenizeSource(renameAll(kOriginal), "b.cpp", TokenizerMode::AST);
  ASSERT_TRUE(a.ok);
  EXPECT_GT(a.tokens.size(), 40u);
  EXPECT_EQ(names(a), names(b));
}

TEST(Tokenizer, LexicalIsInvariantToRenamingButRawIsNot) {
  EXPECT_EQ(names(tokenizeSource(kOriginal, "a.cpp", TokenizerMode::Lexical)),
            names(tokenizeSource(renameAll(kOriginal), "b.cpp", TokenizerMode::Lexical)));
  EXPECT_NE(names(tokenizeSource(kOriginal, "a.cpp", TokenizerMode::Raw)),
            names(tokenizeSource(renameAll(kOriginal), "b.cpp", TokenizerMode::Raw)));
}

TEST(Tokenizer, StubPreludeKeepsStdStatementsInTheAst) {
  auto r = tokenizeSource(kOriginal, "a.cpp", TokenizerMode::AST);
  auto n = names(r);
  // `cin >> n` must survive as a call to an overloaded operator (CALL) - without
  // the prelude, clang drops the statement because `cin` is undeclared.
  int calls = 0;
  for (auto& s : n) calls += s == "CALL";
  EXPECT_GE(calls, 4);
  EXPECT_EQ(r.errors, 0u);
}

TEST(Tokenizer, CanonicalisesLoopsAndComparisons) {
  auto a = names(tokenizeSource("void f(int n){ for(int i=0;i<n;i++){} }", "a.cpp", TokenizerMode::AST));
  auto b = names(tokenizeSource("void f(int n){ int i=0; while(n>i){ ++i; } }", "b.cpp", TokenizerMode::AST));
  EXPECT_NE(std::find(a.begin(), a.end(), "LOOP"), a.end());
  EXPECT_NE(std::find(b.begin(), b.end(), "LOOP"), b.end());
  EXPECT_NE(std::find(b.begin(), b.end(), "CMP"), b.end());
  EXPECT_EQ(std::find(b.begin(), b.end(), "WhileStmt"), b.end());
}

TEST(Tokenizer, LineNumbersReferToOriginalFile) {
  auto r = tokenizeSource("\n\nint g() {\n  return 42;\n}\n", "x.cpp", TokenizerMode::AST);
  ASSERT_GE(r.tokens.size(), 2u);
  EXPECT_EQ(tokenString(r.tokens[0].id), "FUNC");
  EXPECT_EQ(r.tokens[0].line, 3u);
  EXPECT_EQ(r.tokens.back().line, 4u);
}

TEST(Tokenizer, ObfuscatedCopyKeepsMostAstStructure) {
  auto a = names(tokenizeSource(kOriginal, "a.cpp", TokenizerMode::AST));
  auto b = names(tokenizeSource(kObfuscated, "b.cpp", TokenizerMode::AST));
  // Longest common subsequence ratio as a crude structural similarity.
  std::vector<std::vector<int>> dp(a.size() + 1, std::vector<int>(b.size() + 1, 0));
  for (size_t i = 1; i <= a.size(); ++i)
    for (size_t j = 1; j <= b.size(); ++j)
      dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1 : std::max(dp[i - 1][j], dp[i][j - 1]);
  double ratio = 2.0 * dp[a.size()][b.size()] / (a.size() + b.size());
  EXPECT_GT(ratio, 0.85);
}

TEST(Tokenizer, DeadCodeIsEliminated) {
  const char* clean = R"(int f(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) s += i;
  return s;
})";
  const char* junk = R"(int f(int n) {
  int t = 17;
  if (t < 0) t = -t;
  int s = 0;
  long long z = 0;
  z += 4;
  for (int i = 0; i < n; i++) { bool b = false; if (b) { b = !b; } s += i; }
  int q = 3 * 9;
  (void)q;
  return s;
})";
  EXPECT_EQ(names(tokenizeSource(clean, "a.cpp", TokenizerMode::AST)),
            names(tokenizeSource(junk, "b.cpp", TokenizerMode::AST)));
}

TEST(Tokenizer, LiveCodeIsKept) {
  // `c` reaches the output through `return`, `d` through a call: both live.
  auto n = names(tokenizeSource(R"(int g(int n) {
  int c = 0;
  c += 2;
  int d = 5;
  printf("%d", d);
  return c;
})", "a.cpp", TokenizerMode::AST));
  int vars = 0;
  for (auto& s : n) vars += s == "VAR";
  EXPECT_EQ(vars, 2);
}
