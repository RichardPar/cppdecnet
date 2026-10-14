// The CHOST gate: object 42 as a CTERM host for local programs, driven by
// a scripted CTERM terminal end on a second node.

#include "harness.h"
#include "posix_compat.h"

#include "decnet/common/socket.h"
#include "decnet/common/work.h"
#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/cterm.h"
#include "decnet/session/session.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#include <sys/stat.h>
#endif

using namespace decnet;
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
                     = std::chrono::seconds (15))
{
    auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (pred ()) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return pred ();
}

std::uint16_t get16 (ByteView b, std::size_t at)
{
    return static_cast<std::uint16_t> (b[at] | (b[at + 1] << 8));
}

Bytes common_data (const Bytes &m)
{
    Bytes f { 9, 0, static_cast<std::uint8_t> (m.size ()),
              static_cast<std::uint8_t> (m.size () >> 8) };
    f.insert (f.end (), m.begin (), m.end ());
    return f;
}

// The terminal end, as much of it as the gate needs: it binds, initiates,
// says how big it is, collects what is written, and types when asked.
class Terminal : public Application {
public:
    void connect_received (SessionConnection &, ByteView) override
    {
        std::lock_guard l (m_);
        accepted_ = true;
    }

    void data_received (SessionConnection &c, ByteView msg) override
    {
        std::lock_guard l (m_);
        conn_ = &c;
        if (msg.empty ()) return;
        if (msg[0] == 1) {                              // bind request
            ++binds_;
            Bytes a { 4, 2, 4, 0, 7, 0 };
            a.insert (a.end (), 8, ' ');
            a.insert (a.end (), { 0, 0, 0 });
            c.send_data (a);
        } else if (msg[0] == 2) {                       // unbind
            unbound_ = true;
        } else if (msg[0] == 9) {
            for (std::size_t at = 2; at + 2 <= msg.size ();) {
                std::size_t len = get16 (msg, at);
                at += 2;
                cterm (c, msg.subspan (at, len));
                at += len;
            }
        }
    }

    void disconnected (SessionConnection &, unsigned) override
    {
        std::lock_guard l (m_);
        conn_ = nullptr;
        ++disconnects_;
    }

    // Type keys into the read the gate has outstanding.  On the node
    // thread, which owns the connection.
    void type (Node &n, const std::string &keys, std::size_t term_pos)
    {
        n.add_work (std::make_unique<CallbackWork> ([this, keys, term_pos] {
            std::lock_guard l (m_);
            if (!conn_ || !reads_) return;
            --reads_;
            Bytes d { 3, 0, 0, 0, 0, 0, static_cast<std::uint8_t> (term_pos), 0 };
            d.insert (d.end (), keys.begin (), keys.end ());
            conn_->send_data (common_data (d));
        }));
    }

    void hang_up (Node &n)
    {
        n.add_work (std::make_unique<CallbackWork> ([this] {
            std::lock_guard l (m_);
            if (conn_) conn_->disconnect ();
        }));
    }

    std::string output () { std::lock_guard l (m_); return output_; }
    std::string prompt () { std::lock_guard l (m_); return prompt_; }
    int reads () { std::lock_guard l (m_); return reads_; }
    int reads_total () { std::lock_guard l (m_); return reads_total_; }
    bool unbound () { std::lock_guard l (m_); return unbound_; }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }
    int attributes_set () { std::lock_guard l (m_); return attrs_; }

    std::uint16_t width = 132, height = 40;

