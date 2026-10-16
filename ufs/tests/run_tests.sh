#!/bin/bash
# Build and run the UFS unit/integration tests against the project's objects.
# Usage: ./ufs/tests/run_tests.sh   (from the repo root, runs `make` first)
set -e
cd "$(dirname "$0")/../.."
make -j4 > /dev/null
OBJS=$(find build -name '*.o' ! -name main.o)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
status=0
for t in test_construction test_arbitration test_e2e test_completion; do
	g++ -std=c++11 -g -Isrc/host -Isrc/ssd -Isrc/sim -Isrc/exec -Isrc/utils -Isrc \
		"ufs/tests/$t.cpp" $OBJS -o "$TMP/$t"
	# Each test binary gets a fresh process: they share the engine singleton, and
	# MQSim's cin.get()s require stdin at EOF (always run MQSim binaries with
	# stdin redirected from /dev/null in automation, or a finished run hangs)
	if "$TMP/$t" < /dev/null; then
		echo "[$t] OK"
	else
		echo "[$t] FAILED"
		status=1
	fi
done
exit $status
