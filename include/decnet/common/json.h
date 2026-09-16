// decnet/common/json.h -- minimal JSON for the application protocol.
//
// The external application protocol is one flat JSON object per line:
// string keys, and values that are strings, integers, booleans, null, or a
// flat array of those.  This reads and writes exactly that.
//
// Byte strings are encoded as latin-1 (byte n is code point n), matching
// PyDECnet's DNJsonEncoder.  Strings may contain NUL (MIRROR's function
// code is 0x00), so libraries that store values as C strings, such as
// cJSON, cannot be used.

#ifndef DECNET_COMMON_JSON_H
#define DECNET_COMMON_JSON_H

#include "decnet/common/exceptions.h"
#include "decnet/common/types.h"

#include <map>
#include <optional>
#include <string>
#include <variant>

namespace decnet::json {

struct ParseError : DNAException { using DNAException::DNAException; };

// One value in an object.
class Value {
public:
    using Array = std::vector<Value>;

    Value () = default;
    Value (std::string s) : v_ (std::move (s)) {}
    Value (const char *s) : v_ (std::string (s)) {}
    Value (std::int64_t i) : v_ (i) {}
    Value (int i) : v_ (static_cast<std::int64_t> (i)) {}
    Value (bool b) : v_ (b) {}
    Value (Array a) : v_ (std::move (a)) {}

    bool is_null () const noexcept { return v_.index () == 0; }
    bool is_string () const noexcept { return v_.index () == 1; }
    bool is_int () const noexcept { return v_.index () == 2; }
    bool is_bool () const noexcept { return v_.index () == 3; }
    bool is_array () const noexcept { return v_.index () == 4; }

    const std::string &as_string () const;
    std::int64_t as_int () const;
    bool as_bool () const;
    const Array &as_array () const;

    // The string read as bytes: latin-1, so one character is one byte.
    Bytes as_bytes () const;

    // However the value is typed, rendered as text -- what a log record's
    // argument substitution needs.
    std::string to_text () const;

    std::string encode () const;

private:
    std::variant<std::monostate, std::string, std::int64_t, bool,
                 std::vector<Value>> v_;
};

// A flat JSON object.  Key order is preserved on output so that encoded
// messages are reproducible, which makes them testable.
class Object {
public:
    void set (std::string key, Value v);
    // Store bytes as a latin-1 string.
    void set_bytes (std::string key, ByteView b);

    bool has (const std::string &key) const;
    const Value *get (const std::string &key) const;

    // Typed accessors with a default, since most fields are optional.
    std::string str (const std::string &key,
                     const std::string &dflt = {}) const;
    std::int64_t num (const std::string &key, std::int64_t dflt = 0) const;
    Bytes bytes (const std::string &key) const;

    std::string encode () const;

    // Substitute this object's "args" array at each {} placeholder, as
    // PyDECnet formats application log records.  Extra placeholders are left
    // as they are.
    std::string format_message (const std::string &key = "message",
                                const std::string &argkey = "args") const;

    // Parse one object.  Throws ParseError on anything that is not a flat
    // object of the shape described above.
    static Object parse (const std::string &text);

    std::size_t size () const noexcept { return order_.size (); }

private:
    std::vector<std::string>      order_;
    std::map<std::string, Value>  fields_;
};

// Escape a string into JSON form, including the \uXXXX escapes a latin-1
// byte above 0x7f needs.
std::string quote (const std::string &s);

}   // namespace decnet::json

#endif  // DECNET_COMMON_JSON_H
