// Recovering from a neighbour that stops answering.
//
// This is the case a dead or wedged node presents: the TCP connection is
// still established, so nothing at the socket layer reports anything, and
// the only thing that can notice is the routing layer's listen timer.  The
// question these tests ask is whether the circuit comes back by itself once
// the neighbour starts answering again.
//
// The silence is produced by a relay in the middle that holds both
// connections open and discards everything it is given.

#include "harness.h"

#include "decnet/common/socket.h"
#include "decnet/config.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"
#include "decnet/datalink/datalink.h"
#include "decnet/datalink/ptp.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace decnet;
using namespace decnet::nice;
using namespace decnet::session;

namespace {

std::uint16_t free_port ()
{
    SourceAddress any ("127.0.0.1", 0);
    Socket s = any.create_server ();
    if (!s) throw std::runtime_error ("cannot find a free port");
    sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    ::getsockname (s.fd (), reinterpret_cast<sockaddr *> (&sa), &len);
    return ntohs (reinterpret_cast<sockaddr_in *> (&sa)->sin_port);
}

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout
                     = std::chrono::seconds (20))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return pred ();
}

// A TCP relay that can be told to go deaf: it keeps both connections open
// and throws away whatever passes through.
class Relay {
public:
    Relay (std::uint16_t listen_port, std::uint16_t dest_port)
        : lport_ (listen_port), dport_ (dest_port)
    {
        th_ = std::thread ([this] { run (); });
    }
    ~Relay ()
    {
        stop_.store (true);
        if (th_.joinable ()) th_.join ();
    }

    void deaf (bool on) { deaf_.store (on); }

private:
    static int connect_to (std::uint16_t port)
    {
        int fd = ::socket (AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons (port);
        sa.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        if (::connect (fd, reinterpret_cast<sockaddr *> (&sa), sizeof sa) < 0) {
            ::close (fd);
            return -1;
        }
        return fd;
    }

    void run ()
    {
        int lfd = ::socket (AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt (lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons (lport_);
        sa.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        if (::bind (lfd, reinterpret_cast<sockaddr *> (&sa), sizeof sa) < 0
            || ::listen (lfd, 4) < 0) {
            std::printf ("  relay: cannot listen on %u\n", lport_);
            ::close (lfd);
            return;
        }

        while (!stop_.load ()) {
            pollfd p { lfd, POLLIN, 0 };
            if (::poll (&p, 1, 100) <= 0) continue;
            int cfd = ::accept (lfd, nullptr, nullptr);
            if (cfd < 0) continue;
            int sfd = connect_to (dport_);
            if (sfd < 0) { ::close (cfd); continue; }
            pump (cfd, sfd);
            ::close (cfd);
            ::close (sfd);
        }
        ::close (lfd);
    }

    void pump (int a, int b)
    {
        std::uint8_t buf[8192];
        while (!stop_.load ()) {
            pollfd p[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
            int n = ::poll (p, 2, 100);
            if (n < 0) return;
            if (n == 0) continue;
            for (int i = 0; i < 2; ++i) {
                if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                int from = i == 0 ? a : b;
                int to   = i == 0 ? b : a;
                ssize_t got = ::recv (from, buf, sizeof buf, 0);
                if (got <= 0) return;           // closed: drop both ends
                if (deaf_.load ()) continue;    // swallow it
                ssize_t off = 0;
                while (off < got) {
                    ssize_t put = ::send (to, buf + off, got - off,
                                          MSG_NOSIGNAL);
                    if (put <= 0) return;
                    off += put;
                }
            }
        }
    }

    std::uint16_t     lport_, dport_;
    std::atomic<bool> deaf_ { false }, stop_ { false };
    std::thread       th_;
};

class Ncp : public Application {
public:
    void connect_received (SessionConnection &, ByteView) override
    { std::lock_guard l (m_); ++accepts_; }
    void data_received (SessionConnection &, ByteView data) override
    { std::lock_guard l (m_); replies_.push_back (Bytes (data.begin (), data.end ())); }
    void disconnected (SessionConnection &, unsigned reason) override
    { std::lock_guard l (m_); ++disconnects_; reason_ = reason; }

    int accepts () { std::lock_guard l (m_); return accepts_; }
    std::size_t count () { std::lock_guard l (m_); return replies_.size (); }
    Bytes at (std::size_t i) { std::lock_guard l (m_); return replies_.at (i); }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }
    unsigned reason () { std::lock_guard l (m_); return reason_; }

private:
    std::mutex m_;
    int accepts_ = 0, disconnects_ = 0;
    unsigned reason_ = 0;
    std::vector<Bytes> replies_;
};

NiceRequest read_request ()
{
    NiceRequest r;
    r.function = fn_read;
    r.info = info_char;
    r.entity_type = Entity::node;
    r.entity = ReqEntity::make_node (Nodeid ());
    return r;
}

// Node A listens on aport; node B connects to relayport, which the relay
// forwards to aport.
struct Pair {
    std::uint16_t aport = free_port ();
    std::uint16_t rport = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Relay> relay;

    Pair ()
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "system --ident \"test executor\"\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (aport)
              + ":listen --t3 2\n")),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (rport)
              + ":connect --t3 2\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
        relay = std::make_unique<Relay> (rport, aport);
    }

