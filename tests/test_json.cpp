// =========================================================================
// test_json.cpp - Unit tests for JSON parser
// =========================================================================
// TEACHING NOTE: These tests exercise the JSON parser. We test:
//   - All JSON types (null, bool, number, string, array, object)
//   - Nested structures (object in array in object, etc.)
//   - String escape sequences (\n, \t, \uXXXX, etc.)
//   - Number formats (integers, floats, negative, exponent)
//   - Serialization (to_string round-trip)
//   - Error handling (malformed input)
// =========================================================================

#include "test_framework.hpp"
#include "json.hpp"

using namespace chinstrap;

// --- Basic types ---

TEST(parse_null) {
    JsonValue v = JsonValue::parse("null");
    ASSERT_TRUE(v.is_null());
}

TEST(parse_true) {
    JsonValue v = JsonValue::parse("true");
    ASSERT_TRUE(v.is_bool());
    ASSERT_TRUE(v.as_bool());
}

TEST(parse_false) {
    JsonValue v = JsonValue::parse("false");
    ASSERT_TRUE(v.is_bool());
    ASSERT_FALSE(v.as_bool());
}

TEST(parse_integer) {
    JsonValue v = JsonValue::parse("42");
    ASSERT_TRUE(v.is_number());
    ASSERT_EQ(v.as_int(), 42);
}

TEST(parse_float) {
    JsonValue v = JsonValue::parse("3.14");
    ASSERT_TRUE(v.is_number());
    // Use a small epsilon for float comparison
    double diff = v.as_number() - 3.14;
    ASSERT_TRUE(diff < 0.0001 && diff > -0.0001);
}

TEST(parse_negative_number) {
    JsonValue v = JsonValue::parse("-123");
    ASSERT_TRUE(v.is_number());
    ASSERT_EQ(v.as_int(), -123);
}

TEST(parse_exponent) {
    JsonValue v = JsonValue::parse("1e3");
    ASSERT_TRUE(v.is_number());
    ASSERT_EQ(v.as_int(), 1000);
}

TEST(parse_string) {
    JsonValue v = JsonValue::parse("\"hello\"");
    ASSERT_TRUE(v.is_string());
    ASSERT_STREQ(v.as_string(), "hello");
}

TEST(parse_empty_string) {
    JsonValue v = JsonValue::parse("\"\"");
    ASSERT_TRUE(v.is_string());
    ASSERT_STREQ(v.as_string(), "");
}

TEST(parse_empty_array) {
    JsonValue v = JsonValue::parse("[]");
    ASSERT_TRUE(v.is_array());
    ASSERT_EQ(v.as_array().size(), static_cast<std::size_t>(0));
}

TEST(parse_array_of_numbers) {
    JsonValue v = JsonValue::parse("[1, 2, 3]");
    ASSERT_TRUE(v.is_array());
    ASSERT_EQ(v.as_array().size(), static_cast<std::size_t>(3));
    ASSERT_EQ(v.as_array()[0].as_int(), 1);
    ASSERT_EQ(v.as_array()[1].as_int(), 2);
    ASSERT_EQ(v.as_array()[2].as_int(), 3);
}

TEST(parse_empty_object) {
    JsonValue v = JsonValue::parse("{}");
    ASSERT_TRUE(v.is_object());
    ASSERT_EQ(v.as_object().size(), static_cast<std::size_t>(0));
}

