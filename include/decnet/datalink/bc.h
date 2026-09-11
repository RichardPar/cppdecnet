// decnet/datalink/bc.h -- broadcast (LAN) datalinks.
//
// Port of datalink.BcDatalink, BcPort and BcCounters.  A broadcast circuit
// differs from a point to point one in two ways that matter here: a frame
// carries a destination address, so the port has to say which addresses it
// wants; and several upper layers share one circuit, each taking a
// different protocol type -- routing on 60-03, MOP on 60-01.
//
// There is no state machine: a LAN is either there or it is not, and the
// routing sublayer above discovers its neighbours by listening rather than
// by handshaking.

#ifndef DECNET_DATALINK_BC_H
#define DECNET_DATALINK_BC_H

#include "decnet/common/socket.h"
#include "decnet/datalink/datalink.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

namespace decnet::datalink {

class BcDatalink;

// The DEC Ethernet protocol types.  Ports of common.MOPDLPROTO and
// ROUTINGPROTO.
inline constexpr std::uint16_t MOPDL_PROTO   = 0x6001;
inline constexpr std::uint16_t ROUTING_PROTO = 0x6003;

// The multicast addresses the routing layer uses.  Ports of the constants
// at the top of route_eth.py.
Macaddr all_routers ();
Macaddr all_endnodes ();

// Ethernet frame sizes.  A DEC padded frame carries a two byte length
// after the protocol type, so its payload is two bytes shorter.
inline constexpr std::size_t ETH_MTU = 1500;
inline constexpr std::size_t ETH_MIN_FRAME = 60;
inline constexpr std::size_t ETH_HDR_LEN = 14;

struct BcPortCounters {
    std::uint64_t bytes_sent = 0, pkts_sent = 0;
    std::uint64_t bytes_recv = 0, pkts_recv = 0;
    std::uint64_t mcbytes_recv = 0, mcpkts_recv = 0;
};

// One upper layer's use of a broadcast circuit: a protocol type, an
// individual address, and a set of multicast addresses it wants.
class BcPort : public Port {
public:
    BcPort (BcDatalink *dl, Element *owner, std::uint16_t proto,
            bool pad = true);

    std::uint16_t proto () const noexcept { return proto_; }

    Macaddr macaddr () const;
    void set_macaddr (Macaddr addr);

    void add_multicast (Macaddr addr);
    void remove_multicast (Macaddr addr);
    void set_promiscuous (bool on = true) { promisc_ = on; }

    // Would this port accept a frame sent to dest?
    bool accepts (Macaddr dest) const;

    // Send to a specific destination.  The Port::send inherited from the
    // point to point side has no destination and is not meaningful here.
    void send (Bytes msg, Macaddr dest);
    void send (Bytes) override;

    const BcPortCounters &counters () const noexcept { return counters_; }
    BcPortCounters &mutable_counters () noexcept { return counters_; }

    void nice_read_port (const nice::NiceRequest &req,
                         nice::NiceReply &r) override;

private:
    friend class BcDatalink;

    std::uint16_t     proto_;
    bool              pad_;
    Macaddr           macaddr_;
    std::set<Bytes>   multicast_;      // keyed by the six address bytes
    bool              promisc_ = false;
    BcPortCounters    counters_;
};

class BcDatalink : public Datalink {
public:
    BcDatalink (Element *owner, std::string name, bool random_address);

    bool use_mop () const noexcept override { return true; }

    // Ethernet, in both of NICE's numberings.
    unsigned nice_type () const noexcept override { return 6; }
    unsigned nice_protocol () const noexcept override { return 6; }

    void nice_read_line (const nice::NiceRequest &req,
                         nice::ReplyDict &resp) override;

    Macaddr hwaddr () const noexcept { return hwaddr_; }
    void set_hwaddr (Macaddr a) noexcept { hwaddr_ = a; }

    // A broadcast circuit has one port per protocol type, not one port
    // total, so this is the call the upper layers use.
    BcPort *create_bc_port (Element *owner, std::uint16_t proto,
                            bool pad = true);

    // The point to point spelling is not meaningful here.
    Port *create_port (Element *owner) override;

    // Put one frame on the wire.  Implemented by each transport.
    virtual void send_frame (const Bytes &frame) = 0;

    // A packet filter matching everything the current ports want: the
    // protocol types they registered, and the addresses they answer to.
    // A transport that can push a filter down to the kernel -- pcap can --
    // uses this so that a busy segment does not wake the receive thread
    // for every frame on it.  The software checks in receive_frame stay:
    // the kernel filter is an optimisation, not the rule.
    std::string filter_expression () const;

    // Called when a port's addresses or protocol change, so a transport
    // holding a kernel filter can rebuild it.
    virtual void filter_changed () {}

    // Nothing is addressed to a broadcast datalink itself: received frames
    // go to the port owners, and there is no state machine to drive.
    void dispatch (Work &) override {}

protected:
    // Called by the receive thread for each frame that arrives.  Splits
    // the header off and hands the payload to whichever port wants it.
    void receive_frame (ByteView frame);

    std::vector<std::unique_ptr<BcPort>> ports_;
    Macaddr hwaddr_;
};

// Build a DEC Ethernet frame: destination, source, protocol type, and --
// for the padded format DECnet uses -- a two byte little endian payload
// length.  Short frames are padded to the 60 byte minimum.
Bytes build_frame (Macaddr dest, Macaddr src, std::uint16_t proto,
                   ByteView payload, bool pad = true);

// The reverse: pull the addresses, protocol and payload out of a frame.
// Returns false if it is too short or its length field is inconsistent.
struct ParsedFrame {
    Macaddr       dest, src;
    std::uint16_t proto = 0;
    ByteView      payload;
};
bool parse_frame (ByteView frame, ParsedFrame &out, bool pad = true);

}   // namespace decnet::datalink

#endif  // DECNET_DATALINK_BC_H
