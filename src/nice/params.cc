#include "decnet/nice/params.h"

#include "decnet/common/exceptions.h"

#include <cstdio>

namespace decnet::nice {

namespace {

// The counter type code lives in the top nibble of the parameter number.
// Odd nibbles are the mapped forms.
std::uint16_t counter_code (unsigned bytes, bool mapped)
{
    std::uint16_t base;
    switch (bytes) {
    case 1:  base = 0xa000; break;
    case 2:  base = 0xc000; break;
    case 4:  base = 0xe000; break;
    default: throw FieldOverflow ("counter width " + std::to_string (bytes)
                                  + " is not 1, 2 or 4");
    }
    return mapped ? static_cast<std::uint16_t> (base + 0x1000) : base;
}

std::uint16_t key_of (std::uint16_t number, bool counter)
{
    return static_cast<std::uint16_t> (number | (counter ? 0x8000 : 0));
}

// A mapped counter prints its qualifiers on their own lines under the
// count, indented to line up past the eleven column number field.
constexpr const char *map_indent = "                   ";

}   // namespace

// -------------------------------------------------------------- Counter

std::uint64_t Counter::max () const noexcept
{
    switch (bytes) {
    case 1:  return 255;
    case 2:  return 65535;
    case 4:  return 4294967295u;
    default: return 0;
    }
}

std::string Counter::format () const
{
    std::uint64_t m = max ();
    // At the ceiling the true count is unknown, so say so rather than
    // print a number that is certainly wrong.
    if (m && value >= m) return ">" + std::to_string (m - 1);
    return std::to_string (value);
}

std::string format_node_number (std::uint64_t v)
{
    unsigned area = static_cast<unsigned> (v >> 10);
    unsigned tid  = static_cast<unsigned> (v & 0x3ff);
    if (area) return std::to_string (area) + "." + std::to_string (tid);
    return std::to_string (tid);
}

namespace {

// A node parameter: the address, and the name in brackets when one came
// with it.  On the wire it is a coded multiple of a two byte number and an
// optional ASCII image, or just the bare number.
std::string format_node (const Value &v)
{
    if (v.kind () != Value::Kind::cm)
        return v.is_number () ? format_node_number (v.as_uint ()) : v.format ();
    const Value::List &l = v.as_list ();
    if (l.empty ()) return "";
    std::string s = l[0].is_number () ? format_node_number (l[0].as_uint ())
                                      : l[0].format ();
    if (l.size () > 1) s += " (" + l[1].format () + ")";
    return s;
}

// A version: the three numbers joined by dots rather than by spaces.
std::string format_version (const Value &v)
{
    if (v.kind () != Value::Kind::cm) return v.format ();
    std::string s;
    for (const Value &e : v.as_list ()) {
        if (!s.empty ()) s += '.';
        s += e.format ();
    }
    return s;
}

}   // namespace

// ------------------------------------------------------------- ParamDef

const ParamDef *find_def (ParamDefs defs, std::uint16_t number, bool counter)
{
    for (const ParamDef &d : defs)
        if (d.number == number && d.counter == counter) return &d;
    return nullptr;
}

// ------------------------------------------------------------ ParamList

void ParamList::set (std::uint16_t number, Value v)
{
    Param p;
    p.number = number;
    p.counter = false;
    p.value = std::move (v);
    params_[key_of (number, false)] = std::move (p);
}

void ParamList::set_counter (std::uint16_t number, Counter c)
{
    Param p;
    p.number = number;
    p.counter = true;
    p.count = c;
    params_[key_of (number, true)] = std::move (p);
}

const Param *ParamList::find (std::uint16_t number, bool counter) const
{
    auto it = params_.find (key_of (number, counter));
    return it == params_.end () ? nullptr : &it->second;
}

void ParamList::encode (Encoder &e) const
{
    for (const auto &[key, p] : params_) {
        if (p.counter) {
            e.uint (p.number | counter_code (p.count.bytes, p.count.mapped), 2);
            if (p.count.mapped) e.uint (p.count.map, 2);
            // Saturate rather than truncate: a count that overflowed its
            // field reads as ">max - 1", not as a small number.
            std::uint64_t m = p.count.max ();
            e.uint (p.count.value > m ? m : p.count.value, p.count.bytes);
        } else {
            e.uint (p.number, 2);
            p.value.encode (e);
        }
    }
}

void ParamList::decode (Decoder &d)
{
    while (!d.empty ()) {
        std::uint16_t num = static_cast<std::uint16_t> (d.uint (2));
        if (num & 0x8000) {
            std::uint16_t code = num & 0xf000;
            Counter c;
            switch (code & 0xe000) {
            case 0xa000: c.bytes = 1; break;
            case 0xc000: c.bytes = 2; break;
            case 0xe000: c.bytes = 4; break;
            default:
                throw DecodeError ("invalid NICE counter code "
                                   + std::to_string (code));
            }
            c.mapped = (code & 0x1000) != 0;
            if (c.mapped) c.map = static_cast<std::uint16_t> (d.uint (2));
            c.value = d.uint (c.bytes);
            set_counter (num & 0x0fff, c);
        } else {
            set (num, Value::decode (d));
        }
    }
}

std::vector<std::string> ParamList::format (ParamDefs defs) const
{
    std::vector<std::string> out;
    out.reserve (params_.size ());
    for (const auto &[key, p] : params_) {
        const ParamDef *def = find_def (defs, p.number, p.counter);
        if (p.counter) {
            char lead[32];
            std::snprintf (lead, sizeof lead, "%11s", p.count.format ().c_str ());
            std::string s = lead;
            s += ' ';
            s += def ? std::string (def->desc)
                     : "Counter #" + std::to_string (p.number);
            if (p.count.map) {
                s += ", including\n";
                s += map_indent;
                bool first = true;
                for (unsigned b = 0; b < 16; ++b) {
                    if (!(p.count.map & (1u << b))) continue;
                    if (!first) { s += '\n'; s += map_indent; }
                    first = false;
                    if (def && b < def->labels.size () && def->labels[b])
                        s += def->labels[b];
                    else
                        s += "Qualifier #" + std::to_string (b);
                }
            }
            out.push_back (std::move (s));
        } else {
            std::string name = def ? def->desc
                                   : "Parameter #" + std::to_string (p.number);
            std::string text;
            switch (def ? def->style : Style::plain) {
            case Style::node:    text = format_node (p.value); break;
            case Style::version: text = format_version (p.value); break;
            default: text = p.value.format (def ? def->labels : Labels { });
            }
            out.push_back (name + " = " + text);
        }
    }
    return out;
}

}   // namespace decnet::nice
