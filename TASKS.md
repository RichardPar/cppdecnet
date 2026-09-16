# TODO

Open work. Unfinished code is also marked with `PORT:` comments
(`make todo`). Intentional omissions are in [NOTDONE.md](NOTDONE.md) and
known defects in [BUGS.md](BUGS.md).

## NSP

- [ ] Request flow control for inbound data (send link service messages
      as a receiver)
- [ ] Offer interrupt credit to the remote end
- [ ] Delayed acknowledgement
- [ ] Phase II connections (no connect acknowledgement)

## Session control

- [ ] Check access control (PyDECnet uses PAM)
- [ ] Run objects as a different user
- [ ] Outbound connections from applications, and `bind`
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

- [ ] MOP console carrier
- [ ] Raise data link and physical line events (classes 5 and 6)
- [ ] Include the source circuit in packet loss events (4.0 to 4.3)
- [ ] ZERO COUNTERS
- [ ] LOOP CIRCUIT and LOOP LINE
- [ ] Phase II NICE
- [ ] NSP node counters in NICE

## Monitoring and API

- [ ] Per-connection NSP page, event display, bridge page
- [ ] HTTPS
- [ ] `apiserver`: JSON API over a Unix socket

## Other

- [ ] `bridge`
- [ ] `dap` and `dap_packets` (FAL)
- [ ] More than one node per process (`src/main/main.cc`)
- [ ] Background name resolution
- [ ] Per-state packet type filtering, and the `ru4l1`, `ru4l2`, `ru3r`
      substates
- [ ] Daemon mode: `--daemon`, pid file, log rotation
