#!/bin/bash
# Copy, build and install decnetd on a remote host, then restart it.
#
#     tools/deploy-arm.sh [user@host]
#
# Installs /usr/local/bin/decnetd, /etc/decnet/myhecnet.conf and
# /etc/systemd/system/decnetd.service.  Host networking (samples/gateway/)
# is not touched.
#
# Sources are touched after rsync because rsync preserves mtimes, which can
# leave make with stale objects.

set -e
TARGET="${1:-richard@192.168.10.151}"
IFACE="${IFACE:-br0}"
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
