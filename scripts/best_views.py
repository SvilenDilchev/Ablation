#!/usr/bin/env python3
"""The best set of views for each TPC-H query, worked out from measured engine constants.

Usage:  python3 scripts/best_views.py [N] [Q3 Q6 ...] [--top K] [--csv]
        N        stream instances per query, 256 by default
        Qx       just these queries, all 22 by default
        --top K  show K runners-up instead of the default 3
        --csv    comma separated instead of an aligned table
"""
import csv
import math
import os
import statistics
import sys


# Costs in nanoseconds, filled in by sweep_consts() from what exp0 measured. Left empty here so
# a missing measurement raises instead of pricing against a stale value. The keys still have to
# exist, since pipeline_costs() looks ops up by name.
OP_NS = {"filter": None, "groupby": None, "join": None, "antijoin": None,
         "project": None, "orderby": None}
GROUPBY_OUT_NS = None   # per output group
GROUPBY_OUT2_NS = None  # per group, two-column key
GROUPBY_STR_NS = None   # per group, string key


def make_stage(label, op, rows_in, rows_out, cut=False, reagg=False, cols=0, strings=0,
               dmul=1, widen=True, grows=None, gkind="int", unsafe_side=False,
               span=None, window=None):
    """One stage of a query's pipeline, with everything the cost model needs to price it.

    rows_in/out  at k = 1, meaning one value of every parameter, from spec selectivities
    cut          this stage's head is matchable by the rewriter (Filter/Join/AntiJoin/Project)
    reagg        the stage aggregates, so a view covering k > 1 parameter values can't cross it
    dmul         parameter cardinality first applied here. None means unbounded.
    widen        the parameter applied here is a range the rewriter can cover with a residual
    grows        False if output rows don't grow when a view covers k values.
                 Defaults to False for aggregates, True for everything else.
    unsafe_side  a Left/Anti join carrying predicates on its destructive side. The rewriter
                 can't represent that in a signature at all, so every cut at or above it is
                 uncacheable. The stage still counts towards the query's total cost, it just
                 can't become a view.
    span/window  for a bounded-continuous parameter (dmul=None), the domain width and the
                 instance window width in the same units. This is what lets a design be
                 banded and serve only a fraction of instances.
    """
    return dict(label=label, op=op, rin=rows_in, rout=rows_out, cut=cut, reagg=reagg,
                cols=cols, strings=strings, dmul=dmul, widen=widen, gkind=gkind,
                unsafe_side=unsafe_side, grows=(not reagg) if grows is None else grows,
                span=span, window=window)


