#include "decnet/datalink/ethernet.h"

#if DN_HAVE_PCAP
#include <pcap.h>
#endif

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <charconv>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#endif

namespace decnet::datalink {

namespace {

bool to_port (std::string_view s, std::uint16_t &out)
{
    unsigned v = 0;
    auto [p, ec] = std::from_chars (s.data (), s.data () + s.size (), v);
    if (ec != std::errc () || p != s.data () + s.size () || v > 65535)
        return false;
    out = static_cast<std::uint16_t> (v);
    return true;
}

std::vector<std::string> split (const std::string &s, char sep)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) { parts.push_back (cur); cur.clear (); }
        else            cur += c;
    }
    parts.push_back (cur);
    return parts;
}

}   // namespace

// -------------------------------------------------------- device parsing

EthernetDevice EthernetDevice::parse (const std::string &device)
{
    std::vector<std::string> parts = split (device, ':');
    if (parts.size () < 2)
        throw std::invalid_argument ("invalid Ethernet device: " + device);

    EthernetDevice d;
    const std::string &api = parts[0];

    if (api == "udp" || api == "bridge") {
        // udp:<localport>:<host>:<remoteport>.  SIMH calls this "udp",
        // pydecnet also accepts "bridge"; they mean the same thing.
        if (parts.size () != 4)
            throw std::invalid_argument ("Ethernet " + api
                                         + " needs localport:host:remoteport in "
                                         + device);
        d.mode = Mode::udp;
        if (!to_port (parts[1], d.source_port)
            || !to_port (parts[3], d.dest_port))
            throw std::invalid_argument ("invalid port in Ethernet device: "
                                         + device);
        d.destination = parts[2];
        if (d.destination.empty ())
            throw std::invalid_argument ("Ethernet " + api
                                         + " needs a peer host in " + device);
        return d;
    }
    if (api == "tap")  { d.mode = Mode::tap;  d.destination = parts[1]; return d; }
    if (api == "pcap") { d.mode = Mode::pcap; d.destination = parts[1]; return d; }

    throw std::invalid_argument ("unknown Ethernet circuit type " + api);
}

std::string EthernetDevice::str () const
{
    switch (mode) {
    case Mode::udp:
        return "udp local :" + std::to_string (source_port) + " peer "
             + destination + ":" + std::to_string (dest_port);
    case Mode::tap:  return "tap " + destination;
    case Mode::pcap: return "pcap " + destination;
    }
    return {};
}

// ------------------------------------------------------------- Ethernet

Ethernet::Ethernet (Element *owner, std::string name, EthernetDevice dev,
                    bool random_address)
    : BcDatalink (owner, std::move (name), random_address),
      dev_ (std::move (dev))
{
    DN_DEBUG ("Ethernet datalink {} initialized: {}, address {}", name_,
              dev_.str (), hwaddr ().str ());
}

Ethernet::~Ethernet ()
{
    // A backstop only: by now the derived object is gone, so this can do
    // nothing but make sure no thread is left running.  Each concrete
    // class calls shutdown() from its own destructor, which is where the
    // work actually happens.
    stopnow_.store (true);
    if (thread_.joinable ()) thread_.join ();
}

void Ethernet::shutdown ()
{
    close ();
}

void Ethernet::open ()
{
    if (thread_.joinable ()) return;
    if (!start_transport ()) {
        DN_ERROR ("cannot open Ethernet circuit {}", name_);
        return;
    }
    stopnow_.store (false);
    thread_ = std::thread ([this] { run (); });
}

void Ethernet::close ()
{
    stopnow_.store (true);
    if (thread_.joinable ()) thread_.join ();
    stop_transport ();
}

void Ethernet::run ()
{
    // Name the thread node.circuit, as pydecnet does, so two nodes in one
    // process can be told apart in the log.
    logging::set_thread_name ((node () ? node ()->name () + "." : "") + name_);
    DN_TRACE ("Ethernet receive thread started for {}", name_);
    try {
        receive_loop ();
    } catch (const std::exception &e) {
        DN_ERROR ("exception in Ethernet receive thread for {}: {}",
                  name_, e.what ());
    }
    DN_TRACE ("Ethernet receive thread for {} exiting", name_);
}

