// json.cpp — see json.hpp.
#include <hbb_common/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace hbb_common::json {

bool operator==(const Value& a, const Value& b) {
    return a.data == b.data;
}

bool operator==(const Member& a, const Member& b) {
    return a.key == b.key && a.value == b.value;
}

namespace {

// serde_json's default recursion limit: nesting depth 128 is rejected.
constexpr int kRecursionLimit = 128;

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) {
            fail("trailing characters");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw JsonError(what + " at offset " + std::to_string(pos_));
    }

    bool at_end() const { return pos_ >= s_.size(); }

    void skip_ws() {
        while (!at_end()) {
            const char c = s_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    void enter() {
        if (++depth_ >= kRecursionLimit) {
            fail("recursion limit exceeded");
        }
    }

    void leave() { --depth_; }

    void expect_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) {
            fail("expected ident");
        }
        pos_ += lit.size();
    }

    Value parse_value() {
        if (at_end()) {
            fail("EOF while parsing a value");
        }
        const char c = s_[pos_];
        switch (c) {
            case 'n':
                expect_literal("null");
                return Value(nullptr);
            case 't':
                expect_literal("true");
                return Value(true);
            case 'f':
                expect_literal("false");
                return Value(false);
            case '"':
                return Value(parse_string());
            case '[':
                return parse_array();
            case '{':
                return parse_object();
            default:
                break;
        }
        if (c == '-' || is_digit(c)) {
            return parse_number();
        }
        fail("expected value");
    }

    Value parse_array() {
        enter();
        ++pos_;  // '['
        Array out;
        skip_ws();
        if (!at_end() && s_[pos_] == ']') {
            ++pos_;
            leave();
            return Value(std::move(out));
        }
        while (true) {
            skip_ws();
            out.push_back(parse_value());
            skip_ws();
            if (at_end()) {
                fail("EOF while parsing a list");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (!at_end() && s_[pos_] == ']') {
                    fail("trailing comma");
                }
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                leave();
                return Value(std::move(out));
            }
            fail("expected `,` or `]`");
        }
    }

    Value parse_object() {
        enter();
        ++pos_;  // '{'
        Object out;
        skip_ws();
        if (!at_end() && s_[pos_] == '}') {
            ++pos_;
            leave();
            return Value(std::move(out));
        }
        while (true) {
            skip_ws();
            if (at_end()) {
                fail("EOF while parsing an object");
            }
            if (s_[pos_] != '"') {
                fail("key must be a string");
            }
            std::string key = parse_string();
            skip_ws();
            if (at_end()) {
                fail("EOF while parsing an object");
            }
            if (s_[pos_] != ':') {
                fail("expected `:`");
            }
            ++pos_;
            skip_ws();
            Value value = parse_value();
            out.push_back(Member{std::move(key), std::move(value)});
            skip_ws();
            if (at_end()) {
                fail("EOF while parsing an object");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (!at_end() && s_[pos_] == '}') {
                    fail("trailing comma");
                }
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                leave();
                return Value(std::move(out));
            }
            fail("expected `,` or `}`");
        }
    }

    uint32_t parse_hex4() {
        if (pos_ + 4 > s_.size()) {
            fail("EOF while parsing a string");
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                fail("invalid escape");
            }
        }
        return v;
    }

    std::string parse_string() {
        ++pos_;  // opening quote
        std::string out;
        while (true) {
            if (at_end()) {
                fail("EOF while parsing a string");
            }
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                if (at_end()) {
                    fail("EOF while parsing a string");
                }
                const char e = s_[pos_++];
                switch (e) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        uint32_t cp = parse_hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                                fail("lone leading surrogate in hex escape");
                            }
                            pos_ += 2;
                            const uint32_t lo = parse_hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) {
                                fail("lone leading surrogate in hex escape");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("lone trailing surrogate in hex escape");
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        fail("invalid escape");
                }
                continue;
            }
            if (c < 0x20) {
                fail("control character (\\u0000-\\u001F) found while parsing a string");
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    Value parse_number() {
        const size_t start = pos_;
        const bool negative = s_[pos_] == '-';
        if (negative) {
            ++pos_;
        }
        if (at_end()) {
            fail("EOF while parsing a value");
        }
        if (s_[pos_] == '0') {
            ++pos_;
            if (!at_end() && is_digit(s_[pos_])) {
                fail("invalid number");  // leading zeros are not JSON
            }
        } else if (is_digit(s_[pos_])) {
            while (!at_end() && is_digit(s_[pos_])) {
                ++pos_;
            }
        } else {
            fail("invalid number");
        }
        bool is_float = false;
        if (!at_end() && s_[pos_] == '.') {
            is_float = true;
            ++pos_;
            if (at_end() || !is_digit(s_[pos_])) {
                fail("invalid number");
            }
            while (!at_end() && is_digit(s_[pos_])) {
                ++pos_;
            }
        }
        if (!at_end() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (!at_end() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (at_end() || !is_digit(s_[pos_])) {
                fail("invalid number");
            }
            while (!at_end() && is_digit(s_[pos_])) {
                ++pos_;
            }
        }
        const char* first = s_.data() + start;
        const char* last = s_.data() + pos_;
        if (!is_float) {
            if (negative) {
                int64_t v = 0;
                const auto [p, ec] = std::from_chars(first, last, v);
                // serde_json turns -0 and i64 underflow into f64.
                if (ec == std::errc() && p == last && v != 0) {
                    return Value(v);
                }
            } else {
                uint64_t v = 0;
                const auto [p, ec] = std::from_chars(first, last, v);
                if (ec == std::errc() && p == last) {
                    return Value(v);  // u64 overflow falls through to f64
                }
            }
        }
        double d = 0.0;
        const auto [p, ec] = std::from_chars(first, last, d);
        if (p != last) {
            fail("invalid number");
        }
        if (ec == std::errc::result_out_of_range) {
            // from_chars leaves `d` untouched here. strtod tells overflow (inf:
            // serde_json "number out of range") from underflow (0: accepted).
            const double r = std::strtod(std::string(first, last).c_str(), nullptr);
            if (std::isinf(r)) {
                fail("number out of range");
            }
            return Value(r);
        }
        if (ec != std::errc()) {
            fail("invalid number");
        }
        return Value(d);
    }

    std::string_view s_;
    size_t pos_ = 0;
    int depth_ = 0;
};

void dump_string(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(ch);  // non-ASCII passes through as raw UTF-8
                }
        }
    }
    out.push_back('"');
}

