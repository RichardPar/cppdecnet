#!/bin/sh
# Start a config file on ours:  pcap-router-run.sh [config]
# Default: samples/pcap-router.conf.  A relative path is taken from where
# you are, not from the repo.
# Runs at realtime priority: SCHED_FIFO, $RTPRIO or 20.  RTPRIO=0 skips it.
# Needs root, or once:
#   sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice+eip build/release/bin/decnetd
CONF=$(realpath "${1:-samples/pcap-router.conf}" 2>/dev/null) || exit 1
cd "$(dirname "$0")/.." || exit 1
[ -r "$CONF" ] || { echo "cannot read $CONF" >&2; exit 1; }
[ "${RTPRIO-20}" = 0 ] && exec build/release/bin/decnetd "$CONF"
exec chrt -f "${RTPRIO:-20}" build/release/bin/decnetd "$CONF"
