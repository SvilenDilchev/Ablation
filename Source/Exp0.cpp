#include "BossQueries.hpp"
#include "Output.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using boss::Symbol;
using namespace boss::utilities;

namespace {

// The generated tables exp0 measures on. Kept between runs: load() only writes a file that
// isn't there yet, so a rerun reuses them. Delete the directory after changing cell() or a
// column layout, or the stale files get measured instead.
const std::string tableDir = std::string(REPO_DIR) + "/generated/tables";

// Registry sizes the rewriter scan is measured at. The first is the empty registry, and the
// per-candidate cost is fitted across all of them.
const std::vector<int> SCAN_SIZES = {50, 100, 200, 400};

// One cell's text for row `index`, wide enough that strings look like TPC-H strings
std::string cell(const std::string &type, long index) {
  if (type == "int64")
    return std::to_string(index);
  if (type == "double")
    return std::to_string(index) + ".5";
  return "value-" + std::to_string(1000000000L + index); // 16 chars, like a TPC-H string
}

// Writes an n-row file with the given column types (no header) and loads it under `name`.
bool load(const std::string &name, long rows, const std::vector<std::string> &cols,
          const std::vector<std::string> &types) {
  std::filesystem::create_directories(tableDir);
  std::string path = tableDir + "/" + name + ".csv";
  if (!std::filesystem::exists(path)) {
    std::ofstream file(path);
    for (long i = 0; i < rows; ++i) {
      for (size_t col = 0; col < cols.size(); ++col)
        file << (col ? "," : "")
             << (cols[col] == "k" ? std::to_string(i % 4) : cell(types[col], i));
      file << "\n";
    }
  }
  return bossq::loadTable(path, name, cols);
}

boss::Expression byName(const std::string &table) { return "ByName"_(Symbol(table)); }
std::int64_t i64(long value) { return static_cast<std::int64_t>(value); }

struct Recorder {
  out::Csv &csv;
  out::LogTail &tail;
  std::map<std::string, std::vector<double>> samples;

  void put(const std::string &measurement, const std::string &type, long rows, int rep, bool ok,
           double seconds, long resultRows, const std::string &note) {
    csv.row({measurement, type, std::to_string(rows), std::to_string(rep), ok ? "true" : "false",
             out::fmt(seconds), std::to_string(resultRows), std::to_string(out::nowMs()), note});

    if (ok)
      samples[measurement + "/" + type + "/" + std::to_string(rows)].push_back(seconds);
    else
      std::cout << measurement << " " << type << " " << rows << " FAILED: " << note << "\n";
  }

  void record(const std::string &measurement, const std::string &type, long rows, int rep,
              const bossq::QueryResult &result) {
    put(measurement, type, rows, rep, result.ok, result.seconds, result.rows, result.error);
  }
  // ACE timer lines emitted since the last call, e.g. "[ACE-TIME] <label>: 0.0123s".
  void recordEngine(const std::string &measurement, const std::string &type, long rows, int rep,
                    const std::string &label) {
    for (auto const &line : tail.readLines()) {
      if (line.rfind("[ACE-TIME]", 0) != 0 || line.find(label) == std::string::npos)
        continue;
      auto colon = line.rfind(": ");
      if (colon == std::string::npos)
        continue;
      put(measurement, type, rows, rep, true, std::stod(line.substr(colon + 2)), rows, label);
    }
  }
  double median(const std::string &key) {
    auto values = samples[key];
    if (values.empty())
      return 0.0;

    std::sort(values.begin(), values.end());
    auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : 0.5 * (values[middle - 1] + values[middle]);
  }
};

} // namespace

