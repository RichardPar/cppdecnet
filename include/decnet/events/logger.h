// decnet/events/logger.h -- where event records go.
//
// Port of event_logger.py.  The architecture gives every sink its own
// filter, and a filter is a set of event numbers, optionally qualified by
// the entity the event is about.  An event is offered to every sink; each
// one decides for itself.
//
// There are three local sink types -- the console, a file, and the
// monitoring interface -- and a remote sink, which is another node's event
// logger reached over a logical link to object 26.  A remote sink is one
// connection carrying up to three filters, because the record says which
// of the far end's own sinks it is destined for.
//
// The one subtlety is what happens when the far end is unreachable: events
// queue, the queue is bounded, and when it fills the oldest entry becomes
// an "events lost" record rather than being dropped silently.

#ifndef DECNET_EVENTS_LOGGER_H
#define DECNET_EVENTS_LOGGER_H

#include "decnet/common/element.h"
#include "decnet/common/timers.h"
#include "decnet/events/events.h"

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

namespace decnet::events {

// A set of event numbers.  Ordered so that formatting can collapse runs.
using EventSet = std::set<EventId>;

// Parse an event list: "3.1,4.1-12,5.2,4,7" or "*.*".  This accepts what
// the Network Management specification calls an event-list, plus the
// extension pydecnet allows of naming several classes in one list.  Throws
// std::invalid_argument on anything else.
EventSet parse_events (const std::string &s);

// Every event a filter may select: the ones this build can actually raise.
// Filtering applies to locally generated events, so enabling one we never
// generate would do nothing, and events belonging to other vendors'
// implementations (classes 31 to 479) are excluded for the same reason.
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

// A file of encoded records, each with a two byte little endian length in
// front of it.  That is the RMS variable length record format, so the file
// can be read on the operating systems this protocol came from.
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
// One connection carries records for all three of the far end's sink
// types, so this holds three filters and stamps each record with the ones
// that wanted it.  While the link is down records queue; the queue is
// bounded, and when it fills the newest entry is replaced by an "events
// lost" record, so the far end learns that there is a hole rather than
// seeing a shorter history than really happened.
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
};

// ------------------------------------------------------------ event logger

class EventLogger : public Element {
public:
    EventLogger (Element *parent, const Config &config);
    ~EventLogger () override;

    void start ();
    void stop ();

    // Close the links to remote sinks.  A remote sink is an application
    // sitting on session control, so it has to let go before session
    // control and NSP are torn down under it -- the same reason session
    // control stops before NSP does.  Local sinks keep working until
    // stop(), so events raised while the layers below are shutting down
    // still reach the console and the log file.
    void stop_remote ();

    void dispatch (Work &) override {}

    // Offer a locally generated event to every sink.
    void logevent (const Event &e);

    // A record that arrived from another node.  Filtering already happened
    // at the source, so this goes straight to whichever local sinks the
    // record itself asks for.
    void logremoteevent (const Event &e);

    void register_monitor (LocalMonitor::Callback cb, const EventSet &events);

    // The filter for a local sink type, for tests and for NCP.
    EventFilter *local_filter (const std::string &type);

    // The remote sink for a node name, for tests.
    RemoteSink *remote_sink (const std::string &node);

private:
    // Local sinks by type name, plus remote sinks by node name.
    std::vector<std::unique_ptr<EventSink>>      sinks_;
    std::map<std::string, EventSink *>           local_;
    std::map<std::string, RemoteSink *>          remote_;
};

// The receiving half of remote logging: object 26, which another node
// connects to in order to send us its event records.  It reads a record per
// message and hands it to the local sinks the record asks for.
std::unique_ptr<session::Application> make_event_receiver (Node *node);

}   // namespace decnet::events

#endif  // DECNET_EVENTS_LOGGER_H
