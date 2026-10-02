#!/usr/bin/env bash
#
# install-decnetd.sh -- make this Linux machine a DECnet node that starts
# at boot: asks who the node is and how it reaches the network, writes
# /etc/decnet/decnetd.conf, installs decnetd, and runs it under systemd.
#
#     tools/install-decnetd.sh              ask, then install and start
#     tools/install-decnetd.sh --dry-run D  ask, then write the files to D
#                                           and show what would be done
#     tools/install-decnetd.sh --uninstall  stop and remove the service
#
# Run it as yourself; it uses sudo for the parts that need root.  Running
# it again replaces the configuration (the old one is kept as a .bak).

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
prefix=/usr/local
confdir=/etc/decnet
conf=$confdir/decnetd.conf
unit=/etc/systemd/system/decnetd.service
statedir=/var/lib/decnet
socket=/tmp/decnetapi.sock
action=install
dry=

while [ $# -gt 0 ]; do
    case $1 in
        --uninstall) action=uninstall ;;
        --dry-run)   dry=$2; shift ;;
        --prefix)    prefix=$2; shift ;;
        -h|--help)   sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           echo "install-decnetd.sh: unknown option $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

if [ -t 1 ]; then
    bold=$'\e[1m' red=$'\e[31m' green=$'\e[32m' plain=$'\e[0m'
else
    bold= red= green= plain=
fi
say ()     { printf '%s\n' "$*"; }
section () { printf '\n%s%s%s\n' "$bold" "$*" "$plain"; }
die ()     { printf '%sinstall-decnetd.sh: %s%s\n' "$red" "$*" "$plain" >&2; exit 1; }

# Who the node runs as by default, and who builds: the person running
# this, even under sudo.
me=${SUDO_USER:-$(id -un)}
if [ "$(id -u)" -eq 0 ]; then sudo=; else sudo=sudo; fi

# run CMD... -- as root, or just shown with --dry-run.
run ()
{
    if [ -n "$dry" ]; then
        printf '  + %s\n' "$*"
    else
        $sudo "$@"
    fi
}

# put FILE MODE -- stdin to FILE as root (with --dry-run, into $dry).
put ()
{
    if [ -n "$dry" ]; then
        cat > "$dry/$(basename "$1")"
        printf '  + write %s (in %s)\n' "$1" "$dry"
    else
        $sudo install -D -m "$2" /dev/stdin "$1"
    fi
}

# ---------------------------------------------------------------- questions

# ask VAR PROMPT DEFAULT [CHECK] -- read a value; CHECK is a function that
# says whether it will do, printing why not.
ask ()
{
    local var=$1 prompt=$2 def=$3 check=${4:-} reply
    while :; do
        if [ -n "$def" ]; then prompt_text="$prompt [$def]: "; else prompt_text="$prompt: "; fi
        read -r -p "$prompt_text" reply || die "no answer to \"$prompt\""
        reply=${reply:-$def}
        if [ -z "$check" ] || "$check" "$reply"; then
            printf -v "$var" '%s' "$reply"
            return
        fi
    done
}

yes_no ()   # PROMPT DEFAULT(y|n)
{
    local reply hint
    [ "$2" = y ] && hint="Y/n" || hint="y/N"
    read -r -p "$1 [$hint]: " reply || die "no answer to \"$1\""
    reply=${reply:-$2}
    case $reply in [Yy]*) return 0 ;; *) return 1 ;; esac
}

valid_name ()
{
    if [[ $1 =~ ^[A-Za-z0-9]{1,6}$ && $1 =~ [A-Za-z] ]]; then return 0; fi
    say "  A node name is one to six letters and digits, with at least one letter."
    return 1
}

valid_address ()
{
    if [[ $1 =~ ^([0-9]+)\.([0-9]+)$ ]] \
        && (( BASH_REMATCH[1] >= 1 && BASH_REMATCH[1] <= 63 \
              && BASH_REMATCH[2] >= 1 && BASH_REMATCH[2] <= 1023 )); then
        return 0
    fi
    say "  An address is area.node: area 1 to 63, node 1 to 1023, like 29.151."
    return 1
}

valid_address_or_none () { [ -z "$1" ] || [ "$1" = - ] || valid_address "$1"; }
valid_name_or_none ()    { [ -z "$1" ] || [ "$1" = - ] || valid_name "$1"; }

valid_port ()
{
    if [[ $1 =~ ^[0-9]+$ ]] && (( $1 >= 1 && $1 <= 65535 )); then return 0; fi
    say "  A port is a number from 1 to 65535."
    return 1
}

