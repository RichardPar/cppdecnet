Host configuration for the gateway node
=======================================

What the machine needs around decnetd when it shares one Ethernet segment
with a simulated PDP-11.  These are the files that actually run on the
Rock Pi (node 29.150 CPPNOD); copy them, do not adapt them blind -- the
interface names and the MAC in 10-br0.yaml are that machine's.

    10-br0.yaml        -> /etc/netplan/
    30-tap0.netdev     -> /etc/systemd/network/
    40-tap0.network    -> /etc/systemd/network/
    decnetd.service    -> /etc/systemd/system/

Why a bridge at all
-------------------

libpcap on a shared physical NIC never sees the host's own
locally-originated frames: a switch does not reflect a frame back out the
port it came in on.  So decnetd on the raw NIC can reach other machines on
the wire but can never reach a guest on a tap, and the guest can never
reach it.  A real Linux bridge loops traffic between its own members in
software, which is what makes one DECnet circuit reach both the segment
and the simulator.  simh's own run.ini says the same thing from the other
side.

So: br0 holds end0 and tap0, the host address lives on br0, decnetd's
circuit is `pcap:br0`, and the simulator attaches to tap0.

Why the tap is created here and not by the simulator
----------------------------------------------------

simh will make its own tap, but it transmits on it immediately -- before
networkd has noticed the new device, enslaved it and brought it up -- and a
write to a tap whose interface is still down fails with EIO.  simh reports
that as

    Sockets: _eth_write(tap):  error 5 - Input/output error
    %SIM-ERROR: XU: Eth: Error Transmitting packet: Input/output error
    You may need to run as root.

which is misleading twice over: it is not a permission problem (opening the
device is the part that needs CAP_NET_ADMIN, and that succeeded), and root
does not fix it.  What follows is worse than the error: RSX's DELUA driver
gives up after those failures and the node stays off the network for the
rest of the boot, while NCP still reports the circuit as On -- that is the
administrative state, not evidence of anything working.

30-tap0.netdev creates tap0 at boot instead, owned by the user the
simulator runs as, and 40-tap0.network puts it in the bridge.  The device
is then up and bridged before simh starts, and there is no race to lose.
An "up" tap with no carrier is the right state to find: carrier appears
when the simulator opens the fd, and a write only needs the interface up.

Installing this while the simulator is running does not take effect, and
the way it fails is quiet.  networkd cannot create a device whose name is
already taken --

    tap0: TUNSETIFF failed: Device or resource busy
    tap0: Failed to create netdev: Device or resource busy

-- and then it configures simh's own tap instead, a moment after simh has
already tried to transmit on it.  So the ordering looks fixed and is not.
Stop the simulator, `networkctl reload`, check that `networkctl status
tap0` names a NetDev File, and only then start it again.

Checking it
-----------

    ip -br addr                     br0 has the address, end0 and tap0 are up
    ls -l /sys/class/net/*/master   both point at br0
    cat /sys/class/net/tap0/statistics/rx_packets    climbs = the guest sends
    curl -s localhost:8102/circuits                  adjacencies, by name

The adjacency being up is itself the two-way proof on a LAN: a router
adjacency only comes up when each end finds itself named in the other's
hello.  To see that on the wire:

    tcpdump -i br0 -nn -t -X -c 1 'ether src aa:00:04:00:9e:74'

and read the E-list at the end of the hello -- 7 reserved bytes, a count,
then one 7-byte entry per router: 6 bytes of address and a byte holding the
priority with the two-way bit at 0x80.
