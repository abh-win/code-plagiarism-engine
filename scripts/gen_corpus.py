#!/usr/bin/env python3
"""Synthetic C++ corpus with labelled plagiarism, for evaluating the engine.

Every *original* program is assembled from 3-5 randomly chosen algorithm
snippets (out of ~30, most with several implementation variants), random
constants, random identifier names and a random formatting style.
Each original gets 0-3 *plagiarised copies*, re-rendered from the same logical
program with an obfuscation level:

  L1  rename every identifier, reformat (indent / brace style), change comments
  L2  L1 + reorder functions, for<->while, i++/++i/i+=1, a<b -> b>a,
      x+=y <-> x=x+y
  L3  L2 + dead-code insertion, split/merged declarations, redundant parentheses

Ground truth (labels.csv) lists every positive pair: all pairs of files that
derive from the same original. All other pairs are negatives, including
independent programs that happen to use the same algorithm (with their own
names, constants and style), which is what makes the task non-trivial.

Usage: gen_corpus.py OUT_DIR [--originals 2600] [--seed 1]
"""

import argparse
import csv
import os
import random
import re
import string

# --------------------------------------------------------------------------- names

POOLS = {
    "i": ["i", "idx", "k", "p", "it", "pos"],
    "j": ["j", "jj", "q", "inner", "t", "m2"],
    "n": ["n", "len", "sz", "cnt", "N", "total_n"],
    "arr": ["arr", "a", "v", "nums", "data", "vals", "xs"],
    "res": ["res", "ans", "result", "out", "best", "acc"],
    "tmp": ["tmp", "t", "swp", "hold", "aux"],
    "s": ["s", "str", "text", "word", "line"],
    "x": ["x", "val", "key", "target", "q"],
}


def rand_ident(rng):
    first = rng.choice(string.ascii_lowercase)
    rest = "".join(rng.choice(string.ascii_lowercase + string.digits + "_") for _ in range(rng.randint(1, 7)))
    return first + rest


class Names:
    """Consistent identifier mapping. `obfuscated` draws random names."""

    RESERVED = {"int", "for", "if", "do", "while", "auto", "char", "long", "bool", "void", "new",
                "case", "else", "std", "main", "cin", "cout", "endl", "max", "min", "swap", "sort",
                "abs", "map", "set", "pow", "sqrt", "string", "vector", "pair", "try", "and", "or",
                "not", "xor", "goto", "this", "true", "false", "using", "return", "break", "const",
                "static", "struct", "class", "enum", "union", "delete", "default", "double", "float",
                "short", "signed", "switch", "unsigned", "volatile", "inline", "friend", "public",
                "private", "virtual", "operator", "template", "typename", "namespace", "register",
                "extern", "sizeof", "typedef", "throw", "catch", "explicit", "mutable", "asm"}

    def __init__(self, rng, obfuscated):
        self.rng = rng
        self.obf = obfuscated
        self.map = {}
        self.used = set()

    def __call__(self, key):
        if key not in self.map:
            base = key.split("#")[0]
            for _ in range(1000):
                if self.obf:
                    cand = rand_ident(self.rng)
                else:
                    pool = POOLS.get(base)
                    cand = self.rng.choice(pool) if pool else base
                    if cand in self.used:
                        cand = cand + str(self.rng.randint(2, 99))
                if cand not in self.used and cand not in self.RESERVED:
                    break
            self.used.add(cand)
            self.map[key] = cand
        return self.map[key]


# --------------------------------------------------------------------------- rendering context

class Ctx:
    def __init__(self, rng, names, style):
        self.rng = rng
        self.n = names
        self.st = style

    # expressions -----------------------------------------------------------
    def P(self, e):
        return f"({e})" if self.st["parens"] and self.rng.random() < 0.5 else e

    def lt(self, a, b):
        return f"{b} > {a}" if self.st["cmp"] == "gt" else f"{a} < {b}"

    def le(self, a, b):
        return f"{b} >= {a}" if self.st["cmp"] == "gt" else f"{a} <= {b}"

    def inc(self, v):
        m = self.st["inc"]
        return {"post": f"{v}++", "pre": f"++{v}", "plus1": f"{v} += 1"}[m]

    def dec(self, v):
        m = self.st["inc"]
        return {"post": f"{v}--", "pre": f"--{v}", "plus1": f"{v} -= 1"}[m]

    def addto(self, x, y, op="+"):
        return f"{x} {op}= {y};" if self.st["compound"] else f"{x} = {x} {op} {self.P(y)};"

    # statements ------------------------------------------------------------
    def decls(self, typ, pairs):
        """pairs: [(name, init)] -> one or several declarations."""
        if self.st["merge_decls"] and len(pairs) > 1:
            return [f"{typ} " + ", ".join(f"{n} = {v}" for n, v in pairs) + ";"]
        return [f"{typ} {n} = {v};" for n, v in pairs]

    def loop(self, var, start, end, body, cmp="lt"):
        cond = self.lt(var, end) if cmp == "lt" else self.le(var, end)
        if self.st["loop"] == "while":
            return [("block", "", [f"int {var} = {start};",
                                   ("block", f"while ({cond})", body + [self.inc(var) + ";"])])]
        return [("block", f"for (int {var} = {start}; {cond}; {self.inc(var)})", body)]

    def down_loop(self, var, start, end, body):
        cond = self.le(end, var)
        if self.st["loop"] == "while":
            return [("block", "", [f"int {var} = {start};",
                                   ("block", f"while ({cond})", body + [self.dec(var) + ";"])])]
        return [("block", f"for (int {var} = {start}; {cond}; {self.dec(var)})", body)]

    def dead(self):
        if self.rng.random() >= self.st["dead"]:
            return []
        d = self.n(f"tmp#dead{self.rng.randint(0, 10**9)}")
        k = self.rng.randint(1, 97)
        return self.rng.choice([
            [f"int {d} = {k};", f"if ({d} < 0) {d} = -{d};"],
            [f"int {d} = {k} * {self.rng.randint(2, 9)};", f"(void){d};"],
            [f"bool {d} = false;", ("block", f"if ({d})", [f"{d} = !{d};"])],
            [f"long long {d} = 0;", f"{d} += {k};"],
        ])

    def with_dead(self, stmts):
        out = []
        for s in stmts:
            out.extend(self.dead())
            out.append(s)
        return out


