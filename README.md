# cppdecnet

A DECnet Phase IV node in C++, ported from Paul Koning's
[PyDECnet](https://github.com/pkoning2/pydecnet). It reads PyDECnet
configuration files and runs PyDECnet external applications unchanged.

Built with [HECnet](http://mim.softjar.se/) in mind: the hobbyist
DECnet network that links real and emulated DEC machines around the
world, with its main router a PDP-11 running RSX-11M-PLUS in Stockholm.
cppdecnet can run as an endnode or router on HECnet, and can act as a
gateway between a local Ethernet segment and the rest of the network.

## Contents

- [Status](#status)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Joining HECnet](#joining-hecnet)
- [Monitoring](#monitoring)
- [API](#api)
- [File access](#file-access)
- [Running as a service](#running-as-a-service)
- [Development](#development)
- [Licence](#licence)

## Status

Implemented:

- Data links: Multinet (TCP and UDP), Ethernet (UDP, TAP, pcap), DDCMP
  (UDP, TCP, telnet, serial)
- Routing: endnode, level 1 router, level 2 router
- NSP: logical links, segmentation, retransmission, flow control,
  interrupt messages, and congestion control after DEC-TR-353: a window
  that opens as segments are acknowledged and shuts down on a loss,
  go-back-N on a timeout, and an immediate resend on a NAK. VMS, which
  asks for no flow control and drops what it can't take, needs it for
  uploads of any size
- Session control: object database, built-in MIRROR and TIMESTAMP,
  external programs
- MOP: system id, counters, loopback, on 60-02 as VMS and RSX expect
- Event logging: filters, console/file/monitor sinks, remote sinks
- Network management: NICE listener (object 19), read only
- Monitoring pages over HTTP
- PyDECnet's JSON API over a Unix socket: the session API, for programs
  that open or accept logical links, and the mop API, for system ids,
  counters and loop tests from a program
- File access: `dnfal`, a FAL (object 17) serving a directory, with
  directory, read and, if allowed, create, delete and rename

Tested against PyDECnet, and against a PDP-11 running RSX on a real
Ethernet segment.

Not implemented yet: Phase II and Phase III neighbours, NICE SET and
ZERO, the MOP console carrier, access control checking, the API's
node, nsp and routing requests, and the bridge. See [TASKS.md](TASKS.md),
[NOTDONE.md](NOTDONE.md) and [BUGS.md](BUGS.md).

## Quick start

Requires a C++20 compiler (GCC 13+ or Clang 16+), GNU Make and libcrypt
(`libcrypt-dev`, for dnfal's password hashes). libpcap (`libpcap-dev`) is
optional and needed only for pcap circuits. The build looks for it every
time but doesn't rebuild what's already built, so after installing it run
`make clean` and then `make`.

```sh
make
./build/release/bin/decnetd --log-level debug samples/endnode.conf
```

Build targets:

```sh
make                   # release build, including tests
make check             # build and run the tests
make BUILD=debug       # debug build with ASan and UBSan
make features          # show optional features detected
make todo              # list PORT: markers in the source
make help              # all targets
```

Output goes to `build/<flavour>/`: `bin/decnetd` (the daemon),
`bin/dnfal` (the file access listener, see [File access](#file-access)),
`bin/dnping` (loop test tool) and `lib/libdecnet.a`.

Daemon options:

```
  -L, --log-level LEVEL   trace, debug, info, warning, error
  -e, --log-file FILE     log to FILE instead of stderr
  -V, --version           print the version and exit
  -h, --help              print this message
```

## Configuration

The syntax is PyDECnet's. A minimal endnode:

```
routing 1.2 --type endnode
node 1.2 CPPND
node 1.1 PYNODE
circuit mul-0 Multinet 127.0.0.1:17700:connect --t3 15
```

Commands for features that are not implemented are parsed and ignored,
so existing PyDECnet files load. More examples are in `samples/`.

### Circuits

```
circuit <name> <Multinet|Ethernet|DDCMP> <device> [options]
```

| Device | Type | Privilege |
|---|---|---|
| `Multinet <host>:<port>:connect` | point to point over TCP, outbound | none |
| `Multinet <host>:<port>:listen` | point to point over TCP, inbound | none |
| `Multinet <host>:<port>[:<lport>]` | point to point over UDP | none |
| `Ethernet udp:<lport>:<host>:<rport>` | Ethernet frames in UDP | none |
| `Ethernet tap:<dev>` | Linux TAP device | root, or a tap owned by the user |
| `Ethernet pcap:<iface>` | real Ethernet | `CAP_NET_RAW`, `CAP_NET_ADMIN` |
| `DDCMP udp:<lport>:<host>:<rport>` | DDCMP over UDP | none |
| `DDCMP tcp:<lport>:<host>:<rport>` | DDCMP over TCP (SIMH compatible) | none |
| `DDCMP telnet:<lport>:<host>:<rport>` | DDCMP over a telnet connection | none |
| `DDCMP serial:<dev>[:<speed>]` | serial line | access to the device |

Common options: `--cost`, `--t1`, `--t3` (hello timer), `--mop`.
Ethernet circuits also take `--priority`, `--nr` and `--random-address`.

**Multinet.** One end connects and the other listens; the port defaults
to 700. UDP mode works but logs a warning, since UDP does not guarantee
ordered delivery.

**DDCMP.** Over TCP both ends listen and connect and the first connection
wins, as SIMH does. Serial lines run raw 8N1 with no flow control. The
synchronous framer is not implemented.

**Ethernet over UDP.** One Ethernet frame per datagram, for LAN testing
without privileges. `bridge:` is accepted as a synonym for `udp:`.

**TAP.** The device must already exist:

```sh
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip link set tap0 up
```

**pcap.** Grant the capabilities to the (release) binary rather than
running as root:

```sh
sudo setcap cap_net_raw,cap_net_admin+eip build/release/bin/decnetd
```

Frames are sent from the Phase IV address derived from the node number
(1.20 is `aa-00-04-00-14-04`) with the interface in promiscuous mode, so
wireless interfaces do not work. `tools/pcap-survey.sh <iface> <seconds>`
lists the stations already on a segment. Two endnodes do not form an
adjacency with each other; if the segment has no router, configure the
node as one. See `samples/pcap.conf` and `samples/pcap-router.conf`.

## Joining HECnet

HECnet connections, node names and node numbers are handled by Johnny
Billquist; see the [HECnet page](http://mim.softjar.se/) for how to
join. Nodes connect through Johnny's bridge program or over Multinet to
an existing node. cppdecnet does not implement the bridge protocol yet, so
use a Multinet link.

`samples/hecnet.conf` is a level 1 router joining a local Ethernet
segment to HECnet:

```
node 29.150 CPPNOD
routing 29.150 --type l1router

circuit eth-0 Ethernet pcap:eth0 --t3 10 --mop
circuit mul-0 Multinet 11.22.33.44:9618:connect --cost 10

http --http-port 8102
```

Replace the node address, interface, peer address and port with the ones
you were given. The far end listens on its side of the Multinet link.
Use `--type l2router` only if your node is to be an area router.

### Node names

`node @<file>` reads a node name database: lines of `<address> <NAME>`,
the format of the HECnet `nodenames.dat`.

`node @hecnet --cache <file>` keeps that list up to date from the one
Johnny Billquist maintains at `http://mim.softjar.se/hecnet.dat`:

```
node @hecnet --cache /var/lib/decnet/hecnet.dat
```

The configuration only ever reads the cache, so the node starts with the
names it had last time whether or not the network is up. The fetch runs
in the background afterwards and rewrites the cache; a failure is a
logged warning and nothing else, since node names change what a page says
and never how anything routes.

Refresh is weekly by default, `--refresh <seconds>` to change it, zero to
fetch once at startup. Each refresh sends `If-Modified-Since` with the
`Last-Modified` from the previous fetch, which MIM answers with `304 Not
Modified` when nothing has changed, so the usual weekly cost is one small
exchange and no rewrite.

A name the configuration gives is never overwritten by a fetched one, so
local overrides stay put wherever they appear in the file. `node @<url>`
also works if you want a list from somewhere else; only `http://` is
supported, as there is no TLS.

`decnetd --fetch-nodes <config>` refreshes the caches and exits, for cron
or for a first run:

```
$ decnetd --fetch-nodes /etc/decnet/myhecnet.conf
1257 node names from http://mim.softjar.se/hecnet.dat to /var/lib/decnet/hecnet.dat
$ decnetd --fetch-nodes /etc/decnet/myhecnet.conf
http://mim.softjar.se/hecnet.dat unchanged since Thu, 17 Sep 2026 22:52:00 GMT
```

`node @neighbours` learns names from the network itself, for the nodes
nobody's list has. Each neighbour is asked for the names it knows (as
`TELL n SHOW KNOWN NODES` would) once its adjacency is up, and again every
hour (`--refresh <seconds>` to change that). And any node a logical link
runs to, in either direction, is asked its own name (`TELL n SHOW
EXECUTOR`) if we have none for it. Open a file on an unnamed VMS node and
from then on you can call it by name. Learned names only fill gaps: they
never replace a name from the configuration or a fetched list, and never
give one name to two addresses. They are not kept across restarts; the
neighbours are simply asked again. An extension, not in PyDECnet.

```
node @neighbours
```

If a simulator on the same host shares the Ethernet segment, the host
needs a bridge and a tap device; see `samples/gateway/README.txt`.

## Monitoring

### NCP

Object 19 answers NICE READ INFORMATION for all entities, and LOOP NODE:

```
NCP> TELL CPPNOD SHOW KNOWN CIRCUITS
NCP> TELL CPPNOD SHOW EXECUTOR CHARACTERISTICS
```

SET returns "unrecognized function", as PyDECnet does. ZERO COUNTERS
returns "privilege violation".

### HTTP

```
http --http-port 8102
```

| Page | Contents |
|---|---|
| `/` | node summary |
| `/nodes` | nodes (`?all=1` includes unreachable nodes) |
| `/circuits` | circuits and adjacencies |
| `/lines` | lines |
| `/areas` | reachable areas |
| `/modules` | modules |
| `/logging` | event sinks and filters |

Each page accepts `?info=summary|status|char|counters`.

The node pages hide entries with nothing to report and say how many, with
a **Show all** button at the foot that adds `?all=1`; the button on the
full page hides them again. "Nothing to report" means no parameters at
all -- which is every node but the executor on a characteristics read --
or, on a counters read, a counter set that is entirely zero apart from
"seconds since last zeroed", which is this node's uptime rather than
anything about the node listed. The executor is always shown. Circuits
and lines are never filtered: there are few of them and an idle one is
still worth seeing.

On a node carrying the HECnet list this is the difference between a
readable page and an unreadable one -- 121 rows of 2159 on the summary
page, 1 of 1257 on characteristics.

Counters are kept across the layers and reported on every page that has
them: the twelve NSP per-node counters plus the executor's eight, the
routing layer's per-circuit terminating, originating and transit counts
with circuit down, initialization failure, adjacency down and peak
adjacencies, the datalink traffic counters, and DDCMP's NAK and reply
timeout counters. `NOTDONE.md` lists the architected counters that are
still not kept and why.

The server listens on all interfaces with no authentication. HTTPS is not
supported; `--https-port` is ignored.

## API

```
api /run/decnet/api.sock --mode 660
```

Programs talk to the node over a Unix socket, one JSON object per line,
in PyDECnet's format, so PyDECnet's `decnet/connectors.py` and
`async_connectors.py` work unchanged. The socket defaults to `$DECNETAPI`
or `/tmp/decnetapi.sock`, mode 666 (on Windows, `decnetapi.sock` in
`%TEMP%`). A socket file left by a node that died is replaced; one that
still answers stops the second node's API from starting.

```
api --on-demand --idle 7200
```

`--on-demand` is an extension, for a desktop node that's on the network
only while it's being used. The node starts with its routing circuits
down, so it connects to nothing, and brings them up when the first API
client connects. Once the last client has gone, it takes them down again
after `--idle` seconds (two hours if not given), unless another client
connects first. The first request after a quiet spell takes a second or
two longer, while the circuit comes up.

`{}` lists the system and its APIs. The `session` API opens and accepts
logical links:

| Request `type` | Fields | Reply, then events |
|---|---|---|
| `connect` | `dest` (name or address), `remuser` (number or name), `localuser`, `data`, `username`, `password`, `account`, `proxy` | `connecting` with a `handle`, then `accept` or `reject` with a `reason` |
| `bind` | `num` and/or `name` | `bind` with a handle; inbound links arrive as `connect` with `listenhandle` |
| `accept`, `reject` | `handle`, `data` | `runstate` once the link is running |
| `data`, `interrupt` | `handle`, `data` | `data`, `interrupt` from the far end |
| `disconnect`, `abort` | `handle`, `data` | `disconnect` with a `reason` from the far end |

Byte strings are latin-1 JSON strings. A `tag` on a request comes back on
its reply. Disconnecting a bind handle withdraws the object. When a client
goes away its links fail with reason 38 ("object failed") and its objects
are withdrawn.

```python
from decnet.connectors import SimpleApiConnector
api = SimpleApiConnector ("/run/decnet/api.sock")
conn, reply = api.connect (dest = "MIM", remuser = 25)     # MIRROR
conn.data (b"\x00hello")
print (bytes (conn.recv ()))                               # b"\x01hello"
conn.disconnect ()
```

The `mop` API works on Ethernet circuits with `--mop`. A node that runs
only MOP, with no `routing` line, still has an API, offering only `mop`.
`circuit` names the circuit; it can be left out when there is only one.
A station (`dest`) is an Ethernet address, or a node name or address,
which stands for its DECnet Ethernet address.

| Request `type` | Fields | Reply |
|---|---|---|
| `get` | | `circuits`: each one's `name`, `hwaddr`, `macaddr`, `services` |
| `sysid` | `circuit` | `sysid`: every station heard, with its `srcaddr`, `software`, `device`, `processor`, `services`... and `age` in seconds |
| `sysid` | `dest`, `timeout` | asks that station: `status` `ok` with `sysid`, or `timeout` |
| `counters` | `dest`, `timeout` | `status`, and the station's Ethernet counters |
| `loop` | `dest` (one, a list of up to three, or none for the loopback multicast), `timeout`, `packets`, `fast` | `status`, `dest` (who answered), `delays`: each round trip in seconds, -1 for none |

The replies to requests that ask another station come when it answers,
matched to the request by its `tag`. `timeout` is in seconds, 1 to 60,
default 3. A loop of several packets pauses a second after each answer,
as PyDECnet does, unless `fast` is true.

[PathNoWorks](https://github.com/RichardPar/PathNoWorks) is built on
this API: network management, file access, a FUSE mount, remote login,
mail, MOP, X11 over DECnet and a Qt desktop, all for Linux. Its
`build.sh` clones cppdecnet for you.

The session and mop APIs are implemented; the node, nsp and routing ones
are not. Anyone who can open the socket can make and accept connections
as this node, so set the mode accordingly.

## File access

`dnfal` is a File Access Listener: it lets other nodes list, read and
write files in one directory. decnetd runs it as object 17, one process
per connection, in place of PyDECnet's `fal.py`. It works with VMS
`DIRECTORY`, `TYPE`, `COPY`, `RENAME` and `DELETE`, and with the
PathNoWorks file tools.

### Setting up a file server

This makes the machine a file server for the rest of the network, with a
public directory anyone may read and a private one for you. The examples
assume decnetd runs as the user `richard` and is set up as a service the
way `tools/install-decnetd.sh` does it, with its configuration in
`/etc/decnet/decnetd.conf`.

**1. The directories.** dnfal serves one directory tree, the root, and
reads and writes files as the user decnetd runs as, so that user must own
it:

```sh
sudo mkdir -p /srv/decnet/pub /srv/decnet/richard
sudo chown -R richard: /srv/decnet          # or decnet:, if it runs as decnet
echo "Welcome to PNW" > /srv/decnet/pub/readme.txt
```

**2. The users file.** Who may connect, with what password, to which
directory, and whether they may write. Make a password hash first;
`dnfal --hash` asks for the password and prints the hash:

```sh
$ dnfal --hash
Password:
$y$j9T$D2rJ5...
```

Then write `/etc/decnet/fal.users`:

```
# user    password hash      directory   access
RICHARD   $y$j9T$D2rJ5...    richard     rw
GUEST     -                  pub         ro

# Connections that give no user at all.  Leave this out to refuse them.
*         -                  pub         ro

# Proxy access: users on other nodes, let in without a password.
proxy     VAXXY::RICHARD     richard
proxy     *::SYSTEM          -
```

It holds password hashes, so keep it from other users, while letting
decnetd's user read it:

```sh
sudo chown root:richard /etc/decnet/fal.users
sudo chmod 640 /etc/decnet/fal.users
```

**3. The object.** Add this line to `/etc/decnet/decnetd.conf`, all on
one line, and restart decnetd:

```
object --number 17 --name FAL --file /usr/local/bin/dnfal --argument /srv/decnet --argument users=/etc/decnet/fal.users
```

```sh
sudo systemctl restart decnetd
```

The users file is read again for every connection, so changes to it apply
at once, with no restart. Only the `object` line needs one.

**4. Try it.** From another Linux machine with PathNoWorks, where PNW is
this node:

```sh
$ pnw-dir 'PNW::'                           # no user: the "*" entry, pub
Directory PNW::/

readme.txt                            1  02-OCT-26 10:36:54  [richard]  (,RWD,RWD,R)

Total of 1 file, 1 block.
$ pnw-type 'PNW"GUEST"::readme.txt'
Welcome to PNW
$ pnw-dir 'PNW"RICHARD secret"::'           # RICHARD's own directory
$ pnw-copy notes.txt 'PNW"RICHARD secret"::'
notes.txt -> PNW::/notes.txt (6 bytes, text)
$ pnw-copy notes.txt 'PNW"GUEST"::'
pnw-copy: Open error: privilege violation (OS denies access).
$ pnw-dir 'PNW"RICHARD wrong"::'
pnw-dir: cannot connect to FAL: Access control rejected
```

And from VMS:

```
$ DIRECTORY PNW::                              ! proxy, or the "*" entry
$ TYPE PNW"GUEST"::"readme.txt"
$ DIRECTORY PNW"RICHARD secret"::
$ COPY LOGIN.COM PNW"RICHARD secret"::
$ COPY PNW"RICHARD secret"::"notes.txt" []
```

VMS sends a password typed without quotes in capitals. dnfal tries one
that doesn't match as sent again in lower case, so a lower-case password
works either way; mixed-case ones are best avoided.

### Who gets in

Each connection is matched against the users file like this:

| The connection gives | It gets |
|---|---|
| a user and password that match a line | that line's directory and access |
| a user and a wrong password, or a user not in the file | refused |
| no user at all | the `*` line, or refused if there isn't one |
| a proxy request (no password, the proxy flag set) | the best `proxy` line, or the `*` line if none matches |

- Names match without regard to case.
- The hash is anything crypt(3) accepts: `dnfal --hash`,
  `openssl passwd -6` or `mkpasswd`. `-` means no password.
- A relative directory is under the root given to dnfal; an absolute one
  is used as it is. Nobody gets outside their directory, through `..` or
  a symbolic link.
- `rw` lets that user create, delete and rename files; `ro` is read only.

Proxy lines work as a VMS proxy database does. A proxy request says who
the user is at the far node; the most specific line wins (node and user,
then node, then user, then `*::*`), and `-` refuses:

```
proxy   VMSNOD::RICHARD   richard      # that user, from that node
proxy   VMSNOD::*         guest        # anyone else from that node
proxy   *::SYSTEM         -            # SYSTEM from anywhere: never
```

A node is a name or an address. With no matching line a proxy request
gets the `*` entry, as VMS falls back to its default account. The name a
proxy request gives is never taken as a user in the file. Proxy access
trusts the far node to say truthfully who its user is, as DECnet always
has, so give it out only to nodes you trust. VMS asks for proxy access
whenever no user and password are given, unless its executor's outgoing
proxy is disabled. From PathNoWorks, `pnw-dir --proxy 'PNW::'` asks as
your Linux login name.

Without a users file at all, anyone who can reach the node can read the
root, and, with `--argument rw`, change it too. That's fine on a private
network of your own, but please don't put a FAL on HECnet without one.

### Logging and trouble

dnfal logs each connection through decnetd (`journalctl -u decnetd`):

```
dnfal: connection from VAXXY (29.157) user GUEST, /srv/decnet/pub read only
dnfal: connection from VAXXY (29.157) user RICHARD by proxy as RICHARD, /srv/decnet/richard read/write
dnfal: access control rejected for user RICHARD from VAXXY (29.157)
```

| What you see | Why |
|---|---|
| `Access control rejected`; VMS says the login information is invalid | wrong user or password, no `*` line for a connection without one, or a proxy refused with `-`. It comes after a second's delay, on purpose |
| `Unrecognized object` (VMS: `%SYSTEM-F-NOSUCHOBJ`) | no `object --number 17` line, or decnetd wasn't restarted after adding it |
| `privilege violation` on a write | the user's access is `ro`; or the directory isn't writable by decnetd's user |
| decnetd's log says the users file has a mistake | it names the line; nobody gets in until it's fixed |

`--argument trace` logs every DAP message and its bytes, seen with decnetd
at `--log-level debug`. It's the first thing to look at when a transfer
misbehaves.

### Other arguments

dnfal tells requesters it is VMS with an RMS-32 file system: VMS `COPY`
will not send a binary file to a file system it thinks is ULTRIX's, which
is what PyDECnet's FAL says it is. `--argument ostype=192 --argument
filesys=13` says that instead.

File names may be Unix (`sub/file.txt`) or VMS style
(`[SUB]FILE.TXT;1`); names match without regard to case, and versions are
ignored. Text sent as variable length records with carriage return
control is stored with a newline per record; anything else is stored as
received. A file being written is renamed into place only when the
transfer completes.

Tested with OpenVMS VAX 6.2 as the requester: DIRECTORY, TYPE, COPY in
both directions (text, fixed and variable binary), RENAME and DELETE.

## Running as a service

`tools/install-decnetd.sh` does the whole job on a Linux machine with
systemd. It asks:

- the node's type (endnode, level 1 or level 2 router), name and address;
- how it reaches the network, once per circuit: Multinet to a peer that
  listens (its IP address or host name, and port), Multinet listening for
  a peer (and which address may connect), or Ethernet through pcap;
- the peer's DECnet name and address, so you can use its name;
- for an endnode, whether to disconnect when idle (`api --on-demand`);
- whether to keep HECnet's node list and serve the monitoring pages;
- whether to run as you or as a `decnet` user of its own.

Then it shows the configuration, and once you say so, builds decnetd if
needed, installs it under `/usr/local`, writes `/etc/decnet/decnetd.conf`
and a systemd unit with only the capabilities the circuits need, and
starts it. Run it as yourself; it uses sudo where it must. If decnetd is
already running by hand, it offers to stop it first rather than put the
node on the network twice.

```sh
tools/install-decnetd.sh                  # ask, install, start
tools/install-decnetd.sh --dry-run DIR    # ask, write the files to DIR only
tools/install-decnetd.sh --uninstall      # stop and remove the service
```

To do it by hand instead, `samples/gateway/decnetd.service` runs the
daemon as an unprivileged user with only `CAP_NET_RAW` and `CAP_NET_ADMIN`:

```sh
sudo install -d /etc/decnet
sudo install -m 0644 samples/hecnet.conf /etc/decnet/myhecnet.conf
sudo install -m 0755 build/release/bin/decnetd /usr/local/bin/decnetd
sudo install -m 0644 samples/gateway/decnetd.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now decnetd
```

Edit `User=`, `Group=` and the configuration path in the unit to suit.
`tools/deploy-arm.sh` copies, builds and installs on a remote host over
ssh.

## Development

```
include/decnet/     public headers
  common/           types, logging, timers, work queue, state machine, CRC, JSON
  packet/           packet layout framework
  datalink/         Multinet, Ethernet, DDCMP
  routing/          routing packets, adjacencies, circuits, routers
  nsp/              NSP
  session/          session control
  mop/              MOP
  nice/             NICE and the network management listener
  events/           event logging
  http/             monitoring pages
src/                implementation
tools/              command line tools and scripts
tests/              tests, one binary per test_*.cc
mk/                 build fragments
samples/            example configurations
RSX/                RSX-11M-PLUS test programs, built on the PDP-11
```

Each source file names the PyDECnet module it was ported from, and
unfinished code is marked `PORT:`. Design notes are in
[PORTING.md](PORTING.md).

`make check` runs the tests: unit tests ported from PyDECnet, wire format
checks against PyDECnet output, and end to end tests that run two nodes
in one process over real sockets. `BUILD` defaults to release; use
`make check BUILD=debug` for the sanitizer build.

### RSX programs

`RSX/` has a few MACRO-11 programs for testing from an RSX box, like
TSTIME for querying the TIMESTAMP object. They get built on the PDP-11,
not by make. Source and a built .TSK are both in there; see
[RSX/README.md](RSX/README.md).

## Licence

BSD 3-clause. PyDECnet is copyright Paul Koning, also BSD 3-clause; its
notice is included in [LICENSE](LICENSE).

Thanks to Paul Koning for PyDECnet, and to Johnny Billquist and the
HECnet community for keeping DECnet running.

"DECnet" may be a trademark. Digital and its successors have no
involvement in this project.
