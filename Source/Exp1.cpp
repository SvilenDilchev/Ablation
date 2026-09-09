// exp1: all 22 TPC-H queries, no views, spec parameters, on BOSS / DuckDB / MonetDB.
// google benchmark drives the whole thing, so it takes its flags.
#include "BossQueries.hpp"
#include "Output.hpp"
#include "Queries.hpp"
#include "SystemsEmbedded.hpp"

#include <benchmark/benchmark.h>

#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int WARMUP_ITERATIONS = 3;
out::SysInfoSampler *sampler = nullptr;

struct QueryInfo {
  long rows = 0;
  double checksum = 0.0;
  bool ok = false;
};
std::map<std::string, QueryInfo> bossInfo; // filled by the first warm-up of each BOSS bench

void bossBench(benchmark::State &state, const std::string &queryName) {
  sampler->setPhase("BOSS");
  auto run = [&] { return bossq::runQuery(queryName, {}); };
  bool failed = false;

  {
    // First warm-up doubles as our rows/checksum sample; the timed loop throws its results away
    auto result = run();
    failed = !result.ok;
    bossInfo[queryName] = {result.rows, result.checksum, result.ok};
    if (!result.ok)
      std::cout << "BOSS " << queryName << " FAILED: " << result.error << "\n";
  }

  for (int i = 1; !failed && i < WARMUP_ITERATIONS; ++i)
    failed = !run().ok;

  for (auto _ : state) {
    if (!failed) {
      // does nothing here, but MultiKernelBOSS's loop has it so we keep it for consistency
      state.PauseTiming();
      state.ResumeTiming();
      auto result = run();
      if (!result.ok)
        failed = true;
      benchmark::DoNotOptimize(result);
    }
  }

  if (failed)
    state.SkipWithError(("BOSS " + queryName + " failed").c_str());
}

void sqlBench(benchmark::State &state, sysemb::Result (*runDb)(const std::string &),
              const char *system, const std::string &queryName) {
  sampler->setPhase(system);

  // MonetDB/e only allows one in-memory database at a time, so drop both DuckDBs before we
  // start on it. The two MonetDB modes release each other on open.
  if (std::string_view(system).starts_with("MonetDB"))
    sysemb::releaseDuckdb();

  auto sql = tpch::sqlQuery(queryName, {});
  bool failed = false;

  for (int i = 0; !failed && i < WARMUP_ITERATIONS; ++i) {
    auto result = runDb(sql);
    if (!result.ok) {
      failed = true;
      std::cout << system << " " << queryName << " FAILED: " << result.error << "\n";
    }
  }

  for (auto _ : state) {
    if (!failed) {
      auto result = runDb(sql);
      if (!result.ok)
        failed = true;
      benchmark::DoNotOptimize(result);
    }
  }

  if (failed)
    state.SkipWithError((std::string(system) + " " + queryName + " failed").c_str());
}

// Console reporter that also keeps every run so we can write the table and results.csv after
class Collector : public benchmark::ConsoleReporter {
public:
  void ReportRuns(const std::vector<Run> &report) override {
    for (auto const &run : report)
      runs.push_back(run);
    benchmark::ConsoleReporter::ReportRuns(report);
  }
  std::vector<Run> runs;
};

} // namespace

