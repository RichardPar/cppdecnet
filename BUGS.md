# Bugs

Two lists. First what is still wrong, then what has been fixed and was
interesting enough to write up.

Work that has not been started is in [TASKS.md](TASKS.md), and things left
out on purpose are in [NOTDONE.md](NOTDONE.md). A `PORT:` comment in the
source means "not written yet", not "broken".

---

## Outstanding

### 1. An endnode originates ShortData -- checked, not a defect

`EndnodeRouting::send` builds a `ShortData` where the Python builds a
`LongData`, on the grounds that an endnode's one circuit may be an Ethernet
and the long header is what carries the Ethernet addresses.

This entry used to say it "becomes a real bug the moment an endnode has a
LAN circuit". That was checked on 10-Sep-2026, against exactly that case --
`samples/pcap.conf`, an endnode whose only circuit is a pcap Ethernet --
and it is wrong. Every LAN send path goes through
`LanCircuit::send_to_mac`, which converts to `LongData` before it reaches
the port. The short form is an internal representation; the wire is long
and correct.

Left here rather than deleted because the divergence from the Python is real,
even though the consequence claimed for it was not. Kept at number 1 so the
numbering other documents refer to does not shift.

`src/routing/routing.cc`, `src/routing/lan.cc`

### 2. Endnode circuit cost is sampled once

`L1Router::adj_up` reads the circuit's cost when the adjacency comes up and
writes it into the endnode column; nothing re-reads it. Fine while cost is
configuration-only, which it is. It would be wrong if cost became settable
at run time, which NCP `SET CIRCUIT COST` does.

### 3. `PtpCircuit::running()` casts away const

`src/routing/ptp.cc` uses `const_cast` to call `in_state` from a const
member. Cosmetic, but the kind of thing that hides a real constness problem
later. `StateMachine::in_state` should be const.

### 4. Frames from our own source address are dropped unconditionally

`BcDatalink::receive_frame` ignores any frame whose source is our own
address, which is right on a medium that echoes. But `--random-address`
does not stop two nodes drawing the same address, and the symptom would be
a circuit silently ignoring its peer. Worth at least logging.

### 5. NSP asks for no flow control on its own inbound data

Outbound flow control is done: a peer that asks for segment or message mode
gets it, and link service messages carry the credit. Our own connect
message still asks for `SVC_NONE`, so a peer sends to us without credit.
The Python does the same, so this interoperates, but it means we cannot slow
a fast sender down. Doing it needs us to send link service messages as a
receiver, which is the other half of the same machinery.

### 6. `UdpMultinet::create_port` is a pointless passthrough

Dead code in `src/datalink/multinet.cc`. It calls the base and returns.

### 7. A level 1 router sends no routing messages to a LAN of endnodes

`LanCircuit::wants_updates` returns false unless a router adjacency is up,
so a circuit whose only neighbour is an endnode never carries a routing
message. The Python has no such test: it sends to ALL_ROUTERS every `t1`
regardless (`routing.py`, `Update.dispatch`).

The gate looks wrong on its own terms -- a router that has just come up and
has not yet heard a peer stays silent, and a peer waiting to hear from it
waits for the message being withheld.

It was removed on 10-Sep-2026 to match the Python and put back the same day.
Against the real PDP-11 on the wired segment, the gated build flapped its
adjacency and the ungated build did not bring one up at all -- five extra
frames per ten seconds at that machine's Ethernet controller is the
suspicion, unproven. So the change is not simply correct, and the reason it
was attempted -- that it was the last difference between our frames and
The Python's -- had already lapsed, because the Python flaps against BAJI too.

Retest with a second *router* on the segment rather than an endnode, where
the traffic is wanted and the receiver is not a PDP-11.

`src/routing/lan.cc`

### 8. `test_eventlog` hangs, rarely, under heavy load

`eventlog.an_event_travels_to_a_remote_sink` used to fail about one run in
three on a busy machine. Most of that is fixed and the cause is written up
below; what is left is rarer and is a different failure.

The remaining one is a **hang**, not a failed assertion: the process runs
past two minutes having brought both circuits up, and prints no result.
Seen once in twelve runs with every core spinning; then not once in the
following seventy-five runs under the same load, so it is rarer than one in
twenty and has not been caught in the act.

