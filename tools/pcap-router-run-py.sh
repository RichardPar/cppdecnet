#!/bin/sh
# Start samples/pcap-router.conf on PyDECnet.  Extra arguments are passed on.
cd "$(dirname "$0")/.." || exit 1
exec sudo PYTHONPATH=${PYDECNET:-/home/richard/Source/Decnet/pydecnet/pydecnet} \
     python3 -m decnet.main "$@" samples/pcap-router.conf
