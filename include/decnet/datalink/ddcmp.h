// decnet/datalink/ddcmp.h -- the DDCMP datalink.
//
// Port of ddcmp.py.  DDCMP is the one datalink here that is a protocol in
// its own right: it frames, it sequences, and it retransmits, where
// Multinet and Ethernet get all three from what carries them.
//
// The message layer is this header; the protocol and its transports
// follow in ddcmp.cc.
//
// Three framing notes, because they are what the format is about.
//
// Every message starts with one of three bytes -- SOH for data, ENQ for
// control, DLE for maintenance -- and is exactly eight bytes of header
// including a CRC-16 over the first six.  A receiver that has lost its
// place hunts for one of those three bytes and checks the header CRC; a
// header that passes is a frame start, and that is the whole of
// resynchronisation.
//
// A data message carries its payload after the header, followed by a
// second CRC-16 over the payload alone.  The two CRCs are separate on
// purpose: a header damaged in transit is indistinguishable from noise,
// but a good header with a bad payload can still be acknowledged as
// received-in-error, which is what lets the sender retransmit exactly
// that message rather than resynchronising the link.
//
// Sequence numbers are modulo 256, and deliberately not RFC 1982: DDCMP
// allows up to modulus - 1 messages outstanding, where RFC 1982 comparison
// needs the window to stay under half the modulus.  the Python says the same
// thing in a comment on its Seq class.  So these are compared by the
// protocol's own rules, not by common/modulo.

#ifndef DECNET_DATALINK_DDCMP_H
#define DECNET_DATALINK_DDCMP_H

#include "decnet/common/crc.h"
#include "decnet/common/types.h"
#include "decnet/packet/packet.h"

#include "decnet/common/backoff.h"
#include "decnet/common/socket.h"
#include "decnet/common/timers.h"
#include "decnet/datalink/ptp.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace decnet::datalink::ddcmp {

// The start bytes, in the octal the Python writes them in.
inline constexpr std::uint8_t SOH = 0201;   // a data message
inline constexpr std::uint8_t ENQ = 0005;   // a control message
inline constexpr std::uint8_t DLE = 0220;   // a maintenance message
inline constexpr std::uint8_t SYN = 0226;   // fill between messages
inline constexpr std::uint8_t DEL = 0377;   // pad after a trailer

// Control message types.
inline constexpr std::uint8_t ACK   = 1;
inline constexpr std::uint8_t NAK   = 2;
inline constexpr std::uint8_t REP   = 3;
inline constexpr std::uint8_t STRT  = 6;
inline constexpr std::uint8_t STACK = 7;

// NAK reasons, which travel in the subtype field.
inline constexpr std::uint8_t R_HCRC = 1;    // header CRC error
inline constexpr std::uint8_t R_CRC  = 2;    // data CRC error
inline constexpr std::uint8_t R_REP  = 3;    // answering a REP
inline constexpr std::uint8_t R_BUF  = 8;    // no receive buffer
inline constexpr std::uint8_t R_OVER = 9;    // receive overrun
inline constexpr std::uint8_t R_SHRT = 16;   // buffer too short
inline constexpr std::uint8_t R_FMT  = 17;   // header format error

// Header length including its CRC.  Every message begins with one.
inline constexpr std::size_t HDRLEN = 8;

// The largest payload a count field can describe: fourteen bits.
inline constexpr std::size_t MAXCOUNT = 0x3fff;

// ------------------------------------------------------------ sequences

// A DDCMP sequence number.  Modulo 256, with the comparison the protocol
// needs rather than RFC 1982's: "is n in the window (lo, hi]" is asked
// directly, because with up to 255 messages outstanding there is no
// half-modulus rule to fall back on.
class Seq {
public:
    constexpr Seq () noexcept = default;
    constexpr explicit Seq (unsigned v) noexcept
        : v_ (static_cast<std::uint8_t> (v)) {}

    constexpr std::uint8_t value () const noexcept { return v_; }
    // Explicit: an implicit conversion makes `seq + 1` ambiguous with the
    // integer promotion, and the compiler is right to complain -- one of
    // them wraps at 256 and the other does not.
    constexpr explicit operator std::uint8_t () const noexcept { return v_; }

    constexpr Seq &operator++ () noexcept
    { v_ = static_cast<std::uint8_t> (v_ + 1); return *this; }
    constexpr Seq operator+ (unsigned n) const noexcept
    { return Seq (static_cast<unsigned> (v_ + n)); }

    // How many steps forward from this to other, modulo 256.
    constexpr std::uint8_t distance (Seq other) const noexcept
    { return static_cast<std::uint8_t> (other.v_ - v_); }

