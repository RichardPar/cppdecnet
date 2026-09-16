#include "decnet/nice/nml.h"

#include "decnet/node.h"

namespace decnet::nice {

NiceReply &ReplyDict::make (Group &g, bool reuse)
{
    if (reuse && !g.items.empty ()) return *g.items.back ();
    g.items.push_back (std::make_unique<NiceReply> ());
    return *g.items.back ();
}

NiceReply &ReplyDict::node_entry (Nodeid id)
{
    Group &g = numeric_[id.value ()];
    bool fresh = g.items.empty ();
    NiceReply &r = make (g, true);
    if (fresh) {
        r.has_entity = true;
        // The name comes from the node database, so that a reply says
        // "1.3 (GROK)" wherever the reader has a name for the address.
        r.entity = Entity::make_node (node_ ? node_->nicenode (id)
                                            : NiceNode (id));
    }
    return r;
}

bool ReplyDict::contains_node (Nodeid id) const
{
    return numeric_.find (id.value ()) != numeric_.end ();
}

NiceReply &ReplyDict::area_entry (unsigned area)
{
    Group &g = numeric_[area];
    bool fresh = g.items.empty ();
    NiceReply &r = make (g, true);
    if (fresh) {
        r.has_entity = true;
        r.entity = Entity::make_area (area);
    }
    return r;
}

NiceReply &ReplyDict::named_entry (const std::string &name)
{
    Group &g = named_[name];
    bool fresh = g.items.empty ();
    NiceReply &r = make (g, true);
    if (fresh) {
        r.has_entity = true;
        r.entity = Entity::make_string (kind_, name);
    }
    return r;
}

NiceReply &ReplyDict::add_named (const std::string &name)
{
    Group &g = named_[name];
    NiceReply &r = make (g, false);
    r.has_entity = true;
    r.entity = Entity::make_string (kind_, name);
    return r;
}

std::vector<std::vector<NiceReply *>>
ReplyDict::sorted (const NiceRequest &req) const
{
    std::vector<std::vector<NiceReply *>> out;

    auto add = [&out] (const Group &g) {
        std::vector<NiceReply *> group;
        group.reserve (g.items.size ());
        for (const auto &p : g.items) group.push_back (p.get ());
        if (!group.empty ()) out.push_back (std::move (group));
    };

    if (kind_ == Entity::node) {
        // Executor first.
        Nodeid exec = node_ ? node_->id () : Nodeid ();
        auto self = numeric_.find (exec.value ());
        if (self != numeric_.end () && req.entity.match (exec))
            add (self->second);
        for (const auto &[key, g] : numeric_) {
            if (key == exec.value ()) continue;
            if (!req.entity.match (Nodeid (static_cast<std::uint16_t> (key))))
                continue;
            add (g);
        }
        return out;
    }

    for (const auto &[key, g] : numeric_) {
        // An area request names a specific area or all of them; a request
        // for one area must not answer about another.
        if (req.entity.code == 0 && kind_ == Entity::area
            && key != req.entity.area)
            continue;
        add (g);
    }
    for (const auto &[key, g] : named_) {
        if (!req.entity.match (key)) continue;
        add (g);
    }
    return out;
}

}   // namespace decnet::nice
