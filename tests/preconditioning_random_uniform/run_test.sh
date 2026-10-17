#!/usr/bin/env bash
# Regression check for the preconditioning RANDOM_UNIFORM crash (plan.md
# section 8 item 6): builds MQSim and runs it against a device and
# workload large enough to trigger it, checking for a clean exit. Slow
# on purpose: preconditioning alone takes a couple of minutes on a
# device this size, so this is not part of the fast default suite, run
# it deliberately. Usage:
# ./tests/preconditioning_random_uniform/run_test.sh (from the repo
# root, runs `make` first).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "Building MQSim..."
make

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

LOG="$WORKDIR/mqsim.log"
echo "Running MQSim (preconditioning alone takes a couple of minutes on this device)..."
if ! "$REPO_ROOT/MQSim" -i "$SCRIPT_DIR/ssdconfig.xml" -w "$SCRIPT_DIR/workload.xml" \
	< /dev/null > "$LOG" 2>&1; then
	echo "FAIL: MQSim exited with an error."
	cat "$LOG"
	exit 1
fi

echo "PASS: Enabled_Preconditioning with RANDOM_UNIFORM completed without crashing."
