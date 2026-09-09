#include "Output.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace out {
namespace {

std::mutex phaseMutex;

// Only quote when we have to, and double up any quote that's already in there
std::string csvEscape(const std::string &field) {
  if (field.find_first_of(",\"\n") == std::string::npos)
    return field;

  std::string quoted = "\"";
  for (char character : field)
    quoted += character == '"' ? "\"\"" : std::string(1, character);
  return quoted + "\"";
}

// Pulls one "Key: <n> kB" line out of a /proc file, 0 if it isn't there. Every memory number
// we track comes through here: VmRSS / VmHWM / VmSwap for this process, MemAvailable and
// SwapTotal-SwapFree for the machine.
long readKb(const std::string &path, const std::string &key) {
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line))
    if (line.rfind(key, 0) == 0)
      return std::stol(line.substr(key.size()));
  return 0;
}

// Total CPU this process has burned, user plus kernel, in clock ticks. This is what tells a
// query that's genuinely computing apart from one that's sat blocked on swap.
// Fields 14 and 15 of /proc/self/stat, utime and stime. The comm field before them is
// parenthesised and may itself contain spaces or brackets, so we start counting after its
// closing one rather than splitting the whole line.
long long procTicks() {
  std::ifstream file("/proc/self/stat");
  std::string statLine((std::istreambuf_iterator<char>(file)), {});
  auto after = statLine.substr(statLine.rfind(')') + 2);

  std::istringstream fields(after);
  std::string field;
  long long utime = 0, stime = 0;
  for (int i = 0; fields >> field; ++i) {
    if (i == 11)
      utime = std::stoll(field);
    if (i == 12) {
      stime = std::stoll(field);
      break;
    }
  }
  return utime + stime;
}

// Machine-wide busy/idle split from /proc/stat's first line. We count iowait as idle: the
// experiments swap under memory pressure, and blocking on that isn't the CPU doing work.
void sysCpu(long long &total, long long &idle) {
  std::ifstream file("/proc/stat");
  std::string label;
  long long fields[10] = {};
  file >> label; // the leading "cpu" word, discarded
  for (auto &field : fields)
    file >> field;

  total = 0;
  for (auto field : fields)
    total += field;
  idle = fields[3] + fields[4]; // idle plus iowait
}

} // namespace

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string dirFor(const std::string &experiment) {
  std::string dir = std::string(REPO_DIR) + "/results/" + experiment;
  std::filesystem::create_directories(dir);
  return dir;
}

