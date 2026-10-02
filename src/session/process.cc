#include "decnet/session/process.h"

#include "decnet/common/logging.h"
#include "decnet/node.h"

#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace decnet::session {

namespace {

// Handles are opaque to the program: it echoes back whatever we send.  A
// counter keeps them distinct in the log.
std::int64_t next_handle ()
{
    static std::atomic<std::int64_t> counter { 1 };
    return counter.fetch_add (1);
}

// The pipes to the program are file descriptors on both systems: on
// Windows they are CRT descriptors wrapped round the pipe handles.
#ifdef _WIN32
int fd_read (int fd, void *buf, unsigned len) { return ::_read (fd, buf, len); }
int fd_write (int fd, const void *buf, unsigned len) { return ::_write (fd, buf, len); }
void fd_close (int fd) { ::_close (fd); }
#else
ssize_t fd_read (int fd, void *buf, std::size_t len) { return ::read (fd, buf, len); }
ssize_t fd_write (int fd, const void *buf, std::size_t len) { return ::write (fd, buf, len); }
void fd_close (int fd) { ::close (fd); }
#endif

// Read one line from a file descriptor, unbuffered.  Returns false at end
// of file.
bool read_line (int fd, std::string &out)
{
    out.clear ();
    char c;
    for (;;) {
        auto n = fd_read (fd, &c, 1);
        if (n == 0) return !out.empty ();
        if (n < 0) {
            if (errno == EINTR) continue;
            return !out.empty ();
        }
        if (c == '\n') {
            // A program on Windows writes its stdout in text mode.
            if (!out.empty () && out.back () == '\r') out.pop_back ();
            return true;
        }
        out += c;
    }
}

#ifdef _WIN32

// Quote one argument so CommandLineToArgvW, and so the C runtime of the
// program, hands it back unchanged.
void append_quoted (std::string &cmd, const std::string &arg)
{
    if (!cmd.empty ()) cmd += ' ';
    if (!arg.empty () && arg.find_first_of (" \t\n\v\"") == std::string::npos) {
        cmd += arg;
        return;
    }
    cmd += '"';
    for (std::size_t i = 0; ; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size () && arg[i] == '\\') { ++slashes; ++i; }
        if (i == arg.size ()) {
            cmd.append (slashes * 2, '\\');     // before the closing quote
            break;
        }
        if (arg[i] == '"') {
            cmd.append (slashes * 2 + 1, '\\');
            cmd += '"';
        } else {
            cmd.append (slashes, '\\');
            cmd += arg[i];
        }
    }
    cmd += '"';
}

std::string last_error_text ()
{
    DWORD err = ::GetLastError ();
    char buf[256] = "";
    DWORD n = ::FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM
                                | FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, err, 0, buf, sizeof buf, nullptr);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.'))
        buf[--n] = '\0';
    return n ? std::string (buf) : "error " + std::to_string (err);
}

#endif

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
    // Close stdin to tell the program to exit.
    if (to_child_ >= 0) { fd_close (to_child_); to_child_ = -1; }
    if (out_thread_.joinable ()) out_thread_.join ();
    if (err_thread_.joinable ()) err_thread_.join ();
    if (from_child_ >= 0) { fd_close (from_child_); from_child_ = -1; }
    if (child_log_ >= 0)  { fd_close (child_log_); child_log_ = -1; }
#ifdef _WIN32
    if (process_) {
        // As below: it should be gone already, so only force it if not.
        if (::WaitForSingleObject (process_, 0) == WAIT_TIMEOUT) {
            ::TerminateProcess (process_, 1);
            ::WaitForSingleObject (process_, INFINITE);
        }
        DWORD status = 0;
        ::GetExitCodeProcess (process_, &status);
        DN_TRACE ("object {} exited with status {}", program_, status);
        ::CloseHandle (process_);
        process_ = nullptr;
        pid_ = -1;
    }
#else
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
#endif
}

#ifdef _WIN32

