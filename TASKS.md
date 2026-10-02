# TODO

Open work. Unfinished code is also marked with `PORT:` comments
(`make todo`). Intentional omissions are in [NOTDONE.md](NOTDONE.md) and
known defects in [BUGS.md](BUGS.md).

## NSP

- [ ] Request flow control for inbound data (send link service messages
      as a receiver)
- [ ] Offer interrupt credit to the remote end
- [x] Delayed acknowledgement, when the sender sets the delay bit: held
      one tick for a reply to carry it, as PyDECnet does
- [ ] Set the delay bit on our own data segments
- [ ] Phase II connections (no connect acknowledgement)
- [x] Congestion control after DEC-TR-353: a window that opens with each
      acknowledgement and shuts on a loss, go-back-N on a timeout, an
      immediate resend on a NAK (uploads to VMS stalled without it)
- [x] Retransmit timer from a weighted average of measured round trips
      (one to five seconds), as PyDECnet does

## Session control

- [ ] Check access control (PyDECnet uses PAM)
- [ ] Run objects as a different user
- [x] Outbound connections from applications, and `bind` (through the API)
- [ ] `pmr` (object 123)
- [ ] Finish `tools/dnping`

## Routing

- [ ] Phase III neighbours: `PtpInit3`, 8-bit addresses, home area
- [ ] `Phase3EndnodeRouting` and `Phase3Router`
- [ ] Phase II: `NodeInit`, `NodeVerify`, routing by name, `ru2`,
      `intercept.py`
- [ ] LAN router table overflow (`--nr`) and the `adj_rej` event
- [ ] Check endnode hello test data on receipt
- [ ] Take a point to point circuit down when an init arrives in `ru`
- [ ] `start_works` false for Multinet over UDP

## Data links

- [ ] DDCMP synchronous framer
- [ ] GRE
- [ ] Test DDCMP against SIMH

## MOP, events, NICE

- [x] MOP system id on 60-02, the console protocol type, where VMS and
      RSX listen for it
- [x] Accept a system id whose last item runs past the end (RSX pads it)
- [ ] MOP console carrier: reservation, client and server, an API request
      (`connect` with `dest` and `verification`, `data`, `disconnect`),
      as PyDECnet's `CarrierClientConnection`
- [ ] MOP load and dump (PyDECnet has neither)
- [ ] Raise data link and physical line events (classes 5 and 6)
- [x] Include the source circuit in packet loss events (4.0 to 4.3)
- [ ] ZERO COUNTERS
- [ ] LOOP CIRCUIT and LOOP LINE
- [ ] Phase II NICE
- [x] NSP node counters in NICE
- [x] Routing circuit counters, executor counters, DDCMP error counters,
      broadcast line counters

## Node names

- [x] Fix `node @file`, which silently loaded nothing
- [x] `node @hecnet --cache FILE`, fetched over HTTP, refreshed weekly
      with `If-Modified-Since`
- [x] `decnetd --fetch-nodes` for cron
- [ ] Fetch over DECnet instead: `MIM::HECNET:` needs DAP/FAL, or NICE
      `COPY KNOWN NODES FROM MIM` needs a NICE client
- [x] `node @neighbours`: names learned from each neighbour's known nodes,
      and from any node a link runs to (its executor), filling gaps only
- [ ] Keep learned names across restarts (a `--cache` file, as `@hecnet`)

## Monitoring and API

- [x] Delete `src/http/monitor.cc`, an older monitoring interface nothing used
- [ ] `logging` pages are empty: `EventLogger::nice_read` is a stub
- [ ] Per-connection NSP page, event display, bridge page
- [ ] HTTPS
- [x] `apiserver`: JSON API over a Unix socket, session API
- [x] Nested JSON objects and numbers with fractions
- [x] The API's mop requests: `get`, `sysid`, `counters`, `loop`
- [x] An API on a node that runs only MOP, with no `routing` line
- [ ] The API's node, nsp, routing and ncp requests

## Other

- [ ] `bridge`
- [x] `dap_packets`: DAP message codec (`src/dap/`), checked against
      PyDECnet's encoding
- [x] FAL server (object 17): `dnfal`, directory, read, create, erase,
      rename, confined to a root directory
- [x] Access control for dnfal: its own user file (crypt hashes, per user
      directory and read/write)
- [ ] PAM login and running as that user, for a decnetd run as root, as
      PyDECnet does
- [x] Proxy access: proxy lines in dnfal's user file.  Check against VMS
      which field carries the remote user (RQSTRID or the source end user)
- [ ] dnfal: block mode and record access, append, submit/execute
- [ ] More than one node per process (`src/main/main.cc`)
- [ ] Background name resolution
- [ ] Per-state packet type filtering, and the `ru4l1`, `ru4l2`, `ru3r`
      substates
- [ ] Daemon mode: `--daemon`, pid file, log rotation

## Build

- [ ] Rebuild when the compiler flags change. Installing libpcap after a
      build changes `FEATURE_FLAGS`, but nothing is recompiled until
      `make clean`
- [ ] Rebuild `libdecnet.a` when a source file is removed. Nothing in it
      is newer, so the old object stays in the archive until `make clean`
- [ ] Cache the feature probes in `$(BUILDDIR)/features.mk`, as
      `mk/config.mk` says it does; they run on every `make` now