# Every query as a pipeline of stages along its cacheable branch.
#
# uses={m: u} means the query evaluates stages [0..m] u times over, as Q15's revenue aggregate
# and Q11's base join do. A view cut at or below m is built once and serves all u references;
# one cut above m pays for the extra passes itself.
#
# manual=[...] are mixed or widened granularities the scalar sweep can't index, given as
# (cut_index, V, rows_of_the_view, k_for_cost_scaling, label).
#
# A stage costs: op constant (exp0) * (base-side input rows + carried rows * carried scale),
# scaled by gamma so the whole pipeline matches the plain cost exp1 measured.
QUERIES = {
 "Q1": dict(stages=[
    make_stage("filter shipdate", "filter", 6_001_215, 5_860_000, cut=True, cols=7, strings=2, dmul=61, grows=False),
    make_stage("derive prices", "project", 5_860_000, 5_860_000, cut=True, cols=10, strings=2, grows=False),
    make_stage("aggregate", "groupby", 5_860_000, 4, reagg=True, cols=10, strings=2),
    make_stage("rename", "project", 4, 4, cut=True, cols=10, strings=2, grows=False),
    make_stage("order", "orderby", 4, 4)]),

 "Q2": dict(stages=[
    make_stage("nation x region", "join", 30, 5, cut=True, cols=2, strings=1, dmul=5, widen=False),
    make_stage("x supplier", "join", 10_005, 2_000, cut=True, cols=7, strings=5),
    make_stage("x partsupp", "join", 802_000, 160_000, cut=True, cols=10, strings=5),
    make_stage("min cost", "groupby", 160_000, 40_000, reagg=True, cols=2),
    make_stage("part filter + joins", "join", 360_000, 800, cut=True, cols=10, strings=5,
               dmul=50 * 150),  # SIZE x TYPE
    make_stage("order", "orderby", 800, 100)]),

 "Q3": dict(stages=[
    make_stage("cust-seg x orders-date", "join", 1_650_000, 146_000, cut=True, cols=5, dmul=155, widen=False),
    make_stage("x lineitem-date", "join", 6_147_000, 30_000, cut=True, cols=8),
    make_stage("aggregate", "groupby", 30_000, 11_000, reagg=True, cols=4),
    make_stage("order + top", "orderby", 11_000, 10)],
    manual=[(1, 5, 524_000, 17.5, "5 per-segment views, dates widened to [1994,1996)")]),

 "Q4": dict(stages=[
    make_stage("late lineitems", "filter", 6_001_215, 3_800_000, cut=True, cols=1),
    make_stage("per-order dedup", "groupby", 3_800_000, 940_000, cut=True, reagg=True, cols=1),
    make_stage("x orders-date", "join", 997_000, 21_000, cut=True, cols=2, strings=1, dmul=58),
    make_stage("aggregate", "groupby", 21_000, 5, reagg=True, cols=2, strings=1),
    make_stage("order", "orderby", 5, 5)]),

 "Q5": dict(stages=[
    make_stage("supp x nation-region", "join", 10_030, 2_000, cut=True, cols=3, strings=1, dmul=5, widen=False),
    make_stage("x lineitem", "join", 6_003_215, 1_200_000, cut=True, cols=5, strings=1),
    make_stage("x orders-date", "join", 2_700_000, 182_000, cut=True, cols=5, strings=1, dmul=5),
    make_stage("x customer", "join", 332_000, 7_000, cut=True, cols=5, strings=1),
    make_stage("aggregate", "groupby", 7_000, 5, reagg=True),
    make_stage("order", "orderby", 5, 5)]),

 "Q6": dict(stages=[
    make_stage("filter", "filter", 6_001_215, 90_000, cut=True, cols=4, dmul=80),
    make_stage("aggregate", "groupby", 90_000, 1, reagg=True)],
    manual=[(0, 5, 400_000, 4.4, "5 per-year views, quantity/discount widened")]),

 "Q7": dict(stages=[
    make_stage("lineitem-date x supp", "join", 1_810_000, 1_800_000, cut=True, cols=5),
    make_stage("orders x customer", "join", 1_650_000, 1_500_000, cut=True, cols=2),
    make_stage("chain join", "join", 3_300_000, 1_800_000, cut=True, cols=7),
    make_stage("x nation-pair", "join", 1_800_002, 144_000, cut=True, cols=7, strings=1, dmul=600, widen=False),
    make_stage("x nation-pair", "join", 144_002, 12_000, cut=True, cols=7, strings=2),
    make_stage("aggregate", "groupby", 12_000, 6, reagg=True),
    make_stage("order", "orderby", 6, 6)]),

 "Q8": dict(stages=[
    make_stage("cust x nation x region", "join", 150_030, 30_000, cut=True, cols=1, dmul=5, widen=False),
    make_stage("x orders-date", "join", 480_000, 90_000, cut=True, cols=3),
    make_stage("x lineitem", "join", 6_091_000, 360_000, cut=True, cols=7),
    make_stage("x part-type", "join", 360_400, 2_600, cut=True, cols=7, dmul=150, widen=False),
    make_stage("x supp x nation", "join", 12_625, 2_600, cut=True, cols=8, strings=1),
    make_stage("aggregate", "groupby", 2_600, 2, reagg=True),
    make_stage("share", "project", 2, 2, cut=True, cols=2, dmul=25, widen=False)]),

 "Q9": dict(stages=[
    make_stage("part price window", "filter", 200_000, 12_000, cut=True, cols=2, dmul=None,
      span=1197.99, window=74.05),
    make_stage("x nation x supp x partsupp", "join", 822_000, 48_000, cut=True, cols=5, strings=1),
    make_stage("x lineitem", "join", 6_049_000, 360_000, cut=True, cols=8, strings=1),
    make_stage("x orders", "join", 1_860_000, 360_000, cut=True, cols=8, strings=1),
    make_stage("aggregate", "groupby", 360_000, 175, reagg=True),
    make_stage("order", "orderby", 175, 175)]),

 "Q10": dict(stages=[
    make_stage("orders-date x customer", "join", 207_000, 57_000, cut=True, cols=9, strings=5, dmul=24),
    make_stage("x lineitem-R", "join", 1_557_000, 57_000, cut=True, cols=11, strings=5),
    make_stage("x nation", "join", 57_025, 57_000, cut=True, cols=12, strings=5),
    make_stage("aggregate", "groupby", 57_000, 38_000, reagg=True, cols=8, strings=5, gkind="str"),
    make_stage("order + top", "orderby", 38_000, 20)]),

 "Q11": dict(uses={0: 2}, stages=[
    make_stage("partsupp x supp-nation", "join", 800_400, 32_000, cut=True, cols=2, dmul=25, widen=False),
    make_stage("aggregate + threshold", "groupby", 64_000, 29_000, reagg=True, cols=2, dmul=None),
    make_stage("order", "orderby", 29_000, 29_000)]),

 "Q12": dict(stages=[
    make_stage("lineitem dates", "filter", 6_001_215, 210_000, cut=True, cols=16, strings=7, dmul=5),
    make_stage("x orders", "join", 1_710_000, 210_000, cut=True, cols=25, strings=9),
    make_stage("shipmode filter", "filter", 210_000, 60_000, cut=True, cols=25, strings=9, dmul=21, widen=False),
    make_stage("aggregate", "groupby", 60_000, 2, reagg=True),
    make_stage("order", "orderby", 2, 2)]),

 "Q13": dict(stages=[
    make_stage("customer leftjoin orders", "join", 1_650_000, 1_550_000, cut=True, cols=17, strings=9),
    make_stage("comment pattern", "project", 1_550_000, 1_550_000, cut=True, cols=2, dmul=16, widen=False, grows=False),
    make_stage("per-customer count", "groupby", 1_550_000, 150_000, reagg=True, cols=1),
    make_stage("distribution", "groupby", 150_000, 40, reagg=True, cols=2),
    make_stage("order", "orderby", 40, 40)]),

 "Q14": dict(stages=[
    make_stage("lineitem month", "filter", 6_001_215, 71_000, cut=True, cols=16, strings=9, dmul=60),
    make_stage("x part", "join", 271_000, 71_000, cut=True, cols=5, strings=1),
    make_stage("aggregate", "groupby", 71_000, 1, reagg=True)]),

 "Q15": dict(uses={1: 2}, stages=[
    make_stage("lineitem window", "filter", 6_001_215, 214_000, cut=True, cols=16, strings=9, dmul=58),
    make_stage("per-supplier revenue", "groupby", 214_000, 10_000, cut=True, reagg=True, cols=2),
    make_stage("max + supplier join", "join", 30_000, 1, cut=True, cols=5, strings=3),
    make_stage("order", "orderby", 1, 1)]),

 "Q16": dict(stages=[
    make_stage("complaint suppliers", "filter", 10_000, 4, cut=True, cols=1),
    # The anti join's destructive side carries predicates, which the rewriter can't put in a
    # signature, so this stage and everything above it is rejected outright.
    make_stage("part filter x partsupp x anti", "antijoin", 1_010_000, 9_700, cut=True, cols=4,
               strings=2, dmul=125 * 150, unsafe_side=True),  # BRAND x TYPE
    make_stage("dedup", "groupby", 9_700, 9_700, reagg=True, cols=3, strings=2, gkind="str"),
    make_stage("count", "groupby", 9_700, 18_300, reagg=True, cols=4, strings=2, gkind="str"),
    make_stage("order", "orderby", 18_300, 18_300)]),

 "Q17": dict(stages=[
    make_stage("per-part avg", "groupby", 6_001_215, 200_000, cut=True, reagg=True, cols=2),
    make_stage("x lineitem x part-filter", "join", 6_202_000, 6_000, cut=True, cols=4, dmul=1000, widen=False),
    make_stage("quantity filter + sum", "groupby", 6_000, 1, reagg=True)]),

 "Q18": dict(stages=[
    make_stage("per-order quantity", "groupby", 6_001_215, 1_500_000, reagg=True, cols=2),
    make_stage("threshold", "filter", 1_500_000, 60, cut=True, cols=2, dmul=4, grows=False),
    make_stage("x customer x orders", "join", 1_650_060, 60, cut=True, cols=5, grows=False),
    make_stage("aggregate + order + top", "groupby", 60, 60, reagg=True)]),

 "Q19": dict(stages=[
    make_stage("shipping filter", "filter", 6_001_215, 430_000, cut=True, cols=16, strings=9),
    make_stage("x part", "join", 630_000, 430_000, cut=True, cols=25, strings=13),
    # Or across four columns: the rewriter has no coverage reasoning for that shape, so nothing
    # at or above here can serve a query it doesn't match exactly.
    make_stage("brand/qty filter", "filter", 430_000, 121, cut=True, cols=25, strings=13,
               dmul=25 * 25 * 25, unsafe_side=True),
    make_stage("aggregate", "groupby", 121, 1, reagg=True)]),

 "Q20": dict(stages=[
    make_stage("lineitem year", "filter", 6_001_215, 860_000, cut=True, cols=4, dmul=5),
    make_stage("per part-supp quantity", "groupby", 860_000, 600_000, cut=True, reagg=True, cols=3, gkind="int2"),
    make_stage("x partsupp x part x supp", "join", 1_610_000, 186, cut=True, cols=2, strings=2,
               dmul=92),  # COLOR
    make_stage("order", "orderby", 186, 186)]),

 "Q21": dict(stages=[
    make_stage("per-order supp range", "groupby", 6_001_215, 1_500_000, reagg=True, cols=3),
    make_stage("multi-supplier orders", "filter", 1_500_000, 1_400_000, cut=True, cols=3),
    make_stage("joins + anti + nation filter", "antijoin", 9_000_000, 411, cut=True, cols=3, strings=1,
      dmul=25, widen=False, unsafe_side=True),
    make_stage("aggregate + order", "groupby", 411, 100, reagg=True)]),

 "Q22": dict(stages=[
    make_stage("country code project", "project", 150_000, 150_000, cut=True, cols=3, strings=1),
    make_stage("anti orders", "antijoin", 1_650_000, 50_000, cut=True, cols=3, strings=1),
    make_stage("code filter + avg join", "join", 200_000, 6_400, cut=True, cols=3, strings=1, dmul=None),
    make_stage("aggregate + order", "groupby", 6_400, 7, reagg=True)]),
}