valid_host ()
{
    if [[ -n $1 && $1 =~ ^[A-Za-z0-9.:_-]+$ ]]; then return 0; fi
    say "  Give the peer's IP address or host name."
    return 1
}

valid_host_or_any () { [ -z "$1" ] || [ "$1" = any ] || valid_host "$1"; }

valid_iface ()
{
    if [ -e "/sys/class/net/$1" ]; then return 0; fi
    say "  There's no interface called $1 here.  These are: $(ls /sys/class/net | tr '\n' ' ')"
    return 1
}

valid_minutes ()
{
    if [[ $1 =~ ^[0-9]+$ ]] && (( $1 >= 1 )); then return 0; fi
    say "  A number of minutes, at least 1."
    return 1
}

valid_user ()
{
    if [ "$1" = decnet ] || id "$1" >/dev/null 2>&1; then return 0; fi
    say "  There's no user called $1."
    return 1
}

# ------------------------------------------------------------------ uninstall

if [ "$action" = uninstall ]; then
    section "Removing the decnetd service"
    if systemctl list-unit-files decnetd.service >/dev/null 2>&1; then
        run systemctl disable --now decnetd.service || true
    fi
    run rm -f "$unit"
    run systemctl daemon-reload
    run rm -f "$prefix/bin/decnetd" "$prefix/bin/dnfal" "$prefix/bin/dnping"
    say "Done.  The configuration is still in $confdir, and the decnet user (if"
    say "there was one) is still there; remove them yourself if you're sure."
    exit 0
fi

# ----------------------------------------------------------------- the node

command -v systemctl >/dev/null || die "this needs systemd, and there's no systemctl here"
[ -n "$dry" ] && mkdir -p "$dry"

section "This node"
say "  An endnode is right for a desktop: one circuit, to a router that looks"
say "  after the rest of the network.  A router joins circuits together and"
say "  carries other nodes' traffic."
say "    1) endnode"
say "    2) level 1 router (routes within its area)"
say "    3) level 2 router (an area router)"
pick_type () { case $1 in 1|endnode) return 0 ;; 2|l1router) return 0 ;; 3|l2router) return 0 ;;
               *) say "  1, 2 or 3."; return 1 ;; esac; }
ask type_choice "Node type" 1 pick_type
case $type_choice in 1|endnode) ntype=endnode ;; 2|l1router) ntype=l1router ;; *) ntype=l2router ;; esac

ask name "Node name" "" valid_name
name=${name^^}
say "  On HECnet, your area's coordinator gives you an address; elsewhere, pick"
say "  one nobody on your network is using."
ask address "Node address (area.node)" "" valid_address

# ------------------------------------------------------------------ circuits

circuits=()
peer_nodes=()
need_raw=no
need_low_port=no
n_mul=0
n_eth=0

have_pcap=unknown
if [ -x "$repo/build/release/bin/decnetd" ] || [ -f "$repo/Makefile" ]; then
    case $(make -s -C "$repo" features 2>/dev/null | awk '/libpcap/ {print $3}') in
        yes) have_pcap=yes ;; no) have_pcap=no ;;
    esac
fi

add_circuit ()
{
    section "Circuit $(( ${#circuits[@]} + 1 ))"
    say "    1) Multinet, connecting to a peer that listens (the usual for an endnode)"
    say "    2) Multinet, listening for a peer that connects to us"
    say "    3) Ethernet, straight onto a wired LAN (libpcap)"
    pick_kind () { case $1 in 1|2|3) return 0 ;; *) say "  1, 2 or 3."; return 1 ;; esac; }
    local kind host port iface t3 peer_addr peer_name dev
    ask kind "How does it reach the network" 1 pick_kind

    case $kind in
        1)
            ask host "The peer's IP address or host name" "" valid_host
            ask port "The port it listens on" 7100 valid_port
            dev="Multinet $host:$port:connect"
            ;;
        2)
            say "  Only the peer's address may connect.  Leave it empty (or say \"any\")"
            say "  to take a connection from anywhere."
            ask host "The peer's IP address or host name" "" valid_host_or_any
            [ "$host" = any ] && host=
            ask port "The port to listen on" 7100 valid_port
            (( port < 1024 )) && need_low_port=yes
            dev="Multinet $host:$port:listen"
            ;;
        3)
            if [ "$have_pcap" = no ]; then
                say "  ${red}This decnetd was built without libpcap, so it can't use an Ethernet"
                say "  circuit.  Install libpcap-dev, then 'make clean && make' in cppdecnet.${plain}"
                return 1
            fi
            say "  It must be a wired interface: Wi-Fi access points drop DECnet's frames."
            ask iface "Interface" "$(ls /sys/class/net | grep -v -e '^lo$' -e '^w' | head -1)" valid_iface
            need_raw=yes
            dev="Ethernet pcap:$iface"
            ;;
    esac

    if [ "$kind" = 3 ]; then
        t3=10
        circuits+=("circuit eth-$n_eth $dev --t3 $t3 --mop")
        n_eth=$(( n_eth + 1 ))
    else
        t3=15
        circuits+=("circuit mul-$n_mul $dev --t3 $t3")
        n_mul=$(( n_mul + 1 ))
        say "  Naming the node at the other end lets you use its name.  Leave these"
        say "  empty if you don't know them."
        ask peer_addr "Its DECnet address" "" valid_address_or_none
        ask peer_name "Its DECnet name" "" valid_name_or_none
        if [ -n "$peer_addr" ] && [ "$peer_addr" != - ] && [ -n "$peer_name" ] && [ "$peer_name" != - ]; then
            peer_nodes+=("node $peer_addr ${peer_name^^}")
        fi
    fi
    return 0
}

