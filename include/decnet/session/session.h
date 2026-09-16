// decnet/session/session.h -- session control.
//
// Port of session.py.  Maintains the object database and hands inbound
// connections to the requested object.
//
// PORT: access control data is carried but not verified (PyDECnet uses
// PAM).

#ifndef DECNET_SESSION_SESSION_H
#define DECNET_SESSION_SESSION_H

#include <chrono>
#include "decnet/common/element.h"
#include "decnet/nsp/nsp.h"
#include "decnet/session/packets.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace decnet {
class Config;
}

namespace decnet::session {

class Session;
class SessionConnection;

// What an application implements.  One instance is created per inbound
// connection, so it can hold per-conversation state.
class Application {
public:
    virtual ~Application () = default;

    // A connection has arrived.  Answer with accept() or reject() on it.
    virtual void connect_received (SessionConnection &c, ByteView data) = 0;

    // A complete message, already reassembled by NSP.
    virtual void data_received (SessionConnection &c, ByteView data) = 0;

    // Out of band data.  Most applications do not use it, so this has a
    // default rather than being pure.
    virtual void interrupt_received (SessionConnection &, ByteView) {}

    // The link is gone.  The connection is already closed.
    virtual void disconnected (SessionConnection &c, unsigned reason) {}
};

// An application's handle on one conversation.
class SessionConnection {
public:
    SessionConnection (Session *parent, nsp::Connection *conn) noexcept
        : parent_ (parent), conn_ (conn) {}

    void accept (Bytes data = {}, std::uint8_t fcopt = 0);
    void reject (unsigned reason = 0, Bytes data = {});
    void send_data (Bytes data);
    void disconnect (unsigned reason = 0, Bytes data = {});

    // Send out of band data, at most 16 bytes.  False if the link is not
    // running or the far end has not released another interrupt.
    bool interrupt (Bytes data);
    bool can_interrupt () const noexcept;

    bool running () const noexcept;
    nsp::Connection *nsp_connection () const noexcept { return conn_; }

    // Who is at the other end, and what they asked for.
    Nodeid remote () const noexcept;
    const EndUser &destination () const noexcept { return dstname_; }
    const EndUser &source () const noexcept { return srcname_; }

private:
    friend class Session;

    Session         *parent_;
    nsp::Connection *conn_;
    EndUser          dstname_, srcname_;
};

// The object database entry: how to make an application for a connection.
using ApplicationFactory = std::function<std::unique_ptr<Application> ()>;

struct Object {
    std::uint8_t       number = 0;
    std::string        name;
    ApplicationFactory factory;
};

class Session : public Element, public nsp::SessionControl {
public:
    Session (Element *parent, const Config &config);
    ~Session () override;

    void start ();
    void stop ();

    // Register an object.  Either the number or the name may be zero or
    // empty, but not both -- an object nothing can ask for is useless.
    void add_object (std::uint8_t number, std::string name,
                     ApplicationFactory factory);

    const Object *find_object (std::uint8_t number) const;
    const Object *find_object (const std::string &name) const;
    std::size_t object_count () const noexcept { return objects_.size (); }

    // Open a connection to an object on another node.  The application
    // handles the reply through the factory-made instance.
    SessionConnection *connect (Nodeid dest, EndUser dstname, EndUser srcname,
                                Bytes connectdata,
                                std::unique_ptr<Application> app);

    // Same, with access control fields.
    SessionConnection *connect (Nodeid dest, ConnectData cd,
                                std::unique_ptr<Application> app);

    // ------------------------------------------- the NSP SessionControl API
    void connect_received (nsp::Connection &c, ByteView payload) override;
    void connect_confirmed (nsp::Connection &c, ByteView data) override;
    void connect_rejected (nsp::Connection &c, unsigned reason,
                           ByteView data) override;
    void data_received (nsp::Connection &c, ByteView data) override;
    void interrupt_received (nsp::Connection &c, ByteView data) override;
    void disconnected (nsp::Connection &c, unsigned reason,
                       ByteView data) override;

    void dispatch (Work &) override {}

private:
    // Everything known about one live conversation.
    struct Live {
        std::unique_ptr<SessionConnection> conn;
        std::unique_ptr<Application>       app;
    };

    Live *find_live (nsp::Connection &c);

    // Retire a conversation.  Moved aside rather than destroyed, since this is
    // called from inside a callback into the application.
    void retire (nsp::Connection &c);

    std::vector<Object>                          objects_;
    std::map<std::uint8_t, std::size_t>          by_number_;
    std::map<std::string, std::size_t>           by_name_;
    std::map<nsp::Connection *, Live>            live_;
    // Finished conversations, freed after a grace period, as NSP does with
    // closed connections.
    struct Retired {
        Live                                  live;
        std::chrono::steady_clock::time_point when;
    };
    std::vector<Retired> finished_;

public:
    // As in NSP, and for the same reason: the tests need to see how many
    // are held and to shorten the wait.
    std::size_t finished_count () const noexcept { return finished_.size (); }
    void set_finished_grace (std::chrono::seconds g) noexcept
    { finished_grace_ = g; }

private:

    std::chrono::seconds finished_grace_ { 60 };

    void sweep_finished ();
};

// Loopback application, object 25.  Port of decnet/modules/mirror.py.
std::unique_ptr<Application> make_mirror ();

// Register the objects that are enabled by default.
void add_default_objects (Session &s);

}   // namespace decnet::session

#endif  // DECNET_SESSION_SESSION_H