private:
    void cterm (SessionConnection &c, ByteView m)
    {
        switch (m[0]) {
        case 1: {                                       // initiate
            Bytes i { 1, 0, 1, 0, 0 };
            i.insert (i.end (), 8, ' ');
            i.insert (i.end (), { 1, 2, 0x00, 0x02 });
            c.send_data (common_data (i));
            break;
        }
        case 2: {                                       // start read
            ++reads_;
            ++reads_total_;
            std::size_t eop = get16 (m, 10);
            std::size_t data = 17 + m[16];
            prompt_.assign (m.begin () + static_cast<std::ptrdiff_t> (data),
                            m.begin () + static_cast<std::ptrdiff_t> (data + eop));
            break;
        }
        case 7:                                         // write
            output_.append (m.begin () + 5, m.end ());
            break;
        case 10: {                                      // read characteristics
            Bytes r { 11, 0, 0x09, 0x01,
                      static_cast<std::uint8_t> (width),
                      static_cast<std::uint8_t> (width >> 8),
                      0x0a, 0x01,
                      static_cast<std::uint8_t> (height),
                      static_cast<std::uint8_t> (height >> 8) };
            c.send_data (common_data (r));
            break;
        }
        case 11:                                        // set characteristics
            for (std::size_t at = 2; at + 5 <= m.size (); at += 5)
                if (get16 (m, at) == 0x0202) ++attrs_;
            break;
        }
    }

    std::mutex         m_;
    SessionConnection *conn_ = nullptr;
    bool               accepted_ = false, unbound_ = false;
    int                binds_ = 0, reads_ = 0, reads_total_ = 0, disconnects_ = 0, attrs_ = 0;
    std::string        output_, prompt_;
};

// The terminal end of the remote terminal protocol, as RSX's RMT: it
// answers the host's config, and serves the reads and writes it asks for.
class RmtTerminal : public Application {
public:
    void connect_received (SessionConnection &, ByteView) override {}

    void data_received (SessionConnection &c, ByteView msg) override
    {
        std::lock_guard l (m_);
        conn_ = &c;
        if (msg.empty ()) return;
        switch (msg[0]) {
        case 1:                                         // config
            host_config_.assign (msg.begin (), msg.end ());
            c.send_data (Bytes { 1, 1, 0, 0, 12, 0, 2, 0, 92, 0,
                                 0x01, 0x50, 0, 0 });
            break;
        case 3: {                                       // write
            output_.append (msg.begin () + 10, msg.end ());
            if (!(msg[1] & 0x80)) {
                // Answered with its header, as RSX's RMT does.
                ++write_replies_wanted_;
                Bytes r (msg.begin (), msg.begin () + 10);
                r[2] = 0;
                c.send_data (r);
            }
            break;
        }
        case 7:                                         // single character mode
            ++single_;
            break;
        case 2:                                         // dis
            ++dis_;
            break;
        case 4:                                         // read
        case 5:                                         // read with prompt
            ++reads_total_;
            pending_.assign (msg.begin (), msg.begin () + 10);
            prompt_.assign (msg.begin () + 10, msg.end ());
            break;
        }
    }

    void disconnected (SessionConnection &, unsigned) override
    {
        std::lock_guard l (m_);
        conn_ = nullptr;
        ++disconnects_;
    }

    // Complete the outstanding read with what was typed, then what ended
    // it, as RMT does: RCT counts only the typed part.
    void type (Node &n, const std::string &input, const std::string &term = "")
    {
        n.add_work (std::make_unique<CallbackWork> ([this, input, term] {
            std::lock_guard l (m_);
            if (!conn_ || pending_.empty ()) return;
            Bytes r = pending_;
            r[2] = 0;
            r[3] = term == "\x03" ? 1 : 0;
            r[6] = static_cast<std::uint8_t> (input.size ());
            r[7] = 0;
            r[8] = r[9] = 0;
            r.insert (r.end (), input.begin (), input.end ());
            r.insert (r.end (), term.begin (), term.end ());
            pending_.clear ();
            conn_->send_data (r);
        }));
    }

    // ^C, which RMT sends as a message of its own.
    void control_c (Node &n)
    {
        n.add_work (std::make_unique<CallbackWork> ([this] {
            std::lock_guard l (m_);
            if (conn_) conn_->send_data (Bytes { 12, 0, 0, 0 });
        }));
    }