until add_circuit; do :; done
if [ "$ntype" != endnode ]; then
    while yes_no "Add another circuit?" n; do
        until add_circuit; do :; done
    done
fi

# ------------------------------------------------------------ disconnecting

on_demand=
section "Staying connected"
if [ "$ntype" = endnode ]; then
    say "  A desktop node can stay off the network until a PathNoWorks program"
    say "  needs it: the circuit comes up when the first one connects, and goes"
    say "  down once none has been connected for a while."
    if yes_no "Disconnect when nothing has used it for a while?" n; then
        ask minutes "After how many minutes" 120 valid_minutes
        on_demand=" --on-demand --idle $(( minutes * 60 ))"
    fi
else
    say "  A router stays connected: other nodes rely on it to carry their traffic."
fi

# ---------------------------------------------------------- the rest of it

section "Names, monitoring and access"
hecnet=no
if yes_no "Keep HECnet's list of node names, fetched weekly from MIM?" n; then
    hecnet=yes
fi

http=
if yes_no "Serve the monitoring web pages?" n; then
    ask http "Port" 8102 valid_port
fi

say "  decnetd needs no root.  Running it as you is simplest on a desktop: the"
say "  PathNoWorks tools you run can use it straight away.  Or give it a user of"
say "  its own, \"decnet\", and let a group share the socket."
ask runas "Run decnetd as" "$me" valid_user
add_me=no
if [ "$runas" = decnet ]; then
    api_mode=660
    if [ "$me" != root ] && yes_no "Let $me use it (adds $me to the decnet group)?" y; then
        add_me=yes
    fi
else
    api_mode=600
fi

# -------------------------------------------------------------- the files

timestamp=$(date '+%Y-%m-%d %H:%M')
config=$(
    printf '# decnetd.conf -- written by install-decnetd.sh, %s.\n' "$timestamp"
    printf '# Edit freely, then: sudo systemctl restart decnetd\n\n'
    printf 'routing %s --type %s\n\n' "$address" "$ntype"
    printf 'node %s %s\n' "$address" "$name"
    for l in "${peer_nodes[@]}"; do printf '%s\n' "$l"; done
    if [ "$hecnet" = yes ]; then
        printf 'node @hecnet --cache %s/hecnet.dat\n' "$statedir"
    fi
    printf '\n'
    for l in "${circuits[@]}"; do printf '%s\n' "$l"; done
    printf '\n# The socket the PathNoWorks tools look for.  Whoever can open it acts\n'
    printf '# as this node.\n'
    printf 'api %s --mode %s%s\n' "$socket" "$api_mode" "$on_demand"
    if [ -n "$http" ]; then
        printf '\n# Monitoring pages: on all interfaces, with no login.\n'
        printf 'http --http-port %s\n' "$http"
    fi
)

caps=()
[ "$need_raw" = yes ] && caps+=(CAP_NET_RAW CAP_NET_ADMIN)
[ "$need_low_port" = yes ] && caps+=(CAP_NET_BIND_SERVICE)

