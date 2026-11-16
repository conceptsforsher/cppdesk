// test_json.cpp — hbb_common::json (serde_json stand-in used by src/ipc.rs).
// Expected texts are what serde_json 1.x produces / accepts / rejects.

#include <hbb_common/json.hpp>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using namespace hbb_common::json;

bool parses(const std::string& text) {
    try {
        (void)parse(text);
        return true;
    } catch (const JsonError&) {
        return false;
    }
}

std::string roundtrip(const std::string& text) {
    return dump(parse(text));
}

void test_dump_shapes() {
    CHECK(dump(Value()) == "null");
    CHECK(dump(Value(true)) == "true");
    CHECK(dump(Value(false)) == "false");
    CHECK(dump(Value(0)) == "0");
    CHECK(dump(Value(-5)) == "-5");
    CHECK(dump(Value(static_cast<uint64_t>(18446744073709551615ull))) == "18446744073709551615");
    CHECK(dump(Value(static_cast<int64_t>(-9223372036854775807ll - 1))) == "-9223372036854775808");
    CHECK(dump(Value("x")) == "\"x\"");
    CHECK(dump(Value(Array{})) == "[]");
    CHECK(dump(Value(Object{})) == "{}");
    Array a;
    a.emplace_back(1);
    a.emplace_back("two");
    a.emplace_back(nullptr);
    CHECK(dump(Value(a)) == "[1,\"two\",null]");
    // Insertion order is kept (serde_derive emits fields in declaration order).
    Object o;
    o.push_back(Member{"t", Value("Login")});
    o.push_back(Member{"c", Value(Object{Member{"id", Value(1)}, Member{"ok", Value(false)}})});
    CHECK(dump(Value(o)) == "{\"t\":\"Login\",\"c\":{\"id\":1,\"ok\":false}}");
}

void test_dump_escapes() {
    CHECK(dump(Value("a\"b\\c")) == "\"a\\\"b\\\\c\"");
    CHECK(dump(Value("\b\f\n\r\t")) == "\"\\b\\f\\n\\r\\t\"");
    CHECK(dump(Value(std::string("\x01\x1f", 2))) == "\"\\u0001\\u001f\"");  // lowercase hex
    CHECK(dump(Value(std::string("\0", 1))) == "\"\\u0000\"");
    CHECK(dump(Value("/")) == "\"/\"");          // '/' is not escaped
    CHECK(dump(Value("\x7f")) == "\"\x7f\"");    // DEL is not escaped
    CHECK(dump(Value("h\xc3\xa9llo \xe2\x82\xac")) == "\"h\xc3\xa9llo \xe2\x82\xac\"");  // raw UTF-8
}

void test_dump_floats() {
    CHECK(dump(Value(1.0)) == "1.0");
    CHECK(dump(Value(0.5)) == "0.5");
    CHECK(dump(Value(-2.25)) == "-2.25");
    CHECK(dump(Value(1e300)) == "1e300");
    CHECK(dump(Value(1e-7)) == "1e-7");
}

void test_numbers() {
    // serde_json::Number classes: PosInt / NegInt / Float.
    CHECK(parse("0").is_u64());
    CHECK(parse("42").is_u64());
    CHECK(parse("-42").is_i64());
    CHECK(parse("18446744073709551615").is_u64());
    CHECK(parse("18446744073709551616").is_f64());  // u64 overflow -> f64
    CHECK(parse("-9223372036854775808").is_i64());
    CHECK(parse("-9223372036854775809").is_f64());  // i64 underflow -> f64
    CHECK(parse("-0").is_f64());                    // serde_json: -0 is a float
    CHECK(parse("1.5").is_f64());
    CHECK(parse("1e2").is_f64());
    CHECK(parse("1E+2").is_f64());
    CHECK(parse("1e-400").is_f64());  // underflow is fine (0.0)
    CHECK(!parses("1e400"));          // overflow: "number out of range"
    CHECK(parse("42").as_i64() == 42);
    CHECK(parse("-1").as_u64() == std::nullopt);
    CHECK(parse("9223372036854775808").as_i64() == std::nullopt);
    CHECK(parse("1.0").as_i64() == std::nullopt);  // floats never satisfy integer fields
    CHECK(parse("-0").as_i64() == std::nullopt);
    CHECK(parse("2.5").as_f64() == 2.5);
    // Grammar.
    CHECK(!parses("01"));
    CHECK(!parses("-"));
    CHECK(!parses("1."));
    CHECK(!parses(".5"));
    CHECK(!parses("1e"));
    CHECK(!parses("+1"));
    CHECK(!parses("0x10"));
    CHECK(!parses("NaN"));
}

void test_strings() {
    CHECK(*parse("\"a\\\"b\"").as_string() == "a\"b");
    CHECK(*parse("\"\\/\\b\\f\\n\\r\\t\\\\\"").as_string() == "/\b\f\n\r\t\\");
    CHECK(*parse("\"\\u0041\"").as_string() == "A");
    CHECK(*parse("\"\\u00e9\"").as_string() == "\xc3\xa9");
    CHECK(*parse("\"\\u20AC\"").as_string() == "\xe2\x82\xac");
    CHECK(*parse("\"\\ud83d\\ude00\"").as_string() == "\xf0\x9f\x98\x80");  // surrogate pair
    CHECK(!parses("\"\\ud83d\""));          // lone leading surrogate
    CHECK(!parses("\"\\ud83d\\u0041\""));   // leading + non-trailing
    CHECK(!parses("\"\\ude00\""));          // lone trailing surrogate
    CHECK(!parses("\"\\x41\""));            // invalid escape
    CHECK(!parses("\"\\u12\""));            // short hex escape
    CHECK(!parses(std::string("\"a\nb\"")));  // raw control character
    CHECK(!parses(std::string("\"\t\"")));
    CHECK(!parses("\"unterminated"));
    CHECK(!parses("\"bad \xff utf8\""));    // from_str only ever sees valid UTF-8
}