    bool up () const
    {
        return a->routing ()->adjacency_count () == 1
            && b->routing ()->adjacency_count () == 1;
    }

    void start ()
    {
        a->start ();
        b->start ();
        DN_ASSERT (wait_until ([&] { return up (); }));
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    // Drop the connection under the circuit, as a network blip does.
    void bounce ()
    {
        auto *dl = dynamic_cast<datalink::PtpDatalink *>
                       (b->datalink ()->circuit ("mul-0"));
        DN_ASSERT (dl != nullptr);
        dl->reconnect (true);
    }

    // One NICE conversation over the circuit, as NCP would run it.
    bool exchange (const char *what)
    {
        auto client = std::make_unique<Ncp> ();
        Ncp *ncp = client.get ();
        ConnectData cd;
        cd.dstname = EndUser::number (19);
        cd.srcname = EndUser::named ("NCP");
        cd.connectdata = Bytes { 4, 0, 0 };
        SessionConnection *c =
            b->session ()->connect (Nodeid::parse ("1.1"), std::move (cd),
                                    std::move (client));
        if (!c) { std::printf ("  %s: connect() returned null\n", what); return false; }
        if (!wait_until ([&] { return ncp->accepts () || ncp->disconnects (); },
                         std::chrono::seconds (30))) {
            std::printf ("  %s: no answer to the connect\n", what);
            return false;
        }
        if (ncp->disconnects ()) {
            std::printf ("  %s: connect refused, reason %u\n", what,
                         ncp->reason ());
            return false;
        }
        c->send_data (read_request ().encode ());
        if (!wait_until ([&] { return ncp->count () >= 1; },
                         std::chrono::seconds (30))) {
            std::printf ("  %s: no reply to the request\n", what);
            return false;
        }
        NiceReply r = NiceReply::parse (ncp->at (0), Entity::node);
        c->disconnect ();
        if (r.retcode != rc_success) {
            std::printf ("  %s: retcode %d\n", what, r.retcode);
            return false;
        }
        return true;
    }
};

}   // namespace

// The neighbour goes quiet with its socket still up, then starts answering
// again.  The circuit must notice the silence, and must come back.
DN_TEST (recovery, circuit_recovers_after_neighbour_goes_quiet)
{
    Pair p;
    p.start ();
    DN_ASSERT (p.exchange ("before the silence"));

    p.relay->deaf (true);
    // t3 is 2s, so the listen timeout is 6s.  Both ends should notice.
    bool noticed = wait_until ([&] {
        return p.a->routing ()->adjacency_count () == 0
            && p.b->routing ()->adjacency_count () == 0;
    }, std::chrono::seconds (30));
    std::printf ("  silence noticed: %s (A adj %zu, B adj %zu)\n",
                 noticed ? "yes" : "NO",
                 p.a->routing ()->adjacency_count (),
                 p.b->routing ()->adjacency_count ());
    DN_ASSERT (noticed);

    p.relay->deaf (false);
    bool back = wait_until ([&] { return p.up (); }, std::chrono::seconds (60));
    std::printf ("  came back: %s (A adj %zu, B adj %zu)\n",
                 back ? "yes" : "NO",
                 p.a->routing ()->adjacency_count (),
                 p.b->routing ()->adjacency_count ());
    DN_ASSERT (back);

    DN_ASSERT (p.exchange ("after the silence"));
    p.stop ();
}

// The same, twice over, because a state machine that recovers once can
// still be left somewhere it cannot recover from again.
DN_TEST (recovery, recovers_from_repeated_silence)
{
    Pair p;
    p.start ();
    DN_ASSERT (p.exchange ("first"));

    for (int i = 1; i <= 2; ++i) {
        p.relay->deaf (true);
        DN_ASSERT (wait_until ([&] {
            return p.b->routing ()->adjacency_count () == 0;
        }, std::chrono::seconds (30)));
        p.relay->deaf (false);
        bool back = wait_until ([&] { return p.up (); },
                                std::chrono::seconds (60));
        std::printf ("  round %d: back %s\n", i, back ? "yes" : "NO");
        DN_ASSERT (back);
        char what[32];
        std::snprintf (what, sizeof what, "round %d", i);
        DN_ASSERT (p.exchange (what));
    }
    p.stop ();
}

// The other way a neighbour disappears: the connection itself drops and is
// remade.  This path always worked -- the datalink notices the socket and
// reconnects on its own -- and the test is here so that it stays that way
// alongside the one above, which did not.
DN_TEST (recovery, nice_works_after_the_connection_drops)
{
    Pair p;
    p.start ();
    DN_ASSERT (p.exchange ("before the drop"));

    p.bounce ();
    DN_ASSERT (wait_until ([&] {
        return p.b->routing ()->adjacency_count () == 0;
    }));
    DN_ASSERT (wait_until ([&] { return p.up (); },
                           std::chrono::seconds (60)));

    DN_ASSERT (p.exchange ("after the drop"));
    p.stop ();
}
