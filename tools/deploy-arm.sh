#!/bin/bash
#
# Deploy this tree to the Rock Pi gateway and (re)start it there.
#
#     tools/deploy-arm.sh [user@host]
#
# Defaults to richard@192.168.10.151, the board that runs node 29.150
# CPPNOD.  Needs ssh access and sudo on the far end; it will ask for both
# rather than carrying a password.
#
# What it installs:
#     /usr/local/bin/decnetd
#     /etc/decnet/myhecnet.conf          (from samples, with pcap:end0)
#     /etc/systemd/system/decnetd.service
#
# It does NOT touch the host's networking.  The bridge and tap that the
# circuit depends on are in samples/gateway/, with the reasoning; install
# those once, by hand, on a machine you can still reach if you get it
# wrong.
#
# The `touch` after the copy is not optional.  rsync preserves source
# mtimes, so a file whose newly copied version is older than the object
# built from the previous copy leaves make believing the object is current
# -- and the result is a daemon built from a mixture of two trees, which
# looks exactly like a fixed bug that is not fixed.  See BUGS.md.

set -e
TARGET="${1:-richard@192.168.10.151}"
IFACE="${IFACE:-br0}"   # the bridge, not the raw NIC -- see samples/gateway/
HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "== copying to $TARGET =="
rsync -az --delete --exclude build/ --exclude '*.o' "$HERE/" "$TARGET:cppdecnet/"

echo "== building =="
ssh "$TARGET" '
    set -e
    cd ~/cppdecnet
    find src include tests tools -type f \( -name "*.cc" -o -name "*.h" \) \
        -exec touch {} +
    make -j"$(nproc)" daemon
'

echo "== the service =="
sed "s|^circuit eth-0 Ethernet pcap:[^ ]*|circuit eth-0 Ethernet pcap:$IFACE|" \
    "$HERE/samples/myhecnet.conf" | ssh "$TARGET" 'cat > /tmp/myhecnet.conf'
scp -q "$HERE/samples/gateway/decnetd.service" "$TARGET:/tmp/decnetd.service"
ssh -t "$TARGET" '
    set -e
    sudo install -d -m 0755 /etc/decnet
    sudo install -m 0644 /tmp/myhecnet.conf   /etc/decnet/myhecnet.conf
    sudo install -m 0755 ~/cppdecnet/build/release/bin/decnetd /usr/local/bin/decnetd
    sudo install -m 0644 /tmp/decnetd.service /etc/systemd/system/decnetd.service
    sudo systemctl daemon-reload
    sudo systemctl enable decnetd
    sudo systemctl restart decnetd
'
echo "== running =="
ssh "$TARGET" 'systemctl --no-pager --full status decnetd | head -12'
