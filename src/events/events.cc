#include "decnet/events/events.h"

#include "decnet/common/exceptions.h"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace decnet::events {

namespace {

// Days from 1970-01-01 to a civil date, and back.  Howard Hinnant's
// algorithms.  The event timestamp is an offset from 1 January 1977 in
// local wall clock time with no daylight saving correction, so it cannot
// come from a UTC clock reading: it has to be built from the broken down
// local time, and read back the same way.
constexpr long days_from_civil (long y, unsigned m, unsigned d) noexcept
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned> (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long> (doe) - 719468;
}

struct Civil { long y; unsigned m, d; };

constexpr Civil civil_from_days (long z) noexcept
{
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned> (z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = static_cast<long> (yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    return { y + (m <= 2), m, d };
}

// 1 January 1977, the base of the event timestamp.
constexpr long jbase_days = days_from_civil (1977, 1, 1);

constexpr const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

constexpr unsigned half_day = 12 * 60 * 60;

}   // namespace

Value node_value (const NiceNode &n)
{
    if (n.name.empty ()) return Value::cm ({ Value::du (n.id.value (), 2) });
    return Value::cm ({ Value::du (n.id.value (), 2), Value::ai (n.name) });
}

Event::Event (EventId i, Entity ent)
    : id (i), entity (std::move (ent))
{
    stamp_now ();
}

void Event::stamp_now ()
{
    timeval tv { };
    gettimeofday (&tv, nullptr);
    std::tm lt { };
    time_t secs = tv.tv_sec;
    localtime_r (&secs, &lt);

    long days = days_from_civil (lt.tm_year + 1900,
                                 static_cast<unsigned> (lt.tm_mon + 1),
                                 static_cast<unsigned> (lt.tm_mday))
              - jbase_days;
    unsigned sod = static_cast<unsigned> (lt.tm_hour) * 3600
                 + static_cast<unsigned> (lt.tm_min) * 60
                 + static_cast<unsigned> (lt.tm_sec);
    halfday = static_cast<std::uint16_t> (days * 2 + sod / half_day);
    seconds = static_cast<std::uint16_t> (sod % half_day);
    milliseconds = static_cast<std::uint16_t> (tv.tv_usec / 1000);
    ms_absent = false;
}

// ---------------------------------------------------------- convenience

Event &Event::param (std::uint16_t n, Value v)
{
    params.set (n, std::move (v));
    return *this;
}

Event &Event::coded (std::uint16_t n, std::uint64_t v, unsigned bytes)
{
    params.set (n, Value::c (v, bytes));
    return *this;
}

Event &Event::number (std::uint16_t n, std::uint64_t v, unsigned bytes)
{
    params.set (n, Value::du (v, bytes));
    return *this;
}

Event &Event::text (std::uint16_t n, std::string s)
{
    params.set (n, Value::ai (std::move (s)));
    return *this;
}

Event &Event::image (std::uint16_t n, Bytes b)
{
    params.set (n, Value::hi (std::move (b)));
    return *this;
}

Event &Event::counter (std::uint16_t n, std::uint64_t v, unsigned bytes)
{
    nice::Counter c;
    c.value = v;
    c.bytes = bytes;
    params.set_counter (n, c);
    return *this;
}

// -------------------------------------------------------------- encoding

void Event::encode (Encoder &e) const
{
    e.byte (function);
    e.byte (static_cast<std::uint8_t> ((console ? 1 : 0) | (file ? 2 : 0)
                                       | (monitor ? 4 : 0)));
    // Five bits of code and nine of class, with a spare bit between them.
    e.uint ((id.code & 0x1f) | (static_cast<std::uint32_t> (id.cls) << 6), 2);
    e.uint (halfday, 2);
    e.uint (seconds, 2);
    e.uint ((milliseconds & 0x3ff) | (ms_absent ? 0x8000u : 0u), 2);
    source.encode (e);
    entity.encode (e);
    params.encode (e);
}

Bytes Event::encode () const
{
    Bytes out;
    Encoder e (out);
    encode (e);
    return out;
}

Event Event::decode (Decoder &d)
{
    Event ev;
    ev.function = d.byte ();
    std::uint8_t flags = d.byte ();
    ev.console = (flags & 1) != 0;
    ev.file    = (flags & 2) != 0;
    ev.monitor = (flags & 4) != 0;
    std::uint16_t ec = static_cast<std::uint16_t> (d.uint (2));
    ev.id.code = static_cast<std::uint8_t> (ec & 0x1f);
    ev.id.cls  = static_cast<std::uint16_t> ((ec >> 6) & 0x1ff);
    ev.halfday = static_cast<std::uint16_t> (d.uint (2));
    ev.seconds = static_cast<std::uint16_t> (d.uint (2));
    std::uint16_t ms = static_cast<std::uint16_t> (d.uint (2));
    ev.milliseconds = static_cast<std::uint16_t> (ms & 0x3ff);
    ev.ms_absent = (ms & 0x8000) != 0;
    ev.source = NiceNode::decode (d);
    ev.entity = Entity::decode (d);
    ev.params.decode (d);
    return ev;
}

Event Event::parse (ByteView buf)
{
    Decoder d (buf);
    return decode (d);
}

// --------------------------------------------------------------- display

logging::Level Event::level () const
{
    const EventDef *def = find_event (id);
    return def ? def->level : logging::Level::info;
}

std::string Event::timestamp () const
{
    long total = static_cast<long> (seconds)
               + static_cast<long> (halfday % 2) * half_day;
    long days = halfday / 2 + total / 86400;
    total %= 86400;
    Civil c = civil_from_days (jbase_days + days);
    char buf[64];
    std::snprintf (buf, sizeof buf, "%02u-%s-%04ld %02ld:%02ld:%02ld",
                   c.d, month_names[c.m - 1], c.y,
                   total / 3600, (total / 60) % 60, total % 60);
    std::string s = buf;
    if (!ms_absent) {
        std::snprintf (buf, sizeof buf, ".%03u", milliseconds);
        s += buf;
    }
    return s;
}

std::string Event::str () const
{
    const EventDef *def = find_event (id);

    std::string out = "Event type " + id.str ();
    if (def) { out += ", "; out += def->text; }
    out += "\nFrom node " + source.str () + ", occurred " + timestamp ();

    // The body is laid out the way NICE.format does it: fields separated
    // by commas, gathered several to a line, each continuation line
    // indented under the header.
    constexpr std::size_t indent = 4;
    constexpr std::size_t width = 70 - (indent + 1);

    std::vector<std::string> lines;
    std::string cur;
    auto add = [&] (std::string s) {
        if (s.empty ()) return;
        s += ',';
        if (cur.size () + s.size () < width) {
            if (!cur.empty ()) cur += ' ';
            cur += s;
        } else {
            if (!cur.empty ()) lines.push_back (cur);
            cur = std::move (s);
        }
    };

    add (entity.str ());
    for (std::string &s : params.format (def ? def->params : ParamDefs { }))
        add (std::move (s));
    if (!cur.empty ()) lines.push_back (cur);

    for (const std::string &l : lines) {
        out += '\n';
        out.append (indent, ' ');
        out += l;
    }
    // Every field was given a trailing comma; the last one is not wanted.
    while (!out.empty () && out.back () == ',') out.pop_back ();
    return out;
}

}   // namespace decnet::events