std::unique_ptr<Datalink> Ethernet::create (Element *owner,
                                            const std::string &name,
                                            const std::string &device,
                                            bool random_address)
{
    EthernetDevice dev = EthernetDevice::parse (device);
    switch (dev.mode) {
    case EthernetDevice::Mode::udp:
        return std::make_unique<BridgeEthernet> (owner, name, dev,
                                                 random_address);
    case EthernetDevice::Mode::tap:
        return std::make_unique<TapEthernet> (owner, name, dev,
                                              random_address);
    case EthernetDevice::Mode::pcap:
#if DN_HAVE_PCAP
        return std::make_unique<PcapEthernet> (owner, name, dev,
                                               random_address);
#else
        DN_ERROR ("pcap Ethernet circuits need libpcap, which was not found "
                  "when this was built ({}); see make features", name);
        return nullptr;
#endif
    }
    return nullptr;
}

// ---------------------------------------------------------- PcapEthernet

#if DN_HAVE_PCAP

Macaddr PcapEthernet::interface_address (const std::string &ifname)
{
#ifdef __linux__
    int fd = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return { };
    ifreq ifr {};
    std::strncpy (ifr.ifr_name, ifname.c_str (), IFNAMSIZ - 1);
    Macaddr result;
    if (::ioctl (fd, SIOCGIFHWADDR, &ifr) == 0) {
        std::array<std::uint8_t, 6> a {};
        std::memcpy (a.data (), ifr.ifr_hwaddr.sa_data, 6);
        result = Macaddr (a);
    }
    ::close (fd);
    return result;
#else
    (void) ifname;
    return { };
#endif
}

PcapEthernet::PcapEthernet (Element *owner, std::string name,
                            EthernetDevice dev, bool random_address)
    : Ethernet (owner, std::move (name), dev, random_address)
{
    // Take the interface's own address as the circuit's, unless a random
    // one was asked for.  Ports created after this inherit it, which is
    // what makes MOP report the real hardware address of the card rather
    // than zeroes.  Routing overrides its own port with the DECnet derived
    // address, as it must.
    if (!random_address) {
        Macaddr a = interface_address (dev_.destination);
        if (a != Macaddr ()) {
            set_hwaddr (a);
            DN_DEBUG ("Ethernet {} using hardware address {} of {}", name_,
                      a.str (), dev_.destination);
        } else {
            DN_DEBUG ("Ethernet {}: no hardware address for {}", name_,
                      dev_.destination);
        }
    }
}

PcapEthernet::~PcapEthernet () { shutdown (); }

bool PcapEthernet::start_transport ()
{
    char err[PCAP_ERRBUF_SIZE] = "";
    // Promiscuous: the addresses DECnet answers to are not the ones the
    // card was configured with, so anything less would filter out exactly
    // the traffic we came for.
    pcap_t *p = pcap_open_live (dev_.destination.c_str (),
                                static_cast<int> (ETH_MTU + 64), 1,
                                poll_timeout_ms, err);
    if (!p) {
        DN_ERROR ("cannot open {} for {}: {}", dev_.destination, name_, err);
        return false;
    }
    if (err[0]) DN_DEBUG ("opening {}: {}", dev_.destination, err);

    if (pcap_datalink (p) != DLT_EN10MB) {
        DN_ERROR ("{} is not an Ethernet interface ({})", dev_.destination,
                  pcap_datalink_val_to_name (pcap_datalink (p)));
        pcap_close (p);
        return false;
    }
    // Inbound only.  Without this every frame we transmit is captured on
    // the way out and handed back to us as though a neighbour had sent it.
    if (pcap_setdirection (p, PCAP_D_IN) < 0)
        DN_DEBUG ("{}: cannot restrict capture to inbound frames: {}",
                  dev_.destination, pcap_geterr (p));

    {
        std::lock_guard lock (pcap_mutex_);
        pcap_ = p;
    }
    install_filter ();
    DN_DEBUG ("Ethernet {} capturing on {}", name_, dev_.destination);
    return true;
}

