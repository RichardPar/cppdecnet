# Known issues

## Monitoring pages ignore `info=char`

`/circuits?info=char` shows the summary fields instead of the
characteristics (hello timer, cost, router priority, designated router).
The same data is returned correctly over NICE, so the problem is likely in
how the page builds its request.

`src/http/monitor.cc`

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
