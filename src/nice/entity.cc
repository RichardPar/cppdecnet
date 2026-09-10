#include "decnet/nice/entity.h"

#include "decnet/common/exceptions.h"

namespace decnet::nice {

// ------------------------------------------------------------- NiceNode

std::string NiceNode::str () const
{
    if (name.empty ()) return id.str ();
    return id.str () + " (" + name + ")";
}

void NiceNode::encode (Encoder &e) const
{
    e.uint (id.value (), 2);
    // The executor flag rides in the top bit of the name length, so a name
    // is limited to 127 characters here rather than the usual 255.
    if (name.size () > 127)
        throw FieldOverflow ("node name longer than 127 characters");
    e.byte (static_cast<std::uint8_t> (name.size () | (executor ? 0x80 : 0)));
    e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (name.data ()),
                     name.size ()));
}

NiceNode NiceNode::decode (Decoder &d)
{
    NiceNode n;
    n.id = Nodeid (static_cast<std::uint16_t> (d.uint (2)));
    std::uint8_t len = d.byte ();
    n.executor = (len & 0x80) != 0;
    ByteView b = d.raw (len & 0x7f);
    n.name.assign (reinterpret_cast<const char *> (b.data ()), b.size ());
    return n;
}

// --------------------------------------------------------------- Entity

Entity Entity::make_node (NiceNode n)
{
    Entity e; e.kind_ = node; e.node_ = std::move (n); return e;
}

Entity Entity::make_area (unsigned a)
{
    Entity e; e.kind_ = area; e.area_ = a; return e;
}

Entity Entity::make_string (std::uint8_t kind, std::string name)
{
    Entity e; e.kind_ = kind; e.name_ = std::move (name); return e;
}

std::string Entity::label () const
{
    switch (kind_) {
    case node:    return "Node";
    case line:    return "Line";
    case logging: return "Logging";
    case circuit: return "Circuit";
    case module:  return "Module";
    case area:    return "Area";
    case none:    return "";
    default:      return "Entity #" + std::to_string (kind_);
    }
}

std::string Entity::value () const
{
    switch (kind_) {
    case node: return node_.str ();
    case area: return std::to_string (area_);
    case none: return "";
    default:   return name_;
    }
}

std::string Entity::key () const
{
    return std::to_string (kind_) + ":" + value ();
}

std::string Entity::str () const
{
    if (kind_ == none) return "";
    return label () + " = " + value ();
}

void Entity::encode (Encoder &e) const
{
    e.byte (kind_);
    switch (kind_) {
    case node:
        node_.encode (e);
        break;
    case area:
        // A specific area is a zero byte -- the "not a counted string"
        // marker -- and then the area number.
        e.byte (0);
        e.byte (static_cast<std::uint8_t> (area_));
        break;
    case none:
        break;
    default:
        if (name_.size () > 255)
            throw FieldOverflow ("entity name longer than 255 characters");
        e.byte (static_cast<std::uint8_t> (name_.size ()));
        e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (name_.data ()),
                         name_.size ()));
        break;
    }
}

Entity Entity::decode (Decoder &d)
{
    std::uint8_t kind = d.byte ();
    switch (kind) {
    case node:
        return make_node (NiceNode::decode (d));
    case area: {
        if (d.byte () != 0)
            throw DecodeError ("area entity without its zero marker");
        return make_area (d.byte ());
    }
    case none:
        return make_none ();
    default: {
        // Every other code, known or not, is a counted string.  DECnet/E
        // sends circuit events with a line entity, so a reader that
        // insisted on the code it expected would reject them.
        std::size_t len = d.byte ();
        ByteView b = d.raw (len);
        return make_string (kind,
                            std::string (reinterpret_cast<const char *> (b.data ()),
                                         b.size ()));
    }
    }
}

}   // namespace decnet::nice