int main() {
  auto dir = out::dirFor("exp0");
  out::redirectStderr(dir);
  out::SysInfoSampler sampler(dir);
  out::LogTail tail(dir + "/instrumentation.log");
  out::Csv csv(dir + "/results.csv", "measurement,type,rows,rep,ok,seconds,result_rows,t_ms,note");
  Recorder rec{csv, tail};

  const int reps = 5;
  const std::vector<long> sizes = {1000000, 4000000};

  std::cout << "=== exp0: engine constants ===\n";

  // The floor cost of being a BOSS query at all: engine hops, signature extraction, a scan of
  // an empty registry. Subtracting it from hit/int64/1 leaves the view protocol on its own.
  sampler.setPhase("pipeline");
  bossq::clearViews();
  load("p1", 1, {"x"}, {"int64"});
  // The first calls through a cold pipeline cost 3-4x the steady state, so warm up unrecorded;
  // this is a fixed cost, taken as a median, not a slope, so warm-up would not cancel out.
  for (int warm = 0; warm < 15; ++warm)
    bossq::runExpression("Slice"_("Project"_(byName("p1"), "x"_), 0, 1));
  for (int rep = 1; rep <= 20; ++rep)
    rec.record("pipeline", "int64", 1, rep,
               bossq::runExpression("Slice"_("Project"_(byName("p1"), "x"_), 0, 1)));

  // A Project view over an n-row table, materialised then reused inside a Slice. Wall clock for
  // the whole VE -> ACE -> VE round trip, so a hit at one row is what it costs to serve a cached
  // view holding no data, which is the floor on how small a view can usefully be. The engine's
  // own timers come back through recordEngine: rebox (Arrow -> BOSS on build) and unbox
  // (BOSS -> Arrow on reuse), per type.
  for (auto const &type : {"int64", "string"}) {
    sampler.setPhase(std::string("miss/hit ") + type);
    for (long rows : {1L, 1000000L, 4000000L}) {
      std::string table = std::string("t_") + type + "_" + std::to_string(rows);
      load(table, rows, {"x"}, {type});
      auto query = [&] { return "Slice"_("Project"_(byName(table), "x"_), 0, 1); };

      for (int rep = 1; rep <= reps; ++rep) {
        bossq::clearViews();
        bossq::defineView("V", "Project"_(byName(table), "x"_));
        tail.readLines();
        rec.record("miss", type, rows, rep, bossq::runExpression(query()));
        rec.recordEngine("rebox_engine", type, rows, rep,
                         "boxing loop (Arrow Table -> boss::Expression, " + std::to_string(rows) +
                             " rows x 1 cols) for V");
        rec.record("hit", type, rows, rep, bossq::runExpression(query()));
        rec.recordEngine("unbox_engine", type, rows, rep,
                         "Borrowed(V) BOSS literal -> Arrow rebuild");
      }
    }
  }
  bossq::clearViews();

  // Plain operators over n rows. Two sizes, because every constant here is a slope rather than a
  // median: the difference between them cancels the per-call overhead and leaves the per-row cost.
  sampler.setPhase("operators");
  for (long rows : sizes) {
    std::string table = "op_" + std::to_string(rows);
    std::string wideTable = "wide_" + std::to_string(rows);
    std::string joinLeft = "ja_" + std::to_string(rows);
    std::string joinRight = "jb_" + std::to_string(rows);

    load(table, rows, {"x", "k"}, {"int64", "int64"});
    load(wideTable, rows, {"x", "a", "b", "s", "k"},
         {"int64", "int64", "double", "string", "int64"});
    load(joinLeft, rows, {"a", "x"}, {"int64", "int64"});
    load(joinRight, rows, {"b", "y"}, {"int64", "int64"});

    for (int rep = 1; rep <= reps; ++rep) {
      rec.record("filter", "int64", rows, rep,
                 bossq::runExpression("Filter"_(byName(table), "Greater"_("x"_, i64(rows / 2)))));
      rec.record(
          "filter_wide", "int64", rows, rep,
          bossq::runExpression("Filter"_(byName(wideTable), "Greater"_("x"_, i64(rows / 2)))));
      rec.record("groupby", "int64", rows, rep,
                 bossq::runExpression("GroupBy"_(byName(table), "Sum"_("x"_), "k"_)));
      rec.record("groupby_hi", "int64", rows, rep,
                 bossq::runExpression("GroupBy"_(byName(table), "Sum"_("k"_), "x"_)));
      rec.record("groupby_hi2", "int64", rows, rep,
                 bossq::runExpression("GroupBy"_(byName(table), "CountAll"_(), "x"_, "k"_)));
      rec.record("groupby_str", "string", rows, rep,
                 bossq::runExpression(
                     "GroupBy"_(byName("t_string_" + std::to_string(rows)), "CountAll"_(), "x"_)));
      rec.record(
          "join", "int64", rows, rep,
          bossq::runExpression("Join"_(byName(joinLeft), byName(joinRight), "Equal"_("a"_, "b"_))));
      rec.record("project", "int64", rows, rep,
                 bossq::runExpression("Project"_(byName(table), "x"_)));
    }
  }

  // OrderBy is n log n, so where you measure it decides the slope you get. Every query in the
  // set sorts somewhere between 1 and 38k rows, so it gets measured there. Taking the slope at
  // 1M/4M like the other operators puts it about 50% high for the sizes that actually occur.
  sampler.setPhase("orderby");
  for (long rows : {10000L, 40000L}) {
    std::string table = "ord_" + std::to_string(rows);
    load(table, rows, {"x", "k"}, {"int64", "int64"});
    for (int rep = 1; rep <= reps; ++rep)
      rec.record("orderby", "int64", rows, rep,
                 bossq::runExpression("OrderBy"_(byName(table), "keys"_("x"_))));
  }

  // A query matching none of the registered views, run against a registry that grows each
  // round. The per-candidate cost is the slope across those rounds. One subtraction won't do
  // it: the scan is tens of microseconds sitting on a pipeline cost that drifts by about as
  // much between runs, so a two-point difference is mostly drift.
  sampler.setPhase("rewriter scan");
  load("scan_t", 1000, {"x"}, {"int64"});
  auto scanQuery = [] { return "Filter"_(byName("scan_t"), "Greater"_("x"_, i64(-1))); };

  // baseline first: the same query with nothing registered
  for (int rep = 1; rep <= reps; ++rep)
    rec.record("scan", "views", 0, rep, bossq::runExpression(scanQuery()));

  int defined = 0;
  for (int target : SCAN_SIZES) {
    for (; defined < target; ++defined) {
      bossq::defineView("SCAN_V" + std::to_string(defined),
                        "Filter"_(byName("scan_t"), "Greater"_("x"_, i64(2000 + defined))));
    }

    for (int rep = 1; rep <= reps; ++rep)
      rec.record("scan", "views", target, rep, bossq::runExpression(scanQuery()));
  }
  bossq::clearViews();
  sampler.setPhase("done");

  auto perRow = [&](const std::string &measurement, const std::string &type) {
    return (rec.median(measurement + "/" + type + "/4000000") -
            rec.median(measurement + "/" + type + "/1000000")) /
           3e6 * 1e9;
  };
  std::cout << "\nderived constants\n";
  for (auto const &type : {"int64", "string"}) {
    std::string pad(8 - std::string(type).size(), ' ');
    std::cout << "rebox " << type << pad << " " << out::fmt(perRow("rebox_engine", type), 3)
              << " ns/row (engine)   " << out::fmt(perRow("miss", type), 3)
              << " ns/row (wall, incl. project)\n";
    std::cout << "unbox " << type << pad << " " << out::fmt(perRow("unbox_engine", type), 3)
              << " ns/row (engine)   " << out::fmt(perRow("hit", type), 3)
              << " ns/row (wall, incl. slice)\n";
  }
  std::cout << "filter  (50% selectivity)      " << out::fmt(perRow("filter", "int64"), 2)
            << " ns/row\n";
  std::cout << "filter  (wide row, 5 cols)     " << out::fmt(perRow("filter_wide", "int64"), 2)
            << " ns/row\n";
  std::cout << "groupby (4 keys)               " << out::fmt(perRow("groupby", "int64"), 2)
            << " ns/row\n";
  std::cout << "groupby (all-distinct keys)    " << out::fmt(perRow("groupby_hi", "int64"), 2)
            << " ns/row\n";
  std::cout << "groupby (2 distinct int keys)  " << out::fmt(perRow("groupby_hi2", "int64"), 2)
            << " ns/row\n";
  std::cout << "groupby (distinct string key)  " << out::fmt(perRow("groupby_str", "string"), 2)
            << " ns/row\n";
  std::cout << "join    (1:1)                  " << out::fmt(perRow("join", "int64"), 2)
            << " ns/row\n";
  // Least squares through (registry size, seconds). The pipeline's fixed cost is the same in
  // every one of those, so it lands in the intercept and leaves the slope alone.
  std::vector<int> scanSizes = {0};
  scanSizes.insert(scanSizes.end(), SCAN_SIZES.begin(), SCAN_SIZES.end());
  double meanSize = 0, meanSeconds = 0;
  for (int size : scanSizes) {
    meanSize += size;
    meanSeconds += rec.median("scan/views/" + std::to_string(size));
  }
  meanSize /= scanSizes.size();
  meanSeconds /= scanSizes.size();

  double covariance = 0, variance = 0;
  for (int size : scanSizes) {
    double sizeOffset = size - meanSize;
    covariance += sizeOffset * (rec.median("scan/views/" + std::to_string(size)) - meanSeconds);
    variance += sizeOffset * sizeOffset;
  }
  std::cout << "rewriter scan per candidate    " << out::fmt(covariance / variance * 1e6, 2)
            << " us (fitted over " << scanSizes.front() << "-" << scanSizes.back()
            << " views)\n";
  double pipe = rec.median("pipeline/int64/1"), hit1 = rec.median("hit/int64/1");
  std::cout << "pipeline fixed cost            " << out::fmt(pipe * 1e6, 1) << " us per query\n";
  std::cout << "cache hit on a 1-row view      " << out::fmt(hit1 * 1e6, 1)
            << " us (protocol cost, " << out::fmt((hit1 - pipe) * 1e6, 1)
            << " us of it view machinery)\n";
  std::cout << "cache hit on 1M int64 rows     "
            << out::fmt(rec.median("hit/int64/1000000") * 1e3, 2) << " ms wall\n";
  return 0;
}
