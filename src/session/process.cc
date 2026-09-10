#include "decnet/session/process.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace decnet::session {

namespace {

// Handles are opaque to the program: it echoes back whatever we send.  A
// counter keeps them distinct in the log.
std::int64_t next_handle ()
{
    static std::atomic<std::int64_t> counter { 1 };
    return counter.fetch_add (1);
}

// Read one line from a file descriptor.  Returns false at end of file.
// Deliberately unbuffered: two threads read two different descriptors and
// a shared buffer would be one more thing to get wrong, and these lines
// are short.
bool read_line (int fd, std::string &out)
{
    out.clear ();
    char c;
    for (;;) {
        ssize_t n = ::read (fd, &c, 1);
        if (n == 0) return !out.empty ();
        if (n < 0) {
            if (errno == EINTR) continue;
            return !out.empty ();
        }
        if (c == '\n') return true;
        out += c;
    }
}

}   // namespace

ProcessApplication::ProcessApplication (Node *node, std::string program,
                                        std::vector<std::string> arguments)
    : node_ (node), program_ (std::move (program)),
      arguments_ (std::move (arguments)),
      handle_ (next_handle ())
{
}

ProcessApplication::~ProcessApplication ()
{
    shutdown ();
}

void ProcessApplication::shutdown ()
{
    stopping_.store (true);
    // Closing its input is how a program is told there is nothing more
    // coming; a well behaved one then exits, which ends our reader
    // threads.
    if (to_child_ >= 0) { ::close (to_child_); to_child_ = -1; }
    if (out_thread_.joinable ()) out_thread_.join ();
    if (err_thread_.joinable ()) err_thread_.join ();
    if (from_child_ >= 0) { ::close (from_child_); from_child_ = -1; }
    if (child_log_ >= 0)  { ::close (child_log_); child_log_ = -1; }
    if (pid_ > 0) {
        int status = 0;
        // It has had its input closed and both pipes drained, so this
        // should not block for long.
        if (::waitpid (pid_, &status, WNOHANG) == 0) {
            ::kill (pid_, SIGTERM);
            ::waitpid (pid_, &status, 0);
        }
        if (WIFSIGNALED (status))
            DN_DEBUG ("object {} exited on signal {}", program_,
                      WTERMSIG (status));
        else
            DN_TRACE ("object {} exited with status {}", program_,
                      WEXITSTATUS (status));
        pid_ = -1;
    }
}

