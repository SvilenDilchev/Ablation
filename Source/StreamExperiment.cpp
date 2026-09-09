#include "StreamExperiment.hpp"
#include "BossQueries.hpp"
#include "Output.hpp"
#include "Streams.hpp"
#include "Views.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace streamexp {
namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point from) {
  return std::chrono::duration<double>(Clock::now() - from).count();
}

// One pass over every instance of every stream: the per-query results, and the wall time for
// the lot. Two of these, plain and view-enabled, are what a speedup is computed from.
struct Pass {
  std::vector<bossq::QueryResult> results;
  double seconds = 0.0;
};

Pass runPass(const std::string &condition, int streamCount,
             const std::vector<streams::QueryInstance> &instances, double budget, out::Csv &csv,
             out::LogTail &tail) {
  Pass pass;
  tail.read(); // drain anything the previous pass left, so the first query's counters are its own

  auto passStart = Clock::now();
  for (auto const &instance : instances) {
    std::ostringstream id;
    id << condition << " N=" << streamCount << " stream=" << instance.streamId
       << " pos=" << instance.position << " " << instance.query;

    // The markers bracket this query in the instrumentation log, so every [VE] line between
    // them can be attributed to it when the log is read back.
    out::marker("BEGIN " + id.str());
    auto start = out::nowMs();
    auto result = bossq::runQuery(instance.query, instance.params);
    auto end = out::nowMs();
    out::marker("END " + id.str() + " seconds=" + out::fmt(result.seconds));

    auto cacheStats = tail.read(); // the cache events this one query caused

    csv.row({out::fmt(budget, 0),
             std::to_string(streamCount),
             condition,
             std::to_string(instance.streamId),
             std::to_string(instance.seed),
             std::to_string(instance.position),
             instance.query,
             instance.rawParams,
             result.ok ? "true" : "false",
             out::fmt(result.seconds),
             std::to_string(result.rows),
             out::fmt(result.checksum, 4),
             std::to_string(cacheStats.hits),
             std::to_string(cacheStats.misses),
             std::to_string(cacheStats.admits),
             std::to_string(cacheStats.evicts),
             std::to_string(cacheStats.drops),
             std::to_string(cacheStats.inlines),
             out::fmt(cacheStats.cacheBytes, 0),
             std::to_string(cacheStats.cacheViews),
             std::to_string(start),
             std::to_string(end),
             result.error});

    pass.results.push_back(std::move(result));
  }

  pass.seconds = since(passStart);
  return pass;
}

// Whether the view-enabled run computed what the plain run did. A failed query is not counted
// as a mismatch here, since there is no answer to compare; failures are tallied separately.
// The tolerance is relative because checksums run to eleven digits and float addition doesn't
// have to associate the same way once a view has reordered the work.
bool sameAnswer(const bossq::QueryResult &left, const bossq::QueryResult &right) {
  if (!left.ok || !right.ok)
    return true;
  if (left.rows != right.rows)
    return false;

  double scale = std::max({1.0, std::fabs(left.checksum), std::fabs(right.checksum)});
  return std::fabs(left.checksum - right.checksum) <= 1e-6 * scale;
}

} // namespace

std::vector<int> parseCounts(const char *arg) {
  std::vector<int> counts;
  if (arg && *arg) {
    std::istringstream stream(arg);
    std::string token;
    while (std::getline(stream, token, ','))
      counts.push_back(std::stoi(token));
  }

  if (counts.empty())
    counts = {4, 8, 16, 32, 64, 128, 256};
  return counts;
}

