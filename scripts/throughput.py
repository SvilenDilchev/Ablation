#!/usr/bin/env python3
"""Per-query speedup of the view-enabled system over plain, at each stream count, from exp3.
Each cell is that query's total plain time over its total view-enabled time across all N
instances. The ALL row charges the view definition cost; the per-query rows don't.

Usage:  python3 scripts/throughput.py [--csv]
"""
import csv
import os
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")


def totals(experiment, plain_condition="plain", view_condition="mech"):
    """(N -> query -> {plain, view}, N -> setup seconds)"""
    by_n, setup = {}, {}
    for row in csv.DictReader(open(os.path.join(RESULTS, experiment, "results.csv"))):
        n = int(row["n_streams"])
        if row["condition"] == f"{view_condition}_setup":
            setup[n] = setup.get(n, 0.0) + float(row["seconds"])
            continue
        if row["ok"] != "true" or row["condition"] not in (plain_condition, view_condition):
            continue
        side = "plain" if row["condition"] == plain_condition else "view"
        by_n.setdefault(n, {}).setdefault(row["query"], {"plain": 0.0, "view": 0.0})
        by_n[n][row["query"]][side] += float(row["seconds"])
    return by_n, setup


def main(argv):
    """One row per query, one column per stream count, and the whole workload underneath."""
    by_n, setup = totals("exp3")
    counts = sorted(by_n)
    queries = sorted(by_n[counts[-1]], key=lambda q: int(q[1:].rstrip("R")))

    rows = []
    for query in queries:
        cells = []
        for n in counts:
            pair = by_n[n].get(query)
            cells.append(f"{pair['plain'] / pair['view']:.2f}x" if pair and pair["view"] else "-")
        rows.append([query] + cells)

    # the whole workload, and this is where the view definition cost gets charged. The
    # per-query rows above leave it out, so ALL is always the more pessimistic figure.
    plain_totals, view_totals, overall = [], [], []
    for n in counts:
        plain = sum(v["plain"] for v in by_n[n].values())
        view = sum(v["view"] for v in by_n[n].values()) + setup.get(n, 0.0)
        plain_totals.append(f"{plain:.1f}")
        view_totals.append(f"{view:.1f}")
        overall.append(f"{plain / view:.2f}x")
    rows.append(["plain s"] + plain_totals)
    rows.append(["views s"] + view_totals)
    rows.append(["ALL"] + overall)

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(["query"] + [f"N={n}" for n in counts])
        out.writerows(rows)
        return

    print(f"{'query':6}" + "".join(f"{'N=' + str(n):>10}" for n in counts))
    for row in rows:
        print(f"{row[0]:6}" + "".join(f"{cell:>10}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