TEST(parse_simple_object) {
    JsonValue v = JsonValue::parse("{\"key\": \"value\"}");
    ASSERT_TRUE(v.is_object());
    ASSERT_EQ(v.as_object().size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(v["key"].as_string(), "value");
}

// --- Nested structures ---

TEST(parse_nested_object) {
    JsonValue v = JsonValue::parse("{\"outer\": {\"inner\": 42}}");
    ASSERT_TRUE(v.is_object());
    ASSERT_TRUE(v["outer"].is_object());
    ASSERT_EQ(v["outer"]["inner"].as_int(), 42);
}

TEST(parse_array_of_objects) {
    JsonValue v = JsonValue::parse("[{\"name\": \"Alice\"}, {\"name\": \"Bob\"}]");
    ASSERT_TRUE(v.is_array());
    ASSERT_EQ(v.as_array().size(), static_cast<std::size_t>(2));
    ASSERT_STREQ(v[0]["name"].as_string(), "Alice");
    ASSERT_STREQ(v[1]["name"].as_string(), "Bob");
}

TEST(parse_object_with_array) {
    JsonValue v = JsonValue::parse("{\"items\": [1, 2, 3]}");
    ASSERT_TRUE(v.is_object());
    ASSERT_TRUE(v["items"].is_array());
    ASSERT_EQ(v["items"].as_array().size(), static_cast<std::size_t>(3));
}

// --- String escapes ---

TEST(parse_string_with_escapes) {
    JsonValue v = JsonValue::parse("\"hello\\nworld\"");
    ASSERT_STREQ(v.as_string(), "hello\nworld");
}

TEST(parse_string_with_tab) {
    JsonValue v = JsonValue::parse("\"a\\tb\"");
    ASSERT_STREQ(v.as_string(), "a\tb");
}

TEST(parse_string_with_quote) {
    JsonValue v = JsonValue::parse("\"say \\\"hello\\\"\"");
    ASSERT_STREQ(v.as_string(), "say \"hello\"");
}

TEST(parse_string_with_backslash) {
    JsonValue v = JsonValue::parse("\"C:\\\\path\"");
    ASSERT_STREQ(v.as_string(), "C:\\path");
}

TEST(parse_string_with_unicode_escape) {
    JsonValue v = JsonValue::parse("\"\\u0041\"");
    ASSERT_STREQ(v.as_string(), "A");
}

// --- Whitespace handling ---

TEST(parse_with_whitespace) {
    JsonValue v = JsonValue::parse("  {  \"key\"  :  42  }  ");
    ASSERT_TRUE(v.is_object());
    ASSERT_EQ(v["key"].as_int(), 42);
}

// --- has() and operator[] ---

TEST(object_has_key) {
    JsonValue v = JsonValue::parse("{\"a\": 1, \"b\": 2}");
    ASSERT_TRUE(v.has("a"));
    ASSERT_TRUE(v.has("b"));
    ASSERT_FALSE(v.has("c"));
}

TEST(array_indexing) {
    JsonValue v = JsonValue::parse("[10, 20, 30]");
    ASSERT_EQ(v[0].as_int(), 10);
    ASSERT_EQ(v[1].as_int(), 20);
    ASSERT_EQ(v[2].as_int(), 30);
}

// --- Serialization ---

TEST(serialize_null) {
    JsonValue v;
    ASSERT_STREQ(v.to_string(), "null");
}

TEST(serialize_bool) {
    JsonValue t(true);
    JsonValue f(false);
    ASSERT_STREQ(t.to_string(), "true");
    ASSERT_STREQ(f.to_string(), "false");
}

TEST(serialize_integer) {
    JsonValue v(42);
    ASSERT_STREQ(v.to_string(), "42");
}

TEST(serialize_string) {
    JsonValue v("hello");
    ASSERT_STREQ(v.to_string(), "\"hello\"");
}

TEST(serialize_string_with_escapes) {
    JsonValue v("hello\nworld");
    std::string s = v.to_string();
    ASSERT_TRUE(s.find("\\n") != std::string::npos);
}

TEST(serialize_array) {
    JsonValue v(JsonArray{JsonValue(1), JsonValue(2), JsonValue(3)});
    ASSERT_STREQ(v.to_string(), "[1, 2, 3]");
}

TEST(serialize_object) {
    JsonObject obj;
    obj["key"] = JsonValue("value");
    JsonValue v(obj);
    ASSERT_STREQ(v.to_string(), "{\"key\": \"value\"}");
}

// --- Complex round-trip ---

TEST(serialize_roundtrip) {
    JsonObject obj;
    obj["name"] = JsonValue("Chinstrap");
    obj["version"] = JsonValue(static_cast<double>(1));
    obj["active"] = JsonValue(true);

    JsonArray arr;
    arr.push_back(JsonValue("http"));
    arr.push_back(JsonValue("https"));
    obj["schemes"] = JsonValue(arr);

    JsonValue original(obj);
    std::string serialized = original.to_string();
    JsonValue reparsed = JsonValue::parse(serialized);

    ASSERT_TRUE(reparsed.is_object());
    ASSERT_STREQ(reparsed["name"].as_string(), "Chinstrap");
    ASSERT_EQ(reparsed["version"].as_int(), 1);
    ASSERT_TRUE(reparsed["active"].as_bool());
    ASSERT_TRUE(reparsed["schemes"].is_array());
    ASSERT_EQ(reparsed["schemes"].as_array().size(), static_cast<std::size_t>(2));
}

// --- Error handling ---

TEST(parse_empty_input_throws) {
    bool threw = false;
    try {
        JsonValue::parse("");
    } catch (...) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(parse_trailing_comma_throws) {
    bool threw = false;
    try {
        JsonValue::parse("[1, 2, 3,]");
    } catch (...) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

RUN_TESTS()