    Bytes host_config () { std::lock_guard l (m_); return host_config_; }
    std::string output () { std::lock_guard l (m_); return output_; }
    std::string prompt () { std::lock_guard l (m_); return prompt_; }
    Bytes pending () { std::lock_guard l (m_); return pending_; }
    int reads_total () { std::lock_guard l (m_); return reads_total_; }
    int disconnects () { std::lock_guard l (m_); return disconnects_; }
    int write_replies_wanted () { std::lock_guard l (m_); return write_replies_wanted_; }
    int single () { std::lock_guard l (m_); return single_; }
    int dis () { std::lock_guard l (m_); return dis_; }

private:
    std::mutex         m_;
    SessionConnection *conn_ = nullptr;
    Bytes              host_config_, pending_;
    std::string        output_, prompt_;
    int                reads_total_ = 0, disconnects_ = 0, write_replies_wanted_ = 0;
    int                single_ = 0, dis_ = 0;
};

struct Pair {
    std::uint16_t port = free_port ();
    Config acfg, bcfg;
    std::unique_ptr<Node> a, b;

    explicit Pair (const std::string &lines)
        : acfg (Config::from_string (
              "routing 1.1 --type endnode\nnode 1.1 NODEA\nnode 1.2 NODEB\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":listen --t3 2\n" + lines)),
          bcfg (Config::from_string (
              "routing 1.2 --type endnode\nnode 1.2 NODEB\nnode 1.1 NODEA\n"
              "circuit mul-0 Multinet 127.0.0.1:" + std::to_string (port)
              + ":connect --t3 2\n"))
    {
        a = std::make_unique<Node> (acfg);
        b = std::make_unique<Node> (bcfg);
    }

    void start ()
    {
        a->start ();
        b->start ();
        wait_until ([&] {
            return a->routing ()->adjacency_count () == 1
                && b->routing ()->adjacency_count () == 1;
        });
    }
    void stop () { if (b) b->stop (); if (a) a->stop (); }

    // SET HOST NODEA from NODEB.
    Terminal *set_host ()
    {
        Terminal *raw = new Terminal;
        b->add_work (std::make_unique<CallbackWork> ([this, raw] {
            b->session ()->connect (Nodeid::parse ("1.1"),
                                    EndUser::number (CTERM_OBJECT),
                                    EndUser::named ("TESTER"), {},
                                    std::unique_ptr<Terminal> (raw));
        }));
        return raw;
    }

    // RMT NODEA from NODEB: object 23.
    RmtTerminal *rmt ()
    {
        RmtTerminal *raw = new RmtTerminal;
        b->add_work (std::make_unique<CallbackWork> ([this, raw] {
            b->session ()->connect (Nodeid::parse ("1.1"),
                                    EndUser::number (RMT_OBJECT),
                                    EndUser::named ("TESTER"), {},
                                    std::unique_ptr<RmtTerminal> (raw));
        }));
        return raw;
    }
};

std::string write_script (const std::string &name, const std::string &body)
{
    std::string path = dntest::tmp_dir () + "/dntest_chost_" + name + ".sh";
    std::ofstream f (path);
    f << "#!/bin/sh\n" << body;
    f.close ();
    ::chmod (path.c_str (), 0755);
    return path;
}

}   // namespace

DN_TEST (cterm, chost_lines_are_read)
{
    Config c = Config::from_string (
        "chost shell --file /bin/sh --argument -l"
        " --description 'A login shell'\n"
        "chost GAMES --file /usr/games/adventure --term vt220\n");
    DN_ASSERT_EQ (c.chosts ().size (), 2u);
    DN_ASSERT_EQ (c.chosts ()[0].name, std::string ("SHELL"));
    DN_ASSERT_EQ (c.chosts ()[0].file, std::string ("/bin/sh"));
    DN_ASSERT_EQ (c.chosts ()[0].arguments.size (), 1u);
    DN_ASSERT_EQ (c.chosts ()[0].description, std::string ("A login shell"));
    DN_ASSERT_EQ (c.chosts ()[0].term, std::string ("vt100"));
    DN_ASSERT_EQ (c.chosts ()[1].term, std::string ("vt220"));

    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("chost --file /bin/sh\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("chost SHELL\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("chost A --file x\nchost a --file y\n"));
}

#ifndef _WIN32

DN_TEST (cterm, chost_lines_register_object_42)
{
    Config c = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n"
        "chost SHELL --file /bin/sh\n");
    Node n (c);
    const Object *o = n.session ()->find_object (CTERM_OBJECT);
    DN_ASSERT (o != nullptr);
    DN_ASSERT_EQ (o->name, std::string ("CTERM"));
    const Object *r = n.session ()->find_object (RMT_OBJECT);
    DN_ASSERT (r != nullptr);
    DN_ASSERT_EQ (r->name, std::string ("REMACP"));

