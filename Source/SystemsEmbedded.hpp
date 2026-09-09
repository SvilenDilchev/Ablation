#pragma once
#include <string>

namespace sysemb {

struct Result {
  bool ok = false;
  double seconds = 0.0;
  std::string error;
};

// Two runners per system: one pinned to a single thread, one on MultiKernelBOSS's own defaults
// (DuckDB maximum_threads=100, MonetDB left on its normal optimiser pipeline). We pin DuckDB
// with maximum_threads=1 and MonetDB with optimizer='sequential_pipe'.
Result duckdb(const std::string &sql);
Result duckdbMulti(const std::string &sql);
Result monetdbe(const std::string &sql);
Result monetdbeMulti(const std::string &sql);

// Drops the in-memory database. Whatever runs next reloads it from scratch.
void releaseDuckdb();
void releaseMonetdbe();

std::string duckdbVersion();
std::string monetdbeVersion();

} // namespace sysemb
