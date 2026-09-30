// tools/dnfal.cc -- File Access Listener, run by decnetd as object 17.
//
// A replacement for PyDECnet's applications/fal.py.  decnetd starts one
// per connection and talks to it over stdin and stdout in PyDECnet's
// object protocol, one JSON object per line.  Configure it as
//
//     object --number 17 --name FAL --file dnfal --argument /srv/decnet
//
// Arguments, each its own --argument:
//
//     ROOT          the directory served
//     rw            allow create, delete and rename (without a user file)
//     users=FILE    check user names and passwords; see dap/fal_users.h
//     trace         log each DAP message
//
// Without a user file anyone may connect, and ROOT and rw apply to all.
// With one, a connect is refused with "access control rejected" unless
// its user and password match an entry, which then says the directory and
// access.  Everything is confined to that directory.
//
//     dnfal --hash      reads a password, prints a hash for the user file
//
// Log lines go to stderr, which decnetd logs.

#include "decnet/common/json.h"
#include "decnet/dap/fal.h"
#include "decnet/dap/fal_users.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <termios.h>
#include <unistd.h>

using namespace decnet;

namespace {

// Session control's "access control rejected".
constexpr int BAD_AUTH = 34;

// Log through decnetd: a JSON record on stderr, at PyDECnet's levels.
void log (int level, const std::string &text)
{
    json::Object o;
    o.set ("level", level);
    o.set ("message", "dnfal: " + text);
    std::cerr << o.encode () << std::endl;
}

// The object protocol, as a DAP transport.
class ObjectLink : public dap::Transport {
public:
    // The connect that starts it all, or nothing if stdin closes first.
    std::optional<json::Object> wait_connect ()
    {
        std::string line;
        while (std::getline (std::cin, line)) {
            auto m = parse (line);
            if (m && m->str ("type") == "connect") {
                handle_ = m->num ("handle");
                return m;
            }
        }
        return std::nullopt;
    }

    void accept () { reply ("accept"); }

    void reject (int reason)
    {
        json::Object o;
        o.set ("handle", handle_);
        o.set ("type", "reject");
        o.set ("data", "");
        o.set ("reason", reason);
        put (o);
    }

    std::optional<Bytes> recv () override
    {
        std::string line;
        while (std::getline (std::cin, line)) {
            auto m = parse (line);
            if (!m) continue;
            std::string type = m->str ("type");
            if (type == "data") return m->bytes ("data");
            if (type == "disconnect" || type == "reject" || type == "abort")
                return std::nullopt;
            // runstate, interrupt: nothing to do.
        }
        return std::nullopt;
    }

    void send (const Bytes &msg) override
    {
        json::Object o;
        o.set ("handle", handle_);
        o.set ("type", "data");
        o.set_bytes ("data", msg);
        put (o);
    }

    void disconnect () { reply ("disconnect"); }

private:
    std::optional<json::Object> parse (const std::string &line)
    {
        try {
            return json::Object::parse (line);
        } catch (const std::exception &e) {
            log (30, std::string ("bad message from decnetd: ") + e.what ());
            return std::nullopt;
        }
    }

    void reply (const char *type)
    {
        json::Object o;
        o.set ("handle", handle_);
        o.set ("type", type);
        o.set ("data", "");
        put (o);
    }

    void put (const json::Object &o)
    {
        std::cout << o.encode () << "\n" << std::flush;
    }

    std::int64_t handle_ = 0;
};

// dnfal --hash: a hash for the user file.
int make_hash ()
{
    bool tty = ::isatty (STDIN_FILENO);
    termios old {};
    if (tty) {
        std::cerr << "Password: " << std::flush;
        ::tcgetattr (STDIN_FILENO, &old);
        termios quiet = old;
        quiet.c_lflag &= ~static_cast<tcflag_t> (ECHO);
        ::tcsetattr (STDIN_FILENO, TCSANOW, &quiet);
    }
    std::string pw;
    std::getline (std::cin, pw);
    if (tty) {
        ::tcsetattr (STDIN_FILENO, TCSANOW, &old);
        std::cerr << "\n";
    }
    if (pw.empty ()) {
        std::cerr << "dnfal: empty password; use - in the file for none\n";
        return 2;
    }
    try {
        std::cout << dap::hash_password (pw) << "\n";
    } catch (const std::exception &e) {
        std::cerr << "dnfal: " << e.what () << "\n";
        return 1;
    }
    return 0;
}

}   // namespace

int main (int argc, char **argv)
{
    dap::FalOptions opts;
    std::string users_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--hash")                         return make_hash ();
        if (a == "rw" || a == "--write")           opts.writable = true;
        else if (a == "trace" || a == "--trace")   opts.trace = true;
        else if (a.rfind ("users=", 0) == 0)       users_file = a.substr (6);
        else if (opts.root.empty ())               opts.root = a;
        else {
            log (40, "unexpected argument " + a);
            return 2;
        }
    }
    if (opts.root.empty ()) {
        log (40, "usage: dnfal ROOT [rw] [users=FILE] [trace]");
        return 2;
    }

    ObjectLink link;
    auto connect = link.wait_connect ();
    if (!connect) return 0;
    std::string from = connect->str ("destination");
    std::string user = connect->str ("username");

    if (!users_file.empty ()) {
        // Read the file for each connection, so changes apply at once.
        std::optional<dap::FalUser> who;
        try {
            auto users = dap::FalUsers::load (users_file, opts.root);
            who = users.authenticate (user, connect->str ("password"));
        } catch (const std::exception &e) {
            log (40, e.what ());
        }
        if (!who) {
            log (30, "access control rejected for "
                     + (user.empty () ? std::string ("anonymous") : "user " + user)
                     + " from " + from);
            // Slow down anyone trying passwords.
            std::this_thread::sleep_for (std::chrono::seconds (1));
            link.reject (BAD_AUTH);
            return 0;
        }
        opts.root = who->root;
        opts.writable = who->writable;
    }

    link.accept ();
    log (20, "connection from " + from
             + (user.empty () ? std::string () : " user " + user) + ", "
             + opts.root + (opts.writable ? " read/write" : " read only"));
    try {
        dap::FalServer fal (link, opts);
        fal.run ();
    } catch (const std::exception &e) {
        log (40, e.what ());
        link.disconnect ();
        return 1;
    }
    return 0;
}
