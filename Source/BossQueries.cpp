#include "BossQueries.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <variant>

using ComplexExpression = boss::DefaultExpressionSystem::ComplexExpression;
using boss::Symbol;
using namespace boss::utilities;

namespace bossq {
namespace {

const std::string engineDir = ENGINE_DIR; // build/deps/lib, set in CMakeLists
const std::string viewEnginePath = engineDir + "/libViewEngine.so";
const std::string arrowEnginePath = engineDir + "/libArrowComputeEngine.so";
const std::string tpchDir = TPCH_DATA_DIR; // set in CMakeLists

// dbgen's .tbl files carry no header, so Load has to be told the column names positionally.
const std::vector<std::pair<std::string, std::vector<std::string>>> tableSchemas = {
    {"lineitem",
     {"l_orderkey", "l_partkey", "l_suppkey", "l_linenumber", "l_quantity", "l_extendedprice",
      "l_discount", "l_tax", "l_returnflag", "l_linestatus", "l_shipdate", "l_commitdate",
      "l_receiptdate", "l_shipinstruct", "l_shipmode", "l_comment"}},
    {"orders",
     {"o_orderkey", "o_custkey", "o_orderstatus", "o_totalprice", "o_orderdate", "o_orderpriority",
      "o_clerk", "o_shippriority", "o_comment"}},
    {"customer",
     {"c_custkey", "c_name", "c_address", "c_nationkey", "c_phone", "c_acctbal", "c_mktsegment",
      "c_comment"}},
    {"part",
     {"p_partkey", "p_name", "p_mfgr", "p_brand", "p_type", "p_size", "p_container",
      "p_retailprice", "p_comment"}},
    {"supplier",
     {"s_suppkey", "s_name", "s_address", "s_nationkey", "s_phone", "s_acctbal", "s_comment"}},
    {"partsupp", {"ps_partkey", "ps_suppkey", "ps_availqty", "ps_supplycost", "ps_comment"}},
    {"nation", {"n_nationkey", "n_name", "n_regionkey", "n_comment"}},
    {"region", {"r_regionkey", "r_name", "r_comment"}},
};

// Most of the engine control operators just answer true or false
bool boolResult(boss::Expression &&expression) {
  auto result = eval(std::move(expression));
  return std::holds_alternative<bool>(result) && std::get<bool>(result);
}

// A column arrives either as loose arguments or as spans, never both, so count whichever it is
size_t columnLength(ComplexExpression const &column) {
  if (!column.getDynamicArguments().empty())
    return column.getDynamicArguments().size();

  // spans hold the real data for anything sizeable, so add up their lengths

  size_t total = 0;
  for (auto const &span : column.getSpanArguments())
    std::visit([&](auto const &values) { total += values.size(); }, span);
  return total;
}

// Everything numeric is summed as a double. Strings contribute their length instead, because
// Q20 projects no numeric column at all and s_address's lengths have to stand in for one.
template <typename F> void forEachValue(ComplexExpression const &column, F &&callback) {
  for (auto const &argument : column.getDynamicArguments()) {
    std::visit(overload([&](double value) { callback(value); },
                        [&](std::float_t value) { callback(value); },
                        [&](std::int64_t value) { callback(double(value)); },
                        [&](std::int32_t value) { callback(double(value)); },
                        [&](std::int8_t value) { callback(double(value)); },
                        [&](std::string const &text) { callback(double(text.size())); },
                        [&](auto const &) {}), // anything else contributes nothing
               argument);
  }

  // same again for spans, which is how the big columns actually come back
  for (auto const &span : column.getSpanArguments()) {
    std::visit(
        [&](auto const &values) {
          using Element =
              std::remove_const_t<typename std::decay_t<decltype(values)>::element_type>;
          if constexpr (std::is_arithmetic_v<Element> && !std::is_same_v<Element, bool>) {
            for (auto value : values)
              callback(static_cast<double>(value));
          } else if constexpr (std::is_same_v<Element, std::string>) {
            for (auto const &text : values)
              callback(static_cast<double>(text.size()));
          }
        },
        span);
  }
}

// Unwraps Nullable(column, Nulls(...)) and Date(column) to the column itself.
ComplexExpression const *unwrap(ComplexExpression const *column) {
  while (column->getHead().getName() == "Nullable" || column->getHead().getName() == "Date") {
    auto const &inner = column->getDynamicArguments();
    if (inner.empty() || !std::holds_alternative<ComplexExpression>(inner.at(0)))
      return column; // malformed wrapper, hand back what we've got rather than crashing
    column = &std::get<ComplexExpression>(inner.at(0));
  }
  return column;
}

// Finds the named column in the result table and sums it. That one number is how we tell
// whether two systems agree, without having to compare whole tables.
void checksum(const boss::Expression &result, const std::string &columnName,
              QueryResult &queryResult) {
  if (columnName.empty() || !std::holds_alternative<ComplexExpression>(result))
    return;
  auto const &table = std::get<ComplexExpression>(result);
  if (table.getHead().getName() != "Table")
    return;

  // walk the columns until we hit the one we were asked for, then stop
  for (auto const &argument : table.getDynamicArguments()) {
    if (!std::holds_alternative<ComplexExpression>(argument))
      continue;
    auto const *column = unwrap(&std::get<ComplexExpression>(argument));
    if (column->getHead().getName() != columnName)
      continue;
    queryResult.checksumFound = true;
    queryResult.rows = static_cast<long>(columnLength(*column));
    forEachValue(*column, [&](double value) { queryResult.checksum += value; });
    return;
  }
}

} // namespace

// VE rewrites and resolves cache references, ACE executes, VE runs again to fold in ACE's
// measurements and apply admission and eviction. Both VE entries are the same .so, so the
// registry and cache persist across the two passes.
boss::Expression eval(boss::Expression &&expression) {
  return boss::evaluate("EvaluateInEngines"_(
      "List"_(viewEnginePath, arrowEnginePath, viewEnginePath), std::move(expression)));
}

namespace {

// The engines report failure two ways: a bare string, or an ErrorWhenEvaluatingExpression
// wrapper with the message as its second argument. Both have to be caught.
bool isError(const boss::Expression &expression, std::string &message) {
  if (std::holds_alternative<std::string>(expression)) {
    message = std::get<std::string>(expression);
    return true;
  }
  if (auto const *complex = std::get_if<ComplexExpression>(&expression);
      complex && complex->getHead().getName() == "ErrorWhenEvaluatingExpression") {
    auto const &args = complex->getDynamicArguments();
    message = args.size() > 1 && std::holds_alternative<std::string>(args.at(1))
                  ? std::get<std::string>(args.at(1))
                  : "<no message>";
    return true;
  }
  return false;
}

} // namespace

bool loadTable(const std::string &path, const std::string &name,
               const std::vector<std::string> &columns) {
  // Load(path, col1, col2, ...) wrapped in Name(..., tableName), which is what registers it
  boss::ExpressionArguments loadArgs;
  loadArgs.emplace_back(path);
  for (auto const &column : columns)
    loadArgs.emplace_back(Symbol(column));

  boss::ExpressionArguments nameArgs;
  nameArgs.emplace_back(ComplexExpression("Load"_, {}, std::move(loadArgs)));
  nameArgs.emplace_back(Symbol(name));
  auto result = eval(ComplexExpression("Name"_, {}, std::move(nameArgs)));
  std::string err;
  if (isError(result, err)) {
    std::cout << "load " << name << " failed: " << err << "\n";
    return false;
  }
  return true;
}

// Stops at the first table that fails, since nothing downstream will work without it
bool loadTables() {
  for (auto const &[table, columns] : tableSchemas)
    if (!loadTable(tpchDir + "/" + table + ".tbl", table, columns))
      return false;
  return true;
}

bool defineView(const std::string &name, boss::Expression &&body) {
  boss::ExpressionArguments args;
  args.emplace_back(Symbol(name));
  args.emplace_back(std::move(body));
  return boolResult(ComplexExpression("DefineView"_, {}, std::move(args), {}));
}

bool clearViews() { return boolResult(ComplexExpression("ClearViews"_, {}, {}, {})); }
bool setCacheBudget(double bytes) { return boolResult("SetCacheBudget"_(bytes)); }
bool setEngineMode(const std::string &mode) { return boolResult("SetEngineMode"_(Symbol(mode))); }

bool setEvictionPolicy(const std::string &policy) {
  return boolResult("SetEvictionPolicy"_(Symbol(policy)));
}

// Q11's HAVING threshold is a fraction of a sum over the whole partsupp join, so unlike every
// other parameter it cannot be a literal. runBuilt clocks build(), so this is charged to Q11.
double q11Threshold(const tpch::QueryParams &params) {
  auto result = eval("GroupBy"_(tpch::q11BaseJoin(params.q11Nation), "Sum"_("value"_)));
  std::string err;
  if (isError(result, err))
    throw std::runtime_error("Q11 threshold: " + err);

  // Reuse the checksum machinery to pull the single sum back out of the result table
  QueryResult sumResult;
  checksum(result, "sum(value)", sumResult);
  if (!sumResult.checksumFound)
    throw std::runtime_error("Q11 threshold: sum(value) column not found");
  return sumResult.checksum * params.q11Fraction;
}

QueryResult runBuilt(const std::function<boss::Expression()> &build,
                     const std::string &checksumCol) {
  QueryResult queryResult;
  auto start = std::chrono::steady_clock::now();

  try {
    // build() is inside the clock on purpose, it's where Q11's extra round trip happens
    auto expression = build();
    auto result = eval(std::move(expression));
    queryResult.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // an engine error comes back as a value, not a throw, so it needs checking separately
    if (isError(result, queryResult.error))
      return queryResult;

    checksum(result, checksumCol, queryResult);
    queryResult.ok = true;
  } catch (std::exception const &ex) {
    // A throw still gets a time, so a failed query doesn't show up as instant
    queryResult.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    queryResult.error = ex.what();
  }
  return queryResult;
}

QueryResult runQuery(const std::string &name, const tpch::QueryParams &params) {
  return runBuilt([&] { return tpch::bossQuery(name, params); }, tpch::checksumColumn(name));
}

QueryResult runExpression(boss::Expression &&expression, const std::string &checksumCol) {
  return runBuilt([&]() -> boss::Expression { return std::move(expression); }, checksumCol);
}

} // namespace bossq
