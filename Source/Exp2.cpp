#include "BossQueries.hpp"
#include "LiteViews.hpp"
#include "Output.hpp"
#include "Queries.hpp"

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int WARMUP_ITERATIONS = 3; // as in exp1, MultiKernelBOSS's warm-up count

// Lite mode runs throughout, so ACE executes the identical plan in every condition and any
// difference between them is ViewEngine's own storage and substitution cost. lite_views.hpp
// has how each cut is found.
//   plain      the query as written, the baseline
//   base       its FROM+WHERE sub-tree replaced by a view reference
//   aggregate  its first GroupBy replaced
//   max        everything below the root operator replaced
//   whole      the entire query replaced, so a result cache
//   nested     the four cuts as a chain, each level naming the next, so one reference
//              resolves recursively back to the original tree
const std::string baselineCondition = "plain";
const std::vector<std::string> conditions = {"plain", "base",  "aggregate",
                                             "max",   "whole", "nested"};

lite::Cut cutOf(const std::string &condition) {
  return condition == "base"        ? lite::Cut::Base
         : condition == "aggregate" ? lite::Cut::Aggregate
         : condition == "max"       ? lite::Cut::Max
                                    : lite::Cut::Whole;
}

// What each query's conditions need, worked out once before any measurement.
struct Plan {
  std::map<lite::Cut, lite::Setup> cuts; // the carved node and slide index of each flat cut
  lite::NestedSetup nested;              // the chain's levels, innermost first
};
std::map<std::string, Plan> plans;

struct Outcome {
  bool ok = false;
  long rows = 0;
  double checksum = 0.0;
};
std::map<std::string, std::map<std::string, Outcome>> outcomes; // query -> condition

out::SysInfoSampler *sampler = nullptr;

// The tree a condition executes: the query as written, or with sub-trees replaced by view
// references. Built inside the timed region, like every other experiment (Q11's threshold
// is a round trip made during building).
boss::Expression buildQuery(const std::string &query, const std::string &condition) {
  if (condition == "plain")
    return tpch::bossQuery(query, {});
  if (condition == "nested")
    return lite::buildNested(query, plans[query].nested.levels);

  auto cut = cutOf(condition);
  auto carved = lite::carve(tpch::bossQuery(query, {}), cut, plans[query].cuts[cut].index,
                            lite::viewName(query, cut));
  return std::move(carved.rewritten);
}

void bench(benchmark::State &state, const std::string &query, const std::string &condition) {
  sampler->setPhase(query + "/" + condition);
  auto run = [&] {
    return bossq::runBuilt([&] { return buildQuery(query, condition); },
                           tpch::checksumColumn(query));
  };
  auto first = run(); // the recorded result: every condition must reproduce the baseline's
  outcomes[query][condition] = {first.ok, first.rows, first.checksum};

  if (!first.ok) {
    state.SkipWithError((query + "/" + condition + ": " + first.error).c_str());
    return;
  }

  for (int i = 1; i < WARMUP_ITERATIONS; ++i)
    (void)run();

  for (auto _ : state) {
    auto result = run();
    benchmark::DoNotOptimize(result);
  }
}

// The console reporter, additionally keeping every run so the table and the CSV can be
// written afterwards. Aggregate rows (mean/median/stddev) are gbench's own summaries of
// runs already kept, so they are skipped.
class Collector : public benchmark::ConsoleReporter {
public:
  void ReportRuns(const std::vector<Run> &report) override {
    for (auto const &run : report)
      if (run.aggregate_name.empty() && !run.skipped && run.iterations > 0)
        runs.push_back(run);
    benchmark::ConsoleReporter::ReportRuns(report);
  }
  std::vector<Run> runs;
};

std::vector<char *> withDefaults(int argc, char **argv) {
  std::vector<char *> args(argv, argv + argc);
  static char reps[] = "--benchmark_repetitions=5";
  static char interleave[] = "--benchmark_enable_random_interleaving=true";
  auto absent = [&args](const char *flag) {
    for (auto *arg : args)
      if (std::strncmp(arg, flag, std::strlen(flag)) == 0)
        return false;
    return true;
  };

  if (absent("--benchmark_repetitions"))
    args.push_back(reps);
  if (absent("--benchmark_enable_random_interleaving"))
    args.push_back(interleave);
  return args;
}

} // namespace

