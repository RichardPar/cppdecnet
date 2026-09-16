// decnet/datalink/datalink.h -- the datalink layer.
//
// Port of DatalinkLayer, Datalink and Port from datalink.py, plus the work
// items for the point to point state machine.
//
// An upper layer creates a Port on a Datalink.  Received frames and status
// changes are delivered as work items on the node queue.

#ifndef DECNET_DATALINK_DATALINK_H
#define DECNET_DATALINK_DATALINK_H

#include "decnet/common/element.h"
#include "decnet/common/types.h"
#include "decnet/common/work.h"
#include "decnet/nice/nml.h"

#include <map>
#include <memory>
#include <string>

namespace decnet {

class Config;
struct CircuitConfig;

namespace datalink {

class Datalink;
class Port;

// ------------------------------------------------------------ work items

// Notification that a datalink has come up or gone down.  Port of
// datalink.DlStatus.
class DlStatus : public Work {
public:
    enum class Status { up, down };

    DlStatus (Element *owner, Status s) noexcept : Work (owner), status_ (s) {}

    const char *kind () const noexcept override { return "DlStatus"; }
    Status status () const noexcept { return status_; }
    bool is_up () const noexcept { return status_ == Status::up; }

private:
    Status status_;
};

// Requests from the layer above.  These are work items rather than direct
// calls so that they serialise cleanly with the state machine.
class Start : public Work {
public:
    explicit Start (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "Start"; }
};

class Stop : public Work {
public:
    explicit Stop (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "Stop"; }
};

// Protocol restart: remote restart notification, as DNA calls it.  Any
// underlying connection is kept where the datalink can manage it.
class Restart : public Work {
public:
    explicit Restart (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "Restart"; }
};

// Internal: start over with fresh connections.  "now" skips the holdoff,
// which is right when the reason for reconnecting was itself a timeout.
class Reconnect : public Work {
public:
    Reconnect (Element *o, bool now = false) noexcept : Work (o), now_ (now) {}
    const char *kind () const noexcept override { return "Reconnect"; }
    bool now () const noexcept { return now_; }

private:
    bool now_;
};

// Internal: the receive thread has connected, or has exited.
class Connected : public Work {
public:
    explicit Connected (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "Connected"; }
};

class ThreadExit : public Work {
public:
    explicit ThreadExit (Element *o) noexcept : Work (o) {}
    const char *kind () const noexcept override { return "ThreadExit"; }
};

// -------------------------------------------------------------- counters

// The subset of the architected counters PyDECnet keeps for a point to
// point circuit.
struct PtpCounters {
    std::uint64_t bytes_sent = 0, pkts_sent = 0;
    std::uint64_t bytes_recv = 0, pkts_recv = 0;

    void reset () noexcept { *this = PtpCounters {}; }
};

// ------------------------------------------------------------------ Port
//
// An upper layer's handle on a datalink.  Port of datalink.Port.
class Port : public Element {
public:
    Port (Datalink *dl, Element *owner) noexcept;

    Element *owner () const noexcept { return owner_; }

    // Ask the datalink to start, stop or restart.  Each queues a work item.
    void open ();
    void close ();
    void restart ();

    virtual void send (Bytes msg) = 0;

    // False when the datalink cannot detect a remote restart (Multinet over
    // UDP).
    virtual bool start_works () const noexcept { return true; }

    // Add circuit type and traffic counters to a NICE circuit reply.  Port of
    // Port.nice_read_port.
    virtual void nice_read_port (const nice::NiceRequest &req,
                                 nice::NiceReply &r);

    void dispatch (Work &) override {}

protected:
    Datalink *datalink_;
    Element  *owner_;
};

// -------------------------------------------------------------- Datalink
//
// Abstract base for one circuit.  Port of datalink.Datalink.
class Datalink : public Element {
public:
    Datalink (Element *owner, std::string name) noexcept;

    const std::string &name () const noexcept { return name_; }

    // Called at node start and stop.  No-ops for point to point links, which
    // are controlled through the port.
    virtual void open () {}
    virtual void close () {}

    // Create the single port for this circuit.  Throws if one exists.
    virtual Port *create_port (Element *owner) = 0;

    // True if MOP should run on this kind of circuit.
    virtual bool use_mop () const noexcept { return false; }

    virtual const PtpCounters *counters () const noexcept { return nullptr; }

    // NICE circuit type code: 6 for Ethernet, 0 for DDCMP point to point.
    virtual unsigned nice_type () const noexcept { return 0; }

    // The NICE line protocol code, which is the same list with a different
    // number for the DDCMP variants.  Port of nice_protocol.
    virtual unsigned nice_protocol () const noexcept { return 0; }

    // Answer a line read for this circuit.  Port of
    // Datalink.nice_read_line.
    virtual void nice_read_line (const nice::NiceRequest &req,
                                 nice::ReplyDict &resp);

protected:
    std::string name_;
};

// --------------------------------------------------------- DatalinkLayer
//
// Container for the circuits.  Port of datalink.DatalinkLayer.
class DatalinkLayer : public Element {
public:
    DatalinkLayer (Element *owner, const Config &config);
    ~DatalinkLayer () override;

    void start ();
    void stop ();

    Datalink *circuit (const std::string &name) const;

    // Every circuit, in configuration order, so callers can iterate
    // without the map's ordering getting in the way.
    const std::vector<Datalink *> &circuits () const noexcept
    { return order_; }

    void dispatch (Work &) override {}

    // Answer the line half of a NICE read.  Port of
    // DatalinkLayer.nice_read.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp);

    // Build one circuit from its configuration line.  Logs and returns null
    // on failure.
    static std::unique_ptr<Datalink> create (Element *owner,
                                             const CircuitConfig &c);

private:
    std::map<std::string, std::unique_ptr<Datalink>> circuits_;
    std::vector<Datalink *>                          order_;
};

}   // namespace datalink
}   // namespace decnet

#endif  // DECNET_DATALINK_DATALINK_H