def build_rate(V, N):
    """What fraction of N instances have to build a view rather than reuse one.

    With V distinct views and N instances landing on them at random, the expected number of
    distinct views actually touched is V * (1 - (1 - 1/V)^N). Divided by N, that's the share
    of instances paying build cost instead of reuse cost. More views means more misses.
    """
    if not V:
        return 1.0
    return V * (1.0 - (1.0 - 1.0 / V) ** N) / N


def pipeline_total(spec, k, first_param):
    """Cost of running the whole query once, including any subtree the query evaluates more
    than once (see uses)."""
    stages = spec["stages"]
    total = pipeline_costs(stages, k, first_param)
    for cut, uses in spec.get("uses", {}).items():
        total += (uses - 1) * pipeline_costs(stages, k, first_param, cut)
    return total


def pipeline_costs(stages, k, first_param, upto=None):
    """Total cost of stages [0, upto] with k parameter values covered: base-side input rows are
    fixed, carried rows scale with the previous stage's output scale."""
    total, prev_out, prev_scale = 0.0, 0, 1.0

    for i, stage in enumerate(stages[: (upto + 1) if upto is not None else len(stages)]):
        # rows arriving from a base table are fixed; rows carried from the previous stage
        # scale with whatever k that stage was covering
        base_in = max(0, stage["rin"] - prev_out)
        total += OP_NS[stage["op"]] * 1e-9 * (base_in + prev_out * prev_scale)

        if stage["op"] == "groupby":
            per_group = {"int": GROUPBY_OUT_NS, "int2": GROUPBY_OUT2_NS, "str": GROUPBY_STR_NS}[stage["gkind"]]
            total += per_group * 1e-9 * stage["rout"]

        scale = k if (i >= first_param and stage["grows"]) else 1.0
        prev_out, prev_scale = stage["rout"], scale

    return total


