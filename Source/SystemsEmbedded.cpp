#include "SystemsEmbedded.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <duckdb.hpp>
#include <monetdb/monetdbe.h>

namespace sysemb {
namespace {

// SF1 .tbl files, loaded into both systems as in-memory databases
const std::string tpchDir = TPCH_DATA_DIR; // set in CMakeLists
const std::string tableNames[] = {"region",   "nation",   "part",   "supplier",
                               "partsupp", "customer", "orders", "lineitem"};

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- DuckDB
// MultiKernelBOSS's schema: DECIMAL(15,2) numerics, BIGINT keys, CHAR/VARCHAR widths, PK+FK
// constraints, and a trailing "dummy" column to absorb dbgen's line-final '|'.
struct DuckDBHandling {
  std::unique_ptr<duckdb::DBConfig> config;
  duckdb::DuckDB db;
  duckdb::Connection con;

  explicit DuckDBHandling(bool multithreaded)
      : config(makeConfig(multithreaded)), db(":memory:", config.get()), con(db) {
    const std::vector<std::string> createCmds{
        R"(CREATE TABLE region (
             r_regionkey INTEGER NOT NULL,
             r_name CHAR(25) NOT NULL,
             r_comment VARCHAR(152),
             dummy VARCHAR,
             PRIMARY KEY(r_regionkey)
           );)",
        R"(CREATE TABLE nation (
             n_nationkey INTEGER NOT NULL,
             n_name CHAR(25) NOT NULL,
             n_regionkey INTEGER NOT NULL,
             n_comment VARCHAR(152),
             dummy VARCHAR,
             PRIMARY KEY(n_nationkey),
             FOREIGN KEY (n_regionkey) REFERENCES region(r_regionkey)
           );)",
        R"(CREATE TABLE part (
             p_partkey BIGINT NOT NULL,
             p_name VARCHAR(55) NOT NULL,
             p_mfgr CHAR(25) NOT NULL,
             p_brand CHAR(10) NOT NULL,
             p_type VARCHAR(25) NOT NULL,
             p_size INTEGER NOT NULL,
             p_container CHAR(10) NOT NULL,
             p_retailprice DECIMAL(15,2) NOT NULL,
             p_comment VARCHAR(23) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(p_partkey)
           );)",
        R"(CREATE TABLE supplier (
             s_suppkey BIGINT NOT NULL,
             s_name CHAR(25) NOT NULL,
             s_address VARCHAR(40) NOT NULL,
             s_nationkey INTEGER NOT NULL,
             s_phone CHAR(15) NOT NULL,
             s_acctbal DECIMAL(15,2) NOT NULL,
             s_comment VARCHAR(101) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(s_suppkey),
             FOREIGN KEY (s_nationkey) REFERENCES nation(n_nationkey)
           );)",
        R"(CREATE TABLE partsupp (
             ps_partkey BIGINT NOT NULL,
             ps_suppkey BIGINT NOT NULL,
             ps_availqty BIGINT NOT NULL,
             ps_supplycost DECIMAL(15,2) NOT NULL,
             ps_comment VARCHAR(199) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(ps_partkey, ps_suppkey),
             FOREIGN KEY (ps_partkey) REFERENCES part(p_partkey),
             FOREIGN KEY (ps_suppkey) REFERENCES supplier(s_suppkey)
           );)",
        R"(CREATE TABLE customer (
             c_custkey BIGINT NOT NULL,
             c_name VARCHAR(25) NOT NULL,
             c_address VARCHAR(40) NOT NULL,
             c_nationkey INTEGER NOT NULL,
             c_phone CHAR(15) NOT NULL,
             c_acctbal DECIMAL(15,2) NOT NULL,
             c_mktsegment CHAR(10) NOT NULL,
             c_comment VARCHAR(117) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(c_custkey),
             FOREIGN KEY (c_nationkey) REFERENCES nation(n_nationkey)
           );)",
        R"(CREATE TABLE orders (
             o_orderkey BIGINT NOT NULL,
             o_custkey BIGINT NOT NULL,
             o_orderstatus CHAR(1) NOT NULL,
             o_totalprice DECIMAL(15,2) NOT NULL,
             o_orderdate DATE NOT NULL,
             o_orderpriority CHAR(15) NOT NULL,
             o_clerk CHAR(15) NOT NULL,
             o_shippriority INTEGER NOT NULL,
             o_comment VARCHAR(79) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(o_orderkey),
             FOREIGN KEY (o_custkey) REFERENCES customer(c_custkey)
           );)",
        R"(CREATE TABLE lineitem (
             l_orderkey BIGINT NOT NULL,
             l_partkey BIGINT NOT NULL,
             l_suppkey BIGINT NOT NULL,
             l_linenumber BIGINT NOT NULL,
             l_quantity DECIMAL(15,2) NOT NULL,
             l_extendedprice DECIMAL(15,2) NOT NULL,
             l_discount DECIMAL(15,2) NOT NULL,
             l_tax DECIMAL(15,2) NOT NULL,
             l_returnflag CHAR(1) NOT NULL,
             l_linestatus CHAR(1) NOT NULL,
             l_shipdate DATE NOT NULL,
             l_commitdate DATE NOT NULL,
             l_receiptdate DATE NOT NULL,
             l_shipinstruct CHAR(25) NOT NULL,
             l_shipmode CHAR(10) NOT NULL,
             l_comment VARCHAR(44) NOT NULL,
             dummy VARCHAR,
             PRIMARY KEY(l_orderkey, l_linenumber),
             FOREIGN KEY (l_orderkey) REFERENCES orders(o_orderkey),
             FOREIGN KEY (l_partkey,l_suppkey) REFERENCES partsupp(ps_partkey,ps_suppkey)
           );)"};
    for (auto const &cmd : createCmds) {
      auto result = con.Query(cmd);
      if (result->HasError())
        std::cerr << result->GetError() << "\n";
    }
    for (auto const &table : tableNames) {
      auto insertCmd = "INSERT INTO " + table + " SELECT * FROM read_csv_auto('" + tpchDir + "/" +
                       table + ".tbl');";
      auto result = con.Query(insertCmd);
      if (result->HasError())
        std::cerr << result->GetError() << "\n";
    }
  }

  static std::unique_ptr<duckdb::DBConfig> makeConfig(bool multithreaded) {
    auto config = std::make_unique<duckdb::DBConfig>();
    config->options.maximum_threads = multithreaded ? 100 : 1;
    config->options.temporary_directory = "duckdbtemp";
    return config;
  }
};

