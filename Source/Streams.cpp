#include "Streams.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace streams {
namespace {

const std::string dbgenDir = DBGEN_DIR; // set in CMakeLists
// Where qgen is told to write its output, one file per stream
const std::string qgenLogDir = std::string(REPO_DIR) + "/generated/qgen";

// Q9 in this harness filters p_retailprice (BOSSKernelBenchmarks' choke point), not the spec's
// p_name LIKE, so its window is drawn from the real SF1 price domain with the original width.
constexpr double Q9_PRICE_LOW = 901.00, Q9_PRICE_HIGH = 2098.99, Q9_WIDTH = 74.05;

std::chrono::year_month_day parseDate(const std::string &iso) {
  int year = 0, month = 0, day = 0;
  char dash = 0;
  std::istringstream(iso) >> year >> dash >> month >> dash >> day;
  return {std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
          std::chrono::day{static_cast<unsigned>(day)}};
}

std::string formatDate(std::chrono::year_month_day ymd) {
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-'
         << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
         << static_cast<unsigned>(ymd.day());
  return stream.str();
}

// qgen has hung before, so every call gets a hard timeout rather than stalling the experiment
bool runShell(const std::string &command, int timeoutSec) {
  auto full =
      "timeout --signal=KILL " + std::to_string(timeoutSec) + "s " + command + " 2>/dev/null";
  return std::system(full.c_str()) == 0;
}

// qgen writes one tab-separated line per query: the query number, then its parameter values
std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    auto pos = line.find('\t', start);
    fields.push_back(
        line.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
    if (pos == std::string::npos)
      break;
    start = pos + 1;
  }
  return fields;
}

// splitmix64's constants
constexpr uint64_t MASTER_SEED = 0x9E3779B97F4A7C15ull;
constexpr uint64_t SPLITMIX_A = 0xBF58476D1CE4E5B9ull;
constexpr uint64_t SPLITMIX_B = 0x94D049BB133111EBull;

// Counter to seed. qgen's generator is seed * 16807 % 2147483647, so small sequential seeds
// give small, closely spaced first draws, which land on the same value once mapped onto a
// short domain like REGION. Scrambling spreads them.
unsigned scramble(uint64_t counter) {
  counter = (counter ^ (counter >> 30)) * SPLITMIX_A;
  counter = (counter ^ (counter >> 27)) * SPLITMIX_B;
  counter ^= counter >> 31;
  return static_cast<unsigned>(counter & 0xFFFFFFFFu); // qgen's -r takes 32 bits
}

// The two numbers qgen needs for each stream: which query ordering to use (-p) and which seed to
// draw parameter values from (-r). Both come off one fixed master seed, so an N=4 run gives the
// same four streams every time, and they are the first four an N=256 run uses.
std::vector<std::pair<int, unsigned>> streamSpecs(int streamCount) {
  std::vector<std::pair<int, unsigned>> specs;
  uint64_t counter = MASTER_SEED;

  for (int i = 0; i < streamCount; ++i) {
    counter += MASTER_SEED;
    unsigned seed = scramble(counter);

    // qgen's permutation table holds 41 orderings and indexes it modulo 41, so past 41 streams
    // the orderings come round again with fresh seeds.
    specs.emplace_back(301 + i, seed);
  }
  return specs;
}

