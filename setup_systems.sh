#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p systems && cd systems

if [ ! -f duckdb-0.8.1/libduckdb.so ]; then
  mkdir -p duckdb-0.8.1
  wget -q -O duckdb-0.8.1/libduckdb.zip \
    https://github.com/duckdb/duckdb/releases/download/v0.8.1/libduckdb-linux-amd64.zip
  unzip -o -q -d duckdb-0.8.1 duckdb-0.8.1/libduckdb.zip
  rm duckdb-0.8.1/libduckdb.zip
fi
echo "duckdb-0.8.1 ready"

if [ ! -f monetdb-jun2023/install/lib/libmonetdbe.so ]; then
  git clone --depth 1 --branch Jun2023_SP2_release \
    https://github.com/MonetDB/MonetDB monetdb-src
  cmake -S monetdb-src -B monetdb-build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/monetdb-jun2023/install" -DTESTING=OFF -DODBC=OFF
  cmake --build monetdb-build -j"$(nproc)"
  cmake --install monetdb-build
  rm -rf monetdb-src monetdb-build
fi
echo "monetdb-jun2023 ready"

if [ ! -f benchmark/install/lib/libbenchmark.a ]; then
  wget -q -O benchmark.zip https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
  unzip -q benchmark.zip
  rm benchmark.zip
  cmake -S benchmark-1.8.3 -B benchmark-build -DCMAKE_BUILD_TYPE=Release \
    -DBENCHMARK_ENABLE_TESTING=OFF -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/benchmark/install"
  cmake --build benchmark-build -j"$(nproc)"
  cmake --install benchmark-build
  rm -rf benchmark-1.8.3 benchmark-build
fi
echo "google-benchmark ready"