Not diagnosed. A stack trace is what it needs.
`tools/catch-eventlog-hang.sh` does the whole thing; by hand it is:

    for i in $(seq 1 $(nproc)); do (timeout 300 yes > /dev/null &); done
    ./build/release/bin/test_eventlog & pid=$!
    sleep 45
    kill -0 $pid && gdb -p $pid -batch -ex "thread apply all bt"

Note the load generator has to be killable by name (`pkill -x yes`). A
`while :; do :; done` loop matches the pattern of the shell running the
hunt, and killing that produces an empty log that looks exactly like a
hung test -- which cost a diagnosis cycle here.

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

### A LAN neighbour was addressed by an address it does not listen on

`RoutingLanCircuit::handle` recorded a neighbour's address as
`Macaddr::from_nodeid (id)`, the Phase IV derived `AA-00-04-00-xx-xx`.
The Python does exactly the same -- `Adjacency.macid = Macaddr (self.nodeid)`
-- so this was faithful to the port's source rather than a transcription
slip. The specification agrees with both: a Phase IV node programs its
derived address into its card.

A real PDP-11, BAJI at 1.19 on the test segment, does not. It announces
`id = aa-00-04-00-13-04` inside its hellos while transmitting from
`08-00-2b-11-22-33`, the card's own DEC address. A loopback probe settled
what that means, and it is not subtle:

    aa:00:04:00:13:04   0 of 5 replied
    08:00:2b:11:22:33   5 of 5 replied

So the node receives nothing at all on the address we would have sent to.
Hellos are multicast and would have kept working, so the symptom would have
been an adjacency that comes up, stays up, and passes no traffic -- with
both ends looking healthy in their own logs.

The fix is to believe the source address of the frame the neighbour
actually sent. Doing that turned up the real gap: the routing layer never
had it. `LanCircuit::decode` took a `Macaddr &src` out parameter, and its
body ended with

    (void) src;

so every LAN adjacency was recorded against an all zeroes address the
moment anything believed it. The datalink knew the address -- `bc.cc`
parses it into `ParsedFrame::src` and uses it to drop our own echoed
frames -- but `Received` had nowhere to put it, so it was dropped on the
way up. The Python carries it as `work.src`; that part had not been ported.

`Received` now carries the source address, `BcDatalink::receive_frame`
passes it, and the LAN circuits address neighbours by it. `decode` lost the
out parameter it never filled in.

Worth noting how it was caught. The first attempt changed only the three
places that assigned `from_nodeid`, which compiled clean and looked right.
`test_lan.endnode_data_reaches_the_router` failed immediately: the endnode
had dutifully sent its data to 00-00-00-00-00-00. A test that asks whether
a packet arrived, rather than whether an adjacency exists, is what made the
difference -- the same lesson as the LAN routing table fix above.

That fix was incomplete, and the tests could not have said so. It corrected
what the circuits record and what an endnode sends, but not
`LanCircuit::send_to`, the path a *router* forwards by: it still called
`Adjacency::macid ()`, the derived address. So a router with a route
through BAJI would have put every transit packet on the wire addressed to
an address BAJI does not answer on, with the adjacency up and the routing
table correct throughout.

Nothing caught it because every LAN test builds both ends out of our own
nodes, which do program their derived address into the card, so derived and
actual agree and the two paths are indistinguishable. `test_lan` now has a
`Station`: a datalink port with no routing layer above it, announcing one
address and answering on another, which is the only arrangement in which
the difference is visible. `send_to` now prefers the recorded source
address and keeps the derived one as the fallback for a neighbour nothing
has been heard from. Fixed 11-Sep-2026.

### A hello identical in every field was still rejected, for its padding

An Ethernet frame shorter than 60 bytes has to be padded. We padded with
zeros; the Python pads with 0x42 (`FILL = b'\x42' * 60` in `ethernet.py`).
That cannot matter, because DEC's padded format puts the payload length two
bytes into the frame and a receiver has no business reading past it.

It mattered. A PDP-11 running RSX, sent our router hello, built an
adjacency to node **21.426** -- an address that exists nowhere on the
network and appears nowhere in our packet. Sent the Python's, it built a
correct one to 1.20. The two hellos were identical for all 27 payload
bytes; the fill was the only difference. So that end reads past the length
it is given, and what it finds there becomes part of an address.

