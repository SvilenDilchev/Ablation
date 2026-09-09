#!/usr/bin/env bash
# Builds everything and runs experiments 0-4 back to back
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)"

exps=("$@")
[ $# -eq 0 ] && exps=(Exp0 Exp1 Exp2 Exp3 Exp4)
for exp in "${exps[@]}"; do
  out="results/${exp,,}"   # the binaries write to a lower case results dir
  mkdir -p "$out"
  echo "===== $exp  $(date) ====="
  "./build/$exp" | tee "$out/stdout.log"
done

