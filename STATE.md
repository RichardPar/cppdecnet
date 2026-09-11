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
- short frames were padded with zeros rather than pydecnet's 0x42, which
  was enough for BAJI to build an adjacency to a node that does not exist
- a level 1 router sent no routing messages at all to a LAN whose only
  neighbour was an endnode

Our router hello is now byte for byte identical to pydecnet's, verified by
generating pydecnet's own message offline and diffing it against captured
bytes.

**The adjacency flaps, and that is not ours.** BAJI holds the adjacency for
three or four hello intervals and then reports

    Event type 4.18, Adjacency down
    Adjacency listener receive timeout, Adjacent node = 1.20

while our hellos continue without a gap -- every 10 seconds, both
multicasts, `0 packets dropped by kernel`. pydecnet does the same thing
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

- **A/B against pydecnet on the same wire.** `pydecnet` is in
  `../pydecnet`; a config with the same node number, node type, hello timer
  and interface makes the running stack the only variable. It said
  immediately whether a symptom was ours, and twice the answer was not what
  was expected.
- **Generate the reference message offline.** Importing pydecnet's own
  packet classes and diffing their bytes against a capture found the
  padding difference in one step, with no wire access needed.

## Next in the queue

NICE and the monitoring pages landed on 11-Sep-2026. `nicepackets`, `nml`
(object 19, which is what NCP connects to) and an HTTP server serving one
page per NICE entity are in, with 316 tests in 26 binaries passing in both
flavours.

What that leaves, in the order it is worth doing:

- **NICE SET and ZERO**, which are refused today. They want the access
  control decision first; see `NOTDONE.md`.
- **LOOP CIRCUIT and LOOP LINE**, which drive MOP loopback. The loopback
  works; nothing connects NICE to it.
- **`apiserver`**, the JSON API over a Unix socket, which is the rest of
  phase 7.

One trap found while doing this, worth knowing before the next session:
`BUILD` now defaults to **release**, not debug (`mk/config.mk`). A plain
`make` builds `build/release`, so running `build/debug/bin/test_*` out of
habit runs whatever was there last. That produced a convincing failure in
`test_nml` that did not exist -- the binary was three hours stale. Run
`make check` for release and `make check BUILD=debug` for the sanitizer
build, and do not run the binaries by hand from the wrong tree.
