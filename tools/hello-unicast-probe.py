#!/usr/bin/env python3
"""Send a router hello to a station's unicast address instead of the multicast.

If the station then adopts 1.20 as its designated router, it is not
receiving the DECnet multicast addresses.

    sudo tools/hello-unicast-probe.py <iface> <mac> [count]

The frame is a captured decnetd hello:

    router-hello l1rout vers 2 src 1.20 blksize 591 pri 64 hello 10
"""

import os
import socket
import struct
import sys
import time

ROUTING_PROTO = 0x6003

# Length field, 27 byte hello, padding.
HELLO_BODY = bytes.fromhex(
    "1b00"
    "0b020000aa0004001404024f0240000a"
    "00000800000000000000000000000000"
    "000000000000000000000000")

SRC = bytes.fromhex("aa0004001404")     # 1.20


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

    print("\nCheck the far end with NCP> SHOW ADJACENT NODES")
    return 0


if __name__ == '__main__':
    sys.exit(main())
