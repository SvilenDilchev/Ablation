#!/usr/bin/env python3
"""BOSS's runtime divided by each other system's, from exp1. Below 1 means BOSS was faster.
Q13R is left out because only BOSS runs it, so there's nothing to divide by.

Usage:  python3 scripts/three_system_ratios.py [--csv]
"""
import csv
import os
import statistics
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
OTHERS = ["DuckDB-1T", "DuckDB-MT", "MonetDB-1T", "MonetDB-MT"]


def measured():
    """query -> system -> median seconds, from exp1"""
    samples = {}
    for row in csv.DictReader(open(os.path.join(RESULTS, "exp1", "results.csv"))):
        if row["ok"] == "true":
            samples.setdefault(row["query"], {}).setdefault(row["system"], []).append(
                float(row["seconds"]))
    return {query: {system: statistics.median(runs) for system, runs in per_system.items()}
            for query, per_system in samples.items()}


def main(argv):
    """One row per query, BOSS divided by each of the other four."""
    seconds = measured()
    # Q13R goes entirely: only BOSS runs it, so there is nothing to divide by
    order = sorted((q for q in seconds if q != "Q13R"), key=lambda q: int(q[1:]))

    rows = []
    for query in order:
        boss = seconds[query]["BOSS"]
        rows.append([query] + [f"{boss / seconds[query][s]:.2f}" for s in OTHERS])

    totals = {s: sum(seconds[q][s] for q in order) for s in ["BOSS"] + OTHERS}
    rows.append(["ALL"] + [f"{totals['BOSS'] / totals[s]:.2f}" for s in OTHERS])

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(["query"] + [f"BOSS/{s}" for s in OTHERS])
        out.writerows(rows)
        return

    # 17 wide because "BOSS/MonetDB-1T" is 15 characters and the columns ran into each other
    print(f"{'query':6}" + "".join(f"{'BOSS/' + s:>17}" for s in OTHERS))
    for row in rows:
        print(f"{row[0]:6}" + "".join(f"{cell:>17}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
