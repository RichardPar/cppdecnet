# Things left undone on purpose

Three lists. What will not be done at all, what is postponed and why, and
where the port behaves differently from the Python by choice.

This is not the same as [TASKS.md](TASKS.md), which is the queue of work
still to do, or [BUGS.md](BUGS.md), which is defects. Everything here is a
decision someone made, so if you disagree with one you can change it
without first working out whether it was an oversight.

Sites in the code carry a `PORT:` comment saying what is missing there.
`make todo` lists them.

---

## Not doing

### The network mapper

`mapper.py`, about 1,700 lines, plus the bundled Leaflet assets,
`mapdoc.html` and `mapper-internals.html`.

It is a HECnet-specific map server. Upstream asks people not to run more
instances of it without coordinating first, and it has no bearing on the
protocol stack. Anyone who wants a map can keep running the Python for it.

### Objects loaded in-process

The Python objects declared with `--module` are Python modules imported into
the daemon. The equivalent here would be a shared object with a known entry
point, and it is not obviously worth the machinery: built-in objects are
registered in code (MIRROR is), and anything else can be a program declared
with `--file`, which already works and is what most objects use.

Revisit if something turns up that needs in-process speed and cannot be
built in.

---

## Postponed, with the reason

### NSP will not ask a peer to slow down

Our connect message asks for `SVC_NONE`, so a peer sends to us without
credit. Outbound flow control is done: a peer that asks for segment or
message mode gets it.

The Python asks for `SVC_NONE` too, so this is not an incompatibility. It
does mean a fast sender cannot be throttled. Doing it needs us to send link
service messages as a receiver, which is the other half of machinery that
already exists.

### Interrupt credit is not offered to the far end

Interrupts work in both directions, and the far end starts with permission
to send us one. We never send a link service message granting another, so a
peer that sends one interrupt and waits for credit will wait forever.

The Python has the same gap and says so: it accepts more than one inbound
interrupt without policing credit, on the grounds that nothing it talks to
depends on being throttled. We do the same, and for the same reason. It
belongs with the other half of inbound flow control, above.

### Delayed acknowledgement

The `dly` flag in a data segment says the receiver may hold its
acknowledgement briefly, which cuts the number of bare acknowledgements on
a busy link. Every acknowledgement goes out immediately at the moment. This
is throughput, not correctness.

### Phase II and Phase III neighbours

Phase III needs eight bit address handling throughout: answering a
`PtpInit3` with our own, and supplying the home area on packets that arrive
without one. Phase II additionally needs `NodeInit` and `NodeVerify`,
routing by name rather than address, and the intercept machinery, which
The Python itself only implements in part.

Both are ported at the site in `ri` where they attach. Few people run
either today, which is why they are behind Phase IV work rather than in
front of it.

### Access control is carried but not checked

The request id, password and account travel in the connect message and an
object can look at them. Nothing authenticates them. The Python uses PAM for
this and refuses to start with authentication configured if the PAM module
is missing.

Related: the Python can run an object as a different user, setting a uid and
gid in the child before exec. Ours runs everything as the daemon's user.
Both belong together, and both want a decision about what the daemon's
security posture should be rather than a quick implementation.

One visible consequence: event 2.1, access control reject, is never raised.
The event is in the catalogue; the check that would trigger it is not.

### The data link and physical event classes

Event classes 5 and 6 are defined here -- their names, severities and
parameter meanings are all in the catalogue, so a record from another node
displays properly -- but nothing raises one.

They describe things a real data link reports: state changes, error
thresholds, tributary selection, carrier and modem transitions. Multinet
and Ethernet-over-UDP have none of that. The circuits that would raise them
are DDCMP and a real synchronous line. DDCMP is now ported, so
these events are reachable and simply are not raised yet.

Class 0, network management, is in the same position: the automatic counter
events belong with the NICE protocol, which is the next piece of work.

### The source circuit on a packet loss event

Events 4.0 to 4.3, the packet loss events, should name the circuit the
dropped packet arrived on. `forward()` is not told which circuit that was,
so those events carry the packet header and no entity.

Threading the source circuit through `forward()` means changing a virtual
that three routers implement, for the benefit of three events that are
logged at debug level. It is worth doing with the counters work, where the
same information is needed per circuit anyway.

### The MOP console carrier

The remote console: a station reserves another node's console port and
carries a terminal session over it. The messages parse, so a console
carrier message from another node is recognised rather than discarded, but
nothing implements the reservation, the sequencing or the client and server
ends.

