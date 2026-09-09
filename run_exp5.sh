#!/bin/bash
# This guy's different cuz it puts a physical RAM cap
set -e
cd "$(dirname "$0")"
if pgrep -x "Exp[0-5]" > /dev/null; then echo "another experiment is running; abort"; exit 1; fi
mkdir -p results/exp5
# set -e would kill the script here before the exit code got written, which is the one case
# worth recording, so it comes off for the run itself
set +e
systemd-run --user --scope -p MemoryMax=5952M -p MemorySwapMax=6G \
  ./build/Exp5 "$@" > results/exp5/stdout.log 2>&1
status=$?
set -e

echo "Exp5 exit=$status" >> results/exp5/stdout.log
exit $status
