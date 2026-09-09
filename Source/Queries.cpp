#include "Queries.hpp"
#include "BossQueries.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

using boss::Symbol;
using namespace boss::utilities;

namespace tpch {
namespace {

boss::Expression byName(const std::string &table) { return "ByName"_(Symbol(table)); }

boss::Expression revenueExpr() {
  return "Multiply"_("l_extendedprice"_, "Subtract"_(1.0, "l_discount"_));
}

} // namespace

const std::vector<std::string> &allQueries() {
  static const std::vector<std::string> names = {
      "Q1",  "Q2",  "Q3",  "Q4",  "Q5",  "Q6",  "Q7",  "Q8",  "Q9",  "Q10", "Q11",
      "Q12", "Q13", "Q14", "Q15", "Q16", "Q17", "Q18", "Q19", "Q20", "Q21", "Q22"};
  return names;
}

const std::vector<std::string> &viewQueries() {
  static const std::vector<std::string> names = {
      "Q1",  "Q2",   "Q3",  "Q4",  "Q5",  "Q6",  "Q7",  "Q8",  "Q9",  "Q10", "Q11",
      "Q12", "Q13R", "Q14", "Q15", "Q16", "Q17", "Q18", "Q19", "Q20", "Q21", "Q22"};
  return names;
}

// ---------------------------------------------------------------- Q1
// Pricing summary over everything shipped before the cutoff, grouped by return flag and status
boss::Expression q1Aggregate(const std::string &shipdateBound) {
  auto grouped = "GroupBy"_(
      "Project"_("Filter"_(byName("lineitem"), "LessEqual"_("l_shipdate"_, shipdateBound)),
                 "As"_(revenueExpr(), "disc_price"_),
                 "As"_("Multiply"_(revenueExpr(), "Add"_("l_tax"_, 1.0)), "charge"_),
                 "l_returnflag"_, "l_linestatus"_, "l_quantity"_, "l_extendedprice"_,
                 "l_discount"_),
      "Sum"_("l_quantity"_), "Sum"_("l_extendedprice"_), "Sum"_("disc_price"_), "Sum"_("charge"_),
      "Count"_("l_quantity"_), "Mean"_("l_quantity"_), "Mean"_("l_extendedprice"_),
      "Mean"_("l_discount"_), "l_returnflag"_, "l_linestatus"_);
  return "Project"_(std::move(grouped), "l_returnflag"_, "l_linestatus"_,
                    "As"_(Symbol("sum(l_quantity)"), "sum_qty"_),
                    "As"_(Symbol("sum(l_extendedprice)"), "sum_base_price"_),
                    "As"_(Symbol("sum(disc_price)"), "sum_disc_price"_),
                    "As"_(Symbol("sum(charge)"), "sum_charge"_),
                    "As"_(Symbol("mean(l_quantity)"), "avg_qty"_),
                    "As"_(Symbol("mean(l_extendedprice)"), "avg_price"_),
                    "As"_(Symbol("mean(l_discount)"), "avg_disc"_),
                    "As"_(Symbol("count(l_quantity)"), "count_order"_));
}

boss::Expression buildQ1(const QueryParams &params) {
  return "OrderBy"_(q1Aggregate(params.q1ShipdateBound), "keys"_("l_returnflag"_, "l_linestatus"_));
}

// ---------------------------------------------------------------- Q2
// The cheapest supplier for each part of a given size and type within one region
boss::Expression q2SuppRegion(const std::string &region) {
  auto nationRegion =
      "Project"_("Join"_(byName("nation"), "Filter"_(byName("region"), "Equal"_("r_name"_, region)),
                         "Equal"_("n_regionkey"_, "r_regionkey"_)),
                 "n_nationkey"_, "n_name"_);
  auto suppNation = "Project"_(
      "Join"_("Project"_(byName("supplier"), "s_suppkey"_, "s_nationkey"_, "s_acctbal"_, "s_name"_,
                         "s_address"_, "s_phone"_, "s_comment"_),
              std::move(nationRegion), "Equal"_("s_nationkey"_, "n_nationkey"_)),
      "s_suppkey"_, "s_acctbal"_, "s_name"_, "s_address"_, "s_phone"_, "s_comment"_, "n_name"_);
  return "Join"_(std::move(suppNation),
                 "Project"_(byName("partsupp"), "ps_suppkey"_, "ps_partkey"_, "ps_supplycost"_),
                 "Equal"_("s_suppkey"_, "ps_suppkey"_));
}

boss::Expression buildQ2(const QueryParams &params) {
  auto minCost =
      "Project"_("GroupBy"_(q2SuppRegion(params.q2Region), "Min"_("ps_supplycost"_), "ps_partkey"_),
                 "ps_partkey"_, "As"_(Symbol("min(ps_supplycost)"), "min_supplycost"_));
  auto filteredPart = "Filter"_(byName("part"), "And"_("Equal"_("p_size"_, params.q2Size),
                                                       "Like"_("p_type"_, params.q2TypeLike)));
  auto joined = "Join"_("Join"_(std::move(filteredPart), q2SuppRegion(params.q2Region),
                                "Equal"_("p_partkey"_, "ps_partkey"_)),
                        std::move(minCost), "Equal"_("ps_partkey"_, "ps_partkey"_),
                        "Equal"_("ps_supplycost"_, "min_supplycost"_));
  auto ordered = "OrderBy"_(std::move(joined),
                            "keys"_("Desc"_("s_acctbal"_), "n_name"_, "s_name"_, "p_partkey"_));
  return "Project"_("Slice"_(std::move(ordered), 0, 100), "s_acctbal"_, "s_name"_, "n_name"_,
                    "p_partkey"_, "p_mfgr"_, "s_address"_, "s_phone"_, "s_comment"_);
}

// ---------------------------------------------------------------- Q3
// Unshipped orders for one market segment, ranked by revenue
boss::Expression q3BaseJoin(const std::string &segment, const std::string &orderDateBound,
                            const std::string &shipDateBound) {
  auto custOrders = "Join"_(
      "Project"_("Filter"_(byName("customer"), "Equal"_("c_mktsegment"_, segment)), "c_custkey"_),
      "Project"_("Filter"_(byName("orders"), "Less"_("o_orderdate"_, orderDateBound)),
                 "o_orderkey"_, "o_custkey"_, "o_orderdate"_, "o_shippriority"_),
      "Equal"_("c_custkey"_, "o_custkey"_));
  return "Join"_(std::move(custOrders),
                 "Project"_("Filter"_(byName("lineitem"), "Greater"_("l_shipdate"_, shipDateBound)),
                            "l_orderkey"_, "l_shipdate"_, "l_extendedprice"_, "l_discount"_),
                 "Equal"_("o_orderkey"_, "l_orderkey"_));
}

boss::Expression buildQ3(const QueryParams &params) {
  auto projected =
      "Project"_(q3BaseJoin(params.q3Segment, params.q3Date, params.q3Date), "l_orderkey"_,
                 "o_orderdate"_, "o_shippriority"_, "As"_(revenueExpr(), "disc_price"_));
  auto grouped = "GroupBy"_(std::move(projected), "Sum"_("disc_price"_), "l_orderkey"_,
                            "o_orderdate"_, "o_shippriority"_);
  auto final =
      "Project"_(std::move(grouped), "l_orderkey"_, "As"_(Symbol("sum(disc_price)"), "revenue"_),
                 "o_orderdate"_, "o_shippriority"_);
  return "Slice"_("OrderBy"_(std::move(final), "keys"_("Desc"_("revenue"_), "o_orderdate"_)), 0,
                  10);
}

// ---------------------------------------------------------------- Q4
// Orders in a quarter that had at least one lineitem delivered late
boss::Expression q4Qualifying() {
  auto grouped =
      "GroupBy"_("Filter"_(byName("lineitem"), "Less"_("l_commitdate"_, "l_receiptdate"_)),
                 "CountAll"_(), "l_orderkey"_);
  return "Project"_(std::move(grouped), "l_orderkey"_);
}

boss::Expression q4OrdersJoin(const std::string &dateLo, const std::string &dateHi) {
  return "Join"_(
      "Project"_("Filter"_(byName("orders"), "And"_("GreaterEqual"_("o_orderdate"_, dateLo),
                                                    "Less"_("o_orderdate"_, dateHi))),
                 "o_orderkey"_, "o_orderdate"_, "o_orderpriority"_),
      q4Qualifying(), "Equal"_("o_orderkey"_, "l_orderkey"_));
}

boss::Expression buildQ4(const QueryParams &params) {
  // o_orderdate is pruned after the filter. The widened views (q4OrdersJoin)
  // keep the column on their side and serve this query cross-shape with a date residual.
  auto ordersJoin =
      "Join"_("Project"_("Filter"_(byName("orders"),
                                   "And"_("GreaterEqual"_("o_orderdate"_, params.q4DateLo),
                                          "Less"_("o_orderdate"_, params.q4DateHi))),
                         "o_orderkey"_, "o_orderpriority"_),
              q4Qualifying(), "Equal"_("o_orderkey"_, "l_orderkey"_));
  auto grouped = "GroupBy"_(std::move(ordersJoin), "CountAll"_(), "o_orderpriority"_);
  auto final = "Project"_(std::move(grouped), "o_orderpriority"_,
                          "As"_(Symbol("count_all()"), "order_count"_));
  return "OrderBy"_(std::move(final), "keys"_("o_orderpriority"_));
}

// ---------------------------------------------------------------- Q5
// Revenue where the customer and the supplier sit in the same nation
boss::Expression q5LiSupp(const std::string &region) {
  auto suppRegion = "Project"_(
      "Join"_(byName("supplier"),
              "Join"_(byName("nation"), "Filter"_(byName("region"), "Equal"_("r_name"_, region)),
                      "Equal"_("n_regionkey"_, "r_regionkey"_)),
              "Equal"_("s_nationkey"_, "n_nationkey"_)),
      "s_suppkey"_, "s_nationkey"_, "n_name"_);
  return "Project"_(
      "Join"_(byName("lineitem"), std::move(suppRegion), "Equal"_("l_suppkey"_, "s_suppkey"_)),
      "l_orderkey"_, "l_extendedprice"_, "l_discount"_, "s_nationkey"_, "n_name"_);
}

boss::Expression q5RegionYearJoin(const std::string &region, const std::string &dateLo,
                                  const std::string &dateHi) {
  auto ordLiSupp =
      "Project"_("Join"_("Filter"_(byName("orders"), "And"_("GreaterEqual"_("o_orderdate"_, dateLo),
                                                            "Less"_("o_orderdate"_, dateHi))),
                         q5LiSupp(region), "Equal"_("o_orderkey"_, "l_orderkey"_)),
                 "o_custkey"_, "s_nationkey"_, "n_name"_, "l_extendedprice"_, "l_discount"_);
  return "Project"_("Join"_(byName("customer"), std::move(ordLiSupp),
                            "Equal"_("c_custkey"_, "o_custkey"_),
                            "Equal"_("c_nationkey"_, "s_nationkey"_)),
                    "n_name"_, "l_extendedprice"_, "l_discount"_);
}

boss::Expression buildQ5(const QueryParams &params) {
  auto projected = "Project"_(q5RegionYearJoin(params.q5Region, params.q5DateLo, params.q5DateHi),
                              "n_name"_, "As"_(revenueExpr(), "disc_price"_));
  auto grouped = "GroupBy"_(std::move(projected), "Sum"_("disc_price"_), "n_name"_);
  auto final =
      "Project"_(std::move(grouped), "n_name"_, "As"_(Symbol("sum(disc_price)"), "revenue"_));
  return "OrderBy"_(std::move(final), "keys"_("Desc"_("revenue"_)));
}

// ---------------------------------------------------------------- Q6
// One straight scan of lineitem, no joins at all. The cheapest query in the set.
boss::Expression q6NarrowedLineitem() {
  return "Project"_(byName("lineitem"), "l_shipdate"_, "l_quantity"_, "l_discount"_,
                    "l_extendedprice"_);
}

boss::Expression buildQ6(const QueryParams &params) {
  return "GroupBy"_("Project"_("Filter"_(q6NarrowedLineitem(),
                                         "And"_("Less"_("l_quantity"_, params.q6Quantity),
                                                "Greater"_("l_discount"_, params.q6DiscountLow),
                                                "Less"_("l_discount"_, params.q6DiscountHigh),
                                                "Less"_("l_shipdate"_, params.q6DateUpper),
                                                "Greater"_("l_shipdate"_, params.q6DateLower))),
                               "As"_("Multiply"_("l_extendedprice"_, "l_discount"_), "revenue"_)),
                    "Sum"_("revenue"_));
}

// ---------------------------------------------------------------- Q7
// Revenue moving between two nations, split by year
boss::Expression q7Chain() {
  auto join1 = "Project"_(
      "Join"_("Filter"_(byName("lineitem"), "And"_("GreaterEqual"_("l_shipdate"_, "1995-01-01"),
                                                   "LessEqual"_("l_shipdate"_, "1996-12-31"))),
              byName("supplier"), "Equal"_("l_suppkey"_, "s_suppkey"_)),
      "l_orderkey"_, "l_extendedprice"_, "l_discount"_, "l_shipdate"_, "s_nationkey"_);
  auto join2 = "Project"_(
      "Join"_(byName("orders"), byName("customer"), "Equal"_("o_custkey"_, "c_custkey"_)),
      "o_orderkey"_, "c_nationkey"_);
  return "Join"_(std::move(join1), std::move(join2), "Equal"_("l_orderkey"_, "o_orderkey"_));
}

boss::Expression buildQ7(const QueryParams &params) {
  // Nation tables pre-filtered to the pair (predicate pushdown). Served by
  // Served by the parameter-free chain view (q7Chain) with the nation joins run on top.
  auto nationPair = [&] {
    return "Filter"_(byName("nation"), "Or"_("Equal"_("n_name"_, params.q7Nation1),
                                             "Equal"_("n_name"_, params.q7Nation2)));
  };
  auto join3 = q7Chain();
  auto join4 = "Join"_(std::move(join3),
                       "Project"_(nationPair(), "As"_("n_nationkey"_, "sn_nationkey"_),
                                  "As"_("n_name"_, "supp_nation"_)),
                       "Equal"_("s_nationkey"_, "sn_nationkey"_));
  auto join5 = "Join"_(std::move(join4),
                       "Project"_(nationPair(), "As"_("n_nationkey"_, "cn_nationkey"_),
                                  "As"_("n_name"_, "cust_nation"_)),
                       "Equal"_("c_nationkey"_, "cn_nationkey"_));
  auto filtered =
      "Filter"_(std::move(join5), "Or"_("And"_("Equal"_("supp_nation"_, params.q7Nation1),
                                               "Equal"_("cust_nation"_, params.q7Nation2)),
                                        "And"_("Equal"_("supp_nation"_, params.q7Nation2),
                                               "Equal"_("cust_nation"_, params.q7Nation1))));
  auto projected = "Project"_(std::move(filtered), "supp_nation"_, "cust_nation"_,
                              "As"_("Year"_("Timestamp"_("l_shipdate"_)), "l_year"_),
                              "As"_(revenueExpr(), "volume"_));
  auto grouped = "GroupBy"_(std::move(projected), "Sum"_("volume"_), "supp_nation"_, "cust_nation"_,
                            "l_year"_);
  auto final = "Project"_(std::move(grouped), "supp_nation"_, "cust_nation"_, "l_year"_,
                          "As"_(Symbol("sum(volume)"), "revenue"_));
  return "OrderBy"_(std::move(final), "keys"_("supp_nation"_, "cust_nation"_, "l_year"_));
}

// ---------------------------------------------------------------- Q8
// One nation's share of a part type within its region, by year
boss::Expression q8Join2(const std::string &region) {
  auto custRegion = "Project"_("Join"_("Join"_(byName("customer"), byName("nation"),
                                               "Equal"_("c_nationkey"_, "n_nationkey"_)),
                                       "Filter"_(byName("region"), "Equal"_("r_name"_, region)),
                                       "Equal"_("n_regionkey"_, "r_regionkey"_)),
                               "c_custkey"_);
  auto join1 = "Join"_(
      "Project"_("Filter"_(byName("orders"), "And"_("GreaterEqual"_("o_orderdate"_, "1995-01-01"),
                                                    "LessEqual"_("o_orderdate"_, "1996-12-31"))),
                 "o_orderkey"_, "o_custkey"_, "o_orderdate"_),
      std::move(custRegion), "Equal"_("o_custkey"_, "c_custkey"_));
  return "Join"_(std::move(join1),
                 "Project"_(byName("lineitem"), "l_orderkey"_, "l_partkey"_, "l_suppkey"_,
                            "l_extendedprice"_, "l_discount"_),
                 "Equal"_("o_orderkey"_, "l_orderkey"_));
}

boss::Expression buildQ8(const QueryParams &params) {
  auto suppNation = "Project"_(byName("nation"), "As"_("n_nationkey"_, "sn_nationkey"_),
                               "As"_("n_name"_, "supp_nation"_));
  auto join3 = "Join"_(q8Join2(params.q8Region),
                       "Filter"_(byName("part"), "Equal"_("p_type"_, params.q8PartType)),
                       "Equal"_("l_partkey"_, "p_partkey"_));
  auto join4 = "Join"_(std::move(join3), byName("supplier"), "Equal"_("l_suppkey"_, "s_suppkey"_));
  auto join5 =
      "Join"_(std::move(join4), std::move(suppNation), "Equal"_("s_nationkey"_, "sn_nationkey"_));
  auto projected = "Project"_(
      std::move(join5), "As"_("Year"_("Timestamp"_("o_orderdate"_)), "o_year"_), "supp_nation"_,
      "As"_("IfElse"_("Equal"_("supp_nation"_, params.q8Nation), revenueExpr(), 0.0),
            "nation_volume"_),
      "As"_(revenueExpr(), "volume"_));
  auto grouped =
      "GroupBy"_(std::move(projected), "Sum"_("nation_volume"_), "Sum"_("volume"_), "o_year"_);
  return "Project"_(
      "OrderBy"_(std::move(grouped), "keys"_("o_year"_)), "o_year"_,
      "As"_("Divide"_(Symbol("sum(nation_volume)"), Symbol("sum(volume)")), "mkt_share"_));
}

// ---------------------------------------------------------------- Q9
// Keeps BOSSKernelBenchmarks' p_retailprice range predicate instead of the spec's p_name LIKE.
boss::Expression q9Chain(double priceLow, double priceHigh, bool keepPrice) {
  boss::Expression partFiltered =
      keepPrice
          ? "Project"_("Filter"_(byName("part"), "And"_("Greater"_("p_retailprice"_, priceLow),
                                                        "Less"_("p_retailprice"_, priceHigh))),
                       "p_partkey"_, "p_retailprice"_)
          : "Project"_("Filter"_(byName("part"), "And"_("Greater"_("p_retailprice"_, priceLow),
                                                        "Less"_("p_retailprice"_, priceHigh))),
                       "p_partkey"_);
  auto nationSupplier =
      "Join"_(byName("nation"), byName("supplier"), "Equal"_("n_nationkey"_, "s_nationkey"_));
  auto withPartsupp = "Project"_(
      "Join"_(std::move(nationSupplier), byName("partsupp"), "Equal"_("s_suppkey"_, "ps_suppkey"_)),
      "n_name"_, "ps_partkey"_, "ps_suppkey"_, "ps_supplycost"_);
  auto partNsp = "Join"_(std::move(partFiltered), std::move(withPartsupp),
                         "Equal"_("p_partkey"_, "ps_partkey"_));
  auto lineJoin =
      "Join"_(std::move(partNsp), byName("lineitem"), "Equal"_("ps_partkey"_, "l_partkey"_),
              "Equal"_("ps_suppkey"_, "l_suppkey"_));
  boss::Expression withLineitem =
      keepPrice ? "Project"_(std::move(lineJoin), "n_name"_, "p_retailprice"_, "ps_supplycost"_,
                             "l_orderkey"_, "l_extendedprice"_, "l_discount"_, "l_quantity"_)
                : "Project"_(std::move(lineJoin), "n_name"_, "ps_supplycost"_, "l_orderkey"_,
                             "l_extendedprice"_, "l_discount"_, "l_quantity"_);
  return "Join"_(std::move(withLineitem),
                 "Project"_(byName("orders"), "o_orderkey"_, "o_orderdate"_),
                 "Equal"_("l_orderkey"_, "o_orderkey"_));
}

boss::Expression buildQ9(const QueryParams &params) {
  auto projected = "Project"_(
      q9Chain(params.q9PriceLow, params.q9PriceHigh, false), "As"_("n_name"_, "nation_name"_),
      "As"_("Year"_("Timestamp"_("o_orderdate"_)), "o_year"_),
      "As"_("Subtract"_(revenueExpr(), "Multiply"_("ps_supplycost"_, "l_quantity"_)), "amount"_));
  auto grouped = "GroupBy"_(std::move(projected), "Sum"_("amount"_), "nation_name"_, "o_year"_);
  auto final = "Project"_(std::move(grouped), "nation_name"_, "o_year"_,
                          "As"_(Symbol("sum(amount)"), "amount"_));
  return "OrderBy"_(std::move(final), "keys"_("nation_name"_, "Desc"_("o_year"_)));
}

// ---------------------------------------------------------------- Q10
// Customers who returned parts in a quarter, ranked by the revenue lost
boss::Expression q10BaseJoin(const std::string &dateLo, const std::string &dateHi) {
  auto join1 =
      "Join"_("Project"_("Filter"_(byName("orders"), "And"_("GreaterEqual"_("o_orderdate"_, dateLo),
                                                            "Less"_("o_orderdate"_, dateHi))),
                         "o_orderkey"_, "o_custkey"_, "o_orderdate"_),
              "Project"_(byName("customer"), "c_custkey"_, "c_name"_, "c_acctbal"_, "c_address"_,
                         "c_phone"_, "c_comment"_, "c_nationkey"_),
              "Equal"_("o_custkey"_, "c_custkey"_));
  auto join2 =
      "Join"_(std::move(join1),
              "Project"_("Filter"_(byName("lineitem"), "Equal"_("l_returnflag"_, std::string("R"))),
                         "l_orderkey"_, "l_extendedprice"_, "l_discount"_),
              "Equal"_("o_orderkey"_, "l_orderkey"_));
  return "Join"_(std::move(join2), "Project"_(byName("nation"), "n_nationkey"_, "n_name"_),
                 "Equal"_("c_nationkey"_, "n_nationkey"_));
}

boss::Expression buildQ10(const QueryParams &params) {
  auto projected = "Project"_(q10BaseJoin(params.q10DateLo, params.q10DateHi), "c_custkey"_,
                              "c_name"_, "c_acctbal"_, "c_address"_, "c_phone"_, "c_comment"_,
                              "n_name"_, "As"_(revenueExpr(), "disc_price"_));
  auto grouped = "GroupBy"_(std::move(projected), "Sum"_("disc_price"_), "c_custkey"_, "c_name"_,
                            "c_acctbal"_, "c_address"_, "c_phone"_, "c_comment"_, "n_name"_);
  auto final = "Project"_(std::move(grouped), "c_custkey"_, "c_name"_,
                          "As"_(Symbol("sum(disc_price)"), "revenue"_), "c_acctbal"_, "n_name"_,
                          "c_address"_, "c_phone"_, "c_comment"_);
  return "Slice"_("OrderBy"_(std::move(final), "keys"_("Desc"_("revenue"_))), 0, 20);
}

// ---------------------------------------------------------------- Q11
// The HAVING threshold is a global scalar: computed by a first round trip (bossq::q11Threshold),
// then plugged in as a literal. The query clock covers both round trips.
boss::Expression q11BaseJoin(const std::string &nation) {
  return "Project"_(
      "Join"_(byName("partsupp"),
              "Join"_(byName("supplier"), "Filter"_(byName("nation"), "Equal"_("n_name"_, nation)),
                      "Equal"_("s_nationkey"_, "n_nationkey"_)),
              "Equal"_("ps_suppkey"_, "s_suppkey"_)),
      "ps_partkey"_, "As"_("Multiply"_("ps_supplycost"_, "ps_availqty"_), "value"_));
}

boss::Expression buildQ11(const QueryParams &params) {
  double threshold = bossq::q11Threshold(params);
  auto grouped = "GroupBy"_(q11BaseJoin(params.q11Nation), "Sum"_("value"_), "ps_partkey"_);
  auto filtered = "Filter"_(std::move(grouped), "Greater"_(Symbol("sum(value)"), threshold));
  auto projected =
      "Project"_(std::move(filtered), "ps_partkey"_, "As"_(Symbol("sum(value)"), "value"_));
  return "OrderBy"_(std::move(projected), "keys"_("Desc"_("value"_)));
}

// ---------------------------------------------------------------- Q12
// Late orders split by whether their priority was urgent
boss::Expression q12YearJoin(const std::string &dateLo, const std::string &dateHi) {
  auto filteredLineitem =
      "Filter"_(byName("lineitem"), "And"_("Less"_("l_commitdate"_, "l_receiptdate"_),
                                           "And"_("Less"_("l_shipdate"_, "l_commitdate"_),
                                                  "And"_("GreaterEqual"_("l_receiptdate"_, dateLo),
                                                         "Less"_("l_receiptdate"_, dateHi)))));
  return "Join"_(byName("orders"), std::move(filteredLineitem),
                 "Equal"_("o_orderkey"_, "l_orderkey"_));
}

boss::Expression q12StackedJoin(const std::string &dateLo, const std::string &dateHi,
                                const std::string &mode1, const std::string &mode2) {
  auto numericFiltered =
      "Filter"_(byName("lineitem"), "And"_("Less"_("l_commitdate"_, "l_receiptdate"_),
                                           "And"_("Less"_("l_shipdate"_, "l_commitdate"_),
                                                  "And"_("GreaterEqual"_("l_receiptdate"_, dateLo),
                                                         "Less"_("l_receiptdate"_, dateHi)))));
  auto modeFiltered = "Filter"_(std::move(numericFiltered), "Or"_("Equal"_("l_shipmode"_, mode1),
                                                                  "Equal"_("l_shipmode"_, mode2)));
  return "Join"_(byName("orders"), std::move(modeFiltered), "Equal"_("o_orderkey"_, "l_orderkey"_));
}

boss::Expression buildQ12(const QueryParams &params) {
  // Stacked shape: the categorical mode filter runs below the join on the numeric filter's
  // small output (fastest plain plan); the year views (q12YearJoin) still serve it via a
  // mode residual, since view compatibility doesn't need the mode lifted above the join.
  auto filtered =
      q12StackedJoin(params.q12DateLo, params.q12DateHi, params.q12Mode1, params.q12Mode2);
  auto isHigh = [] {
    return "Or"_("Equal"_("o_orderpriority"_, std::string("1-URGENT")),
                 "Equal"_("o_orderpriority"_, std::string("2-HIGH")));
  };
  auto projected =
      "Project"_(std::move(filtered), "l_shipmode"_, "As"_("IfElse"_(isHigh(), 1, 0), "high_flag"_),
                 "As"_("IfElse"_(isHigh(), 0, 1), "low_flag"_));
  auto grouped =
      "GroupBy"_(std::move(projected), "Sum"_("high_flag"_), "Sum"_("low_flag"_), "l_shipmode"_);
  auto final = "Project"_(std::move(grouped), "l_shipmode"_,
                          "As"_(Symbol("sum(high_flag)"), "high_line_count"_),
                          "As"_(Symbol("sum(low_flag)"), "low_line_count"_));
  return "OrderBy"_(std::move(final), "keys"_("l_shipmode"_));
}

// ---------------------------------------------------------------- Q13
// How many customers have how many orders, with one comment pattern excluded
boss::Expression q13BaseJoin() {
  return "LeftJoin"_(byName("customer"), byName("orders"), "Equal"_("c_custkey"_, "o_custkey"_));
}

boss::Expression q13Adjusted(const std::string &word1, const std::string &word2) {
  std::string pattern = "%" + word1 + "%" + word2 + "%";
  return "Project"_(q13BaseJoin(), "c_custkey"_,
                    "As"_("IfElse"_("IsValid"_("o_orderkey"_),
                                    "IfElse"_("Like"_("o_comment"_, pattern), 0, 1), 0),
                          "qualifies"_));
}

// The original form: the NOT LIKE predicate sits on the null-extending side of the left
// join, behind the rewriter's representability wall, so no view can serve this tree. Q13R
// below is the rewrite the view experiments run instead.
boss::Expression buildQ13(const QueryParams &params) {
  std::string pattern = "%" + params.q13Word1 + "%" + params.q13Word2 + "%";
  auto joined = "LeftJoin"_(byName("customer"),
                            "Filter"_(byName("orders"), "Not"_("Like"_("o_comment"_, pattern))),
                            "Equal"_("c_custkey"_, "o_custkey"_));
  auto flagged = "Project"_(std::move(joined), "c_custkey"_,
                            "As"_("IfElse"_("IsValid"_("o_orderkey"_), 1, 0), "qualifies"_));
  auto grouped1 = "GroupBy"_(std::move(flagged), "Sum"_("qualifies"_), "c_custkey"_);
  auto projected = "Project"_(std::move(grouped1), "As"_(Symbol("sum(qualifies)"), "c_count"_));
  auto grouped2 = "GroupBy"_(std::move(projected), "CountAll"_(), "c_count"_);
  auto final =
      "Project"_(std::move(grouped2), "c_count"_, "As"_(Symbol("count_all()"), "custdist"_));
  return "OrderBy"_(std::move(final), "keys"_("Desc"_("custdist"_), "Desc"_("c_count"_)));
}

// Q13 rewritten to be servable: the predicate becomes part of the qualifies flag, computed
// above the join, and the aggregation counts the flags.
boss::Expression buildQ13R(const QueryParams &params) {
  auto grouped1 =
      "GroupBy"_(q13Adjusted(params.q13Word1, params.q13Word2), "Sum"_("qualifies"_), "c_custkey"_);
  auto projected = "Project"_(std::move(grouped1), "As"_(Symbol("sum(qualifies)"), "c_count"_));
  auto grouped2 = "GroupBy"_(std::move(projected), "CountAll"_(), "c_count"_);
  auto final =
      "Project"_(std::move(grouped2), "c_count"_, "As"_(Symbol("count_all()"), "custdist"_));
  return "OrderBy"_(std::move(final), "keys"_("Desc"_("custdist"_), "Desc"_("c_count"_)));
}

// ---------------------------------------------------------------- Q14
// What share of a month's revenue came from promotional parts
boss::Expression q14BaseJoin(const std::string &dateLo, const std::string &dateHi) {
  auto filteredLineitem =
      "Project"_("Filter"_(byName("lineitem"), "And"_("GreaterEqual"_("l_shipdate"_, dateLo),
                                                      "Less"_("l_shipdate"_, dateHi))),
                 "l_partkey"_, "l_extendedprice"_, "l_discount"_, "l_shipdate"_);
  return "Join"_(std::move(filteredLineitem), "Project"_(byName("part"), "p_partkey"_, "p_type"_),
                 "Equal"_("l_partkey"_, "p_partkey"_));
}

boss::Expression buildQ14(const QueryParams &params) {
  auto projected =
      "Project"_(q14BaseJoin(params.q14DateLo, params.q14DateHi),
                 "As"_("IfElse"_("Like"_("p_type"_, std::string("PROMO%")), revenueExpr(), 0.0),
                       "promo_disc_price"_),
                 "As"_(revenueExpr(), "disc_price"_));
  auto grouped =
      "GroupBy"_(std::move(projected), "Sum"_("promo_disc_price"_), "Sum"_("disc_price"_));
  return "Project"_(std::move(grouped),
                    "As"_("Multiply"_(100.0, "Divide"_(Symbol("sum(promo_disc_price)"),
                                                       Symbol("sum(disc_price)"))),
                          "promo_revenue"_));
}

// ---------------------------------------------------------------- Q15
// The supplier with the highest revenue in a quarter
boss::Expression q15Revenue(const std::string &dateLo, const std::string &dateHi) {
  auto windowed = "Filter"_(byName("lineitem"), "And"_("GreaterEqual"_("l_shipdate"_, dateLo),
                                                       "Less"_("l_shipdate"_, dateHi)));
  auto projected = "Project"_(std::move(windowed), "l_suppkey"_, "As"_(revenueExpr(), "rev"_));
  return "Project"_("GroupBy"_(std::move(projected), "Sum"_("rev"_), "l_suppkey"_), "l_suppkey"_,
                    Symbol("sum(rev)"));
}

boss::Expression q15TopSuppliers(const std::string &dateLo, const std::string &dateHi) {
  auto maxRevenue = "GroupBy"_(q15Revenue(dateLo, dateHi), "Max"_(Symbol("sum(rev)")));
  auto topSuppliers = "Join"_(q15Revenue(dateLo, dateHi), std::move(maxRevenue),
                              "Equal"_(Symbol("sum(rev)"), Symbol("max(sum(rev))")));
  auto joined =
      "Join"_(byName("supplier"), std::move(topSuppliers), "Equal"_("s_suppkey"_, "l_suppkey"_));
  return "Project"_(std::move(joined), "s_suppkey"_, "s_name"_, "s_address"_, "s_phone"_,
                    "As"_(Symbol("sum(rev)"), "total_revenue"_));
}

boss::Expression buildQ15(const QueryParams &params) {
  return "OrderBy"_(q15TopSuppliers(params.q15DateLo, params.q15DateHi), "keys"_("s_suppkey"_));
}

// ---------------------------------------------------------------- Q16
// How many suppliers can supply each part, with complaint suppliers excluded
boss::Expression q16ComplaintSupps() {
  return "Project"_(
      "Filter"_(byName("supplier"), "Like"_("s_comment"_, std::string("%Customer%Complaints%"))),
      "s_suppkey"_);
}

boss::Expression buildQ16(const QueryParams &params) {
  boss::Expression sizeCond = "Equal"_("p_size"_, params.q16Sizes.at(7));
  for (int i = 6; i >= 0; --i)
    sizeCond = "Or"_("Equal"_("p_size"_, params.q16Sizes.at(i)), std::move(sizeCond));
  auto filteredPart = "Filter"_(
      byName("part"),
      "And"_("Not"_("Equal"_("p_brand"_, params.q16Brand)),
             "And"_("Not"_("Like"_("p_type"_, params.q16TypeNotLike)), std::move(sizeCond))));
  auto joined =
      "Join"_(std::move(filteredPart), byName("partsupp"), "Equal"_("p_partkey"_, "ps_partkey"_));
  auto antiJoined =
      "AntiJoin"_(std::move(joined), q16ComplaintSupps(), "Equal"_("ps_suppkey"_, "s_suppkey"_));
  auto perSupplierPerGroup = "Project"_("GroupBy"_(std::move(antiJoined), "CountAll"_(), "p_brand"_,
                                                   "p_type"_, "p_size"_, "ps_suppkey"_),
                                        "p_brand"_, "p_type"_, "p_size"_);
  auto grouped =
      "GroupBy"_(std::move(perSupplierPerGroup), "CountAll"_(), "p_brand"_, "p_type"_, "p_size"_);
  auto final = "Project"_(std::move(grouped), "p_brand"_, "p_type"_, "p_size"_,
                          "As"_(Symbol("count_all()"), "supplier_cnt"_));
  return "OrderBy"_(std::move(final),
                    "keys"_("Desc"_("supplier_cnt"_), "p_brand"_, "p_type"_, "p_size"_));
}

// ---------------------------------------------------------------- Q17
// Lineitems well below the average quantity for their own part
boss::Expression q17PartAvg() {
  return "Project"_("GroupBy"_(byName("lineitem"), "Avg"_("l_quantity"_), "l_partkey"_),
                    "As"_("l_partkey"_, "ap_partkey"_),
                    "As"_(Symbol("mean(l_quantity)"), "avg_qty"_));
}

boss::Expression buildQ17(const QueryParams &params) {
  auto targetParts =
      "Filter"_(byName("part"), "And"_("Equal"_("p_brand"_, params.q17Brand),
                                       "Equal"_("p_container"_, params.q17Container)));
  auto joined = "Join"_(
      "Join"_(byName("lineitem"), std::move(targetParts), "Equal"_("l_partkey"_, "p_partkey"_)),
      q17PartAvg(), "Equal"_("l_partkey"_, "ap_partkey"_));
  auto filtered =
      "Filter"_(std::move(joined), "Less"_("l_quantity"_, "Multiply"_(0.2, "avg_qty"_)));
  auto grouped = "GroupBy"_(std::move(filtered), "Sum"_("l_extendedprice"_));
  return "Project"_(std::move(grouped),
                    "As"_("Divide"_(Symbol("sum(l_extendedprice)"), 7.0), "avg_yearly"_));
}

// ---------------------------------------------------------------- Q18
// Orders whose total quantity crosses a threshold, with the customer attached
boss::Expression q18HeavyOrders(int quantity) {
  return "Filter"_("GroupBy"_(byName("lineitem"), "Sum"_("l_quantity"_), "l_orderkey"_),
                   "Greater"_(Symbol("sum(l_quantity)"), quantity));
}

boss::Expression q18Joined(int quantityThreshold) {
  auto heavy =
      "Project"_(q18HeavyOrders(quantityThreshold), "l_orderkey"_, Symbol("sum(l_quantity)"));
  auto custOrders = "Project"_(
      "Join"_(byName("customer"), byName("orders"), "Equal"_("c_custkey"_, "o_custkey"_)),
      "o_orderkey"_, "o_custkey"_, "o_orderdate"_, "o_totalprice"_);
  return "Join"_(std::move(heavy), std::move(custOrders), "Equal"_("l_orderkey"_, "o_orderkey"_));
}

boss::Expression buildQ18(const QueryParams &params) {
  auto grouped = "GroupBy"_(q18Joined(params.q18Quantity), "Sum"_(Symbol("sum(l_quantity)")),
                            "o_orderkey"_, "o_orderdate"_, "o_totalprice"_, "o_custkey"_);
  auto final = "Project"_(std::move(grouped), "o_orderkey"_, "o_orderdate"_, "o_totalprice"_,
                          "o_custkey"_, "As"_(Symbol("sum(sum(l_quantity))"), "sum_l_quantity"_));
  return "Slice"_("OrderBy"_(std::move(final), "keys"_("Desc"_("o_totalprice"_), "o_orderdate"_)),
                  0, 100);
}

// ---------------------------------------------------------------- Q19
// Three brand / container / quantity branches OR'd together
boss::Expression q19SharedJoin() {
  auto shipFiltered = "Filter"_(
      byName("lineitem"), "And"_("Equal"_("l_shipinstruct"_, std::string("DELIVER IN PERSON")),
                                 "Or"_("Equal"_("l_shipmode"_, std::string("AIR")),
                                       "Equal"_("l_shipmode"_, std::string("AIR REG")))));
  return "Join"_(std::move(shipFiltered), byName("part"), "Equal"_("l_partkey"_, "p_partkey"_));
}

boss::Expression buildQ19(const QueryParams &params) {
  auto branch = [](const std::string &brand, const char *c1, const char *c2, const char *c3,
                   const char *c4, double qty, int sizeHi) {
    return "And"_("Equal"_("p_brand"_, brand),
                  "And"_("Or"_("Equal"_("p_container"_, std::string(c1)),
                               "Or"_("Equal"_("p_container"_, std::string(c2)),
                                     "Or"_("Equal"_("p_container"_, std::string(c3)),
                                           "Equal"_("p_container"_, std::string(c4))))),
                         "And"_("GreaterEqual"_("l_quantity"_, qty),
                                "And"_("LessEqual"_("l_quantity"_, qty + 10.0),
                                       "And"_("GreaterEqual"_("p_size"_, 1),
                                              "LessEqual"_("p_size"_, sizeHi))))));
  };
  auto cond = "Or"_(
      branch(params.q19Brand1, "SM CASE", "SM BOX", "SM PACK", "SM PKG", params.q19Qty1, 5),
      "Or"_(
          branch(params.q19Brand2, "MED BAG", "MED BOX", "MED PKG", "MED PACK", params.q19Qty2, 10),
          branch(params.q19Brand3, "LG CASE", "LG BOX", "LG PKG", "LG PACK", params.q19Qty3, 15)));
  auto projected =
      "Project"_("Filter"_(q19SharedJoin(), std::move(cond)), "As"_(revenueExpr(), "revenue"_));
  return "Project"_("GroupBy"_(std::move(projected), "Sum"_("revenue"_)),
                    "As"_(Symbol("sum(revenue)"), "revenue"_));
}

// ---------------------------------------------------------------- Q20
// Suppliers holding excess stock of parts whose name starts with a given colour
boss::Expression q20LineitemQty(const std::string &dateLo, const std::string &dateHi) {
  auto grouped =
      "GroupBy"_("Filter"_(byName("lineitem"), "And"_("GreaterEqual"_("l_shipdate"_, dateLo),
                                                      "Less"_("l_shipdate"_, dateHi))),
                 "Sum"_("l_quantity"_), "l_partkey"_, "l_suppkey"_);
  return "Project"_(std::move(grouped), "l_partkey"_, "l_suppkey"_, Symbol("sum(l_quantity)"));
}

boss::Expression buildQ20(const QueryParams &params) {
  auto forestParts = "Filter"_(byName("part"), "Like"_("p_name"_, params.q20PartPrefix + "%"));
  auto qualifyingSupps =
      "Project"_("Filter"_("Join"_("Join"_(byName("partsupp"),
                                           q20LineitemQty(params.q20DateLo, params.q20DateHi),
                                           "Equal"_("ps_partkey"_, "l_partkey"_),
                                           "Equal"_("ps_suppkey"_, "l_suppkey"_)),
                                   std::move(forestParts), "Equal"_("ps_partkey"_, "p_partkey"_)),
                           "Greater"_("ps_availqty"_, "Multiply"_(0.5, Symbol("sum(l_quantity)")))),
                 "ps_suppkey"_);
  auto dedupedSupps = "Project"_(
      "GroupBy"_(std::move(qualifyingSupps), "CountAll"_(), "ps_suppkey"_), "ps_suppkey"_);
  auto joined = "Join"_("Join"_("Filter"_(byName("nation"), "Equal"_("n_name"_, params.q20Nation)),
                                byName("supplier"), "Equal"_("n_nationkey"_, "s_nationkey"_)),
                        std::move(dedupedSupps), "Equal"_("s_suppkey"_, "ps_suppkey"_));
  return "OrderBy"_("Project"_(std::move(joined), "s_name"_, "s_address"_), "keys"_("s_name"_));
}

// ---------------------------------------------------------------- Q21
// Suppliers who were the only one late on a multi-supplier order
boss::Expression lateLi() {
  return "Filter"_(byName("lineitem"), "Greater"_("l_receiptdate"_, "l_commitdate"_));
}

boss::Expression q21MultiSuppOrders() {
  return "Filter"_(
      "GroupBy"_(byName("lineitem"), "Min"_("l_suppkey"_), "Max"_("l_suppkey"_), "l_orderkey"_),
      "Not"_("Equal"_(Symbol("min(l_suppkey)"), Symbol("max(l_suppkey)"))));
}

boss::Expression q21MultiLateOrders() {
  return "Filter"_("GroupBy"_(lateLi(), "Min"_("l_suppkey"_), "Max"_("l_suppkey"_), "l_orderkey"_),
                   "Not"_("Equal"_(Symbol("min(l_suppkey)"), Symbol("max(l_suppkey)"))));
}

boss::Expression q21AntiJoined(const std::string &nation) {
  auto lateJoinMulti =
      "Join"_(lateLi(), q21MultiSuppOrders(), "Equal"_("l_orderkey"_, "l_orderkey"_));
  auto renamed = "Project"_(std::move(lateJoinMulti), "As"_(Symbol("l_orderkey_l"), "l_orderkey"_),
                            "l_suppkey"_);
  auto withOrders = "Join"_(
      std::move(renamed), "Filter"_(byName("orders"), "Equal"_("o_orderstatus"_, std::string("F"))),
      "Equal"_("l_orderkey"_, "o_orderkey"_));
  auto suppNation =
      "Join"_(byName("supplier"), "Filter"_(byName("nation"), "Equal"_("n_name"_, nation)),
              "Equal"_("s_nationkey"_, "n_nationkey"_));
  auto withSupplier =
      "Join"_(std::move(withOrders), std::move(suppNation), "Equal"_("l_suppkey"_, "s_suppkey"_));
  return "AntiJoin"_(std::move(withSupplier), q21MultiLateOrders(),
                     "Equal"_("l_orderkey"_, "l_orderkey"_));
}

boss::Expression buildQ21(const QueryParams &params) {
  auto grouped = "GroupBy"_(q21AntiJoined(params.q21Nation), "CountAll"_(), "s_name"_);
  auto final = "Project"_(std::move(grouped), "s_name"_, "As"_(Symbol("count_all()"), "numwait"_));
  return "Slice"_("OrderBy"_(std::move(final), "keys"_("Desc"_("numwait"_), "s_name"_)), 0, 100);
}

// ---------------------------------------------------------------- Q22
// Customers with an above-average balance who have never ordered
boss::Expression q22WithCode() {
  return "Project"_(byName("customer"), "c_custkey"_, "c_acctbal"_,
                    "As"_("Substring"_("c_phone"_, 0, 2), "cntrycode"_));
}

boss::Expression q22NoOrders() {
  return "AntiJoin"_(q22WithCode(), byName("orders"), "Equal"_("c_custkey"_, "o_custkey"_));
}

boss::Expression q22CountryFilter(const std::vector<std::string> &codes) {
  boss::ExpressionArguments orArgs;
  for (auto const &code : codes)
    orArgs.emplace_back("Equal"_("cntrycode"_, code));
  return boss::ComplexExpression("Or"_, {}, std::move(orArgs), {});
}

boss::Expression buildQ22(const QueryParams &params) {
  // Code filter below the anti-join. The parameter-free anti-join view
  // serves it cross-shape with a code residual.
  auto inScope = "AntiJoin"_("Filter"_(q22WithCode(), q22CountryFilter(params.q22CountryCodes)),
                             byName("orders"), "Equal"_("c_custkey"_, "o_custkey"_));
  auto avgBalance =
      "GroupBy"_("Filter"_(q22WithCode(), "And"_("Greater"_("c_acctbal"_, 0.0),
                                                 q22CountryFilter(params.q22CountryCodes))),
                 "Avg"_("c_acctbal"_));
  auto aboveAvg = "Join"_(std::move(inScope), std::move(avgBalance),
                          "Greater"_("c_acctbal"_, Symbol("mean(c_acctbal)")));
  auto grouped = "GroupBy"_("Project"_(std::move(aboveAvg), "cntrycode"_, "c_acctbal"_),
                            "CountAll"_(), "Sum"_("c_acctbal"_), "cntrycode"_);
  auto projected =
      "Project"_(std::move(grouped), "cntrycode"_, "As"_(Symbol("count_all()"), "numcust"_),
                 "As"_(Symbol("sum(c_acctbal)"), "totacctbal"_));
  return "OrderBy"_(std::move(projected), "keys"_("cntrycode"_));
}

boss::Expression bossQuery(const std::string &name, const QueryParams &params) {
  if (name == "Q1")
    return buildQ1(params);
  if (name == "Q2")
    return buildQ2(params);
  if (name == "Q3")
    return buildQ3(params);
  if (name == "Q4")
    return buildQ4(params);
  if (name == "Q5")
    return buildQ5(params);
  if (name == "Q6")
    return buildQ6(params);
  if (name == "Q7")
    return buildQ7(params);
  if (name == "Q8")
    return buildQ8(params);
  if (name == "Q9")
    return buildQ9(params);
  if (name == "Q10")
    return buildQ10(params);
  if (name == "Q11")
    return buildQ11(params);
  if (name == "Q12")
    return buildQ12(params);
  if (name == "Q13")
    return buildQ13(params);
  if (name == "Q13R")
    return buildQ13R(params);
  if (name == "Q14")
    return buildQ14(params);
  if (name == "Q15")
    return buildQ15(params);
  if (name == "Q16")
    return buildQ16(params);
  if (name == "Q17")
    return buildQ17(params);
  if (name == "Q18")
    return buildQ18(params);
  if (name == "Q19")
    return buildQ19(params);
  if (name == "Q20")
    return buildQ20(params);
  if (name == "Q21")
    return buildQ21(params);
  if (name == "Q22")
    return buildQ22(params);
  throw std::runtime_error("unknown query: " + name);
}

std::string checksumColumn(const std::string &name) {
  if (name == "Q1")
    return "sum_base_price";
  if (name == "Q2")
    return "s_acctbal";
  if (name == "Q3")
    return "revenue";
  if (name == "Q4")
    return "order_count";
  if (name == "Q5")
    return "revenue";
  if (name == "Q6")
    return "sum(revenue)";
  if (name == "Q7")
    return "revenue";
  if (name == "Q8")
    return "mkt_share";
  if (name == "Q9")
    return "amount";
  if (name == "Q10")
    return "revenue";
  if (name == "Q11")
    return "value";
  if (name == "Q12")
    return "high_line_count";
  if (name == "Q13" || name == "Q13R")
    return "custdist";
  if (name == "Q14")
    return "promo_revenue";
  if (name == "Q15")
    return "total_revenue";
  if (name == "Q16")
    return "supplier_cnt";
  if (name == "Q17")
    return "avg_yearly";
  if (name == "Q18")
    return "sum_l_quantity";
  if (name == "Q19")
    return "revenue";
  if (name == "Q20")
    return "s_address";
  if (name == "Q21")
    return "numwait";
  if (name == "Q22")
    return "totacctbal";
  return "";
}

// ================================================================ SQL (DuckDB / MonetDB dialect)
namespace {

std::string num(double value) {
  std::ostringstream os;
  os << std::setprecision(10) << value;
  return os.str();
}

std::string sqlQ1(const QueryParams &params) {
  return "SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty, SUM(l_extendedprice) AS "
         "sum_base_price,"
         " SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,"
         " SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, AVG(l_quantity) AS "
         "avg_qty,"
         " AVG(l_extendedprice) AS avg_price, AVG(l_discount) AS avg_disc, COUNT(*) AS count_order"
         " FROM lineitem WHERE l_shipdate <= '" +
         params.q1ShipdateBound +
         "'"
         " GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus;";
}

std::string sqlQ2(const QueryParams &params) {
  return "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment"
         " FROM part, supplier, partsupp, nation, region"
         " WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = " +
         std::to_string(params.q2Size) + " AND p_type LIKE '" + params.q2TypeLike +
         "' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey"
         " AND r_name = '" +
         params.q2Region +
         "' AND ps_supplycost = (SELECT MIN(ps_supplycost)"
         " FROM partsupp, supplier, nation, region WHERE p_partkey = ps_partkey AND s_suppkey = "
         "ps_suppkey"
         " AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = '" +
         params.q2Region +
         "')"
         " ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100;";
}

std::string sqlQ3(const QueryParams &params) {
  return "SELECT l_orderkey, o_orderdate, o_shippriority, SUM(l_extendedprice * (1 - l_discount)) "
         "AS revenue"
         " FROM customer, orders, lineitem WHERE c_mktsegment = '" +
         params.q3Segment +
         "'"
         " AND c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < DATE '" +
         params.q3Date +
         "'"
         " AND l_shipdate > DATE '" +
         params.q3Date +
         "' GROUP BY l_orderkey, o_orderdate, o_shippriority"
         " ORDER BY revenue DESC, o_orderdate LIMIT 10;";
}

std::string sqlQ4(const QueryParams &params) {
  return "SELECT o_orderpriority, COUNT(*) AS order_count FROM orders"
         " WHERE o_orderdate >= '" +
         params.q4DateLo + "' AND o_orderdate < '" + params.q4DateHi +
         "'"
         " AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < "
         "l_receiptdate)"
         " GROUP BY o_orderpriority ORDER BY o_orderpriority;";
}

std::string sqlQ5(const QueryParams &params) {
  return "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue"
         " FROM customer, orders, lineitem, supplier, nation, region"
         " WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey"
         " AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = "
         "r_regionkey"
         " AND r_name = '" +
         params.q5Region + "' AND o_orderdate >= '" + params.q5DateLo +
         "'"
         " AND o_orderdate < '" +
         params.q5DateHi + "' GROUP BY n_name ORDER BY revenue DESC;";
}

std::string sqlQ6(const QueryParams &params) {
  return "SELECT SUM(l_extendedprice * l_discount) AS revenue FROM lineitem"
         " WHERE l_shipdate >= DATE '" +
         params.q6ShipLo + "' AND l_shipdate < DATE '" + params.q6DateUpper +
         "'"
         " AND l_discount BETWEEN " +
         num(params.q6DiscLo) + " AND " + num(params.q6DiscHi) + " AND l_quantity < " +
         std::to_string(params.q6Quantity) + ";";
}

std::string sqlQ7(const QueryParams &params) {
  return "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
         " SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, EXTRACT(YEAR FROM "
         "l_shipdate) AS l_year,"
         " l_extendedprice * (1 - l_discount) AS volume FROM supplier, lineitem, orders, customer, "
         "nation n1, nation n2"
         " WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey"
         " AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey"
         " AND ((n1.n_name = '" +
         params.q7Nation1 + "' AND n2.n_name = '" + params.q7Nation2 +
         "')"
         " OR (n1.n_name = '" +
         params.q7Nation2 + "' AND n2.n_name = '" + params.q7Nation1 +
         "'))"
         " AND l_shipdate >= '1995-01-01' AND l_shipdate <= '1996-12-31') AS shipping"
         " GROUP BY supp_nation, cust_nation, l_year ORDER BY supp_nation, cust_nation, l_year;";
}

std::string sqlQ8(const QueryParams &params) {
  return "SELECT o_year, SUM(CASE WHEN nation = '" + params.q8Nation +
         "' THEN volume ELSE 0 END) / SUM(volume) AS mkt_share"
         " FROM (SELECT EXTRACT(YEAR FROM o_orderdate) AS o_year, l_extendedprice * (1 - "
         "l_discount) AS volume,"
         " n2.n_name AS nation FROM part, supplier, lineitem, orders, customer, nation n1, nation "
         "n2, region"
         " WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND "
         "o_custkey = c_custkey"
         " AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey AND r_name = '" +
         params.q8Region +
         "'"
         " AND s_nationkey = n2.n_nationkey AND o_orderdate >= '1995-01-01' AND o_orderdate <= "
         "'1996-12-31'"
         " AND p_type = '" +
         params.q8PartType + "') AS all_nations GROUP BY o_year ORDER BY o_year;";
}

std::string sqlQ9(const QueryParams &params) {
  return "SELECT nation, o_year, SUM(amount) AS sum_profit FROM (SELECT n_name AS nation,"
         " EXTRACT(YEAR FROM o_orderdate) AS o_year, l_extendedprice * (1 - l_discount) - "
         "ps_supplycost * l_quantity AS amount"
         " FROM part, supplier, lineitem, partsupp, orders, nation WHERE s_suppkey = l_suppkey AND "
         "ps_suppkey = l_suppkey"
         " AND ps_partkey = l_partkey AND p_partkey = l_partkey AND o_orderkey = l_orderkey AND "
         "s_nationkey = n_nationkey"
         " AND p_retailprice > " +
         num(params.q9PriceLow) + " AND p_retailprice < " + num(params.q9PriceHigh) +
         ") AS profit"
         " GROUP BY nation, o_year ORDER BY nation, o_year DESC;";
}

std::string sqlQ10(const QueryParams &params) {
  return "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, "
         "n_name,"
         " c_address, c_phone, c_comment FROM customer, orders, lineitem, nation"
         " WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate >= '" +
         params.q10DateLo +
         "'"
         " AND o_orderdate < '" +
         params.q10DateHi +
         "' AND l_returnflag = 'R' AND c_nationkey = n_nationkey"
         " GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment"
         " ORDER BY revenue DESC LIMIT 20;";
}

std::string sqlQ11(const QueryParams &params) {
  return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value FROM partsupp, supplier, "
         "nation"
         " WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = '" +
         params.q11Nation +
         "'"
         " GROUP BY ps_partkey HAVING SUM(ps_supplycost * ps_availqty) > ("
         " SELECT SUM(ps_supplycost * ps_availqty) * " +
         num(params.q11Fraction) +
         " FROM partsupp, supplier, nation"
         " WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = '" +
         params.q11Nation +
         "')"
         " ORDER BY value DESC;";
}

std::string sqlQ12(const QueryParams &params) {
  return "SELECT l_shipmode, SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = "
         "'2-HIGH' THEN 1 ELSE 0 END)"
         " AS high_line_count, SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
         "'2-HIGH' THEN 1 ELSE 0 END)"
         " AS low_line_count FROM orders, lineitem WHERE o_orderkey = l_orderkey"
         " AND l_shipmode IN ('" +
         params.q12Mode1 + "', '" + params.q12Mode2 +
         "') AND l_commitdate < l_receiptdate"
         " AND l_shipdate < l_commitdate AND l_receiptdate >= '" +
         params.q12DateLo + "' AND l_receiptdate < '" + params.q12DateHi +
         "'"
         " GROUP BY l_shipmode ORDER BY l_shipmode;";
}

std::string sqlQ13(const QueryParams &params) {
  return "SELECT c_count, COUNT(*) AS custdist FROM (SELECT c_custkey, COUNT(o_orderkey) AS c_count"
         " FROM customer LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
         "'%" +
         params.q13Word1 + "%" + params.q13Word2 +
         "%' GROUP BY c_custkey) AS c_orders GROUP BY c_count ORDER BY custdist DESC, c_count "
         "DESC;";
}

std::string sqlQ14(const QueryParams &params) {
  return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - "
         "l_discount) ELSE 0 END)"
         " / SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue FROM lineitem, part"
         " WHERE l_partkey = p_partkey AND l_shipdate >= '" +
         params.q14DateLo + "' AND l_shipdate < '" + params.q14DateHi + "';";
}

std::string sqlQ15(const QueryParams &params) {
  return "WITH revenue0 AS (SELECT l_suppkey AS supplier_no, SUM(l_extendedprice * (1 - "
         "l_discount)) AS total_revenue"
         " FROM lineitem WHERE l_shipdate >= '" +
         params.q15DateLo + "' AND l_shipdate < '" + params.q15DateHi +
         "'"
         " GROUP BY l_suppkey) SELECT s_suppkey, s_name, s_address, s_phone, total_revenue FROM "
         "supplier, revenue0"
         " WHERE s_suppkey = supplier_no AND total_revenue = (SELECT MAX(total_revenue) FROM "
         "revenue0) ORDER BY s_suppkey;";
}

std::string sqlQ16(const QueryParams &params) {
  std::string sizes;
  for (size_t i = 0; i < params.q16Sizes.size(); ++i)
    sizes += (i ? ", " : "") + std::to_string(params.q16Sizes[i]);
  return "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt FROM "
         "partsupp, part"
         " WHERE p_partkey = ps_partkey AND p_brand <> '" +
         params.q16Brand + "' AND p_type NOT LIKE '" + params.q16TypeNotLike +
         "'"
         " AND p_size IN (" +
         sizes +
         ") AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier"
         " WHERE s_comment LIKE '%Customer%Complaints%') GROUP BY p_brand, p_type, p_size"
         " ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;";
}

std::string sqlQ17(const QueryParams &params) {
  return "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly FROM lineitem, part WHERE p_partkey = "
         "l_partkey"
         " AND p_brand = '" +
         params.q17Brand + "' AND p_container = '" + params.q17Container +
         "'"
         " AND l_quantity < (SELECT 0.2 * AVG(l_quantity) FROM lineitem WHERE l_partkey = "
         "p_partkey);";
}

std::string sqlQ18(const QueryParams &params) {
  return "SELECT c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity)"
         " FROM customer, orders, lineitem WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem "
         "GROUP BY l_orderkey"
         " HAVING SUM(l_quantity) > " +
         std::to_string(params.q18Quantity) +
         ") AND c_custkey = o_custkey AND o_orderkey = l_orderkey"
         " GROUP BY c_custkey, o_orderkey, o_orderdate, o_totalprice ORDER BY o_totalprice DESC, "
         "o_orderdate LIMIT 100;";
}

std::string sqlQ19(const QueryParams &params) {
  auto branch = [](const std::string &brand, const char *containers, double qty, int sizeHi) {
    return "(p_partkey = l_partkey AND p_brand = '" + brand + "' AND p_container IN (" +
           containers +
           ")"
           " AND l_quantity >= " +
           num(qty) + " AND l_quantity <= " + num(qty + 10.0) + " AND p_size BETWEEN 1 AND " +
           std::to_string(sizeHi) +
           " AND l_shipmode IN ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON')";
  };
  return "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue FROM lineitem, part WHERE " +
         branch(params.q19Brand1, "'SM CASE', 'SM BOX', 'SM PACK', 'SM PKG'", params.q19Qty1, 5) +
         " OR " +
         branch(params.q19Brand2, "'MED BAG', 'MED BOX', 'MED PKG', 'MED PACK'", params.q19Qty2,
                10) +
         " OR " +
         branch(params.q19Brand3, "'LG CASE', 'LG BOX', 'LG PACK', 'LG PKG'", params.q19Qty3, 15) +
         ";";
}

std::string sqlQ20(const QueryParams &params) {
  return "SELECT s_name, s_address FROM supplier, nation WHERE s_suppkey IN (SELECT ps_suppkey "
         "FROM partsupp"
         " WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '" +
         params.q20PartPrefix +
         "%')"
         " AND ps_availqty > (SELECT 0.5 * SUM(l_quantity) FROM lineitem WHERE l_partkey = "
         "ps_partkey"
         " AND l_suppkey = ps_suppkey AND l_shipdate >= '" +
         params.q20DateLo + "' AND l_shipdate < '" + params.q20DateHi +
         "'))"
         " AND s_nationkey = n_nationkey AND n_name = '" +
         params.q20Nation + "' ORDER BY s_name;";
}

std::string sqlQ21(const QueryParams &params) {
  return "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation"
         " WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'"
         " AND l1.l_receiptdate > l1.l_commitdate AND EXISTS (SELECT * FROM lineitem l2"
         " WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)"
         " AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey"
         " AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)"
         " AND s_nationkey = n_nationkey AND n_name = '" +
         params.q21Nation +
         "' GROUP BY s_name"
         " ORDER BY numwait DESC, s_name LIMIT 100;";
}

std::string sqlQ22(const QueryParams &params) {
  std::string codes;
  for (size_t i = 0; i < params.q22CountryCodes.size(); ++i)
    codes += (i ? ",'" : "'") + params.q22CountryCodes[i] + "'";
  std::string code = "SUBSTRING(c_phone FROM 1 FOR 2)";
  return "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal FROM (SELECT " +
         code + " AS cntrycode, c_acctbal FROM customer WHERE " + code + " IN (" + codes +
         ") AND c_acctbal > ("
         "SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 AND " +
         code + " IN (" + codes +
         "))"
         " AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) AS custsale"
         " GROUP BY cntrycode ORDER BY cntrycode;";
}

} // namespace

std::string sqlQuery(const std::string &name, const QueryParams &params) {
  if (name == "Q1")
    return sqlQ1(params);
  if (name == "Q2")
    return sqlQ2(params);
  if (name == "Q3")
    return sqlQ3(params);
  if (name == "Q4")
    return sqlQ4(params);
  if (name == "Q5")
    return sqlQ5(params);
  if (name == "Q6")
    return sqlQ6(params);
  if (name == "Q7")
    return sqlQ7(params);
  if (name == "Q8")
    return sqlQ8(params);
  if (name == "Q9")
    return sqlQ9(params);
  if (name == "Q10")
    return sqlQ10(params);
  if (name == "Q11")
    return sqlQ11(params);
  if (name == "Q12")
    return sqlQ12(params);
  if (name == "Q13")
    return sqlQ13(params);
  if (name == "Q14")
    return sqlQ14(params);
  if (name == "Q15")
    return sqlQ15(params);
  if (name == "Q16")
    return sqlQ16(params);
  if (name == "Q17")
    return sqlQ17(params);
  if (name == "Q18")
    return sqlQ18(params);
  if (name == "Q19")
    return sqlQ19(params);
  if (name == "Q20")
    return sqlQ20(params);
  if (name == "Q21")
    return sqlQ21(params);
  if (name == "Q22")
    return sqlQ22(params);
  throw std::runtime_error("unknown query: " + name);
}

} // namespace tpch