Found by A/B: the Python was run as the level 1 router on the same segment
with the same node number and circuit, which is the one test that says
whether a difference is ours. Then the Python's own `RouterHello` class was
used to generate the message offline and diff it against the captured
bytes, which is what narrowed it to the fill.

The lesson is not about padding. It is that matching a reference
implementation's *incidental* choices is worth doing when the peer is forty
years old and may read past what it was told.

### A triggered routing update postponed the periodic one indefinitely

`Update::send_now` restarted the periodic timer at `t1` after any send. A
triggered update is sent whenever the table changes, so on a circuit whose
topology keeps changing the periodic sweep was pushed back every time and
could go out arbitrarily late, or never.

That matters because the sweep is what recovers from a triggered update
that was lost. Losing it does not stop routes converging; it stops them
converging again after a drop, which is the failure that looks like a
network with a long memory for stale routes.

The Python schedules the next update after a triggered one at the time
elapsed since the last *full* update, capped at `t1`, and only a full
update restarts the interval (`Update.dispatch`). A full update therefore
follows a triggered one within at most another `t1`, so the sweep happens
every `t1` to `2*t1` however busy the circuit is. Ported, with `lastfull_`
as the extra state it needs.

### Closed connections and finished conversations are now reclaimed

NSP kept every closed `Connection` and session control every finished
conversation, because both are retired from inside a callback into the
object being retired and freeing them there is a use-after-free -- the one
written up below. Nothing ever freed them, so a node that opened and closed
many links grew without bound.

Each now carries the time it was retired and is destroyed once a grace
period has passed, sixty seconds by default. The sweep runs when another
entry is retired, so the work happens exactly when there is something to
reclaim and an idle node does nothing.

The ordering is the part worth remembering: **sweep before adding, never
after.** A sweep that runs after the push can reclaim the very object whose
stack frame is about to be returned into, which is the original bug wearing
a new hat. Written the other way round first, and the test that set the
grace period to zero to avoid waiting out a minute hung immediately --
which is the same defect this entry is about, reintroduced and then caught
by a test written to check it was gone.

### The NSP retransmission timer was a constant

`retransmit_time_` was two seconds, whatever the link. Every retry waited
the same two seconds, and the limit was a fixed five, so a peer that was
briefly unreachable was hammered at full rate and then given up on ten
seconds later -- while a genuinely slow path had every ordinary round trip
look like a loss.

The Python does not do this, and the `PORT:` note in `NSP::NSP` said so:
the timer follows the round trip time actually measured to each node.
Ported, as `Connection::acktimeout` and `Connection::update_delay`:

- each node carries a smoothed estimate in its `Nodeinfo`, so a second
  connection to the same node starts with what the first one learned
- a measurement is folded in as `1/(weight+1)` of the difference, so one
  slow exchange moves the estimate a little rather than replacing it
- the estimate is floored at one second and capped at five. The floor is
  not about timer granularity: on a DDCMP serial line the latency depends
  on packet length, and an estimate taken from short packets produces
  false timeouts the moment a long one is sent. The cap is because
  congestion otherwise feeds itself -- a longer estimate means a longer
  timeout, which hides the congestion that caused it
- the timeout is the estimate times a factor, because waiting exactly the
  round trip time makes every ordinary variation look like a loss

Two things the arithmetic alone does not say, both of which matter:

**A retransmitted packet is never timed.** Once a packet has gone out
twice there is no way to know which transmission an acknowledgement
answers, and measuring from the first inflates the estimate on every loss
-- which lengthens the timeout, which loses more.

**Only one packet is timed at a time.** Timing several measures the same
round trip repeatedly and learns nothing extra.

The retries now back off as well, doubling to a thirty second ceiling,
and the limit comes from the configuration rather than a constant.

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
peer, as the Python does. A poll error on a datagram socket is logged and
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
deferred the failure to the far end: the Python rejected our first interop
configuration for exactly this. Porting `common.nodename` and `circname`
promptly caught two invalid names in our own tests.

The TLV tolerant path left a runt item unconsumed, tripping the enclosing
packet's extra-data check. The Python swallows it.

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

Where the port behaves differently from the Python by choice, the reasoning
is in [NOTDONE.md](NOTDONE.md) and repeated in a comment at the site. None
of it is accidental.
