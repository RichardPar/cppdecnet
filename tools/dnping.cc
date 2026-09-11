// tools/dnping.cc -- loop test a node, the way NCP's LOOP NODE does.
//
// Port of applications/dnping.  The far end needs nothing running for it:
// MIRROR is object 25, every DECnet node has one, and looping through it
// is how you find out whether the whole stack reaches a node rather than
// just whether routing thinks it does.
//
// This is the client half.  The server half -- answering someone else's
// LOOP NODE -- is in the NICE listener, src/nice/nml.cc.
//
// It needs a node of its own to speak from, because DECnet has no
// connectionless way to ask this: a loop test is a logical link to an
// object, so there has to be a node to open it from.  That is what the
// configuration file argument is for.  Once the API server exists this
// could instead talk to a running daemon; see TASKS.md.

#include "decnet/common/logging.h"
#include "decnet/common/types.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/l1router.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"
#include "decnet/version.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace decnet;
using namespace decnet::session;

namespace {

using Clock = std::chrono::steady_clock;

// What came back, and when.
struct Reply {
    Bytes data;
    double ms = 0;
};

class Pinger : public Application {
public:
    void connect_received (SessionConnection &, ByteView) override {}

    void data_received (SessionConnection &, ByteView data) override
    {
        std::lock_guard l (m_);
        double ms = std::chrono::duration<double, std::milli> (
            Clock::now () - sent_at_).count ();
        replies_.push_back (Reply { Bytes (data.begin (), data.end ()), ms });
    }

    void disconnected (SessionConnection &, unsigned reason) override
    {
        std::lock_guard l (m_);
        gone_ = true;
        reason_ = reason;
    }

    // The connect was confirmed when the far end's accept data arrives, and
    // MIRROR's accept data is the largest message it will take.
    void accepted (ByteView data)
    {
        std::lock_guard l (m_);
        accepted_ = true;
        maxlen_ = data.size () >= 2
            ? static_cast<unsigned> (data[0] | (data[1] << 8)) : 0;
    }

    void mark_sent () { std::lock_guard l (m_); sent_at_ = Clock::now (); }