    // An object line for 42 wins over the gate.
    Config c2 = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n"
        "object --number 42 --name MYCTERM --file /bin/true\n"
        "chost SHELL --file /bin/sh\n");
    Node n2 (c2);
    DN_ASSERT_EQ (n2.session ()->find_object (CTERM_OBJECT)->name,
                  std::string ("MYCTERM"));

    // No chost lines, no object 42.
    Config c3 = Config::from_string (
        "routing 1.1 --type endnode\nnode 1.1 NODEA\n"
        "circuit mul-0 Multinet 127.0.0.1:1:connect\n");
    Node n3 (c3);
    DN_ASSERT (n3.session ()->find_object (CTERM_OBJECT) == nullptr);
}

DN_TEST (cterm, one_chost_runs_its_program_on_a_terminal)
{
    std::string prog = write_script ("one",
        "stty size\n"
        "echo \"term=$TERM node=$DECNET_NODE chost=$DECNET_CHOST\"\n"
        "echo \"via $DECNET_PROTOCOL\"\n"
        "read x\n"
        "echo \"got $x\"\n");
    Pair p ("chost ONLY --file " + prog + " --term vt220\n");
    p.start ();
    Terminal *t = p.set_host ();

    // The terminal's size, as it answered, and the environment.
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("chost=ONLY") != std::string::npos;
    }));
    DN_ASSERT (t->output ().find ("40 132") != std::string::npos);
    DN_ASSERT (t->output ().find ("term=vt220 node=NODEB") != std::string::npos);
    DN_ASSERT (t->output ().find ("via CTERM") != std::string::npos);
    // ^C, ^Y and ^O were made ordinary characters.
    DN_ASSERT_EQ (t->attributes_set (), 3);

    // Keys go to the program; the terminal echoes, not CTERM.
    DN_ASSERT (wait_until ([&] { return t->reads () == 1; }));
    t->type (*p.b, "hello\r", 5);
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("got hello") != std::string::npos;
    }));
    DN_ASSERT (t->output ().find ("hello\r\n") != std::string::npos);

    // The program has ended: the gate unbinds and disconnects.
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));
    DN_ASSERT (t->unbound ());

    p.stop ();
}

DN_TEST (cterm, several_chosts_offer_a_menu)
{
    Pair p ("chost FIRST --file /bin/echo --argument first-program"
            " --description 'The first'\n"
            "chost SECOND --file /bin/echo --argument second-program\n");
    p.start ();
    Terminal *t = p.set_host ();

    DN_ASSERT (wait_until ([&] { return t->reads () == 1; }));
    std::string menu = t->output ();
    DN_ASSERT (menu.find ("NODEA CHOST gate") != std::string::npos);
    DN_ASSERT (menu.find ("1  FIRST") != std::string::npos);
    DN_ASSERT (menu.find ("The first") != std::string::npos);
    DN_ASSERT (menu.find ("2  SECOND") != std::string::npos);
    DN_ASSERT_EQ (t->prompt (), std::string ("\r\nChost> "));

    // Something that is not there: asked again.
    t->type (*p.b, "third\r", 5);
    DN_ASSERT (wait_until ([&] { return t->reads_total () == 2; }));
    DN_ASSERT (t->output ().find ("No such host: THIRD") != std::string::npos);

    // A prefix of one name is enough.
    t->type (*p.b, "sec\r", 3);
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("second-program") != std::string::npos;
    }));
    DN_ASSERT (t->output ().find ("first-program") == std::string::npos);
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));

    p.stop ();
}