int main(int argc, char **argv) {
  // repetitions and interleaving are forced on unless the caller asked for something else
  auto args = withDefaults(argc, argv);
  int nargs = static_cast<int>(args.size());

  // everything lands in results/exp2/, and the sampler starts ticking the moment it's built
  auto dir = out::dirFor("exp2");
  out::redirectStderr(dir);
  out::SysInfoSampler sysinfo(dir);
  sampler = &sysinfo;
  out::Csv csv(
      dir + "/results.csv",
      "condition,query,ok,seconds,iterations,rows,checksum,view_head,carve_index,t_ms,note");
  auto const &queries = tpch::viewQueries();

  // load the tables into BOSS, nothing else works until this is done
  std::cout << "=== exp2: ViewEngine Lite mode, " << queries.size() << " queries x "
            << conditions.size() << " conditions, google benchmark ===\n";
  sampler->setPhase("load");
  if (!bossq::loadTables()) {
    std::cout << "table load failed\n";
    return 1;
  }

  // Lite mode for the whole run, and every view defined up front: Lite consults the registry
  // only where a reference appears, so the flat views and the chain (whose names carry an _N
  // suffix) coexist, and no condition pays for defining anything.
  sampler->setPhase("define");
  bossq::setEngineMode("Lite");
  for (auto const &query : queries) {
    auto &plan = plans[query];
    for (auto cut : lite::cuts)
      plan.cuts[cut] = lite::define(query, cut);
    plan.nested = lite::defineNested(query);

    std::cout << std::left << std::setw(5) << query << "cuts:";
    for (auto cut : lite::cuts)
      std::cout << " " << lite::cutName(cut) << "="
                << (plan.cuts[cut].ok ? plan.cuts[cut].head : "FAILED")
                << (plan.cuts[cut].index != 0 ? "+" + std::to_string(plan.cuts[cut].index) : "");
    std::cout << "  chain=" << plan.nested.levels.size() << " levels\n";
  }

  // register every (query, condition) pair. Nothing runs yet.
  for (auto const &query : queries)
    for (auto const &condition : conditions)
      benchmark::RegisterBenchmark((query + "/" + condition).c_str(), [query, condition](
                                                                          benchmark::State &state) {
        bench(state, query, condition);
      })->Unit(benchmark::kMillisecond);

  // this is where everything actually runs, and the collector catches each result on the way out
  benchmark::Initialize(&nargs, args.data());
  Collector collector;
  benchmark::RunSpecifiedBenchmarks(&collector);
  sampler->setPhase("done");

  // Mean seconds per iteration, pooled over gbench's repetitions of each case.
  // condition -> query -> (accumulated seconds, iterations)
  std::map<std::string, std::map<std::string, std::pair<double, long>>> pooled;
  auto now = std::to_string(out::nowMs());
  for (auto const &run : collector.runs) {
    auto name = run.benchmark_name();
    auto slash = name.find('/');
    std::string query = name.substr(0, slash), condition = name.substr(slash + 1);

    auto &entry = pooled[condition][query];
    entry.first += run.real_accumulated_time;
    entry.second += static_cast<long>(run.iterations);

    auto const &outcome = outcomes[query][condition];
    auto const &plan = plans[query];
    std::string head = condition == "plain"    ? ""
                       : condition == "nested" ? "chain"
                                               : plan.cuts.at(cutOf(condition)).head;
    int index = condition == "plain"    ? 0
                : condition == "nested" ? static_cast<int>(plan.nested.levels.size())
                                        : plan.cuts.at(cutOf(condition)).index;
    csv.row({condition, query, outcome.ok ? "true" : "false",
             out::fmt(run.real_accumulated_time / double(run.iterations)),
             std::to_string(run.iterations), std::to_string(outcome.rows),
             out::fmt(outcome.checksum, 4), head, std::to_string(index), now, ""});
  }
  // one extra row per cut recording what defining the view cost, separate from the query rows
  for (auto const &query : queries)
    for (auto cut : lite::cuts) {
      auto const &setup = plans[query].cuts.at(cut);
      csv.row({std::string("define_") + lite::cutName(cut), query, setup.ok ? "true" : "false",
               out::fmt(setup.defineSeconds), "0", "0", "0", setup.head,
               std::to_string(setup.index), now,
               setup.ok ? (setup.rejected.empty() ? "" : "fell back past " + setup.rejected)
                        : setup.error});
    }

  // gbench gives us one run per repetition, so pool them back into a single mean
  auto mean = [&pooled](const std::string &condition, const std::string &query) {
    auto byCondition = pooled.find(condition);
    if (byCondition == pooled.end())
      return 0.0;
    auto found = byCondition->second.find(query);
    return found == byCondition->second.end() || found->second.second == 0
               ? 0.0
               : found->second.first / double(found->second.second);
  };

  // Every condition runs the same plan, so every checksum must equal the baseline's.
  int mismatches = 0, failures = 0;
  for (auto const &[query, byCondition] : outcomes) {
    auto baseline = byCondition.find(baselineCondition);
    for (auto const &[condition, outcome] : byCondition) {
      if (!outcome.ok) {
        ++failures;
      } else if (baseline != byCondition.end() && baseline->second.ok &&
                 (outcome.rows != baseline->second.rows ||
                  std::abs(outcome.checksum - baseline->second.checksum) >
                      1e-6 * std::max(1.0, std::abs(baseline->second.checksum)))) {
        ++mismatches;
        std::cout << query << "/" << condition << " CHECKSUM MISMATCH vs " << baselineCondition
                  << "\n";
      }
    }
  }

  // the summary table from here down, one column per condition
  std::cout << "\nmean seconds per iteration (google benchmark)\n"
            << std::fixed << std::setprecision(4) << std::left << std::setw(6) << "query";
  for (auto const &condition : conditions)
    std::cout << std::right << std::setw(12) << condition;
  std::cout << "\n";
  for (auto const &query : queries) {
    std::cout << std::left << std::setw(6) << query;
    for (auto const &condition : conditions)
      std::cout << std::right << std::setw(12) << out::fmt(mean(condition, query), 4);
    std::cout << "\n";
  }

  // and the totals, each condition as a multiple of the plain baseline
  std::cout << "\nsum of per-query means:\n";
  double baselineTotal = 0;
  for (auto const &condition : conditions) {
    double total = 0;
    for (auto const &query : queries)
      total += mean(condition, query);
    if (condition == baselineCondition)
      baselineTotal = total;
    std::cout << std::left << std::setw(12) << condition << out::fmt(total, 4) << "s  "
              << out::fmt(baselineTotal > 0 ? total / baselineTotal : 0, 4) << "x\n";
  }
  std::cout << "checksum mismatches: " << mismatches << ", failures: " << failures << "\n";
  benchmark::Shutdown();
  return mismatches || failures ? 1 : 0;
}
