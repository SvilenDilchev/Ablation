#include "LiteViews.hpp"
#include "BossQueries.hpp"

#include <chrono>

using ComplexExpression = boss::DefaultExpressionSystem::ComplexExpression;
using boss::Symbol;
using namespace boss::utilities;

namespace lite {
namespace {

// Carried down the descent: which cut we want, which eligible node to take (target), and how
// many we've passed so far.
struct Walk {
  Cut cut;
  int target;
  bool nestedRef = false; // emit a bare-symbol view reference instead of QueryView(name)
  int seen = 0;
  bool done = false;
};

// Which operators can serve as a cut point for each kind of cut
bool eligible(Cut cut, const std::string &head, int depth) {
  bool leaf = head == "ByName" || head == "Table";
  bool base =
      head == "Filter" || head == "Join" || head == "LeftJoin" || head == "AntiJoin" || leaf;
  switch (cut) {
  case Cut::Base:
    return base;
  case Cut::Aggregate:
    return head == "GroupBy" || base;
  case Cut::Max:
    return depth >= 1;
  case Cut::Whole:
    return true;
  }
  return false;
}

// Descends the input spine looking for the target node, swaps it for a view reference and
// hands the original sub-tree back through out.body.
boss::Expression walk(boss::Expression &&expression, Walk &state, int depth,
                      const std::string &name, Carve &out) {
  auto *complex = std::get_if<ComplexExpression>(&expression);
  if (complex == nullptr || state.done)
    return std::move(expression);

  auto head = complex->getHead().getName();
  if (eligible(state.cut, head, depth)) {
    if (state.seen == state.target) {
      state.done = true;
      out.head = head;
      out.body = std::move(expression);
      if (state.nestedRef)
        return boss::Expression(Symbol(name)); // bare-symbol view reference
      return "QueryView"_(Symbol(name));
    }
    ++state.seen;
  }

  if (head == "ByName" || head == "Table")
    return std::move(expression); // base table, nothing below it

  // only argument 0 is followed, which is what makes this the input spine rather than a
  // full tree walk
  auto [headSymbol, statics, dynamics, spans] = std::move(*complex).decompose();
  if (!dynamics.empty())
    dynamics[0] = walk(std::move(dynamics[0]), state, depth + 1, name, out);
  return ComplexExpression(std::move(headSymbol), std::move(statics), std::move(dynamics),
                           std::move(spans));
}

} // namespace

const char *cutName(Cut cut) {
  switch (cut) {
  case Cut::Base:
    return "base";
  case Cut::Aggregate:
    return "aggregate";
  case Cut::Max:
    return "max";
  case Cut::Whole:
    return "whole";
  }
  return "";
}

// e.g. Q3 + Cut::Base -> "Q3_LITE_BASE". The nested chain appends _N to keep its own views
// separate from the flat ones in the same registry.
std::string viewName(const std::string &query, Cut cut) {
  std::string name = query + "_LITE_" + cutName(cut);
  for (auto &character : name)
    character = static_cast<char>(toupper(character));
  return name;
}

Carve carve(boss::Expression &&query, Cut cut, int index, const std::string &name, bool nestedRef) {
  Carve out;
  Walk state{cut, index, nestedRef};
  out.rewritten = walk(std::move(query), state, 0, name, out);
  return out;
}

Setup define(const std::string &query, Cut cut, const tpch::QueryParams &params) {
  Setup setup;
  auto start = std::chrono::steady_clock::now();

  try {
    // Walk down the spine until one of the candidates is accepted. Rejections get recorded so
    // the results table can show how far we had to fall back. 12 is just a depth cap.
    for (int index = 0; index <= 12; ++index) {
      auto carved = carve(tpch::bossQuery(query, params), cut, index, viewName(query, cut));
      if (!carved.body)
        break; // ran off the end of the spine

      if (bossq::defineView(viewName(query, cut), std::move(*carved.body))) {
        setup.ok = true;
        setup.index = index;
        setup.head = carved.head;
        break;
      }
      setup.rejected += (setup.rejected.empty() ? "" : ",") + carved.head;
    }

    if (!setup.ok)
      setup.error =
          setup.rejected.empty() ? "no sub-tree to cut" : "ViewEngine rejected " + setup.rejected;
  } catch (std::exception const &ex) {
    setup.error = ex.what();
  }

  setup.defineSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return setup;
}

NestedSetup defineNested(const std::string &query, const tpch::QueryParams &params) {
  NestedSetup setup;
  auto start = std::chrono::steady_clock::now();

  try {
    // Each cut carves out of what the previous one left behind, so every level's definition
    // holds a reference to the level below rather than the raw sub-tree.
    auto current = tpch::bossQuery(query, params);
    for (auto cut : cuts) {
      auto name = viewName(query, cut) + "_N";

      for (int index = 0; index <= 12; ++index) {
        auto carved = carve(current.clone(boss::expressions::CloneReason::EXPRESSION_SUBSTITUTION),
                            cut, index, name, true);
        if (!carved.body)
          break; // no candidate node, so this cut collapses onto an inner one

        if (bossq::defineView(name, std::move(*carved.body))) {
          current = std::move(carved.rewritten);
          setup.levels.emplace_back(cut, index);
          break;
        }
      }
    }

    setup.ok = !setup.levels.empty();
    if (!setup.ok)
      setup.error = "no level defined";
  } catch (std::exception const &ex) {
    setup.error = ex.what();
  }

  setup.defineSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return setup;
}

// Replays the levels defineNested recorded, so we get the same nested tree without paying to
// define anything again.
boss::Expression buildNested(const std::string &query,
                             const std::vector<std::pair<Cut, int>> &levels,
                             const tpch::QueryParams &params) {
  auto current = tpch::bossQuery(query, params);
  for (auto const &[cut, index] : levels) {
    auto carved = carve(std::move(current), cut, index, viewName(query, cut) + "_N", true);
    current = std::move(carved.rewritten);
  }
  return current;
}

} // namespace lite