// One handle per mode, built on first use and kept until someone releases it
std::unique_ptr<DuckDBHandling> &duckHandle(bool multithreaded) {
  static std::unique_ptr<DuckDBHandling> handle1;
  static std::unique_ptr<DuckDBHandling> handleM;
  return multithreaded ? handleM : handle1;
}

duckdb::Connection &duckCon(bool multithreaded) {
  auto &handle = duckHandle(multithreaded);
  if (!handle)
    handle = std::make_unique<DuckDBHandling>(multithreaded);
  return handle->con;
}

// ---------------------------------------------------------------- MonetDB/e
struct MonetHandling {
  monetdbe_database db = nullptr;

  explicit MonetHandling(bool multithreaded) {
    if (monetdbe_open(&db, nullptr, nullptr))
      throw std::runtime_error("monetdbe_open failed");
    // sequential_pipe is how MultiKernelBOSS pins MonetDB to one thread. The multithreaded
    // arm doesn't touch it, so MonetDB keeps its normal optimiser pipeline.
    if (!multithreaded)
      exec("set optimizer='sequential_pipe';");
    exec("START TRANSACTION;");
    const std::vector<std::string> createCmds{
        R"(CREATE TABLE region (
             r_regionkey INTEGER NOT NULL,
             r_name CHAR(25) NOT NULL,
             r_comment VARCHAR(152)
           );)",
        R"(CREATE TABLE nation (
             n_nationkey INTEGER NOT NULL,
             n_name CHAR(25) NOT NULL,
             n_regionkey INTEGER NOT NULL,
             n_comment VARCHAR(152)
           );)",
        R"(CREATE TABLE part (
             p_partkey BIGINT NOT NULL,
             p_name VARCHAR(55) NOT NULL,
             p_mfgr CHAR(25) NOT NULL,
             p_brand CHAR(10) NOT NULL,
             p_type VARCHAR(25) NOT NULL,
             p_size INTEGER NOT NULL,
             p_container CHAR(10) NOT NULL,
             p_retailprice DECIMAL(15,2) NOT NULL,
             p_comment VARCHAR(23) NOT NULL
           );)",
        R"(CREATE TABLE supplier (
             s_suppkey BIGINT NOT NULL,
             s_name CHAR(25) NOT NULL,
             s_address VARCHAR(40) NOT NULL,
             s_nationkey INTEGER NOT NULL,
             s_phone CHAR(15) NOT NULL,
             s_acctbal DECIMAL(15,2) NOT NULL,
             s_comment VARCHAR(101) NOT NULL
           );)",
        R"(CREATE TABLE partsupp (
             ps_partkey BIGINT NOT NULL,
             ps_suppkey BIGINT NOT NULL,
             ps_availqty BIGINT NOT NULL,
             ps_supplycost DECIMAL(15,2) NOT NULL,
             ps_comment VARCHAR(199) NOT NULL
           );)",
        R"(CREATE TABLE customer (
             c_custkey BIGINT NOT NULL,
             c_name VARCHAR(25) NOT NULL,
             c_address VARCHAR(40) NOT NULL,
             c_nationkey INTEGER NOT NULL,
             c_phone CHAR(15) NOT NULL,
             c_acctbal DECIMAL(15,2) NOT NULL,
             c_mktsegment CHAR(10) NOT NULL,
             c_comment VARCHAR(117) NOT NULL
           );)",
        R"(CREATE TABLE orders (
             o_orderkey BIGINT NOT NULL,
             o_custkey BIGINT NOT NULL,
             o_orderstatus CHAR(1) NOT NULL,
             o_totalprice DECIMAL(15,2) NOT NULL,
             o_orderdate DATE NOT NULL,
             o_orderpriority CHAR(15) NOT NULL,
             o_clerk CHAR(15) NOT NULL,
             o_shippriority INTEGER NOT NULL,
             o_comment VARCHAR(79) NOT NULL
           );)",
        R"(CREATE TABLE lineitem (
             l_orderkey BIGINT NOT NULL,
             l_partkey BIGINT NOT NULL,
             l_suppkey BIGINT NOT NULL,
             l_linenumber BIGINT NOT NULL,
             l_quantity DECIMAL(15,2) NOT NULL,
             l_extendedprice DECIMAL(15,2) NOT NULL,
             l_discount DECIMAL(15,2) NOT NULL,
             l_tax DECIMAL(15,2) NOT NULL,
             l_returnflag CHAR(1) NOT NULL,
             l_linestatus CHAR(1) NOT NULL,
             l_shipdate DATE NOT NULL,
             l_commitdate DATE NOT NULL,
             l_receiptdate DATE NOT NULL,
             l_shipinstruct CHAR(25) NOT NULL,
             l_shipmode CHAR(10) NOT NULL,
             l_comment VARCHAR(44) NOT NULL
           );)"};
    for (auto const &cmd : createCmds)
      exec(cmd);
    const std::vector<std::string> pkCmds{
        "ALTER TABLE part ADD CONSTRAINT part_kpey PRIMARY KEY (p_partkey);",
        "ALTER TABLE supplier ADD CONSTRAINT supplier_pkey PRIMARY KEY (s_suppkey);",
        "ALTER TABLE partsupp ADD CONSTRAINT partsupp_pkey PRIMARY KEY (ps_partkey, ps_suppkey);",
        "ALTER TABLE customer ADD CONSTRAINT customer_pkey PRIMARY KEY (c_custkey);",
        "ALTER TABLE orders ADD CONSTRAINT orders_pkey PRIMARY KEY (o_orderkey);",
        "ALTER TABLE lineitem ADD CONSTRAINT lineitem_pkey PRIMARY KEY (l_orderkey, l_linenumber);",
        "ALTER TABLE nation ADD CONSTRAINT nation_pkey PRIMARY KEY (n_nationkey);",
        "ALTER TABLE region ADD CONSTRAINT region_pkey PRIMARY KEY (r_regionkey);"};
    for (auto const &cmd : pkCmds)
      exec(cmd);
    const std::vector<std::string> fkCmds{
        R"(ALTER TABLE supplier ADD CONSTRAINT supplier_nation_fkey
             FOREIGN KEY (s_nationkey) REFERENCES nation(n_nationkey);)",
        R"(ALTER TABLE partsupp ADD CONSTRAINT partsupp_part_fkey
             FOREIGN KEY (ps_partkey) REFERENCES part(p_partkey);)",
        R"(ALTER TABLE partsupp ADD CONSTRAINT partsupp_supplier_fkey
             FOREIGN KEY (ps_suppkey) REFERENCES supplier(s_suppkey);)",
        R"(ALTER TABLE customer ADD CONSTRAINT customer_nation_fkey
             FOREIGN KEY (c_nationkey) REFERENCES nation(n_nationkey);)",
        R"(ALTER TABLE orders ADD CONSTRAINT orders_customer_fkey
             FOREIGN KEY (o_custkey) REFERENCES customer(c_custkey);)",
        R"(ALTER TABLE lineitem ADD CONSTRAINT lineitem_orders_fkey
             FOREIGN KEY (l_orderkey) REFERENCES orders(o_orderkey);)",
        R"(ALTER TABLE lineitem ADD CONSTRAINT lineitem_partsupp_fkey
             FOREIGN KEY (l_partkey,l_suppkey) REFERENCES partsupp(ps_partkey,ps_suppkey);)",
        R"(ALTER TABLE nation ADD CONSTRAINT nation_region_fkey
             FOREIGN KEY (n_regionkey) REFERENCES region(r_regionkey);)"};
    for (auto const &cmd : fkCmds)
      exec(cmd);
    for (auto const &table : tableNames)
      exec("COPY INTO " + table + " FROM '" + tpchDir + "/" + table +
           ".tbl' USING DELIMITERS '|', '\n';");
    exec("COMMIT;");
  }

  // monetdbe_query wants a mutable char*, so the sql gets copied first
  void exec(const std::string &sql) {
    monetdbe_result *queryResult = nullptr;
    std::string sqlCopy = sql;
    char *err = monetdbe_query(db, sqlCopy.data(), &queryResult, nullptr);

    if (queryResult)
      monetdbe_cleanup_result(db, queryResult);
    if (err)
      throw std::runtime_error("monetdbe setup failed: " + std::string(err));
  }

  ~MonetHandling() { monetdbe_close(db); }
};

