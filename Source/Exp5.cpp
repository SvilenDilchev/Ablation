#include "StreamExperiment.hpp"

int main(int argc, char **argv) {
  // Same scarcity as exp4, but physical. ViewEngine keeps exp3's 18 GB budget, which never
  // binds, and run_exp5.sh caps the process instead: 5952M is the 6467 MiB exp3 peaked at,
  // minus the 515 MiB of views exp4's 75% budget couldn't hold. So the kernel pages out
  // whatever exp4's policy would have evicted, and the two can be compared.
  return streamexp::run("exp5", 18e9, streamexp::parseCounts(argc > 1 ? argv[1] : ""));
}
