#include "decnet/events/logger.h"

#include "decnet/nice/nml.h"

#include "decnet/config.h"
#include "decnet/node.h"
#include "decnet/session/session.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace decnet::events {

namespace {

// How long to wait before trying a remote sink connection again.
constexpr double conn_retry = 30.0;

// How many records to hold for a remote sink that is not connected.
constexpr std::size_t queue_limit = 50;

// The object number the event logger listens on, and the version we claim.
constexpr std::uint8_t evl_object = 26;
constexpr std::uint8_t my_version[3] = { 4, 0, 0 };

const char *const sink_type_names[3] = { "console", "file", "monitor" };

int sink_type_id (const std::string &name)
{
    for (int i = 0; i < 3; ++i)
        if (name == sink_type_names[i]) return i;
    return -1;
}

// Read a run of digits, returning false if there are none.
bool read_uint (const std::string &s, std::size_t &i, unsigned &out)
{
    std::size_t start = i;
    unsigned v = 0;
    while (i < s.size () && std::isdigit (static_cast<unsigned char> (s[i]))) {
        v = v * 10 + static_cast<unsigned> (s[i] - '0');
        if (v > 100000) throw std::invalid_argument ("number too large in "
                                                     "event list");
        ++i;
    }
    out = v;
    return i != start;
}

}   // namespace

// ------------------------------------------------------------- parsing

