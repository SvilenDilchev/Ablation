#include "StreamExperiment.hpp"

int main(int argc, char **argv) {
  // 18 GB is the recycling paper's TPC-H figure. At SF1 it never binds, so nothing is ever
  // evicted and this measures what the views buy with no cache pressure at all.
  return streamexp::run("exp3", 18e9, streamexp::parseCounts(argc > 1 ? argv[1] : ""));
}
