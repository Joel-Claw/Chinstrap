// =========================================================================
// json.hpp - Minimal JSON Parser
// =========================================================================
// TEACHING NOTE: JSON (JavaScript Object Notation) is the most common
// data format on the web. Browsers use it for:
//   - Configuration files (like our config)
//   - API responses (REST, AJAX)
//   - Web manifests (package.json, WebAppManifest)
//   - Data exchange between client and server
//
// JSON has six data types:
//   1. String  - "hello" (double-quoted, with escape sequences)
//   2. Number  - 42, 3.14, -0.5 (integer or floating point)
//   3. Object  - {"key": value, "key2": value2} (unordered map)
//   4. Array   - [value, value, value] (ordered list)
//   5. Boolean - true or false
//   6. null    - null (absence of value)
//
// We implement a recursive descent parser. Recursive descent means each
// JSON type has a function that parses it, and functions call each other
// recursively for nested structures (e.g., an object containing arrays
// containing objects).
//
// Real browsers have JSON parsers in their JavaScript engines (V8 in
// Chrome, SpiderMonkey in Firefox). Those are highly optimized with
// just-in-time compilation. Our parser is simple and correct.
// =========================================================================

#ifndef CHINSTRAP_JSON_HPP
#define CHINSTRAP_JSON_HPP

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <stdexcept>

namespace chinstrap {

// -------------------------------------------------------------------------
// JsonValue - A tagged union of all JSON types
// -------------------------------------------------------------------------
// TEACHING NOTE: We use std::variant (C++17) to represent JSON values.
// A variant holds one of several types at a time and knows which one
// is active. This is a type-safe alternative to a union or void*.
//
// The types are:
//   - std::nullptr_t  -> JSON null
//   - bool            -> JSON true/false
//   - double         -> JSON number (we use double for all numbers)
//   - std::string    -> JSON string
//   - JsonArray      -> JSON array (vector of JsonValue)
//   - JsonObject     -> JSON object (map of string -> JsonValue)
//
// In C++17, std::variant gives us a clean, type-safe way to handle this.
// Before C++17, you would use a tagged union with manual memory management.
// -------------------------------------------------------------------------

// Forward declarations
class JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

class JsonValue {
public:
    // The variant holding the actual value
    using ValueType = std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        JsonArray,
        JsonObject
    >;

    JsonValue() : value_(nullptr) {}
    JsonValue(std::nullptr_t) : value_(nullptr) {}
    JsonValue(bool b) : value_(b) {}
    JsonValue(double d) : value_(d) {}
    JsonValue(int i) : value_(static_cast<double>(i)) {}
    JsonValue(const std::string& s) : value_(s) {}
    JsonValue(const char* s) : value_(std::string(s)) {}
    JsonValue(const JsonArray& arr) : value_(arr) {}
    JsonValue(const JsonObject& obj) : value_(obj) {}

    // Type checks
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_number() const { return std::holds_alternative<double>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<JsonArray>(value_); }
    bool is_object() const { return std::holds_alternative<JsonObject>(value_); }

    // Value accessors (throw if wrong type)
    bool as_bool() const { return std::get<bool>(value_); }
    double as_number() const { return std::get<double>(value_); }
    int as_int() const { return static_cast<int>(std::get<double>(value_)); }
    const std::string& as_string() const { return std::get<std::string>(value_); }
    const JsonArray& as_array() const { return std::get<JsonArray>(value_); }
    const JsonObject& as_object() const { return std::get<JsonObject>(value_); }

    // Mutable accessors
    JsonArray& as_array_mut() { return std::get<JsonArray>(value_); }
    JsonObject& as_object_mut() { return std::get<JsonObject>(value_); }

    // Convenience: object key access
    // TEACHING NOTE: This makes it easy to chain accesses:
    //   json["config"]["homepage"].as_string()
    const JsonValue& operator[](const std::string& key) const;
    const JsonValue& operator[](std::size_t index) const;

    // Check if object has a key
    bool has(const std::string& key) const;

    // Serialize back to JSON string
    // TEACHING NOTE: This is the inverse of parsing. We walk the value
    // tree and produce JSON text. We need to handle string escaping
    // (quotes, backslashes, control characters).
    std::string to_string(int indent = 0) const;

    // Parse a JSON string into a JsonValue
    // TEACHING NOTE: This is the main entry point. It creates a parser
    // internally and returns the root value. Throws on parse errors.
    static JsonValue parse(const std::string& text);

    // Parse a JSON file
    static JsonValue parse_file(const std::string& path);

private:
    ValueType value_;
};

// -------------------------------------------------------------------------
// JsonParser - Internal recursive descent parser
// -------------------------------------------------------------------------
// TEACHING NOTE: The parser is a class that holds the input string and
// a position cursor. Each parse_* method advances the cursor and returns
// a parsed value. This is the classic recursive descent pattern.
//
// The grammar (simplified from RFC 8259):
//   value  = object | array | string | number | true | false | null
//   object = '{' [ string ':' value (',' string ':' value)* ] '}'
//   array  = '[' [ value (',' value)* ] ']'
//   string = '"' char* '"'
//   number = [minus] int [frac] [exp]
//
// We skip whitespace between tokens. Strings handle standard escape
// sequences: \n \r \t \" \\ \/ \uXXXX
// -------------------------------------------------------------------------

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text), pos_(0) {}

    JsonValue parse();

private:
    const std::string& text_;
    std::size_t pos_;

    void skip_whitespace();
    JsonValue parse_value();
    JsonValue parse_string();
    JsonValue parse_number();
    JsonValue parse_object();
    JsonValue parse_array();
    JsonValue parse_literal();  // true, false, null

    // Parse a string body (without surrounding quotes)
    std::string parse_string_body();

    // Peek at the current character without advancing
    char peek() const;
    char advance();
    bool at_end() const;
    bool match(char expected);
    void expect(char expected, const char* context);
};

} // namespace chinstrap

#endif // CHINSTRAP_JSON_HPP