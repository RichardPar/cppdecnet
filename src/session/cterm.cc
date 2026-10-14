#include "decnet/session/cterm.h"

#include "decnet/common/logging.h"
#include "decnet/common/timers.h"
#include "decnet/common/work.h"
#include "decnet/node.h"

#ifndef _WIN32

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

namespace decnet::session {

namespace {

// Foundation message types.
enum : std::uint8_t {
    f_bind = 1, f_unbind = 2, f_bind_accept = 4, f_common_data = 9
};

// CTERM message types.
enum : std::uint8_t {
    m_initiate = 1, m_start_read = 2, m_read_data = 3, m_oob = 4,
    m_write = 7, m_read_char = 10, m_characteristics = 11
};

// Characteristic selectors: class in the high byte, identifier low.
constexpr std::uint16_t sel_width = 0x0109, sel_height = 0x010a,
                        sel_char_attr = 0x0202;

// Start Read flags (three bytes).
constexpr std::uint32_t rd_no_raise   = 1u << 6,    // II = 1
                        rd_no_edit    = 2u << 8,    // DDD = 2
                        rd_no_echo    = 1u << 11,   // N
                        rd_this_terms = 1u << 14,   // ZZ = 1: the set given
                        rd_no_escapes = 1u << 16;   // EE = 1

// Write flags: beginning and end of a message (0x30), stop discarding
// (0x08), and transparent (0x800), so the terminal end neither wraps long
// lines nor makes anything of escape sequences: sixel images have lines far
// longer than the screen is wide, and VMS would break them up.
constexpr std::uint16_t wr_whole = 0x0838;

constexpr std::uint8_t CTRL_C = 0x03, CR = 0x0d, CTRL_Y = 0x19, CTRL_Z = 0x1a,
                       CTRL_O = 0x0f;

// Output flow: the program is held when NSP has this many segments queued
// for the link, and let go again when it is down to the second figure.
constexpr std::size_t txq_high = 32, txq_low = 8;
// Output chunks read from the pseudo terminal and not yet sent.
constexpr unsigned max_outstanding = 4;

// How long to wait for the far end to take the last of the output before
// disconnecting anyway.
constexpr auto drain_limit = std::chrono::seconds (10);
// How long to wait for the terminal to say how big it is.
constexpr auto size_limit = std::chrono::seconds (2);

void put16 (Bytes &b, std::size_t v)
{
    b.push_back (static_cast<std::uint8_t> (v));
    b.push_back (static_cast<std::uint8_t> (v >> 8));
}

std::uint16_t get16 (ByteView b, std::size_t at)
{
    return at + 1 < b.size ()
        ? static_cast<std::uint16_t> (b[at] | (b[at + 1] << 8)) : 0;
}

Bytes to_bytes (std::string_view s)
{
    return Bytes (s.begin (), s.end ());
}

Bytes start_read (std::uint32_t flags, std::size_t max,
                  const std::bitset<256> &terms, std::string_view prompt = {})
{
    Bytes m { m_start_read, static_cast<std::uint8_t> (flags),
              static_cast<std::uint8_t> (flags >> 8),
              static_cast<std::uint8_t> (flags >> 16) };
    put16 (m, prompt.size () + max);    // the limit counts the prompt
    put16 (m, prompt.size ());          // end of data
    put16 (m, 0);                       // timeout
    put16 (m, prompt.size ());          // end of prompt
    put16 (m, 0);                       // start of display
    put16 (m, 0);                       // low water
    m.push_back (32);
    for (unsigned i = 0; i < 32; ++i) {
        std::uint8_t byte = 0;
        for (unsigned b = 0; b < 8; ++b)
            if (terms[i * 8 + b]) byte |= static_cast<std::uint8_t> (1u << b);
        m.push_back (byte);
    }
    m.insert (m.end (), prompt.begin (), prompt.end ());
    return m;
}

// A program running on a pseudo terminal.
struct Pty {
    ::pid_t     pid = -1;
    int         master = -1;
    std::string slave;          // the terminal end's name, for flow ()
};

// Stop or restart the program's output, as ^S and ^Q would.  On the
// terminal end: stopping the master does nothing to it.  Opened just for
// this, since a descriptor held open would stop the master seeing the
// program go.
void flow (const Pty &p, bool on)
{
    if (p.slave.empty ()) return;
    int fd = ::open (p.slave.c_str (), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    ::tcflow (fd, on ? TCOON : TCOOFF);
    ::close (fd);
}

// Start h's program on a new pseudo terminal.  env is added to the
// daemon's environment.  On failure, says why in err.
bool spawn_on_pty (const ChostConfig &h,
                   const std::vector<std::string> &env, const ::winsize &ws,
                   Pty &out, std::string &err)
{
    int master = ::posix_openpt (O_RDWR | O_NOCTTY);
    if (master < 0) { err = std::strerror (errno); return false; }
    ::fcntl (master, F_SETFD, FD_CLOEXEC);
    const char *sname = nullptr;
    if (::grantpt (master) < 0 || ::unlockpt (master) < 0
        || !(sname = ::ptsname (master))) {
        err = std::strerror (errno);
        ::close (master);
        return false;
    }
    ::ioctl (master, TIOCSWINSZ, &ws);

    // Open the terminal end here, not in the child: until something has it
    // open, reading the master fails as if the program had ended.
    int slave = ::open (sname, O_RDWR | O_NOCTTY | O_CLOEXEC);
    int exec_pipe[2];
    if (slave < 0 || ::pipe (exec_pipe) < 0) {
        err = std::strerror (errno);
        if (slave >= 0) ::close (slave);
        ::close (master);
        return false;
    }
    ::fcntl (exec_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl (exec_pipe[1], F_SETFD, FD_CLOEXEC);

    // Everything the child needs is built before the fork.
    std::vector<std::string> argv { h.file };
    argv.insert (argv.end (), h.arguments.begin (), h.arguments.end ());
    std::vector<char *> cargv;
    for (std::string &a : argv) cargv.push_back (a.data ());
    cargv.push_back (nullptr);

    std::vector<std::string> envs;
    for (char **e = environ; e && *e; ++e) {
        std::string_view v (*e);
        bool replaced = std::any_of (env.begin (), env.end (),
            [&] (const std::string &n) {
                auto eq = n.find ('=');
                return v.substr (0, eq + 1) == std::string_view (n).substr (0, eq + 1);
            });
        if (!replaced && v.rfind ("COLUMNS=", 0) != 0 && v.rfind ("LINES=", 0) != 0)
            envs.emplace_back (v);
    }
    envs.insert (envs.end (), env.begin (), env.end ());
    std::vector<char *> cenv;
    for (std::string &e : envs) cenv.push_back (e.data ());
    cenv.push_back (nullptr);

    ::pid_t pid = ::fork ();
    if (pid < 0) {
        err = std::strerror (errno);
        ::close (slave); ::close (master);
        ::close (exec_pipe[0]); ::close (exec_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // The child.  Only async-signal-safe calls from here to exec.
        ::setsid ();
        ::ioctl (slave, TIOCSCTTY, 0);
        ::dup2 (slave, STDIN_FILENO);
        ::dup2 (slave, STDOUT_FILENO);
        ::dup2 (slave, STDERR_FILENO);
        // The daemon ignores SIGPIPE and blocks SIGINT and SIGTERM for its
        // own use; the program is owed the defaults, or ^C does nothing.
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int s : { SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD,
                       SIGTSTP, SIGTTIN, SIGTTOU })
            ::sigaction (s, &dfl, nullptr);
        sigset_t none;
        sigemptyset (&none);
        ::sigprocmask (SIG_SETMASK, &none, nullptr);
        environ = cenv.data ();
        ::execvp (cargv[0], cargv.data ());
        int e = errno;
        ssize_t ignored = ::write (exec_pipe[1], &e, sizeof e);
        (void) ignored;
        ::_exit (127);
    }

    ::close (exec_pipe[1]);
    int exec_errno = 0;
    ssize_t got;
    do {
        got = ::read (exec_pipe[0], &exec_errno, sizeof exec_errno);
    } while (got < 0 && errno == EINTR);
    ::close (exec_pipe[0]);
    // The program has the terminal open now, or never will.
    ::close (slave);
    if (got > 0) {
        int status;
        ::waitpid (pid, &status, 0);
        ::close (master);
        err = std::strerror (exec_errno);
        return false;
    }
    ::fcntl (master, F_SETFL, ::fcntl (master, F_GETFL) | O_NONBLOCK);
    out.pid = pid;
    out.master = master;
    out.slave = sname;
    return true;
}

// Collect a program that is finishing, and see that it does: hung up on,
// it has five seconds before it is killed.  On a thread of its own, so the
// node never waits for it.
void reap (::pid_t pid)
{
    ::kill (-pid, SIGHUP);
    std::thread ([pid] {
        int status;
        for (int i = 0; i < 50; ++i) {
            if (::waitpid (pid, &status, WNOHANG) != 0) return;
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        ::kill (-pid, SIGKILL);
        ::waitpid (pid, &status, 0);
    }).detach ();
}

// What every terminal protocol needs: a program on a pseudo terminal, or a
// menu of them first; output passed on no faster than the link takes it;
// and an orderly end.  A protocol says how to write, how to ask for a key
// and how to ask for a line, and passes back what the terminal sends.
class TerminalGate : public Application {
public:
    TerminalGate (Node *node, std::shared_ptr<const std::vector<ChostConfig>> hosts)
        : node_ (node), hosts_ (std::move (hosts)),
          timer_ ([this] { tick (); })
    {
    }

    ~TerminalGate () override
    {
        if (node_) node_->timers ().stop (&timer_);
        stop_program ();
    }

    void connect_received (SessionConnection &c, ByteView) override
    {
        conn_ = &c;
        peer_ = c.remote ().str ();
        if (node_)
            if (const Nodeinfo *n = node_->find_node (c.remote (), false);
                n && !n->name.empty ())
                peer_ = n->name;
        DN_INFO ("SET HOST from {} ({})", peer_, c.source ().str ());
        c.accept ();
    }

    void disconnected (SessionConnection &, unsigned reason) override
    {
        DN_DEBUG ("SET HOST from {}: link closed, reason {}", peer_, reason);
        conn_ = nullptr;
        state_ = State::done;
        if (node_) node_->timers ().stop (&timer_);
        stop_program ();
    }

protected:
    enum class State { starting, sizing, menu, running, closing, done };

    // ------------------------------------------------- the protocol's part

    // Write to the terminal, as it is.
    virtual void write (ByteView data) = 0;
    // Ask for the next key: no echo, no editing.
    virtual void read_char () = 0;
    // Show text and read a line, echoed and editable.
    virtual void read_line (std::string_view prompt) = 0;
    // Said to the terminal before disconnecting.
    virtual void farewell () {}
    // The protocol's name, for the program: $DECNET_PROTOCOL.
    virtual const char *protocol () const = 0;
    // The protocol's own flow control: true while it has as much output
    // outstanding as it wants.  The program is held meanwhile.
    virtual bool congested () { return false; }

    // ---------------------------------------- what the protocol calls back

    void send (Bytes m)
    {
        if (conn_) conn_->send_data (std::move (m));
    }

    void write (std::string_view s)
    {
        Bytes b = to_bytes (s);
        write (ByteView (b.data (), b.size ()));
    }

    // The terminal is set up.  Wait a little for it to say how big it is,
    // if it has been asked, so the program starts with that.
    void ready (bool wait_for_size)
    {
        if (!wait_for_size) { begin (); return; }
        state_ = State::sizing;
        deadline_ = std::chrono::steady_clock::now () + size_limit;
        arm ();
    }

    void size (unsigned width, unsigned height)
    {
        if (width)  ws_.ws_col = static_cast<unsigned short> (width);
        if (height) ws_.ws_row = static_cast<unsigned short> (height);
        if (pty_.master >= 0) ::ioctl (pty_.master, TIOCSWINSZ, &ws_);
        if (state_ == State::sizing) begin ();
    }

    // Keys that answer read_char (): to the program, and ask for more.
    void typed (ByteView keys)
    {
        if (state_ != State::running) return;
        to_program (keys);
        read_char ();
    }

    // ^C or the like, out of band: to the program, or out of the menu.
    void interrupt (std::uint8_t c)
    {
        if (state_ == State::running) to_program (ByteView (&c, 1));
        else if (state_ == State::menu) finish ();
    }

    // The line that answers read_line (), and what ended it.
    void line (std::string input, std::uint8_t term)
    {
        if (state_ == State::menu) choose (std::move (input), term);
    }

    // Something for the program, as if the terminal had sent it.
    void answer (ByteView b) { to_program (b); }

    // The protocol has room for more output again.
    void uncongested ()
    {
        if (state_ == State::running) tick ();
    }

    // The far end has gone, or said it is going.
    void hang_up ()
    {
        stop_program ();
        state_ = State::done;
        if (conn_) conn_->disconnect ();
    }

    State                                            state_ = State::starting;
    std::string                                      peer_;

    std::string node_name () const { return node_ ? node_->name () : ""; }

private:
    // The terminal is ready: the program, or a choice of them.
    void begin ()
    {
        if (hosts_->size () == 1) run (hosts_->front ());
        else menu ();
    }

    // ------------------------------------------------------------ menu

    void menu ()
    {
        state_ = State::menu;
        std::string s = "\r\n" + (node_ ? node_->name () : std::string ())
                      + " CHOST gate\r\n\r\n";
        for (std::size_t i = 0; i < hosts_->size (); ++i) {
            const ChostConfig &h = (*hosts_)[i];
            std::string n = std::to_string (i + 1);
            s += std::string (4 - std::min<std::size_t> (n.size (), 3), ' ') + n
               + "  " + h.name;
            if (!h.description.empty ())
                s += std::string (18 - h.name.size (), ' ') + h.description;
            s += "\r\n";
        }
        s += "\r\nType a name or a number, or ^Z to leave.\r\n";
        write (s);
        read_line ("\r\nChost> ");
    }

    void choose (std::string input, std::uint8_t term)
    {
        if (term != CR) {
            write (std::string_view ("\r\n"));
            finish ();
            return;
        }
        std::erase_if (input, [] (unsigned char c) { return std::isspace (c); });
        std::transform (input.begin (), input.end (), input.begin (),
                        [] (unsigned char c) { return std::toupper (c); });
        write (std::string_view ("\r\n"));
        if (input.empty ()) { read_line ("\r\nChost> "); return; }

        // A number, a name, or the start of just one name.
        const ChostConfig *pick = nullptr;
        unsigned matches = 0;
        for (std::size_t i = 0; i < hosts_->size (); ++i) {
            const ChostConfig &h = (*hosts_)[i];
            if (input == std::to_string (i + 1) || input == h.name) {
                pick = &h;
                matches = 1;
                break;
            }
            if (h.name.rfind (input, 0) == 0) { pick = &h; ++matches; }
        }
        if (matches != 1) {
            write (matches ? "\r\n" + input + " is ambiguous\r\n"
                           : "\r\nNo such host: " + input + "\r\n");
            read_line ("\r\nChost> ");
            return;
        }
        run (*pick);
    }

    // --------------------------------------------------------- program

    void run (const ChostConfig &h)
    {
        std::vector<std::string> env {
            "TERM=" + h.term,
            "DECNET_NODE=" + peer_,
            "DECNET_NODEID=" + (conn_ ? conn_->remote ().str () : std::string ()),
            "DECNET_USER=" + (conn_ ? conn_->source ().str () : std::string ()),
            "DECNET_CHOST=" + h.name,
            std::string ("DECNET_PROTOCOL=") + protocol (),
        };
        std::string err;
        if (!spawn_on_pty (h, env, ws_, pty_, err)) {
            DN_ERROR ("chost {}: cannot run {}: {}", h.name, h.file, err);
            write ("\r\n%CHOST-E-NOSTART, " + h.name + " is not available\r\n");
            finish ();
            return;
        }
        DN_INFO ("SET HOST from {}: running {} ({}) as pid {}", peer_, h.name,
                 h.file, pty_.pid);
        if (::pipe (wake_) < 0) {
            DN_ERROR ("chost {}: {}", h.name, std::strerror (errno));
            finish ();
            return;
        }
        ::fcntl (wake_[0], F_SETFD, FD_CLOEXEC);
        ::fcntl (wake_[1], F_SETFD, FD_CLOEXEC);
        state_ = State::running;
        stopping_ = false;
        reader_ = std::thread ([this] { read_pty (); });
        read_char ();
    }

    void to_program (ByteView keys)
    {
        std::size_t at = 0;
        while (pty_.master >= 0 && at < keys.size ()) {
            ssize_t n = ::write (pty_.master, keys.data () + at, keys.size () - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                // The program is not reading and its terminal is full.
                DN_DEBUG ("SET HOST from {}: input lost: {}", peer_,
                          n < 0 ? std::strerror (errno) : "no room");
                return;
            }
            at += static_cast<std::size_t> (n);
        }
    }

    // On the reader thread: pass the program's output to the node thread,
    // no faster than the link takes it.
    void read_pty ()
    {
        logging::set_thread_name ("chost " + peer_);
        std::uint8_t buf[256];
        for (;;) {
            {
                std::unique_lock l (m_);
                cv_.wait (l, [&] {
                    return stopping_ || (!paused_ && outstanding_ < max_outstanding);
                });
                if (stopping_) return;
            }
            pollfd p[2] = { { pty_.master, POLLIN, 0 }, { wake_[0], POLLIN, 0 } };
            if (::poll (p, 2, -1) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (p[1].revents) return;
            ssize_t n = ::read (pty_.master, buf, sizeof buf);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;      // EIO: everything on the terminal has gone
            {
                std::lock_guard l (m_);
                ++outstanding_;
            }
            post ([this, out = Bytes (buf, buf + n)] { from_program (out); });
        }
        post ([this] { program_ended (); });
    }

    // Run fn on the node thread, unless this has been destroyed by then.
    void post (std::function<void ()> fn)
    {
        if (!node_) return;
        node_->add_work (std::make_unique<CallbackWork> (
            [alive = std::weak_ptr<int> (alive_), fn = std::move (fn)] {
                if (!alive.expired ()) fn ();
            }));
    }

    void from_program (const Bytes &out)
    {
        {
            std::lock_guard l (m_);
            if (outstanding_) --outstanding_;
        }
        if (state_ == State::running)
            write (ByteView (out.data (), out.size ()));
        if (queued () > txq_high || congested ()) {
            bool was;
            {
                std::lock_guard l (m_);
                was = paused_;
                paused_ = true;
            }
            // Hold the program itself, not only the reading of what it
            // writes: the terminal holds some 17K, which on a slow link is
            // many seconds of output queued ahead of anything new.
            if (!was) flow (pty_, false);
            arm ();
        }
        cv_.notify_all ();
    }

    void program_ended ()
    {
        if (state_ != State::running) return;
        DN_INFO ("SET HOST from {}: program ended", peer_);
        stop_program ();
        finish ();
    }

    // Hang up on the program and wait for the reader thread.
    void stop_program ()
    {
        {
            std::lock_guard l (m_);
            stopping_ = true;
        }
        cv_.notify_all ();
        if (wake_[1] >= 0) {
            ssize_t ignored = ::write (wake_[1], "x", 1);
            (void) ignored;
        }
        if (reader_.joinable ()) reader_.join ();
        if (pty_.pid > 0) { reap (pty_.pid); pty_.pid = -1; }
        if (pty_.master >= 0) { ::close (pty_.master); pty_.master = -1; }
        for (int &fd : wake_)
            if (fd >= 0) { ::close (fd); fd = -1; }
    }

    // ------------------------------------------------------- ending

    // Our side is done: say so, then disconnect once the far end has had
    // everything, so the last of the output is not lost.
    void finish ()
    {
        if (state_ == State::closing || state_ == State::done) return;
        state_ = State::closing;
        farewell ();
        deadline_ = std::chrono::steady_clock::now () + drain_limit;
        tick ();
    }

    std::size_t queued () const
    {
        nsp::Connection *n = conn_ ? conn_->nsp_connection () : nullptr;
        return n ? n->queued () : 0;
    }

    void arm ()
    {
        if (node_) node_->timers ().start (&timer_, std::chrono::milliseconds (100));
    }

    void tick ()
    {
        if (state_ == State::sizing) {
            if (std::chrono::steady_clock::now () > deadline_) begin ();
            else arm ();
            return;
        }
        if (state_ == State::closing) {
            if (queued () == 0 || std::chrono::steady_clock::now () > deadline_) {
                state_ = State::done;
                if (conn_) conn_->disconnect ();
            } else {
                arm ();
            }
            return;
        }
        bool resume;
        {
            std::lock_guard l (m_);
            resume = paused_ && queued () <= txq_low && !congested ();
            if (resume) paused_ = false;
        }
        if (resume) {
            flow (pty_, true);
            cv_.notify_all ();
        }
        else if (paused_) arm ();
    }

    Node                                            *node_;
    std::shared_ptr<const std::vector<ChostConfig>>  hosts_;
    SessionConnection                               *conn_ = nullptr;
    ::winsize                                        ws_ { 24, 80, 0, 0 };

    Pty                     pty_;
    int                     wake_[2] = { -1, -1 };
    std::thread             reader_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stopping_ = false;
    bool                    paused_ = false;
    unsigned                outstanding_ = 0;

    CallbackTimer                          timer_;
    std::chrono::steady_clock::time_point  deadline_;   // sizing or closing
    std::shared_ptr<int>                   alive_ = std::make_shared<int> (0);
};

// ------------------------------------------------------------------ CTERM

// Object 42: CTERM over Terminal Foundation Services, what VMS (and MIM's
// RSX) use for SET HOST.
class CtermGate : public TerminalGate {
public:
    using TerminalGate::TerminalGate;

    void run_state (SessionConnection &) override
    {
        // The host speaks first: Foundation version 2.4, as VMS.
        send (Bytes { f_bind, 2, 4, 0, 7, 0, 0x10, 0 });
    }

    void data_received (SessionConnection &, ByteView msg) override
    {
        if (msg.empty ()) return;
        switch (msg[0]) {
        case f_bind_accept:
            if (bound_) break;
            bound_ = true;
            initiate ();
            break;
        case f_unbind:
            DN_DEBUG ("SET HOST from {}: the terminal unbound", peer_);
            hang_up ();
            break;
        case f_common_data: {
            std::size_t at = 2;
            while (at + 2 <= msg.size ()) {
                std::size_t len = get16 (msg, at);
                at += 2;
                if (at + len > msg.size ()) break;
                cterm_message (msg.subspan (at, len));
                at += len;
            }
            break;
        }
        default:
            break;      // mode messages: only command mode is used
        }
    }

private:
    // One or more CTERM messages in a Foundation common data message.
    void send_cterm (std::initializer_list<Bytes> msgs)
    {
        Bytes f { f_common_data, 0 };
        for (const Bytes &m : msgs) {
            put16 (f, m.size ());
            f.insert (f.end (), m.begin (), m.end ());
        }
        send (std::move (f));
    }

    using TerminalGate::write;

    void write (ByteView data) override
    {
        // Short of the far end's message limit, with room for the headers.
        std::size_t chunk = maxmsg_ > 64 ? maxmsg_ - 16 : 48;
        for (std::size_t at = 0; at < data.size (); at += chunk) {
            Bytes m { m_write, static_cast<std::uint8_t> (wr_whole),
                      static_cast<std::uint8_t> (wr_whole >> 8), 0, 0 };
            ByteView part = data.subspan (at, std::min (chunk, data.size () - at));
            m.insert (m.end (), part.begin (), part.end ());
            send_cterm ({ m });
        }
    }

    // Every character ends the read.
    void read_char () override
    {
        std::bitset<256> all;
        all.set ();
        send_cterm ({ start_read (rd_no_raise | rd_no_edit | rd_no_echo
                                  | rd_this_terms | rd_no_escapes, 80, all) });
    }

    void read_line (std::string_view prompt) override
    {
        std::bitset<256> terms;
        for (std::uint8_t c : { CR, CTRL_Z, CTRL_C, CTRL_Y }) terms.set (c);
        send_cterm ({ start_read (rd_this_terms | rd_no_escapes, 32, terms,
                                  prompt) });
    }

    void farewell () override
    {
        send (Bytes { f_unbind, 3, 0 });
    }

    const char *protocol () const override { return "CTERM"; }

    void initiate ()
    {
        Bytes i { m_initiate, 0, 1, 4, 0 };
        const char rev[] = "CPPDECNT";
        i.insert (i.end (), rev, rev + 8);
        i.insert (i.end (), { 1, 2 }); put16 (i, 512);      // max message
        i.insert (i.end (), { 2, 2 }); put16 (i, 1012);     // input buffer
        i.insert (i.end (), { 3, 4, 0xfe, 0x7f, 0, 0 });    // messages 1-14
        send_cterm ({ i });
    }

    void cterm_message (ByteView m)
    {
        if (m.empty ()) return;
        switch (m[0]) {
        case m_initiate:
            if (!initiated_) { initiated_ = true; terminal_ready (m); }
            break;
        case m_read_data:
            if (m.size () >= 8) read_done (m.subspan (8), get16 (m, 6));
            break;
        case m_oob:
            // Not asked for, but a terminal may send ^C or ^Y out of band
            // all the same.
            if (m.size () >= 3) interrupt (m[2]);
            break;
        case m_characteristics: {
            unsigned w = 0, h = 0;
            for (std::size_t at = 2; at + 4 <= m.size (); at += 4) {
                std::uint16_t sel = get16 (m, at), v = get16 (m, at + 2);
                if (sel == sel_width)       w = v;
                else if (sel == sel_height) h = v;
                else break;
            }
            size (w, h);
            break;
        }
        default:
            break;      // write completions, discard and input state
        }
    }

    void terminal_ready (ByteView init)
    {
        // Parameters follow the version and the 8 byte revision.
        for (std::size_t at = 13; at + 2 <= init.size ();) {
            std::uint8_t id = init[at], len = init[at + 1];
            at += 2;
            if (at + len > init.size ()) break;
            if (id == 1 && len == 2)
                maxmsg_ = std::clamp<std::size_t> (get16 (init, at), 64, 8192);
            at += len;
        }

        // ^C, ^Y and ^O are to be ordinary characters, sent in band, so they
        // reach the pseudo terminal as typed; and ask how big the terminal
        // is.
        Bytes set { m_characteristics, 0 };
        for (std::uint8_t c : { CTRL_C, CTRL_Y, CTRL_O }) {
            put16 (set, sel_char_attr);
            set.insert (set.end (), { c, 0xff, 0x00 });
        }
        Bytes ask { m_read_char, 0 };
        put16 (ask, sel_width);
        put16 (ask, sel_height);
        send_cterm ({ set, ask });
        ready (true);
    }

    void read_done (ByteView data, std::size_t term_pos)
    {
        if (state_ == State::running) {
            typed (data);
        } else if (state_ == State::menu) {
            term_pos = std::min (term_pos, data.size ());
            line (std::string (data.begin (), data.begin ()
                               + static_cast<std::ptrdiff_t> (term_pos)),
                  term_pos < data.size () ? data[term_pos] : CR);
        }
    }

    bool        bound_ = false;
    bool        initiated_ = false;
    std::size_t maxmsg_ = 512;
};

// ------------------------------------------------------- remote terminal

// Object 23: the remote terminal protocol from before CTERM, which RSX's
// RMT still uses for SET HOST.  Its RSX variant: each side sends a config
// message, then the host asks for reads and writes, each a message with a
// ten byte header:
//
//     FNC MOD FLG STS IDN RSV RCT(2) TCT(2) DAT...
//
// PyDECnet's notes (doc/protocols/remote-terminal.txt) give the layout,
// from RSTS's terminal end.  What RSX's RMT wants on top was found by
// watching RSX-11M-PLUS V4.6's own host (RMHACP) and trying RMT against
// this one: the config with its three strings, single character mode for
// raw input, and binary writes paced by their replies.
class RmtGate : public TerminalGate {
public:
    using TerminalGate::TerminalGate;

    void run_state (SessionConnection &) override
    {
        // Config, as an RSX host sends it: version 1.0.0, RSX-11M, the RSX
        // variant, then three counted strings -- the system, a single
        // space (meaning unknown) and the node name padded to six.  RMT
        // calls a host without them incompatible, and hangs up with a Dis.
        Bytes m { r_config, 1, 0, 0, 4, 0, rp_rsx, 0 };
        std::string name = node_name ();
        name.resize (std::max<std::size_t> (name.size (), 6), ' ');
        for (std::string_view f : { std::string_view ("RSX-11M-Plus"),
                                    std::string_view (" "),
                                    std::string_view (name) }) {
            m.push_back (static_cast<std::uint8_t> (f.size ()));
            m.insert (m.end (), f.begin (), f.end ());
        }
        send (std::move (m));
    }

    void data_received (SessionConnection &, ByteView msg) override
    {
        DN_TRACE ("SET HOST from {}: RMT {}", peer_, hex (msg));
        if (msg.empty ()) return;
        switch (msg[0]) {
        case r_config:
            if (configured_) break;
            configured_ = true;
            if (msg.size () >= 8 && !(get16 (msg, 6) & rp_rsx))
                DN_WARN ("SET HOST from {}: the terminal does not offer the "
                         "RSX protocol (it offers {:#x})", peer_, get16 (msg, 6));
            // RP.BUF: the largest message (RSX says 92), header and all.
            if (msg.size () >= 10 && get16 (msg, 8) >= 32)
                chunk_ = std::min<std::size_t> (get16 (msg, 8), 512) - 10;
            ready (false);
            break;
        case r_dis:
            // The terminal has given up on us, saying which version it is.
            DN_WARN ("SET HOST from {}: RMT says it is incompatible "
                     "(it is version {}.{})", peer_,
                     msg.size () > 1 ? msg[1] : 0, msg.size () > 2 ? msg[2] : 0);
            break;
        case r_write:
            // A write done.
            if (unreplied_) --unreplied_;
            last_reply_ = std::chrono::steady_clock::now ();
            answer_reports ();
            uncongested ();
            break;
        case r_read:
        case r_read_prompt:
            if (msg.size () >= 10) read_done (msg);
            break;
        case r_ctrl_c:
            interrupt (CTRL_C);
            break;
        default:
            DN_DEBUG ("SET HOST from {}: RMT sent {}", peer_, hex (msg));
            break;
        }
    }

private:
    enum : std::uint8_t {
        r_config = 1, r_dis = 2, r_write = 3, r_read = 4, r_read_prompt = 5,
        r_single = 7, r_ctrl_c = 12
    };
    // Protocol type: the RSX variant.
    static constexpr std::uint8_t rp_rsx = 2;
    // RP.MOD: a binary write, which RSX's terminal driver leaves alone
    // (normal writes are wrapped at the terminal's width); a read with no
    // echo that any control character ends, as RSX reads a password.
    static constexpr std::uint8_t wm_binary = 0x01, rm_no_echo_ctrl = 0x18;
    // Writes awaiting their reply.  RMT does everything in order, so a
    // read waits behind every write already sent: keep it short.  RSX-11M-
    // PLUS V4.6's RMT manages some 35 writes (2.8K) a second however many
    // are in flight, so more than a few only slows the keys.
    static constexpr unsigned max_unreplied = 4;
    // A write whose reply has not come in this long is given up on.
    static constexpr auto reply_limit = std::chrono::seconds (3);
    // Reads kept posted.
    static constexpr unsigned reads_posted = 2;
    // Failed reads in a row that mean the terminal has hung up.
    static constexpr unsigned max_failed_reads = 50;

    Bytes header (std::uint8_t fnc, std::uint8_t mod, std::uint8_t flg,
                  std::size_t rct, std::size_t tct)
    {
        Bytes m { fnc, mod, flg, 0, ++idn_, 0 };
        put16 (m, rct);
        put16 (m, tct);
        return m;
    }

    using TerminalGate::write;

    void write (ByteView data) override
    {
        // A status report request (ESC [5n) is answered here, not by the
        // terminal: RMT loses what a terminal sends while it is writing,
        // and the answer always comes then.  The answer is given once the
        // terminal has had everything before it, which is what a program
        // asks it for: a game uses it to keep no more than a frame ahead.
        static constexpr std::uint8_t dsr[] = { 0x1b, '[', '5', 'n' };
        Bytes out;
        out.reserve (data.size () + dsr_match_);
        for (std::uint8_t c : data) {
            if (c == dsr[dsr_match_]) {
                if (++dsr_match_ == sizeof dsr) {
                    dsr_match_ = 0;
                    send_writes (out);
                    out.clear ();
                    reports_.push_back (writes_sent_);
                }
                continue;
            }
            // Not a request after all: what was held back is ordinary output.
            out.insert (out.end (), dsr, dsr + dsr_match_);
            dsr_match_ = 0;
            if (c == dsr[0]) { dsr_match_ = 1; continue; }
            out.push_back (c);
        }
        send_writes (out);
        answer_reports ();
    }

    void send_writes (const Bytes &data)
    {
        for (std::size_t at = 0; at < data.size (); at += chunk_) {
            std::size_t n = std::min (chunk_, data.size () - at);
            Bytes m = header (r_write, wm_binary, 0, 0, n);
            m.insert (m.end (), data.begin () + static_cast<std::ptrdiff_t> (at),
                      data.begin () + static_cast<std::ptrdiff_t> (at + n));
            if (!unreplied_) last_reply_ = std::chrono::steady_clock::now ();
            ++unreplied_;
            ++writes_sent_;
            send (std::move (m));
        }
    }

    // Answer each status report whose writes have all been done.
    void answer_reports ()
    {
        std::uint64_t done = writes_sent_ - unreplied_;
        while (!reports_.empty () && reports_.front () <= done) {
            reports_.pop_front ();
            static constexpr std::uint8_t ok[] = { 0x1b, '[', '0', 'n' };
            answer (ByteView (ok, sizeof ok));
        }
    }

    bool congested () override
    {
        if (unreplied_ < max_unreplied) return false;
        if (std::chrono::steady_clock::now () - last_reply_ > reply_limit) {
            DN_DEBUG ("SET HOST from {}: no reply to {} writes; going on "
                      "without", peer_, unreplied_);
            unreplied_ = 0;
            answer_reports ();
            return false;
        }
        return true;
    }

    // One character, no echo.  Single character mode first, as RSX's host
    // turns on for EDT: without it RMT reads everything typed and hands
    // over only the one character asked for, losing the rest.  And two
    // reads, not one: RMT does everything in order, so the read that
    // replaces one just done can wait behind output, and what is typed
    // while RMT has no read at all is lost.  Each read done is replaced,
    // so there is always one waiting.
    void read_char () override
    {
        unsigned n = 1;
        if (!single_) {
            single_ = true;
            Bytes m = header (r_single, 0x11, 0x04, 0, 0);
            m.push_back (0x04);         // as RSX sends it, length or no
            send (std::move (m));
            n = reads_posted;
        }
        for (unsigned i = 0; i < n; ++i)
            send (header (r_read, rm_no_echo_ctrl, 0, 1, 0));
    }

    // An ordinary line read, which the terminal echoes and edits, ended by
    // return, escape, ^Z or ^C.
    void read_line (std::string_view prompt) override
    {
        Bytes m = header (r_read_prompt, 0, 0, 32, prompt.size ());
        m.insert (m.end (), prompt.begin (), prompt.end ());
        send (std::move (m));
    }

    // As RSX's host does before it disconnects.
    void farewell () override
    {
        send (Bytes { r_dis, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    }

    const char *protocol () const override { return "RMT"; }

    void read_done (ByteView msg)
    {
        // RCT is how much was typed, not counting what ended the read; the
        // data holds both.
        std::size_t rct = get16 (msg, 6);
        std::uint8_t sts = msg[3];
        ByteView dat = msg.subspan (10);
        if (sts != 0 && rct == 0
            && (dat.empty () || (dat.size () == 1 && dat[0] == 0))) {
            // A failed read with nothing in it.  One now and then is
            // nothing much; a run of them is RSX telling us the terminal
            // has gone (hung up), failing every read at once.
            if (++failed_reads_ >= max_failed_reads) {
                DN_DEBUG ("SET HOST from {}: {} reads failed in a row, status "
                          "{}: the terminal has gone", peer_, failed_reads_, sts);
                hang_up ();
                return;
            }
            if (state_ == State::running) read_char ();
            return;
        }
        failed_reads_ = 0;
        ByteView input = dat.subspan (0, std::min (rct, dat.size ()));
        std::uint8_t term = sts == 1 ? CTRL_C : rct < dat.size () ? dat[rct] : CR;
        if (state_ == State::running) {
            if (input.empty () && !dat.empty ())
                input = dat.subspan (0, 1);     // a control character alone
            typed (input);
        } else if (state_ == State::menu) {
            line (std::string (input.begin (), input.end ()),
                  term == CTRL_Z || term == CTRL_C ? term : CR);
        }
    }

    static std::string hex (ByteView b)
    {
        std::string s;
        static const char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < b.size () && i < 32; ++i) {
            if (i) s += ' ';
            s += digits[b[i] >> 4];
            s += digits[b[i] & 15];
        }
        if (b.size () > 32) s += " ...";
        return s;
    }

    bool         configured_ = false;
    bool         single_ = false;
    std::size_t  chunk_ = 64;      // data in one write
    std::uint8_t idn_ = 0;
    unsigned     unreplied_ = 0;
    std::uint64_t writes_sent_ = 0;
    std::deque<std::uint64_t> reports_;     // writes_sent_ at each ESC [5n
    std::size_t  dsr_match_ = 0;            // of ESC [5n, seen so far
    unsigned     failed_reads_ = 0;
    std::chrono::steady_clock::time_point last_reply_;
};

}   // namespace

bool cterm_gate_supported () noexcept { return true; }

namespace {

template <typename Gate>
ApplicationFactory gate (Node *node, std::vector<ChostConfig> hosts)
{
    auto shared = std::make_shared<const std::vector<ChostConfig>> (std::move (hosts));
    return [node, shared] () -> std::unique_ptr<Application> {
        return std::make_unique<Gate> (node, shared);
    };
}

}   // namespace

ApplicationFactory cterm_gate (Node *node, std::vector<ChostConfig> hosts)
{
    return gate<CtermGate> (node, std::move (hosts));
}

ApplicationFactory rmt_gate (Node *node, std::vector<ChostConfig> hosts)
{
    return gate<RmtGate> (node, std::move (hosts));
}

}   // namespace decnet::session

#else   // _WIN32

namespace decnet::session {

bool cterm_gate_supported () noexcept { return false; }

ApplicationFactory cterm_gate (Node *, std::vector<ChostConfig>)
{
    return {};
}

ApplicationFactory rmt_gate (Node *, std::vector<ChostConfig>)
{
    return {};
}

}   // namespace decnet::session

#endif