    // Is this number in the range (lo, hi], going forward from lo?  That
    // is the question an acknowledgement asks: "does this ack cover the
    // message I still have unacknowledged?"
    constexpr bool in_window (Seq lo, Seq hi) const noexcept
    {
        std::uint8_t span = lo.distance (hi);
        std::uint8_t off  = lo.distance (*this);
        return off != 0 && off <= span;
    }

    friend constexpr bool operator== (Seq, Seq) noexcept = default;

private:
    std::uint8_t v_ = 0;
};

// ------------------------------------------------------------- messages

// What a decoded message turned out to be.
enum class MsgKind { data, maintenance, ack, nak, rep, start, stack };

// One decoded DDCMP message.  the Python gives each kind a class and indexes
// them on the start byte; here they share a struct, because the header is
// one shape with two readings and the fields that differ are two bytes.
// Which reading applies is what `kind` says.
struct Message {
    MsgKind       kind = MsgKind::data;

    // Data and maintenance messages: how much payload follows.  Control
    // messages: the type and subtype in the same two bytes.
    std::uint16_t count = 0;
    std::uint8_t  type = 0;
    std::uint8_t  subtype = 0;

    bool          qsync = false;    // no synchronisation needed before this
    bool          select = false;   // the far end may transmit after this

    Seq           resp;             // what the sender has received
    Seq           num;              // this message's own number
    std::uint8_t  addr = 1;         // station address; always 1 point to point

    Bytes         payload;
    bool          crcok = true;     // did the payload CRC check out?

    bool is_data () const noexcept
    { return kind == MsgKind::data || kind == MsgKind::maintenance; }

    // Does this kind carry a response number the sender must fill in?
    // REP, START and STACK do not, and a maintenance message does not.
    bool sets_resp () const noexcept
    {
        return kind == MsgKind::data || kind == MsgKind::ack
            || kind == MsgKind::nak;
    }

    // The eight header bytes, header CRC included.
    Bytes encode_header () const;

    // The whole message: header, payload, payload CRC.
    Bytes encode () const;

    // A one line description, for the trace log.
    std::string str () const;
};

// Builders for the messages that have no payload.
Message make_ack (Seq resp);
Message make_nak (Seq resp, std::uint8_t reason);
Message make_rep (Seq num);
Message make_start ();
Message make_stack ();
Message make_data (Seq num, Seq resp, Bytes payload);
Message make_maintenance (Bytes payload);

// ------------------------------------------------------------- decoding

// Why a header was rejected.
enum class HdrError { none, too_short, bad_start, bad_crc };

// Decode the eight header bytes.  The CRC is checked unless told not to:
// a framer board that has already checked it hands over a header without
// one, which is what `check` is for.
//
// A header that fails is not an error to report upward -- on a stream the
// receiver simply has not found the start of a message yet -- so this
// returns the reason rather than throwing.
HdrError decode_header (ByteView buf, Message &out, bool check = true);

// Find the next plausible header in a stream: the first position whose
// start byte is one of the three and whose header CRC checks out.  Returns
// the offset, or nothing if no complete header is present yet.
//
// This is the whole of DDCMP resynchronisation.  A receiver that has lost
// its place cannot trust a length field, because the length it would read
// came from the noise that lost it -- so it trusts nothing but a header
// that passes its own CRC.
std::optional<std::size_t> find_header (ByteView buf);

// ------------------------------------------------------------- protocol

// The DDCMP protocol engine, with no transport in it.
//
// Everything that makes DDCMP a protocol rather than a frame format lives
// here: the startup handshake, sequence numbers, acknowledgement,
// retransmission and the maintenance mode.  What carries the bytes is the
// transport's business, and it is given to this class as two callbacks --
// one to put a message on the wire, one to hand a payload upward.
//
// Splitting it this way is what makes the protocol testable.  Two engines
// wired to each other exercise the startup handshake, a lost message, a
// NAK and a wrapped sequence number with no sockets, no timers and no
// scheduling -- and those are the parts that are hard to get right.
//
// States are the Python's, and its names: Istart after we have sent a Start,
// Astart after we have answered one, Running, and Maintenance.  See the
// DDCMP spec V4.1 table 3, the startup state table.
class Protocol {
public:
    enum class State { halted, istart, astart, running, maintenance };

    // What the engine needs from whatever is carrying it.
    struct Hooks {
        std::function<void (const Message &)> send;      // put on the wire
        std::function<void (Bytes)>           deliver;   // hand up a payload
        std::function<void ()>                up;        // link is running
        std::function<void ()>                down;      // link lost state
        // Ask for a timeout in this many seconds; zero cancels.  The
        // engine never reads a clock, so a test can drive it by hand.
        std::function<void (double)>          set_timer;
    };

