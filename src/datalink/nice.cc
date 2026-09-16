// src/datalink/nice.cc -- NICE reads for the datalink layer.
//
// Ports of DatalinkLayer.nice_read, Datalink.nice_read_line,
// Port.nice_read_port and Ethernet.nice_read_line.
//
// A line read reports the device and its counters.  A circuit read adds
// the port's circuit type and traffic counters to what routing reports.

#include "decnet/datalink/bc.h"
#include "decnet/datalink/datalink.h"
#include "decnet/nice/nml.h"

namespace decnet::datalink {

using nice::Counter;
using nice::Entity;
using nice::NiceReply;
using nice::NiceRequest;
using nice::ReplyDict;
using nice::Value;

namespace {

// Add a datalink's traffic counters.
void copy_counters (const PtpCounters &c, NiceReply &r)
{
    r.params.set_counter (1000, Counter { c.bytes_recv, 4, false, 0 });
    r.params.set_counter (1001, Counter { c.bytes_sent, 4, false, 0 });
    r.params.set_counter (1010, Counter { c.pkts_recv, 4, false, 0 });
    r.params.set_counter (1011, Counter { c.pkts_sent, 4, false, 0 });
}

}   // namespace

// ------------------------------------------------------------------ Port

void Port::nice_read_port (const NiceRequest &req, NiceReply &r)
{
    if (req.chars ()) {
        r.params.set (1112, Value::c (datalink_->nice_type ()));
    } else if (req.counters ()) {
        if (const PtpCounters *c = datalink_->counters ()) copy_counters (*c, r);
    }
}

void BcPort::nice_read_port (const NiceRequest &req, NiceReply &r)
{
    if (req.chars ()) {
        r.params.set (1112, Value::c (datalink_->nice_type ()));
        return;
    }
    if (!req.counters ()) return;
    // Broadcast ports also have multicast counters.
    r.params.set_counter (1000, Counter { counters_.bytes_recv, 4, false, 0 });
    r.params.set_counter (1001, Counter { counters_.bytes_sent, 4, false, 0 });
    r.params.set_counter (1002, Counter { counters_.mcbytes_recv, 4, false, 0 });
    r.params.set_counter (1010, Counter { counters_.pkts_recv, 4, false, 0 });
    r.params.set_counter (1011, Counter { counters_.pkts_sent, 4, false, 0 });
    r.params.set_counter (1012, Counter { counters_.mcpkts_recv, 4, false, 0 });
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
        if (const PtpCounters *c = counters ()) copy_counters (*c, r);
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
