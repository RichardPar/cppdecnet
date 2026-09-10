#!/usr/bin/env python3
"""Ask a station to loop a frame back, over the Ethernet loopback protocol.

Answers one question: is that station receiving unicast frames we send to
this address?  It runs entirely at the Ethernet level -- it claims no
DECnet node address and speaks no routing -- so a result here is about the
wire and the far station's address filter, nothing else.

That is the point.  A node announces a Phase IV id of aa-00-04-00-xx-xx
inside its hellos but may transmit from, and listen on, the address its
card came with.  If so, multicast hellos work and unicast data vanishes.
Probing both addresses in turn says which one it answers to.

    sudo tools/mop-loop-probe.py enx00051be19c68 aa:00:04:00:13:04
    sudo tools/mop-loop-probe.py enx00051be19c68 08:00:2b:11:22:33

Needs root, for the raw socket.  The message layout matches
LoopSkip/LoopFwd/LoopReply in include/decnet/mop/packets.h, so what this
sends is what decnetd would send.
"""

import fcntl
import os
import socket
import struct
import sys
import time

LOOP_PROTO = 0x9000
FUNC_REPLY = 1          # LoopReply::function_code
FUNC_FORWARD = 2        # LoopFwd::function_code


def mac_bytes(s):
    parts = s.replace('-', ':').split(':')
    if len(parts) != 6:
        raise ValueError("bad MAC address: " + s)
    return bytes(int(p, 16) for p in parts)


def mac_str(b):
    return ':'.join('%02x' % c for c in b)


def build(forward_to, receipt, data):
    """A loop message: forward to us, then reply.

    The far station reads the first function, "forward to this address",
    and sends the message back with the skip count advanced past it.  We
    then read the second, "reply", and that round trip is the answer.
    """
    msg = struct.pack('<H', 0)                  # skip count, we start at 0
    msg += struct.pack('<H', FUNC_FORWARD)      # function: forward data
    msg += forward_to                           # ... to here: us
    msg += struct.pack('<H', FUNC_REPLY)        # function: reply
    msg += struct.pack('<H', receipt)
    msg += data
    return msg


def parse_reply(payload, expect_receipt):
    """A reply has the skip count stepped past the forward block, 8 bytes."""
    if len(payload) < 6:
        return None
    skip = struct.unpack_from('<H', payload, 0)[0]
    if skip + 4 > len(payload):
        return None
    func = struct.unpack_from('<H', payload, 2 + skip)[0]
    if func != FUNC_REPLY:
        return None
    receipt = struct.unpack_from('<H', payload, 4 + skip)[0]
    if receipt != expect_receipt:
        return None
    return payload[6 + skip:]


def own_mac(iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        info = fcntl.ioctl(s.fileno(), 0x8927,  # SIOCGIFHWADDR
                           struct.pack('256s', iface.encode()[:15]))
        return info[18:24]
    finally:
        s.close()


def probe(iface, target, count, timeout):
    src = own_mac(iface)
    dst = mac_bytes(target)

    print("interface : %s (%s)" % (iface, mac_str(src)))
    print("target    : %s" % mac_str(dst))
    print("protocol  : 0x%04x, Ethernet loopback\n" % LOOP_PROTO)

    tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                       socket.htons(LOOP_PROTO))
    tx.bind((iface, 0))
    tx.settimeout(timeout)

    good = 0
    for n in range(1, count + 1):
        receipt = n
        data = b'cppdecnet loop probe'
        msg = build(src, receipt, data)
        frame = dst + src + struct.pack('!H', LOOP_PROTO) + msg
        frame += b'\x00' * max(0, 60 - len(frame))   # Ethernet minimum

        tx.send(frame)
        sent = time.time()

        deadline = sent + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                print("  %d: no reply" % n)
                break
            tx.settimeout(left)
            try:
                buf = tx.recv(2048)
            except socket.timeout:
                print("  %d: no reply" % n)
                break
            if len(buf) < 14:
                continue
            if struct.unpack_from('!H', buf, 12)[0] != LOOP_PROTO:
                continue
            if buf[6:12] == src:            # our own frame, echoed by the tap
                continue
            body = parse_reply(buf[14:], receipt)
            if body is None:
                continue
            ms = (time.time() - sent) * 1000.0
            print("  %d: reply from %s in %.1f ms" % (n, mac_str(buf[6:12]), ms))
            good += 1
            break
        time.sleep(0.3)

    print("\n%d of %d replied." % (good, count))
    if good:
        print("That station receives unicast sent to %s." % mac_str(dst))
    else:
        print("No reply. Either it does not listen on %s, or it does not\n"
              "answer the loopback protocol at all. Try the other address\n"
              "before concluding anything." % mac_str(dst))
    return 0 if good else 1


def self_test():
    """Check the encoding without a wire, so the layout can be reviewed."""
    src = mac_bytes('00:05:1b:e1:9c:68')
    msg = build(src, 1, b'ab')
    print("built:", ' '.join('%02x' % c for c in msg))
    assert msg[0:2] == b'\x00\x00',                    "skip must start 0"
    assert struct.unpack_from('<H', msg, 2)[0] == 2,   "forward function"
    assert msg[4:10] == src,                           "forward address"
    assert struct.unpack_from('<H', msg, 10)[0] == 1,  "reply function"
    assert struct.unpack_from('<H', msg, 12)[0] == 1,  "receipt"
    assert msg[14:] == b'ab',                          "payload"
    # What comes back: same message, skip stepped past the 8 byte forward.
    returned = struct.pack('<H', 8) + msg[2:]
    assert parse_reply(returned, 1) == b'ab',          "reply parse"
    assert parse_reply(returned, 2) is None,           "receipt mismatch"
    print("self test passed: layout matches mop/packets.h")
    return 0


def main():
    args = sys.argv[1:]
    if args and args[0] == '--self-test':
        return self_test()
    if len(args) < 2:
        print(__doc__)
        return 2
    iface, target = args[0], args[1]
    count = int(args[2]) if len(args) > 2 else 5
    if os.geteuid() != 0:
        print("needs root: sudo %s %s" % (sys.argv[0], ' '.join(args)))
        return 1
    return probe(iface, target, count, 2.0)


if __name__ == '__main__':
    sys.exit(main())
