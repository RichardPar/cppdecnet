// decnet/packet/indexed.h -- packet class lookup by code.
//
// Port of packet.Indexed, packet.IndexedPacket and the indexer metaclass.
// Packet families share a header, and a header field selects the class.
// Registration is explicit:
//
//     DN_REGISTER_PACKET_MASKED (RoutingPacketBase, ShortData, 0x02, 0xc7);
//
//  * masked registration: a class claims every key whose masked bits
//    match.  Used by routing, where the flags byte mixes type and flags.
//  * nested indexes: a registered class can be the root of a second index
//    on another field, e.g. PtpInit34 (version byte at offset 6) and
//    P2StartBase (starttype).

#ifndef DECNET_PACKET_INDEXED_H
#define DECNET_PACKET_INDEXED_H

#include "decnet/packet/packet.h"

#include <map>
#include <memory>

namespace decnet::packet {

// Read one byte of a raw buffer, for use in an index_key function.
inline std::uint8_t require_byte (ByteView b, std::size_t off)
{
    if (b.size () <= off)
        throw MissingData ("buffer too short for the index field");
    return b[off];
}

// One level of class lookup.  Port of the classindex dictionary plus the
// findclassb walk over nested levels.
template <typename Root>
class PacketIndex {
public:
    using Factory = std::unique_ptr<Root> (*) ();
    using KeyFn   = std::uint64_t (*) (ByteView);

    struct Entry {
        Factory      factory = nullptr;    // a concrete class
        PacketIndex *nested  = nullptr;    // or another level of lookup
        const char  *name    = "";
    };

    // limit is the size of PyDECnet's nlist (n) index; zero means the index
    // is a dictionary with no range check.
    PacketIndex (KeyFn keyfn, std::size_t limit) noexcept
        : keyfn_ (keyfn), limit_ (limit) {}

    void add (std::uint64_t key, Factory f, const char *name)
    {
        check_range (key);
        entries_[key] = Entry { f, nullptr, name };
    }

    // Claim every key whose masked bits match, which is what
    // classindexmask does.  Requires a limit, since that bounds the set.
    void add_masked (std::uint64_t key, std::uint64_t mask, Factory f,
                     const char *name)
    {
        if (!limit_)
            throw InternalError ("masked registration needs an index limit");
        std::uint64_t want = key & mask;
        for (std::uint64_t k = 0; k < limit_; ++k)
            if ((k & mask) == want) entries_[k] = Entry { f, nullptr, name };
    }

    void add_nested (std::uint64_t key, PacketIndex *child, const char *name)
    {
        check_range (key);
        entries_[key] = Entry { nullptr, child, name };
    }

    void add_nested_masked (std::uint64_t key, std::uint64_t mask,
                            PacketIndex *child, const char *name)
    {
        if (!limit_)
            throw InternalError ("masked registration needs an index limit");
        std::uint64_t want = key & mask;
        for (std::uint64_t k = 0; k < limit_; ++k)
            if ((k & mask) == want) entries_[k] = Entry { nullptr, child, name };
    }

    // The class to use when the index has no entry -- port of the
    // defaultclass hook, which PtpInit34 uses to accept unknown versions.
    void set_default (Factory f, const char *name) noexcept
    { default_ = Entry { f, nullptr, name }; }

    // Walk the levels until one yields a concrete class, then build it.
    // Port of Indexed.findclassb.
    std::unique_ptr<Root> create (ByteView buf) const
    {
        const PacketIndex *level = this;
        // Bounded so that a registration mistake cannot spin forever.
        for (int depth = 0; depth < 8; ++depth) {
            std::uint64_t key = level->keyfn_ (buf);
            if (level->limit_ && key >= level->limit_)
                throw DecodeError ("index " + std::to_string (key)
                                   + " out of range");
            const Entry *e = nullptr;
            auto it = level->entries_.find (key);
            if (it != level->entries_.end ())            e = &it->second;
            else if (level->default_.factory)            e = &level->default_;
            if (!e)
                throw DecodeError ("no packet class for index "
                                   + std::to_string (key));
            if (e->nested) { level = e->nested; continue; }
            return e->factory ();
        }
        throw InternalError ("packet index nested too deeply");
    }

    std::size_t size () const noexcept { return entries_.size (); }

private:
    void check_range (std::uint64_t key) const
    {
        if (limit_ && key >= limit_)
            throw InternalError ("index key " + std::to_string (key)
                                 + " beyond the index limit");
    }

    KeyFn                            keyfn_;
    std::size_t                      limit_;
    std::map<std::uint64_t, Entry>   entries_;
    Entry                            default_;
};

// Base for the root of an indexed packet family.  Root must supply
//     static std::uint64_t index_key (ByteView);
// and declare its index with DN_PACKET_INDEX.
template <typename Root>
class Indexed {
public:
    // All classes in the family, including nested index roots, decode to this
    // pointer type.  A nested level is a PacketIndex over the same Root with a
    // different key function.
    using family_root = Root;

    virtual ~Indexed () = default;

