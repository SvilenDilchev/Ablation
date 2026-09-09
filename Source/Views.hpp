#pragma once

#include <string>
#include <vector>

namespace views {

struct View {
  std::string name;
  std::string query; // which pattern it serves
};

// The view set for experiments 3 and 4. Each family is the argmin of the design-space sweep
// (scripts/best_views.py) at N = 256, which prices every cacheable cut at every feasible
// granularity from the exp0/1/2 constants and TPC-H's spec cardinalities.
const std::vector<View> &all();

// Defines every view in Q1..Q22 order. Returns the names ViewEngine refused, so empty means
// they all went in.
std::vector<std::string> defineAll();

} // namespace views