service=$(
    printf '[Unit]\n'
    printf 'Description=DECnet node %s (%s), cppdecnet\n' "$name" "$address"
    printf 'Documentation=https://github.com/RichardPar/cppdecnet\n'
    printf 'After=network-online.target\n'
    printf 'Wants=network-online.target\n\n'
    printf '[Service]\n'
    printf 'Type=simple\n'
    printf 'ExecStart=%s/bin/decnetd --log-level info %s\n' "$prefix" "$conf"
    printf 'User=%s\n' "$runas"
    printf 'Group=%s\n' "$( [ "$runas" = decnet ] && echo decnet || id -gn "$runas")"
    printf '# Its node name cache; the API socket goes in the real /tmp, where the\n'
    printf '# tools look, so no PrivateTmp.\n'
    printf 'StateDirectory=decnet\n'
    if [ ${#caps[@]} -gt 0 ]; then
        printf 'AmbientCapabilities=%s\n' "${caps[*]}"
        printf 'CapabilityBoundingSet=%s\n' "${caps[*]}"
    else
        printf 'CapabilityBoundingSet=\n'
    fi
    printf 'NoNewPrivileges=yes\n'
    printf 'ProtectSystem=full\n'
    printf 'Restart=on-failure\n'
    printf 'RestartSec=5\n\n'
    printf '[Install]\n'
    printf 'WantedBy=multi-user.target\n'
)

section "What will be installed"
say "$config" | sed 's/^/    /'
say ""
say "  as $conf, run by systemd ($unit) as $runas."
yes_no "Go ahead?" y || { say "Nothing done."; exit 0; }

# ------------------------------------------------------------- installing

section "Installing"

# Build, as the person running this rather than as root.
decnetd_bin=$repo/build/release/bin/decnetd
if [ ! -x "$decnetd_bin" ]; then
    say "  decnetd isn't built yet; building it."
    if [ "$(id -u)" -eq 0 ] && [ "$me" != root ]; then
        sudo -u "$me" make -C "$repo" -j "$(nproc)"
    else
        make -C "$repo" -j "$(nproc)"
    fi
fi
for b in decnetd dnfal dnping; do
    [ -x "$repo/build/release/bin/$b" ] || die "$b didn't build"
done

if [ -z "$dry" ]; then
    say "  sudo may ask for your password."
    $sudo true
fi

run install -d "$prefix/bin"
run install -m 755 "$repo/build/release/bin/decnetd" "$repo/build/release/bin/dnfal" \
    "$repo/build/release/bin/dnping" "$prefix/bin/"

if [ "$runas" = decnet ] && ! id decnet >/dev/null 2>&1; then
    run useradd --system --user-group --home-dir "$statedir" --no-create-home \
        --shell /usr/sbin/nologin --comment "DECnet node" decnet
fi
if [ "$add_me" = yes ]; then
    run usermod -aG decnet "$me"
fi

if [ -z "$dry" ] && [ -e "$conf" ]; then
    backup=$conf.$(date +%Y%m%d-%H%M%S).bak
    $sudo cp -p "$conf" "$backup"
    say "  The old configuration is kept as $backup."
fi
printf '%s\n' "$config" | put "$conf" 644
printf '%s\n' "$service" | put "$unit" 644
run systemctl daemon-reload

# One node per address: a decnetd already running by hand would be the
# same node twice on the network.
others=$(pgrep -a -x decnetd 2>/dev/null | grep -v -F "$prefix/bin/decnetd --log-level info $conf" || true)
start=yes
if [ -n "$others" ]; then
    say ""
    say "  ${red}decnetd is already running here, not as this service:${plain}"
    say "$others" | sed 's/^/    /'
    say "  Running both would put the node on the network twice."
    if yes_no "Stop it?" y; then
        for pid in $(printf '%s\n' "$others" | awk '{print $1}'); do run kill "$pid"; done
    else
        start=no
    fi
fi

if [ "$start" = yes ]; then
    run systemctl enable decnetd.service
    run systemctl restart decnetd.service
else
    run systemctl enable decnetd.service
    say "  Enabled for the next boot, not started.  Start it once the other one"
    say "  has stopped: sudo systemctl start decnetd"
fi

[ -n "$dry" ] && { section "Dry run: nothing changed.  The files are in $dry."; exit 0; }

# ---------------------------------------------------------------- checking

if [ "$start" = yes ]; then
    section "Checking"
    for _ in $(seq 20); do
        [ -S "$socket" ] && break
        sleep 0.5
    done
    if systemctl is-active --quiet decnetd.service && [ -S "$socket" ]; then
        printf '  %s✓%s decnetd is running as %s (%s), and its API is at %s\n' \
            "$green" "$plain" "$name" "$address" "$socket"
    else
        say "  ${red}decnetd didn't start properly.  The log says:${plain}"
        $sudo journalctl -u decnetd.service -n 20 --no-pager | sed 's/^/    /'
        exit 1
    fi
fi

section "Done"
say "  Configuration:  $conf"
say "  Service:        sudo systemctl {status|restart|stop} decnetd"
say "  Log:            journalctl -u decnetd -f"
[ "$add_me" = yes ] && say "  Log out and in again so the decnet group applies to $me."
say "  Remove it with: $0 --uninstall"
