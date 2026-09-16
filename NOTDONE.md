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
- NSP node counters are not reported.

### NSP

- Inbound flow control is not requested (`SVC_NONE`), same as PyDECnet.
- Interrupt credit is not offered to the remote end, same as PyDECnet.
- Acknowledgements are never delayed.

### Routing

- Phase II and Phase III neighbours.
- Packet loss events (4.0 to 4.3) do not name the circuit, because
  `forward()` does not receive it.
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

### Monitoring

The HTTP pages cover the NICE entities only. There is no per-connection
NSP page, event display, bridge page, REST API or HTTPS. Pages are built
from `nice_read`, so they only show what NICE can report.

### Process

- One node per process. Additional configuration files are ignored with a
  warning.
- Peer names are re-resolved on the receive thread rather than a helper
  thread.
