// src/mop/nice.cc -- what MOP tells network management.
//
// Port of Mop.nice_read.  The only module MOP has is the configurator,
// which reports what other stations on each circuit have announced about
// themselves in their system id messages.  That is the one NCP command
// that shows a machine which has said nothing to routing at all: a station
// only has to be powered on and talking MOP to appear here.
//
// One circuit produces one reply per station heard, and they travel as a
// group -- all but the last carry "more to come for this entity" -- which
// is what ReplyDict::add_named is for.  A circuit that has heard nothing
// still gets one reply, so that NCP prints the circuit with its
// surveillance state rather than leaving it out.

#include "decnet/mop/mop.h"
#include "decnet/nice/nml.h"
#include "decnet/node.h"

#include <ctime>

namespace decnet::mop {

using nice::Entity;
using nice::NiceReply;
using nice::NiceRequest;
using nice::ReplyDict;
using nice::Value;

namespace {

constexpr const char *configurator = "CONFIGURATOR";

// The functions bitmap, as a list of the bit numbers that are set.  the Python
// builds the same list by walking the seven service flags in order.
Value functions_value (const SysId &s)
{
    nice::Value::List bits;
    const bool flags[] = { s.loop, s.dump, s.ploader, s.sloader,
                           s.boot, s.carrier, s.counters };
    for (unsigned i = 0; i < 7; ++i)
        if (flags[i]) bits.push_back (Value::du (i));
    return Value::cm (std::move (bits));
}

// "Last report" is a date and time without a year: day, month, hour,
// minute, second.  Port of the time.localtime call in Mop.nice_read.
Value last_report_value (std::chrono::system_clock::time_point t)
{
    std::time_t tt = std::chrono::system_clock::to_time_t (t);
    std::tm tm {};
    ::localtime_r (&tt, &tm);
    return Value::cm ({ Value::du (static_cast<unsigned> (tm.tm_mday)),
                        Value::du (static_cast<unsigned> (tm.tm_mon + 1)),
                        Value::du (static_cast<unsigned> (tm.tm_hour)),
                        Value::du (static_cast<unsigned> (tm.tm_min)),
                        Value::du (static_cast<unsigned> (tm.tm_sec)) });
}

// Elapsed time, as hours, minutes and seconds.
Value elapsed_value (double seconds)
{
    auto total = static_cast<unsigned long> (seconds < 0 ? 0 : seconds);
    return Value::cm ({ Value::du (total / 3600, 2),
                        Value::du ((total / 60) % 60),
                        Value::du (total % 60) });
}

// What one station said about itself.
void fill_station (const HeardSystem &h, const std::string &circuit,
                   NiceReply &r)
{
    const SysId &s = h.sysid;
    r.params.set (100, Value::ai (circuit));
    const auto &b = h.address.bytes ();
    r.params.set (120, Value::hi (Bytes (b.begin (), b.end ())));
    r.params.set (130, last_report_value (h.last_report));
    if (s.loop || s.dump || s.ploader || s.sloader || s.boot || s.carrier
        || s.counters)
        r.params.set (1002, functions_value (s));
    if (s.version)
        r.params.set (1001, Value::cm ({ Value::du (s.version->v1),
                                         Value::du (s.version->v2),
                                         Value::du (s.version->v3) }));
    if (s.console_user) r.params.set (1003, Value::hi (*s.console_user));
    if (s.reservation_timer)
        r.params.set (1004, Value::du (*s.reservation_timer, 2));
    if (s.console_cmd_size)
        r.params.set (1005, Value::du (*s.console_cmd_size, 2));
    if (s.console_resp_size)
        r.params.set (1006, Value::du (*s.console_resp_size, 2));
    if (s.hwaddr) r.params.set (1007, Value::hi (*s.hwaddr));
    if (s.device) r.params.set (1100, Value::c (*s.device));
    if (s.software) {
        // A software id is either a name or one of three well known codes,
        // and NCP prints the two differently.
        if (s.software->is_code)
            r.params.set (1200, Value::cm ({ Value::ds (s.software->code) }));
        else
            r.params.set (1200, Value::cm ({ Value::du (0),
                                             Value::ai (s.software->text) }));
    }
    if (s.processor) r.params.set (1300, Value::c (*s.processor));
    if (s.datalink)  r.params.set (1400, Value::c (*s.datalink));
    if (s.bufsize)   r.params.set (1401, Value::du (*s.bufsize, 2));
}

}   // namespace

double SysIdHandler::elapsed () const noexcept
{
    std::chrono::duration<double> dt =
        std::chrono::steady_clock::now () - started_;
    return dt.count ();
}

void Mop::nice_read (const NiceRequest &req, ReplyDict &resp)
{
    // The configurator is a status item: there is nothing to say about it
    // in a characteristics or counters read.
    if (req.entity_type != Entity::module || !req.sumstat ()) return;
    // Either a plural request, or the one module we have by name.
    if (req.entity.one () && !req.entity.match (std::string (configurator)))
        return;

    // A circuit qualifier narrows it to one circuit.
    const std::string *qual = nullptr;
    if (req.has_qual_circuit && !req.qual_circuit.empty ())
        qual = &req.qual_circuit;

    for (MopCircuit *c : order_) {
        if (qual && c->name () != *qual) continue;
        SysIdHandler *sysid = c->sysid ();
        if (!sysid) continue;

        NiceReply *first = nullptr;
        if (req.stat ()) {
            // Status: one reply per station heard, in address order, which
            // is the order the map already keeps them in.
            for (const auto &[key, h] : sysid->heard ()) {
                NiceReply &r = resp.add_named (configurator);
                if (!first) first = &r;
                fill_station (h, c->name (), r);
            }
        }
        if (!first) {
            // Nothing heard, or a summary read: the circuit still gets an
            // entry, so that it shows up at all.
            first = &resp.add_named (configurator);
            first->params.set (100, Value::ai (c->name ()));
        }
        // Surveillance and elapsed time go on the first reply for the
        // circuit, because they are about the circuit rather than about
        // any one station on it.
        first->params.set (110, Value::c (0));      // Surveillance disabled
        first->params.set (111, elapsed_value (sysid->elapsed ()));
    }
}

}   // namespace decnet::mop
