# Bugs

Two lists. First what is still wrong, then what has been fixed and was
interesting enough to write up.

Work that has not been started is in [TASKS.md](TASKS.md), and things left
out on purpose are in [NOTDONE.md](NOTDONE.md). A `PORT:` comment in the
source means "not written yet", not "broken".

---

## Outstanding

### 1. An endnode originates ShortData, which is wrong on a LAN

`EndnodeRouting::send` builds a `ShortData` packet. PyDECnet builds a
`LongData` there, because an endnode's one circuit may be an Ethernet and
the long header is what carries the Ethernet addresses.

Harmless today. The only circuits that reach it are point to point, where
the circuit would convert a long header to a short one anyway. It becomes a
real bug the moment an endnode has a LAN circuit.

`src/routing/routing.cc`

### 2. Triggered updates reset the periodic timer

`Update::send_now` restarts the periodic timer at T1 after any send.
PyDECnet distinguishes: after a triggered update it schedules the next
periodic one at the time remaining since the last full update, so a busy
circuit still gets its sweep on schedule.

Effect: on a circuit with frequent topology changes, full updates go out
later than they should. Routes still converge, since triggered updates
carry the changes, but a lost update is recovered from more slowly. Easy
fix: track `lastfull` the way PyDECnet does.

`src/routing/l1router.cc`

### 3. Endnode circuit cost is sampled once

`L1Router::adj_up` reads the circuit's cost when the adjacency comes up and
writes it into the endnode column; nothing re-reads it. Fine while cost is
configuration-only, which it is. It would be wrong if cost became settable
at run time, which NCP `SET CIRCUIT COST` does.

### 4. `PtpCircuit::running()` casts away const

`src/routing/ptp.cc` uses `const_cast` to call `in_state` from a const
member. Cosmetic, but the kind of thing that hides a real constness problem
later. `StateMachine::in_state` should be const.

### 5. Frames from our own source address are dropped unconditionally

`BcDatalink::receive_frame` ignores any frame whose source is our own
address, which is right on a medium that echoes. But `--random-address`
does not stop two nodes drawing the same address, and the symptom would be
a circuit silently ignoring its peer. Worth at least logging.

### 6. Closed connections are never reclaimed

NSP keeps finished `Connection` objects in a `closed_` list and session
control keeps finished conversations in `finished_`, because a caller may
still hold a pointer to one. See the use-after-free below for why that
matters. Nothing ever frees them, so a node that opens and closes many
links grows slowly. Needs reference counting, or a sweep that reclaims an
entry once nothing outside can reach it.

### 7. NSP asks for no flow control on its own inbound data

Outbound flow control is done: a peer that asks for segment or message mode
gets it, and link service messages carry the credit. Our own connect
message still asks for `SVC_NONE`, so a peer sends to us without credit.
PyDECnet does the same, so this interoperates, but it means we cannot slow
a fast sender down. Doing it needs us to send link service messages as a
receiver, which is the other half of the same machinery.

### 8. `UdpMultinet::create_port` is a pointless passthrough

Dead code in `src/datalink/multinet.cc`. It calls the base and returns.

---

## Fixed

Grouped by what they teach, because several are the same mistake wearing
different clothes.

### Virtual dispatch during construction and destruction, three times

**Pure virtual call, crash.** `BaseRouter`'s constructor created the
routing circuits, and each circuit asked the router for its node type.
During a base class constructor that lands on the pure virtual and aborts.
Circuit creation moved to `init_circuits()`, called from each concrete
router's own constructor body.

**Same call, no crash, wrong answer on the wire.** With the call moved into
the circuit's constructor, an `L2Router` still got the wrong answer: a
circuit is created from the router's constructor, and for a derived router
that runs while the base part is still the most derived class. So an area
router announced itself in its initialisation message as a level 1 router,
and every cross-area adjacency was correctly rejected by the peer as an
address out of range. Nothing crashed. The node lied about itself. The
initialisation message is now built when it is sent.

**Destructor.** `~Ethernet` called `close()`, which reaches the pure
virtual `stop_transport()` once the derived object is gone. Worse, the
receive thread runs derived code, so stopping it in the base destructor
would already have been too late. Each concrete datalink now stops its own
thread from its own destructor.

The rule: anything a constructor or destructor needs from the object it is
building or tearing down must be deferred to first use, or done by the
derived class.

### Static initialisers do not work for registration in a static library

Packet classes register themselves into their family index. Namespace-scope
static initialisers fail inside `libdecnet.a`: the linker pulls in an
archive member only when something already needed references it, so a
translation unit that exists purely to register classes may never be linked
at all.

