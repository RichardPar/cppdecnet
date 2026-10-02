// src/common/namelearner.cc -- node names from the network itself.  See
// the header.

#include "decnet/namelearner.h"

#include "decnet/common/logging.h"
#include "decnet/nice/nml.h"
#include "decnet/nice/packets.h"
#include "decnet/node.h"
#include "decnet/routing/routing.h"
#include "decnet/session/session.h"

#include <memory>

namespace decnet {

namespace nm = nice;

namespace {

// How often to look at the neighbours.  A new adjacency is asked within
// this long of coming up.
constexpr double TICK = 5.0;

// One question to one node's network management listener, object 19.
class NameQuery : public session::Application {
public:
    NameQuery (NameLearner *learner, Nodeid id, bool known)
        : learner_ (learner), id_ (id), known_ (known) {}

    void connect_received (session::SessionConnection &c, ByteView) override
    {
        // Our connect was accepted: ask.
        nm::NiceRequest r;
        r.function = nm::fn_read;
        r.entity_type = nm::Entity::node;
        r.entity = known_
            ? nm::ReqEntity::make_wild (nm::Entity::node, nm::ReqEntity::known)
            : nm::ReqEntity::make_node (Nodeid ());     // the executor
        r.info = nm::info_summary;
        c.send_data (r.encode ());
    }

    void data_received (session::SessionConnection &c, ByteView data) override
    {
        try {
            nm::NiceReply hdr = nm::NiceReply::parse_header (data);
            if (hdr.retcode == nm::rc_multiple) { multiple_ = true; return; }
            if (hdr.retcode == nm::rc_done || hdr.retcode < 0) {
                c.disconnect ();
                return;
            }
            nm::NiceReply reply = nm::NiceReply::parse (data, nm::Entity::node);
            if (reply.has_entity && reply.entity.kind () == nm::Entity::node) {
                const nm::NiceNode &n = reply.entity.as_node ();
                if (!n.name.empty ()) learner_->found (n.id, n.name);
            }
        } catch (const std::exception &e) {
            // One reply we can't read is no reason to stop reading.
            DN_DEBUG ("names from {}: unreadable reply: {}", id_.str (),
                      e.what ());
        }
        if (!multiple_) c.disconnect ();
    }

    void disconnected (session::SessionConnection &, unsigned) override
    {
        learner_->finished (id_);
    }

private:
    NameLearner *learner_;
    Nodeid       id_;
    bool         known_;
    bool         multiple_ = false;
};

}   // namespace

NameLearner::NameLearner (Node *node, unsigned refresh)
    : node_ (node), refresh_ (refresh), timer_ ([this] { tick (); })
{
}

void NameLearner::start ()
{
    node_->timers ().start (&timer_, TICK);
}

void NameLearner::stop ()
{
    node_->timers ().stop (&timer_);
}

bool NameLearner::asked_lately (Nodeid id) const
{
    auto it = asked_.find (id.value ());
    return it != asked_.end () && Clock::now () - it->second < refresh_;
}

void NameLearner::tick ()
{
    if (auto *r = node_->routing ()) {
        for (const auto &[key, adj] : r->adjacencies ()) {
            if (!adj) continue;
            Nodeid id = adj->nodeid ();
            if (id == node_->id () || busy_.count (id.value ())
                || asked_lately (id))
                continue;
            ask (id, true);
        }
    }
    if (learned_ != learned_before_) {
        DN_INFO ("{} node names learned from the network so far", learned_);
        learned_before_ = learned_;
    }
    node_->timers ().start (&timer_, TICK);
}

void NameLearner::link_running (Nodeid id)
{
    if (id == node_->id () || busy_.count (id.value ()) || asked_lately (id))
        return;
    const Nodeinfo *info = node_->find_node (id, false);
    if (info && !info->name.empty ()) return;
    ask (id, false);
}

void NameLearner::ask (Nodeid id, bool known)
{
    auto *s = node_->session ();
    if (!s) return;
    asked_[id.value ()] = Clock::now ();
    DN_DEBUG ("asking {} for {}", id.str (),
              known ? "the names it knows" : "its name");
    // Mark it busy before connecting: a refusal can come back before
    // connect returns.
    busy_.insert (id.value ());
    auto *sc = s->connect (id, session::EndUser::number (19),
                           session::EndUser::named ("NAMES"),
                           Bytes (std::begin (nm::nice_version),
                                  std::end (nm::nice_version)),
                           std::make_unique<NameQuery> (this, id, known));
    if (!sc) busy_.erase (id.value ());
}

void NameLearner::found (Nodeid id, const std::string &name)
{
    if (node_->learn_node_name (id, name)) {
        ++learned_;
        DN_DEBUG ("learned {} is {}", id.str (), name);
    }
}

void NameLearner::finished (Nodeid id)
{
    busy_.erase (id.value ());
}

}   // namespace decnet
