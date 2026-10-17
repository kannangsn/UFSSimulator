#!/usr/bin/env bash
# Regression check for the GC write-completion race (plan.md section 8,
# items 8/9/10): RANDOM_PP on this GC-stress fixture used to crash
# deterministically with "Inconsistency in the global mapping table when
# locking an LPA!" partway through, since a GC destination write's
# on-flash metadata was not actually written yet when another GC
# candidate search looked at it. Usage:
# ./tests/gc_write_completion_race/run_test.sh (from the repo root, runs
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
	echo "FAIL: RANDOM_PP exited with an error."
	cat "$LOG"
	exit 1
fi

echo "PASS: RANDOM_PP completed the GC-stress fixture without crashing."