void PcapEthernet::stop_transport ()
{
    std::lock_guard lock (pcap_mutex_);
    if (!pcap_) return;
    pcap_close (static_cast<pcap_t *> (pcap_));
    pcap_ = nullptr;
}

void PcapEthernet::filter_changed ()
{
    install_filter ();
}

void PcapEthernet::install_filter ()
{
    std::string expr = filter_expression ();
    std::lock_guard lock (pcap_mutex_);
    if (!pcap_ || expr.empty ()) return;

    pcap_t *p = static_cast<pcap_t *> (pcap_);
    bpf_program prog {};
    if (pcap_compile (p, &prog, expr.c_str (), 1, PCAP_NETMASK_UNKNOWN) < 0) {
        // Not fatal: receive_frame checks every frame anyway, so a filter
        // we could not compile costs efficiency, not correctness.
        DN_DEBUG ("{}: cannot compile filter \"{}\": {}", name_, expr,
                  pcap_geterr (p));
        return;
    }
    if (pcap_setfilter (p, &prog) < 0)
        DN_DEBUG ("{}: cannot install filter: {}", name_, pcap_geterr (p));
    else
        DN_TRACE ("{} filter: {}", name_, expr);
    pcap_freecode (&prog);
}

void PcapEthernet::receive_loop ()
{
    pcap_t *p;
    {
        std::lock_guard lock (pcap_mutex_);
        p = static_cast<pcap_t *> (pcap_);
    }
    if (!p) return;

    // pcap's own read timeout is not a promise on every platform, so where
    // the handle can be polled we poll it and only call into pcap when
    // there is something there.  That is what makes stopping prompt.
    int fd = pcap_get_selectable_fd (p);

    auto handler = [] (u_char *user, const pcap_pkthdr *h, const u_char *bytes) {
        auto *self = reinterpret_cast<PcapEthernet *> (user);
        if (h->caplen < ETH_HDR_LEN) return;
        self->receive_frame (ByteView (bytes, h->caplen));
    };

    for (;;) {
        if (stopping ()) return;
        if (fd >= 0) {
            PollResult r = poll_socket (fd, true, false, poll_timeout_ms);
            if (stopping ()) return;
            if (r.error) return;
            if (r.timeout || !r.readable) continue;
        }
        int n = pcap_dispatch (p, -1, handler,
                               reinterpret_cast<u_char *> (this));
        if (n == -1) {
            DN_ERROR ("capture error on {}: {}", name_, pcap_geterr (p));
            return;
        }
        if (n == -2) return;            // pcap_breakloop
    }
}

void PcapEthernet::send_frame (const Bytes &frame)
{
    std::lock_guard lock (pcap_mutex_);
    if (!pcap_) return;
    // Errors are ignored, which is the DECnet way: a datalink that cannot
    // send a frame has lost a frame, and the layers above already cope
    // with that.
    (void) pcap_inject (static_cast<pcap_t *> (pcap_), frame.data (),
                        frame.size ());
}

#endif  // DN_HAVE_PCAP

// -------------------------------------------------------- BridgeEthernet

BridgeEthernet::BridgeEthernet (Element *owner, std::string name,
                                EthernetDevice dev, bool random_address)
    : Ethernet (owner, std::move (name), dev, random_address),
      source_ ("", dev.source_port),
      dest_ (dev.destination, dev.dest_port)
{
}

bool BridgeEthernet::start_transport ()
{
    socket_ = create_udp (dest_, source_);
    if (!socket_) {
        DN_TRACE ("Ethernet {} could not bind to :{}", name_,
                  dev_.source_port);
        return false;
    }
    DN_DEBUG ("Ethernet {} bound to :{}, peer {}", name_, dev_.source_port,
              dest_.str ());
    return true;
}