The symptom is a packet family that decodes correctly in one program and
not in another, depending on what else each happens to call. That is how it
appeared, with `test_rpacket` passing (it called `ntype_string`, which
dragged the object file in) and `test_routingmsg` failing on the same
packets. `DN_PACKET_INDEX_REGISTERED` now names a registration function,
which both gives the linker the reference it needs and runs it once before
the first lookup.

### Object lifetime inside a callback

Session control destroyed an application from inside a call into it.
`connect_rejected` and `disconnected` told the application the link had
gone, then erased the map entry that owned it. Fine when the callback
returns straight away, and a use-after-free the moment an application does
the obvious thing and disconnects from inside its own `data_received`,
because the erase then frees the object whose stack frame control is about
to return into.

ASan caught it on the first run of the session control tests. This is the
case the porting plan predicted sanitizers would earn their keep on: Python
cannot have this bug, since the collector keeps the object alive as long as
a frame refers to it. A finished conversation is now moved aside rather
than destroyed. Outstanding item 6 is the cost of that.

### Timers and convergence

The broadcast routing timer was ignored, and it made a test flaky. A router
sends its full routing message periodically, and DECnet uses two periods:
`--t1`, ten minutes, for point to point, and `--bct1`, ten seconds, for
broadcast. The port used `t1` for both.

Nothing looked wrong until the LAN routing test failed about one run in
four. The cause was worth chasing rather than re-running. On a LAN the two
ends bring their adjacencies up independently, one hello interval apart, so
a triggered update is quite often sent while the far end's adjacency is
still forming, and is correctly dropped. The periodic update is what
recovers from that, which is exactly why a LAN uses ten seconds. At ten
minutes a routine and expected miss looked like a dead route.

Confirmed by experiment rather than by the failure going away: with the two
nodes started 1.2 seconds apart, `bct1 = 600` failed to converge 6 times
out of 6, and `bct1 = 10` converged 6 out of 6. Eighteen post-fix runs of
the previously flaky test, no failures.

A timer constant copied to the wrong place does not fail. It converges
slowly, and slow enough is indistinguishable from broken.

### Datagram sockets

A bounced datagram killed a circuit permanently. UDP sockets were
`connect()`ed, which is what makes `send()` work and also what makes the
kernel report ICMP errors. Sending to a peer that has not started yet draws
an ICMP port unreachable, the next `poll()` returns `POLLERR`, and the
receive loop treated that as fatal and exited. Nothing restarted it.

The failure was asymmetric and so looked like anything but a socket
problem: of two nodes on a UDP-carried LAN, whichever started first sent
into the void, lost its receive thread and was deaf afterwards, while the
one that started second worked perfectly. Only one direction of a
symmetric configuration worked.

UDP sockets are now bound but never connected. Sends use `sendto`,
receives use `recvfrom` with the sender checked against the configured
peer, as PyDECnet does. A poll error on a datagram socket is logged and
ignored: a dropped packet is not a dead circuit. The same hazard was live
in Multinet's UDP mode and is fixed there too.

A LAN adjacency was also reported new on every hello, because the "have we
heard this neighbour before" test looked at a field never populated for LAN
adjacencies. A router logged "heard router X, waiting for two-way" once per
hello interval, forever.

### The routing layer could not see its LAN neighbours

`RoutingLanCircuit` brought adjacencies up and logged them, but created no
`Adjacency` object and told the router nothing. An Ethernet circuit found
its neighbours and then did nothing with them: routing messages had no
adjacency to be attributed to, and the routing table had no column for a
LAN neighbour. Fixed by giving both circuit kinds a common `Circuit` base,
so one `Adjacency` type serves both.

That fix looked complete and its tests passed, but the update process still
sent nothing. With adjacencies registered, `set_srm` correctly marked
destinations for advertisement into a table of update processes keyed by
point to point circuit, of which a LAN node has none. `Update` now works
against `Circuit`.

Worth remembering: the first fix passed its own tests. What caught the
second half was a test that asked whether a route actually appeared in the
table, rather than whether an adjacency appeared in a list.

### Ordering and lifetime

Layers stopped after the work queue had already stopped. `Node::stop()`
queued the datalink Stop items after the shutdown sentinel. The queue is
FIFO, so those items sat in a queue nothing was draining.

`BaseRouter::start()` was not virtual, so `L1Router::start()`, which builds
this node's own column in the routing matrix, never ran. Nothing was
reachable, not even ourselves.

