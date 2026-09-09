#include "Views.hpp"
#include "BossQueries.hpp"
#include "Streams.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <sstream>

using boss::Symbol;
using namespace boss::utilities;
using namespace tpch;

namespace views {
namespace {

boss::Expression byName(const std::string &table) { return "ByName"_(Symbol(table)); }

// "1995-03-01" from (1995, 3)
std::string month(int year, int monthNumber) {
  std::ostringstream stream;
  stream << year << "-" << std::setfill('0') << std::setw(2) << monthNumber << "-01";
  return stream.str();
}

const std::vector<std::string> regions = {"AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"};
const std::vector<std::string> segments = {"AUTOMOBILE", "BUILDING", "FURNITURE", "HOUSEHOLD",
                                           "MACHINERY"};
const std::vector<std::string> nations = {
    "ALGERIA",      "ARGENTINA",      "BRAZIL",        "CANADA",     "CHINA", "EGYPT",   "ETHIOPIA",
    "FRANCE",       "GERMANY",        "INDIA",         "INDONESIA",  "IRAN",  "IRAQ",    "JAPAN",
    "JORDAN",       "KENYA",          "MOROCCO",       "MOZAMBIQUE", "PERU",  "ROMANIA", "RUSSIA",
    "SAUDI ARABIA", "UNITED KINGDOM", "UNITED STATES", "VIETNAM"};

struct Def {
  View view;
  std::function<boss::Expression()> body;
};

std::vector<Def> definitions() {
  std::vector<Def> defs;
  auto add = [&](const std::string &query, const std::string &name,
                 std::function<boss::Expression()> body) {
    defs.push_back({{name, query}, std::move(body)});
  };

  // Q1: DELTA in [60,120] days -> 61 shipdate bounds; the whole aggregate per bound (4 rows each).
  for (int delta = 60; delta <= 120; ++delta) {
    auto bound = streams::addDays("1998-12-01", -delta);
    add("Q1", "Q1_VIEW_" + bound, [bound] { return q1Aggregate(bound); });
  }

  // Q2: REGION (5); SIZE/TYPE filter part, which joins on afterwards.
  for (auto const &region : regions)
    add("Q2", "Q2_VIEW_" + region, [region] { return q2SuppRegion(region); });

  // Q3: SEGMENT (5), with the date window widened to cover every spec DATE (1995-03-01..31).
  for (auto const &segment : segments)
    add("Q3", "Q3_VIEW_" + segment,
        [segment] { return q3BaseJoin(segment, "1996-01-01", "1994-01-01"); });

  // Q4: 3 views over the orders join, date windows widened (argmin V=3 over 58 months).
  {
    const char *bounds[] = {"1993-01-01", "1994-09-01", "1996-04-01", "1997-11-01"};
    for (int i = 0; i < 3; ++i) {
      std::string lo = bounds[i], hi = streams::addMonths(bounds[i + 1], 2);
      add("Q4", "Q4_VIEW_" + lo, [lo, hi] { return q4OrdersJoin(lo, hi); });
    }
  }

  // Q5: 25 exact (region, year) views of the full join (argmin V=25).
  for (auto const &region : regions)
    for (int year = 1993; year <= 1997; ++year) {
      std::string lo = std::to_string(year) + "-01-01", hi = std::to_string(year + 1) + "-01-01";
      add("Q5", "Q5_VIEW_" + region + "_" + std::to_string(year),
          [region, lo, hi] { return q5RegionYearJoin(region, lo, hi); });
    }

  // Q6: DATE year (5); QUANTITY {24,25} and DISCOUNT widened to their full domains.
  for (int year = 1993; year <= 1997; ++year) {
    std::string lower = std::to_string(year - 1) + "-12-31",
                upper = std::to_string(year + 1) + "-01-01";
    add("Q6", "Q6_VIEW_" + std::to_string(year), [lower, upper] {
      return "Filter"_(q6NarrowedLineitem(),
                       "And"_("Less"_("l_quantity"_, 26), "Greater"_("l_discount"_, 0.0098),
                              "Less"_("l_discount"_, 0.1002), "Less"_("l_shipdate"_, upper),
                              "Greater"_("l_shipdate"_, lower)));
    });
  }

  // Q7: parameter-free join chain (the nation pair filters its output).
  add("Q7", "Q7_VIEW", [] { return q7Chain(); });

  // Q8: REGION (5).
  for (auto const &region : regions)
    add("Q8", "Q8_VIEW_" + region, [region] { return q8Join2(region); });

  // Q9: two price bands over the continuous domain (model argmin V=2; measured 3.50x vs the
  // whole-domain view's 3.02x). Windows straddling the 1500 boundary run plain (~7% observed 6.3%).
  for (auto const &[name, lo, hi] :
       {std::tuple<const char *, double, double>{"LO", 901.0, 1500.0}, {"HI", 1499.99, 2098.99}})
    add("Q9", std::string("Q9_VIEW_") + name, [lo, hi] {
      auto partFiltered =
          "Project"_("Filter"_(byName("part"), "And"_("Greater"_("p_retailprice"_, lo),
                                                      "Less"_("p_retailprice"_, hi))),
                     "p_partkey"_, "p_retailprice"_);
      auto nationSupplier =
          "Join"_(byName("nation"), byName("supplier"), "Equal"_("n_nationkey"_, "s_nationkey"_));
      auto withPartsupp = "Project"_("Join"_(std::move(nationSupplier), byName("partsupp"),
                                             "Equal"_("s_suppkey"_, "ps_suppkey"_)),
                                     "n_name"_, "ps_partkey"_, "ps_suppkey"_, "ps_supplycost"_);
      auto partNsp = "Join"_(std::move(partFiltered), std::move(withPartsupp),
                             "Equal"_("p_partkey"_, "ps_partkey"_));
      auto withLineitem = "Project"_("Join"_(std::move(partNsp), byName("lineitem"),
                                             "Equal"_("ps_partkey"_, "l_partkey"_),
                                             "Equal"_("ps_suppkey"_, "l_suppkey"_)),
                                     "n_name"_, "p_retailprice"_, "ps_supplycost"_, "l_orderkey"_,
                                     "l_extendedprice"_, "l_discount"_, "l_quantity"_);
      return "Join"_(std::move(withLineitem),
                     "Project"_(byName("orders"), "o_orderkey"_, "o_orderdate"_),
                     "Equal"_("l_orderkey"_, "o_orderkey"_));
    });

  // Q10: 7 widened views over the 24-month DATE domain (argmin V=7); residual date filter.
  {
    const int sizes[] = {4, 3, 3, 4, 3, 3, 4};
    std::string lo = "1993-02-01";
    for (int size : sizes) {
      std::string next = streams::addMonths(lo, size), hi = streams::addMonths(next, 2);
      add("Q10", "Q10_VIEW_" + lo, [lo, hi] { return q10BaseJoin(lo, hi); });
      lo = next;
    }
  }

  // Q11: NATION (25); the same join is used by both of Q11's round trips.
  for (auto const &nation : nations) {
    // "SAUDI ARABIA" can't go in a view name as-is
    std::string safe = nation;
    std::replace(safe.begin(), safe.end(), ' ', '_');
    add("Q11", "Q11_VIEW_" + safe, [nation] { return q11BaseJoin(nation); });
  }

  // Q12: DATE year (5); the shipmode pair is a residual filter.
  for (int year = 1993; year <= 1997; ++year) {
    std::string lo = std::to_string(year) + "-01-01", hi = std::to_string(year + 1) + "-01-01";
    add("Q12", "Q12_VIEW_" + std::to_string(year), [lo, hi] { return q12YearJoin(lo, hi); });
  }

  // Q13: 16 exact per-wordpair views of the pattern projection (argmin V=16, 2 numeric columns).
  for (auto const &w1 : {"special", "pending", "unusual", "express"})
    for (auto const &w2 : {"packages", "requests", "accounts", "deposits"})
      add("Q13R", std::string("Q13_VIEW_") + w1 + "_" + w2,
          [w1, w2] { return q13Adjusted(w1, w2); });

  // Q14: 12 widened 5-month views over the 60-month DATE domain
  {
    std::string lo = "1993-01-01";
    for (int i = 0; i < 12; ++i) {
      std::string hi = streams::addMonths(lo, 5);
      add("Q14", "Q14_VIEW_" + lo, [lo, hi] { return q14BaseJoin(lo, hi); });
      lo = hi;
    }
  }

  // Q15: 58 exact per-month per-supplier revenue aggregates (argmin; serves both of the
  // query's two evaluations of the revenue subtree).
  for (int year = 1993; year <= 1997; ++year)
    for (int monthNumber = 1; monthNumber <= 12; ++monthNumber) {
      if (year == 1997 && monthNumber > 10)
        break; // the domain stops at 1997-10

      auto lo = month(year, monthNumber), hi = streams::addMonths(lo, 3);
      add("Q15", "Q15_VIEW_" + lo, [lo, hi] { return q15Revenue(lo, hi); });
    }

  // Q16: no view. Its only parameter-free part, the complaint suppliers at 32 bytes, saves less
  // than consulting the view costs.

  // Q17: parameter-free per-part average.
  add("Q17", "Q17_VIEW", [] { return q17PartAvg(); });

  // Q18: the customer x orders join included, threshold widened to 311 (argmin V=1, ~60 rows).
  add("Q18", "Q18_VIEW", [] { return q18Joined(311); });

  // Q19: parameter-free shipping filter + part join.
  add("Q19", "Q19_VIEW", [] { return q19SharedJoin(); });

  // Q20: DATE year (5).
  for (int year = 1993; year <= 1997; ++year) {
    std::string lo = std::to_string(year) + "-01-01", hi = std::to_string(year + 1) + "-01-01";
    add("Q20", "Q20_VIEW_" + std::to_string(year), [lo, hi] { return q20LineitemQty(lo, hi); });
  }

  // Q21: two parameter-free aggregates. The sweep's per-nation anti-joined views are blocked by
  // the rewriter (predicates on an anti join's destructive side are unrepresentable).
  add("Q21", "Q21_MULTISUPP_VIEW", [] { return q21MultiSuppOrders(); });
  add("Q21", "Q21_MULTILATE_VIEW", [] { return q21MultiLateOrders(); });

  // Q22: parameter-free NOT EXISTS(orders).
  add("Q22", "Q22_VIEW", [] { return q22NoOrders(); });

  // ---- Manually designed cross-query serving views (union columns; each shape occurs as a
  // subtree in two query families and carries the union of both consumers' columns plus the
  // columns their residual filters need). The query-specialised families above stay deployed,
  // so these only serve where the rewriter's scoring prefers them.
  // supplier x nation x region, per region: the identical subtree of Q2 (q2SuppRegion's
  // supplier-nation core) and Q5 (q5LiSupp's suppRegion core).
  for (auto const &region : regions)
    add("SHARED", "SNR_VIEW_" + region, [region] {
      return "Project"_("Join"_(byName("supplier"),
                                "Join"_(byName("nation"),
                                        "Filter"_(byName("region"), "Equal"_("r_name"_, region)),
                                        "Equal"_("n_regionkey"_, "r_regionkey"_)),
                                "Equal"_("s_nationkey"_, "n_nationkey"_)),
                        "s_suppkey"_, "s_nationkey"_, "s_acctbal"_, "s_name"_, "s_address"_,
                        "s_phone"_, "s_comment"_, "n_name"_);
    });
  // customer x orders, customer on the left: Q18's custOrders subtree exactly; Q3's base join
  // with the segment and date predicates as residuals (c_mktsegment/o_orderdate exposed).
  add("SHARED", "CO_CUST_ORDERS_VIEW", [] {
    return "Project"_(
        "Join"_(byName("customer"), byName("orders"), "Equal"_("c_custkey"_, "o_custkey"_)),
        "c_custkey"_, "c_mktsegment"_, "o_orderkey"_, "o_custkey"_, "o_orderdate"_, "o_totalprice"_,
        "o_shippriority"_);
  });
  // orders x customer, orders on the left: Q7's join2 subtree; Q10's base join with the date
  // predicate as a residual (o_orderdate exposed).
  add("SHARED", "OC_ORDERS_CUSTOMER_VIEW", [] {
    return "Project"_(
        "Join"_(byName("orders"), byName("customer"), "Equal"_("o_custkey"_, "c_custkey"_)),
        "o_orderkey"_, "o_custkey"_, "o_orderdate"_, "c_custkey"_, "c_name"_, "c_acctbal"_,
        "c_address"_, "c_phone"_, "c_comment"_, "c_nationkey"_);
  });
  return defs;
}

} // namespace

// VIEWS_ONLY_QUERY=<tag> restricts the registry to one family tag (e.g. SHARED) for probes.
bool enabled(const Def &definition) {
  const char *only = std::getenv("VIEWS_ONLY_QUERY");
  return !only || definition.view.query == only;
}

// Built once on first call, since definitions() rebuilds every lambda each time it runs
const std::vector<View> &all() {
  static const std::vector<View> cached = [] {
    std::vector<View> out;
    for (auto const &definition : definitions())
      if (enabled(definition))
        out.push_back(definition.view);
    return out;
  }();
  return cached;
}

std::vector<std::string> defineAll() {
  std::vector<std::string> failed;
  for (auto const &definition : definitions())
    if (enabled(definition) && !bossq::defineView(definition.view.name, definition.body()))
      failed.push_back(definition.view.name);
  return failed;
}

} // namespace views
