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
- [Running as a service](#running-as-a-service)
- [Development](#development)
- [Licence](#licence)

## Status

Implemented:

- Data links: Multinet (TCP and UDP), Ethernet (UDP, TAP, pcap), DDCMP
  (UDP, TCP, telnet, serial)
- Routing: endnode, level 1 router, level 2 router
- NSP: logical links, segmentation, retransmission, flow control,
  interrupt messages
- Session control: object database, built-in MIRROR, external programs
- MOP: system id, counters, loopback
- Event logging: filters, console/file/monitor sinks, remote sinks
- Network management: NICE listener (object 19), read only
- Monitoring pages over HTTP

Tested against PyDECnet, and against a PDP-11 running RSX on a real
Ethernet segment.

Not implemented yet: Phase II and Phase III neighbours, NICE SET and
ZERO, the MOP console carrier, access control checking, the JSON API,
the bridge and DAP/FAL. See [TASKS.md](TASKS.md),
[NOTDONE.md](NOTDONE.md) and [BUGS.md](BUGS.md).

## Quick start

Requires a C++20 compiler (GCC 13+ or Clang 16+) and GNU Make. libpcap
is optional and needed only for pcap circuits.

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

Counters are kept across the layers and reported on every page that has
them: the twelve NSP per-node counters plus the executor's eight, the
routing layer's per-circuit terminating, originating and transit counts
with circuit down, initialization failure, adjacency down and peak
adjacencies, the datalink traffic counters, and DDCMP's NAK and reply
timeout counters. `NOTDONE.md` lists the architected counters that are
still not kept and why.

The server listens on all interfaces with no authentication. HTTPS is not
supported; `--https-port` is ignored.

## Running as a service

`samples/gateway/decnetd.service` runs the daemon as an unprivileged user
with only `CAP_NET_RAW` and `CAP_NET_ADMIN`:

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
```

Each source file names the PyDECnet module it was ported from, and
unfinished code is marked `PORT:`. Design notes are in
[PORTING.md](PORTING.md).

`make check` runs the tests: unit tests ported from PyDECnet, wire format
checks against PyDECnet output, and end to end tests that run two nodes
in one process over real sockets. `BUILD` defaults to release; use
`make check BUILD=debug` for the sanitizer build.

## Licence

BSD 3-clause. PyDECnet is copyright Paul Koning, also BSD 3-clause; its
notice is included in [LICENSE](LICENSE).

Thanks to Paul Koning for PyDECnet, and to Johnny Billquist and the
HECnet community for keeping DECnet running.

"DECnet" may be a trademark. Digital and its successors have no
involvement in this project.
