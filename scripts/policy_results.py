#!/usr/bin/env python3
"""Overall speedup under exp4's 1.622 GB budget, one row per eviction policy, with exp3's
unconstrained run on top for reference. Evictions and rejected admissions come from the
ViewEngine counters the log parser picked up.

Usage:  python3 scripts/policy_results.py [--csv]
"""
import csv
import os
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
POLICIES = ["Benefit", "LRU", "Random"]


def read(experiment):
    """That experiment's rows, and the stream counts it ran"""
    rows = list(csv.DictReader(open(os.path.join(RESULTS, experiment, "results.csv"))))
    return rows, sorted({int(r["n_streams"]) for r in rows})


def speedup(rows, n, view_condition):
    """Plain time over view time at this stream count, charging the view definition cost.

    The plain pass is shared by every policy at that count, so the three are compared against
    the same baseline rather than against three separately warmed ones.
    """
    plain = sum(float(r["seconds"]) for r in rows
                if int(r["n_streams"]) == n and r["condition"] == "plain" and r["ok"] == "true")
    view = sum(float(r["seconds"]) for r in rows
               if int(r["n_streams"]) == n and r["condition"] == view_condition
               and r["ok"] == "true")
    setup = sum(float(r["seconds"]) for r in rows
                if int(r["n_streams"]) == n and r["condition"] == f"{view_condition}_setup")
    return plain / (view + setup) if view else 0.0


def counters(rows, n, view_condition):
    """(evictions, rejected admissions) this policy racked up"""
    evicts = sum(int(r["ve_evicts"]) for r in rows
                 if int(r["n_streams"]) == n and r["condition"] == view_condition)
    # a drop is an admission the policy refused, so the view was built and then thrown away
    drops = sum(int(r["ve_drops"]) for r in rows
                if int(r["n_streams"]) == n and r["condition"] == view_condition)
    return evicts, drops


def main(argv):
    """A row per policy, then a row per policy's eviction counts."""
    exp3, counts = read("exp3")
    exp4, _ = read("exp4")

    rows = [["unconstrained"] + [f"{speedup(exp3, n, 'mech'):.2f}x" for n in counts]]
    for policy in POLICIES:
        rows.append([policy] + [f"{speedup(exp4, n, 'mech_' + policy):.2f}x" for n in counts])
    for policy in POLICIES:
        cells = []
        for n in counts:
            evicts, drops = counters(exp4, n, "mech_" + policy)
            cells.append(f"{evicts} ({drops})")
        rows.append([f"{policy} evictions"] + cells)

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow([""] + [f"N={n}" for n in counts])
        out.writerows(rows)
        return

    print(f"{'':18}" + "".join(f"{'N=' + str(n):>12}" for n in counts))
    for row in rows:
        print(f"{row[0]:18}" + "".join(f"{cell:>12}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