DN_TEST (cterm, control_z_leaves_the_menu)
{
    Pair p ("chost A --file /bin/true\nchost B --file /bin/true\n");
    p.start ();
    Terminal *t = p.set_host ();
    DN_ASSERT (wait_until ([&] { return t->reads () == 1; }));
    t->type (*p.b, "\x1a", 0);
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));
    DN_ASSERT (t->unbound ());
    p.stop ();
}

DN_TEST (cterm, the_program_is_hung_up_on_when_the_link_goes)
{
    std::string pidfile = dntest::tmp_dir () + "/dntest_chost.pid";
    std::remove (pidfile.c_str ());
    std::string prog = write_script ("sleeper",
        "echo $$ > " + pidfile + "\necho ready\nexec sleep 100\n");
    Pair p ("chost SLEEPER --file " + prog + "\n");
    p.start ();
    Terminal *t = p.set_host ();
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("ready") != std::string::npos;
    }));
    pid_t pid = 0;
    std::ifstream (pidfile) >> pid;
    DN_ASSERT (pid > 0);
    DN_ASSERT_EQ (::kill (pid, 0), 0);

    t->hang_up (*p.b);
    DN_ASSERT (wait_until ([&] { return ::kill (pid, 0) != 0; }));

    p.stop ();
}

DN_TEST (cterm, a_program_that_will_not_start_says_so)
{
    Pair p ("chost GONE --file /nonexistent/program\n");
    p.start ();
    Terminal *t = p.set_host ();
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));
    DN_ASSERT (t->output ().find ("GONE is not available") != std::string::npos);
    p.stop ();
}

DN_TEST (cterm, lots_of_output_all_arrives)
{
    // Far more than the link holds at once: the program is held back, not
    // its output dropped.
    Pair p ("chost LOTS --file /bin/sh --argument -c"
            " --argument 'seq 1 20000; echo done'\n");
    p.start ();
    Terminal *t = p.set_host ();
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; },
                           std::chrono::seconds (60)));
    std::string out = t->output ();
    DN_ASSERT (out.find ("\r\n19999\r\n20000\r\ndone") != std::string::npos);
    DN_ASSERT (out.find ("\r\n10000\r\n10001\r\n") != std::string::npos);
    p.stop ();
}

DN_TEST (cterm, rsx_rmt_runs_the_program)
{
    std::string prog = write_script ("rmt",
        "echo \"ready chost=$DECNET_CHOST via $DECNET_PROTOCOL\"\n"
        "read x\n"
        "echo \"got $x\"\n");
    Pair p ("chost ONLY --file " + prog + "\n");
    p.start ();
    RmtTerminal *t = p.rmt ();

    // The host's config says RSX: protocol type 2, and carries the three
    // strings RSX's RMT insists on, the last the node name.
    DN_ASSERT (wait_until ([&] { return t->host_config ().size () >= 8; }));
    Bytes cfg = t->host_config ();
    DN_ASSERT_EQ (unsigned (cfg[6]), 2u);
    std::string tail (cfg.begin () + 8, cfg.end ());
    DN_ASSERT_EQ (tail, std::string ("\x0cRSX-11M-Plus\x01 \x06NODEA "));

    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("ready chost=ONLY via RMT") != std::string::npos;
    }));
    // Writes are binary and answered, which paces them.
    DN_ASSERT (t->write_replies_wanted () > 0);

    // Single character mode, then reads of one character with no echo;
    // each key goes to the program, and the next is asked for.
    DN_ASSERT (wait_until ([&] { return t->pending ().size () == 10; }));
    DN_ASSERT_EQ (t->single (), 1);
    Bytes rd = t->pending ();
    DN_ASSERT_EQ (unsigned (rd[0]), 4u);
    DN_ASSERT_EQ (unsigned (rd[1]), 0x18u);
    DN_ASSERT_EQ (unsigned (rd[6]), 1u);
    for (char c : std::string ("hi\r")) {
        int before = t->reads_total ();
        t->type (*p.b, std::string (1, c));
        DN_ASSERT (wait_until ([&] { return t->reads_total () > before; }));
    }
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("got hi") != std::string::npos;
    }));
    // A Dis before the disconnect, as RSX's own host ends a session.
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));
    DN_ASSERT_EQ (t->dis (), 1);
    p.stop ();
}

