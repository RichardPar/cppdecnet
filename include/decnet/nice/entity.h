// decnet/nice/entity.h -- the thing a NICE message or an event is about.
//
// Port of the entity classes at the top of nice_coding.py.  Network
// management names five kinds of thing: a node, a line, a logging sink, a
// circuit and a module, plus an area.  Every one of them is encoded as a
// one byte kind followed by a body whose shape depends on the kind, and
// every one of them formats as "Label = value".
//
// pydecnet gives each kind a class and indexes the classes on the code
// byte, generating a class on the fly for a code it has not seen.  A kind
// enumeration and a body that is either a string, a node or an area number
// says the same thing with less machinery, and an unrecognised code needs
// no class of its own: it keeps its number and formats as "Entity #9".

#ifndef DECNET_NICE_ENTITY_H
#define DECNET_NICE_ENTITY_H

#include "decnet/common/types.h"
#include "decnet/packet/buffer.h"

#include <cstdint>
#include <string>

namespace decnet::nice {

using packet::Decoder;
using packet::Encoder;

// A node address that may carry the node's name, and may be flagged as the
// executor -- the node the management request is talking to.  Port of
// common.NiceNode.  The name length byte carries the executor flag in its
// top bit, which is why this is not simply a Nodeid and a string.
struct NiceNode {
    Nodeid      id;
    std::string name;
    bool        executor = false;

    NiceNode () = default;
    NiceNode (Nodeid i, std::string n = {}, bool ex = false)
        : id (i), name (std::move (n)), executor (ex) {}

    // "1.3 (GROK)", or just "1.3" when there is no name.
    std::string str () const;

    void encode (Encoder &e) const;
    static NiceNode decode (Decoder &d);

    friend bool operator== (const NiceNode &, const NiceNode &) = default;
};

class Entity {
public:
    // The wire codes.  Anything else is kept as it arrived; see the
    // "unknown" constructor below.
    enum Kind : std::uint8_t {
        node = 0, line = 1, logging = 2, circuit = 3, module = 4,
        area = 5, none = 255
    };

    Entity () = default;                        // none

    static Entity make_node (NiceNode n);
    static Entity make_area (unsigned a);
    static Entity make_string (std::uint8_t kind, std::string name);
    static Entity make_none () { return Entity (); }

    // Shorthands for the four string kinds.
    static Entity make_circuit (std::string n) { return make_string (circuit, std::move (n)); }
    static Entity make_line    (std::string n) { return make_string (line,    std::move (n)); }
    static Entity make_module  (std::string n) { return make_string (module,  std::move (n)); }
    static Entity make_logging (std::string n) { return make_string (logging, std::move (n)); }

    std::uint8_t kind () const noexcept { return kind_; }
    bool is_none () const noexcept { return kind_ == none; }

    const NiceNode   &as_node   () const noexcept { return node_; }
    const std::string &as_string () const noexcept { return name_; }
    unsigned           as_area   () const noexcept { return area_; }

    // The label this kind of entity prints under: "Node", "Circuit", or
    // "Entity #9" for a code with no name.
    std::string label () const;

    // Just the value: "DMC-0", "2.5 (ARK)", "51".
    std::string value () const;

    // A stable string identifying this entity, for use as a map key: the
    // kind number and the value, so a circuit and a line of the same name
    // are different keys.
    std::string key () const;

    // "Circuit = DMC-0".  Empty for the none entity, which is how an event
    // with nothing to name leaves the line out altogether.
    std::string str () const;

    void encode (Encoder &e) const;
    static Entity decode (Decoder &d);

    friend bool operator== (const Entity &, const Entity &) = default;

private:
    std::uint8_t kind_ = none;
    NiceNode     node_;
    std::string  name_;
    unsigned     area_ = 0;
};

}   // namespace decnet::nice

#endif  // DECNET_NICE_ENTITY_H
