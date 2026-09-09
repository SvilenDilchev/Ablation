#!/usr/bin/env python3
"""Overall stream speedup against the number of streams: what exp3 measured, what the cost model
predicts, and the ceiling the model approaches as N grows and the build cost amortises away.

Usage:  python3 scripts/speedup_curve.py [--csv]
"""
import csv
import importlib.util
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(os.path.dirname(HERE), "results")
CEILING_N = 1 << 20  # big enough that the build rate is nil and only reuse and scan are left


def load_model():
    """best_views.py, loaded by path so this script still runs from anywhere"""
    spec = importlib.util.spec_from_file_location("best_views", os.path.join(HERE, "best_views.py"))
    model = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    return model


def measured():
    """N -> (plain seconds, view seconds), with the view definition cost charged to the views."""
    plain, view = {}, {}
    for row in csv.DictReader(open(os.path.join(RESULTS, "exp3", "results.csv"))):
        n = int(row["n_streams"])
        # the setup rows go on the view side, so defining the views is charged to them
        if row["condition"] == "mech_setup":
            view[n] = view.get(n, 0.0) + float(row["seconds"])
        elif row["ok"] == "true" and row["condition"] == "plain":
            plain[n] = plain.get(n, 0.0) + float(row["seconds"])
        elif row["ok"] == "true" and row["condition"] == "mech":
            view[n] = view.get(n, 0.0) + float(row["seconds"])
    return {n: (plain[n], view[n]) for n in sorted(plain)}


def measured_ceiling(totals):
    """Where the measured curve is heading, from the marginal cost of one more stream between
    the two largest N. By then the views are all built, so what's left per stream is the reuse
    and scan cost, and the ceiling is the plain marginal cost over that."""
    last, previous = sorted(totals)[-2:][::-1]
    plain_step = totals[last][0] - totals[previous][0]
    view_step = totals[last][1] - totals[previous][1]
    return plain_step / view_step


def predicted(model, consts, plain, streams):
    """The model's overall speedup: every query's plain cost over every query's best design."""
    # summed across queries rather than averaging their individual speedups, so a fast query
    # with a huge ratio does not drag the overall figure up
    total_plain = total_design = 0.0
    for query in model.QUERIES:
        cost = plain.get("Q13R", plain.get(query)) if query == "Q13" else plain.get(query)
        if not cost or math.isnan(cost):
            continue
        best = model.sweep(query, cost, consts, model.candidates(query), streams,
                           model.overlap(query))[0]
        total_plain += cost
        total_design += best[3]
    return total_plain / total_design


def deployed_ceiling(model, consts, plain, chosen_at, ceiling_at):
    """The model's ceiling for the designs it actually picks at chosen_at, priced at ceiling_at.

    Re-running the sweep at a huge N lets it pick bigger designs than the ones that were
    deployed, because the V <= N feasibility cap stops binding. That ceiling belongs to a view
    set nobody ran. So the design is fixed at the stream count the experiment reached, and only
    the stream count is taken to the limit."""
    total_plain = total_design = 0.0
    for query in model.QUERIES:
        cost = plain.get("Q13R", plain.get(query)) if query == "Q13" else plain.get(query)
        if not cost or math.isnan(cost):
            continue
        label, cut, views, _ = model.sweep(query, cost, consts, model.candidates(query),
                                           chosen_at, model.overlap(query))[0]
        at_limit = model.sweep(query, cost, consts, model.candidates(query), ceiling_at,
                               model.overlap(query))
        pinned = next(d for d in at_limit if (d[0], d[1], d[2]) == (label, cut, views))
        total_plain += cost
        total_design += pinned[3]
    return total_plain / total_design


def main(argv):
    """Measured against predicted at every stream count exp3 ran, and where it tops out."""
    model = load_model()
    consts = model.sweep_consts(model.exp0_constants())
    plain = model.exp1_plain()

    totals = measured()
    rows = [[n, f"{totals[n][0] / totals[n][1]:.2f}x", f"{predicted(model, consts, plain, n):.2f}x"]
            for n in totals]

    # two ceilings: what the model tends to, and what the measurements are tending to
    model_ceiling = deployed_ceiling(model, consts, plain, max(totals), CEILING_N)
    true_ceiling = measured_ceiling(totals)

    if "--csv" in argv:
        out = csv.writer(sys.stdout)
        out.writerow(["n_streams", "measured", "model"])
        out.writerows(rows)
        out.writerow(["ceiling", f"{true_ceiling:.2f}x", f"{model_ceiling:.2f}x"])
        return

    print(f"{'N':>6}{'measured':>12}{'model':>10}")
    for n, run, model_value in rows:
        print(f"{n:>6}{run:>12}{model_value:>10}")
    print(f"{'ceiling':>6}{true_ceiling:>11.2f}x{model_ceiling:>9.2f}x")


if __name__ == "__main__":
    main(sys.argv[1:])