void dump_float(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";  // serde_json writes NaN/inf as null
        return;
    }
    char buf[64];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, d);
    if (ec != std::errc()) {
        out += "null";
        return;
    }
    const std::string s(buf, p);
    const size_t e = s.find('e');
    if (e == std::string::npos) {
        out += s;
        if (s.find('.') == std::string::npos) {
            out += ".0";  // serde_json marks whole floats: 1.0, not 1
        }
        return;
    }
    // ryu-style exponent: no '+', no leading zeros ("1e21", "1e-7").
    out.append(s, 0, e + 1);
    size_t i = e + 1;
    if (i < s.size() && s[i] == '+') {
        ++i;
    } else if (i < s.size() && s[i] == '-') {
        out.push_back('-');
        ++i;
    }
    while (i + 1 < s.size() && s[i] == '0') {
        ++i;
    }
    out.append(s, i, std::string::npos);
}

void dump_value(std::string& out, const Value& v) {
    std::visit(
        [&out](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>) {
                out += std::to_string(x);
            } else if constexpr (std::is_same_v<T, double>) {
                dump_float(out, x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                dump_string(out, x);
            } else if constexpr (std::is_same_v<T, Array>) {
                out.push_back('[');
                bool first = true;
                for (const Value& e : x) {
                    if (!first) {
                        out.push_back(',');
                    }
                    first = false;
                    dump_value(out, e);
                }
                out.push_back(']');
            } else {
                static_assert(std::is_same_v<T, Object>);
                out.push_back('{');
                bool first = true;
                for (const Member& m : x) {
                    if (!first) {
                        out.push_back(',');
                    }
                    first = false;
                    dump_string(out, m.key);
                    out.push_back(':');
                    dump_value(out, m.value);
                }
                out.push_back('}');
            }
        },
        v.data);
}

}  // namespace

