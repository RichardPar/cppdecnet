// decnet/session/process.h -- objects run as separate processes.
//
// Port of session.ProcessConnector.  An object declared with --file is run
// as a child process.  Session control sends JSON lines on its stdin, the
// program replies on stdout, and stderr goes to the log.  Compatible with
// PyDECnet applications.
//
// One process per connection.

#ifndef DECNET_SESSION_PROCESS_H
#define DECNET_SESSION_PROCESS_H

#include "decnet/common/json.h"
#include "decnet/session/session.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace decnet {
class Node;
}

namespace decnet::session {

class ProcessApplication : public Application {
public:
    ProcessApplication (Node *node, std::string program,
                        std::vector<std::string> arguments);
    ~ProcessApplication () override;

    void connect_received (SessionConnection &c, ByteView data) override;
    void data_received (SessionConnection &c, ByteView data) override;
    void interrupt_received (SessionConnection &c, ByteView data) override;
    void disconnected (SessionConnection &c, unsigned reason) override;

    // Whether the program is running.  A program that fails to start is
    // reported as "no such object".
    bool started () const noexcept { return pid_ > 0; }

private:
    bool spawn ();
    void shutdown ();

    // Send one message to the program.
    void send (const json::Object &o);

    // Reader threads.  They post callbacks to the node thread.
    void read_stdout ();
    void read_stderr ();

    // Act on one request from the program.  Runs on the node thread.
    void handle (const json::Object &req);

    Node                    *node_;
    std::string              program_;
    std::vector<std::string> arguments_;

    ::pid_t pid_ = -1;
    int     to_child_ = -1;      // its standard input
    int     from_child_ = -1;    // its standard output
    int     child_log_ = -1;     // its standard error

    std::thread       out_thread_, err_thread_;
    std::atomic<bool> stopping_ { false };

    SessionConnection *conn_ = nullptr;
    std::int64_t       handle_ = 0;
};

// Build a factory for an object implemented by a program.
ApplicationFactory process_application (Node *node, std::string program,
                                        std::vector<std::string> arguments);

}   // namespace decnet::session

#endif  // DECNET_SESSION_PROCESS_H
