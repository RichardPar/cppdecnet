#include "decnet/common/json.h"

#include <charconv>
#include <cstdio>

namespace decnet::json {

// ----------------------------------------------------------------- Value

const std::string &Value::as_string () const
{
    if (!is_string ()) throw ParseError ("value is not a string");
    return std::get<std::string> (v_);
}

std::int64_t Value::as_int () const
{
    if (!is_int ()) throw ParseError ("value is not a number");
    return std::get<std::int64_t> (v_);
}

bool Value::as_bool () const
{
    if (!is_bool ()) throw ParseError ("value is not a boolean");
    return std::get<bool> (v_);
}

const Value::Array &Value::as_array () const
{
    if (!is_array ()) throw ParseError ("value is not an array");
    return std::get<Array> (v_);
}

double Value::as_double () const
{
    if (is_int ()) return static_cast<double> (std::get<std::int64_t> (v_));
    if (!is_double ()) throw ParseError ("value is not a number");
    return std::get<double> (v_);
}

Value::Value (Object o) : v_ (std::make_shared<const Object> (std::move (o))) {}

const Object &Value::as_object () const
{
    if (!is_object ()) throw ParseError ("value is not an object");
    return *std::get<std::shared_ptr<const Object>> (v_);
}

namespace {

// Shortest text that reads back as the same double.
std::string number (double d)
{
    char buf[32];
    auto [p, ec] = std::to_chars (buf, buf + sizeof buf, d);
    if (ec != std::errc ()) return "0";
    std::string out (buf, p);
    // Keep it a JSON number with a fraction, so it reads back as a double.
    if (out.find_first_of (".eE") == std::string::npos
        && out.find_first_not_of ("-0123456789") == std::string::npos)
        out += ".0";
    return out;
}

}   // namespace

std::string Value::to_text () const
{
    switch (v_.index ()) {
    case 0: return "null";
    case 1: return std::get<std::string> (v_);
    case 2: return std::to_string (std::get<std::int64_t> (v_));
    case 3: return std::get<bool> (v_) ? "true" : "false";
    case 4: {
        std::string out;
        bool first = true;
        for (const Value &e : std::get<Array> (v_)) {
            if (!first) out += ", ";
            first = false;
            out += e.to_text ();
        }
        return out;
    }
    case 5: return number (std::get<double> (v_));
    case 6: return as_object ().encode ();
    }
    return {};
}

Bytes Value::as_bytes () const
{
    const std::string &s = as_string ();
    return Bytes (s.begin (), s.end ());
}

std::string Value::encode () const
{
    switch (v_.index ()) {
    case 0: return "null";
    case 1: return quote (std::get<std::string> (v_));
    case 2: return std::to_string (std::get<std::int64_t> (v_));
    case 3: return std::get<bool> (v_) ? "true" : "false";
    case 4: {
        std::string out = "[";
        bool first = true;
        for (const Value &e : std::get<Array> (v_)) {
            if (!first) out += ',';
            first = false;
            out += e.encode ();
        }
        out += ']';
        return out;
    }
    case 5: return number (std::get<double> (v_));
    case 6: return as_object ().encode ();
    }
    return "null";
}

// ---------------------------------------------------------------- quote

std::string quote (const std::string &s)
{
    std::string out = "\"";
    char buf[8];
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            // Escape control characters and bytes above 0x7f as \u00XX.
            if (c < 0x20 || c > 0x7e) {
                std::snprintf (buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char> (c);
            }
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------- Object

void Object::set (std::string key, Value v)
{
    if (!fields_.count (key)) order_.push_back (key);
    fields_[std::move (key)] = std::move (v);
}

void Object::set_bytes (std::string key, ByteView b)
{
    set (std::move (key), std::string (b.begin (), b.end ()));
}

bool Object::has (const std::string &key) const
{
    return fields_.count (key) != 0;
}

const Value *Object::get (const std::string &key) const
{
    auto it = fields_.find (key);
    return it == fields_.end () ? nullptr : &it->second;
}

std::string Object::str (const std::string &key, const std::string &dflt) const
{
    const Value *v = get (key);
    return (v && v->is_string ()) ? v->as_string () : dflt;
}

std::int64_t Object::num (const std::string &key, std::int64_t dflt) const
{
    const Value *v = get (key);
    return (v && v->is_int ()) ? v->as_int () : dflt;
}

Bytes Object::bytes (const std::string &key) const
{
    const Value *v = get (key);
    if (!v || !v->is_string ()) return {};
    return v->as_bytes ();
}

std::string Object::encode () const
{
    std::string out = "{";
    bool first = true;
    for (const std::string &k : order_) {
        if (!first) out += ',';
        first = false;
        out += quote (k);
        out += ':';
        out += fields_.at (k).encode ();
    }
    out += '}';
    return out;
}

// ---------------------------------------------------------------- parse

namespace {

struct Parser {
    const std::string &s;
    std::size_t        i = 0;

    void skip_ws ()
    {
        while (i < s.size () && (s[i] == ' ' || s[i] == '\t'
                                 || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    char peek ()
    {
        if (i >= s.size ()) throw ParseError ("unexpected end of JSON");
        return s[i];
    }

    void expect (char c)
    {
        skip_ws ();
        if (peek () != c)
            throw ParseError (std::string ("expected '") + c + "'");
        ++i;
    }

    std::string parse_string ()
    {
        expect ('"');
        std::string out;
        for (;;) {
            if (i >= s.size ()) throw ParseError ("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size ()) throw ParseError ("unterminated escape");
            char e = s[i++];
            switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (i + 4 > s.size ()) throw ParseError ("short \\u escape");
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s[i + static_cast<std::size_t> (k)];
                    v <<= 4;
                    if (h >= '0' && h <= '9')      v |= static_cast<unsigned> (h - '0');
                    else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned> (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned> (h - 'A' + 10);
                    else throw ParseError ("bad \\u escape");
                }
                i += 4;
                // Only the latin-1 range can be represented as one byte,
                // which is all this protocol uses.
                if (v > 0xff)
                    throw ParseError ("\\u escape outside latin-1");
                out += static_cast<char> (v);
                break;
            }
            default: throw ParseError ("unknown escape");
            }
        }
        return out;
    }

    Value parse_value ()
    {
        skip_ws ();
        char c = peek ();
        if (c == '"') return Value (parse_string ());
        if (c == '{') return Value (parse_object ());
        if (c == '[') {
            ++i;
            Value::Array a;
            skip_ws ();
            if (peek () == ']') { ++i; return Value (std::move (a)); }
            for (;;) {
                a.push_back (parse_value ());
                skip_ws ();
                char d = peek ();
                if (d == ',') { ++i; continue; }
                if (d == ']') { ++i; break; }
                throw ParseError ("expected ',' or ']'");
            }
            return Value (std::move (a));
        }
        if (c == 't') {
            if (s.compare (i, 4, "true") != 0) throw ParseError ("bad literal");
            i += 4;
            return Value (true);
        }
        if (c == 'f') {
            if (s.compare (i, 5, "false") != 0) throw ParseError ("bad literal");
            i += 5;
            return Value (false);
        }
        if (c == 'n') {
            if (s.compare (i, 4, "null") != 0) throw ParseError ("bad literal");
            i += 4;
            return Value ();
        }
        // A number: an integer, or a double if it has a fraction or an
        // exponent.
        std::size_t start = i;
        if (c == '-') ++i;
        while (i < s.size () && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start || (s[start] == '-' && i == start + 1))
            throw ParseError ("expected a value");
        if (i < s.size () && (s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
            while (i < s.size () && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'
                                     || s[i] == 'e' || s[i] == 'E'
                                     || s[i] == '+' || s[i] == '-'))
                ++i;
            double d = 0;
            auto [p, ec] = std::from_chars (s.data () + start, s.data () + i, d);
            if (ec != std::errc () || p != s.data () + i)
                throw ParseError ("bad number");
            return Value (d);
        }
        std::int64_t n = 0;
        auto [p, ec] = std::from_chars (s.data () + start, s.data () + i, n);
        (void) p;
        if (ec != std::errc ()) throw ParseError ("bad number");
        return Value (n);
    }

    Object parse_object ()
    {
        // Deep nesting would only come from a client trying to exhaust the
        // stack; nothing in the protocol goes past three levels.
        if (++depth > 32) throw ParseError ("JSON nested too deeply");
        Object o;
        expect ('{');
        skip_ws ();
        if (peek () == '}') { ++i; --depth; return o; }
        for (;;) {
            skip_ws ();
            std::string key = parse_string ();
            expect (':');
            o.set (key, parse_value ());
            skip_ws ();
            char c = peek ();
            if (c == ',') { ++i; continue; }
            if (c == '}') { ++i; break; }
            throw ParseError ("expected ',' or '}'");
        }
        --depth;
        return o;
    }

    int depth = 0;
};

}   // namespace

std::string Object::format_message (const std::string &key,
                                    const std::string &argkey) const
{
    std::string msg = str (key);
    const Value *args = get (argkey);
    if (!args || !args->is_array ()) return msg;

    std::string out;
    std::size_t next = 0;
    const Value::Array &a = args->as_array ();
    for (std::size_t i = 0; i < msg.size (); ++i) {
        if (msg[i] == '{' && i + 1 < msg.size () && msg[i + 1] == '}'
            && next < a.size ()) {
            out += a[next++].to_text ();
            ++i;
        } else {
            out += msg[i];
        }
    }
    return out;
}

Object Object::parse (const std::string &text)
{
    Parser p { text };
    Object o = p.parse_object ();
    p.skip_ws ();
    if (p.i != text.size ())
        throw ParseError ("trailing data after JSON object");
    return o;
}

}   // namespace decnet::json