std::string dump(const Value& value) {
    std::string out;
    dump_value(out, value);
    return out;
}

Value parse(std::string_view text) {
    if (!is_valid_utf8(text)) {
        throw JsonError("invalid UTF-8");  // from_str takes &str: UTF-8 by construction
    }
    return Parser(text).parse_document();
}

bool is_valid_utf8(std::string_view bytes) {
    size_t i = 0;
    const size_t n = bytes.size();
    auto cont = [&](size_t k) {
        return k < n && (static_cast<unsigned char>(bytes[k]) & 0xC0) == 0x80;
    };
    while (i < n) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            if (!cont(i + 1)) {
                return false;
            }
            i += 2;
            continue;
        }
        if (c >= 0xE0 && c <= 0xEF) {
            if (i + 1 >= n) {
                return false;
            }
            const auto c1 = static_cast<unsigned char>(bytes[i + 1]);
            if (c == 0xE0 && (c1 < 0xA0 || c1 > 0xBF)) {
                return false;  // overlong
            }
            if (c == 0xED && (c1 < 0x80 || c1 > 0x9F)) {
                return false;  // UTF-16 surrogate range
            }
            if (!cont(i + 1) || !cont(i + 2)) {
                return false;
            }
            i += 3;
            continue;
        }
        if (c >= 0xF0 && c <= 0xF4) {
            if (i + 1 >= n) {
                return false;
            }
            const auto c1 = static_cast<unsigned char>(bytes[i + 1]);
            if (c == 0xF0 && (c1 < 0x90 || c1 > 0xBF)) {
                return false;  // overlong
            }
            if (c == 0xF4 && (c1 < 0x80 || c1 > 0x8F)) {
                return false;  // above U+10FFFF
            }
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) {
                return false;
            }
            i += 4;
            continue;
        }
        return false;  // C0/C1, F5..FF, stray continuation byte
    }
    return true;
}

std::string from_utf8_lossy(std::string_view bytes) {
    // core::str::lossy::Utf8Chunks: walk one sequence at a time; on error the
    // invalid chunk is the lead byte plus the continuation bytes accepted so
    // far, and it becomes a single U+FFFD.
    const size_t n = bytes.size();
    auto at = [&](size_t k) -> unsigned {
        return k < n ? static_cast<unsigned char>(bytes[k]) : 0u;  // safe_get: 0 past the end
    };
    auto cont = [](unsigned b) { return (b & 0xC0) == 0x80; };
    std::string out;
    out.reserve(n);
    size_t valid_start = 0;
    size_t i = 0;
    while (i < n) {
        const size_t start = i;
        const unsigned byte = at(i++);
        bool ok = true;
        if (byte >= 0x80) {
            if (byte >= 0xC2 && byte <= 0xDF) {
                if (cont(at(i))) {
                    ++i;
                } else {
                    ok = false;
                }
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                const unsigned c1 = at(i);
                const bool lead = byte == 0xE0   ? (c1 >= 0xA0 && c1 <= 0xBF)
                                  : byte == 0xED ? (c1 >= 0x80 && c1 <= 0x9F)
                                                 : (c1 >= 0x80 && c1 <= 0xBF);
                if (!lead) {
                    ok = false;
                } else if (!cont(at(++i))) {
                    ok = false;
                } else {
                    ++i;
                }
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                const unsigned c1 = at(i);
                const bool lead = byte == 0xF0   ? (c1 >= 0x90 && c1 <= 0xBF)
                                  : byte == 0xF4 ? (c1 >= 0x80 && c1 <= 0x8F)
                                                 : (c1 >= 0x80 && c1 <= 0xBF);
                if (!lead) {
                    ok = false;
                } else if (!cont(at(++i))) {
                    ok = false;
                } else if (!cont(at(++i))) {
                    ok = false;
                } else {
                    ++i;
                }
            } else {
                ok = false;  // C0/C1, F5..FF, stray continuation byte
            }
        }
        if (!ok) {
            out.append(bytes.substr(valid_start, start - valid_start));
            out.append("\xEF\xBF\xBD");
            valid_start = i;
        }
    }
    out.append(bytes.substr(valid_start));
    return out;
}

}  // namespace hbb_common::json
