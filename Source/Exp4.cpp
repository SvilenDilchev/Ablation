#include "StreamExperiment.hpp"

int main(int argc, char **argv) {
  // 1.622 GB is 75% of the 2.162 GB the whole view set occupies, measured in exp3. A quarter of
  // the working set can never fit, so eviction pressure is constant and the three policies have
  // to keep choosing. Same streams and views as exp3, so the only difference is the budget.
  return streamexp::run("exp4", 1.622e9, streamexp::parseCounts(argc > 1 ? argv[1] : ""),
                        {"Benefit", "LRU", "Random"});
}
