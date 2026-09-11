#include "decnet/mop/mop.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/datalink.h"
#include "decnet/node.h"
#include "decnet/version.h"

#include <random>

namespace decnet::mop {

using datalink::MOPDL_PROTO;

namespace {

// Loopback runs on its own protocol type, unpadded.
constexpr std::uint16_t LOOP_PROTO = 0x9000;

// The first announcement goes out much sooner than the periodic one, so a
// node that has just started is visible without a ten minute wait.  Port
// of SYSID_STARTRATIO.
constexpr double SYSID_START_RATIO = 30.0;

std::string key_of (Macaddr a) { return a.str (); }

}   // namespace

// ---------------------------------------------------------- SysIdHandler

SysIdHandler::SysIdHandler (MopCircuit *parent, datalink::BcPort *port)
    : Element (parent), parent_ (parent), port_ (port)
{
    port_->add_multicast (console_multicast ());
}

double SysIdHandler::next_id_delay () const
{
    static thread_local std::mt19937 gen { std::random_device {} () };
    std::uniform_real_distribution<double> d (8 * 60, 12 * 60);
    return d (gen);
}

void SysIdHandler::start ()
{
    started_ = std::chrono::steady_clock::now ();
    if (node ())
        node ()->timers ().start (this, next_id_delay () / SYSID_START_RATIO);
    DN_DEBUG ("MOP system id handler started on {}", parent_->name ());
}

void SysIdHandler::stop ()
{
    if (node ()) node ()->timers ().stop (this);
}

void SysIdHandler::timeout ()
{
    DN_TRACE ("sending periodic system id on {}", parent_->name ());
    send_id (console_multicast (), 0);
    if (node ()) node ()->timers ().start (this, next_id_delay ());
}

void SysIdHandler::send_id (Macaddr dest, std::uint16_t receipt)
{
    SysId s;
    s.receipt  = receipt;
    s.version  = Version { 3, 0, 0 };
    s.loop     = true;
    s.counters = true;

    Macaddr hw = port_->macaddr ();
    const auto &b = hw.bytes ();
    s.hwaddr = Bytes (b.begin (), b.end ());

    // Device 9 is PCL-11: a real code, but obviously not an Ethernet
    // controller, which is the point.  pydecnet picks it for the same
    // reason.
    s.device    = 9;
    s.datalink  = 1;                 // Ethernet
    s.processor = 2;                 // communications server
    s.software  = SoftwareId::named ("DECnet/C++");
    port_->send (s.encode_packet (), dest);
}

void SysIdHandler::request_id (Macaddr dest, std::uint16_t receipt)
{
    RequestId r;
    r.receipt = receipt;
    port_->send (r.encode_packet (), dest);
}

void SysIdHandler::send_counters (Macaddr dest, std::uint16_t receipt)
{
    Counters c;
    c.receipt = receipt;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds> (
        std::chrono::steady_clock::now () - started_).count ();
    c.time_since_zeroed = static_cast<std::uint16_t> (
        std::min<long long> (elapsed, 65535));

    // The counters a software implementation actually has.  The error
    // counts stay zero: they describe a controller, and there is not one.
    const datalink::BcPortCounters &pc = port_->counters ();
    c.bytes_recv   = static_cast<std::uint32_t> (pc.bytes_recv);
    c.bytes_sent   = static_cast<std::uint32_t> (pc.bytes_sent);
    c.pkts_recv    = static_cast<std::uint32_t> (pc.pkts_recv);
    c.pkts_sent    = static_cast<std::uint32_t> (pc.pkts_sent);
    c.mcbytes_recv = static_cast<std::uint32_t> (pc.mcbytes_recv);
    c.mcpkts_recv  = static_cast<std::uint32_t> (pc.mcpkts_recv);

    port_->send (c.encode_packet (), dest);
}

void SysIdHandler::dispatch (Work &w)
{
    auto *r = dynamic_cast<Received *> (&w);
    if (!r) return;

    auto pkt = MopPacketBase::parse_frame (
        ByteView (r->packet ().data (), r->packet ().size ()));
    if (!pkt) {
        DN_TRACE ("undecodable MOP message on {}", parent_->name ());
        return;
    }

    // The datalink does not tell us the source address, so take it from
    // the message where there is one.  A system ID carries its hardware
    // address; a request does not, so we answer to the multicast address
    // and let the requester pick it out.
    if (auto *s = dynamic_cast<SysId *> (pkt.get ())) {
        Macaddr src;
        if (s->hwaddr && s->hwaddr->size () == 6) {
            std::array<std::uint8_t, 6> a {};
            std::copy (s->hwaddr->begin (), s->hwaddr->end (), a.begin ());
            src = Macaddr (a);
        }
        std::string k = key_of (src);
        bool seen = heard_.count (k) != 0;
        DN_TRACE ("system id on {} from {} node {}", parent_->name (),
                  seen ? "known" : "new", src.str ());
        heard_[k] = HeardSystem { src, *s,
                                  std::chrono::steady_clock::now (),
                                  std::chrono::system_clock::now () };
        return;
    }
    if (auto *q = dynamic_cast<RequestId *> (pkt.get ())) {
        send_id (console_multicast (), q->receipt);
        return;
    }
    if (auto *q = dynamic_cast<RequestCounters *> (pkt.get ())) {
        send_counters (console_multicast (), q->receipt);
        return;
    }
    // PORT: console carrier messages arrive here and are ignored.
}

// ----------------------------------------------------------- LoopHandler

LoopHandler::LoopHandler (MopCircuit *parent, datalink::BcDatalink *dl)
    : Element (parent), parent_ (parent)
{
    // Loopback frames are not padded: the protocol carries its own
    // length information in the skip count.
    port_ = dl->create_bc_port (this, LOOP_PROTO, false);
    port_->add_multicast (loop_multicast ());
}

void LoopHandler::loop (Macaddr dest, Bytes payload)
{
    // The message carries two functions in turn.  The far station reads
    // the first, "forward to this address", and sends the message back
    // here with the skip count advanced past it.  We then read the second,
    // "reply", and that is the round trip.
    LoopReply rep;
    rep.receipt = ++receipt_;
    rep.payload = std::move (payload);

    LoopFwd fwd;
    // Bind the address to a local: macaddr() returns by value, so taking a
    // reference to its bytes() would outlive the temporary.
    Macaddr me = port_->macaddr ();
    fwd.dest.assign (me.bytes ().begin (), me.bytes ().end ());
    fwd.payload = rep.encode ();

    LoopSkip top;
    top.skip = 0;
    top.payload = fwd.encode ();
    port_->send (top.encode (), dest);
}

void LoopHandler::dispatch (Work &w)
{
    auto *r = dynamic_cast<Received *> (&w);
    if (!r) return;
    const Bytes &buf = r->packet ();

    LoopSkip top;
    try {
        top.decode (ByteView (buf.data (), buf.size ()));
    } catch (const DecodeError &) {
        return;
    }
    // The skip count must be even and must leave room for a function
    // code.  A bad one means the message is not for us to interpret.
    if ((top.skip & 1) || top.skip + 4u > buf.size ()) return;

    ByteView rest (buf.data () + 2 + top.skip, buf.size () - 2 - top.skip);
    if (rest.size () < 2) return;
    std::uint16_t function = static_cast<std::uint16_t> (
        rest[0] | (rest[1] << 8));

    if (function == LoopFwd::function_code) {
        LoopFwd f;
        try {
            f.decode (rest);
        } catch (const DecodeError &) {
            return;
        }
        if (f.dest.size () != 6) return;
        std::array<std::uint8_t, 6> a {};
        std::copy (f.dest.begin (), f.dest.end (), a.begin ());
        Macaddr dest (a);
        // Forwarding to a multicast address would multiply the message.
        if (dest.is_multicast ()) return;

        // Step past this function and pass it on.  The next station sees
        // whatever follows, which is how the reply finds its way back.
        LoopSkip out;
        out.skip = static_cast<std::uint16_t> (top.skip + 8);
        out.payload = top.payload;
        port_->send (out.encode (), dest);
        return;
    }
    if (function == LoopReply::function_code) {
        LoopReply f;
        try {
            f.decode (rest);
        } catch (const DecodeError &) {
            return;
        }
        ++replies_;
        last_reply_ = f.payload;
        DN_TRACE ("loop reply on {}, {} bytes", parent_->name (),
                  f.payload.size ());
        return;
    }
}

// ------------------------------------------------------------ MopCircuit

MopCircuit::MopCircuit (Element *parent, std::string name,
                        datalink::BcDatalink *dl)
    : Element (parent), name_ (std::move (name)), datalink_ (dl)
{
    // The port is owned by the circuit and its traffic forwarded to the
    // handler, because the handler does not exist yet when the port has to
    // be created.  The loop handler makes its own, since by then it does.
    datalink::BcPort *p = dl->create_bc_port (this, MOPDL_PROTO);
    sysid_ = std::make_unique<SysIdHandler> (this, p);
    loop_  = std::make_unique<LoopHandler> (this, dl);
    DN_DEBUG ("MOP initialized on circuit {}", name_);
}

void MopCircuit::dispatch (Work &w) { sysid_->dispatch (w); }

void MopCircuit::start () { sysid_->start (); }
void MopCircuit::stop ()  { sysid_->stop (); }

// ------------------------------------------------------------------- Mop

Mop::Mop (Element *parent, const Config &config)
    : Element (parent)
{
    DN_DEBUG ("initializing MOP layer");
    datalink::DatalinkLayer *dll = node () ? node ()->datalink () : nullptr;
    if (!dll) return;

    for (const CircuitConfig &c : config.circuits ()) {
        if (!c.mop) continue;
        datalink::Datalink *dl = dll->circuit (c.name);
        auto *bc = dynamic_cast<datalink::BcDatalink *> (dl);
        if (!bc) {
            if (dl)
                DN_DEBUG ("MOP asked for on {}, which is not a broadcast "
                          "circuit; ignored", c.name);
            continue;
        }
        auto mc = std::make_unique<MopCircuit> (this, c.name, bc);
        MopCircuit *raw = mc.get ();
        circuits_[c.name] = std::move (mc);
        order_.push_back (raw);
    }
}

Mop::~Mop () = default;

void Mop::start ()
{
    DN_DEBUG ("starting MOP");
    for (MopCircuit *c : order_) c->start ();
}

void Mop::stop ()
{
    DN_DEBUG ("stopping MOP");
    for (MopCircuit *c : order_) c->stop ();
}

MopCircuit *Mop::circuit (const std::string &name) const
{
    std::string key;
    try {
        key = circname (name);
    } catch (const std::invalid_argument &) {
        return nullptr;
    }
    auto it = circuits_.find (key);
    return it == circuits_.end () ? nullptr : it->second.get ();
}

}   // namespace decnet::mop
