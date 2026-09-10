// decnet/mop/mop.h -- the MOP layer.
//
// Port of the handler classes in mop.py.  One MopCircuit per Ethernet
// circuit, each with a system ID handler on protocol 60-01 and a loopback
// handler on 90-00.
//
// MOP is what makes a node visible to maintenance tools: NCP SHOW MODULE
// CONFIGURATOR lists what a node has heard, and NCP LOOP CIRCUIT uses the
// loopback protocol.  It runs only on broadcast circuits.
//
// PORT: the console carrier, both client and server, is not here.  Its
// messages parse, but reserving a console and carrying a terminal session
// over it is a state machine of its own.  Load and dump are not here
// either, and pydecnet does not implement them.

#ifndef DECNET_MOP_MOP_H
#define DECNET_MOP_MOP_H

#include "decnet/common/element.h"
#include "decnet/common/timers.h"
#include "decnet/datalink/bc.h"
#include "decnet/mop/packets.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace decnet {
class Config;
namespace datalink { class DatalinkLayer; }
}

namespace decnet::mop {

class MopCircuit;

// What we have heard from another station.
struct HeardSystem {
    Macaddr                               address;
    SysId                                 sysid;
    std::chrono::steady_clock::time_point last_heard;
};

// System ID on one circuit: announce ourselves periodically, answer
// requests, and remember what others say.  Port of mop.SysIdHandler.
class SysIdHandler : public Element, public Timer {
public:
    SysIdHandler (MopCircuit *parent, datalink::BcPort *port);

    void start ();
    void stop ();

    void dispatch (Work &w) override;
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

    // Send our identity to one address, echoing a receipt number back.
    void send_id (Macaddr dest, std::uint16_t receipt);

    // Ask a station who it is.
    void request_id (Macaddr dest, std::uint16_t receipt = 0);

    const std::map<std::string, HeardSystem> &heard () const noexcept
    { return heard_; }

private:
    void send_counters (Macaddr dest, std::uint16_t receipt);

    // How long until we announce ourselves again.  Randomised between
    // eight and twelve minutes, so that a room full of nodes does not
    // synchronise on one instant.
    double next_id_delay () const;

    MopCircuit                        *parent_;
    datalink::BcPort                  *port_;
    std::map<std::string, HeardSystem> heard_;
    std::chrono::steady_clock::time_point started_;
};

// The loopback protocol.  A message carries a skip count and a list of
// functions; a station asked to forward bumps the count and passes it on,
// so the reply retraces the path it came by.  Port of mop.LoopHandler.
class LoopHandler : public Element {
public:
    LoopHandler (MopCircuit *parent, datalink::BcDatalink *dl);

    void dispatch (Work &w) override;

    // Send a loop message to one station and expect it back.
    void loop (Macaddr dest, Bytes payload);

    std::uint64_t replies () const noexcept { return replies_; }
    const Bytes &last_reply () const noexcept { return last_reply_; }

private:
    MopCircuit       *parent_;
    datalink::BcPort *port_;
    std::uint64_t     replies_ = 0;
    std::uint16_t     receipt_ = 0;
    Bytes             last_reply_;
};

// MOP on one circuit.
class MopCircuit : public Element {
public:
    MopCircuit (Element *parent, std::string name,
                datalink::BcDatalink *dl);

    const std::string &name () const noexcept { return name_; }
    datalink::BcDatalink *datalink () const noexcept { return datalink_; }

    void start ();
    void stop ();

    SysIdHandler *sysid () const noexcept { return sysid_.get (); }
    LoopHandler *loop () const noexcept { return loop_.get (); }

    // Traffic on the system id port arrives here and is passed on; see
    // the constructor for why.
    void dispatch (Work &w) override;

private:
    std::string                   name_;
    datalink::BcDatalink         *datalink_;
    std::unique_ptr<SysIdHandler> sysid_;
    std::unique_ptr<LoopHandler>  loop_;
};

// The MOP layer: a container for the per-circuit objects.  Port of mop.Mop.
class Mop : public Element {
public:
    Mop (Element *parent, const Config &config);
    ~Mop () override;

    void start ();
    void stop ();

    MopCircuit *circuit (const std::string &name) const;
    const std::vector<MopCircuit *> &circuits () const noexcept
    { return order_; }

    void dispatch (Work &) override {}

private:
    std::map<std::string, std::unique_ptr<MopCircuit>> circuits_;
    std::vector<MopCircuit *>                          order_;
};

}   // namespace decnet::mop

#endif  // DECNET_MOP_MOP_H