A test macro bound a reference into a temporary. `DN_ASSERT_EQ` took
`const auto &` of each operand, so an accessor returning a reference into a
temporary left it dangling before the comparison ran. Silent garbage in a
normal build; ASan caught it at once.

### Protocol and interoperability

IPv4-mapped IPv6 addresses. A listening socket bound dual stack reports an
IPv4 peer as `::ffff:a.b.c.d`, while resolving that peer's name gives a
plain `AF_INET` address. Comparing the family first rejected every IPv4
peer, so Multinet listen mode accepted nothing.

`@file` includes resolved against the working directory rather than the
including configuration file's directory, so a real `pydecnet.conf` failed
to load.

Node and circuit names were not validated. DECnet node names are at most
six characters and must contain a letter. Accepting anything else only
deferred the failure to the far end: PyDECnet rejected our first interop
configuration for exactly this. Porting `common.nodename` and `circname`
promptly caught two invalid names in our own tests.

The TLV tolerant path left a runt item unconsumed, tripping the enclosing
packet's extra-data check. PyDECnet swallows it.

### Layers were stopped from the wrong thread

`Node::stop()` stopped every layer on the caller's thread while the node's
main loop was still dispatching work on its own. The layers hold the state
that loop dispatches into, so `NSP::stop()` freeing its connections raced
with `Connection::receive` running on one of them.

ASan caught it as a heap-use-after-free about one run in thirty of
`test_nsp`, which is exactly the frequency that gets a failure written off
as a flake. The comment above the code even explained why the stops came
before the shutdown sentinel -- so that queued work would still be
processed -- without noticing that "still being processed" and "being freed"
were happening at the same time.

Stopping now happens on the node's own thread: `stop()` posts a
`CallbackWork` that runs the whole sequence, then the shutdown sentinel
behind it. The queue is FIFO, so the layers are stopped after everything
already in flight and before the loop exits.

### An event raised from inside a state function outran the state change

The routing state machine assigns the new state only after the state
function returns. `PtpCircuit::up()` is called from inside that function,
just before it returns "running", so anything `up()` triggers runs while
the circuit still says it is not running.

That did not matter until the event logger existed. Then the circuit-up
event went to a remote sink, which opened a logical link, which sent a
connect initiate back down the same circuit -- and `send_raw` starts with
`if (!running ()) return;`, so the packet was dropped without a word. The
link never came up, and thirty seconds later it tried again.

What made it hard to see was that everything upstream looked right: the
trace showed the connect being sent to the correct next hop on the correct
circuit. The packet was thrown away one line further down.

Routing events are now built where they are raised and reported through a
`CallbackWork`, so they are delivered after the dispatch that produced them
has finished and the state is what it claims to be.

### Frame parsing assumed one format for every protocol

The Ethernet receive path parsed every frame as DEC padded format, which
carries a two byte payload length after the protocol type. Routing and MOP
use it; loopback does not.

So a loopback frame had its first two payload bytes read as a length, and
whatever came out of that was passed up as the payload. Nothing crashed and
nothing was logged: the loop request simply never came back, and the
symptom was a loopback that did not work at all rather than one that worked
badly.

Which format applies depends on the port, and which port a frame belongs to
depends on the protocol type in its header. The receive path now parses the
header first, finds the port, and reads the payload the way that port
expects.

### Process handling

`fork()` succeeding says nothing about `exec`. The first version of the
subprocess application code returned success as soon as the fork worked, so
a missing program silently hung the connection instead of rejecting it.
Fixed with the usual idiom: a close-on-exec pipe the child writes `errno`
to, which the parent reads as either an error number or end of file.

---

## Not bugs, but worth knowing

Stale binaries look exactly like real failures. Twice, `make` built the
library and daemon but not the tests, and a hand-run test binary gave a
result from the previous build. Once that produced a gdb backtrace pointing
at code that had already been changed. The default target now builds the
tests too, which removes the trap rather than relying on remembering it.

cJSON cannot carry the application protocol. It stores string values as
NUL-terminated C strings, so a NUL byte in the data truncates the value.
This protocol tunnels arbitrary bytes through JSON strings and NUL is not a
corner case: the mirror's function code is zero, the first byte of every
request it receives. Any JSON library with a C-string interface has the
same problem; one that reports an explicit length would be fine.

Where the port behaves differently from PyDECnet by choice, the reasoning
is in [NOTDONE.md](NOTDONE.md) and repeated in a comment at the site. None
of it is accidental.
