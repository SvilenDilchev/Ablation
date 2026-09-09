# Ablation

The five experiments behind the ViewEngine evaluation.

## 1. Build the comparison systems

```
./setup_systems.sh
```

Fetches DuckDB 0.8.1, MonetDB Jun2023_SP2 and google benchmark 1.8.3 into `systems/`. Takes a
while the first time, mostly MonetDB. Skips anything already there.

## 2. Check the paths

All three engines are fetched and built by step 3 into `build/deps/lib`, each pinned to the
commit the evaluation was run against. ArrowComputeEngine builds Arrow from source, so the
first build takes a while. Two paths point outside the repo, hardcoded to where they sit on my
server, so pass them to cmake rather than editing anything:

| variable | what |
|---|---|
| `TPCH_DATA_DIR` | the TPC-H .tbl files |
| `DBGEN_DIR` | the TPC-H dbgen directory, for the qgen binary |

Everything else is worked out from where the repo is, so it can be checked out anywhere.

## 3. Compile

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## 4. Run

```
./run_all.sh                 # Exp0 through Exp4, in order
./run_all.sh Exp1            # or just one
./run_exp5.sh                # Exp5 on its own, it needs a systemd-run memory cap
```

Each binary writes `results.csv`, `instrumentation.log` and `sysinfo.csv` into `results/<exp>/`,
truncating whatever was there before. The run scripts add `stdout.log` on top, so running a
binary directly gives you three files and running it through a script gives you four.

## 5. Get the numbers out

One script per table. `--csv` on any of them for comma separated instead.

```
python3 scripts/best_views.py            # the view design per query
python3 scripts/three_system_bench.py    # seconds per query, all systems
python3 scripts/three_system_ratios.py   # BOSS against each of them
python3 scripts/paper_ratios.py          # ours against the BOSS paper's figure 13
python3 scripts/splicing_overhead.py     # what Lite mode costs
python3 scripts/throughput.py            # per-query speedup at each stream count
python3 scripts/speedup_curve.py         # overall speedup against N
python3 scripts/policy_results.py        # the three eviction policies
python3 scripts/physical_results.py      # the physical memory cap
```