// Redirects at the file-descriptor level, not via std::cerr's buffer: the engines are separate
// .so's writing to their own streams, and only fd 2 catches all of them in one file.
void redirectStderr(const std::string &dir) {
  int logFd = open((dir + "/instrumentation.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  dup2(logFd, 2); // fd 2 is stderr, so every .so in the process follows
  close(logFd);
  setenv("BOSS_VE_DEBUG", "1", 1);
}

void marker(const std::string &text) { std::cerr << ">>> " << text << " t=" << nowMs() << "\n"; }

Csv::Csv(const std::string &path, const std::string &header) : out_(path, std::ios::trunc) {
  out_ << header << "\n";
  out_.flush();
}

// Flushed on every row, so a run that dies partway still leaves usable data behind
void Csv::row(std::initializer_list<std::string> fields) {
  bool first = true;
  for (auto const &field : fields) {
    out_ << (first ? "" : ",") << csvEscape(field);
    first = false;
  }
  out_ << "\n";
  out_.flush();
}

// std::fixed matters: without it the big checksums come back in scientific notation
std::string fmt(double value, int precision) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

LogTail::LogTail(const std::string &path) : path_(path) {}

// Reads whatever's been appended since last time. The chunk usually stops mid line because the
// log is still being written, so only complete lines come back and the rest waits in leftover_.
std::vector<std::string> LogTail::readLines() {
  std::vector<std::string> lines;
  std::ifstream file(path_);
  if (!file)
    return lines;

  // pick up from wherever the last read stopped
  file.seekg(offset_);
  std::string chunk((std::istreambuf_iterator<char>(file)), {});
  offset_ += static_cast<std::streamoff>(chunk.size());
  leftover_ += chunk;

  // take every complete line, stop at the last newline we can find
  size_t start = 0;
  while (true) {
    auto newline = leftover_.find('\n', start);
    if (newline == std::string::npos)
      break;
    lines.push_back(leftover_.substr(start, newline - start));
    start = newline + 1;
  }
  leftover_.erase(0, start); // the tail with no newline yet waits for the next call
  return lines;
}

// Counters are per-read deltas so they start from zero, but the occupancy fields carry the
// last value we saw. A read with no PASS-END line keeps the old occupancy instead of claiming 0.
VeStats LogTail::read() {
  VeStats stats = lastStats_;
  stats.hits = stats.misses = stats.admits = stats.evicts = stats.drops = stats.inlines = 0;
  for (auto const &line : readLines()) {
    if (line.rfind("[VE] ", 0) != 0)
      continue;
    if (line.rfind("[VE] HIT ", 0) == 0)
      ++stats.hits;
    else if (line.rfind("[VE] MISS ", 0) == 0 && line.find(" size=") != std::string::npos)
      ++stats.misses;
    else if (line.rfind("[VE] ADMIT ", 0) == 0)
      ++stats.admits;
    else if (line.rfind("[VE] EVICT ", 0) == 0)
      ++stats.evicts;
    else if (line.rfind("[VE] DROP ", 0) == 0)
      ++stats.drops;
    else if (line.rfind("[VE] INLINE ", 0) == 0)
      ++stats.inlines;
    else if (line.rfind("[VE] PASS-END occupancy=", 0) == 0) {
      double occupancy = 0, budget = 0;
      int views = 0;
      if (sscanf(line.c_str(), "[VE] PASS-END occupancy=%lf/%lf cached=%d", &occupancy, &budget,
                 &views) == 3) {
        stats.cacheBytes = occupancy;
        stats.cacheBudget = budget;
        stats.cacheViews = views;
      }
    }
  }
  lastStats_ = stats;
  return stats;
}

SysInfoSampler::SysInfoSampler(const std::string &dir)
    : dir_(dir), phase_("start"), csv_(dir + "/sysinfo.csv", std::ios::trunc) {
  csv_ << "t_ms,phase,rss_mb,hwm_mb,swap_mb,mem_available_mb,swap_used_mb,proc_cpu_pct,sys_cpu_pct,"
          "cache_bytes,cache_budget_bytes,cache_views\n";
  thread_ = std::thread([this] { loop(); });
}

SysInfoSampler::~SysInfoSampler() {
  stop_ = true;
  if (thread_.joinable())
    thread_.join();
}

void SysInfoSampler::setPhase(const std::string &phase) {
  std::lock_guard<std::mutex> lock(phaseMutex);
  phase_ = phase;
}

// One row into sysinfo.csv every 250ms, until the destructor sets stop_. What we track:
//   rss / hwm / swap    this process's memory. hwm is the peak, which is the number exp4 and
//                       exp5 are really after
//   mem_available       the machine's headroom, so we can see pressure that isn't ours
//   swap_used           machine-wide swap in use
//   proc_cpu_pct        ~100 is one core busy. Confirms the -1T runs really are single threaded,
//                       and near 0 while wall time climbs means we're thrashing, not computing
//   sys_cpu_pct         the same for the whole machine
//   cache_*             ViewEngine occupancy, scraped back out of the instrumentation log
// The CPU percentages are deltas against the previous tick, so the first sample is measured
// from whenever the thread started.
void SysInfoSampler::loop() {
  LogTail tail(dir_ + "/instrumentation.log");

  // first readings, so the first sample has something to be a delta against
  long long lastTicks = procTicks(), lastTotal = 0, lastIdle = 0;
  sysCpu(lastTotal, lastIdle);
  auto lastSampleTime = std::chrono::steady_clock::now();
  const double ticksPerSecond = static_cast<double>(sysconf(_SC_CLK_TCK));

  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - lastSampleTime).count();
    long long ticks = procTicks(), total = 0, idle = 0;
    sysCpu(total, idle);

    // ticks burned since the last sample, over the wall time it actually took
    double processCpuPercent =
        elapsed > 0 ? 100.0 * (ticks - lastTicks) / ticksPerSecond / elapsed : 0.0;
    double systemCpuPercent =
        total > lastTotal ? 100.0 * (1.0 - double(idle - lastIdle) / double(total - lastTotal))
                          : 0.0;

    // carry these into the next round
    lastTicks = ticks;
    lastTotal = total;
    lastIdle = idle;
    lastSampleTime = now;

    auto cacheStats = tail.read(); // whatever ViewEngine logged since we last looked

    // setPhase gets called from the experiment thread, so this one needs the lock
    std::string phase;
    {
      std::lock_guard<std::mutex> lock(phaseMutex);
      phase = phase_;
    }

    csv_ << nowMs() << "," << csvEscape(phase) << ","
         << readKb("/proc/self/status", "VmRSS:") / 1024 << ","
         << readKb("/proc/self/status", "VmHWM:") / 1024 << ","
         << readKb("/proc/self/status", "VmSwap:") / 1024 << ","
         << readKb("/proc/meminfo", "MemAvailable:") / 1024 << ","
         << (readKb("/proc/meminfo", "SwapTotal:") - readKb("/proc/meminfo", "SwapFree:")) / 1024
         << "," << fmt(processCpuPercent, 1) << "," << fmt(systemCpuPercent, 1) << ","
         << fmt(cacheStats.cacheBytes, 0) << "," << fmt(cacheStats.cacheBudget, 0) << ","
         << cacheStats.cacheViews << "\n";
    csv_.flush(); // flushed every row, a run that gets killed still keeps what it had
  }
}

} // namespace out
