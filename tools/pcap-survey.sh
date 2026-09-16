#!/bin/bash
# Capture DECnet traffic on an interface and list the stations heard.
# Capture only, nothing is transmitted.
#
#   sudo tools/pcap-survey.sh [interface] [seconds]

set -u

IFACE=${1:-enx00051be19c68}
DUR=${2:-90}
OUT=${OUTDIR:-/tmp}/survey-$(date +%Y%m%d-%H%M%S)
PCAP=$OUT.pcap
TXT=$OUT.txt

# Phase IV MAC is AA-00-04-00 + (area * 1024 + node), little endian.
WANT_MAC=aa:00:04:00:14:04   # 1.20
PDP_MAC=aa:00:04:00:13:04    # 1.19

[ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "no such interface: $IFACE" >&2; exit 1; }

echo "interface : $IFACE"
ip -br link show "$IFACE"
echo "duration  : ${DUR}s"
echo "capture   : $PCAP"
echo

# 6001 MOP dump/load, 6002 MOP console, 6003 routing, 9000 loopback
FILTER='ether proto 0x6003 or ether proto 0x6002 or ether proto 0x6001 or ether proto 0x9000'

echo "listening (Ctrl-C to stop early)..."
timeout "$DUR" tcpdump -i "$IFACE" -s 0 -w "$PCAP" "$FILTER" 2>&1 | sed 's/^/  tcpdump: /'
rc=${PIPESTATUS[0]}
[ "$rc" -eq 124 ] || [ "$rc" -eq 0 ] || echo "  (tcpdump exit $rc)"
echo

[ -s "$PCAP" ] || { echo "capture file is empty; tcpdump did not start"; exit 1; }

tcpdump -r "$PCAP" -e -nn -vv -XX > "$TXT" 2>/dev/null
FRAMES=$(tcpdump -r "$PCAP" -nn 2>/dev/null | wc -l)

echo "=== result ==="
echo "frames captured: $FRAMES"
echo

if [ "$FRAMES" -eq 0 ]; then
    echo "Nothing heard.  Check the interface sees any traffic:"
    echo "  sudo timeout 10 tcpdump -i $IFACE -nn -c 20"
fi

echo "--- stations heard ---"
tcpdump -r "$PCAP" -e -nn 2>/dev/null \
  | awk '{ for (i=1;i<=NF;i++) if ($i ~ /^aa:00:04:00:[0-9a-f]{2}:[0-9a-f]{2}$/) { print $i; break } }' \
  | sort | uniq -c | sort -rn \
  | while read -r n mac; do
        lo=$(echo "$mac" | cut -d: -f5); hi=$(echo "$mac" | cut -d: -f6)
        v=$(( 0x$hi * 256 + 0x$lo ))
        printf "  %-20s %-10s %5d frames\n" "$mac" "$((v >> 10)).$((v & 1023))" "$n"
    done
echo

echo "--- hello destinations ---"
echo "  ab:00:00:03:00:00 = endnode hellos"
echo "  ab:00:00:04:00:00 = router hellos"
tcpdump -r "$PCAP" -e -nn 2>/dev/null \
  | grep -oE '(ab|cf|09):00:00:[0-9a-f]{2}:00:00' | sort | uniq -c | sed 's/^/  /'
echo

echo "--- IS 1.20 FREE? ---"
if tcpdump -r "$PCAP" -e -nn 2>/dev/null | grep -qi "$WANT_MAC"; then
    echo "  No, $WANT_MAC is in use."
else
    echo "  Yes, $WANT_MAC not seen."
fi
echo

echo "--- IS THE PDP (1.19) THERE? ---"
if tcpdump -r "$PCAP" -e -nn 2>/dev/null | grep -qi "$PDP_MAC"; then
    echo "  Yes, heard $PDP_MAC."
else
    echo "  Not heard."
fi
echo

echo "--- first routing frames ---"
tcpdump -r "$PCAP" -e -nn -vv -XX 'ether proto 0x6003' 2>/dev/null | head -60

echo
echo "full text log : $TXT"
echo "raw capture   : $PCAP"

if [ -n "${SUDO_USER:-}" ]; then chown "$SUDO_USER" "$PCAP" "$TXT" 2>/dev/null; fi
