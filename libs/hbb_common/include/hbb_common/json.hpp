// json.hpp — minimal serde_json stand-in for the JSON that src/ipc.rs exchanges.
//
// Upstream ipc.rs encodes its `Data` enum with serde_json (`to_vec` /
// `from_str`). This module provides exactly what that needs, the same way
// toml.hpp stands in for confy's TOML:
//   - Value: null / bool / number / string / array / object. Numbers keep
//     serde_json's three-way split (PosInt u64, NegInt i64, Float f64), so
//     integer fields can apply serde's range rules (an i32 field rejects
//     2^31, a u8 rejects 256, any integer field rejects 1.0 and -0).
//   - dump(): compact output, byte-identical to serde_json::to_string for every
//     shape ipc.rs emits (insertion order, no whitespace, serde_json escapes).
//   - parse(): strict RFC 8259 parsing with serde_json's failure classes
//     (trailing characters, raw control characters in strings, lone
//     surrogates, leading zeros, recursion limit 128). Errors throw JsonError.
//   - is_valid_utf8(): std::str::from_utf8 parity. ipc.rs checks UTF-8
//     before it hands bytes to serde_json.
// Objects keep insertion order and duplicate keys. serde_derive structs reject
// duplicate fields while HashMap decoding keeps the last one, so the caller
// applies whichever rule fits. Floats are parsed and emitted in shortest
// round-trip form (std::to_chars), but no ipc.rs shape uses them, so ryu's
// exact layout (for example "1e21" against "1e+21") is not reproduced.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace hbb_common::json {

// serde_json::Error parity (message only; no line/column bookkeeping).
struct JsonError : std::runtime_error {
    explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

struct Value;
struct Member;
using Array = std::vector<Value>;
using Object = std::vector<Member>;  // insertion-ordered, duplicates kept

struct Value {
    using Storage =
        std::variant<std::nullptr_t, bool, uint64_t, int64_t, double, std::string, Array, Object>;
    Storage data;

    Value() : data(std::in_place_type<std::nullptr_t>, nullptr) {}
    Value(std::nullptr_t) : data(std::in_place_type<std::nullptr_t>, nullptr) {}
    Value(bool b) : data(std::in_place_type<bool>, b) {}
    // Integers normalize like serde_json::Number: non-negative values become
    // PosInt (u64) and negative ones NegInt (i64), whatever the source type.
    template <class T,
              std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T v) : data(make_int(v)) {}
    Value(double d) : data(std::in_place_type<double>, d) {}
    Value(const char* s) : data(std::in_place_type<std::string>, s) {}
    Value(std::string s) : data(std::in_place_type<std::string>, std::move(s)) {}
    Value(std::string_view s) : data(std::in_place_type<std::string>, s) {}
    Value(Array a) : data(std::in_place_type<Array>, std::move(a)) {}
    Value(Object o) : data(std::in_place_type<Object>, std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_u64() const { return std::holds_alternative<uint64_t>(data); }
    bool is_i64() const { return std::holds_alternative<int64_t>(data); }
    bool is_f64() const { return std::holds_alternative<double>(data); }
    bool is_number() const { return is_u64() || is_i64() || is_f64(); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }

    const bool* as_bool() const { return std::get_if<bool>(&data); }
    const std::string* as_string() const { return std::get_if<std::string>(&data); }
    const Array* as_array() const { return std::get_if<Array>(&data); }
    const Object* as_object() const { return std::get_if<Object>(&data); }

    // serde integer visitors: PosInt/NegInt in range are accepted, floats
    // never are (serde_json reports "invalid type: floating point").
    std::optional<int64_t> as_i64() const {
        if (const auto* u = std::get_if<uint64_t>(&data)) {
            if (*u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return static_cast<int64_t>(*u);
            }
            return std::nullopt;
        }
        if (const auto* i = std::get_if<int64_t>(&data)) {
            return *i;
        }
        return std::nullopt;
    }
    std::optional<uint64_t> as_u64() const {
        if (const auto* u = std::get_if<uint64_t>(&data)) {
            return *u;
        }
        return std::nullopt;
    }
    std::optional<double> as_f64() const {
        if (const auto* d = std::get_if<double>(&data)) {
            return *d;
        }
        if (const auto* u = std::get_if<uint64_t>(&data)) {
            return static_cast<double>(*u);
        }
        if (const auto* i = std::get_if<int64_t>(&data)) {
            return static_cast<double>(*i);
        }
        return std::nullopt;
    }

private:
    template <class T>
    static Storage make_int(T v) {
        if constexpr (std::is_signed_v<T>) {
            if (v < 0) {
                return Storage(std::in_place_type<int64_t>, static_cast<int64_t>(v));
            }
        }
        return Storage(std::in_place_type<uint64_t>, static_cast<uint64_t>(v));
    }
};

struct Member {
    std::string key;
    Value value;
};

// Deep structural equality (u64 7 and i64 7 never coexist: see make_int).
bool operator==(const Value& a, const Value& b);
bool operator==(const Member& a, const Member& b);

// serde_json::to_string parity (compact).
std::string dump(const Value& value);

// serde_json::from_str::<Value> parity: whitespace around the value is
// allowed, anything else after it is "trailing characters". Throws JsonError.
Value parse(std::string_view text);

// std::str::from_utf8 parity: rejects overlong forms, UTF-16 surrogates
// (U+D800..U+DFFF), code points above U+10FFFF and truncated sequences.
bool is_valid_utf8(std::string_view bytes);

// String::from_utf8_lossy parity: each maximal invalid subsequence (Rust's
// Utf8Chunks rule) becomes one U+FFFD. Valid input comes back unchanged.
std::string from_utf8_lossy(std::string_view bytes);

}  // namespace hbb_common::json
