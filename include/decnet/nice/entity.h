// decnet/nice/entity.h -- NICE entities.
//
// Port of the entity classes in nice_coding.py: node, line, logging,
// circuit, module and area.  Each is encoded as a one byte kind followed
// by a kind-specific body, and formats as "Label = value".  Unknown kinds
// format as "Entity #n".

#ifndef DECNET_NICE_ENTITY_H
#define DECNET_NICE_ENTITY_H

#include "decnet/common/types.h"
#include "decnet/packet/buffer.h"

#include <cstdint>
#include <string>

namespace decnet::nice {

using packet::Decoder;
using packet::Encoder;

// Node address with optional name and executor flag.  Port of
// common.NiceNode.  The executor flag is the top bit of the name length.
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

    // Unique key: kind and value.
    std::string key () const;

    // "Circuit = DMC-0".  Empty for the none entity, which is how an event
    // with nothing to name leaves the line out altogether.
    std::string str () const;

    void encode (Encoder &e) const;
    static Entity decode (Decoder &d);

    // Encoding without the kind byte, as used in NICE replies.  Event records
    // include it.
    void encode_body (Encoder &e) const;
    static Entity decode_body (std::uint8_t kind, Decoder &d);

    friend bool operator== (const Entity &, const Entity &) = default;

private:
    std::uint8_t kind_ = none;
    NiceNode     node_;
    std::string  name_;
    unsigned     area_ = 0;
};

}   // namespace decnet::nice

#endif  // DECNET_NICE_ENTITY_H