DN_TEST (cterm, rsx_rmt_output_is_paced_by_replies_and_all_arrives)
{
    // Far more output than the three writes allowed in flight: all of it
    // arrives, in order.
    Pair p ("chost LOTS --file /bin/sh --argument -c"
            " --argument 'seq 1 3000; echo done'\n");
    p.start ();
    RmtTerminal *t = p.rmt ();
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; },
                           std::chrono::seconds (60)));
    std::string out = t->output ();
    DN_ASSERT (out.find ("\r\n2999\r\n3000\r\ndone") != std::string::npos);
    DN_ASSERT (out.find ("\r\n1500\r\n1501\r\n") != std::string::npos);
    p.stop ();
}

DN_TEST (cterm, rsx_rmt_menu_and_control_c)
{
    std::string sleeper = write_script ("rmtsleep",
        "trap 'echo interrupted; exit 0' INT\necho sleeping\n"
        "while :; do sleep 1; done\n");
    Pair p ("chost FIRST --file /bin/echo --argument first-program\n"
            "chost NAPPER --file " + sleeper + "\n");
    p.start ();
    RmtTerminal *t = p.rmt ();

    // The menu, then a line read with the prompt.
    DN_ASSERT (wait_until ([&] { return t->reads_total () == 1; }));
    DN_ASSERT (t->output ().find ("CHOST gate") != std::string::npos);
    DN_ASSERT_EQ (unsigned (t->pending ()[0]), 5u);
    DN_ASSERT_EQ (t->prompt (), std::string ("\r\nChost> "));

    t->type (*p.b, "nap", "\r");
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("sleeping") != std::string::npos;
    }));

    // ^C reaches the program as an interrupt.
    t->control_c (*p.b);
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("interrupted") != std::string::npos;
    }));
    DN_ASSERT (wait_until ([&] { return t->disconnects () == 1; }));
    p.stop ();
}

DN_TEST (cterm, rsx_rmt_status_reports_are_answered_by_the_gate)
{
    // RMT loses what a terminal sends while it writes, so the gate answers
    // ESC [5n itself, once the output before it has been written; RSX never
    // sees the request.
    std::string prog = write_script ("dsr",
        "stty raw -echo\n"
        "printf 'frame\\033[5n'\n"
        "r=$(dd bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \\n')\n"
        "stty sane\n"
        "echo \"answer=$r\"\n");
    Pair p ("chost DSR --file " + prog + "\n");
    p.start ();
    RmtTerminal *t = p.rmt ();
    DN_ASSERT (wait_until ([&] {
        return t->output ().find ("answer=") != std::string::npos;
    }));
    DN_ASSERT (t->output ().find ("answer=1b5b306e") != std::string::npos);
    DN_ASSERT (t->output ().find ("frame") != std::string::npos);
    DN_ASSERT (t->output ().find ("\x1b[5n") == std::string::npos);
    p.stop ();
}

#endif