std::unique_ptr<MonetHandling> &monetHandle(bool multithreaded) {
  static std::unique_ptr<MonetHandling> handle1;
  static std::unique_ptr<MonetHandling> handleM;
  return multithreaded ? handleM : handle1;
}

monetdbe_database monet(bool multithreaded) {
  // MonetDB/e allows one in-memory database per process: close the other mode first.
  auto &other = monetHandle(!multithreaded);
  auto &handle = monetHandle(multithreaded);
  if (!handle) {
    other.reset();
    handle = std::make_unique<MonetHandling>(multithreaded);
  }
  return handle->db;
}

// The clock only covers Query(), same as MultiKernelBOSS. Connecting and loading sit outside it.
Result duckdbRun(const std::string &sql, bool multithreaded) {
  Result result;
  auto &con = duckCon(multithreaded);

  auto t0 = std::chrono::steady_clock::now();
  auto queryResult = con.Query(sql);
  result.seconds = seconds(t0);

  result.ok = queryResult != nullptr && !queryResult->HasError();
  if (!result.ok && queryResult)
    result.error = queryResult->GetError();
  return result;
}

// Same again for MonetDB. Opening the database can throw, and that's a failed run rather than
// a crash, so we catch it and report it back through Result.
Result monetdbeRun(const std::string &sql, bool multithreaded) {
  Result result;
  monetdbe_database db;

  try {
    db = monet(multithreaded);
  } catch (std::exception const &ex) {
    result.error = ex.what();
    return result;
  }

  monetdbe_result *queryResult = nullptr;
  std::string sqlCopy = sql;

  auto t0 = std::chrono::steady_clock::now();
  char *err = monetdbe_query(db, sqlCopy.data(), &queryResult, nullptr);
  result.seconds = seconds(t0);

  if (queryResult)
    monetdbe_cleanup_result(db, queryResult);
  result.ok = err == nullptr;
  if (err)
    result.error = err;
  return result;
}

} // namespace

// What exp1 actually calls
Result duckdb(const std::string &sql) { return duckdbRun(sql, false); }
Result duckdbMulti(const std::string &sql) { return duckdbRun(sql, true); }
Result monetdbe(const std::string &sql) { return monetdbeRun(sql, false); }
Result monetdbeMulti(const std::string &sql) { return monetdbeRun(sql, true); }

// Drops both modes' handles. Whatever asks next rebuilds and reloads from the .tbl files.
void releaseDuckdb() {
  duckHandle(false).reset();
  duckHandle(true).reset();
}
void releaseMonetdbe() {
  monetHandle(false).reset();
  monetHandle(true).reset();
}

std::string duckdbVersion() { return std::string("DuckDB ") + duckdb::DuckDB::LibraryVersion(); }
std::string monetdbeVersion() { return "MonetDB/e 11.47.11 (Jun2023_SP2)"; }

} // namespace sysemb
