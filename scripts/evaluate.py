#!/usr/bin/env python3
"""Precision / recall / F1 of the engine against gen_corpus.py ground truth.

Runs `plag` once per configuration with a low reporting threshold, then sweeps
the decision threshold offline. Pairs not reported (Jaccard < floor) count as
predicted-negative.

Usage: evaluate.py CORPUS_DIR [--plag build/plag] [--configs ast:12:8:0.25,lex:12:8,raw:12:8]
(config = tokenizer:k:w[:max_df_frac])
"""
import argparse
import csv
import os
import re
import subprocess
import sys
from collections import defaultdict


def run(plag, corpus, mode, k, w, floor, out_csv, extra):
    cmd = [plag, "--tokenizer", mode, "--k", str(k), "--w", str(w), "--threshold", str(floor),
           "--top", "0", "--csv", out_csv] + extra + [corpus]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return proc.stdout


def level_of(name):
    m = re.search(r"_L(\d)\.cpp$", name)
    return int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--plag", default=os.path.join(os.path.dirname(__file__), "..", "build", "plag"))
    ap.add_argument("--configs", default="ast:12:8,lex:12:8,raw:12:8")
    ap.add_argument("--floor", type=float, default=0.15)
    ap.add_argument("--out", default="results")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    positives = set()
    with open(os.path.join(args.corpus, "labels.csv")) as fh:
        for row in csv.DictReader(fh):
            positives.add(tuple(sorted((row["a"], row["b"]))))
    pos_by_level = defaultdict(int)
    for a, b in positives:
        pos_by_level[max(level_of(a), level_of(b))] += 1

    summary = []
    for cfg in args.configs.split(","):
        parts = cfg.split(":")
        mode, k, w = parts[:3]
        df = parts[3] if len(parts) > 3 else "0.25"
        out_csv = os.path.join(args.out, f"pairs_{mode}_k{k}_w{w}_df{df}.csv")
        stdout = run(args.plag, args.corpus, mode, k, w, args.floor, out_csv, ["--max-df-frac", df])
        timing = next((l for l in stdout.splitlines() if l.startswith("time:")), "")
        files = next((l for l in stdout.splitlines() if l.startswith("files")), "")

        scores = {}
        with open(out_csv) as fh:
            for row in csv.DictReader(fh):
                key = tuple(sorted((os.path.basename(row["a"]), os.path.basename(row["b"]))))
                scores[key] = float(row["jaccard"])

        # Sort once, then sweep thresholds with running TP/FP counts: O(P log P).
        ranked = sorted(((sc, p in positives) for p, sc in scores.items()), reverse=True)
        best, curve = None, []
        tp = fp = idx = 0
        for t in [x / 100 for x in range(99, 4, -1)]:
            while idx < len(ranked) and ranked[idx][0] >= t:
                tp += ranked[idx][1]
                fp += not ranked[idx][1]
                idx += 1
            fn = len(positives) - tp
            prec = tp / (tp + fp) if tp + fp else 1.0
            rec = tp / (tp + fn) if tp + fn else 0.0
            f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
            curve.append((t, prec, rec, f1, fp))
            if best is None or f1 > best[3]:
                best = (t, prec, rec, f1, fp)
        curve.reverse()

        t = best[0]
        rec_level = {}
        for lvl in sorted(pos_by_level):
            hits = sum(1 for p in positives if max(level_of(p[0]), level_of(p[1])) == lvl and scores.get(p, 0) >= t)
            rec_level[lvl] = hits / pos_by_level[lvl]
        # Hardest negatives: highest-scoring non-plagiarised pairs.
        neg_scores = sorted((s for p, s in scores.items() if p not in positives), reverse=True)

        print(f"== {cfg}   {files}")
        print(f"   {timing}")
        print(f"   best F1 {best[3]:.4f} at threshold {t:.2f}: precision {best[1]:.4f}, recall {best[2]:.4f}, "
              f"false positives {best[4]}")
        print("   recall by obfuscation level: " + ", ".join(f"L{l} {r:.3f}" for l, r in rec_level.items()))
        print(f"   top negative Jaccard: {neg_scores[:3] if neg_scores else []}")
        summary.append((cfg, best, rec_level))
        with open(os.path.join(args.out, f"pr_{mode}_k{k}_w{w}_df{df}.csv"), "w") as fh:
            fh.write("threshold,precision,recall,f1,false_positives\n")
            for row in curve:
                fh.write(",".join(f"{x:.4f}" if isinstance(x, float) else str(x) for x in row) + "\n")

    print("\nconfig,best_threshold,precision,recall,f1," + ",".join(f"recall_L{l}" for l in sorted(pos_by_level)))
    for cfg, best, rl in summary:
        print(f"{cfg},{best[0]:.2f},{best[1]:.4f},{best[2]:.4f},{best[3]:.4f}," + ",".join(f"{rl[l]:.4f}" for l in sorted(rl)))


if __name__ == "__main__":
    sys.exit(main())
