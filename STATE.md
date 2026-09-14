# Where the work stopped

Written 2026-09-10, updated 2026-09-10. This is a session handover note,
not a permanent document -- delete it once the open item below is closed.

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
- `tests/test_event.cc` (16 tests, against the Python's own byte vectors)
  and `tests/test_eventlog.cc` (19 tests, including a record crossing a
  real circuit between two nodes)

Events are raised from routing, NSP and node state changes.

**The pcap datalink.** `PcapEthernet` in `include/decnet/datalink/ethernet.h`
and `src/datalink/ethernet.cc`, plus `BcDatalink::filter_expression()` in
`src/datalink/bc.cc`. Compiled and unit tested; **never yet run on a real
segment** -- see below.

**Recovery from a neighbour that stops answering** (14-Sep-2026). Two
defects, both written up in `BUGS.md`, both of which left a node off the
network permanently rather than temporarily:

- `Port::restart ()` posts a `Restart` work item that no point to point
  datalink handled, so every restart the routing layer asked for -- a
  listen timeout above all -- did nothing, and the circuit waited in `ds`
  for a `DlStatus` UP that was never coming. Handled now in
  `PtpDatalink::validate` (reconnect, no holdoff) and overridden in
  `Ddcmp::validate` (restart the protocol, keep the transport), which is
  how the Python splits it.
- `EndnodeLanCircuit` cleared the timed-out adjacency but not `dr_`, so the
  endnode never re-adopted a designated router that came back.
- `RoutingLanCircuit` did the same on the router side and never re-ran the
  election, so a router whose designated router died never took over --
  and because only the designated router sends hellos to the endnodes,
  that took the whole segment down with it.
- A router heard but never confirmed two-way had no adjacency object, so
  nothing owned a listen timer for it and nothing ever aged it out; its
  priority stayed in the election for good.

There is also an event where there was none: a point to point listen
timeout now raises 4.8, circuit down, reason listener timeout, as the
Python's `PtpCircuit.adj_timeout` does.

Tests: `tests/test_recovery.cc` (three, two nodes joined through a relay
that can be told to hold both connections open and discard everything --
which is what a wedged node looks like from outside, and what a dropped
socket does not), plus `ddcmp.a_restart_request_is_obeyed_and_the_circuit_comes_back`
and three in `tests/test_lan.cc`:
`endnode_readopts_a_router_that_went_quiet`,
`a_router_takes_over_when_the_designated_router_goes_quiet` and
`a_router_heard_once_does_not_block_the_election_for_good`. Every one of
them was checked against the unfixed code and fails there.

Worth keeping in mind for the next one of these: the first version of the
takeover test waited for `designated_router ()` rather than for an
adjacency to be up, and so stopped the far node three milliseconds after
it started. That accident is what found the aging defect -- the test was
wrong and the failure was real.

**A crash in the event logger, found while running the suite to check the
above** (14-Sep-2026). `RemoteSink::send_events` popped each record after
sending it, and the send goes down through session control, NSP and routing
on the caller's stack -- so an event raised down there re-entered the loop
and the two calls popped the same record twice. One run of `test_eventlog`
in twelve under load died in the `Event` destructor. Fixed: one loop at a
time, and the record leaves the queue before it is sent. `BUGS.md` has the
stack. Whether this was also the rare `test_eventlog` **hang** (item 8) is
not known; the hang has not appeared since, but it was never frequent
enough for that to mean much.

**Two defects fixed**, both written up in `BUGS.md`:
- an event raised from inside a routing state function outran the state
  change, so the connect it triggered was dropped by `send_raw`
- `Node::stop()` stopped the layers from the caller's thread while the
  main loop was still dispatching into them

**The shutdown race fix is stress verified.** 40 consecutive runs of
`test_nsp` under ASan, all clean: 22 passed / 0 failed each, and no
sanitizer output in any of the 40 logs. Before the fix this aborted about
one run in thirty, so 40 clean runs is the evidence that was wanted. The
loop that produces it:

    for i in $(seq 1 40); do
        ./build/debug/bin/test_nsp > /tmp/nsp-$i.log 2>&1 || {
            echo "FAILED run $i"; grep -E 'ERROR|runtime error' /tmp/nsp-$i.log; }
    done

Note when re-running it: a run takes about 25 seconds under ASan, so the
loop needs ~17 minutes. Counting log *files* is not the same as counting
finished runs -- the last file exists while its run is still going. Check
that each log ends with its pass summary.

## Deployed

The gateway node runs on the Rock Pi at 192.168.10.151 as a systemd
service: node 29.150 CPPNOD, `/usr/local/bin/decnetd` with
`/etc/decnet/myhecnet.conf`, `CAP_NET_RAW` and `CAP_NET_ADMIN` as ambient
capabilities rather than root. `tools/deploy-arm.sh` repeats the whole
thing; `samples/gateway/` is the host side of it.

One circuit reaches both PDP-11s because the host has a bridge: br0 holds
the wired NIC and the tap the simulator attaches to. That is not
decoration. libpcap on a shared physical NIC never sees the host's own
locally-originated frames, so a circuit on the raw NIC and a guest on a tap
cannot hear each other in either direction -- and the tap has to exist,
bridged and up, *before* simh starts, or its first transmit fails with EIO,
RSX's DELUA gives up for the rest of the boot, and NCP goes on reporting
the circuit as On. Both halves are written up in `samples/gateway/`.

Live evidence, 14-Sep-2026:

    ETH-0  Adjacent node 29.158 (RAXDA)   the simh PDP-11, over tap0
    ETH-0  Adjacent node 29.159 (BAJI)    the real PDP-11, on the wire
    MUL-0  Adjacent node 29.1             HECnet

with BAJI's own hellos naming `rtr 29.158` -- two level 1 routers at equal
priority on one segment, the election decided by address, and this node
correctly standing down. The other end's hello names us with the two-way
bit set, which is the part that cannot be faked from our own logs.

## Open

**The pcap circuit works against real hardware. What is left is the data
path.**

Verified on 10-Sep-2026 against BAJI, a PDP-11 running RSX at node 1.19 on
the wired segment, with `samples/pcap-router.conf`:

    17:42:26  ETH-0 designated router is self
    17:42:26  Event 4.15 Adjacency up -- ETH-0, Adjacent node 1.19 (BAJI)

and, from BAJI's own hellos on the wire, the confirmation that matters
because it comes from the other end rather than from our logs:

    17:53:01.128  endnode-hello src 1.19 ... rtr 0.0
    17:53:01.669  endnode-hello src 1.19 ... rtr 1.20

The second hello followed the first by half a second: an endnode announces
a change of designated router straight away rather than waiting for its
timer. So frames move in both directions, the adjacency is two way, and
BAJI has chosen us as its router. Earlier in the same session BAJI also
logged `Adjacency down ... Adjacent node = 1.20` when decnetd was stopped,
which is the same fact from the other side.

Three defects were found and fixed against BAJI, all of them ours, all
written up in `BUGS.md`:

- a LAN neighbour was addressed by its derived MAC, which BAJI does not
  listen on
- short frames were padded with zeros rather than the Python's 0x42, which
  was enough for BAJI to build an adjacency to a node that does not exist
- a level 1 router sent no routing messages at all to a LAN whose only
  neighbour was an endnode

Our router hello is now byte for byte identical to the Python's, verified by
generating the Python's own message offline and diffing it against captured
bytes.

**The adjacency flaps, and that is not ours.** BAJI holds the adjacency for
three or four hello intervals and then reports

    Event type 4.18, Adjacency down
    Adjacency listener receive timeout, Adjacent node = 1.20

while our hellos continue without a gap -- every 10 seconds, both
multicasts, `0 packets dropped by kernel`. The Python does the same thing
against BAJI, which is what settles it: the flap is on the PDP-11 side.
Chasing it further belongs with BAJI's DECnet or its Ethernet controller,
not here.

Still to do:

- **Data.** `NCP> LOOP NODE CPPNOD` from RSX -- note the `NCP` prefix, it
  is not an MCR command -- goes to our MIRROR over NSP and is the real end
  to end test. Not yet run. It needs the adjacency to be up at the moment
  it runs, which the flap makes a matter of timing.
- **Unicast to BAJI in anger**, which `LOOP NODE` would be the first of.

Method worth keeping, because it is what actually resolved this:

- **A/B against the Python on the same wire.** `the Python` is in
  The Python source tree beside this one; a config with the same node number, node type, hello timer
  and interface makes the running stack the only variable. It said
  immediately whether a symptom was ours, and twice the answer was not what
  was expected.
- **Generate the reference message offline.** Importing the Python's own
  packet classes and diffing their bytes against a capture found the
  padding difference in one step, with no wire access needed.

## Next in the queue

DDCMP was started on 11-Sep-2026. The protocol is done and one of
its four transports is.

**In, and tested:** the message layer and the protocol engine.
`include/decnet/datalink/ddcmp.h`, `src/datalink/ddcmp_packets.cc` and
`src/datalink/ddcmp.cc`, with 25 tests in `tests/test_ddcmp.cc`.

The seven message forms encode byte for byte as the Python's own `ddcmp`
module encodes them; the vectors and how to regenerate them are in the
test file. The protocol engine has no transport in it -- what carries the
bytes arrives as callbacks -- so the tests wire two engines to each other
and exercise the startup handshake, a lost message, a NAK, a REP, the send
window, a stale acknowledgement, a restart, and 300 messages through the
sequence number wrap, with no sockets and no timers.

**Also in:** every transport except the synchronous framer -- UDP, TCP,
telnet and a real serial line -- and the wiring. A configuration line

    circuit ddc-0 DDCMP udp:27801:127.0.0.1:27802 --t3 2

builds a DDCMP circuit, and two nodes bring a routing adjacency up over
it -- the first time the protocol runs on real sockets, real threads and
real timers rather than against another engine in the same test.

**Not in yet:** the remaining transports.

- The synchronous framer, a board that frames in hardware and hands over
  headers with the CRC already checked. `Ddcmp::create` carries a `PORT:`
  note where it attaches.
- Nothing has yet run against SIMH, which speaks DDCMP over TCP and is
  the first real peer available for this datalink -- everything so far
  has been this implementation talking to itself.

Also open, from the bug work earlier the same day:

- `test_eventlog` hangs about one run in 26 under heavy load. Not
  diagnosed. `tools/catch-eventlog-hang.sh` loads the machine, runs the
  test until one hangs and dumps its thread stacks; it needs root,
  because `ptrace_scope` is 1 here and gdb cannot otherwise attach to a
  process that is not its own child. `BUGS.md` item 8 has the detail.

