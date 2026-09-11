// decnet/nsp/nsp.h -- NSP logical links.
//
// Port of nsp.py.  NSP turns the datagram service routing provides into
// ordered logical links: it assigns link addresses, runs the connect and
// disconnect handshakes, numbers and acknowledges data segments, and
// reassembles messages that had to be split.
//
// This state machine is smaller than the spec's.  The NSP spec models
// session control as polling NSP for things it needs to hear, so it needs
// a state for each "waiting to be polled".  Here, as in pydecnet, session
// control is told rather than polled, so the O, DN, RJ, NC, NR, DRC, CN,
// DIC and DR states do not exist.
//
// Flow control here is outbound only, which is what pydecnet does too.
// Our own connect message asks for SVC_NONE, so a peer sends to us without
// credit; what the peer asks for in its connect message governs what we
// send, and we honour segment and message modes and the link service
// messages that carry credit.  A window of qmax unacknowledged segments
// applies whatever the mode.
//
// A link has two subchannels.  Data carries the ordered byte stream;
// "other" carries interrupt messages and the link service messages that
// grant credit.  Each has its own sequence numbers, and an acknowledgement
// can refer to either: a plain one refers to the subchannel the packet
// arrived on, a "cross" one to the other.  route_ack does that.
//
// PORT: still to do --
//  * requesting flow control on our own inbound data, which needs us to
//    send link service messages as a receiver.
//  * Phase II connections, which have no connect acknowledgement.

#ifndef DECNET_NSP_NSP_H
#define DECNET_NSP_NSP_H

#include "decnet/common/element.h"
#include "decnet/common/statemachine.h"
#include "decnet/common/timers.h"
#include "decnet/nice/nml.h"
#include "decnet/nsp/packets.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>

namespace decnet {
class Config;
namespace routing { class BaseRouter; }
}

namespace decnet::nsp {

class NSP;
class Connection;

// Why a connection ended.  The values are the reason codes that travel in
// disconnect messages.
enum DiscReason : std::uint16_t {
    DISC_NORMAL   = 0,
    DISC_NO_RES   = 1,
    DISC_NO_LINK  = 41,
    DISC_COMPLETE = 42,
    DISC_OBJ_FAIL = 38,
    DISC_UNREACH  = 39
};

// What the layer above NSP implements.  Session control will be the real
// one; the tests supply their own.
class SessionControl {
public:
    virtual ~SessionControl () = default;

    // An inbound connection arrived.  Answer with accept() or reject().
    virtual void connect_received (Connection &c, ByteView payload) = 0;

    // Our outbound connection was accepted, or was not.
    virtual void connect_confirmed (Connection &c, ByteView data) = 0;
    virtual void connect_rejected (Connection &c, unsigned reason,
                                   ByteView data) = 0;

    // A complete message arrived, already reassembled.
    virtual void data_received (Connection &c, ByteView data) = 0;

    // An interrupt message arrived.  Not every layer above cares, so this
    // has a default rather than being pure.
    virtual void interrupt_received (Connection &, ByteView) {}

    // The link is gone.
    virtual void disconnected (Connection &c, unsigned reason,
                               ByteView data) = 0;
};

// One end of a logical link.
class Connection : public Element, public StateMachine<Connection> {
public:
    static constexpr const char *class_name = "Connection";

    // Built by NSP, never directly.
    Connection (NSP *parent, std::uint16_t srcaddr, Nodeid dest);

    std::uint16_t srcaddr () const noexcept { return srcaddr_; }
    std::uint16_t dstaddr () const noexcept { return dstaddr_; }
    Nodeid dest () const noexcept { return dest_; }
    unsigned phase () const noexcept { return cphase_; }
    std::uint16_t segsize () const noexcept { return segsize_; }
    bool running () const noexcept;
    bool closed () const noexcept;

    // Flow control state, for tests and monitoring.
    std::uint8_t flow_control () const noexcept { return flow_; }
    std::size_t queued () const noexcept { return txq_.size (); }
    std::size_t in_flight () const noexcept;
    std::size_t out_of_order () const noexcept { return ooo_.size (); }
    unsigned interrupt_credit () const noexcept
    { return int_max_msg_ >= int_next_msg_ ? int_max_msg_ - int_next_msg_ + 1
                                           : 0; }