int run(const std::string &experiment, double budgetBytes, const std::vector<int> &streamCounts,
        const std::vector<std::string> &evictionPolicies) {
  auto dir = out::dirFor(experiment);
  out::redirectStderr(dir);
  out::SysInfoSampler sampler(dir);
  out::LogTail tail(dir + "/instrumentation.log");
  out::Csv csv(dir + "/results.csv", "budget_bytes,n_streams,condition,stream_id,seed,position,"
                                     "query,params,ok,seconds,rows,checksum,"
                                     "ve_hits,ve_misses,ve_admits,ve_evicts,ve_drops,ve_inlines,"
                                     "cache_bytes,cache_views,t_start_ms,t_end_ms,note");

  // With more than one policy the condition names get suffixed, so the CSV can tell them apart
  bool multi = evictionPolicies.size() > 1;
  std::cout << "=== " << experiment << ": BOSS stream experiment, cache budget "
            << budgetBytes / 1e9 << " GB, N =";
  for (int streamCount : streamCounts)
    std::cout << " " << streamCount;
  if (multi) {
    std::cout << ", policies =";
    for (auto const &policy : evictionPolicies)
      std::cout << " " << policy;
  }
  std::cout << "\n";

  // The tables are loaded once and stay resident for every stream count and policy below, so
  // nothing measured here includes load time.
  sampler.setPhase("load");
  out::marker("LOAD BEGIN");
  auto loadStart = Clock::now();
  if (!bossq::loadTables()) {
    std::cout << "table load failed\n";
    return 1;
  }
  out::marker("LOAD END");
  std::cout << "tables loaded in " << out::fmt(since(loadStart), 1) << "s\n";

  bool anyFailure = false;

  for (int streamCount : streamCounts) {
    auto instances = streams::generate(streamCount);
    std::cout << "\n--- N=" << streamCount << " (" << instances.size() << " instances) ---\n";

    // The plain pass runs once per stream count and is shared by every policy below it, so a
    // policy comparison is never against a differently-warmed baseline.
    sampler.setPhase("N=" + std::to_string(streamCount) + " plain");
    bossq::clearViews();
    out::marker("PASS plain N=" + std::to_string(streamCount));
    auto plain = runPass("plain", streamCount, instances, budgetBytes, csv, tail);
    std::cout << "plain: " << out::fmt(plain.seconds, 1) << "s\n";

    for (auto const &policy : evictionPolicies) {
      // Condition strings keep the historical "mech"/"mech_setup" names when there is only one
      // policy, so single-policy experiments (exp3/exp5) produce byte-compatible CSVs.
      std::string mechName = multi ? "mech_" + policy : "mech";
      sampler.setPhase("N=" + std::to_string(streamCount) + " " + mechName);
      bossq::clearViews();
      bossq::setCacheBudget(budgetBytes);

      if (!bossq::setEvictionPolicy(policy)) {
        std::cout << "SetEvictionPolicy(" << policy << ") refused\n";
        return 1;
      }
      // Defining all 253 views is charged to the experiment: it lands in the ALL speedup below,
      // so a design that is expensive to register can't hide that cost.
      out::marker("DEFINE VIEWS BEGIN N=" + std::to_string(streamCount) + " " + mechName);
      auto defineStart = Clock::now();
      auto failed = views::defineAll();
      double setupSeconds = since(defineStart);
      out::marker("DEFINE VIEWS END");

      std::string setupNote = failed.empty() ? "" : "refused: ";
      for (auto const &name : failed)
        setupNote += name + " ";

      csv.row({out::fmt(budgetBytes, 0),
               std::to_string(streamCount),
               mechName + "_setup",
               "",
               "",
               "",
               "ALL",
               "",
               failed.empty() ? "true" : "false",
               out::fmt(setupSeconds),
               std::to_string(views::all().size()),
               "0",
               "0",
               "0",
               "0",
               "0",
               "0",
               "0",
               "0",
               "0",
               std::to_string(out::nowMs()),
               std::to_string(out::nowMs()),
               setupNote});

      out::marker("PASS " + mechName + " N=" + std::to_string(streamCount));
      auto mech = runPass(mechName, streamCount, instances, budgetBytes, csv, tail);
      std::cout << mechName << ":  " << out::fmt(mech.seconds, 1) << "s (+"
                << out::fmt(setupSeconds, 2) << "s to define " << views::all().size() << " views"
                << (failed.empty() ? "" : ", some REFUSED") << ")\n";

      // Every instance is compared against its own plain twin at the same index, since both
      // passes walked the same instance list in the same order.
      struct Agg {
        int count = 0;
        double plain = 0, mech = 0;
      };
      std::map<std::string, Agg> byQuery;
      int mismatches = 0, failures = 0;

      for (size_t i = 0; i < instances.size(); ++i) {
        auto &agg = byQuery[instances[i].query];
        agg.count++;
        agg.plain += plain.results[i].seconds;
        agg.mech += mech.results[i].seconds;
        if (!plain.results[i].ok || !mech.results[i].ok)
          ++failures;
        if (!sameAnswer(plain.results[i], mech.results[i])) {
          ++mismatches;
          std::cout << "MISMATCH " << instances[i].query << " stream=" << instances[i].streamId
                    << " plain=" << out::fmt(plain.results[i].checksum, 4)
                    << " mech=" << out::fmt(mech.results[i].checksum, 4) << "\n";
        }
      }
      std::cout << "checksums: " << instances.size() - mismatches << "/" << instances.size()
                << " agree, " << failures << " failures\n";

      // One row per query shape, summed over that shape's instances across all the streams
      std::cout << std::left << std::setw(6) << "query" << std::right << std::setw(10) << "plain_s"
                << std::setw(10) << "mech_s" << std::setw(9) << "speedup" << "\n";

      for (auto const &[query, agg] : byQuery)
        std::cout << std::left << std::setw(6) << query << std::right << std::setw(10)
                  << out::fmt(agg.plain, 3) << std::setw(10) << out::fmt(agg.mech, 3)
                  << std::setw(8) << out::fmt(agg.mech > 0 ? agg.plain / agg.mech : 0, 2) << "x\n";

      // ALL is the headline number, and it charges the view definition cost that the per-query
      // rows above leave out, so it's always the more pessimistic of the two.
      std::cout << std::left << std::setw(6) << "ALL" << std::right << std::setw(10)
                << out::fmt(plain.seconds, 3) << std::setw(10)
                << out::fmt(mech.seconds + setupSeconds, 3) << std::setw(8)
                << out::fmt(plain.seconds / (mech.seconds + setupSeconds), 2) << "x\n";

      // One bad instance anywhere fails the whole run, so a silent wrong answer can't pass
      if (mismatches || failures)
        anyFailure = true;
    }
  }

  sampler.setPhase("done");
  return anyFailure ? 1 : 0;
}

} // namespace streamexp
