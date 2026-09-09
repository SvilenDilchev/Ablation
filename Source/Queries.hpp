#pragma once

#include <BOSS.hpp>
#include <ExpressionUtilities.hpp>

#include <string>
#include <vector>

namespace tpch {

// The spec's qualification parameters, every one a literal. Date ranges are pre-resolved to
// [lo, hi) bounds rather than being computed from an interval at query time.
struct QueryParams {
  std::string q1ShipdateBound = "1998-08-31";
  std::string q2Region = "EUROPE";
  int q2Size = 15;
  std::string q2TypeLike = "%BRASS";
  std::string q3Segment = "BUILDING";
  std::string q3Date = "1995-03-15";
  std::string q4DateLo = "1993-07-01", q4DateHi = "1993-10-01";
  std::string q5Region = "ASIA";
  std::string q5DateLo = "1994-01-01", q5DateHi = "1995-01-01";
  // Q6 carries each bound twice. BOSS compares with strict Greater/Less against nudged
  // constants (0.0499, 0.07001) lifted from MultiKernelBOSS, which sidesteps 0.05 and 0.07
  // not being exactly representable as doubles. The SQL text keeps the spec's BETWEEN.
  std::string q6DateLower = "1993-12-31", q6DateUpper = "1995-01-01";
  std::string q6ShipLo = "1994-01-01"; // inclusive form of q6DateLower, for the SQL text
  double q6DiscountLow = 0.0499, q6DiscountHigh = 0.07001;
  double q6DiscLo = 0.05, q6DiscHi = 0.07; // BETWEEN bounds, for the SQL text
  int q6Quantity = 24;
  std::string q7Nation1 = "FRANCE", q7Nation2 = "GERMANY";
  std::string q8Nation = "BRAZIL", q8Region = "AMERICA", q8PartType = "ECONOMY ANODIZED STEEL";
  double q9PriceLow = 1006.05, q9PriceHigh = 1080.1;
  std::string q10DateLo = "1993-10-01", q10DateHi = "1994-01-01";
  std::string q11Nation = "GERMANY";
  double q11Fraction = 0.0001;
  std::string q12Mode1 = "MAIL", q12Mode2 = "SHIP";
  std::string q12DateLo = "1994-01-01", q12DateHi = "1995-01-01";
  std::string q13Word1 = "special", q13Word2 = "requests";
  std::string q14DateLo = "1995-09-01", q14DateHi = "1995-10-01";
  std::string q15DateLo = "1996-01-01", q15DateHi = "1996-04-01";
  std::string q16Brand = "Brand#45", q16TypeNotLike = "MEDIUM POLISHED%";
  std::vector<int> q16Sizes = {49, 14, 23, 45, 19, 3, 36, 9};
  std::string q17Brand = "Brand#23", q17Container = "MED BOX";
  int q18Quantity = 300;
  std::string q19Brand1 = "Brand#12", q19Brand2 = "Brand#23", q19Brand3 = "Brand#34";
  double q19Qty1 = 1.0, q19Qty2 = 10.0, q19Qty3 = 20.0;
  std::string q20PartPrefix = "forest", q20Nation = "CANADA";
  std::string q20DateLo = "1994-01-01", q20DateHi = "1995-01-01";
  std::string q21Nation = "SAUDI ARABIA";
  std::vector<std::string> q22CountryCodes = {"13", "31", "23", "29", "30", "18", "17"};
};

// The 22 TPC-H queries. Q13 is the original form. "Q13R" is a 23rd recognised name holding
// its rewrite, the flag form the rewriter's feasibility wall demands, which the view
// experiments run in Q13's place (viewQueries).
const std::vector<std::string> &allQueries();
const std::vector<std::string> &viewQueries();

boss::Expression bossQuery(const std::string &name, const QueryParams &params);
std::string sqlQuery(const std::string &name, const QueryParams &params);

// Numeric column summed for cross-checking a BOSS result; Q20 has none, so its
// s_address lengths are summed instead.
std::string checksumColumn(const std::string &name);

// Sub-expressions shared between a query and its views (views.cpp, lite_views.cpp).
boss::Expression q1Aggregate(const std::string &shipdateBound);
boss::Expression q2SuppRegion(const std::string &region);
boss::Expression q3BaseJoin(const std::string &segment, const std::string &orderDateBound,
                            const std::string &shipDateBound);
boss::Expression q4Qualifying();
boss::Expression q4OrdersJoin(const std::string &dateLo, const std::string &dateHi);
boss::Expression q5RegionYearJoin(const std::string &region, const std::string &dateLo,
                                  const std::string &dateHi);
boss::Expression q6NarrowedLineitem();
boss::Expression q7Chain(); // the 4-way chain below the nation joins
boss::Expression q8Join2(const std::string &region);
boss::Expression q10BaseJoin(const std::string &dateLo, const std::string &dateHi);
boss::Expression q11BaseJoin(const std::string &nation);
boss::Expression q12YearJoin(const std::string &dateLo, const std::string &dateHi);
boss::Expression q12StackedJoin(const std::string &dateLo, const std::string &dateHi,
                                const std::string &mode1, const std::string &mode2);
boss::Expression q13Adjusted(const std::string &word1, const std::string &word2);
boss::Expression q14BaseJoin(const std::string &dateLo, const std::string &dateHi);
boss::Expression q15Revenue(const std::string &dateLo, const std::string &dateHi);
boss::Expression q17PartAvg();
boss::Expression q18Joined(int quantityThreshold);
boss::Expression q19SharedJoin();
boss::Expression q20LineitemQty(const std::string &dateLo, const std::string &dateHi);
boss::Expression q21MultiSuppOrders();
boss::Expression q21MultiLateOrders();
boss::Expression q22NoOrders();

} // namespace tpch
