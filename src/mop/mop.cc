#include "decnet/mop/mop.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/datalink.h"
#include "decnet/node.h"
#include "decnet/version.h"

#include <random>

namespace decnet::mop {

using datalink::MOPCONS_PROTO;

namespace {

// Tell a request that no answer is coming.  A delayed call is simply not
// made.
struct NoAnswer {
    void operator() (SysIdDone &d)    const { if (d) d (nullptr, Macaddr ()); }
    void operator() (CountersDone &d) const { if (d) d (nullptr, Macaddr ()); }
    void operator() (LoopDone &d)     const { if (d) d (false, Macaddr ()); }
    void operator() (std::function<void ()> &) const {}
};

// Loopback runs on its own protocol type, unpadded.
constexpr std::uint16_t LOOP_PROTO = 0x9000;

// The first announcement is sent early.  Port of SYSID_STARTRATIO.
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

    // Device 9 (PCL-11), as PyDECnet uses.
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

void SysIdHandler::request_counters (Macaddr dest, std::uint16_t receipt)
{
    RequestCounters r;
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

    // Answers go back to whoever asked.  A frame without a source -- not
    // from an Ethernet -- is answered to the multicast address, as it
    // always used to be.
    Macaddr src = r->src ();
    Macaddr reply_to = src == Macaddr () ? console_multicast () : src;

    if (auto *s = dynamic_cast<SysId *> (pkt.get ())) {
        std::string k = key_of (src);
        bool seen = heard_.count (k) != 0;
        DN_TRACE ("system id on {} from {} node {}", parent_->name (),
                  seen ? "known" : "new", src.str ());
        heard_[k] = HeardSystem { src, *s,
                                  std::chrono::steady_clock::now (),
                                  std::chrono::system_clock::now () };
        if (s->receipt) parent_->answer_id (s->receipt, *s, src);
        return;
    }
    if (auto *q = dynamic_cast<RequestId *> (pkt.get ())) {
        send_id (reply_to, q->receipt);
        return;
    }
    if (auto *q = dynamic_cast<RequestCounters *> (pkt.get ())) {
        send_counters (reply_to, q->receipt);
        return;
    }
    if (auto *c = dynamic_cast<Counters *> (pkt.get ())) {
        parent_->answer_counters (c->receipt, *c, src);
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
    loop (dest, {}, std::move (payload), ++receipt_);
}

Macaddr LoopHandler::macaddr () const { return port_->macaddr (); }

void LoopHandler::loop (Macaddr first, const std::vector<Macaddr> &then,
                        Bytes payload, std::uint16_t receipt)
{
    // The functions, built from the end: the reply, forward to us, and
    // before that forward to each station in then.
    LoopReply rep;
    rep.receipt = receipt;
    rep.payload = std::move (payload);
    Bytes msg = rep.encode ();

    std::vector<Macaddr> hops (then);
    hops.push_back (port_->macaddr ());
    for (auto h = hops.rbegin (); h != hops.rend (); ++h) {
        LoopFwd fwd;
        // Bind the address to a local: bytes() of a temporary would not
        // outlive it.
        Macaddr a = *h;
        fwd.dest.assign (a.bytes ().begin (), a.bytes ().end ());
        fwd.payload = std::move (msg);
        msg = fwd.encode ();
    }

    LoopSkip top;
    top.skip = 0;
    top.payload = std::move (msg);
    port_->send (top.encode (), first);
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
        parent_->answer_loop (f.receipt, r->src ());
        return;
    }
}

// ------------------------------------------------------------ MopCircuit

MopCircuit::MopCircuit (Element *parent, std::string name,
                        datalink::BcDatalink *dl)
    : Element (parent), name_ (std::move (name)), datalink_ (dl)
{
    // The circuit owns the port and forwards to the handler, which is created
    // later.  The loop handler creates its own port.
    datalink::BcPort *p = dl->create_bc_port (this, MOPCONS_PROTO);
    sysid_ = std::make_unique<SysIdHandler> (this, p);
    loop_  = std::make_unique<LoopHandler> (this, dl);
    DN_DEBUG ("MOP initialized on circuit {}", name_);
}

MopCircuit::~MopCircuit ()
{
    if (node ()) node ()->timers ().stop (this);
}

void MopCircuit::dispatch (Work &w) { sysid_->dispatch (w); }

void MopCircuit::start () { sysid_->start (); }

void MopCircuit::stop ()
{
    sysid_->stop ();
    if (node ()) node ()->timers ().stop (this);
    // Nobody will answer now.
    auto pending = std::move (pending_);
    pending_.clear ();
    for (auto &[receipt, p] : pending) std::visit (NoAnswer {}, p.done);
}

std::uint16_t MopCircuit::next_receipt ()
{
    // Loop receipts share the numbering, so an answer to one request
    // cannot be taken for another's.
    do {
        ++receipt_;
    } while (receipt_ == 0 || pending_.count (receipt_));
    return receipt_;
}

void MopCircuit::wait_for (std::uint16_t receipt, double timeout,
                           Callback done)
{
    auto deadline = std::chrono::steady_clock::now ()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
              std::chrono::duration<double> (timeout));
    pending_[receipt] = Pending { std::move (done), deadline };
    rearm ();
}

std::optional<MopCircuit::Callback> MopCircuit::take (std::uint16_t receipt,
                                                      std::size_t kind)
{
    auto it = pending_.find (receipt);
    if (it == pending_.end () || it->second.done.index () != kind)
        return std::nullopt;
    Callback c = std::move (it->second.done);
    pending_.erase (it);
    rearm ();
    return c;
}

void MopCircuit::rearm ()
{
    if (!node ()) return;
    if (pending_.empty ()) {
        node ()->timers ().stop (this);
        return;
    }
    auto first = pending_.begin ()->second.deadline;
    for (const auto &[r, p] : pending_) first = std::min (first, p.deadline);
    double secs = std::chrono::duration<double> (
        first - std::chrono::steady_clock::now ()).count ();
    // The wheel ticks every JIFFY; anything sooner is the next tick.
    node ()->timers ().start (this, std::max (secs, 0.1));
}

void MopCircuit::timeout ()
{
    auto now = std::chrono::steady_clock::now ();
    std::vector<Callback> expired;
    for (auto it = pending_.begin (); it != pending_.end ();) {
        if (it->second.deadline <= now) {
            expired.push_back (std::move (it->second.done));
            it = pending_.erase (it);
        } else {
            ++it;
        }
    }
    rearm ();
    // Called last: a callback may well start another request.
    for (Callback &c : expired) {
        if (auto *later = std::get_if<Later> (&c)) {
            if (*later) (*later) ();
        } else {
            std::visit (NoAnswer {}, c);
        }
    }
}

void MopCircuit::after (double secs, std::function<void ()> fn)
{
    // A key from the receipt numbering, which never goes on the wire.
    wait_for (next_receipt (), secs, Later (std::move (fn)));
}

void MopCircuit::request_id (Macaddr dest, double timeout, SysIdDone done)
{
    std::uint16_t r = next_receipt ();
    wait_for (r, timeout, std::move (done));
    sysid_->request_id (dest, r);
}

void MopCircuit::request_counters (Macaddr dest, double timeout,
                                   CountersDone done)
{
    std::uint16_t r = next_receipt ();
    wait_for (r, timeout, std::move (done));
    sysid_->request_counters (dest, r);
}

void MopCircuit::loop (Macaddr dest, const std::vector<Macaddr> &then,
                       Bytes payload, double timeout, LoopDone done)
{
    std::uint16_t r = next_receipt ();
    wait_for (r, timeout, std::move (done));
    loop_->loop (dest, then, std::move (payload), r);
}

void MopCircuit::answer_id (std::uint16_t receipt, const SysId &s,
                            Macaddr from)
{
    auto c = take (receipt, 0);
    if (!c) return;
    if (auto *done = std::get_if<SysIdDone> (&*c); done && *done)
        (*done) (&s, from);
}

void MopCircuit::answer_counters (std::uint16_t receipt, const Counters &k,
                                  Macaddr from)
{
    auto c = take (receipt, 1);
    if (!c) return;
    if (auto *done = std::get_if<CountersDone> (&*c); done && *done)
        (*done) (&k, from);
}

void MopCircuit::answer_loop (std::uint16_t receipt, Macaddr from)
{
    auto c = take (receipt, 2);
    if (!c) return;
    if (auto *done = std::get_if<LoopDone> (&*c); done && *done)
        (*done) (true, from);
}

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
    if (name.empty ()) return order_.size () == 1 ? order_.front () : nullptr;
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