    // ------------------------------------------- the session control API
    // fcopt is the flow control this end wants for data coming to it.
    // SVC_NONE, the default, is what pydecnet asks for.
    void accept (Bytes data = {}, std::uint8_t fcopt = SVC_NONE);
    void reject (unsigned reason = 0, Bytes data = {});
    void send_data (Bytes data);
    void disconnect (unsigned reason = 0, Bytes data = {});

    // Send an interrupt message, at most 16 bytes.  Returns false if the
    // link is not running, the data is too long, or the far end has not
    // given us permission to send another.  A DECnet node typically allows
    // one at a time, so the usual pattern is one interrupt, then wait for
    // the credit that its handling releases.
    bool send_interrupt (Bytes data);

    // Is an interrupt allowed right now?
    bool can_interrupt () const noexcept;

    // ------------------------------------------------------------ states
    State s0 (Work &w);          // never used; a connection starts elsewhere
    State ci (Work &w);          // connect initiate sent
    State cd (Work &w);          // connect initiate acknowledged
    State cr (Work &w);          // inbound connect, awaiting a decision
    State cc (Work &w);          // connect confirm sent
    State run (Work &w);         // running
    State di (Work &w);          // disconnect initiate sent
    State cl (Work &w);          // closed

    bool validate (Work &w) { return true; }
    void dispatch (Work &w) override { StateMachine<Connection>::dispatch (w); }
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }
    std::string statename () const override;

private:
    friend class NSP;

    // Start the two halves of a connection.
    void start_outbound (Bytes payload);
    void start_inbound (const ConnInit &pkt);

    // Feed a received packet in.  NSP has already mapped it to us.
    void receive (const NspPacketBase &pkt);

    void send (const NspPacketBase &pkt);
    void send_ack ();
    void set_phase (std::uint32_t info);

    // Data segments: send one message as one or more segments, and put
    // arriving segments back together.
    void send_segments (const Bytes &data);
    void handle_data (const DataSeg &seg);
    void handle_interrupt (const IntMsg &msg);
    void handle_link_service (const LinkSvcMsg &ls);
    void process_ack (Seq num);
    void process_int_ack (Seq num);

    // Apply the acknowledgement fields of a packet.  on_data says which
    // subchannel the packet itself belongs to; a cross acknowledgement
    // refers to the other one.
    void route_ack (const std::optional<AckNum> &a, bool on_data);

    // Deliver one in-sequence segment to the assembly buffer, and hand a
    // finished message up.
    void accept_segment (const DataSeg &seg);

    // Anything unacknowledged goes out again.
    void retransmit ();
    void arm_timer (double seconds);

    State close (unsigned reason, ByteView data, bool tell_session);

    SessionControl *session () const;

    // One packet on the transmit queue, sent or waiting for flow control.
    //
    // segnum and msgnum are absolute counters rather than the twelve bit
    // sequence number on the wire.  The window and credit comparisons are
    // then plain arithmetic, with no modulo wraparound to get wrong.
    struct TxEntry {
        Seq      seq;
        unsigned segnum = 0;
        unsigned msgnum = 0;
        Bytes    frame;
        bool     sent = false;
        bool     is_data = false;
    };

    // May this entry go out now?  Port of Data_Subchannel.flow_ok.
    bool flow_ok (const TxEntry &e) const;

    // Walk the queue sending whatever flow control now allows, stopping at
    // the first entry it does not.  Port of send_blocked.
    void send_blocked ();

    NSP          *parent_;
    std::uint16_t srcaddr_;
    std::uint16_t dstaddr_ = 0;
    Nodeid        dest_;

    unsigned      rphase_ = 4;   // the far end's phase
    unsigned      cphase_ = 4;   // the lower of the two
    std::uint16_t segsize_ = 0;

    // Data subchannel.  Sequence numbers start at 1: the connect message
    // itself is number 0, which is what its acknowledgement refers to.
    Seq   next_send_ { 1 };
    Seq   highest_acked_ { 0 };
    Seq   next_expect_ { 1 };

    std::deque<TxEntry> txq_;
    unsigned next_segnum_ = 1;   // absolute counters, see TxEntry
    unsigned next_msgnum_ = 1;
    unsigned max_acked_seg_ = 0;