// Turns qgen's raw fields into QueryParams. Field 0 is the query number, so the values start
// at 1. Where the spec gives a start date and an interval, qgen only emits the start, and the
// end is worked out here with the interval the spec names for that query.
void fillParams(QueryInstance &instance, const std::vector<std::string> &fields,
                std::mt19937 &q9Rng) {
  auto &params = instance.params;
  const auto &query = instance.query;

  if (query == "Q1")
    params.q1ShipdateBound = addDays("1998-12-01", -std::stoi(fields.at(1)));
  else if (query == "Q2") {
    params.q2Size = std::stoi(fields.at(1));
    params.q2TypeLike = "%" + fields.at(2);
    params.q2Region = fields.at(3);
  } else if (query == "Q3") {
    params.q3Segment = fields.at(1);
    params.q3Date = fields.at(2);
  } else if (query == "Q4") {
    params.q4DateLo = fields.at(1);
    params.q4DateHi = addMonths(fields.at(1), 3);
  } else if (query == "Q5") {
    params.q5Region = fields.at(1);
    params.q5DateLo = fields.at(2);
    params.q5DateHi = addYears(fields.at(2), 1);
  } else if (query == "Q6") {
    double discount = std::stod(fields.at(2));
    params.q6DateLower = addDays(fields.at(1), -1);
    params.q6DateUpper = addYears(fields.at(1), 1);
    params.q6DiscountLow = discount - 0.01 - 1e-4;
    params.q6DiscountHigh = discount + 0.01 + 1e-4;
    params.q6ShipLo = fields.at(1);
    params.q6DiscLo = discount - 0.01;
    params.q6DiscHi = discount + 0.01;
    params.q6Quantity = std::stoi(fields.at(3));
  } else if (query == "Q7") {
    params.q7Nation1 = fields.at(1);
    params.q7Nation2 = fields.at(2);
  } else if (query == "Q8") {
    params.q8Nation = fields.at(1);
    params.q8Region = fields.at(2);
    params.q8PartType = fields.at(3);
  } else if (query == "Q9") {
    // qgen has no parameter for this: our Q9 filters on price where the spec filters on a name
    // pattern, so the window is drawn here instead, from the real SF1 price domain.
    std::uniform_real_distribution<double> dist(Q9_PRICE_LOW, Q9_PRICE_HIGH - Q9_WIDTH);
    params.q9PriceLow = dist(q9Rng);
    params.q9PriceHigh = params.q9PriceLow + Q9_WIDTH;
  } else if (query == "Q10") {
    params.q10DateLo = fields.at(1);
    params.q10DateHi = addMonths(fields.at(1), 3);
  } else if (query == "Q11") {
    params.q11Nation = fields.at(1);
    params.q11Fraction = std::stod(fields.at(2));
  } else if (query == "Q12") {
    params.q12Mode1 = fields.at(1);
    params.q12Mode2 = fields.at(2);
    params.q12DateLo = fields.at(3);
    params.q12DateHi = addYears(fields.at(3), 1);
  } else if (query == "Q13") {
    params.q13Word1 = fields.at(1);
    params.q13Word2 = fields.at(2);
  } else if (query == "Q14") {
    params.q14DateLo = fields.at(1);
    params.q14DateHi = addMonths(fields.at(1), 1);
  } else if (query == "Q15") {
    params.q15DateLo = fields.at(1);
    params.q15DateHi = addMonths(fields.at(1), 3);
  } else if (query == "Q16") {
    params.q16Brand = fields.at(1);
    params.q16TypeNotLike = fields.at(2) + "%";
    params.q16Sizes.clear();
    for (int i = 3; i <= 10; ++i)
      params.q16Sizes.push_back(std::stoi(fields.at(i)));
  } else if (query == "Q17") {
    params.q17Brand = fields.at(1);
    params.q17Container = fields.at(2);
  } else if (query == "Q18")
    params.q18Quantity = std::stoi(fields.at(1));
  else if (query == "Q19") {
    params.q19Brand1 = fields.at(1);
    params.q19Brand2 = fields.at(2);
    params.q19Brand3 = fields.at(3);
    params.q19Qty1 = std::stod(fields.at(4));
    params.q19Qty2 = std::stod(fields.at(5));
    params.q19Qty3 = std::stod(fields.at(6));
  } else if (query == "Q20") {
    params.q20PartPrefix = fields.at(1);
    params.q20DateLo = fields.at(2);
    params.q20DateHi = addYears(fields.at(2), 1);
    params.q20Nation = fields.at(3);
  } else if (query == "Q21")
    params.q21Nation = fields.at(1);
  else if (query == "Q22")
    params.q22CountryCodes.assign(fields.begin() + 1, fields.begin() + 8);
}

std::vector<QueryInstance> generateStream(int streamId, unsigned seed) {
  std::filesystem::create_directories(qgenLogDir);
  std::string logPath =
      qgenLogDir + "/qgen_" + std::to_string(streamId) + "_" + std::to_string(seed) + ".log";
  std::error_code ec;
  std::filesystem::remove(logPath, ec); // qgen appends to -l, so a stale file would double up

  // -p picks the query ordering, -r the parameter seed, -l where to log what it generated.
  // The trailing 1..22 asks for every query in that stream.
  std::ostringstream cmd;
  cmd << "env DSS_QUERY=" << dbgenDir << "/queries " << dbgenDir << "/qgen -b " << dbgenDir
      << "/dists.dss -s 1 -p " << streamId << " -r " << seed << " -l " << logPath
      << " 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 > /dev/null";
  if (!runShell(cmd.str(), 30))
    throw std::runtime_error("qgen failed for stream " + std::to_string(streamId));

  // Its own generator, seeded off the stream's seed, so Q9's window is as reproducible as
  // everything qgen picked.
  std::ifstream log(logPath);
  std::mt19937 q9Rng(seed ^ 0x9E3779B9u);

  std::vector<QueryInstance> out;
  std::string line;
  while (std::getline(log, line)) {
    if (line.empty())
      continue;
    auto fields = splitTabs(line);

    QueryInstance instance;
    instance.query = "Q" + fields.at(0);
    instance.streamId = streamId;
    instance.seed = seed;
    instance.position = static_cast<int>(out.size());

    // rawParams keeps qgen's values verbatim, so results.csv records exactly what was asked for
    for (size_t i = 1; i < fields.size(); ++i)
      instance.rawParams += (i > 1 ? "|" : "") + fields[i];
    fillParams(instance, fields, q9Rng);

    if (instance.query == "Q13")
      instance.query = "Q13R"; // the streams run Q13's rewrite (viewQueries)
    out.push_back(std::move(instance));
  }
  if (out.empty())
    throw std::runtime_error("qgen produced no queries for stream " + std::to_string(streamId));
  return out;
}

} // namespace

std::string addDays(const std::string &iso, int days) {
  return formatDate(
      std::chrono::year_month_day{std::chrono::sys_days{parseDate(iso)} + std::chrono::days{days}});
}

std::string addMonths(const std::string &iso, int months) {
  return formatDate(parseDate(iso) + std::chrono::months{months});
}

std::string addYears(const std::string &iso, int years) {
  auto ymd = parseDate(iso);
  return formatDate({ymd.year() + std::chrono::years{years}, ymd.month(), ymd.day()});
}

// Every stream's instances, flattened into one list in stream order. The experiments walk it
// start to finish, so stream 0's 22 queries run before stream 1's.
std::vector<QueryInstance> generate(int nStreams) {
  std::vector<QueryInstance> all;

  for (auto const &[id, seed] : streamSpecs(nStreams)) {
    auto stream = generateStream(id, seed);
    all.insert(all.end(), std::make_move_iterator(stream.begin()),
               std::make_move_iterator(stream.end()));
  }
  return all;
}

} // namespace streams
