// TIMESTAMP: tells the caller the time.  Port of a PyDECnet contributed
// module.
//
// Each data message is a two character request.  The first says which
// clock: 'U' for UTC, anything else for local time.  The second says how
// to answer: 'B' for a VMS binary time (a 64 bit little endian count of
// 100 ns units since 17-NOV-1858), anything else for VMS ASCII text such
// as "27-SEP-2026:12:58:08.33".

#include "decnet/session/session.h"

#include "decnet/common/platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace decnet::session {

namespace {

// Seconds from the VMS epoch, 17-NOV-1858, to the Unix epoch.
constexpr std::int64_t vms_epoch_offset = 40587LL * 86400;

constexpr const char *months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

class Timestamp : public Application {
public:
    void connect_received (SessionConnection &c, ByteView) override
    {
        c.accept ();
    }

    void data_received (SessionConnection &c, ByteView data) override
    {
        c.send_data (timestamp_reply (data, std::chrono::system_clock::now ()));
    }
};

}   // namespace

Bytes timestamp_reply (ByteView request, std::chrono::system_clock::time_point now)
{
    // A short request takes the defaults, local time as text, where the
    // Python original would fail.
    bool utc    = request.size () > 0 && request[0] == 'U';
    bool binary = request.size () > 1 && request[1] == 'B';

    // Python datetime has microsecond resolution; so does the answer.
    auto us = std::chrono::duration_cast<std::chrono::microseconds> (
        now.time_since_epoch ()).count ();
    std::int64_t secs = us / 1000000;
    std::int64_t frac = us % 1000000;
    if (frac < 0) { frac += 1000000; --secs; }

    std::time_t t = static_cast<std::time_t> (secs);
    std::tm tm {};
    if (utc) gmtime_r (&t, &tm);
    else     localtime_r (&t, &tm);

    if (binary) {
        // The wall clock time in the chosen zone, counted from the VMS
        // epoch as though it were UTC -- what a VMS system clock holds.
#ifdef _WIN32
        // No tm_gmtoff: the offset is the broken-down time read back as UTC.
        std::tm copy = tm;
        std::int64_t gmtoff = utc ? 0 : static_cast<std::int64_t> (::_mkgmtime (&copy)) - secs;
#else
        std::int64_t gmtoff = utc ? 0 : tm.tm_gmtoff;
#endif
        std::int64_t wall = secs + gmtoff;
        auto vms = static_cast<std::uint64_t> (
            (wall + vms_epoch_offset) * 10000000 + frac * 10);
        Bytes out (8);
        for (int i = 0; i < 8; ++i)
            out[i] = static_cast<std::uint8_t> (vms >> (8 * i));
        return out;
    }

    char buf[48];
    int n = std::snprintf (buf, sizeof buf, "%02d-%s-%04d:%02d:%02d:%02d.%02d",
                           tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
                           tm.tm_hour, tm.tm_min, tm.tm_sec,
                           static_cast<int> (frac / 10000));
    return Bytes (buf, buf + n);
}

std::unique_ptr<Application> make_timestamp ()
{
    return std::make_unique<Timestamp> ();
}

}   // namespace decnet::session
