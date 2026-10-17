#!/usr/bin/env bash
# Regression check for GC_Block_Selection_Policy=EXTERNAL (plan.md section 8
# item 9): drives MQSim's live per-decision GC hook with a controller that
# switches the chosen action on every single decision, the condition that
# found and fixed a real race there. Usage: ./tests/external_gc_policy/run_test.sh
# (from the repo root, runs `make` first).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "Building MQSim..."
make

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

STATE_PIPE="$WORKDIR/state.pipe"
ACTION_PIPE="$WORKDIR/action.pipe"
mkfifo "$STATE_PIPE" "$ACTION_PIPE"

sed -e "s#__STATE_PIPE__#$STATE_PIPE#" -e "s#__ACTION_PIPE__#$ACTION_PIPE#" \
	"$SCRIPT_DIR/ssdconfig.xml" > "$WORKDIR/ssdconfig.xml"
cp "$SCRIPT_DIR/workload.xml" "$WORKDIR/workload.xml"

NUM_EPISODES=10
for ep in $(seq 1 "$NUM_EPISODES"); do
	"$REPO_ROOT/MQSim" -i "$WORKDIR/ssdconfig.xml" -w "$WORKDIR/workload.xml" \
		< /dev/null > "$WORKDIR/mqsim.log" 2>&1 &
	MQSIM_PID=$!

	# The controller: answer every decision request with a switched action
	# (0..4, cycling through GREEDY/RGA/RANDOM/RANDOM_P/FIFO), the same
	# per-decision policy switching that found the race this test guards
	# against. MQSim blocks opening these pipes until this script opens the
	# matching end, the standard named-pipe handshake.
	exec 3<"$STATE_PIPE"
	exec 4>"$ACTION_PIPE"
	decisions=0
	while IFS= read -r _line <&3; do
		action=$(( (RANDOM + ep * 7) % 5 ))
		echo "$action" >&4
		decisions=$((decisions + 1))
	done
	exec 3<&-
	exec 4>&-

	set +e
	wait "$MQSIM_PID"
	exit_code=$?
	set -e
	if [ "$exit_code" -ne 0 ]; then
		echo "FAIL: episode $ep exited with status $exit_code after $decisions decisions."
		cat "$WORKDIR/mqsim.log"
		exit 1
	fi
done

echo "PASS: EXTERNAL GC policy completed $NUM_EPISODES episodes of switched decisions without crashing."
