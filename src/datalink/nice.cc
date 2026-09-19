// src/datalink/nice.cc -- NICE reads for the datalink layer.
//
// Ports of DatalinkLayer.nice_read, Datalink.nice_read_line,
// Port.nice_read_port and Ethernet.nice_read_line.
//
// A line read reports the device and its counters.  A circuit read adds
// the port's circuit type and traffic counters to what routing reports.
//
// Both reads ask the datalink for the same counters through add_counters,
// which is what PyDECnet gets by reaching the one counter object from
// either side.

#include "decnet/datalink/bc.h"
#include "decnet/datalink/datalink.h"
#include "decnet/datalink/ddcmp.h"
#include "decnet/nice/nml.h"
#include "decnet/node.h"

namespace decnet::datalink {

using nice::Counter;
using nice::Entity;
using nice::NiceReply;
using nice::NiceRequest;
using nice::ReplyDict;
using nice::Value;

namespace {

// A plain four byte traffic counter, which is what all of these are.
void ctr4 (NiceReply &r, std::uint16_t n, std::uint64_t v)
{
    r.params.set_counter (n, Counter { v, 4, false, 0 });
}

void ctr1 (NiceReply &r, std::uint16_t n, std::uint64_t v)
{
    r.params.set_counter (n, Counter { v, 1, false, 0 });
}

void ctr2 (NiceReply &r, std::uint16_t n, std::uint64_t v)
{
    r.params.set_counter (n, Counter { v, 2, false, 0 });
}

// A mapped counter: the count plus the bitmap of reasons seen.
void ctm1 (NiceReply &r, std::uint16_t n, std::uint64_t v, std::uint16_t map)
{
    r.params.set_counter (n, Counter { v, 1, true, map });
}

}   // namespace

// -------------------------------------------------------------- Datalink

void Datalink::add_counters (NiceReply &r) const
{
    const PtpCounters *c = counters ();
    if (!c) return;
    ctr4 (r, 1000, c->bytes_recv);
    ctr4 (r, 1001, c->bytes_sent);
    ctr4 (r, 1010, c->pkts_recv);
    ctr4 (r, 1011, c->pkts_sent);
}

void BcDatalink::add_counters (NiceReply &r) const
{
    // A broadcast line has no traffic of its own: what crosses it is the
    // sum of what its ports saw.
    BcPortCounters c = combined_counters ();
    ctr4 (r, 1000, c.bytes_recv);
    ctr4 (r, 1001, c.bytes_sent);
    ctr4 (r, 1002, c.mcbytes_recv);
    ctr4 (r, 1010, c.pkts_recv);
    ctr4 (r, 1011, c.pkts_sent);
    ctr4 (r, 1012, c.mcpkts_recv);
    ctr2 (r, 1063, unk_dest_);
}

void Ddcmp::add_counters (NiceReply &r) const
{
    PtpDatalink::add_counters (r);
    if (!proto_) return;        // not opened yet, so nothing has gone wrong
    const ddcmp::Counters &c = proto_->counters ();
    ctm1 (r, 1020, c.data_errors_inbound, c.data_errors_inbound_map);
    ctm1 (r, 1021, c.data_errors_outbound, c.data_errors_outbound_map);
    ctr1 (r, 1030, c.remote_reply_timeouts);
    ctr1 (r, 1031, c.local_reply_timeouts);
    ctm1 (r, 1040, c.remote_buffer_errors, c.remote_buffer_errors_map);
}

// ------------------------------------------------------------------ Port

void Port::nice_read_port (const NiceRequest &req, NiceReply &r)
{
    if (req.chars ()) {
        r.params.set (1112, Value::c (datalink_->nice_type ()));
    } else if (req.counters ()) {
        datalink_->add_counters (r);
    }
}

void BcPort::nice_read_port (const NiceRequest &req, NiceReply &r)
{
    if (req.chars ()) {
        r.params.set (1112, Value::c (datalink_->nice_type ()));
        return;
    }
    if (!req.counters ()) return;
    // A circuit read reports this port's own traffic, not the whole line's:
    // a circuit is one upper layer's use of the wire.  The multicast
    // counters come with it, since routing's hellos are most of them.
    ctr4 (r, 1000, counters_.bytes_recv);
    ctr4 (r, 1001, counters_.bytes_sent);
    ctr4 (r, 1002, counters_.mcbytes_recv);
    ctr4 (r, 1010, counters_.pkts_recv);
    ctr4 (r, 1011, counters_.pkts_sent);
    ctr4 (r, 1012, counters_.mcpkts_recv);
}

// -------------------------------------------------------------- Datalink

void Datalink::nice_read_line (const NiceRequest &req, ReplyDict &resp)
{
    NiceReply &r = resp.named_entry (name_);
    if (req.sumstat ()) {
        r.params.set (0, Value::c (0));         // On
    } else if (req.chars ()) {
        r.params.set (1111, Value::c (0));      // Full duplex
        r.params.set (1112, Value::c (nice_protocol ()));
    } else if (req.counters ()) {
        if (Node *n = node ())
            ctr2 (r, 0, n->seconds_since_zeroed ());
        add_counters (r);
    }
}

void BcDatalink::nice_read_line (const NiceRequest &req, ReplyDict &resp)
{
    Datalink::nice_read_line (req, resp);
    // An Ethernet line has a hardware address, which is the one thing a
    // line read tells you that a circuit read does not.
    if (!req.chars ()) return;
    NiceReply &r = resp.named_entry (name_);
    const auto &b = hwaddr_.bytes ();
    r.params.set (1160, Value::hi (Bytes (b.begin (), b.end ())));
}

// --------------------------------------------------------- DatalinkLayer

void DatalinkLayer::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    if (req.entity_type != Entity::line) return;

    if (req.entity.one ()) {
        // One named line.  A line that is not here is not an error: the
        // listener turns an empty answer into "unrecognized component".
        Datalink *dl = circuit (req.entity.name);
        if (dl) dl->nice_read_line (req, resp);
        return;
    }
    // Active or known lines are the same list, because every circuit that
    // is configured is on.
    for (Datalink *dl : order_) dl->nice_read_line (req, resp);
}

}   // namespace decnet::datalink