EventSet parse_events (const std::string &s)
{
    if (s == "*.*") return filterable_events ();

    EventSet ret;
    bool have_class = false;
    unsigned cls = 0;

    std::size_t pos = 0;
    while (pos <= s.size ()) {
        std::size_t comma = s.find (',', pos);
        std::string item = s.substr (pos, comma == std::string::npos
                                          ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? s.size () + 1 : comma + 1;
        if (item.empty ()) continue;

        std::size_t i = 0;
        unsigned n = 0;
        bool digits = read_uint (item, i, n);

        // "N." names a class for this entry and every entry after it.
        if (digits && i < item.size () && item[i] == '.') {
            if (n >= 512)
                throw std::invalid_argument ("bad class in event list entry "
                                             + item);
            cls = n;
            have_class = true;
            ++i;
            digits = read_uint (item, i, n);
        } else if (!have_class) {
            throw std::invalid_argument ("missing event class in event list "
                                         "entry " + item);
        }

        unsigned first, last;
        if (!digits && i < item.size () && item[i] == '*') {
            ++i;
            first = 0;
            last = 31;
        } else if (digits) {
            first = n;
            last = n;
            if (i < item.size () && item[i] == '-') {
                ++i;
                unsigned hi = 0;
                if (!read_uint (item, i, hi))
                    throw std::invalid_argument ("bad range in event list "
                                                 "entry " + item);
                if (hi <= first)
                    throw std::invalid_argument ("bad range in event list "
                                                 "entry " + item);
                last = hi;
            }
        } else {
            throw std::invalid_argument ("bad event list entry " + item);
        }
        if (i != item.size ())
            throw std::invalid_argument ("bad event list entry " + item);
        if (last > 31)
            throw std::invalid_argument ("bad event numbers in entry " + item);

        for (unsigned e = first; e <= last; ++e)
            ret.insert (EventId { static_cast<std::uint16_t> (cls),
                                  static_cast<std::uint8_t> (e) });
    }
    return ret;
}

const EventSet &filterable_events ()
{
    // Built once from the catalogue.  Classes 31 to 479 belong to other
    // vendors' implementations: we can decode records from them, but we
    // never raise one, so there is nothing to filter.
    static const EventSet set = [] {
        EventSet s;
        for (const EventDef &d : known_events ())
            if (d.id.cls < 31 || d.id.cls >= 480) s.insert (d.id);
        return s;
    } ();
    return set;
}

// -------------------------------------------------------------- filter

void EventFilter::set_filter (const EventSet &events,
                              const std::string &entity_key, bool enable)
{
    for (EventId id : events) {
        // Only events this build can raise are worth filtering on.
        if (!filterable_events ().count (id)) continue;
        if (entity_key.empty ()) {
            if (enable) plain_.insert (id);
            else        plain_.erase (id);
        } else {
            if (enable) qualified_.insert ({ entity_key, id });
            else        qualified_.erase ({ entity_key, id });
        }
    }
}

bool EventFilter::contains (const Event &e) const
{
    if (plain_.count (e.id)) return true;
    return qualified_.count ({ e.entity.key (), e.id }) != 0;
}

std::string EventFilter::format () const
{
    // Collapse runs of consecutive codes within a class: "4.0-19,15".
    std::string out;
    auto it = plain_.begin ();
    while (it != plain_.end ()) {
        std::uint16_t cls = it->cls;
        if (!out.empty ()) out += ',';
        out += std::to_string (cls) + '.';
        bool first_run = true;
        while (it != plain_.end () && it->cls == cls) {
            std::uint8_t lo = it->code, hi = lo;
            ++it;
            while (it != plain_.end () && it->cls == cls
                   && it->code == hi + 1) {
                hi = it->code;
                ++it;
            }
            if (!first_run) out += ',';
            first_run = false;
            out += std::to_string (lo);
            if (hi != lo) out += '-' + std::to_string (hi);
        }
    }
    for (const auto &[key, id] : qualified_) {
        if (!out.empty ()) out += ',';
        out += key + '/' + id.str ();
    }
    return out;
}

// --------------------------------------------------------------- sinks

unsigned EventSink::sinkmask (const Event &e) const
{
    return filter_.contains (e) ? 1u : 0u;
}

void EventSink::logevent (const Event &e)
{
    unsigned m = sinkmask (e);
    if (m) writeevent (e, m);
}

void LocalConsole::writeevent (const Event &e, unsigned)
{
    logging::Level l = e.level ();
    if (logging::enabled (l)) logging::emit (l, e.str ());
}

LocalFile::LocalFile (Element *parent, std::string path)
    : EventSink (parent), path_ (std::move (path))
{
}

LocalFile::~LocalFile ()
{
    if (f_) std::fclose (f_);
}

void LocalFile::start ()
{
    f_ = std::fopen (path_.c_str (), "ab");
    if (!f_)
        DN_ERROR ("cannot open event log file {}", path_);
}

void LocalFile::stop ()
{
    if (f_) { std::fclose (f_); f_ = nullptr; }
}

void LocalFile::writeevent (const Event &e, unsigned)
{
    if (!f_) return;
    Bytes b = e.encode ();
    // A two byte little endian length in front of each record: the RMS
    // variable length record format, so the file reads on the systems this
    // protocol came from.
    std::uint8_t len[2] = { static_cast<std::uint8_t> (b.size () & 0xff),
                            static_cast<std::uint8_t> (b.size () >> 8) };
    std::fwrite (len, 1, 2, f_);
    std::fwrite (b.data (), 1, b.size (), f_);
    std::fflush (f_);
}

void LocalMonitor::register_monitor (Callback cb, const EventSet &events)
{
    cb_ = std::move (cb);
    filter_.set_filter (events);
    DN_DEBUG ("logging monitor initialized, events {}", filter_.format ());
}

void LocalMonitor::writeevent (const Event &e, unsigned)
{
    if (cb_) cb_ (e);
}

// --------------------------------------------------------- remote sink

namespace {

// The application side of a remote sink's connection.  It does nothing but
// tell the sink when the link comes up and when it goes away; the sink
// owns the queue and decides what to send.
class RemoteSinkApp : public session::Application {
public:
    explicit RemoteSinkApp (RemoteSink *sink) noexcept : sink_ (sink) {}

    void connect_received (session::SessionConnection &c, ByteView) override
    { sink_->link_up (&c); }

    void data_received (session::SessionConnection &, ByteView) override {}

    void disconnected (session::SessionConnection &, unsigned reason) override
    {
        DN_TRACE ("event sender link down, reason {}", reason);
        sink_->link_down ();
    }

private:
    RemoteSink *sink_;
};

}   // namespace

RemoteSink::RemoteSink (Element *parent, const LoggingConfig &config)
    : EventSink (parent),
      node_ (config.sink_node),
      username_ (config.sink_username),
      password_ (config.sink_password),
      account_ (config.sink_account)
{
}

RemoteSink::~RemoteSink () = default;

EventFilter &RemoteSink::filter (unsigned sink_type)
{
    return filters_[sink_type < 3 ? sink_type : 0];
}

unsigned RemoteSink::sinkmask (const Event &e) const
{
    unsigned m = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (filters_[i].contains (e)) m |= 1u << i;
    return m;
}

void RemoteSink::writeevent (const Event &e, unsigned mask)
{
    Event copy = e;
    // The record itself says which of the far end's sinks asked for it.
    copy.console = (mask & 1) != 0;
    copy.file    = (mask & 2) != 0;
    copy.monitor = (mask & 4) != 0;

    if (queue_.size () >= queue_limit) {
        // Full.  Say so once, rather than dropping records silently: a
        // reader that sees "events lost" knows there is a hole.
        if (!queue_.empty () && queue_.back ().id == EventId { 0, 0 }) return;
        Event lost { { 0, 0 }, nice::Entity::make_none () };
        lost.source = copy.source;
        queue_.back () = std::move (lost);
        return;
    }
    queue_.push_back (std::move (copy));
    send_events ();
}

void RemoteSink::start ()
{
    // Deliberately no connection attempt here.  Sinks start before the
    // routing and session layers do, so a connection opened now would be
    // to an unreachable node, fail, and put us into the thirty second
    // retry cycle with nothing yet to send.  The link is opened when
    // there is a first record for it.
    stopped_ = false;
}

void RemoteSink::stop ()
{
    stopped_ = true;
    if (conn_) conn_->disconnect ();
    conn_ = nullptr;
    connecting_ = false;
    if (node ()) node ()->timers ().stop (this);
}

void RemoteSink::link_up (session::SessionConnection *c)
{
    DN_TRACE ("event sender connected to {}", node_);
    conn_ = c;
    connecting_ = false;
    send_events ();
}

void RemoteSink::link_down ()
{
    conn_ = nullptr;
    connecting_ = false;
    if (!stopped_ && node ()) node ()->timers ().start (this, conn_retry);
}

void RemoteSink::timeout ()
{
    send_events ();
}

void RemoteSink::dispatch (Work &w)
{
    if (dynamic_cast<Timeout *> (&w)) timeout ();
}

void RemoteSink::send_events ()
{
    if (stopped_) return;

    if (conn_) {
        // send_data goes straight down through session control, NSP and
        // routing, and anything down there may raise an event -- which
        // arrives back here, is queued, and used to re-enter this very
        // loop.  The inner call then sent and popped the record the outer
        // one was still holding, and the outer pop_front destroyed an
        // element that was no longer there.  That is a crash in the
        // Event destructor with nothing in the log to explain it.
        //
        // So: one loop at a time, and each record leaves the queue before
        // it is sent rather than after.
        if (sending_) return;
        sending_ = true;
        while (!queue_.empty () && conn_ && conn_->running ()) {
            Event e = std::move (queue_.front ());
            queue_.pop_front ();
            DN_TRACE ("event sender: sending {} to {}", e.id.str (), node_);
            conn_->send_data (e.encode ());
        }
        sending_ = false;
        return;
    }
    if (connecting_ || node_.empty ()) return;

    session::Session *s = node () ? node ()->session () : nullptr;
    if (!s) return;
    Nodeinfo *info = node ()->find_node (node_);
    if (!info) {
        DN_ERROR ("error opening logging connection to {}: unknown node",
                  node_);
        node_.clear ();
        return;
    }

    DN_TRACE ("event sender connecting to {}, {} queued", node_,
              queue_.size ());
    session::ConnectData cd;
    cd.dstname     = session::EndUser::number (evl_object);
    cd.srcname     = session::EndUser::named ("EVENTLOGGER");
    // The connect data is our protocol version.
    cd.connectdata = Bytes (std::begin (my_version), std::end (my_version));
    cd.rqstrid     = username_;
    cd.passwrd     = password_;
    cd.account     = account_;
    cd.auth        = !username_.empty () || !password_.empty ()
                  || !account_.empty ();

    session::SessionConnection *c =
        s->connect (info->id, std::move (cd),
                    std::make_unique<RemoteSinkApp> (this));
    if (!c) {
        if (node ()) node ()->timers ().start (this, conn_retry);
        return;
    }
    connecting_ = true;
}

// -------------------------------------------------------- event logger

EventLogger::EventLogger (Element *parent, const Config &config)
    : Element (parent)
{
    const auto &lines = config.logging ();
    if (lines.empty ()) {
        // No configuration at all: the console, with everything enabled.
        auto s = std::make_unique<LocalConsole> (this);
        s->filter ().set_filter (filterable_events ());
        local_["console"] = s.get ();
        sinks_.push_back (std::move (s));
        return;
    }

    for (const LoggingConfig &c : lines) {
        int type = sink_type_id (c.type);
        if (type < 0) continue;             // the parser already refused it
        EventFilter *f = nullptr;

        if (!c.sink_node.empty ()) {
            auto it = remote_.find (c.sink_node);
            RemoteSink *rs;
            if (it == remote_.end ()) {
                auto s = std::make_unique<RemoteSink> (this, c);
                rs = s.get ();
                remote_[c.sink_node] = rs;
                sinks_.push_back (std::move (s));
            } else {
                rs = it->second;
            }
            f = &rs->filter (static_cast<unsigned> (type));
        } else {
            auto it = local_.find (c.type);
            if (it == local_.end ()) {
                std::unique_ptr<EventSink> s;
                switch (type) {
                case 0:  s = std::make_unique<LocalConsole> (this); break;
                case 1:  s = std::make_unique<LocalFile> (this, c.sink_file); break;
                default: s = std::make_unique<LocalMonitor> (this); break;
                }
                local_[c.type] = s.get ();
                sinks_.push_back (std::move (s));
            }
            f = &local_[c.type]->filter ();
        }

        if (!c.events.empty ()) {
            f->set_filter (parse_events (c.events));
        } else if (type == 0 && c.sink_node.empty ()) {
            // A local console with no event list gets everything, which is
            // what an operator expects from a console.
            f->set_filter (filterable_events ());
        }

        std::string what = c.sink_node.empty ()
            ? "local sink " + c.type
            : "sink node " + c.sink_node + " " + c.type;
        DN_DEBUG ("initialized {}, events {}", what, f->format ());
    }
}

EventLogger::~EventLogger () = default;

void EventLogger::start ()
{
    for (auto &s : sinks_) s->start ();
}

void EventLogger::stop_remote ()
{
    for (auto &[name, s] : remote_) s->stop ();
}

void EventLogger::stop ()
{
    for (auto &s : sinks_) s->stop ();
}

void EventLogger::logevent (const Event &e)
{
    for (auto &s : sinks_) s->logevent (e);
}

void EventLogger::logremoteevent (const Event &e)
{
    // The originating node already applied its filters, so the only
    // question is which of our local sinks the record asks for.
    static bool Event::*const wanted[3] = { &Event::console, &Event::file,
                                           &Event::monitor };
    for (int i = 0; i < 3; ++i) {
        auto it = local_.find (sink_type_names[i]);
        if (it == local_.end () || !(e.*wanted[i])) continue;
        it->second->logevent (e);
    }
}

void EventLogger::register_monitor (LocalMonitor::Callback cb,
                                    const EventSet &events)
{
    auto it = local_.find ("monitor");
    if (it == local_.end ()) return;
    static_cast<LocalMonitor *> (it->second)->register_monitor (std::move (cb),
                                                                events);
}

void EventLogger::nice_read (const nice::NiceRequest &, nice::ReplyDict &)
{
    // Deliberately empty, matching EventLogger.nice_read upstream.
    //
    // SHOW LOGGING asks which events each sink is set to record, and the
    // architected answer is a logging entity carrying an event list per
    // sink.  The parameter encoding for that -- the event list format in a
    // NICE reply, which is not the same as the one in an event record --
    // is not implemented in the Python either, so there is nothing here to
    // port and no wire format to check a guess against.  Answering nothing
    // makes the listener report "unrecognized component", which is honest:
    // we do not have this information in the form NCP asked for it.
    //
    // The data itself is not missing.  local_filter() has every sink's
    // event set, and the monitoring page prints it.
}

EventFilter *EventLogger::local_filter (const std::string &type)
{
    auto it = local_.find (type);
    return it == local_.end () ? nullptr : &it->second->filter ();
}

RemoteSink *EventLogger::remote_sink (const std::string &node)
{
    auto it = remote_.find (node);
    return it == remote_.end () ? nullptr : it->second;
}

// ---------------------------------------------------- the receiving end

namespace {

// Object 26.  The far end connects, we accept, and every message after
// that is one encoded event record.
class EventReceiver : public session::Application {
public:
    explicit EventReceiver (Node *node) noexcept : node_ (node) {}

    void connect_received (session::SessionConnection &c, ByteView data) override
    {
        // The connect data is the sender's protocol version.  Nothing here
        // depends on it, so it is logged and accepted.
        std::string v;
        for (std::uint8_t b : data) {
            if (!v.empty ()) v += '.';
            v += std::to_string (b);
        }
        DN_DEBUG ("event receiver: connection from {} version {}",
                  c.remote ().str (), v.empty () ? "unknown" : v);
        c.accept ();
    }

    void data_received (session::SessionConnection &c, ByteView data) override
    {
        Event e;
        try {
            e = Event::parse (data);
        } catch (const std::exception &err) {
            DN_DEBUG ("event receiver: bad record from {}: {}",
                      c.remote ().str (), err.what ());
            return;
        }
        DN_TRACE ("event receiver: record {} from {}", e.id.str (),
                  e.source.str ());
        if (node_ && node_->event_logger ())
            node_->event_logger ()->logremoteevent (e);
    }

private:
    Node *node_;
};

}   // namespace

std::unique_ptr<session::Application> make_event_receiver (Node *node)
{
    return std::make_unique<EventReceiver> (node);
}

}   // namespace decnet::events
