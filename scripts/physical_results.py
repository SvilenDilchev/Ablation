#!/usr/bin/env python3
"""Overall speedup under exp5's 5800 MiB physical cap, next to exp3's unconstrained run and
exp4's Benefit policy at the equivalent logical scarcity. Peak swap is this process's own, from
exp5's sysinfo samples, and it's what confirms the cap actually displaced anything.

Usage:  python3 scripts/physical_results.py [--csv]
"""
import csv
import os
import sys

RESULTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")


def read(experiment):
    """That experiment's results rows"""
    return list(csv.DictReader(open(os.path.join(RESULTS, experiment, "results.csv"))))


def speedup(rows, n, view_condition):
    """Plain time over view time at this stream count, charging the view definition cost"""
    plain = sum(float(r["seconds"]) for r in rows
                if int(r["n_streams"]) == n and r["condition"] == "plain" and r["ok"] == "true")
    view = sum(float(r["seconds"]) for r in rows
               if int(r["n_streams"]) == n and r["condition"] == view_condition
               and r["ok"] == "true")
    setup = sum(float(r["seconds"]) for r in rows
                if int(r["n_streams"]) == n and r["condition"] == f"{view_condition}_setup")
    return plain / (view + setup) if view else 0.0


def peak_swap(experiment):
    """N -> the highest process swap seen while that stream count was running"""
    peak = {}
    for row in csv.DictReader(open(os.path.join(RESULTS, experiment, "sysinfo.csv"))):
        if not row["phase"].startswith("N="):
            continue
        # the phase label is like "N=64 mech", so the count is the first field
        n = int(row["phase"].split()[0][2:])
        # swap_mb is this process's own. swap_used_mb is machine wide and would fold in
        # whatever else was running, which the cap had nothing to do with.
        peak[n] = max(peak.get(n, 0.0), float(row["swap_mb"]))
    return peak


def main(argv):
    """The three runs side by side, with the swap that proves the cap bit."""
    exp3, exp4, exp5 = read("exp3"), read("exp4"), read("exp5")
    counts = sorted({int(r["n_streams"]) for r in exp5})
    swap = peak_swap("exp5")

    rows = [
        ["unconstrained"] + [f"{speedup(exp3, n, 'mech'):.2f}x" for n in counts],
        ["Benefit eviction"] + [f"{speedup(exp4, n, 'mech_Benefit'):.2f}x" for n in counts],
        ["physically capped"] + [f"{speedup(exp5, n, 'mech'):.2f}x" for n in counts],
        ["peak swap (MiB)"] + [f"{swap.get(n, 0):.0f}" for n in counts],
    ]

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow([""] + [f"N={n}" for n in counts])
        out.writerows(rows)
        return

    print(f"{'':20}" + "".join(f"{'N=' + str(n):>12}" for n in counts))
    for row in rows:
        print(f"{row[0]:20}" + "".join(f"{cell:>12}" for cell in row[1:]))


if __name__ == "__main__":
    main(sys.argv[1:])