bool ProcessApplication::spawn ()
{
    int in_pipe[2], out_pipe[2], err_pipe[2], exec_pipe[2];
    if (::pipe (in_pipe) < 0) return false;
    if (::pipe (out_pipe) < 0) {
        ::close (in_pipe[0]); ::close (in_pipe[1]);
        return false;
    }
    if (::pipe (err_pipe) < 0) {
        ::close (in_pipe[0]); ::close (in_pipe[1]);
        ::close (out_pipe[0]); ::close (out_pipe[1]);
        return false;
    }
    // A pipe purely to learn whether exec worked.  fork() succeeding says
    // nothing about that: exec fails in the child, which cannot tell the
    // parent any other way.  The child writes errno here and the descriptor
    // is closed by a successful exec, so the parent sees either an error
    // number or end of file.
    if (::pipe (exec_pipe) < 0) {
        ::close (in_pipe[0]); ::close (in_pipe[1]);
        ::close (out_pipe[0]); ::close (out_pipe[1]);
        ::close (err_pipe[0]); ::close (err_pipe[1]);
        return false;
    }
    ::fcntl (exec_pipe[1], F_SETFD, FD_CLOEXEC);

    // Build the argument list.  A Python file is run under an interpreter,
    // as pydecnet does, so that a program does not have to be executable
    // or carry a hash-bang line.
    std::vector<std::string> argv;
    if (program_.size () > 3
        && program_.compare (program_.size () - 3, 3, ".py") == 0) {
        const char *py = ::getenv ("DN_PYTHON");
        argv.push_back (py ? py : "python3");
    }
    argv.push_back (program_);
    for (const std::string &a : arguments_) argv.push_back (a);

    std::vector<char *> cargv;
    cargv.reserve (argv.size () + 1);
    for (std::string &a : argv) cargv.push_back (a.data ());
    cargv.push_back (nullptr);

    ::pid_t pid = ::fork ();
    if (pid < 0) {
        ::close (in_pipe[0]); ::close (in_pipe[1]);
        ::close (out_pipe[0]); ::close (out_pipe[1]);
        ::close (err_pipe[0]); ::close (err_pipe[1]);
        ::close (exec_pipe[0]); ::close (exec_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // The child.  Only async-signal-safe calls from here to exec.
        ::dup2 (in_pipe[0], STDIN_FILENO);
        ::dup2 (out_pipe[1], STDOUT_FILENO);
        ::dup2 (err_pipe[1], STDERR_FILENO);
        ::close (in_pipe[0]);  ::close (in_pipe[1]);
        ::close (out_pipe[0]); ::close (out_pipe[1]);
        ::close (err_pipe[0]); ::close (err_pipe[1]);
        ::close (exec_pipe[0]);
        // A new session, so a signal sent to the daemon's process group
        // does not also hit the object.
        ::setsid ();
        ::execvp (cargv[0], cargv.data ());
        // Only reached if exec failed.  Tell the parent which error it
        // was, then go away.
        int err = errno;
        ssize_t ignored = ::write (exec_pipe[1], &err, sizeof err);
        (void) ignored;
        ::_exit (127);
    }

    ::close (in_pipe[0]);
    ::close (out_pipe[1]);
    ::close (err_pipe[1]);
    ::close (exec_pipe[1]);

    // Wait for exec to succeed or fail.  This read ends either way: on
    // success the descriptor is closed by exec, on failure it carries the
    // error number.
    int exec_errno = 0;
    ssize_t got = ::read (exec_pipe[0], &exec_errno, sizeof exec_errno);
    ::close (exec_pipe[0]);
    if (got > 0) {
        int status = 0;
        ::waitpid (pid, &status, 0);
        ::close (in_pipe[1]); ::close (out_pipe[0]); ::close (err_pipe[0]);
        DN_ERROR ("cannot run object {}: {}", program_,
                  std::strerror (exec_errno));
        return false;
    }

    pid_        = pid;
    to_child_   = in_pipe[1];
    from_child_ = out_pipe[0];
    child_log_  = err_pipe[0];

    DN_DEBUG ("started object {} as pid {}", program_, pid_);
    out_thread_ = std::thread ([this] { read_stdout (); });
    err_thread_ = std::thread ([this] { read_stderr (); });
    return true;
}

void ProcessApplication::send (const json::Object &o)
{
    if (to_child_ < 0) return;
    std::string line = o.encode ();
    line += '\n';
    DN_TRACE ("to object {}: {}", program_, o.encode ());
    std::size_t off = 0;
    while (off < line.size ()) {
        ssize_t n = ::write (to_child_, line.data () + off, line.size () - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            DN_DEBUG ("write to object {} failed: {}", program_,
                      std::strerror (errno));
            return;
        }
        off += static_cast<std::size_t> (n);
    }
}

void ProcessApplication::read_stdout ()
{
    logging::set_thread_name (program_);
    std::string line;
    while (!stopping_.load () && read_line (from_child_, line)) {
        if (line.empty ()) continue;
        json::Object req;
        try {
            req = json::Object::parse (line);
        } catch (const json::ParseError &e) {
            DN_DEBUG ("object {} sent an unparsable request: {}", program_,
                      e.what ());
            continue;
        }
        DN_TRACE ("from object {}: {}", program_, line);
        // Act on it from the node thread, not this one.
        if (node_)
            node_->add_work (std::make_unique<CallbackWork> (
                [this, req] { handle (req); }));
    }
    DN_TRACE ("object {} stdout closed", program_);
}

void ProcessApplication::read_stderr ()
{
    logging::set_thread_name (program_ + " log");
    std::string line;
    while (!stopping_.load () && read_line (child_log_, line)) {
        if (line.empty ()) continue;
        // A log record is a JSON object with a level, a message and
        // optionally a list of arguments to substitute into it at {}
        // placeholders.  Anything else is a plain message.
        try {
            json::Object o = json::Object::parse (line);
            if (o.has ("message")) {
                std::int64_t level = o.num ("level", 10);
                std::string msg = o.format_message ();
                auto lvl = (level >= 40) ? logging::Level::error
                         : (level >= 30) ? logging::Level::warning
                         : (level >= 20) ? logging::Level::info
                                         : logging::Level::debug;
                logging::log (lvl, "{}: {}", program_, msg);
                continue;
            }
        } catch (const json::ParseError &) {
            // Not a log record; fall through.
        }
        DN_DEBUG ("{}: {}", program_, line);
    }
}

// --------------------------------------------- session control -> object

void ProcessApplication::connect_received (SessionConnection &c, ByteView data)
{
    conn_ = &c;
    if (!spawn ()) {
        DN_ERROR ("cannot start object {}", program_);
        c.reject (NO_OBJ);
        return;
    }
    json::Object o;
    o.set ("handle", handle_);
    o.set_bytes ("data", data);
    o.set ("type", "connect");
    o.set ("destination", c.remote ().str ());
    o.set ("srcuser", c.source ().str ());
    o.set ("dstuser", c.destination ().str ());
    send (o);
}

void ProcessApplication::data_received (SessionConnection &c, ByteView data)
{
    conn_ = &c;
    json::Object o;
    o.set ("handle", handle_);
    o.set_bytes ("data", data);
    o.set ("type", "data");
    send (o);
}

void ProcessApplication::interrupt_received (SessionConnection &c,
                                             ByteView data)
{
    conn_ = &c;
    json::Object o;
    o.set ("handle", handle_);
    o.set_bytes ("data", data);
    o.set ("type", "interrupt");
    send (o);
}

void ProcessApplication::disconnected (SessionConnection &c, unsigned reason)
{
    conn_ = &c;
    json::Object o;
    o.set ("handle", handle_);
    o.set ("data", "");
    o.set ("type", "disconnect");
    o.set ("reason", static_cast<std::int64_t> (reason));
    send (o);
    // The program is expected to exit now; closing its input tells it so.
    shutdown ();
    conn_ = nullptr;
}

// --------------------------------------------- object -> session control

void ProcessApplication::handle (const json::Object &req)
{
    std::string type = req.str ("type");
    if (type.empty ()) {
        DN_DEBUG ("object {} sent a request with no type", program_);
        return;
    }
    if (!conn_) {
        DN_DEBUG ("object {} sent {} with no connection", program_, type);
        return;
    }
    Bytes data = req.bytes ("data");

    if (type == "accept") {
        conn_->accept (std::move (data));
    } else if (type == "reject") {
        conn_->reject (0, std::move (data));
    } else if (type == "data") {
        conn_->send_data (std::move (data));
    } else if (type == "disconnect" || type == "abort") {
        // An application does not choose a reason; the architecture makes
        // it zero either way.
        conn_->disconnect (0, std::move (data));
    } else if (type == "interrupt") {
        if (!conn_->interrupt (std::move (data)))
            DN_DEBUG ("object {} interrupt refused: no credit, or the link "
                      "is not running", program_);
    } else {
        DN_DEBUG ("object {} sent an unknown request type {}", program_, type);
    }
}

ApplicationFactory process_application (Node *node, std::string program,
                                        std::vector<std::string> arguments)
{
    return [node, program, arguments] () -> std::unique_ptr<Application> {
        return std::make_unique<ProcessApplication> (node, program, arguments);
    };
}

}   // namespace decnet::session