def render(stmts, style, level=0):
    ind = style["indent"]
    lines = []
    for s in stmts:
        if isinstance(s, str):
            lines.append(ind * level + s)
        else:
            _, header, body = s
            if header == "":
                lines.append(ind * level + "{")
                lines += render(body, style, level + 1)
                lines.append(ind * level + "}")
            elif style["brace"] == "same":
                lines.append(ind * level + header + " {")
                lines += render(body, style, level + 1)
                lines.append(ind * level + "}")
            else:
                lines.append(ind * level + header)
                lines.append(ind * level + "{")
                lines += render(body, style, level + 1)
                lines.append(ind * level + "}")
    return lines


# --------------------------------------------------------------------------- snippets
# Each snippet(c, P) returns (function_stmts, main_call_stmts). `c.n(key)`
# names identifiers; keys are namespaced by snippet id so names stay distinct.

AUTHOR_EXTRAS = [  # live incidental code: debug output, validation, call counting
    lambda c, v, k: [f"static int {v} = 0;", c.inc(v) + ";",
                     ("block", f"if ({v} > {k * 1000})", [f"cerr << \"called \" << {v} << endl;"])],
    lambda c, v, k: [f"int {v} = {k};", ("block", f"if ({v} > {k + 7})", [f"{v} = {k + 7};"]),
                     f"cerr << \"limit \" << {v} << endl;"],
    lambda c, v, k: [("block", f"if ({k} < 0)", [f"cerr << \"invalid input\" << endl;"])],
    lambda c, v, k: [f"const int {v} = {k} * {k};", ("block", f"if ({v} % 7 == 3)", [f"cerr << \"debug \" << {v} << endl;"])],
    lambda c, v, k: [f"double {v} = {k} / 2.0;", f"cerr << fixed << {v} << endl;"],
    lambda c, v, k: [f"bool {v} = {k} % 2 == 0;", ("block", f"if (!{v})", ["cerr << \"odd\" << endl;"])],
]


def fn(c, ret, name, params, body):
    """Wraps a function body. Inserts the original author's incidental code
    (fixed by the logical program, so plagiarised copies keep it while
    independent authors don't share it), then optional dead code."""
    P, u = c.cur
    r = random.Random(P["seed"])
    body = list(body)
    for e in range(P["extras"]):
        v = c.n(f"tmp#extra{u}_{e}")
        stmts = r.choice(AUTHOR_EXTRAS)(c, v, r.randint(1, 50))
        pos = r.randint(0, max(0, len(body) - 1))
        body[pos:pos] = stmts
    return [("block", f"{ret} {name}({params})", c.with_dead(body))]


