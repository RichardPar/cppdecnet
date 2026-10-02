# Known issues

## A second decnetd with the same configuration joins the network

Start decnetd twice from one configuration and the second one comes up as
the same node, on the same circuits, alongside the first. Its API sees the
first one's socket still answering, logs "another server is already using
...", and stays off, but `Node::start` ignores that and starts everything
else. Two routers with one address then fight over their neighbours until
one is stopped. It happens easily by accident, for instance when a wrapper
script is killed and the decnetd it started is not.

A taken API socket should stop the node from starting, or the node should
hold a lock (a pid file, or the socket itself) for its address.

`src/common/node.cc` (`api_->start ()`), `src/api/server.cc`

## TCP DDCMP circuits occasionally fail to come up

`ddcmp.two_nodes_come_up_over_a_tcp_ddcmp_circuit` fails now and then
with the circuit never coming up in its 30 seconds, and nothing logged in
between. It was put down to load, but it failed once in five runs on
2026-10-01 at a load average of 0.9, so load is not the whole story.

Suspected cause, not confirmed: both ends listen and connect at once, and
`TcpDdcmp::check_connection` takes whichever finishes first. On loopback
both outbound connects can finish together. Each end then keeps its own
outbound socket and closes its listener, which resets the connection the
other end chose, so both are left holding dead sockets. Each retries after
its restart backoff, logged only at debug level, and can collide again.
Nothing breaks the tie. Running the test with
`--log-level debug` until it fails would show it.

`src/datalink/ddcmp_link.cc`, `tests/test_ddcmp.cc`

## Two-node tests are timing flaky under load

Every suite that stands two nodes up over a real socket fails
occasionally, a different test each time. Seen so far: `test_nsp`
(`out_of_order_segments_are_held_not_dropped`,
`closed_connections_are_reclaimed`,
`xoff_stops_transmission_and_xon_resumes_it`), `test_counters`
(`nsp_node_counters_follow_a_conversation`), `test_nml`
(`read_an_unknown_node_name_is_unrecognized_component`, waiting for the
link to be accepted), `test_session`
(`finished_conversations_are_reclaimed`, once at load average 2.7, then
0 in 5) and `test_lan` (`a_neighbour_that_stops_sending_hellos_is_dropped`,
once in the sanitizer build while other builds ran).

It is load, not any one change: `test_nsp` measured 1/10 on an unmodified
tree at 06e79dc. Judge this suite by an interleaved comparison with the
tree before a change, never by consecutive runs. On 2026-10-01 the whole
suite ran four and a half times at a load average near 1, with one
failure here (`test_nml`) and one DDCMP failure (above).

The cause is the harness: the nodes talk over a real socket with a 2
second hello timer, and a loaded machine slips past the waits.

The lesson: when a test waits on one observable and then asserts a second,
the two must each be waited for. `counters.losing_the_neighbour_counts_a_circuit_down`
failed exactly that way -- `restart()` counts the circuit down before it
takes the adjacency down, so seeing `cir_down` says nothing yet about
`adj_down`.

`tests/test_nsp.cc`, `tests/test_nml.cc`

## `test_eventlog` occasionally hangs

Rarely, `test_eventlog` hangs after both circuits come up. Thread states
suggest a deadlock (main thread waiting on a futex). Not reproduced
reliably, and not seen in five runs on 2026-10-01.
`tools/catch-eventlog-hang.sh` runs the test repeatedly under load and
dumps thread stacks when it hangs (needs root for gdb).

## Adjacency freed inside its own timeout

`Adjacency::timeout()` can drop the last reference to the adjacency while
it is still executing. Nothing is accessed afterwards, so it is harmless
at present.

A related risk: `Timeout` work items hold a raw `Timer *`. Each carries
the timer's revision count, so a timer that was stopped or restarted
before the item ran is ignored, but checking that count reads the timer,
so a timeout queued for an adjacency that is destroyed before dispatch
still reads freed memory. Not observed. Fixing it properly needs weak
references in timer work items.

`src/routing/adjacency.cc`, `include/decnet/common/timers.h`,
`src/common/timers.cc`

## Slow recovery after a UDP Multinet peer restarts

An init message received in `ru` is ignored, and
`PtpPort::start_works()` always returns true. A UDP Multinet circuit whose
peer restarts recovers only when the listen timer expires. PyDECnet
recovers immediately. The comments in `include/decnet/datalink/multinet.h`
and `include/decnet/routing/ptp.h` say the UDP form reports
`start_works()` false; nothing overrides it, so it doesn't.

`src/routing/ptp.cc`, `src/datalink/ptp.cc`, `src/datalink/multinet.cc`

## Level 1 router sends no routing messages to a LAN of endnodes

`LanCircuit::wants_updates()` returns false unless a router adjacency is
up. PyDECnet sends routing messages regardless. Removing the check caused
adjacency problems with an RSX PDP-11, so it stays for now. Needs testing
with a second router on the segment.

`src/routing/lan.cc`

## Endnode circuit cost read once

`L1Router::adj_up` reads the circuit cost when the adjacency comes up.
Fine while cost is only set in the configuration, wrong once SET CIRCUIT
COST is supported.

`src/routing/l1router.cc`

## Own-address frames dropped silently

`BcDatalink::receive_frame` drops frames with our own source address. If
two nodes pick the same `--random-address`, the circuit ignores its peer
with no log message.

`src/datalink/bc.cc`

## Minor

- `PtpCircuit::running()` and `nice_substate()` `const_cast` themselves
  to call `in_state`, which has since become const; the casts can go.
- `UdpMultinet::create_port` only calls the base class.
