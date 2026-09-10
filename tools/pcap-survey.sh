#!/bin/bash
# Survey a segment for DECnet traffic before putting a node on it.
#
# READ ONLY.  This script captures and never transmits, so it cannot
# disturb anything already on the wire.  Run it before decnetd, because
# claiming a node address that is already in use is worse than not being
# on the segment at all.
#
#   sudo ./pcap-survey.sh [interface] [seconds]
#
# Defaults match samples/pcap.conf.

set -u

IFACE=${1:-enx00051be19c68}
DUR=${2:-90}
OUT=${OUTDIR:-/tmp}/survey-$(date +%Y%m%d-%H%M%S)
PCAP=$OUT.pcap
TXT=$OUT.txt

# The addresses samples/pcap.conf cares about.  A Phase IV station's MAC is
# AA-00-04-00 followed by (area * 1024 + node) as a 16 bit little endian
# value, so 1.19 is ...13:04 and 1.20 is ...14:04.
WANT_MAC=aa:00:04:00:14:04   # 1.20, the address we intend to claim
PDP_MAC=aa:00:04:00:13:04    # 1.19, the PDP we expect to find

[ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "no such interface: $IFACE" >&2; exit 1; }

echo "interface : $IFACE"
ip -br link show "$IFACE"
echo "duration  : ${DUR}s"
echo "capture   : $PCAP"
echo

# 6001 MOP dump/load, 6002 MOP remote console, 6003 DECnet Phase IV routing,
# 9000 the Ethernet loopback protocol MOP loopback rides on.
FILTER='ether proto 0x6003 or ether proto 0x6002 or ether proto 0x6001 or ether proto 0x9000'

echo "listening (Ctrl-C to stop early)..."
timeout "$DUR" tcpdump -i "$IFACE" -s 0 -w "$PCAP" "$FILTER" 2>&1 | sed 's/^/  tcpdump: /'
rc=${PIPESTATUS[0]}
[ "$rc" -eq 124 ] || [ "$rc" -eq 0 ] || echo "  (tcpdump exit $rc)"
echo

[ -s "$PCAP" ] || { echo "capture file is empty -- tcpdump did not start"; exit 1; }

# Full detail with hex, so the frames can be decoded after the fact without
# needing the segment again.
tcpdump -r "$PCAP" -e -nn -vv -XX > "$TXT" 2>/dev/null
FRAMES=$(tcpdump -r "$PCAP" -nn 2>/dev/null | wc -l)

echo "=============== RESULT ==============="
echo "frames captured: $FRAMES"
echo

if [ "$FRAMES" -eq 0 ]; then
    echo "NOTHING HEARD."
    echo "Either the PDP is not on this segment, it is powered off, or the"
    echo "link is not carrying DECnet.  ${DUR}s covers several hello intervals,"
    echo "so a live Phase IV node should have been heard."
    echo
    echo "Sanity check that the interface sees ANY traffic at all:"
    echo "  sudo timeout 10 tcpdump -i $IFACE -nn -c 20"
fi

echo "--- DECnet stations heard (source address -> area.node) ---"
tcpdump -r "$PCAP" -e -nn 2>/dev/null \
  | awk '{ for (i=1;i<=NF;i++) if ($i ~ /^aa:00:04:00:[0-9a-f]{2}:[0-9a-f]{2}$/) { print $i; break } }' \
  | sort | uniq -c | sort -rn \
  | while read -r n mac; do
        lo=$(echo "$mac" | cut -d: -f5); hi=$(echo "$mac" | cut -d: -f6)
        v=$(( 0x$hi * 256 + 0x$lo ))
        printf "  %-20s %-10s %5d frames\n" "$mac" "$((v >> 10)).$((v & 1023))" "$n"
    done
echo "  (no output above = no Phase IV station heard)"
echo

echo "--- destinations (says who is a router and who is an endnode) ---"
echo "  ab:00:00:03:00:00 = where ENDNODE hellos go (routers listen)"
echo "  ab:00:00:04:00:00 = where ROUTER  hellos go (endnodes listen)"
tcpdump -r "$PCAP" -e -nn 2>/dev/null \
  | grep -oE '(ab|cf|09):00:00:[0-9a-f]{2}:00:00' | sort | uniq -c | sed 's/^/  /'
echo

echo "--- IS 1.20 FREE? ---"
if tcpdump -r "$PCAP" -e -nn 2>/dev/null | grep -qi "$WANT_MAC"; then
    echo "  NO.  $WANT_MAC is ALREADY IN USE."
    echo "  Do NOT start decnetd with samples/pcap.conf -- pick another node"
    echo "  number and change both the 'node' and 'routing' lines."
else
    echo "  Nothing claimed $WANT_MAC during the capture.  1.20 looks free."
fi
echo

echo "--- IS THE PDP (1.19) THERE? ---"
if tcpdump -r "$PCAP" -e -nn 2>/dev/null | grep -qi "$PDP_MAC"; then
    echo "  YES.  Heard $PDP_MAC on this segment."
else
    echo "  Not heard.  It may be off, silent, or on another segment."
fi
echo

echo "--- first routing frames, with hex (node type and priority live here) ---"
tcpdump -r "$PCAP" -e -nn -vv -XX 'ether proto 0x6003' 2>/dev/null | head -60

echo
echo "======================================"
echo "full text log : $TXT"
echo "raw capture   : $PCAP"
echo "Paste the RESULT block back, or just point Claude at the text log."

# Hand the files back to the user who invoked sudo.
if [ -n "${SUDO_USER:-}" ]; then chown "$SUDO_USER" "$PCAP" "$TXT" 2>/dev/null; fi
