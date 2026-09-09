#!/usr/bin/env python3
"""Our multithreaded ratios next to the ones read off Figure 13 of the BOSS paper, for the five
choke point queries. Theirs come off a chart so they're approximate, and their machine had 20
cores to our 10, which gives their multithreaded baselines more headroom.

Usage:  python3 scripts/paper_ratios.py [--csv]
"""
import csv
import os
import statistics
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")

# read off Figure 13 of the BOSS paper at SF-1: query -> (vs DuckDB MT, vs MonetDB MT)
PAPER = {"Q1": (13, 13), "Q3": (40, 60), "Q6": (25, 25), "Q9": (7, 50), "Q18": (4, 20)}


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
    """Ours beside theirs, for the five queries the paper charts."""
    seconds = measured()
    # multithreaded on both sides, since that is what the paper's figure plots
    rows = []
    for query, (duck, monet) in PAPER.items():
        boss = seconds[query]["BOSS"]
        rows.append([query, f"~{duck}", f"{boss / seconds[query]['DuckDB-MT']:.1f}",
                     f"~{monet}", f"{boss / seconds[query]['MonetDB-MT']:.1f}"])

    header = ["query", "theirs vs DuckDB", "ours", "theirs vs MonetDB", "ours"]
    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(header)
        out.writerows(rows)
        return

    print(f"{header[0]:6}" + "".join(f"{h:>20}" for h in header[1:]))
    for row in rows:
        print(f"{row[0]:6}" + "".join(f"{cell:>20}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
