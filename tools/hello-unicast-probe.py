#!/usr/bin/env python3
"""Send a router hello to one station's own address instead of the multicast.

A diagnostic for one specific failure: we hear a neighbour, the neighbour
does not hear us.  Router hellos go to the all-endnodes multicast
AB-00-00-04-00-00, so a station whose Ethernet address filter was never
programmed with the DECnet addresses will transmit happily and receive
nothing.  That looks identical to a routing fault and is not one.

This sends the same hello to the station's own hardware address, which it
demonstrably does receive.  If it then adopts us as its designated router,
the multicast filter is the problem and the fault is on that station, not
here.

    sudo tools/hello-unicast-probe.py enx00051be19c68 08:00:2b:11:22:33

Then on the far end (RSX):  NCP> SHOW ADJACENT NODES

The frame is a verbatim replay of one decnetd put on the wire, captured on
10-Sep-2026 and decoded by tcpdump as

    router-hello l1rout vers 2 src 1.20 blksize 591 pri 64 hello 10

Replayed rather than rebuilt on purpose: a hello assembled here could be
subtly wrong, and then a station ignoring it would prove nothing.
"""

import os
import socket
import struct
import sys
import time

ROUTING_PROTO = 0x6003

# Everything from the ethertype onward, exactly as captured: the DEC padded
# length field 001b, the 27 byte hello, then the pad to minimum frame size.
HELLO_BODY = bytes.fromhex(
    "1b00"
    "0b020000aa0004001404024f0240000a"
    "00000800000000000000000000000000"
    "000000000000000000000000")

SRC = bytes.fromhex("aa0004001404")     # 1.20, the address decnetd sends from


def mac_bytes(s):
    parts = s.replace('-', ':').split(':')
    if len(parts) != 6:
        raise ValueError("bad MAC address: " + s)
    return bytes(int(p, 16) for p in parts)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    iface, target = args[0], args[1]
    count = int(args[2]) if len(args) > 2 else 20
    interval = 5.0

    if os.geteuid() != 0:
        print("needs root: sudo %s %s" % (sys.argv[0], ' '.join(args)))
        return 1

    dst = mac_bytes(target)
    frame = dst + SRC + struct.pack('!H', ROUTING_PROTO) + HELLO_BODY

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                      socket.htons(ROUTING_PROTO))
    s.bind((iface, 0))

    print("interface : %s" % iface)
    print("hello     : router-hello, l1 router, 1.20, prio 64, %d bytes"
          % len(frame))
    print("to        : %s  (its own address, not the multicast)" % target)
    print("sending %d, one every %.0f seconds\n" % (count, interval))

    for n in range(1, count + 1):
        s.send(frame)
        print("  %2d sent  %s" % (n, time.strftime("%H:%M:%S")))
        if n != count:
            time.sleep(interval)

    print("\nNow ask the far end whether it found a router:")
    print("    NCP> SHOW ADJACENT NODES")
    print("\nIt names 1.20 or CPPNOD  -> it receives unicast but not the")
    print("  DECnet multicast. Its Ethernet address filter is the fault.")
    print("Still 'No information'   -> it is not accepting our hello for a")
    print("  reason other than addressing; capture and compare.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
