# Limitations

What is not implemented and why. Planned work is in [TASKS.md](TASKS.md);
intentional differences from PyDECnet are listed in
[PORTING.md](PORTING.md).

## Not planned

- **Network mapper** (`mapper.py`). HECnet specific and unrelated to the
  protocol stack.
- **In-process objects** (`--module`). Use built-in objects or `--file`
  programs instead.
- **Changing the interface MAC address.** A pcap circuit transmits with
  the derived Phase IV address and captures promiscuously instead, so the
  interface stays usable for other traffic. Wireless interfaces are not
  supported as a result.

## Not yet implemented

### Access control

User name, password and account are carried in connect messages but not
verified. Objects always run as the daemon's user. Event 2.1 (access
control reject) is never raised. For the same reason NICE is read only.

### NICE

- READ INFORMATION and LOOP NODE are implemented.
- SET returns "unrecognized function". PyDECnet does not implement SET
  either.
- ZERO COUNTERS returns "privilege violation". Counters are never zeroed,
  so "time since counters zeroed" is the node uptime.
- LOOP CIRCUIT and LOOP LINE return "unrecognized function". MOP loopback
  exists but is not connected to NICE.
- Phase II NICE is not implemented.

### NSP

- Inbound flow control is not requested (`SVC_NONE`), same as PyDECnet.
- Interrupt credit is not offered to the remote end, same as PyDECnet.
- Acknowledgements are never delayed.

### Routing

- Phase II and Phase III neighbours.
- Packet types are checked after parsing rather than filtered per state.

### MOP

The console carrier messages parse, but reservation and the client and
server are not implemented. Load and dump are not implemented (nor are
they in PyDECnet).

### Data links

- DDCMP synchronous framer.
- GRE.
- Data link and physical line events (classes 5 and 6) are defined but
  never raised.

### Counters

Implemented across the layers: the twelve NSP per-node counters, the eight
executor counters, the routing layer's circuit counters, the datalink
traffic counters and the DDCMP error counters. What is still missing:

- **Congestion loss** (circuit counters 802 and 812). Nothing queues, so
  there is no congestion to lose packets to. PyDECnet omits them too.
- **Corruption loss** (805), **selection intervals** (1050) and **user
  buffer unavailable** (1065). Not detected.
- **Local buffer errors** (1041). We never NAK for want of a buffer, so
  the count would always be zero. PyDECnet does not keep it either.
- **Ethernet hardware error counters** (1060 to 1062, 1064): send failure,
  collision detect check failure, receive failure, data overrun. These are
  controller statistics, and neither pcap nor a UDP tunnel reports them.
- **Oversized packet loss** (903) and **packet format error** (910) are
  reported but never incremented: the decode path rejects such packets
  before routing sees them. PyDECnet defines them and never counts them
  either.

### HTTP client

`node @<url>` fetches over plain HTTP only. There is no TLS, so `https://`
URLs are refused rather than fetched in the clear; the HECnet list is
served over HTTP, and linking a TLS library for one small file would be a
large dependency for a small job.

Conditional refresh uses `If-Modified-Since` only. `ETag` is parsed by
nothing here because MIM does not send one.

### Monitoring

The HTTP pages cover the NICE entities only. There is no per-connection
NSP page, event display, bridge page, REST API or HTTPS. Pages are built
from `nice_read`, so they only show what NICE can report -- which is why
the `logging` page is empty: `EventLogger::nice_read` is a stub, as the
NICE encoding of logging event lists is not implemented upstream either.

### Process

- One node per process. Additional configuration files are ignored with a
  warning.
- Peer names are re-resolved on the receive thread rather than a helper
  thread.
