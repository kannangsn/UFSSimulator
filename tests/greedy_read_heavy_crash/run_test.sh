#!/usr/bin/env bash
# Regression check for the GREEDY read-heavy crash (plan.md section 8,
# item 14): GREEDY used to crash a read-heavy workload's one GC decision
# with "Inconsistency in the global mapping table when locking an LPA!".
# Select_Block_GREEDY seeded its candidate with block 0 (or block 1) and
# only ever replaced that default with a block that passed
# is_safe_gc_wl_candidate, never checking the default itself. When no
# other block had more invalid pages yet (true this early, on a workload
# this read-heavy), GREEDY returned that unvalidated default, which could
# still have a GC write reservation that had not actually finished on the
# flash chip yet, the same race class as items 8-11. Usage:
# ./tests/greedy_read_heavy_crash/run_test.sh (from the repo root, runs
# `make` first).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "Building MQSim..."
make

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

LOG="$WORKDIR/mqsim.log"
if ! "$REPO_ROOT/MQSim" -i "$SCRIPT_DIR/ssdconfig.xml" -w "$SCRIPT_DIR/workload.xml" \
	< /dev/null > "$LOG" 2>&1; then
	echo "FAIL: GREEDY exited with an error on the read-heavy workload."
	cat "$LOG"
	exit 1
fi

echo "PASS: GREEDY completed the read-heavy workload's one GC decision without crashing."
