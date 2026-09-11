#!/bin/bash
# Catch the rare test_eventlog hang and dump its thread stacks.
#
# Needs root: /proc/sys/kernel/yama/ptrace_scope is 1 on this machine, so
# gdb cannot attach to a process that is not its own descendant.
#
#   sudo /tmp/.../catch-hang.sh            # 80 attempts, default
#   sudo /tmp/.../catch-hang.sh 200        # more patience
#
# The hang appeared about once in 26 runs with every core busy, so the load
# is part of the recipe, not incidental.

set -u
cd /home/richard/Source/Decnet/cppdecnet
OUT=${OUT:-/tmp/eventlog-hang}
ATTEMPTS=${1:-80}
WAIT=${WAIT:-45}
mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || { echo "needs root, for gdb to attach: sudo $0 $*" >&2; exit 1; }

BIN=./build/release/bin/test_eventlog
[ -x "$BIN" ] || { echo "build it first: make BUILD=release" >&2; exit 1; }

echo "loading $(nproc) cpus, then up to $ATTEMPTS runs, ${WAIT}s patience each"
for i in $(seq 1 "$(nproc)"); do (timeout $((ATTEMPTS * WAIT + 120)) yes > /dev/null &); done
sleep 1

for attempt in $(seq 1 "$ATTEMPTS"); do
    "$BIN" > "$OUT/run.log" 2>&1 &
    pid=$!
    for t in $(seq 1 "$WAIT"); do
        sleep 1
        kill -0 "$pid" 2>/dev/null || break
    done

    if kill -0 "$pid" 2>/dev/null; then
        echo "HUNG on attempt $attempt (pid $pid) -- dumping stacks"
        gdb -p "$pid" -batch \
            -ex "set pagination off" \
            -ex "info threads" \
            -ex "thread apply all bt full" \
            > "$OUT/stacks.txt" 2>&1
        kill -9 "$pid" 2>/dev/null
        pkill -x yes
        chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
        echo
        echo "stacks : $OUT/stacks.txt"
        echo "log    : $OUT/run.log"
        echo
        echo "--- threads ---"
        grep -E "^\s*[0-9]+\s+Thread|^Thread" "$OUT/stacks.txt" | head -20
        exit 0
    fi
    wait "$pid" 2>/dev/null
    printf '.'
done

pkill -x yes
echo
echo "no hang in $ATTEMPTS attempts"
