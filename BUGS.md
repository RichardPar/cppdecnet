# Known issues

## `src/http/monitor.cc` is dead code

Nothing references `HttpMonitor`. The live server is `http::Server` in
`src/http/server.cc`, which `Node` constructs; `monitor.cc` is a second,
earlier monitoring interface that is compiled into the library and never
instantiated, not even by a test. Its pages read layer state directly
rather than through `nice_read`, so it does not benefit from the counter
work and would rot further. It should be deleted or wired up.

The lesson: a file that compiles is not a file that runs. The stale entry
this replaced ("monitoring pages ignore `info=char`") described
`monitor.cc`, and was never true of the server actually serving the pages
-- `/circuits?info=char` returns cost, hello timer, listen timer and type
correctly.

`src/http/monitor.cc`, `include/decnet/http/monitor.h`

## `dnping` cannot reach the built-in MIRROR

`dnping -c <config> <node>` fails with `did not accept (reason 38)`
against a `decnetd` that has no `object` line, although object 25 is
registered by default in `Session::start`. Reason 38 is OBJ_FAIL, which
`Connection::cr` returns when session control does not answer in time, so
the connect appears to reach NSP and stall above it. Reproduced on an
unmodified tree at 06e79dc, so it is not a regression from the counter
work.

`src/session/session.cc`, `tools/dnping.cc`

## Two-node tests are timing flaky under load

Every suite that stands two nodes up over a real socket fails
occasionally, a different test each time. Seen so far: `test_nsp`
(`out_of_order_segments_are_held_not_dropped`,
`closed_connections_are_reclaimed`,
`xoff_stops_transmission_and_xon_resumes_it`), `test_counters`
(`nsp_node_counters_follow_a_conversation`) and `test_ddcmp`
(`two_nodes_come_up_over_a_tcp_ddcmp_circuit`).

It is load, not any one change. `test_nsp` measured 1/10 on an unmodified
tree at 06e79dc. `two_nodes_come_up_over_a_tcp_ddcmp_circuit` looked like
a regression at 2 failures in 4 runs, but 06e79dc and the current tree
run interleaved, six rounds each under the same load, failed zero times
on both -- the 2/4 was a machine at load average 2.3, not the code. Judge
this suite by an interleaved comparison, never by consecutive runs.

The cause is the harness: the nodes talk over a real socket with a 2
second hello timer, and a loaded machine slips past the waits.

The lesson: when a test waits on one observable and then asserts a second,
the two must each be waited for. `counters.losing_the_neighbour_counts_a_circuit_down`
failed exactly that way -- `restart()` counts the circuit down before it
takes the adjacency down, so seeing `cir_down` says nothing yet about
`adj_down`.

`tests/test_nsp.cc`

## `test_eventlog` occasionally hangs

Rarely, `test_eventlog` hangs after both circuits come up. Thread states
suggest a deadlock (main thread waiting on a futex). Not reproduced
reliably. `tools/catch-eventlog-hang.sh` runs the test repeatedly under
load and dumps thread stacks when it hangs (needs root for gdb).

## Adjacency freed inside its own timeout

`Adjacency::timeout()` can drop the last reference to the adjacency while
it is still executing. Nothing is accessed afterwards, so it is harmless
at present. A related risk: `Timeout` work items hold a raw `Timer *`, so
a timeout queued for an adjacency that is destroyed before dispatch reads
freed memory. Not observed. Fixing it properly needs weak references in
timer work items.

`src/routing/adjacency.cc`, `include/decnet/common/timers.h`

## Slow recovery after a UDP Multinet peer restarts

An init message received in `ru` is ignored, and
`PtpPort::start_works()` always returns true. A UDP Multinet circuit whose
peer restarts recovers only when the listen timer expires. PyDECnet
recovers immediately.

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

## Own-address frames dropped silently

`BcDatalink::receive_frame` drops frames with our own source address. If
two nodes pick the same `--random-address`, the circuit ignores its peer
with no log message.

## Minor

- `PtpCircuit::running()` uses `const_cast`; `StateMachine::in_state`
  should be const.
- `UdpMultinet::create_port` only calls the base class.