    explicit Protocol (Hooks h, unsigned qmax = 7);

    State state () const noexcept { return state_; }
    const char *state_name () const noexcept;

    // The transport says the connection came up, or went away.
    void connected ();
    void restart ();
    void halt ();

    // A message arrived, or the timer expired.
    void receive (const Message &m);
    void timeout ();

    // A received message that failed its payload CRC, or a header error
    // the transport detected.  DDCMP answers these with a NAK, which is
    // how the far end learns to retransmit exactly one message rather
    // than resynchronising the whole link.
    void receive_error (std::uint8_t reason, const Message *partial = nullptr);

    // Send a payload.  Queued if the window is full, discarded if the
    // link is not running -- which is what a datalink does: routing will
    // notice the circuit is down and stop offering.
    void send (Bytes payload);

    // Maintenance mode, used by MOP to talk to a node that has no routing.
    void send_maintenance (Bytes payload);

    // Numbers, for the tests and for the counters.
    Seq last_received () const noexcept { return r_; }
    Seq last_acked () const noexcept { return a_; }
    Seq last_sent () const noexcept { return n_; }
    std::size_t unacked () const noexcept { return a_.distance (n_); }
    std::size_t queued () const noexcept { return notsent_.size (); }

private:
    void init_state ();
    bool cansend () const noexcept { return a_.distance (n_) < qmax_; }
    void enter_running (bool ack);
    void do_restart ();
    void send_msg (const Message &m, double timer);
    bool process_ack (const Message &m);
    void retransmit ();
    void send_queued (bool queue_only);
    void flush_ack ();

    Hooks       hooks_;
    State       state_ = State::halted;
    unsigned    qmax_;

    Seq         r_, a_, n_;
    bool        ackflag_ = false;
    std::array<std::optional<Message>, 256> unack_;
    std::deque<Bytes> notsent_;

    Backoff     acktmr_ { 1.0, 60.0 };
    Backoff     stacktmr_ { 3.0, 120.0 };
};

}   // namespace decnet::datalink::ddcmp

namespace decnet::datalink {

// ------------------------------------------------------------ the datalink

// The parsed --device argument: proto:lport:host:rport, where proto is
// "udp", "tcp" or "telnet", as the Python takes it.  A serial line is
// serial:devname[:speed] instead.
struct DdcmpDevice {
    enum class Mode { udp, tcp, telnet, serial };

    Mode          mode = Mode::udp;
    std::string   destination;      // peer host, or the device name
    std::uint16_t dest_port = 0;
    std::uint16_t source_port = 0;
    unsigned      speed = 9600;     // serial only

    static DdcmpDevice parse (const std::string &device);
    std::string str () const;
};

// A DDCMP circuit.  The protocol engine does the protocol; this class
// carries its messages and drives its timer.
//
// The engine asks for timeouts and has no clock of its own.  PtpDatalink
// is already a Timer -- StateMachine derives from one, because a state
// machine nearly always needs a timer -- so the timeout hook is an
// override rather than another base.
class Ddcmp : public PtpDatalink {
public:
    Ddcmp (Element *owner, std::string name, DdcmpDevice dev);
    ~Ddcmp () override;

    static constexpr const char *class_name = "DDCMP";

    static std::unique_ptr<Datalink> create (Element *owner,
                                             const std::string &name,
                                             const std::string &device);

    const DdcmpDevice &device () const noexcept { return dev_; }
    const ddcmp::Protocol &protocol () const noexcept { return *proto_; }

    void send (Bytes msg) override;

    // Timer, for the protocol engine.  PtpDatalink's own states do not
    // use it, so there is no contention for the one timer.
    void timeout () override;

protected:
    State connected () override;
    State running (Work &w) override;

    // Put one encoded message on the wire.  What that means is the only
    // thing the transports disagree about.
    virtual void transmit (const ddcmp::Message &m) = 0;

    void make_protocol ();

    DdcmpDevice                       dev_;
    std::unique_ptr<ddcmp::Protocol>  proto_;
    SourceAddress                     source_;
    HostAddress                       dest_;
    Backoff                           conn_timer_ { 5.0, 120.0 };
};

// One datagram is exactly one DDCMP message, so there is no framing to do
// and a lost datagram is a lost message -- which the protocol already
// expects.
class UdpDdcmp : public Ddcmp {
public:
    UdpDdcmp (Element *owner, std::string name, DdcmpDevice dev);

protected:
    void connect () override;
    void disconnect () override;
    bool check_connection () override;
    void receive_loop () override;
    void transmit (const ddcmp::Message &m) override;
};

}   // namespace decnet::datalink

#endif
