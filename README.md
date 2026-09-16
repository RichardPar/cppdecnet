# cppdecnet

A DECnet Phase IV implementation in C++, ported from Paul Koning's
PyDECnet. It uses the same configuration file syntax and the same
external application protocol, so existing PyDECnet configurations and
applications work with it.

## Features

- Data links: Multinet (TCP and UDP), Ethernet (UDP, TAP, pcap), DDCMP
  (UDP, TCP, telnet, serial)
- Routing: endnode, level 1 router, level 2 router
- NSP: logical links, segmentation, retransmission, flow control,
  interrupt messages
- Session control: object database, built-in MIRROR, objects run as
  external programs
- MOP: system id, counters, loopback
- Event logging: filters, console/file/monitor sinks, remote sinks
- Network management: NICE listener (object 19), read only
- Monitoring pages over HTTP

Tested against PyDECnet and against a PDP-11 running RSX on a real
Ethernet segment.

Not implemented yet: Phase II and Phase III neighbours, NICE SET and
ZERO, the MOP console carrier, access control checking, the JSON API,
the bridge and DAP/FAL. See [TASKS.md](TASKS.md) and
[NOTDONE.md](NOTDONE.md).

## Building

Requires a C++20 compiler (GCC 13+ or Clang 16+) and GNU Make. libpcap
is optional; without it pcap circuits are not available.

```sh
make                   # release build, including tests
make check             # build and run the tests
make BUILD=debug       # debug build with ASan and UBSan
make features          # show optional features detected
make todo              # list PORT: markers in the source
make help              # all targets
```

Output goes to `build/<flavour>/`:

```
bin/decnetd            the daemon
bin/dnping             loop test tool
lib/libdecnet.a        the library
```

## Running

```sh
./build/release/bin/decnetd --log-level debug samples/endnode.conf
```

```
  -L, --log-level LEVEL   trace, debug, info, warning, error
  -e, --log-file FILE     log to FILE instead of stderr
  -V, --version           print the version and exit
  -h, --help              print this message
```

Configuration commands for features that are not implemented are parsed
and ignored, so a PyDECnet configuration file will load.

### systemd

`samples/gateway/decnetd.service` runs the daemon as an unprivileged user
with `CAP_NET_RAW` and `CAP_NET_ADMIN`, which is all a pcap circuit needs.

```sh
sudo install -d /etc/decnet
sudo install -m 0644 samples/myhecnet.conf /etc/decnet/myhecnet.conf
sudo install -m 0755 build/release/bin/decnetd /usr/local/bin/decnetd
sudo install -m 0644 samples/gateway/decnetd.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now decnetd
```

`tools/deploy-arm.sh` does the same on a remote host over ssh.

If the node shares an Ethernet segment with an emulator on the same host,
see `samples/gateway/README.txt` for the bridge and tap setup.

## Circuits

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

### Multinet

One end connects and the other listens. The port defaults to 700.

```
circuit mul-0 Multinet 192.168.1.50:700:connect --t3 15
circuit mul-0 Multinet :700:listen
```

Multinet over UDP works but logs a warning, because UDP does not
guarantee ordered delivery. Use TCP where possible.

### DDCMP

```
circuit ddc-0 DDCMP udp:27801:192.168.1.50:27802 --t3 10
circuit ddc-0 DDCMP tcp:27801:192.168.1.50:27802 --t3 10
circuit ser-0 DDCMP serial:/dev/ttyUSB0:38400 --t3 10
```

Over TCP both ends listen and connect, and the first connection wins, as
SIMH does. Serial lines are set to raw 8N1 with no flow control. The
synchronous framer is not implemented.

### Ethernet over UDP

Each datagram carries one Ethernet frame. Useful for testing LAN
behaviour without privileges. The other end swaps the ports.

```
circuit eth-0 Ethernet udp:17802:127.0.0.1:17801 --random-address
```

`bridge:` is accepted as a synonym for `udp:`.

### TAP

```sh
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip link set tap0 up
```

```
circuit eth-0 Ethernet tap:tap0
```

### pcap

```
circuit eth-0 Ethernet pcap:eth0 --t3 10 --mop
```

Grant the capabilities to the binary instead of running as root:

```sh
sudo setcap cap_net_raw,cap_net_admin+eip build/release/bin/decnetd
```

This does not work with the debug build, since sanitizer builds refuse to
run with elevated privileges.

The circuit sends frames with the Phase IV address derived from the node
number (1.20 is `aa-00-04-00-14-04`) and captures in promiscuous mode. The
interface's own address is left alone. This does not work on wireless
interfaces.

`tools/pcap-survey.sh <iface> <seconds>` listens passively and lists the
Phase IV stations it hears. Use it to pick an unused node number.

Two endnodes will not form an adjacency with each other. If there is no
router on the segment, configure the node as a router.

See `samples/pcap.conf` and `samples/pcap-router.conf`.

### Example: Ethernet to Multinet gateway

```
routing 1.20 --type l1router

node 1.20 CPPGW
node 1.19 BAJI
node 1.10 FARSID

circuit eth-0 Ethernet pcap:eth0 --t3 10 --priority 64 --mop
circuit mul-0 Multinet 11.22.33.44:9999:connect --t3 15 --cost 10
```

The remote end:

```
routing 1.10 --type l1router
node 1.10 FARSID
node 1.20 CPPGW
circuit mul-0 Multinet :9999:listen --t3 15 --cost 10
```

All nodes are in area 1. If the remote node is in another area, use
`--type l2router`. The full file is `samples/l1-gateway.conf`.

## Monitoring

### NCP

Object 19 serves NICE READ INFORMATION for all entities and LOOP NODE:

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

The server listens on all interfaces and has no authentication. HTTPS is
not supported; `--https-port` is ignored.

## Source layout

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

Each source file names the PyDECnet module it was ported from. Unfinished
code is marked with `PORT:` comments.

See [PORTING.md](PORTING.md) for design notes.

## Testing

`make check` runs all tests. `BUILD` defaults to release; use
`make check BUILD=debug` for the sanitizer build.

The tests include unit tests ported from PyDECnet, wire format checks
against PyDECnet output, and end to end tests that run two nodes in one
process over real sockets.

## Licence

BSD 3-clause. PyDECnet is copyright Paul Koning, also BSD 3-clause; its
notice is included in [LICENSE](LICENSE).

"DECnet" may be a trademark. Digital and its successors have no
involvement in this project.
