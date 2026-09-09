#pragma once

#include "Queries.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lite {

// Four points along a query's input spine, which is argument 0 of every relational operator
// walked from the root down:
//   base       the first Filter / Join / LeftJoin / AntiJoin, so the FROM + WHERE part
//   aggregate  the first GroupBy, or the base cut for a query with none on its spine
//   max        the root operator's input, so everything but the outermost operator
//   whole      the entire query, a classic result cache
// Lite mode substitutes the definition verbatim, so ACE runs the same expression for every
// cut. Only how much tree ViewEngine stores and splices differs.
enum class Cut { Base, Aggregate, Max, Whole };

inline const Cut cuts[] = {Cut::Base, Cut::Aggregate, Cut::Max, Cut::Whole};
const char *cutName(Cut cut);
std::string viewName(const std::string &query, Cut cut);

struct Carve {
  boss::Expression rewritten; // the query with the cut sub-tree replaced by QueryView(name)
  std::optional<boss::Expression> body; // the cut sub-tree
  std::string head;                     // its operator
};

// nestedRef emits a bare-symbol view reference instead of QueryView(name): inside a view
// definition ViewEngine records a registered view's name as a dependency directly.
Carve carve(boss::Expression &&query, Cut cut, int index, const std::string &name,
            bool nestedRef = false);

// Defines the view. ViewEngine refuses some sub-trees (Q20's predicate against an aggregate is
// a "side effect" to its analyser), and then the next node down the spine gets tried, which is
// what index records.
struct Setup {
  bool ok = false;
  std::string head;
  int index = 0;
  std::string rejected;
  double defineSeconds = 0.0;
  std::string error;
};
Setup define(const std::string &query, Cut cut, const tpch::QueryParams &params = {});

// Defines the cuts as a dependency chain, each level's stored definition holding a QueryView of
// the next level down instead of that sub-tree. Evaluating the rebuilt query then makes Lite
// mode resolve the nesting recursively back to the original tree. Levels whose carve finds
// nothing new, where a cut falls back onto the previous one, collapse.
struct NestedSetup {
  bool ok = false;
  std::vector<std::pair<Cut, int>> levels; // (cut, carve index) per defined level, inside-out
  double defineSeconds = 0.0;
  std::string error;
};
NestedSetup defineNested(const std::string &query, const tpch::QueryParams &params = {});

// Rebuilds that nested query, the tree whose innermost levels are QueryView references, from
// the recorded levels. Called inside the timed region so query building costs match the other
// conditions (Q11's threshold round trip happens during build).
boss::Expression buildNested(const std::string &query,
                             const std::vector<std::pair<Cut, int>> &levels,
                             const tpch::QueryParams &params = {});

} // namespace lite