    // What the peer asked for in its connect message, and the credit it
    // has given us since.
    std::uint8_t flow_ = SVC_NONE;
    unsigned     max_seg_ = 0;   // segment mode: highest segment allowed
    unsigned     max_msg_ = 0;   // message mode: highest message allowed
    bool         xon_ = true;
    unsigned     qmax_ = 20;     // unacknowledged segments in flight

    // Segments received ahead of their turn, keyed by sequence number.
    std::map<std::uint16_t, DataSeg> ooo_;

    // The other subchannel.  Simpler than the data one: no segmentation,
    // and credit counted in whole messages, of which one is allowed to
    // start with.
    Seq                 int_next_send_ { 1 };
    Seq                 int_next_expect_ { 1 };
    std::deque<TxEntry> int_txq_;
    unsigned            int_next_msg_ = 1;
    unsigned            int_max_msg_ = 1;

    Bytes assembly_;             // segments received so far for one message
    bool  assembling_ = false;

    // The packet currently being processed, so each state can look at it
    // without every state function repeating the cast.
    const NspPacketBase *received_ = nullptr;

    double conn_timeout_ = 30.0;
    double inact_time_ = 300.0;
    double retransmit_time_ = 2.0;
    unsigned retries_ = 0;
    bool     timer_is_retransmit_ = false;
};

class NSP : public Element {
public:
    NSP (Element *parent, const Config &config);
    ~NSP () override;

    void start ();
    void stop ();

    void set_session_control (SessionControl *sc) noexcept { session_ = sc; }
    SessionControl *session_control () const noexcept { return session_; }

    // Open a connection to a remote node.  Returns null if no link
    // address is available.
    Connection *connect (Nodeid dest, Bytes payload);

    // A packet arrived from routing.  Port of NSP.dispatch, the receive
    // dispatcher of NSP 4.0.1 section 6.2.
    void deliver (Nodeid src, ByteView payload);

    void dispatch (Work &) override {}

    std::size_t connection_count () const noexcept { return by_addr_.size (); }

    // Every live connection, for network management and the monitoring
    // pages.
    const std::map<std::uint16_t, std::unique_ptr<Connection>> &
    connections () const noexcept { return by_addr_; }

    // Answer the part of a NICE read NSP knows about: the node entities,
    // since NSP is what knows which nodes have links to them.  Port of
    // NSP.nice_read.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp);
    Connection *find (std::uint16_t srcaddr) const;

    // How many logical links run to that node.  Network management reports
    // it as "Active links", and the monitoring page shows the same number.
    unsigned links_to (Nodeid dest) const;

    // Send an NSP packet to a node, through routing.
    void send_to (Nodeid dest, const Bytes &frame);

    unsigned nsp_version () const noexcept { return nspver_; }
    unsigned max_connections () const noexcept { return maxconns_; }
    unsigned qmax () const noexcept { return qmax_; }

private:
    friend class Connection;

    // Fill in one node's reply.  Port of NSP.read_node.
    void read_node (const nice::NiceRequest &req, Nodeid id,
                    nice::ReplyDict &resp, unsigned links);

    // Link address assignment.  Port of init_id/get_id/ret_id: addresses
    // are taken from one end of a circular list and returned to the other,
    // so a closed connection's address is not reused for as long as
    // possible -- which is what the Phase II spec requires of a node
    // talking to an intercept node.
    void init_ids ();
    bool get_id (std::uint16_t &out);
    void return_id (std::uint16_t id);

    void close_connection (Connection *c);

    routing::BaseRouter *routing_ = nullptr;
    SessionControl      *session_ = nullptr;
    unsigned             maxconns_ = 4095;
    unsigned             nspver_ = VER_PH4;
    unsigned             qmax_ = 20;

    std::deque<std::uint16_t> free_ids_;
    std::map<std::uint16_t, std::unique_ptr<Connection>> by_addr_;
    // Inbound connections are also indexed by who sent them and their own
    // address, so a retransmitted connect initiate finds the same link.
    std::map<std::pair<std::uint16_t, std::uint16_t>, Connection *> by_remote_;
    // Connections NSP has finished with.  A caller may still hold a
    // pointer to one, so they are kept rather than destroyed; each reports
    // itself closed.
    std::vector<std::unique_ptr<Connection>> closed_;
};

}   // namespace decnet::nsp

#endif  // DECNET_NSP_NSP_H
