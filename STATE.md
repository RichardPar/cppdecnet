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

**Also in:** the UDP transport and the wiring. A configuration line

    circuit ddc-0 DDCMP udp:27801:127.0.0.1:27802 --t3 2

builds a DDCMP circuit, and two nodes bring a routing adjacency up over
it -- the first time the protocol runs on real sockets, real threads and
real timers rather than against another engine in the same test.

**Not in yet:** the remaining transports.

- TCP and telnet, then serial and the synchronous framer. Each is a
  matter of moving bytes: the engine already says what to send and what
  it was given. A stream needs `ddcmp::find_header` on the receive side,
  which is written and tested; telnet additionally escapes the all-ones
  byte. `Ddcmp::create` carries a `PORT:` note where they attach.
- TCP is the one to do next, because SIMH speaks it and that is a real
  peer to test against rather than another copy of ourselves.

Also open, from the bug work earlier the same day:

- `test_eventlog` hangs about one run in 26 under heavy load. Not
  diagnosed. `tools/catch-eventlog-hang.sh` loads the machine, runs the
  test until one hangs and dumps its thread stacks; it needs root,
  because `ptrace_scope` is 1 here and gdb cannot otherwise attach to a
  process that is not its own child. `BUGS.md` item 8 has the detail.

