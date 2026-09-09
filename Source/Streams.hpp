#pragma once

#include "Queries.hpp"

#include <string>
#include <vector>

namespace streams {

// One query that a stream runs: which query it is, where it comes in that stream's order, and
// the parameter values qgen picked for it.
struct QueryInstance {
  std::string query;
  int streamId = 0;
  unsigned seed = 0;
  int position = 0;      // index within its stream, in qgen's permutation order
  std::string rawParams; // qgen's substitution values, '|'-separated
  tpch::QueryParams params;
};

// Builds the streams by running the reference qgen tool: stream i gets qgen's permutation
// -p (301 + i) and a seed worked out from one fixed master seed. Two things follow from that.
// The same N always gives the same streams, and running with N=4 gives you exactly the first
// four streams of an N=256 run. So when the speedup changes between two values of N, that's
// the effect of N, not of the run having picked different queries.
std::vector<QueryInstance> generate(int nStreams);

// Date arithmetic on "YYYY-MM-DD" strings, used here for qgen's date parameters and by views.cpp
// to widen a view's date range beyond one instance's window.
std::string addDays(const std::string &iso, int days);
std::string addMonths(const std::string &iso, int months);
std::string addYears(const std::string &iso, int years);

} // namespace streams