bool ProcessApplication::spawn ()
{
    // Three pipes; only the child's ends are inheritable.
    SECURITY_ATTRIBUTES sa { sizeof sa, nullptr, TRUE };
    HANDLE in_r = nullptr, in_w = nullptr;
    HANDLE out_r = nullptr, out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;
    auto close_all = [&] {
        for (HANDLE h : { in_r, in_w, out_r, out_w, err_r, err_w })
            if (h) ::CloseHandle (h);
    };
    if (!::CreatePipe (&in_r, &in_w, &sa, 0)
        || !::CreatePipe (&out_r, &out_w, &sa, 0)
        || !::CreatePipe (&err_r, &err_w, &sa, 0)) {
        DN_ERROR ("cannot make pipes for object {}: {}", program_,
                  last_error_text ());
        close_all ();
        return false;
    }
    ::SetHandleInformation (in_w, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation (out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation (err_r, HANDLE_FLAG_INHERIT, 0);

    // Run .py files with the Python interpreter, as PyDECnet does.  Windows
    // installs it as python, not python3.
    std::string cmd;
    if (program_.size () > 3
        && program_.compare (program_.size () - 3, 3, ".py") == 0) {
        const char *py = ::getenv ("DN_PYTHON");
        append_quoted (cmd, py ? py : "python");
    }
    append_quoted (cmd, program_);
    for (const std::string &a : arguments_) append_quoted (cmd, a);

    // Pass the child these three handles and nothing else.  Sockets and
    // files are inheritable by default on Windows, and one held open by an
    // object would keep a port bound or a file locked; this is what
    // close-on-exec does on POSIX.
    HANDLE inherit[] = { in_r, out_w, err_w };
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList (nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf (attr_size);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (attr_buf.data ());
    if (!::InitializeProcThreadAttributeList (attrs, 1, 0, &attr_size)
        || !::UpdateProcThreadAttribute (attrs, 0,
                                         PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         inherit, sizeof inherit,
                                         nullptr, nullptr)) {
        DN_ERROR ("cannot run object {}: {}", program_, last_error_text ());
        close_all ();
        return false;
    }

    STARTUPINFOEXA si {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError  = err_w;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi {};
    // A new process group, so Ctrl-C at the daemon's console does not also
    // hit the object; the counterpart of setsid.
    BOOL ok = ::CreateProcessA (nullptr, cmd.data (), nullptr, nullptr, TRUE,
                                CREATE_NEW_PROCESS_GROUP
                                | EXTENDED_STARTUPINFO_PRESENT,
                                nullptr, nullptr, &si.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList (attrs);
    if (!ok) {
        DN_ERROR ("cannot run object {}: {}", program_, last_error_text ());
        close_all ();
        return false;
    }
    ::CloseHandle (pi.hThread);
    ::CloseHandle (in_r);
    ::CloseHandle (out_w);
    ::CloseHandle (err_w);

    process_    = pi.hProcess;
    pid_        = static_cast<long> (pi.dwProcessId);
    to_child_   = ::_open_osfhandle (reinterpret_cast<intptr_t> (in_w),
                                     _O_WRONLY | _O_BINARY);
    from_child_ = ::_open_osfhandle (reinterpret_cast<intptr_t> (out_r),
                                     _O_RDONLY | _O_BINARY);
    child_log_  = ::_open_osfhandle (reinterpret_cast<intptr_t> (err_r),
                                     _O_RDONLY | _O_BINARY);

    DN_DEBUG ("started object {} as pid {}", program_, pid_);
    out_thread_ = std::thread ([this] { read_stdout (); });
    err_thread_ = std::thread ([this] { read_stderr (); });
    return true;
}

#else

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
    // Close-on-exec pipe to report exec failure: the child writes errno, and a
    // successful exec closes it.
    if (::pipe (exec_pipe) < 0) {
        ::close (in_pipe[0]); ::close (in_pipe[1]);
        ::close (out_pipe[0]); ::close (out_pipe[1]);
        ::close (err_pipe[0]); ::close (err_pipe[1]);
        return false;
    }
    ::fcntl (exec_pipe[1], F_SETFD, FD_CLOEXEC);

    // Run .py files with the Python interpreter, as PyDECnet does.
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

    // Wait for exec: EOF on success, errno on failure.
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

#endif

void ProcessApplication::send (const json::Object &o)
{
    if (to_child_ < 0) return;
    std::string line = o.encode ();
    line += '\n';
    DN_TRACE ("to object {}: {}", program_, o.encode ());
    std::size_t off = 0;
    while (off < line.size ()) {
        auto n = fd_write (to_child_, line.data () + off,
                           static_cast<unsigned> (line.size () - off));
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
        // Log record: level, message and optional args for {} placeholders.
        // Anything else is logged as a plain message.
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
    // Access control, as PyDECnet's DictConnector passes it: only if sent.
    if (!c.username ().empty ()) o.set ("username", c.username ());
    if (!c.password ().empty ()) o.set ("password", c.password ());
    if (!c.account ().empty ())  o.set ("account", c.account ());
    // Extensions to PyDECnet's message, for programs that check access
    // themselves: proxy requests, and the requesting node by name.
    if (c.proxy ()) o.set ("proxy", true);
    if (node_)
        if (const Nodeinfo *n = node_->find_node (c.remote (), false);
            n && !n->name.empty ())
            o.set ("nodename", n->name);
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

void ProcessApplication::run_state (SessionConnection &c)
{
    // PyDECnet's connectors wait for this after accepting: FAL, for one,
    // treats any other message as the link failing.
    conn_ = &c;
    json::Object o;
    o.set ("handle", handle_);
    o.set ("data", "");
    o.set ("type", "runstate");
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
        // PyDECnet's reject carries no reason and so is always 0, "rejected
        // by object".  A program that checks access control itself needs 34,
        // "access control rejected", which is what a user is told.
        conn_->reject (static_cast<unsigned> (req.num ("reason", 0)),
                       std::move (data));
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