def first_param_stage(stages):
    """Index of the first stage a substitution parameter reaches. Everything above it is
    parameter-free, so a view cut there serves every instance."""
    for i, stage in enumerate(stages):
        if stage["dmul"] is None or stage["dmul"] > 1:
            return i
    return len(stages)


def domain_at(stages, cut):
    """How many distinct parameter combinations a view cut here would have to cover.
    None if any stage above it is unbounded or continuous, which needs banding instead."""
    domain = 1
    for stage in stages[: cut + 1]:
        if stage["dmul"] is None:
            return None
        domain *= stage["dmul"]
    return domain


def widen_ok(stages, cut, first_param):
    """Whether one view can cover several parameter values by widening its range and letting
    the rewriter add a residual filter. An aggregate in the way makes that impossible, since
    you can't filter a value back out of a sum."""
    span = stages[first_param: cut + 1]
    return not any(stage["reagg"] for stage in span) and all(
        stage["widen"] for stage in span if stage["dmul"] != 1 or stage is span[0])


def price(spec, stages, gamma, consts, candidates, cut, V, N, D, rows_override=None,
          k_override=None, overlap=1, cover=1.0):
    """Predicted cost per instance for one design, meaning one (cut, V) pair.

    Four things get added up:
      below/above  the pipeline either side of the cut, so build cost and reuse cost
      scan         the rewriter scoring candidate views, charged once per query that has to
                   scan them, so a design pays for the load it puts on the whole registry
      build_rate   how often an instance misses and has to build rather than reuse

    candidates is the views registered by other families on this query's base tables. overlap
    is how many queries read those tables, so V * overlap is the scanning this design causes.
    """
    first_param = first_param_stage(stages)
    stage = stages[cut]

    # how many parameter values one view has to cover, and how many rows that makes it
    k = k_override if k_override is not None else ((D / V) if (D and V) else 1.0)
    rows = rows_override if rows_override is not None else (
        stage["rout"] * (k if (cut >= first_param and stage["grows"]) else 1.0))
    num = stage["cols"] - stage["strings"]

    # split the pipeline at the cut: below is what the view stores, above is what still runs
    shared_cut, use_count = next(iter(spec.get("uses", {1 << 30: 1}).items()))
    total = pipeline_total(spec, k, first_param)

    if cut <= shared_cut:
        below = gamma * pipeline_costs(stages, k, first_param, cut)
        above = gamma * (total - use_count * pipeline_costs(stages, k, first_param, cut))
    else:
        below = gamma * (pipeline_costs(stages, k, first_param, cut)
                         + (use_count - 1) * pipeline_costs(stages, k, first_param, shared_cut))
        above = gamma * total - below

    # materialising the view costs a rebox per row, reusing it costs an unbox plus a residual
    build = below + rows * (stage["strings"] * consts["rebox_str"] + num * consts["rebox_num"])
    reuse = above + rows * (stage["strings"] * consts["unbox_str"] + num * consts["unbox_num"]
                            + consts["filter_row"])

    # Every view on these base tables gets scored by every query that reads them, so this
    # design's V views are scanned overlap times over, not once. That's what stops the sweep
    # picking the largest V available: more views help this query and tax every other one.
    scan = 2 * consts["scan"] * (candidates + max(V, 1) * max(overlap, 1))

    # instances the design can't serve fall back to running plain
    served = build_rate(V, max(cover * N, 1.0)) * build + reuse
    plain = gamma * pipeline_total(spec, 1.0, first_param)
    return cover * served + (1.0 - cover) * plain + scan


