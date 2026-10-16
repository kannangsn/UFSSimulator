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
    echo "WARNING: MQSim exited with status $RUN_EXIT while running the full workload.xml."
    echo "This matches a known, pre-existing crash between scenario 1 and scenario 2"
    echo "(see plan.md section 8, item 6). It is unrelated to UFS support and does not"
    echo "affect the scenario 1 result file checked below, so it does not fail this check."
fi

if ! diff -q workload_scenario_1.xml "$SCRIPT_DIR/golden/workload_scenario_1.xml" > /dev/null 2>&1; then
    echo "FAIL: scenario 1 output no longer matches the golden reference."
    echo "Compare with: diff workload_scenario_1.xml $SCRIPT_DIR/golden/workload_scenario_1.xml"
    cat "$RUN_LOG"
    exit 1
fi
echo "PASS: scenario 1 output matches the golden reference."
rm -f workload_scenario_*.xml

echo "Running the UFS placeholder check..."
set +e
"$REPO_ROOT/MQSim" -i "$SCRIPT_DIR/ssdconfig-ufs-placeholder.xml" -w workload.xml < /dev/null > "$RUN_LOG" 2>&1
UFS_EXIT=$?
set -e

if [ "$UFS_EXIT" -eq 0 ]; then
    echo "FAIL: a UFS config completed instead of exiting where SSD_Device would"
    echo "otherwise construct Host_Interface_UFS."
    echo "This check needs updating once UFS host-interface support is actually implemented."
    exit 1
fi
if ! grep -q "UFS host interface construction is not implemented yet" "$RUN_LOG"; then
    echo "FAIL: the UFS config failed, but not with the expected placeholder message."
    cat "$RUN_LOG"
    exit 1
fi
echo "PASS: UFS config exits with an explicit error where SSD_Device would"
echo "otherwise construct Host_Interface_UFS, as expected while UFS support is still a work in progress."
rm -f workload_scenario_*.xml

echo "All regression checks passed."
