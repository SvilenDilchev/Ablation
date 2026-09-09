#!/usr/bin/env python3
"""What it costs ViewEngine to store a view and splice it back, from exp2. Every condition runs
the identical ACE plan, so any difference from plain is the storage and substitution alone.

Usage:  python3 scripts/splicing_overhead.py [--csv]
"""
import csv
import os
import statistics
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
CONDITIONS = ["plain", "base", "aggregate", "max", "whole", "nested"]


def per_query_seconds():
    """condition -> query -> mean seconds, pooling gbench's repetitions of each case."""
    pooled = {}
    for row in csv.DictReader(open(os.path.join(RESULTS, "exp2", "results.csv"))):
        if row["condition"] in CONDITIONS and row["ok"] == "true":
            entry = pooled.setdefault(row["condition"], {}).setdefault(row["query"], [0.0, 0])
            # weight by iterations, since gbench picks a different count per repetition
            entry[0] += float(row["seconds"]) * int(row["iterations"])
            entry[1] += int(row["iterations"])
    return {condition: {query: total / count for query, (total, count) in by_query.items()}
            for condition, by_query in pooled.items()}


def main(argv):
    """One row per condition: what it cost in total, and how far that is from plain."""
    seconds = per_query_seconds()
    plain = seconds["plain"]
    plain_total = sum(plain.values())
    plain_median = statistics.median(plain.values())

    rows = []
    for condition in CONDITIONS:
        by_query = seconds[condition]
        total = sum(by_query.values())
        median = statistics.median(by_query.values())

        if condition == "plain":
            rows.append([condition, f"{total:.4f}", "1.0000", f"{median:.4f}", "-", "-"])
            continue

        # the per-query difference, taken query by query so a slow query can't dominate
        # median of the per-query differences, not the difference of the medians. A slow query
        # would otherwise swamp the 21 fast ones.
        deltas = [by_query[q] - plain[q] for q in by_query if q in plain]
        median_delta = statistics.median(deltas)
        rows.append([condition, f"{total:.4f}", f"{total / plain_total:.4f}", f"{median:.4f}",
                     f"{median_delta * 1000:+.3f} ms", f"{100 * median_delta / plain_median:.3f} %"])

    header = ["condition", "total (s)", "vs plain", "median query (s)", "median d/query",
              "d / median query"]
    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(header)
        out.writerows(rows)
        return

    print(f"{header[0]:10}" + "".join(f"{h:>19}" for h in header[1:]))
    for row in rows:
        print(f"{row[0]:10}" + "".join(f"{cell:>19}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
