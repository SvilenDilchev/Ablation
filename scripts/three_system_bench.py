#!/usr/bin/env python3
"""Mean seconds per query on each system, from exp1. ALL sums the 22 base queries; Q13R is
BOSS's rewrite of Q13 and isn't one of them.

Usage:  python3 scripts/three_system_bench.py [--csv]
"""
import csv
import os
import statistics
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
SYSTEMS = ["BOSS", "DuckDB-1T", "DuckDB-MT", "MonetDB-1T", "MonetDB-MT"]


def measured():
    """query -> system -> median seconds"""
    samples = {}
    for row in csv.DictReader(open(os.path.join(RESULTS, "exp1", "results.csv"))):
        if row["ok"] == "true":
            samples.setdefault(row["query"], {}).setdefault(row["system"], []).append(
                float(row["seconds"]))
    return {query: {system: statistics.median(runs) for system, runs in per_system.items()}
            for query, per_system in samples.items()}


def queries(seconds):
    """Q1..Q22 in order, then Q13R on the end where the thesis puts it."""
    numbered = sorted((q for q in seconds if q != "Q13R"), key=lambda q: int(q[1:]))
    return numbered + (["Q13R"] if "Q13R" in seconds else [])


def main(argv):
    """One row per query, one column per system, plus the 22-query total."""
    seconds = measured()
    order = queries(seconds)
    # Q13R is left out of the total: it is a 23rd query BOSS runs, not one of the spec's 22
    totals = {s: sum(seconds[q].get(s, 0.0) for q in order if q != "Q13R") for s in SYSTEMS}

    rows = [[q] + [f"{seconds[q][s]:.4f}" if s in seconds[q] else "-" for s in SYSTEMS]
            for q in order]
    rows.append(["ALL"] + [f"{totals[s]:.4f}" for s in SYSTEMS])

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(["query"] + SYSTEMS)
        out.writerows(rows)
        return

    print(f"{'query':6}" + "".join(f"{s:>12}" for s in SYSTEMS))
    for row in rows:
        print(f"{row[0]:6}" + "".join(f"{cell:>12}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
