#!/bin/sh
# Run decnetd at SCHED_FIFO priority $RTPRIO (default 20, 0 disables).
#   pcap-router-run.sh [config]        default samples/pcap-router.conf
# Needs root, or:
#   sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice+eip build/release/bin/decnetd
CONF=$(realpath "${1:-samples/pcap-router.conf}" 2>/dev/null) || exit 1
cd "$(dirname "$0")/.." || exit 1
[ -r "$CONF" ] || { echo "cannot read $CONF" >&2; exit 1; }
[ "${RTPRIO-20}" = 0 ] && exec build/release/bin/decnetd "$CONF"
exec chrt -f "${RTPRIO:-20}" build/release/bin/decnetd "$CONF"