int main(int argc, char **argv) {
  // everything lands in results/exp1/, and the sampler starts ticking the moment it's built
  auto dir = out::dirFor("exp1");
  out::redirectStderr(dir);
  out::SysInfoSampler sysinfo(dir);
  sampler = &sysinfo;
  out::Csv csv(dir + "/results.csv",
               "system,query,rep,ok,seconds,rows,checksum,t_start_ms,t_end_ms,note");
  auto const &queries = tpch::allQueries();

  // the 22 queries plus Q13R, which is BOSS only and stays out of the 22-query total
  std::vector<std::string> queryLabels(queries.begin(), queries.end());
  queryLabels.push_back("Q13R");

  // load the tables into BOSS, nothing else works until this is done
  std::cout << "=== exp1: 22 TPC-H queries x {BOSS, DuckDB, MonetDB}, google benchmark ===\n";
  sampler->setPhase("load");
  out::marker("LOAD BEGIN");
  if (!bossq::loadTables()) {
    std::cout << "table load failed\n";
    return 1;
  }
  out::marker("LOAD END");

  // register the BOSS benchmarks. Nothing runs yet, we're just handing google benchmark a name
  // and the function it should call for each one
  for (auto const &queryName : queryLabels) {
    auto bench = [queryName](benchmark::State &state) { bossBench(state, queryName); };
    benchmark::RegisterBenchmark(("BOSS/" + queryName).c_str(), bench)
        ->Unit(benchmark::kMillisecond);
  }

  struct Sql { // 1 system and setting
    const char *name;
    sysemb::Result (*run)(const std::string &);
    std::string (*version)();
  };

  // same again for the SQL systems, 22 each and no Q13R.
  // -1T pins each system to one thread through its own mechanism, -MT just takes its defaults
  for (auto const &db : {Sql{"DuckDB-1T", sysemb::duckdb, sysemb::duckdbVersion},
                         Sql{"DuckDB-MT", sysemb::duckdbMulti, sysemb::duckdbVersion},
                         Sql{"MonetDB-1T", sysemb::monetdbe, sysemb::monetdbeVersion},
                         Sql{"MonetDB-MT", sysemb::monetdbeMulti, sysemb::monetdbeVersion}}) {
    std::cout << db.name << " " << db.version() << "\n";
    for (auto const &queryName : queries) {
      auto bench = [db, queryName](benchmark::State &state) {
        sqlBench(state, db.run, db.name, queryName);
      };
      benchmark::RegisterBenchmark((std::string(db.name) + "/" + queryName).c_str(), bench)
          ->Unit(benchmark::kMillisecond);
    }
  }

  // this is where all 111 actually run, and the collector catches each result on the way out
  benchmark::Initialize(&argc, argv);
  Collector collector;
  benchmark::RunSpecifiedBenchmarks(&collector);
  sysemb::releaseMonetdbe();
  sampler->setPhase("done");

  // one row per (system, query) into results.csv
  auto timestamp = std::to_string(out::nowMs());
  std::map<std::string, std::map<std::string, double>> secs; // system -> query -> s/iter

  for (auto const &run : collector.runs) {
    auto name = run.benchmark_name();
    auto slash = name.find('/');
    std::string system = name.substr(0, slash), queryName = name.substr(slash + 1);
    if (run.skipped || run.iterations == 0)
      continue;

    double seconds = run.real_accumulated_time / double(run.iterations);
    secs[system][queryName] = seconds;
    // only BOSS gives us rows and a checksum, the SQL runners don't hand anything back
    auto info = system == "BOSS" && bossInfo.contains(queryName) ? bossInfo[queryName]
                                                                 : QueryInfo{0, 0.0, true};
    csv.row({system, queryName, "1", info.ok ? "true" : "false", out::fmt(seconds),
             std::to_string(info.rows), out::fmt(info.checksum, 4), timestamp, timestamp,
             "iters=" + std::to_string(run.iterations)});
  }

  // the summary table from here down, one column per system
  std::cout << "\nmean seconds per iteration (google benchmark)\n"
            << std::fixed << std::setprecision(3) << std::left << std::setw(6) << "query";
  for (auto const &[system, _] : secs)
    std::cout << std::right << std::setw(12) << system;
  std::cout << "\n";

  std::map<std::string, double> totals;

  for (auto const &queryName : queryLabels) {
    std::cout << std::left << std::setw(6) << queryName;
    for (auto const &[system, byQuery] : secs) {
      auto it = byQuery.find(queryName);
      // Q13R is an extra row, keep it out of the total
      if (it != byQuery.end() && queryName != "Q13R")
        totals[system] += it->second;
      std::cout << std::right << std::setw(12)
                << (it == byQuery.end() ? "FAIL" : out::fmt(it->second, 3));
    }
    std::cout << "\n";
  }

  // totals across the 22 queries, Q13R isn't in them
  std::cout << std::left << std::setw(6) << "ALL";
  for (auto const &[system, _] : secs)
    std::cout << std::right << std::setw(12) << out::fmt(totals[system], 3);
  benchmark::Shutdown();
  return 0;
}
