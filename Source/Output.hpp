#pragma once

#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace out {

long long nowMs();

// <repo>/results/<experiment>, created if it isn't there already
std::string dirFor(const std::string &experiment);

// Points stderr at <dir>/instrumentation.log and turns on the engines' debug output. It's done at
// the file descriptor level because the engines are separate .so's writing to their own streams.
void redirectStderr(const std::string &dir);

// Drops a ">>> <text> t=<epoch ms>" line into the instrumentation log, so we can tell which
// engine output belongs to which query when reading it back
void marker(const std::string &text);

// results.csv, one row per measurement. Truncates whatever was there when it opens.
class Csv {
public:
  Csv(const std::string &path, const std::string &header);
  void row(std::initializer_list<std::string> fields);

private:
  std::ofstream out_;
};

// Keep precision when writing a double out as a string
std::string fmt(double value, int precision = 6);

// ViewEngine cache events parsed out of the instrumentation log since the previous read().
// The occupancy fields keep the last value we saw, so they persist across reads.
struct VeStats {
  int hits = 0, misses = 0, admits = 0, evicts = 0, drops = 0, inlines = 0;
  double cacheBytes = 0.0, cacheBudget = 0.0;
  int cacheViews = 0;
};

// Follows the instrumentation log while it's being written and hands back whatever is new
class LogTail {
public:
  explicit LogTail(const std::string &path);
  VeStats read();
  std::vector<std::string> readLines(); // complete new lines since the previous call

private:
  std::string path_;
  std::streamoff offset_ = 0;
  std::string leftover_; // the half line the last read stopped on, glued onto the next chunk
  VeStats lastStats_;    // counters restart each read, the occupancy fields carry over from here
};

// sysinfo.csv: RSS / swap / CPU / ViewEngine cache occupancy, sampled every 250ms on its own
// thread. The thread starts in the constructor and is joined in the destructor, so the sampler
// has to outlive whatever it's measuring.
class SysInfoSampler {
public:
  explicit SysInfoSampler(const std::string &dir);
  ~SysInfoSampler();
  void setPhase(const std::string &phase); // tags every sample from here on

private:
  void loop();
  std::string dir_, phase_;
  std::ofstream csv_;
  std::thread thread_;
  bool stop_ = false;
};

} // namespace out
