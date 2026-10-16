#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "Building MQSim..."
make

RUN_LOG="$(mktemp)"
trap 'rm -f "$RUN_LOG"' EXIT

echo "Running the pre-existing NVMe workload (scenario 1 of workload.xml)..."
set +e
"$REPO_ROOT/MQSim" -i ssdconfig.xml -w workload.xml < /dev/null > "$RUN_LOG" 2>&1
RUN_EXIT=$?
set -e

if [ "$RUN_EXIT" -ne 0 ]; then
    echo "FAIL: MQSim exited with status $RUN_EXIT while running the full workload.xml."
    cat "$RUN_LOG"
    exit 1
fi

if ! diff -q workload_scenario_1.xml "$SCRIPT_DIR/golden/workload_scenario_1.xml" > /dev/null 2>&1; then
    echo "FAIL: scenario 1 output no longer matches the golden reference."
    echo "Compare with: diff workload_scenario_1.xml $SCRIPT_DIR/golden/workload_scenario_1.xml"
    cat "$RUN_LOG"
    exit 1
fi
echo "PASS: scenario 1 output matches the golden reference."
rm -f workload_scenario_*.xml

echo "Running the UFS workload (scenario 1 of workload.xml, UFS config)..."
set +e
"$REPO_ROOT/MQSim" -i "$SCRIPT_DIR/ssdconfig-ufs.xml" -w workload.xml < /dev/null > "$RUN_LOG" 2>&1
UFS_EXIT=$?
set -e

if [ "$UFS_EXIT" -ne 0 ]; then
    echo "FAIL: MQSim exited with status $UFS_EXIT while running the UFS workload."
    cat "$RUN_LOG"
    exit 1
fi

if ! diff -q workload_scenario_1.xml "$SCRIPT_DIR/golden/workload_scenario_1_ufs.xml" > /dev/null 2>&1; then
    echo "FAIL: UFS scenario 1 output no longer matches the golden reference."
    echo "Compare with: diff workload_scenario_1.xml $SCRIPT_DIR/golden/workload_scenario_1_ufs.xml"
    cat "$RUN_LOG"
    exit 1
fi
echo "PASS: UFS scenario 1 output matches the golden reference."
rm -f workload_scenario_*.xml

echo "All regression checks passed."