def s_sum(c, P, u):
    f, a, n, r, i = c.n(f"sum{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"res#{u}"), c.n(f"i#{u}")
    body = c.decls("long long", [(r, "0")]) + c.loop(i, "0", n, [c.addto(r, f"{a}[{i}]")]) + [f"return {r};"]
    return fn(c, "long long", f, f"const vector<int>& {a}, int {n}", body), [f"cout << {f}(ARR, N) << endl;"]


def s_max(c, P, u):
    f, a, n, r, i = c.n(f"maxel{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"res#{u}"), c.n(f"i#{u}")
    if P["v"] == 0:
        body = [f"int {r} = {a}[0];"] + c.loop(i, "1", n, [("block", f"if ({c.lt(r, f'{a}[{i}]')})", [f"{r} = {a}[{i}];"])])
    else:
        body = [f"int {r} = INT_MIN;"] + c.loop(i, "0", n, [f"{r} = max({r}, {a}[{i}]);"])
    return fn(c, "int", f, f"const vector<int>& {a}, int {n}", body + [f"return {r};"]), [f"cout << {f}(ARR, N) << endl;"]


def s_bubble(c, P, u):
    f, a, n, i, j, t = (c.n(f"bsort{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"tmp#{u}"))
    if P["v"] == 0:
        swap = [f"int {t} = {a}[{j}];", f"{a}[{j}] = {a}[{j} + 1];", f"{a}[{j} + 1] = {t};"]
    else:
        swap = [f"swap({a}[{j}], {a}[{j} + 1]);"]
    inner = c.loop(j, "0", f"{n} - {i} - 1", [("block", f"if ({c.lt(f'{a}[{j} + 1]', f'{a}[{j}]')})", swap)])
    body = c.loop(i, "0", f"{n} - 1", inner)
    return fn(c, "void", f, f"vector<int>& {a}, int {n}", body), [f"{f}(ARR, N);", f"cout << ARR[0] << endl;"]


def s_insertion(c, P, u):
    f, a, n, i, j, x = (c.n(f"isort{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"x#{u}"))
    inner = [f"int {x} = {a}[{i}];", f"int {j} = {i} - 1;",
             ("block", f"while ({j} >= 0 && {c.lt(x, f'{a}[{j}]')})", [f"{a}[{j} + 1] = {a}[{j}];", c.dec(j) + ";"]),
             f"{a}[{j} + 1] = {x};"]
    return fn(c, "void", f, f"vector<int>& {a}, int {n}", c.loop(i, "1", n, inner)), [f"{f}(ARR, N);", "cout << ARR[N - 1] << endl;"]


def s_selection(c, P, u):
    f, a, n, i, j, m = (c.n(f"ssort{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"res#{u}"))
    inner = [f"int {m} = {i};"] + c.loop(j, f"{i} + 1", n, [("block", f"if ({c.lt(f'{a}[{j}]', f'{a}[{m}]')})", [f"{m} = {j};"])]) + [f"swap({a}[{i}], {a}[{m}]);"]
    return fn(c, "void", f, f"vector<int>& {a}, int {n}", c.loop(i, "0", n, inner)), [f"{f}(ARR, N);", "cout << ARR[0] << endl;"]


def s_bsearch(c, P, u):
    f, a, n, x = c.n(f"bsearch{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"x#{u}")
    lo, hi, mid = c.n(f"i#lo{u}"), c.n(f"j#hi{u}"), c.n(f"tmp#mid{u}")
    if P["v"] == 0:
        body = c.decls("int", [(lo, "0"), (hi, f"{n} - 1")]) + [
            ("block", f"while ({c.le(lo, hi)})", [
                f"int {mid} = {lo} + ({hi} - {lo}) / 2;",
                ("block", f"if ({a}[{mid}] == {x})", [f"return {mid};"]),
                ("block", f"if ({c.lt(f'{a}[{mid}]', x)})", [f"{lo} = {mid} + 1;"]),
                ("block", "else", [f"{hi} = {mid} - 1;"]),
            ]), "return -1;"]
    else:
        body = c.decls("int", [(lo, "-1"), (hi, n)]) + [
            ("block", f"while ({hi} - {lo} > 1)", [
                f"int {mid} = ({lo} + {hi}) / 2;",
                ("block", f"if ({a}[{mid}] >= {x})", [f"{hi} = {mid};"]),
                ("block", "else", [f"{lo} = {mid};"]),
            ]), f"return ({hi} < {n} && {a}[{hi}] == {x}) ? {hi} : -1;"]
    return fn(c, "int", f, f"const vector<int>& {a}, int {n}, int {x}", body), [f"cout << {f}(ARR, N, {P['k']}) << endl;"]


def s_gcd(c, P, u):
    f, a, b, t = c.n(f"gcd{u}"), c.n(f"x#a{u}"), c.n(f"x#b{u}"), c.n(f"tmp#{u}")
    if P["v"] == 0:
        body = [("block", f"while ({b} != 0)", [f"long long {t} = {a} % {b};", f"{a} = {b};", f"{b} = {t};"]), f"return {a};"]
    else:
        body = [("block", f"if ({b} == 0)", [f"return {a};"]), f"return {f}({b}, {a} % {b});"]
    return fn(c, "long long", f, f"long long {a}, long long {b}", body), [f"cout << {f}(ARR[0], ARR[N - 1]) << endl;"]


def s_sieve(c, P, u):
    f, lim, p, i, j, cnt = (c.n(f"sieve{u}"), c.n(f"n#lim{u}"), c.n(f"arr#p{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"res#{u}"))
    body = [f"vector<bool> {p}({lim} + 1, true);", f"{p}[0] = false;", f"{p}[1] = false;"]
    if P["v"] == 0:
        inner = [("block", f"if ({p}[{i}])", [("block", f"for (int {j} = {i} * {i}; {j} <= {lim}; {j} += {i})", [f"{p}[{j}] = false;"])])]
        body += [("block", f"for (int {i} = 2; {i} * {i} <= {lim}; {c.inc(i)})", inner)]
    else:
        inner = [("block", f"if (!{p}[{i}])", ["continue;"]), f"int {j} = 2 * {i};",
                 ("block", f"while ({c.le(j, lim)})", [f"{p}[{j}] = false;", f"{j} += {i};"])]
        body += c.loop(i, "2", lim, inner, cmp="le")
    body += [f"int {cnt} = 0;"] + c.loop(i, "2", lim, [("block", f"if ({p}[{i}])", [c.inc(cnt) + ";"])], cmp="le") + [f"return {cnt};"]
    return fn(c, "int", f, f"int {lim}", body), [f"cout << {f}({P['k'] * 100}) << endl;"]


def s_fib(c, P, u):
    f, n, dp, i = c.n(f"fib{u}"), c.n(f"n#{u}"), c.n(f"arr#dp{u}"), c.n(f"i#{u}")
    mod = P["mod"]
    if P["v"] == 0:
        body = [f"vector<long long> {dp}({n} + 2, 0);", f"{dp}[1] = 1;"] + \
               c.loop(i, "2", n, [f"{dp}[{i}] = ({dp}[{i} - 1] + {dp}[{i} - 2]) % {mod};"], cmp="le") + [f"return {dp}[{n}];"]
    else:
        a, b, t = c.n(f"x#a{u}"), c.n(f"x#b{u}"), c.n(f"tmp#{u}")
        body = c.decls("long long", [(a, "0"), (b, "1")]) + c.loop(i, "0", n, [f"long long {t} = ({a} + {b}) % {mod};", f"{a} = {b};", f"{b} = {t};"]) + [f"return {a};"]
    return fn(c, "long long", f, f"int {n}", body), [f"cout << {f}(N + {P['k']}) << endl;"]


def s_powmod(c, P, u):
    f, b, e, m, r = c.n(f"powmod{u}"), c.n(f"x#b{u}"), c.n(f"x#e{u}"), c.n(f"x#m{u}"), c.n(f"res#{u}")
    body = [f"long long {r} = 1;", f"{b} %= {m};",
            ("block", f"while ({e} > 0)", [("block", f"if ({e} & 1)", [f"{r} = {r} * {b} % {m};"]),
                                           f"{b} = {b} * {b} % {m};", f"{e} >>= 1;"]), f"return {r};"]
    return fn(c, "long long", f, f"long long {b}, long long {e}, long long {m}", body), [f"cout << {f}(ARR[0], N, {P['mod']}) << endl;"]


def s_reverse(c, P, u):
    f, s, i, j = c.n(f"revstr{u}"), c.n(f"s#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}")
    if P["v"] == 0:
        body = c.decls("int", [(i, "0"), (j, f"(int){s}.size() - 1")]) + [("block", f"while ({c.lt(i, j)})", [f"swap({s}[{i}], {s}[{j}]);", c.inc(i) + ";", c.dec(j) + ";"])] + [f"return {s};"]
    else:
        r = c.n(f"res#{u}")
        body = [f"string {r} = \"\";"] + c.down_loop(i, f"(int){s}.size() - 1", "0", [f"{r} += {s}[{i}];"]) + [f"return {r};"]
    return fn(c, "string", f, f"string {s}", body), [f"cout << {f}(STR) << endl;"]


def s_palin(c, P, u):
    f, s, i, n = c.n(f"ispal{u}"), c.n(f"s#{u}"), c.n(f"i#{u}"), c.n(f"n#{u}")
    body = [f"int {n} = {s}.size();"] + c.loop(i, "0", f"{n} / 2", [("block", f"if ({s}[{i}] != {s}[{n} - 1 - {i}])", ["return false;"])]) + ["return true;"]
    return fn(c, "bool", f, f"const string& {s}", body), [f"cout << ({f}(STR) ? \"YES\" : \"NO\") << endl;"]


def s_vowels(c, P, u):
    f, s, r, ch = c.n(f"vowels{u}"), c.n(f"s#{u}"), c.n(f"res#{u}"), c.n(f"x#ch{u}")
    if P["v"] == 0:
        body = [f"int {r} = 0;", ("block", f"for (char {ch} : {s})", [("block", f"if ({ch} == 'a' || {ch} == 'e' || {ch} == 'i' || {ch} == 'o' || {ch} == 'u')", [c.inc(r) + ";"])]), f"return {r};"]
    else:
        i = c.n(f"i#{u}")
        body = [f"int {r} = 0;", f"string vw = \"aeiou\";"] + c.loop(i, "0", f"(int){s}.size()", [("block", f"if (vw.find({s}[{i}]) != string::npos)", [c.addto(r, "1")])]) + [f"return {r};"]
    return fn(c, "int", f, f"const string& {s}", body), [f"cout << {f}(STR) << endl;"]


def s_prefix(c, P, u):
    f, a, n, p, i = c.n(f"prefix{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"arr#pre{u}"), c.n(f"i#{u}")
    body = [f"vector<long long> {p}({n} + 1, 0);"] + c.loop(i, "0", n, [f"{p}[{i} + 1] = {p}[{i}] + {a}[{i}];"]) + [f"return {p};"]
    return fn(c, "vector<long long>", f, f"const vector<int>& {a}, int {n}", body), [f"cout << {f}(ARR, N)[N] << endl;"]


def s_twosum(c, P, u):
    f, a, n, x, m, i = c.n(f"twosum{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"x#{u}"), c.n(f"res#seen{u}"), c.n(f"i#{u}")
    if P["v"] == 0:
        body = [f"map<int, int> {m};"] + c.loop(i, "0", n, [
            ("block", f"if ({m}.count({x} - {a}[{i}]))", ["return true;"]), f"{m}[{a}[{i}]] = {i};"]) + ["return false;"]
    else:
        j = c.n(f"j#{u}")
        body = c.loop(i, "0", n, c.loop(j, f"{i} + 1", n, [("block", f"if ({a}[{i}] + {a}[{j}] == {x})", ["return true;"])])) + ["return false;"]
    return fn(c, "bool", f, f"const vector<int>& {a}, int {n}, int {x}", body), [f"cout << {f}(ARR, N, {P['k']}) << endl;"]


def s_kadane(c, P, u):
    f, a, n, best, cur, i = c.n(f"kadane{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"res#{u}"), c.n(f"tmp#{u}"), c.n(f"i#{u}")
    if P["v"] == 0:
        body = c.decls("long long", [(best, f"{a}[0]"), (cur, f"{a}[0]")]) + c.loop(i, "1", n, [f"{cur} = max((long long){a}[{i}], {cur} + {a}[{i}]);", f"{best} = max({best}, {cur});"])
    else:
        body = c.decls("long long", [(best, "LLONG_MIN"), (cur, "0")]) + c.loop(i, "0", n, [c.addto(cur, f"{a}[{i}]"), ("block", f"if ({c.lt(best, cur)})", [f"{best} = {cur};"]), ("block", f"if ({c.lt(cur, '0')})", [f"{cur} = 0;"])])
    return fn(c, "long long", f, f"const vector<int>& {a}, int {n}", body + [f"return {best};"]), [f"cout << {f}(ARR, N) << endl;"]


def s_isprime(c, P, u):
    f, x, i = c.n(f"isprime{u}"), c.n(f"x#{u}"), c.n(f"i#{u}")
    body = [("block", f"if ({c.lt(x, '2')})", ["return false;"]),
            ("block", f"for (long long {i} = 2; {i} * {i} <= {x}; {c.inc(i)})", [("block", f"if ({x} % {i} == 0)", ["return false;"])]), "return true;"]
    return fn(c, "bool", f, f"long long {x}", body), [f"cout << {f}(ARR[0] + {P['k']}) << endl;"]


def s_digits(c, P, u):
    f, x, r = c.n(f"digits{u}"), c.n(f"x#{u}"), c.n(f"res#{u}")
    if P["v"] == 0:
        body = [f"int {r} = 0;", ("block", f"if ({x} == 0)", ["return 1;"]), ("block", f"while ({x} > 0)", [f"{x} /= 10;", c.inc(r) + ";"]), f"return {r};"]
    else:
        body = [f"int {r} = 0;", ("block", f"while ({x} > 0)", [c.addto(r, f"{x} % 10"), f"{x} /= 10;"]), f"return {r};"]
    return fn(c, "int", f, f"long long {x}", body), [f"cout << {f}(ARR[N - 1]) << endl;"]


def s_freq(c, P, u):
    f, s, m, best, r = c.n(f"freq{u}"), c.n(f"s#{u}"), c.n(f"res#cnt{u}"), c.n(f"x#best{u}"), c.n(f"res#{u}")
    ch, kv = c.n(f"x#ch{u}"), c.n(f"x#kv{u}")
    body = [f"map<char, int> {m};", ("block", f"for (char {ch} : {s})", [f"{m}[{ch}]++;"]),
            f"char {r} = ' ';", f"int {best} = 0;",
            ("block", f"for (auto& {kv} : {m})", [("block", f"if ({c.lt(best, f'{kv}.second')})", [f"{best} = {kv}.second;", f"{r} = {kv}.first;"])]),
            f"return {r};"]
    return fn(c, "char", f, f"const string& {s}", body), [f"cout << {f}(STR) << endl;"]


def s_matmul(c, P, u):
    f, A, B, C, n, i, j, k = (c.n(f"matmul{u}"), c.n(f"arr#A{u}"), c.n(f"arr#B{u}"), c.n(f"arr#C{u}"), c.n(f"n#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"i#k{u}"))
    body = [f"vector<vector<long long>> {C}({n}, vector<long long>({n}, 0));"] + \
           c.loop(i, "0", n, c.loop(j, "0", n, c.loop(k, "0", n, [c.addto(f"{C}[{i}][{j}]", f"{A}[{i}][{k}] * {B}[{k}][{j}]")]))) + [f"return {C};"]
    call = [f"vector<vector<long long>> M{u}(3, vector<long long>(3, {P['k']}));", f"cout << {f}(M{u}, M{u}, 3)[1][1] << endl;"]
    return fn(c, "vector<vector<long long>>", f, f"const vector<vector<long long>>& {A}, const vector<vector<long long>>& {B}, int {n}", body), call


def s_transpose(c, P, u):
    f, A, T, n, m, i, j = c.n(f"transp{u}"), c.n(f"arr#A{u}"), c.n(f"arr#T{u}"), c.n(f"n#{u}"), c.n(f"n#m{u}"), c.n(f"i#{u}"), c.n(f"j#{u}")
    body = [f"int {n} = {A}.size();", f"int {m} = {A}[0].size();", f"vector<vector<int>> {T}({m}, vector<int>({n}));"] + \
           c.loop(i, "0", n, c.loop(j, "0", m, [f"{T}[{j}][{i}] = {A}[{i}][{j}];"])) + [f"return {T};"]
    call = [f"vector<vector<int>> G{u}(2, vector<int>(4, {P['k']}));", f"cout << {f}(G{u}).size() << endl;"]
    return fn(c, "vector<vector<int>>", f, f"const vector<vector<int>>& {A}", body), call


def s_brackets(c, P, u):
    f, s, st, ch = c.n(f"balanced{u}"), c.n(f"s#{u}"), c.n(f"arr#st{u}"), c.n(f"x#ch{u}")
    body = [f"stack<char> {st};", ("block", f"for (char {ch} : {s})", [
        ("block", f"if ({ch} == '(' || {ch} == '[' || {ch} == '{{')", [f"{st}.push({ch});"]),
        ("block", f"else if ({ch} == ')' || {ch} == ']' || {ch} == '}}')", [
            ("block", f"if ({st}.empty())", ["return false;"]),
            f"char o = {st}.top();", f"{st}.pop();",
            ("block", f"if ((o == '(' && {ch} != ')') || (o == '[' && {ch} != ']') || (o == '{{' && {ch} != '}}'))", ["return false;"])])]),
        f"return {st}.empty();"]
    return fn(c, "bool", f, f"const string& {s}", body), [f"cout << {f}(STR) << endl;"]


def s_lis(c, P, u):
    f, a, n, dp, i, j, r = (c.n(f"lis{u}"), c.n(f"arr#{u}"), c.n(f"n#{u}"), c.n(f"arr#dp{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"), c.n(f"res#{u}"))
    body = [f"vector<int> {dp}({n}, 1);", f"int {r} = 0;"] + c.loop(i, "0", n,
        c.loop(j, "0", i, [("block", f"if ({c.lt(f'{a}[{j}]', f'{a}[{i}]')})", [f"{dp}[{i}] = max({dp}[{i}], {dp}[{j}] + 1);"])]) + [f"{r} = max({r}, {dp}[{i}]);"]) + [f"return {r};"]
    return fn(c, "int", f, f"const vector<int>& {a}, int {n}", body), [f"cout << {f}(ARR, N) << endl;"]


def s_knapsack(c, P, u):
    f, w, v, n, W, dp, i, j = (c.n(f"knap{u}"), c.n(f"arr#w{u}"), c.n(f"arr#v{u}"), c.n(f"n#{u}"), c.n(f"n#cap{u}"), c.n(f"arr#dp{u}"), c.n(f"i#{u}"), c.n(f"j#{u}"))
    body = [f"vector<long long> {dp}({W} + 1, 0);"] + c.loop(i, "0", n, c.down_loop(j, W, f"{w}[{i}]", [f"{dp}[{j}] = max({dp}[{j}], {dp}[{j} - {w}[{i}]] + {v}[{i}]);"])) + [f"return {dp}[{W}];"]
    return fn(c, "long long", f, f"const vector<int>& {w}, const vector<int>& {v}, int {n}, int {W}", body), [f"cout << {f}(ARR, ARR, N, {P['k'] * 10}) << endl;"]


def s_lcs(c, P, u):
    f, a, b, dp, i, j = c.n(f"lcs{u}"), c.n(f"s#a{u}"), c.n(f"s#b{u}"), c.n(f"arr#dp{u}"), c.n(f"i#{u}"), c.n(f"j#{u}")
    body = [f"int n1 = {a}.size(), n2 = {b}.size();", f"vector<vector<int>> {dp}(n1 + 1, vector<int>(n2 + 1, 0));"] + \
        c.loop(i, "1", "n1", c.loop(j, "1", "n2", [("block", f"if ({a}[{i} - 1] == {b}[{j} - 1])", [f"{dp}[{i}][{j}] = {dp}[{i} - 1][{j} - 1] + 1;"]),
                                                  ("block", "else", [f"{dp}[{i}][{j}] = max({dp}[{i} - 1][{j}], {dp}[{i}][{j} - 1]);"])], cmp="le"), cmp="le") + [f"return {dp}[n1][n2];"]
    return fn(c, "int", f, f"const string& {a}, const string& {b}", body), [f"cout << {f}(STR, \"{P['word']}\") << endl;"]


def s_coins(c, P, u):
    f, co, amt, dp, i, cn = c.n(f"coins{u}"), c.n(f"arr#{u}"), c.n(f"n#amt{u}"), c.n(f"arr#dp{u}"), c.n(f"i#{u}"), c.n(f"x#c{u}")
    body = [f"vector<long long> {dp}({amt} + 1, 0);", f"{dp}[0] = 1;",
            ("block", f"for (int {cn} : {co})", c.loop(i, cn, amt, [c.addto(f"{dp}[{i}]", f"{dp}[{i} - {cn}]")], cmp="le")),
            f"return {dp}[{amt}];"]
    return fn(c, "long long", f, f"const vector<int>& {co}, int {amt}", body), [f"cout << {f}(vector<int>{{1, 2, 5}}, {P['k'] * 3}) << endl;"]


def s_bfs(c, P, u):
    f, g, n, src, d, q, x, y = (c.n(f"bfs{u}"), c.n(f"arr#g{u}"), c.n(f"n#{u}"), c.n(f"x#src{u}"), c.n(f"arr#dist{u}"), c.n(f"arr#q{u}"), c.n(f"x#cur{u}"), c.n(f"x#nb{u}"))
    body = [f"vector<int> {d}({n}, -1);", f"queue<int> {q};", f"{d}[{src}] = 0;", f"{q}.push({src});",
            ("block", f"while (!{q}.empty())", [f"int {x} = {q}.front();", f"{q}.pop();",
                                                ("block", f"for (int {y} : {g}[{x}])", [("block", f"if ({d}[{y}] == -1)", [f"{d}[{y}] = {d}[{x}] + 1;", f"{q}.push({y});"])])]),
            f"return {d};"]
    call = [f"vector<vector<int>> E{u}(N);"] + c.loop(c.n(f"i#call{u}"), "0", "N - 1", [f"E{u}[{c.n(f'i#call{u}')}].push_back({c.n(f'i#call{u}')} + 1);"]) + [f"cout << {f}(E{u}, N, 0)[N - 1] << endl;"]
    return fn(c, "vector<int>", f, f"const vector<vector<int>>& {g}, int {n}, int {src}", body), call


def s_dfs(c, P, u):
    f, g, vis, x, y, cnt = c.n(f"dfs{u}"), c.n(f"arr#g{u}"), c.n(f"arr#vis{u}"), c.n(f"x#cur{u}"), c.n(f"x#nb{u}"), c.n(f"res#{u}")
    body = [f"{vis}[{x}] = true;", f"int {cnt} = 1;",
            ("block", f"for (int {y} : {g}[{x}])", [("block", f"if (!{vis}[{y}])", [c.addto(cnt, f"{f}({g}, {vis}, {y})")])]), f"return {cnt};"]
    call = [f"vector<vector<int>> H{u}(N);", f"vector<bool> V{u}(N, false);", f"if (N > 1) H{u}[0].push_back(1);", f"cout << {f}(H{u}, V{u}, 0) << endl;"]
    return fn(c, "int", f, f"const vector<vector<int>>& {g}, vector<bool>& {vis}, int {x}", body), call


def s_merge(c, P, u):
    f, a, b, r, i, j = c.n(f"merge{u}"), c.n(f"arr#a{u}"), c.n(f"arr#b{u}"), c.n(f"res#{u}"), c.n(f"i#{u}"), c.n(f"j#{u}")
    body = [f"vector<int> {r};"] + c.decls("size_t", [(i, "0"), (j, "0")]) + [
        ("block", f"while ({c.lt(i, f'{a}.size()')} && {c.lt(j, f'{b}.size()')})", [
            ("block", f"if ({c.le(f'{a}[{i}]', f'{b}[{j}]')})", [f"{r}.push_back({a}[{i}]);", c.inc(i) + ";"]),
            ("block", "else", [f"{r}.push_back({b}[{j}]);", c.inc(j) + ";"])]),
        ("block", f"while ({c.lt(i, f'{a}.size()')})", [f"{r}.push_back({a}[{i}++]);"]),
        ("block", f"while ({c.lt(j, f'{b}.size()')})", [f"{r}.push_back({b}[{j}++]);"]),
        f"return {r};"]
    return fn(c, "vector<int>", f, f"const vector<int>& {a}, const vector<int>& {b}", body), [f"cout << {f}(ARR, ARR).size() << endl;"]


def s_factorial(c, P, u):
    f, n, r, i = c.n(f"fact{u}"), c.n(f"n#{u}"), c.n(f"res#{u}"), c.n(f"i#{u}")
    if P["v"] == 0:
        body = [f"long long {r} = 1;"] + c.loop(i, "2", n, [f"{r} = {r} * {i} % {P['mod']};"], cmp="le") + [f"return {r};"]
    else:
        body = [("block", f"if ({c.le(n, '1')})", ["return 1;"]), f"return {n} * {f}({n} - 1) % {P['mod']};"]
    return fn(c, "long long", f, f"long long {n}", body), [f"cout << {f}(N) << endl;"]


SNIPPETS = [s_sum, s_max, s_bubble, s_insertion, s_selection, s_bsearch, s_gcd, s_sieve, s_fib,
            s_powmod, s_reverse, s_palin, s_vowels, s_prefix, s_twosum, s_kadane, s_isprime,
            s_digits, s_freq, s_matmul, s_transpose, s_brackets, s_lis, s_knapsack, s_lcs, s_coins,
            s_bfs, s_dfs, s_merge, s_factorial]

WORDS = ["apple", "graph", "kharagpur", "matrix", "banana", "string", "letter", "binary"]

# --------------------------------------------------------------------------- programs


def random_style(rng):
    return {
        "indent": rng.choice(["  ", "    ", "\t"]),
        "brace": rng.choice(["same", "next"]),
        "loop": "for", "inc": "post", "cmp": "lt", "compound": True,
        "merge_decls": False, "parens": False, "dead": 0.0,
        "comments": rng.random() < 0.5,
        "bits": rng.random() < 0.5,
    }


def obfuscated_style(rng, base, level):
    st = random_style(rng)  # new formatting
    st["loop"], st["inc"], st["cmp"], st["compound"] = base["loop"], base["inc"], base["cmp"], base["compound"]
    if level >= 2:
        st["loop"] = rng.choice(["for", "while"])
        st["inc"] = rng.choice(["post", "pre", "plus1"])
        st["cmp"] = rng.choice(["lt", "gt"])
        st["compound"] = rng.random() < 0.5
    if level >= 3:
        st["merge_decls"] = rng.random() < 0.5
        st["parens"] = True
        st["dead"] = rng.uniform(0.15, 0.35)
    return st


class OverlapGuard:
    """Rejects a new original if it shares more than `max_shared`
    implementations (snippet, variant) with any earlier original - two
    independent authors should not produce near-identical programs."""

    def __init__(self, max_shared):
        self.max_shared = max_shared
        self.by_item = {}

    def ok(self, items):
        counts = {}
        for it in items:
            for o in self.by_item.get(it, ()):
                counts[o] = counts.get(o, 0) + 1
                if counts[o] > self.max_shared:
                    return False
        return True

    def add(self, oid, items):
        for it in items:
            self.by_item.setdefault(it, []).append(oid)


def make_logical_program(rng, guard=None, oid=0):
    for _ in range(10000):
        k = rng.randint(4, 6)
        chosen = rng.sample(range(len(SNIPPETS)), k)
        variants = [rng.randint(0, 1) for _ in chosen]
        items = [(s, v) for s, v in zip(chosen, variants)]
        if guard is None or guard.ok(items):
            break
    if guard is not None:
        guard.add(oid, items)
    params = [{"v": v, "k": rng.randint(2, 40), "mod": rng.choice([1000000007, 998244353, 1000003]),
               "word": rng.choice(WORDS), "seed": rng.randint(0, 2**31), "extras": rng.choice([0, 1, 1, 2])}
              for v in variants]
    # Original authors also differ in habits.
    habits = {"loop": rng.choice(["for", "for", "while"]), "inc": rng.choice(["post", "pre"]),
              "cmp": "lt", "compound": rng.random() < 0.8}
    return {"snips": chosen, "params": params, "habits": habits, "uid": rng.randint(0, 10**6)}


def render_program(prog, rng, names, style, reorder=False):
    c = Ctx(rng, names, style)
    funcs, calls = [], []
    for idx, (si, P) in enumerate(zip(prog["snips"], prog["params"])):
        c.cur = (P, f"_{idx}")
        f_stmts, call = SNIPPETS[si](c, P, f"_{idx}")
        funcs.append(f_stmts)
        calls.append(call)
    order = list(range(len(funcs)))
    if reorder:
        rng.shuffle(order)
    N, ARR, STR = names("n#main"), names("arr#main"), names("s#main")
    main = [f"int {N};", f"cin >> {N};", f"vector<int> {ARR}({N});"]
    main += c.loop(names("i#main"), "0", N, [f"cin >> {ARR}[{names('i#main')}];"])
    main += [f"string {STR};", f"cin >> {STR};"]
    def subst(stmt):
        if isinstance(stmt, str):
            stmt = re.sub(r"\bARR\b", ARR, stmt)
            stmt = re.sub(r"\bSTR\b", STR, stmt)
            return re.sub(r"\bN\b", N, stmt)
        kind, header, body = stmt
        return (kind, subst(header), [subst(b) for b in body])

    for call in calls:
        main.extend(subst(line) for line in call)
    main.append("return 0;")

    out = []
    if style["comments"]:
        out.append(f"// {rng.choice(['solution', 'assignment', 'lab', 'problem'])} {rng.randint(1, 99)}")
    if style["bits"]:
        out.append("#include <bits/stdc++.h>")
    else:
        out += ["#include <iostream>", "#include <vector>", "#include <string>", "#include <map>",
                "#include <queue>", "#include <stack>", "#include <algorithm>", "#include <climits>"]
    out += ["using namespace std;", ""]
    for i in order:
        if style["comments"] and rng.random() < 0.4:
            out.append(f"// {rng.choice(['helper', 'compute', 'utility', 'step'])} function")
        out += render(funcs[i], style)
        out.append("")
    out += render([("block", "int main()", main)], style)
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--originals", type=int, default=2600)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    os.makedirs(args.out, exist_ok=True)

    labels = []
    files = 0
    guard = OverlapGuard(max_shared=3)
    for o in range(args.originals):
        prog = make_logical_program(rng, guard, o)
        base_style = random_style(rng)
        base_style.update(prog["habits"])
        group = [f"p{o:05d}_orig.cpp"]
        with open(os.path.join(args.out, group[0]), "w") as fh:
            fh.write(render_program(prog, random.Random(rng.random()), Names(random.Random(rng.random()), False), base_style))
        files += 1
        n_copies = rng.choices([0, 1, 2, 3], weights=[0.25, 0.25, 0.25, 0.25])[0]
        for cidx in range(n_copies):
            level = rng.randint(1, 3)
            st = obfuscated_style(rng, base_style, level)
            name = f"p{o:05d}_copy{cidx}_L{level}.cpp"
            with open(os.path.join(args.out, name), "w") as fh:
                fh.write(render_program(prog, random.Random(rng.random()), Names(random.Random(rng.random()), True), st,
                                        reorder=level >= 2))
            group.append(name)
            files += 1
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                labels.append((group[i], group[j]))

    with open(os.path.join(args.out, "labels.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["a", "b"])
        w.writerows(labels)
    print(f"wrote {files} files, {len(labels)} positive pairs to {args.out}")


if __name__ == "__main__":
    main()