    // The virtual forms of the CRTP Packet members, so that a decoded
    // packet can be handled through a pointer to the family root.
    virtual std::size_t decode_into (ByteView buf) = 0;
    virtual Bytes       encode_packet () const = 0;
    virtual const char *packet_name () const noexcept = 0;

    // Pick the class from the buffer, build it and parse.  Throws
    // DecodeError, like PyDECnet.
    static std::unique_ptr<Root> parse_indexed (ByteView buf)
    {
        std::unique_ptr<Root> p = Root::index ().create (buf);
        p->decode_into (buf);
        return p;
    }

    // The form the receive paths use: a bad packet is dropped, not thrown.
    static std::unique_ptr<Root> try_parse_indexed (ByteView buf) noexcept
    {
        try {
            return parse_indexed (buf);
        } catch (const DecodeError &) {
            return nullptr;
        }
    }
};

// Mixin for a concrete member of an indexed family: supplies the virtuals
// from the class's own compile time layout.  Base is either the family
// root or a nested sub-root; either way the factory yields the family
// root's pointer type.
template <typename Derived, typename Base, Extra E = Extra::reject>
struct IndexedBody : Base, Packet<Derived, E> {
    using PacketBase = Packet<Derived, E>;
    using Root       = typename Base::family_root;

    std::size_t decode_into (ByteView buf) override
    { return PacketBase::decode (buf); }

    Bytes encode_packet () const override
    { return PacketBase::encode (); }

    const char *packet_name () const noexcept override { return Derived::name; }

    static std::unique_ptr<Root> make ()
    { return std::unique_ptr<Root> (new Derived ()); }
};

}   // namespace decnet::packet

// Declare a family root's index, in the root's class body.  The index is a
// function local static, so it is constructed before any registration
// regardless of initialisation order.
#define DN_PACKET_INDEX(Root, limit)                                          \
    static ::decnet::packet::PacketIndex<Root> &index ()                      \
    {                                                                         \
        static ::decnet::packet::PacketIndex<Root> idx (&Root::index_key,     \
                                                        (limit));             \
        return idx;                                                           \
    }

// The same, for a family whose members are in a static library.
//
// The linker only includes archive members that are referenced, so static
// initialisers in a registration-only translation unit may never run.
// regfn is a function in the registration unit; naming it here pulls it in
// and runs it once before the first lookup.  Registrations must use
// raw_index(), not index(), to avoid recursion.
#define DN_PACKET_INDEX_REGISTERED(Root, limit, regfn)                        \
    static ::decnet::packet::PacketIndex<Root> &raw_index ()                  \
    {                                                                         \
        static ::decnet::packet::PacketIndex<Root> idx (&Root::index_key,     \
                                                        (limit));             \
        return idx;                                                           \
    }                                                                         \
    static ::decnet::packet::PacketIndex<Root> &index ()                      \
    {                                                                         \
        static const bool dn_registered = ((regfn) (), true);                 \
        (void) dn_registered;                                                 \
        return raw_index ();                                                  \
    }

// Declare a nested level's index.  Self is the class carrying this macro,
// which supplies the key function; the entries still produce Root pointers.
#define DN_PACKET_SUBINDEX(Root, Self, limit)                                 \
    static ::decnet::packet::PacketIndex<Root> &index ()                      \
    {                                                                         \
        static ::decnet::packet::PacketIndex<Root> idx (&Self::index_key,     \
                                                        (limit));             \
        return idx;                                                           \
    }

// Register a concrete class under one key.
#define DN_REGISTER_PACKET(Root, Cls, key)                                    \
    static const bool dn_reg_##Cls = [] {                                     \
        Root::index ().add ((key), &Cls::make, #Cls);                         \
        return true;                                                          \
    } ()

// Register a class for every key whose masked bits match -- classindexmask.
#define DN_REGISTER_PACKET_MASKED(Root, Cls, key, mask)                       \
    static const bool dn_reg_##Cls = [] {                                     \
        Root::index ().add_masked ((key), (mask), &Cls::make, #Cls);          \
        return true;                                                          \
    } ()

// Register a class under an explicit set of keys -- classindexkeys.
#define DN_REGISTER_PACKET_KEYS(Root, Cls, ...)                               \
    static const bool dn_reg_##Cls = [] {                                     \
        for (std::uint64_t k : { __VA_ARGS__ })                               \
            Root::index ().add (k, &Cls::make, #Cls);                         \
        return true;                                                          \
    } ()

// Point a key at a second level of lookup, whose root is Sub.
#define DN_REGISTER_NESTED_MASKED(Root, Sub, key, mask)                       \
    static const bool dn_nest_##Sub = [] {                                    \
        Root::index ().add_nested_masked ((key), (mask), &Sub::index (), #Sub); \
        return true;                                                          \
    } ()

#define DN_REGISTER_NESTED(Root, Sub, key)                                    \
    static const bool dn_nest_##Sub = [] {                                    \
        Root::index ().add_nested ((key), &Sub::index (), #Sub);              \
        return true;                                                          \
    } ()

#endif  // DECNET_PACKET_INDEXED_H