def sweep(query, plain_cost, consts, candidates, N, overlap=1):
    """Every feasible design for one query, cheapest first.

    Walks each cut point and every granularity V that cut admits, prices them all, and returns
    them sorted. "no view" is always in there as the baseline, and any hand-derived designs in
    the spec are priced on the same terms so they compete directly.
    """
    spec = QUERIES[query]
    stages = spec["stages"]
    first_param = first_param_stage(stages)

    # gamma calibrates the modelled pipeline against what exp1 actually measured
    gamma = plain_cost / pipeline_total(spec, 1.0, first_param)

    out = [("no view", None, 0, plain_cost)]
    unsafe_from = next((i for i, stage in enumerate(stages) if stage["unsafe_side"]), len(stages))

    for cut, stage in enumerate(stages):
        if not stage["cut"] or cut >= unsafe_from:
            continue

        # above the first parameter: one view serves every instance, so V is always 1
        if cut < first_param:
            out.append((f"s{cut} {stage['label']} (parameter-free)", cut, 1,
                        price(spec, stages, gamma, consts, candidates, cut, 1, N, None, k_override=1.0,
                              overlap=overlap)))
            continue

        D = domain_at(stages, cut)

        # unbounded parameter: no finite domain to divide, so band it instead
        if D is None:
            continuous = [i for i, stage in enumerate(stages[: cut + 1]) if stage["dmul"] is None]
            discrete = [i for i, stage in enumerate(stages[: cut + 1])
                        if stage["dmul"] not in (1, None)]
            if len(continuous) == 1 and not discrete and stages[continuous[0]]["span"]:
                continuous_stage = stages[continuous[0]]
                span, window = continuous_stage["span"], continuous_stage["window"]
                for V in range(1, int(span // window) + 1):
                    cover = (span - V * window) / (span - window) if V > 1 else 1.0
                    if cover <= 0:
                        break
                    k = (span / window) / V
                    out.append((f"s{cut} {stage['label']} V={V} bands (serve {cover:.0%})", cut, V,
                                price(spec, stages, gamma, consts, candidates, cut, V, N, None,
                                      k_override=k, overlap=overlap, cover=cover)))
            continue

        # finite domain: try every V from one view covering everything up to one per value,
        # unless an aggregate in the way means only the exact V = D design is possible
        granularities = range(1, D + 1) if widen_ok(stages, cut, first_param) else [D]
        for V in granularities:
            # A design can't have more views than there are instances to build them: past
            # that, most of the registry is never populated, so its benefit isn't real.
            if V > N:
                continue
            out.append((f"s{cut} {stage['label']}", cut, V,
                        price(spec, stages, gamma, consts, candidates, cut, V, N, D,
                              overlap=overlap)))

    # the hand-derived designs get priced on exactly the same terms, so they compete directly
    for cut, V, rows, k, label in spec.get("manual", []):
        out.append((label + " [manual]", cut, V,
                    price(spec, stages, gamma, consts, candidates, cut, V, N, domain_at(stages, cut),
                          rows_override=rows, k_override=k, overlap=overlap)))

    ranked = sorted(out, key=lambda design: design[3])

    # A view has to be worth having. Some designs come out a fraction of a microsecond ahead of
    # running plain on a query that takes milliseconds, which is the model's own rounding rather
    # than a saving, so no view stands unless a design beats it at the precision we report.
    plain_design = next(design for design in ranked if design[1] is None)
    if round(plain_cost / ranked[0][3], 2) <= round(plain_cost / plain_design[3], 2):
        ranked.remove(plain_design)
        ranked.insert(0, plain_design)

    return ranked


RES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")

# base tables per query and views per family: feeds the registry-scan term (how many
# candidate views a query must score, and how many queries a family's views tax).
BASE = {
 "Q1": {"lineitem"}, "Q2": {"part", "supplier", "partsupp", "nation", "region"},
 "Q3": {"customer", "orders", "lineitem"}, "Q4": {"orders", "lineitem"},
 "Q5": {"customer", "orders", "lineitem", "supplier", "nation", "region"}, "Q6": {"lineitem"},
 "Q7": {"lineitem", "supplier", "orders", "customer", "nation"},
 "Q8": {"part", "supplier", "lineitem", "orders", "customer", "nation", "region"},
 "Q9": {"part", "supplier", "lineitem", "partsupp", "orders", "nation"},
 "Q10": {"customer", "orders", "lineitem", "nation"}, "Q11": {"partsupp", "supplier", "nation"},
 "Q12": {"orders", "lineitem"}, "Q13": {"customer", "orders"}, "Q14": {"lineitem", "part"},
 "Q15": {"lineitem", "supplier"}, "Q16": {"part", "partsupp", "supplier"}, "Q17": {"lineitem", "part"},
 "Q18": {"customer", "orders", "lineitem"}, "Q19": {"lineitem", "part"},
 "Q20": {"part", "partsupp", "lineitem", "supplier", "nation"},
 "Q21": {"supplier", "lineitem", "orders", "nation"}, "Q22": {"customer", "orders"}}
FAMILY_COUNT = {"Q1": 61, "Q2": 5, "Q3": 5, "Q4": 3, "Q5": 25, "Q6": 5, "Q7": 1, "Q8": 5, "Q9": 2,
                "Q10": 7, "Q11": 25, "Q12": 5, "Q13": 16, "Q14": 12, "Q15": 58, "Q16": 0, "Q17": 1,
                "Q18": 1, "Q19": 1, "Q20": 5, "Q21": 2, "Q22": 1}


def candidates(query):
    """Views the rewriter has to score for this query: everything in the registry sharing one
    of its base tables, minus its own family."""
    return sum(FAMILY_COUNT[other] for other in BASE
               if other != query and BASE[other] & BASE[query])


def overlap(query):
    """How many queries share a base table with this one, so how many get taxed by its views."""
    return sum(1 for other in BASE if BASE[other] & BASE[query])


def read_results(experiment):
    """Rows of results/<experiment>/results.csv."""
    path = os.path.join(RES, experiment, "results.csv")
    with open(path) as f:
        return list(csv.DictReader(f))


def median(values):
    """Median, or NaN for an empty list, so a missing measurement doesn't raise."""
    return statistics.median(values) if values else float("nan")


def exp0_constants():
    """Per-row engine costs, measured in exp0."""
    samples = {}
    for row in read_results("exp0"):
        if row["ok"] == "true":
            key = (row["measurement"], row["type"], int(row["rows"]))
            samples.setdefault(key, []).append(float(row["seconds"]))

    def at(measurement, type_name, rows):
        """Median seconds for one exp0 measurement at one row count."""
        return median(samples.get((measurement, type_name, rows), []))

    def per_row(measurement, type_name):
        """Marginal cost of one row. Two sizes and the difference between them, so the fixed
        setup cost cancels out and what's left is per-row.

        OrderBy uses a smaller pair because it's n log n: its slope depends on where you take
        it, and 10k-40k is the range the queries actually sort in."""
        lo, hi = (10_000, 40_000) if measurement == "orderby" else (1_000_000, 4_000_000)
        return (at(measurement, type_name, hi) - at(measurement, type_name, lo)) / (hi - lo)

    def scan_per_candidate():
        """Per-candidate rewriter cost, as the slope through every registry size exp0 measured.

        The pipeline's fixed cost is identical at every size, so it lands in the intercept and
        the slope comes out clean. Subtracting two points instead would carry that cost's
        run-to-run drift straight into the answer, and it drifts by more than the scan costs."""
        sizes = sorted(rows for measurement, type_name, rows in samples
                       if measurement == "scan" and type_name == "views")
        points = [(size, at("scan", "views", size)) for size in sizes]
        points = [(size, seconds) for size, seconds in points if not math.isnan(seconds)]
        if len(points) < 2:
            return float("nan")

        mean_size = sum(size for size, _ in points) / len(points)
        mean_seconds = sum(seconds for _, seconds in points) / len(points)
        covariance = sum((size - mean_size) * (seconds - mean_seconds) for size, seconds in points)
        variance = sum((size - mean_size) ** 2 for size, _ in points)
        return covariance / variance if variance else float("nan")

    consts = dict(
        scan=scan_per_candidate(),
        filter_row=per_row("filter", "int64"),
        filter_wide_row=per_row("filter_wide", "int64"),
        groupby_row=per_row("groupby", "int64"),
        groupby_hi_row=per_row("groupby_hi", "int64"),
        groupby_hi2_row=per_row("groupby_hi2", "int64"),
        groupby_str_row=per_row("groupby_str", "string"),
        join_row=per_row("join", "int64"),
        project_row=per_row("project", "int64"),
        orderby_row=per_row("orderby", "int64"),
    )
    for type_name in ("int64", "string"):
        consts[f"unbox_{type_name}"] = per_row("unbox_engine", type_name)
        consts[f"rebox_{type_name}"] = per_row("rebox_engine", type_name)
    return consts


def exp1_plain():
    """What each query costs on BOSS with no views at all, measured in exp1."""
    out = {}
    for row in read_results("exp1"):
        if (row["system"] == "BOSS" and row["ok"] == "true" and int(row["rep"]) >= 1
                and row["query"] != "SMOKE"):
            out.setdefault(row["query"], []).append(float(row["seconds"]))
    return {query: median(seconds) for query, seconds in out.items()}


def sweep_consts(consts):
    """Fills in every modelled constant from what exp0 measured, then hands back the ones
    price() needs directly.

    Nothing has a fallback. If exp0 didn't record something, this exits and says so rather
    than pricing designs against a guess, because a wrong constant produces a confident table
    that happens to be wrong.
    """
    missing = []

    def measured(key, floor=0.0):
        """The value if exp0 recorded a usable one, otherwise None and a note in missing."""
        value = consts.get(key, float("nan"))
        if math.isnan(value) or value <= floor:
            missing.append(key)
            return None
        return value

    # a groupby's per-group cost is whatever it costs above its per-row cost
    per_row_groupby = measured("groupby_row")
    for key, name in (("groupby_hi_row", "GROUPBY_OUT_NS"),
                      ("groupby_hi2_row", "GROUPBY_OUT2_NS"),
                      ("groupby_str_row", "GROUPBY_STR_NS")):
        value = measured(key, per_row_groupby or 0.0)
        if value is not None and per_row_groupby is not None:
            globals()[name] = (value - per_row_groupby) * 1e9

    # filter uses the wide-row measurement: real scans carry every column, not just the key
    for key, ops in (("filter_wide_row", ("filter",)),
                     ("join_row", ("join", "antijoin")),
                     ("groupby_row", ("groupby",)),
                     ("project_row", ("project",)),
                     ("orderby_row", ("orderby",))):
        value = measured(key)
        if value is not None:
            for op in ops:
                OP_NS[op] = value * 1e9

    for key in ("scan", "rebox_string", "rebox_int64", "unbox_string", "unbox_int64",
                "filter_row"):
        if math.isnan(consts.get(key, float("nan"))):
            missing.append(key)

    if missing:
        sys.exit("exp0 hasn't measured: " + ", ".join(sorted(set(missing)))
                 + "\nrun ./run_all.sh exp0 first")

    return dict(scan=consts["scan"],
                rebox_str=consts["rebox_string"], rebox_num=max(consts["rebox_int64"], 0.0),
                unbox_str=consts["unbox_string"], unbox_num=max(consts["unbox_int64"], 0.0),
                filter_row=max(consts["filter_row"], 0.0))


def main(argv):
    """Prints the winning design per query, with its predicted speedup over running plain."""
    streams, top, only = 256, 3, []  # three runners-up, so four designs per query by default
    as_csv = "--csv" in argv
    args = iter(a for a in argv if a != "--csv")
    for arg in args:
        if arg == "--top":
            top = int(next(args))
        elif arg.startswith("Q"):
            only.append(arg)
        else:
            streams = int(arg)

    consts = sweep_consts(exp0_constants())
    plain = exp1_plain()

    writer = csv.writer(sys.stdout) if as_csv else None
    if writer:
        writer.writerow(["query", "V", "predicted", "design"])
    else:
        print(f"{'query':6s} {'V':>4s} {'predicted':>10s}  design   (N = {streams})")
    for query in QUERIES:
        if only and query not in only:
            continue

        # Q13 is only ever run in its rewritten form, so that's the cost to price against
        plain_cost = plain.get("Q13R", plain.get(query)) if query == "Q13" else plain.get(query)
        if not plain_cost or math.isnan(plain_cost):
            print(f"{query:6s}  (no measured plain cost in results/exp1)")
            continue

        ranked = sweep(query, plain_cost, consts, candidates(query), streams, overlap(query))
        rows = [ranked[0]]

        # If a hand-derived design exists, always show the best of the other kind next to it,
        # so it's obvious whether the enumeration or the hand design won.
        if any(design[0].endswith("[manual]") for design in ranked):
            want_manual = "[manual]" not in ranked[0][0]
            rows.append(next(d for d in ranked if d[0].endswith("[manual]") == want_manual))

        rows += [design for design in ranked[1: 1 + top] if design not in rows]

        for i, (label, cut, V, total) in enumerate(rows):
            speedup = plain_cost / total
            if writer:
                # a starred query is the runner-up of the other kind, enumerated vs hand-derived
                writer.writerow([query if i == 0 else f"{query}*", V, f"{speedup:.2f}", label])
            else:
                prefix = "" if i == 0 else "   also: "
                print(f"{query if i == 0 else '':6s} {V:4d} {speedup:9.2f}x  {prefix}{label}")


if __name__ == "__main__":
    main(sys.argv[1:])
