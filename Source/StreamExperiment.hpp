#pragma once

#include <string>
#include <vector>

namespace streamexp {

// The driver behind experiments 3, 4 and 5. For each stream count, it runs every query of every
// stream plain, then again with the views defined under the given cache budget, once per
// eviction policy. Running the policies inside one process against one shared plain pass is what
// makes them comparable. Every instance's checksum is cross-checked against its plain twin, and
// a mismatch or a failed query makes this return 1.
int run(const std::string &experiment, double budgetBytes, const std::vector<int> &streamCounts,
        const std::vector<std::string> &evictionPolicies = {"Benefit"});

// Parses "4,8,16" off the command line. Empty or missing gives the full sweep, 4 through 256.
std::vector<int> parseCounts(const char *arg);

} // namespace streamexp
