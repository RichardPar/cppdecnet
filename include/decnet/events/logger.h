// decnet/events/logger.h -- event sinks.
//
// Port of event_logger.py.  Each sink has a filter: a set of event
// numbers, optionally qualified by entity.  Every event is offered to
// every sink.
//
// Local sinks are console, file and monitor.  A remote sink is another
// node's event logger, reached over a logical link to object 26; one
// connection carries records for all three of the far end's sink types.
//
// While a remote sink is unreachable, events queue.  When the queue is full
// an "events lost" record is queued instead.

#ifndef DECNET_EVENTS_LOGGER_H
#define DECNET_EVENTS_LOGGER_H

#include "decnet/common/element.h"
#include "decnet/common/timers.h"
#include "decnet/events/events.h"
#include "decnet/nice/packets.h"

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace decnet {
class Config;
struct LoggingConfig;
namespace session { class Application; class Session; class SessionConnection; }
}

namespace decnet::nice { class ReplyDict; }

namespace decnet::events {

// A set of event numbers.  Ordered so that formatting can collapse runs.
using EventSet = std::set<EventId>;

// Parse an event list: "3.1,4.1-12,5.2,4,7" or "*.*".  Accepts the NM
// spec event-list format plus PyDECnet's extension of several classes in
// one list.  Throws std::invalid_argument on error.
EventSet parse_events (const std::string &s);

// All events a filter may select: those this build can raise.
const EventSet &filterable_events ();

class EventFilter {
public:
    // Add (or with enable false, remove) events.  An entity key qualifies
    // the entry: it then matches only events about that entity.
    void set_filter (const EventSet &events, const std::string &entity_key = { },
                     bool enable = true);

    bool contains (const Event &e) const;
    bool empty () const noexcept { return plain_.empty () && qualified_.empty (); }

    // The event list form, as NCP prints it: "4.7-10,15", one line.
    std::string format () const;

private:
    EventSet                                    plain_;
    std::set<std::pair<std::string, EventId>>   qualified_;
};

// ------------------------------------------------------------------ sinks

class EventSink : public Element {
public:
    explicit EventSink (Element *parent) : Element (parent) {}

    EventFilter &filter () noexcept { return filter_; }

    // Offer an event.  The sink decides whether it wants it.
    void logevent (const Event &e);

    virtual void start () {}
    virtual void stop () {}

    // A sink is an Element so that it can reach the node, but only the
    // remote sink has work addressed to it.
    void dispatch (Work &) override {}

protected:
    // Which of the far end's sinks want this, as a bitmask of
    // console/file/monitor.  A local sink answers 1 or 0.
    virtual unsigned sinkmask (const Event &e) const;
    virtual void writeevent (const Event &e, unsigned mask) = 0;

    EventFilter filter_;
};

// The console: the record goes through the ordinary log at the severity
// the event definition gives it.
class LocalConsole : public EventSink {
public:
    explicit LocalConsole (Element *parent) : EventSink (parent) {}

protected:
    void writeevent (const Event &e, unsigned mask) override;
};

// File of encoded records, each preceded by a two byte little endian
// length (RMS variable length record format).
class LocalFile : public EventSink {
public:
    LocalFile (Element *parent, std::string path);
    ~LocalFile () override;

    void start () override;
    void stop () override;

protected:
    void writeevent (const Event &e, unsigned mask) override;

private:
    std::string path_;
    std::FILE  *f_ = nullptr;
};

// The monitoring interface.  Nothing subscribes yet; a monitor registers a
// callback and the events it wants.
class LocalMonitor : public EventSink {
public:
    using Callback = std::function<void (const Event &)>;

    explicit LocalMonitor (Element *parent) : EventSink (parent) {}

    void register_monitor (Callback cb, const EventSet &events);

protected:
    void writeevent (const Event &e, unsigned mask) override;

private:
    Callback cb_;
};

// Another node's event logger, reached over a logical link to object 26.
//
// Holds a filter for each of the far end's sink types and marks each
// record with the sinks that want it.  Records queue while the link is
// down; when the queue is full the newest entry becomes an "events lost"
// record.
class RemoteSink : public EventSink, public Timer {
public:
    RemoteSink (Element *parent, const LoggingConfig &config);
    ~RemoteSink () override;

    void start () override;
    void stop () override;

    // One filter per far end sink type: 0 console, 1 file, 2 monitor.
    EventFilter &filter (unsigned sink_type);

    // Timer, for the connection retry.
    void timeout () override;
    Element *timer_owner () noexcept override { return this; }

    void dispatch (Work &w) override;

    // Called by the connection's Application as the link comes and goes.
    void link_up (session::SessionConnection *c);
    void link_down ();

    std::size_t queued () const noexcept { return queue_.size (); }

    // Is the logical link open?  Connects are retried every conn_retry
    // seconds.
    bool connected () const noexcept { return conn_ != nullptr; }

protected:
    unsigned sinkmask (const Event &e) const override;
    void writeevent (const Event &e, unsigned mask) override;

private:
    void send_events ();

    EventFilter  filters_[3];
    std::string  node_, username_, password_, account_;
    std::deque<Event> queue_;
    session::SessionConnection *conn_ = nullptr;
    bool         connecting_ = false;
    bool         stopped_ = false;
    // True while send_events is running.  See send_events.
    bool         sending_ = false;
};

// ------------------------------------------------------------ event logger

class EventLogger : public Element {
public:
    EventLogger (Element *parent, const Config &config);
    ~EventLogger () override;

    void start ();
    void stop ();

    // Close links to remote sinks.  Called before session control and NSP
    // stop.  Local sinks keep working until stop().
    void stop_remote ();

    void dispatch (Work &) override {}

    // Offer a locally generated event to every sink.
    void logevent (const Event &e);

    // A record received from another node.  Already filtered at the source,
    // so it goes to the local sinks the record names.
    void logremoteevent (const Event &e);

    void register_monitor (LocalMonitor::Callback cb, const EventSet &events);

    // The filter for a local sink type, for tests and for NCP.
    EventFilter *local_filter (const std::string &type);

    // NICE read for logging.  Port of EventLogger.nice_read, a stub upstream.
    void nice_read (const nice::NiceRequest &req, nice::ReplyDict &resp);

    // The remote sink for a node name, for tests.
    RemoteSink *remote_sink (const std::string &node);

private:
    // Local sinks by type name, plus remote sinks by node name.
    std::vector<std::unique_ptr<EventSink>>      sinks_;
    std::map<std::string, EventSink *>           local_;
    std::map<std::string, RemoteSink *>          remote_;
};

// Object 26: receives event records from other nodes and passes them to
// the local sinks.
std::unique_ptr<session::Application> make_event_receiver (Node *node);

}   // namespace decnet::events

#endif  // DECNET_EVENTS_LOGGER_H
