#pragma once

#include "Queries.hpp"

#include <functional>
#include <string>

namespace bossq {

struct QueryResult {
  bool ok = false;
  double seconds = 0.0;
  long rows = 0;         // rows of the checksum column
  double checksum = 0.0; // sum of the checksum column (numeric values, or string lengths)
  bool checksumFound = false;
  std::string error;
};

// Every expression goes ViewEngine -> ArrowComputeEngine -> ViewEngine. VE runs twice because
// the second pass is where cached values come back out.
boss::Expression eval(boss::Expression &&expression);

// The TPC-H tables, straight from the .tbl files. Nothing else works until this has run.
bool loadTables();
bool loadTable(const std::string &path, const std::string &name,
               const std::vector<std::string> &columns);

// View registry and cache knobs, all of them passed straight through to ViewEngine
bool defineView(const std::string &name, boss::Expression &&body);
bool clearViews();
bool setCacheBudget(double bytes);
bool setEngineMode(const std::string &mode);       // "Full" or "Lite"
bool setEvictionPolicy(const std::string &policy); // "Benefit", "Lru" or "Random"

// Q11's HAVING compares against a fraction of a global sum, and there's no scalar subquery in
// the operator set to express that inline. So the sum runs first and gets pasted in as a
// literal. Throws if it can't be computed.
double q11Threshold(const tpch::QueryParams &params);

// The clock starts before build() so Q11's threshold round trip is inside the measured time.
QueryResult runBuilt(const std::function<boss::Expression()> &build,
                     const std::string &checksumCol);

// The two ways in: by query name, or with an expression that's already built
QueryResult runQuery(const std::string &name, const tpch::QueryParams &params);
QueryResult runExpression(boss::Expression &&expression, const std::string &checksumCol = "");

} // namespace bossq