void BridgeEthernet::stop_transport () { socket_.close (); }

void BridgeEthernet::receive_loop ()
{
    std::uint8_t buf[ETH_MTU + 64];
    for (;;) {
        PollResult p = poll_socket (socket_.fd (), true, false,
                                    poll_timeout_ms);
        if (stopping ()) return;
        if (p.error) {
            // A datagram socket can report a transient error -- an ICMP
            // unreachable from a peer that has not started yet, most
            // often.  That is a dropped packet, not a dead circuit.
            DN_TRACE ("transient error on {}, continuing", name_);
            continue;
        }
        if (p.timeout || !p.readable) continue;

        Endpoint from;
        ssize_t n = recv_datagram (socket_.fd (), buf, sizeof buf, from);
        if (n < 0) continue;
        // Only listen to the peer we were configured to talk to.
        if (!dest_.valid (from)) continue;
        // Anything shorter than a header cannot be a frame.
        if (n <= static_cast<ssize_t> (ETH_HDR_LEN)) continue;
        // A source routed frame is not something DECnet uses; drop it, as
        // pydecnet does with the same check on the source address.
        if (buf[6] & 1) continue;
        receive_frame (ByteView (buf, static_cast<std::size_t> (n)));
    }
}

void BridgeEthernet::send_frame (const Bytes &frame)
{
    // "Ignore any errors, because that's the DECnet way": a LAN drops
    // frames, and a datalink that reported every loss upward would be
    // lying about what the medium guarantees.
    if (!socket_) return;
    const Endpoint *to = dest_.destination ();
    if (!to) return;
    (void) send_datagram (socket_.fd (),
                          ByteView (frame.data (), frame.size ()), *to);
}

// ----------------------------------------------------------- TapEthernet

bool TapEthernet::start_transport ()
{
#ifdef __linux__
    int fd = ::open ("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        DN_ERROR ("cannot open /dev/net/tun for {}: {}", name_,
                  std::strerror (errno));
        return false;
    }
    ifreq ifr {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    // The device string is the interface name, with or without a path.
    std::string dev = dev_.destination;
    if (auto slash = dev.rfind ('/'); slash != std::string::npos)
        dev = dev.substr (slash + 1);
    std::strncpy (ifr.ifr_name, dev.c_str (), IFNAMSIZ - 1);
    if (::ioctl (fd, TUNSETIFF, &ifr) < 0) {
        DN_ERROR ("cannot attach to tap device {}: {}", dev,
                  std::strerror (errno));
        ::close (fd);
        return false;
    }
    socket_ = Socket (fd);
    DN_DEBUG ("Ethernet {} attached to tap device {}", name_, ifr.ifr_name);
    return true;
#else
    DN_ERROR ("tap circuits are only implemented for Linux ({})", name_);
    return false;
#endif
}

void TapEthernet::stop_transport () { socket_.close (); }

void TapEthernet::receive_loop ()
{
    std::uint8_t buf[ETH_MTU + 64];
    for (;;) {
        PollResult p = poll_socket (socket_.fd (), true, false,
                                    poll_timeout_ms);
        if (stopping ()) return;
        if (p.error) return;
        if (p.timeout || !p.readable) continue;

        ssize_t n = ::read (socket_.fd (), buf, sizeof buf);
        if (n <= static_cast<ssize_t> (ETH_HDR_LEN)) continue;
        receive_frame (ByteView (buf, static_cast<std::size_t> (n)));
    }
}

void TapEthernet::send_frame (const Bytes &frame)
{
    if (!socket_) return;
    // A datalink that cannot send a frame has lost a frame, which the
    // layers above already cope with.  The result is read rather than cast
    // away because write() is declared warn_unused_result and a (void)
    // cast does not silence that.
    ssize_t n = ::write (socket_.fd (), frame.data (), frame.size ());
    if (n < 0)
        DN_TRACE ("send failed on {}: {}", name_, std::strerror (errno));
}

}   // namespace decnet::datalink