void test_structure() {
    CHECK(roundtrip(" { \"a\" : [ 1 , 2 ] , \"b\" : null } ") == "{\"a\":[1,2],\"b\":null}");
    CHECK(roundtrip("[]") == "[]");
    CHECK(roundtrip("{}") == "{}");
    CHECK(roundtrip("[true,false,null]") == "[true,false,null]");
    // Duplicates and order survive (callers apply serde's struct/map rules).
    const Value dup = parse("{\"k\":1,\"k\":2}");
    CHECK(dup.as_object() && dup.as_object()->size() == 2);
    CHECK(!parses(""));
    CHECK(!parses("   "));
    CHECK(!parses("[1,]"));        // trailing comma
    CHECK(!parses("{\"a\":1,}"));
    CHECK(!parses("[1 2]"));
    CHECK(!parses("{\"a\" 1}"));
    CHECK(!parses("{a:1}"));       // keys must be strings
    CHECK(!parses("{1:1}"));
    CHECK(!parses("[1]x"));        // trailing characters
    CHECK(!parses("nul"));
    CHECK(!parses("truth"));
    CHECK(!parses("[1"));
    CHECK(!parses("{\"a\":"));
    CHECK(parses("\r\n\t [1] \r\n\t"));
}

void test_recursion_limit() {
    // serde_json: 127 nested containers parse, the 128th is rejected.
    CHECK(parses(std::string(127, '[') + std::string(127, ']')));
    CHECK(!parses(std::string(128, '[') + std::string(128, ']')));
    std::string objs;
    for (int i = 0; i < 128; ++i) {
        objs += "{\"a\":";
    }
    objs += "1";
    objs += std::string(128, '}');
    CHECK(!parses(objs));
}

void test_equality() {
    CHECK(parse("{\"a\":[1,\"x\",null]}") == parse("{ \"a\" : [ 1, \"x\", null ] }"));
    CHECK(!(parse("[1]") == parse("[2]")));
    CHECK(!(parse("1") == parse("1.0")));  // PosInt vs Float
    CHECK(Value(7) == Value(static_cast<uint64_t>(7)));  // integer normalization
}

void test_utf8() {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("\xc3\xa9"));          // U+00E9
    CHECK(is_valid_utf8("\xe2\x82\xac"));      // U+20AC
    CHECK(is_valid_utf8("\xef\xbf\xbf"));      // U+FFFF
    CHECK(is_valid_utf8("\xf0\x9f\x98\x80"));  // U+1F600
    CHECK(is_valid_utf8("\xf4\x8f\xbf\xbf"));  // U+10FFFF
    CHECK(is_valid_utf8(std::string("\0", 1)));
    CHECK(!is_valid_utf8("\x80"));              // stray continuation
    CHECK(!is_valid_utf8("\xc0\x80"));          // overlong NUL
    CHECK(!is_valid_utf8("\xc1\xbf"));          // overlong
    CHECK(!is_valid_utf8("\xe0\x80\x80"));      // overlong 3-byte
    CHECK(!is_valid_utf8("\xed\xa0\x80"));      // U+D800 surrogate
    CHECK(!is_valid_utf8("\xf0\x80\x80\x80"));  // overlong 4-byte
    CHECK(!is_valid_utf8("\xf4\x90\x80\x80"));  // > U+10FFFF
    CHECK(!is_valid_utf8("\xf5\x80\x80\x80"));
    CHECK(!is_valid_utf8("\xff"));
    CHECK(!is_valid_utf8("\xc3"));              // truncated
    CHECK(!is_valid_utf8("\xe2\x82"));
    CHECK(!is_valid_utf8("\xf0\x9f\x98"));
    CHECK(!is_valid_utf8("\xc3\x28"));          // bad continuation
}

}  // namespace

void test_from_utf8_lossy() {
    using hbb_common::json::from_utf8_lossy;
    const std::string r = "\xef\xbf\xbd";  // U+FFFD
    CHECK(from_utf8_lossy("") == "");
    CHECK(from_utf8_lossy("plain") == "plain");
    CHECK(from_utf8_lossy("\xc3\xa9\xf0\x9f\x98\x80") == "\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(from_utf8_lossy(std::string("a\0b", 3)) == std::string("a\0b", 3));
    CHECK(from_utf8_lossy("\xff") == r);
    CHECK(from_utf8_lossy("a\x80" "b") == "a" + r + "b");
    // A truncated sequence is one chunk: lead + accepted continuations.
    CHECK(from_utf8_lossy("\xc3") == r);
    CHECK(from_utf8_lossy("\xe2\x82") == r);
    CHECK(from_utf8_lossy("\xe2\x82x") == r + "x");
    CHECK(from_utf8_lossy("\xf0\x9f\x98") == r);
    // std's documented example.
    CHECK(from_utf8_lossy("Hello \xF0\x90\x80World") == "Hello " + r + "World");
    // A bad second byte ends the chunk at the lead; the rest are strays.
    CHECK(from_utf8_lossy("\xed\xa0\x80") == r + r + r);  // surrogate
    CHECK(from_utf8_lossy("\xc0\x80") == r + r);           // overlong
    CHECK(from_utf8_lossy("\xf4\x90\x80\x80") == r + r + r + r);  // > U+10FFFF
}

int main() {
    test_dump_shapes();
    test_dump_escapes();
    test_dump_floats();
    test_numbers();
    test_strings();
    test_structure();
    test_recursion_limit();
    test_equality();
    test_utf8();
    test_from_utf8_lossy();
    if (g_failures == 0) {
        std::puts("test_json: all tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
