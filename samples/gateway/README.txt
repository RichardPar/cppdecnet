Host configuration for a gateway node
=====================================

Network setup for a host where decnetd shares an Ethernet segment with a
SIMH guest on the same machine. The interface names and the MAC address
in 10-br0.yaml are for one specific machine; change them to match yours.

    10-br0.yaml        -> /etc/netplan/
    30-tap0.netdev     -> /etc/systemd/network/
    40-tap0.network    -> /etc/systemd/network/
    decnetd.service    -> /etc/systemd/system/

Bridge
------

libpcap on a physical NIC does not see frames sent by the host itself, so
a pcap circuit on the NIC cannot talk to a guest on a tap device. A bridge
fixes this.

br0 contains end0 and tap0, the host address is on br0, decnetd uses
`pcap:br0`, and SIMH attaches to tap0.

Tap device
----------

tap0 is created by systemd-networkd at boot, owned by the user SIMH runs
as, and added to the bridge. If SIMH creates the tap itself, it may try to
transmit before the interface is up, which fails with EIO:

    Sockets: _eth_write(tap):  error 5 - Input/output error

and the guest's Ethernet driver may give up for the rest of the boot.

Install these files with the simulator stopped, run `networkctl reload`,
check that `networkctl status tap0` shows the NetDev file, then start the
simulator.

Checking
--------

    ip -br addr                     br0 has the address, end0 and tap0 are up
    ls -l /sys/class/net/*/master   both point to br0
    cat /sys/class/net/tap0/statistics/rx_packets    increases when the guest sends
    curl -s localhost:8102/circuits                  adjacencies

To see the router list in our hellos:

    tcpdump -i br0 -nn -t -X -c 1 'ether src aa:00:04:00:9e:74'
