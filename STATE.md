# Where the work stopped

Written 2026-09-10. This is a session handover note, not a permanent
document -- delete it once the two open items below are closed.

## Done and verified

**Events and the event logger** (phase 6). All tests pass.

- `include/decnet/nice/entity.h`, `src/nice/entity.cc` -- NiceNode and the
  entity kinds
- `include/decnet/nice/params.h`, `src/nice/params.cc` -- parameter lists,
  counters (CTR) and mapped counters (CTM), display styles
- `include/decnet/events/events.h`, `src/events/events.cc` -- the record
  format and the timestamp arithmetic
- `src/events/tables.cc` -- the catalogue: names, severities, parameter
  meanings
- `include/decnet/events/logger.h`, `src/events/logger.cc` -- event lists,
  filters, console/file/monitor sinks, remote sinks over object 26, and
  the receiving end
- `tests/test_event.cc` (16 tests, against pydecnet's own byte vectors)
  and `tests/test_eventlog.cc` (19 tests, including a record crossing a
  real circuit between two nodes)

Events are raised from routing, NSP and node state changes.

**The pcap datalink.** `PcapEthernet` in `include/decnet/datalink/ethernet.h`
and `src/datalink/ethernet.cc`, plus `BcDatalink::filter_expression()` in
`src/datalink/bc.cc`. Compiled and unit tested; **never yet run on a real
segment** -- see below.

**Two defects fixed**, both written up in `BUGS.md`:
- an event raised from inside a routing state function outran the state
  change, so the connect it triggered was dropped by `send_raw`
- `Node::stop()` stopped the layers from the caller's thread while the
  main loop was still dispatching into them

## Open

**1. The shutdown race fix is not yet stress verified.**

The fix is in `Node::stop()` / `Node::stop_layers()` and builds clean, and
`make check` passes. What has *not* been done is the loop that proves the
intermittent failure is gone. Before this fix, `test_nsp` aborted under
ASan about one run in thirty. Run:

    for i in $(seq 1 40); do
        ./build/debug/bin/test_nsp > /tmp/nsp.log 2>&1 || {
            echo "FAILED run $i"; grep -E 'ERROR|runtime error' /tmp/nsp.log; }
    done

Forty clean runs is the evidence. Fewer is not.

**2. The pcap circuit has never touched real hardware.**

There is a real PDP at node 1.19 on the wired segment. `samples/pcap.conf`
is written for it. Two commands need a password, so they have to be run by
hand:

    sudo setcap cap_net_raw,cap_net_admin+eip build/release/bin/decnetd

    sudo timeout 15 tcpdump -i enx00051be19c68 -e -nn -c 20 \
        'ether proto 0x6003 or ether proto 0x6002 or ether proto 0x6001'

Take the capture first. It says whether the PDP is on that segment, what
area and node type it is, and whether 1.20 is free -- and picking a node
address that is already in use is worse than not being on the segment at
all. Then:

    ./build/release/bin/decnetd --log-level debug samples/pcap.conf

MOP is the first thing to try: a system id exchange and a loopback need no
routing adjacency, only a working frame path, so they separate "the frames
are moving" from "the routing is right".

## Next in the queue

`nicepackets` -- the NICE protocol messages. The data coding underneath
them is already done, because event records needed it. See the note in
`NOTDONE.md` about requests omitting the type code byte that responses
carry.