    bool accepted () { std::lock_guard l (m_); return accepted_; }
    bool gone () { std::lock_guard l (m_); return gone_; }
    unsigned reason () { std::lock_guard l (m_); return reason_; }
    unsigned maxlen () { std::lock_guard l (m_); return maxlen_; }
    std::size_t count () { std::lock_guard l (m_); return replies_.size (); }
    Reply at (std::size_t i) { std::lock_guard l (m_); return replies_.at (i); }

private:
    std::mutex        m_;
    std::vector<Reply> replies_;
    Clock::time_point sent_at_ = Clock::now ();
    bool              accepted_ = false, gone_ = false;
    unsigned          reason_ = 0, maxlen_ = 0;
};

template <typename P>
bool wait_until (P pred, std::chrono::milliseconds timeout)
{
    auto deadline = Clock::now () + timeout;
    while (Clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    return pred ();
}

void usage (const char *me)
{
    std::fprintf (stderr,
        "usage: %s -c CONFIG NODE [count] [length]\n"
        "\n"
        "  Loop test NODE through its MIRROR object, as NCP's LOOP NODE does.\n"
        "  NODE is an address or a name from the configuration file.\n"
        "\n"
        "  -c CONFIG   the configuration file to run a node from (required)\n"
        "  count       messages to send, default 4\n"
        "  length      bytes in each, default 40\n", me);
}

}   // namespace

int main (int argc, char **argv)
{
    std::string cfgfile, target;
    int count = 4;
    unsigned length = 40;

    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc)      cfgfile = argv[++i];
        else if (a == "-h" || a == "--help") { usage (argv[0]); return 0; }
        else                                 pos.push_back (a);
    }
    if (cfgfile.empty () || pos.empty ()) { usage (argv[0]); return 2; }

    target = pos[0];
    if (pos.size () > 1) count = std::atoi (pos[1].c_str ());
    if (pos.size () > 2) length = static_cast<unsigned> (std::atoi (pos[2].c_str ()));
    if (count < 1) count = 1;
    if (length < 1) length = 1;

    logging::set_level (logging::Level::warning);

    try {
        Config cfg = Config::from_file (cfgfile);
        Node node (cfg);

        // A name is resolved against the configuration, as the Python tool
        // does: "dnping BAJI" has to work, not only "dnping 29.159".
        Nodeid dest;
        if (Nodeinfo *info = node.find_node (target)) {
            dest = info->id;
        } else {
            try {
                dest = Nodeid::parse (target);
            } catch (const std::exception &) {
                std::fprintf (stderr, "dnping: no node called %s, and it is "
                              "not an address either\n", target.c_str ());
                return 2;
            }
        }

        node.start ();

        // Wait for the destination to be reachable, not merely for a
        // circuit to exist.
        //
        // An adjacency is not a route.  Waiting only for one had this tool
        // connecting while the routing table still said the destination
        // was unreachable, so the connect was dropped before it reached
        // the wire and the only symptom was an NSP timeout thirty seconds
        // later reported as "did not accept".  The far end had never heard
        // anything at all.
        auto reachable = [&] {
            if (auto *l1 = dynamic_cast<routing::L1Router *> (node.routing ()))
                return l1->reachable (dest.value () & 1023u);
            // An endnode has no table: having an adjacency is as much as
            // it can know, since everything goes to its router.
            return node.routing () && node.routing ()->adjacency_count () > 0;
        };
        if (!wait_until (reachable, std::chrono::seconds (30))) {
            std::fprintf (stderr, "dnping: %s did not become reachable\n",
                          dest.str ().c_str ());
            node.stop ();
            return 1;
        }

        auto app = std::make_unique<Pinger> ();
        Pinger *p = app.get ();
        SessionConnection *c = node.session ()->connect (
            dest, EndUser::number (25), EndUser::named ("DNPING"), { },
            std::move (app));
        if (!c) {
            std::fprintf (stderr, "dnping: cannot reach %s\n", dest.str ().c_str ());
            node.stop ();
            return 1;
        }

        if (!wait_until ([&] { return c->running () || p->gone (); },
                         std::chrono::seconds (30)) || p->gone ()) {
            std::fprintf (stderr, "dnping: %s did not accept (reason %u)\n",
                          dest.str ().c_str (), p->reason ());
            node.stop ();
            return 1;
        }
        p->accepted (ByteView ());

        std::printf ("PING %s: %u byte messages through MIRROR\n",
                     dest.str ().c_str (), length);

        // The payload is the usual alternating pattern, so a bit that
        // sticks either way shows up.
        Bytes pattern;
        for (unsigned i = 0; i < length; ++i)
            pattern.push_back (static_cast<std::uint8_t> ((i & 1) ? 0x55 : 0xaa));

        int good = 0;
        double total = 0, best = 0, worst = 0;
        for (int i = 0; i < count; ++i) {
            std::size_t before = p->count ();
            Bytes msg { 0x00 };             // MIRROR: "loop this back"
            msg.insert (msg.end (), pattern.begin (), pattern.end ());

            p->mark_sent ();
            c->send_data (std::move (msg));

            if (!wait_until ([&] { return p->count () > before || p->gone (); },
                             std::chrono::seconds (5)) || p->gone ()) {
                std::printf ("  %d: no reply\n", i + 1);
                if (p->gone ()) break;
                continue;
            }

            Reply r = p->at (before);
            // The reply is the success status byte and then the data back.
            bool same = r.data.size () == pattern.size () + 1
                     && r.data[0] == 0x01
                     && std::memcmp (r.data.data () + 1, pattern.data (),
                                     pattern.size ()) == 0;
            if (same) {
                ++good;
                total += r.ms;
                if (!best || r.ms < best) best = r.ms;
                if (r.ms > worst) worst = r.ms;
                std::printf ("  %d: %u bytes looped in %.1f ms\n", i + 1,
                             length, r.ms);
            } else {
                std::printf ("  %d: reply did not match what was sent\n", i + 1);
            }
            if (i + 1 < count) std::this_thread::sleep_for (
                std::chrono::milliseconds (500));
        }

        std::printf ("\n%d sent, %d looped, %d lost\n", count, good,
                     count - good);
        if (good)
            std::printf ("round trip min %.1f / avg %.1f / max %.1f ms\n",
                         best, total / good, worst);

        c->disconnect ();
        node.stop ();
        return good ? 0 : 1;
    } catch (const std::exception &e) {
        std::fprintf (stderr, "dnping: %s\n", e.what ());
        return 1;
    }
}
