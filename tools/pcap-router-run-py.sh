#!/bin/sh
# Run samples/pcap-router.conf with PyDECnet ($PYDECNET).  Extra arguments
# are passed to decnet.main.
cd "$(dirname "$0")/.." || exit 1
exec sudo PYTHONPATH=${PYDECNET:-/home/richard/Source/Decnet/pydecnet/pydecnet} \
     python3 -m decnet.main "$@" samples/pcap-router.conf
