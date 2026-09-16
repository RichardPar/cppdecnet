#include "decnet/nice/packets.h"

#include "decnet/common/exceptions.h"

#include <map>

namespace decnet::nice {

namespace {

void put_string (Encoder &e, const std::string &s, std::size_t maxlen)
{
    if (s.size () > maxlen)
        throw FieldOverflow ("NICE string longer than "
                             + std::to_string (maxlen) + " characters");
    e.byte (static_cast<std::uint8_t> (s.size ()));
    e.raw (ByteView (reinterpret_cast<const std::uint8_t *> (s.data ()),
                     s.size ()));
}

std::string get_string (Decoder &d)
{
    std::size_t len = d.byte ();
    ByteView b = d.raw (len);
    return std::string (reinterpret_cast<const char *> (b.data ()), b.size ());
}

// A byte read as a signed value, which is how every entity code arrives.
std::int8_t signed_byte (std::uint8_t b)
{
    return static_cast<std::int8_t> (b);
}

}   // namespace

// ------------------------------------------------------------ retcodes

const char *retcode_text (int code)
{
    switch (code) {
    case 1: case 2: case 3: case -128: return nullptr;
    case  -1: return "Unrecognized function or option";
    case  -2: return "Invalid message format";
    case  -3: return "Privilege violation";
    case  -4: return "Oversized Management command message";
    case  -5: return "Management program error";
    case  -6: return "Unrecognized parameter type";
    case  -7: return "Incompatible Management version";
    case  -8: return "Unrecognized component";
    case  -9: return "Invalid identification";
    case -10: return "Line communication error";
    case -11: return "Component in wrong state";
    case -13: return "File open error";
    case -14: return "Invalid file contents";
    case -15: return "Resource error";
    case -16: return "Invalid parameter value";
    case -17: return "Line protocol error";
    case -18: return "File I/O error";
    case -19: return "Mirror link disconnected";
    case -20: return "No room for new entry";
    case -21: return "Mirror connect failed";
    case -22: return "Parameter not applicable";
    case -23: return "Parameter value too long";
    case -24: return "Hardware failure";
    case -25: return "Operation failure";
    case -26: return "System-specific Management function not supported";
    case -27: return "Invalid parameter grouping";
    case -28: return "Bad loopback response";
    case -29: return "Parameter missing";
    default:  return "Unknown error code";
    }
}

std::string detail_text (int retcode, unsigned detail)
{
    // The codes whose detail field names a file.
    static const char *const files[] = {
        "Permanent database", "Load file", "Dump file", "Secondary loader",
        "Tertiary loader", "Secondary dumper", "Volatile database",
        "Diagnostic file"
    };
    // The codes whose detail field says why a mirror connection failed.
    static const char *const mirror[] = {
        "No node name set", "Invalid node name format",
        "Unrecognized node name", "Node unreachable", "Network resources",
        "Rejected by object", "Invalid object name format",
        "Unrecognized object", "Access control rejected", "Object too busy",
        "No response from object", "Remote node shut down",
        "Node or object failed", "Disconnect by object", "Abort by object",
        "Abort by Management", "Local node shut down"
    };
    switch (retcode) {
    case -13: case -14: case -18:
        if (detail < std::size (files)) return files[detail];
        break;
    case -19: case -21:
        if (detail < std::size (mirror)) return mirror[detail];
        break;
    default:
        break;
    }
    return {};
}

// ------------------------------------------------------------- ReqEntity

ReqEntity ReqEntity::make_node (Nodeid n)
{
    ReqEntity r;
    r.etype = Entity::node;
    r.code = 0;
    r.id = n;
    return r;
}

ReqEntity ReqEntity::make_named (std::uint8_t etype, std::string n)
{
    ReqEntity r;
    r.etype = etype;
    if (n.size () > 127)
        throw FieldOverflow ("entity name longer than 127 characters");
    r.code = static_cast<std::int8_t> (n.size ());
    r.name = std::move (n);
    return r;
}

ReqEntity ReqEntity::make_area (unsigned a)
{
    ReqEntity r;
    r.etype = Entity::area;
    r.code = 0;
    r.area = a;
    return r;
}

ReqEntity ReqEntity::make_wild (std::uint8_t etype, std::int8_t code)
{
    return ReqEntity (etype, code);
}

bool ReqEntity::match (Nodeid n) const noexcept
{
    if (code == 0) return n == id;
    if (code == area_wild) return n.area () == id.area ();
    if (code == tid_wild) return n.tid () == id.tid ();
    // Every other plural form leaves the choice to the caller: whether a
    // node is "active" is something NSP knows and the entity does not.
    return code < 0;
}

bool ReqEntity::match (const std::string &n) const noexcept
{
    if (code > 0) return n == name;
    return code < 0;
}

std::string ReqEntity::str () const
{
    // The label the entity type prints under, singular.
    const char *label;
    switch (etype) {
    case Entity::node:    label = "Node"; break;
    case Entity::line:    label = "Line"; break;
    case Entity::logging: label = "Logging"; break;
    case Entity::circuit: label = "Circuit"; break;
    case Entity::module:  label = "Module"; break;
    case Entity::area:    label = "Area"; break;
    default:              label = "Entity"; break;
    }
    if (code < 0) {
        static const char *const mult[] = {
            nullptr, "Known", "Active", "Loop", "Adjacent", "Significant"
        };
        unsigned i = static_cast<unsigned> (-code);
        if (i < std::size (mult) && mult[i])
            return std::string (mult[i]) + " " + label + "s";
        // The two node wildcards: "every node in area 2".
        if (code == area_wild)
            return "Nodes in area " + std::to_string (id.area ());
        if (code == tid_wild)
            return "Nodes numbered " + std::to_string (id.tid ());
        return std::string ("Entities #") + std::to_string (code);
    }
    if (etype == Entity::node) {
        // Node address zero is how a request says "the executor", the node
        // it is talking to.
        if (id.value () == 0) return "executor";
        return std::string (label) + " " + id.str ();
    }
    if (etype == Entity::area) return std::string (label) + " "
                                    + std::to_string (area);
    return std::string (label) + " " + name;
}

void ReqEntity::encode (Encoder &e) const
{
    if (code < 0) {
        e.byte (static_cast<std::uint8_t> (code));
        // The node wildcards carry a number with them.
        if (etype == Entity::node && (code == area_wild || code == tid_wild))
            e.uint (id.value (), 2);
        return;
    }
    // Check for a name first: a node or area may be given by name, and the
    // name's count byte occupies the position of the zero that means "by
    // number".
    if (code > 0) {
        put_string (e, name, 127);
        return;
    }
    if (etype == Entity::node) {
        e.byte (0);
        e.uint (id.value (), 2);
        return;
    }
    if (etype == Entity::area) {
        e.byte (0);
        e.byte (static_cast<std::uint8_t> (area));
        return;
    }
    put_string (e, name, 127);
}

ReqEntity ReqEntity::decode (Decoder &d, std::uint8_t etype)
{
    ReqEntity r;
    r.etype = etype;
    r.code = signed_byte (d.byte ());
    if (etype == Entity::node) {
        // A specific node and the two partial wildcards all carry a number.
        if (r.code == 0 || r.code < significant) {
            r.id = Nodeid (static_cast<std::uint16_t> (d.uint (2)));
            return r;
        }
    } else if (etype == Entity::area) {
        if (r.code == 0) {
            r.area = d.byte ();
            return r;
        }
        if (r.code > 0)
            throw DecodeError ("area entity given as a string");
    }
    if (r.code > 0) {
        ByteView b = d.raw (static_cast<std::size_t> (r.code));
        r.name.assign (reinterpret_cast<const char *> (b.data ()), b.size ());
    } else if (r.code == 0) {
        // Code zero means "a specific one given by number", and only the
        // node and area entities have numbers.
        throw DecodeError ("entity code 0 for a named entity type");
    }
    return r;
}

// ----------------------------------------------------------- NiceRequest

namespace {

// Loop request parameters with their type codes.  Requests omit the code
// byte, so decoding needs this table.
struct ReqParam { std::uint16_t number; std::uint8_t code; };

constexpr ReqParam loop_params[] = {
    { 150, 0x02 },      // Count,  DU-2
    { 151, 0x02 },      // Length, DU-2
    { 152, 0x81 },      // With,   C-1
};

constexpr ReqParam loop_circ_params[] = {
    {  10, 0x20 },      // Physical address,           HI
    { 153, 0x20 },      // Assistant physical address, HI
    { 154, 0x81 },      // Help,                       C-1
    { 155, 0x40 },      // Node,                       AI
    { 156, 0x40 },      // Assistant node,             AI
};

const ReqParam *find_req_param (std::uint16_t n, bool circuit)
{
    for (const ReqParam &p : loop_params)
        if (p.number == n) return &p;
    if (circuit)
        for (const ReqParam &p : loop_circ_params)
            if (p.number == n) return &p;
    return nullptr;
}

}   // namespace

Bytes NiceRequest::encode () const
{
    Bytes out;
    Encoder e (out);
    e.byte (function);
    switch (function) {
    case fn_read:
    case fn_set:
        e.byte (static_cast<std::uint8_t> ((permanent ? 0x80 : 0)
                                           | ((info & 7) << 4)
                                           | (entity_type & 7)));
        entity.encode (e);
        if (function == fn_read) {
            // Qualifiers are entities in a parameter slot and have no type code (see
            // StringQualEntity in nicepackets.py).
            if (has_qual_circuit) {
                e.uint (501, 2);
                put_string (e, qual_circuit, 127);
            }
            if (has_qual_node) {
                e.uint (800, 2);
                qual_node.encode (e);
            }
        }
        break;
    case fn_zero:
        e.byte (static_cast<std::uint8_t> ((readzero ? 0x80 : 0)
                                           | (entity_type & 7)));
        entity.encode (e);
        break;
    case fn_test:
        e.byte (static_cast<std::uint8_t> ((test_type & 7)
                                           | (access_ctl ? 0x80 : 0)));
        entity.encode (e);
        if (access_ctl) {
            put_string (e, username, 39);
            put_string (e, password, 39);
            put_string (e, account, 39);
        }
        e.uint (150, 2); e.uint (loop_count, 2);
        e.uint (151, 2); e.uint (loop_length, 2);
        e.uint (152, 2); e.uint (loop_with, 1);
        if (test_type == test_circuit && !physical_address.empty ()) {
            e.uint (10, 2);
            e.byte (static_cast<std::uint8_t> (physical_address.size ()));
            e.raw (ByteView (physical_address.data (),
                             physical_address.size ()));
        }
        break;
    default:
        throw FieldOverflow ("cannot encode NICE function "
                             + std::to_string (function));
    }
    return out;
}

NiceRequest NiceRequest::parse (ByteView buf)
{
    NiceRequest r;
    Decoder d (buf);
    r.function = d.byte ();
    switch (r.function) {
    case fn_read:
    case fn_set: {
        std::uint8_t flags = d.byte ();
        r.permanent = (flags & 0x80) != 0;
        r.info = (flags >> 4) & 7;
        r.entity_type = flags & 7;
        r.entity = ReqEntity::decode (d, r.entity_type);
        // Stop at an unknown qualifier; without a type code its length is unknown.
        while (!d.empty ()) {
            std::uint16_t num = static_cast<std::uint16_t> (d.uint (2));
            if (num == 501 || num == 822) {
                r.has_qual_circuit = true;
                r.qual_circuit = get_string (d);
            } else if (num == 800) {
                r.has_qual_node = true;
                r.qual_node = ReqEntity::decode (d, Entity::node);
            } else {
                throw DecodeError ("unknown NICE request qualifier "
                                   + std::to_string (num));
            }
        }
        break;
    }
    case fn_zero: {
        std::uint8_t flags = d.byte ();
        r.readzero = (flags & 0x80) != 0;
        r.entity_type = flags & 7;
        r.entity = ReqEntity::decode (d, r.entity_type);
        break;
    }
    case fn_test: {
        std::uint8_t flags = d.byte ();
        r.test_type = flags & 7;
        r.access_ctl = (flags & 0x80) != 0;
        // The test type says which entity kind follows: loop node, loop
        // line or loop circuit.
        switch (r.test_type) {
        case test_node:    r.entity_type = Entity::node; break;
        case test_line:    r.entity_type = Entity::line; break;
        case test_circuit: r.entity_type = Entity::circuit; break;
        default:
            throw DecodeError ("unknown NICE test type "
                               + std::to_string (r.test_type));
        }
        r.entity = ReqEntity::decode (d, r.entity_type);
        if (r.access_ctl) {
            r.username = get_string (d);
            r.password = get_string (d);
            r.account = get_string (d);
        }
        while (!d.empty ()) {
            std::uint16_t num = static_cast<std::uint16_t> (d.uint (2));
            const ReqParam *p = find_req_param (num,
                                                r.test_type == test_circuit);
            if (!p)
                throw DecodeError ("unknown NICE loop parameter "
                                   + std::to_string (num));
            Value v = Value::decode_body (p->code, d);
            switch (num) {
            case 150: r.loop_count = static_cast<unsigned> (v.as_uint ()); break;
            case 151: r.loop_length = static_cast<unsigned> (v.as_uint ()); break;
            case 152: r.loop_with = static_cast<unsigned> (v.as_uint ()); break;
            case 10:  r.physical_address = v.as_bytes (); break;
            default:  break;    // the assistant parameters, which we ignore
            }
        }
        break;
    }
    default:
        throw DecodeError ("unknown NICE function "
                           + std::to_string (r.function));
    }
    return r;
}

std::string NiceRequest::str () const
{
    std::string s;
    switch (function) {
    case fn_read: {
        static const char *const what[] = {
            "summary", "status", "characteristics", "counters", "events"
        };
        s = "read ";
        s += info < std::size (what) ? what[info] : "#" + std::to_string (info);
        break;
    }
    case fn_set:  s = "set"; break;
    case fn_zero: s = readzero ? "read and zero counters" : "zero counters"; break;
    case fn_test: s = "loop"; break;
    default: s = "function #" + std::to_string (function); break;
    }
    return s + " " + entity.str ();
}

// ------------------------------------------------------------- NiceReply

NiceReply NiceReply::error (int rc, unsigned det, std::string msg)
{
    NiceReply r (rc);
    r.detail = static_cast<std::uint16_t> (det);
    r.message = std::move (msg);
    return r;
}

Bytes NiceReply::encode () const
{
    Bytes out;
    Encoder e (out);
    e.uint (static_cast<std::uint64_t> (static_cast<std::int64_t> (retcode)), 1);
    e.uint (detail, 2);
    put_string (e, message, 255);
    if (has_notlooped) e.uint (notlooped, 2);
    if (has_entity) {
        entity.encode_body (e);
        params.encode (e);
    }
    return out;
}

NiceReply NiceReply::parse_header (ByteView buf)
{
    NiceReply r;
    Decoder d (buf);
    r.retcode = signed_byte (d.byte ());
    // Both fields are optional: an implementation that has nothing to add
    // sends the return code alone, and NCP copes.
    if (!d.empty ()) r.detail = static_cast<std::uint16_t> (d.uint (2));
    if (!d.empty ()) r.message = get_string (d);
    return r;
}

NiceReply NiceReply::parse_loop (ByteView buf)
{
    NiceReply r = parse_header (buf);
    // Re-walk the header to find where it ended.  Cheap, and it keeps the
    // optional-field rules in one place rather than duplicated here.
    Decoder d (buf);
    (void) d.byte ();
    if (!d.empty ()) (void) d.uint (2);
    if (!d.empty ()) (void) get_string (d);
    if (!d.empty ()) {
        r.has_notlooped = true;
        r.notlooped = static_cast<std::uint16_t> (d.uint (2));
    }
    return r;
}

NiceReply NiceReply::parse (ByteView buf, std::uint8_t entity_kind)
{
    NiceReply r;
    Decoder d (buf);
    r.retcode = signed_byte (d.byte ());
    r.detail = static_cast<std::uint16_t> (d.uint (2));
    r.message = get_string (d);
    if (!d.empty ()) {
        r.has_entity = true;
        r.entity = Entity::decode_body (entity_kind, d);
        r.params.decode (d);
    }
    return r;
}

std::string NiceReply::format (ParamDefs defs) const
{
    std::string s;
    if (retcode < 0) {
        const char *t = retcode_text (retcode);
        s = t ? t : "Error";
        std::string det = detail_text (retcode, detail);
        if (!det.empty ()) s += ", " + det;
        else if (detail != 0xffff) s += ", parameter " + std::to_string (detail);
        if (!message.empty ()) s += ", " + message;
        return s;
    }
    if (has_entity) s = entity.str ();
    for (const std::string &line : params.format (defs)) {
        if (!s.empty ()) s += '\n';
        s += "    " + line;
    }
    return s;
}

}   // namespace decnet::nice
