#!/usr/bin/env python3
"""How repeatable exp2 is, and whether splicing ever changed an answer. A case is one query
under one condition, measured five times, so the spread across those five is what the table's
sub-percent differences have to be read against.

The define_* conditions are the one-off cost of registering each view, not a measurement, so
they are left out. That leaves 22 queries x 6 conditions.

Usage:  python3 scripts/splicing_variance.py [--csv]
"""
import csv
import os
import statistics
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
CONDITIONS = ["plain", "base", "aggregate", "max", "whole", "nested"]


def cases():
    """(query, condition) -> its repetitions, one entry per gbench repetition."""
    out = {}
    for row in csv.DictReader(open(os.path.join(RESULTS, "exp2", "results.csv"))):
        if row["condition"] in CONDITIONS and row["ok"] == "true":
            out.setdefault((row["query"], row["condition"]), []).append(row)
    return out


def spread(repetitions):
    """Coefficient of variation across a case's repetitions, as a percentage. Sample standard
    deviation, since five repetitions is a sample rather than the whole population."""
    seconds = [float(r["seconds"]) for r in repetitions]
    return statistics.stdev(seconds) / statistics.mean(seconds) * 100


def mismatches(by_case):
    """Cases whose rows or checksum differ from the same query run plain. Splicing is only
    meant to change where the work happens, so anything here is a correctness bug."""
    plain = {query: (reps[0]["rows"], reps[0]["checksum"])
             for (query, condition), reps in by_case.items() if condition == "plain"}

    bad = []
    for (query, condition), reps in sorted(by_case.items()):
        for rep in reps:
            if (rep["rows"], rep["checksum"]) != plain[query]:
                bad.append((query, condition, rep["rows"], rep["checksum"]))
                break
    return bad


def main(argv):
    by_case = cases()
    spreads = sorted(spread(reps) for reps in by_case.values())
    bad = mismatches(by_case)

    # the plain cases are compared against themselves, so they can't fail; the other 110 can
    spliced = sum(1 for query, condition in by_case if condition != "plain")

    rows = [["cases", str(len(by_case))],
            ["repetitions each", str(len(next(iter(by_case.values()))))],
            ["median spread", f"{statistics.median(spreads):.3f}%"],
            ["90th percentile spread", f"{statistics.quantiles(spreads, n=10)[8]:.3f}%"],
            ["worst spread", f"{max(spreads):.3f}%"],
            ["result mismatches", f"{len(bad)} of {len(by_case)} ({spliced} spliced)"]]

    if "--csv" in argv:
        writer = csv.writer(sys.stdout)
        writer.writerow(["measure", "value"])
        writer.writerows(rows)
    else:
        for label, value in rows:
            print(f"{label:<24}{value:>22}")
        for query, condition, count, checksum in bad:
            print(f"  MISMATCH {query} {condition}: rows={count} checksum={checksum}")


if __name__ == "__main__":
    main(sys.argv[1:])