It is a state machine of its own, roughly 350 lines in the Python across the
client and server, and it is the one MOP function that is interactive
rather than a request and a reply. System ID, counters and loopback are
what maintenance tools use to see and test a node, and those are done.

MOP load and dump are not implemented here either. The Python does not
implement them.

### The DDCMP synchronous framer

DDCMP itself is ported: the message layer, the protocol, and the UDP, TCP,
telnet and serial transports.

What is left is the framer -- a board that does the framing in hardware and
hands the host headers with the CRC already checked. The message layer has
the hooks for it (a header can be decoded with the check suppressed, which
is exactly what a framer wants), but nothing drives one, and there is no
hardware here to drive.

A caveat worth keeping in front of anyone who reads the checkmarks: nothing
in DDCMP has yet talked to another implementation. The framing is checked
byte for byte against the Python's encoder, and the protocol is exercised
by running two of our own engines against each other over real sockets and
a real serial line -- but that is this implementation agreeing with itself.
SIMH speaks DDCMP over TCP and is the first real peer available. The
padding and neighbour-address defects found against a real PDP-11 both
passed every self-test we had at the time.

### GRE circuits

A small amount of work on top of the Ethernet datalink, and nothing needs
it while UDP-carried Ethernet, TAP and pcap cover testing and real
segments.

### Changing the interface's hardware address

A pcap circuit sends frames whose source is the DECnet derived address
AA-00-04-00-xx-xx while the interface keeps the address it was configured
with. The alternative -- setting the interface's address, as a dedicated
DECnet host would -- would take the interface down and up and break
whatever else is using it, so the circuit does not do that.

The consequence is that the card's own filter would discard our traffic, so
the circuit captures promiscuously and filters in software, with a BPF
filter pushed down to the kernel to keep the cost off the receive thread.
It also means a pcap circuit does not work on wifi: a managed mode station
cannot transmit with a source address that is not its own.

### Per-state packet type filtering

The Python's `setpackets` builds a sub-index per circuit state, so a packet
that cannot occur in the current state is rejected before it is parsed. The
states here check the type after parsing. The effect is the same and the
cost is one parse of a packet that will be discarded.

This is also where the router running substates (`ru4l1`, `ru4l2`, `ru3r`)
belong. They exist in the Python only to control which packet types each
accepts, so there is nothing to port until the filtering is.

### One node per process

The Python builds one `Node` per configuration file and runs them all in one
process, which is how a whole test network fits in one program. `decnetd`
starts the first configuration file and warns about the rest.

The `Node` class already supports several instances -- the end to end tests
create two in one process -- so this is a change to `main.cc`, not to the
design.

### Background name resolution

The Python re-resolves peer names on a helper thread, so a peer on a dynamic
address is followed without blocking anything. Ours keeps the re-resolution
interval but does the lookup inline on the datalink's receive thread, which
is already allowed to block.

It matters when many circuits share one name and that name is slow to
resolve. Until then, a thread per node is a thread not spent.

### NICE: SET, ZERO, and the Phase II dialect

The NICE protocol is in: the request and reply messages, the parameter
definitions per entity, and `nml`, the object 19 listener NCP connects to.
READ INFORMATION works for every entity at all four levels of detail, and
LOOP NODE loops through MIRROR.

What is left is deliberate rather than unfinished:

**SET is not implemented**, and neither is it in the Python: its `nml` falls
through to "Unsupported NICE request" and answers -1, unrecognized
function. We answer the same. Saying "privilege violation" instead would
imply some credential makes it work, and none does. This is not a gap
against upstream; it is upstream's behaviour.

**ZERO COUNTERS is refused** with -3, privilege violation. Here there *is*
a small gap: the Python implements it, and refuses it the same way only when
its own read-only flag is set. Ours is read-only permanently, because
zeroing counters is a write and the request's user name, password and
account are carried but not authenticated -- the decision the access
control note above is waiting on. Writing first and authenticating later is
the wrong order.

One visible consequence: the executor's "time since counters zeroed" is
reported, and it is really time since the node started, because nothing
ever zeroes them. That is honest only as long as ZERO stays refused.

**LOOP CIRCUIT and LOOP LINE** drive MOP loopback rather than MIRROR. The
loopback protocol is implemented and tested; what is missing is the wiring
from a NICE test request to it, and the state to track a loop in progress
on a circuit rather than on a logical link.

