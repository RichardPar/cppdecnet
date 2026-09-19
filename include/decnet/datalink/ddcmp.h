// decnet/datalink/ddcmp.h -- the DDCMP datalink.
//
// Port of ddcmp.py.  This header has the message layer; the protocol and
// transports are in ddcmp.cc and ddcmp_link.cc.
//
// Every message starts with SOH (data), ENQ (control) or DLE
// (maintenance) and has an 8 byte header ending in a CRC-16 over the first
// six bytes.  To resynchronise, a receiver searches for a start byte whose
// header CRC checks.
//
// Data messages have a second CRC-16 over the payload, so a message with a
// good header and bad payload can be NAKed and retransmitted.
//
// Sequence numbers are modulo 256 but not compared per RFC 1982, since up
// to 255 messages may be outstanding.

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

// The start bytes, in the octal PyDECnet writes them in.
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

// DDCMP sequence number, modulo 256, with a window test instead of RFC 1982
// comparison.
class Seq {
public:
    constexpr Seq () noexcept = default;
    constexpr explicit Seq (unsigned v) noexcept
        : v_ (static_cast<std::uint8_t> (v)) {}

    constexpr std::uint8_t value () const noexcept { return v_; }
    // Explicit, so that seq + 1 is not ambiguous with integer promotion.
    constexpr explicit operator std::uint8_t () const noexcept { return v_; }

    constexpr Seq &operator++ () noexcept
    { v_ = static_cast<std::uint8_t> (v_ + 1); return *this; }
    constexpr Seq operator+ (unsigned n) const noexcept
    { return Seq (static_cast<unsigned> (v_ + n)); }

    // How many steps forward from this to other, modulo 256.
    constexpr std::uint8_t distance (Seq other) const noexcept
    { return static_cast<std::uint8_t> (other.v_ - v_); }

    // Is this number in (lo, hi], going forward from lo?
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

// One decoded DDCMP message.  All kinds share one struct; `kind` says how
// to interpret the header fields.
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

// Decode the 8 header bytes.  Set check to false to skip the CRC (for a
// framer that has already checked it).  Returns the failure reason rather
// than throwing.
HdrError decode_header (ByteView buf, Message &out, bool check = true);

// Find the next valid header in a stream: the first offset with a start
// byte and a good header CRC.  Returns nothing if no complete header is
// present.
std::optional<std::size_t> find_header (ByteView buf);

// ------------------------------------------------------------- protocol

// DDCMP protocol engine, independent of transport.
//
// Handles startup, sequencing, acknowledgement, retransmission and
// maintenance mode.  The transport supplies two callbacks: one to send a
// message and one to deliver a payload upward.  Tests connect two engines
// directly.
//
// States follow PyDECnet: Istart, Astart, Running, Maintenance.  See DDCMP
// V4.1 table 3.
// The DDCMP error counters, which are what a NICE circuit read reports on
// top of the traffic counters every point to point link keeps.  Port of
// ddcmp.DdcmpCounters.
//
// The three mapped counters carry a bitmap of which reasons were seen, not
// one count per reason: that is how the architecture defines CTM counters,
// and the qualifier names in nicedefs.cc index it.
struct Counters {
    std::uint64_t data_errors_inbound = 0;
    std::uint16_t data_errors_inbound_map = 0;
    std::uint64_t data_errors_outbound = 0;
    std::uint16_t data_errors_outbound_map = 0;
    std::uint64_t remote_buffer_errors = 0;
    std::uint16_t remote_buffer_errors_map = 0;
    std::uint64_t remote_reply_timeouts = 0;
    std::uint64_t local_reply_timeouts = 0;
};

// Which counter and which qualifier bit a NAK reason belongs to.  PyDECnet's
// nak_map; R_OVER and R_FMT are deliberately unmapped there and here, so
// this returns false for them.
struct NakCounter { bool data; unsigned bit; };
bool nak_counter (std::uint8_t reason, NakCounter &out) noexcept;

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

    // A received message with a bad payload CRC, or a header error from the
    // transport.  Answered with a NAK.
    void receive_error (std::uint8_t reason, const Message *partial = nullptr);

    // Send a payload.  Queued if the window is full, discarded if the link is
    // not running.
    void send (Bytes payload);

    // Maintenance mode, used by MOP to talk to a node that has no routing.
    void send_maintenance (Bytes payload);

    // The error counters, maintained as NAKs and REPs go by.
    const Counters &counters () const noexcept { return counters_; }

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
    Counters    counters_;
};

}   // namespace decnet::datalink::ddcmp

namespace decnet::datalink {

// ------------------------------------------------------------ the datalink

// Parsed device string: proto:lport:host:rport, where proto is udp, tcp or
// telnet, or serial:devname[:speed].
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

// A DDCMP circuit: runs the protocol engine over a transport and drives
// its timer.  PtpDatalink is already a Timer, so the timeout hook is an
// override.
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

    // The traffic counters every point to point link keeps, plus DDCMP's
    // own error counters from the protocol engine.
    void add_counters (nice::NiceReply &r) const override;

    // Timer, for the protocol engine.  PtpDatalink's own states do not
    // use it, so there is no contention for the one timer.
    void timeout () override;

protected:
    // A restart from the layer above restarts the protocol and keeps the
    // transport connection.  Everything else goes to the base class.
    bool validate (Work &w) override;

    State connected () override;
    State running (Work &w) override;

    // Put one encoded message on the wire.  What that means is the only
    // thing the transports disagree about.
    virtual void transmit (const ddcmp::Message &m) = 0;

    void make_protocol ();

    // Read one message from a byte stream.  Advances a byte at a time until 8
    // bytes pass the header CRC, then reads the payload.  Used by TCP and
    // serial.
    Bytes read_framed_message (const std::function<Bytes (std::size_t)> &readn);

    DdcmpDevice                       dev_;
    std::unique_ptr<ddcmp::Protocol>  proto_;
    SourceAddress                     source_;
    HostAddress                       dest_;
    Backoff                           conn_timer_ { 5.0, 120.0 };
};

// DDCMP over UDP.  One datagram is one message.
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

// DDCMP over TCP.  Both ends listen and connect, and the first connection
// is used, as in SIMH's sim_tmxr.c.
class TcpDdcmp : public Ddcmp {
public:
    TcpDdcmp (Element *owner, std::string name, DdcmpDevice dev);

protected:
    void connect () override;
    void disconnect () override;
    bool check_connection () override;
    void receive_loop () override;
    void transmit (const ddcmp::Message &m) override;

private:
    // Telnet mode carries the all-ones byte doubled, so a DDCMP message
    // containing one is not mistaken for a telnet command.
    Bytes unescape_read (std::size_t n);
    static Bytes escape (const Bytes &b);

    bool   telnet_ = false;
    Socket listener_;       // inbound
    Socket connecting_;     // outbound, until one of them wins
};

// DDCMP over a serial line.
class SerialDdcmp : public Ddcmp {
public:
    SerialDdcmp (Element *owner, std::string name, DdcmpDevice dev);

protected:
    void connect () override;
    void disconnect () override;
    bool check_connection () override;
    void receive_loop () override;
    void transmit (const ddcmp::Message &m) override;

private:
    // Exactly n bytes from the line, or a throw if a stop was asked for.
    // A tty is not a socket, so this cannot use PtpDatalink::recvall.
    Bytes read_line (std::size_t n);

    int fd_ = -1;
};

}   // namespace decnet::datalink

#endif