**Phase II NICE**, the `P2*` classes in `nicepackets.py`, is a different
message format for a different era, and shares the position of Phase II
generally: behind the Phase IV work because few people run it.

### The monitoring pages are a subset

The pages serve an index and one page per NICE entity, at each level of
detail. The Python's `http.py` and `html.py` also offer a per-connection NSP
view, an event display and a bridge page, and none of those is here.

They are served from the same `nice_read` the network management protocol
answers rather than from a second set of accessors into each layer. That is
the point: one description of what a circuit looks like, not two that
drift. It also means the pages can never show something NCP cannot read,
which is a constraint worth having rather than a limitation to remove.

HTTPS is not offered. `--https-port` is accepted and ignored so that a real
a real Python configuration file still loads.

---

## Different on purpose

Each of these is also stated in a comment at the site.

### Sequence numbers report "unordered" instead of raising

`Mod<N>` compares by RFC 1982 rules. With an even modulus, two values
exactly half the modulus apart have no defined order. Python raises
`TypeError`; we return `std::partial_ordering::unordered`, which makes
each of `<`, `<=`, `>` and `>=` false, and `comparable()` asks the question
directly.

C++ has a spelling for "no order" and Python does not. A receive path that
would rather not have an exception thrown at it gets a usable answer.

### The NSP flags byte is carried raw

The Python describes the first byte of an NSP packet as one bitmap whose
fields overlap: `subtype` is bits 4 to 6, while `int_ls` is bit 4, `bom`
bit 5 and `eom` bit 6, and which reading applies depends on the message
type. That works in Python because each packet class supplies only the
attributes that mean something for it.

In a struct with every field present at once, encoding would have to
reconcile two names for one bit. The byte is stored raw with an accessor
for each reading, so the two cannot disagree. The wire forms are identical,
and a regression test checks that against the Python's output.

### A LAN neighbour is addressed by the source it sent from

The Python addresses a LAN neighbour by the Phase IV derived address,
`Adjacency.macid = Macaddr (self.nodeid)`. We use the source address of the
frame the neighbour actually sent, and fall back to the derived address only
when nothing has been heard from a destination and a node id is all there is.

This is a deliberate departure from "follow the Python, because the Python
interoperates". Here it demonstrably does not: a real PDP-11 on the test
segment announces a derived id of `aa-00-04-00-13-04` while transmitting
from `08-00-2b-11-22-33`, and answers a loopback probe on the second
address and not at all on the first. The Python would send it traffic it
cannot receive. See `BUGS.md` for the measurement.

### The level 2 attached flag follows the spec, not the better definition

An area router is "attached" when it can reach an area other than its own.
The Python's own comment points out this is not the best definition: it makes
every area router in an area look attached as soon as the area is attached
at all, so out of area traffic can be drawn to a router that has no out of
area link and has to pass it on again.

The DNA Routing 2.0.0 definition is what other implementations use, so it
is what we use. Interoperability beats being right here.

### JSON is parsed by 200 lines of our own code

cJSON is packaged and was tried. It stores string values as NUL-terminated
C strings, so a NUL byte in the data truncates the value. The application
protocol tunnels arbitrary bytes through JSON strings and NUL is not a
corner case: MIRROR's function code is zero, the first byte of every
request it receives.

Any JSON library with a C-string interface has the same problem. One that
reports an explicit length would be fine, and would be worth switching to.

### Packet classes register through an explicit function

The Python registers each packet class in its family index when the class is
created. Doing the same with static initialisers does not work inside a
static library: the linker pulls in an archive member only when something
already needed references it.

`DN_PACKET_INDEX_REGISTERED` names a registration function instead. This is
the one place the port is much more verbose than the Python, and it is
forced rather than chosen. See [BUGS.md](BUGS.md) for how it showed up.

### Finished connections are kept, not freed

NSP keeps closed `Connection` objects and session control keeps finished
conversations, because a caller may still hold a pointer to one. Freeing
them inside a callback into the application was a use-after-free, which is
in [BUGS.md](BUGS.md).

Keeping them is a deliberate trade against a slow leak on a long running
node, which is [BUGS.md](BUGS.md) item 6. The real fix is reference
counting or a sweep, and it wants doing before this runs anywhere for
months at a time.

### One subprocess per connection

An object declared with `--file` gets a new process for each connection.
The protocol supports several connections on one process, keyed by handle,
and the Python's own applications exit when their connection closes because
that is how the Python drives them too.

Matching that behaviour is what lets the Python's applications run here
unchanged, which was the point.
