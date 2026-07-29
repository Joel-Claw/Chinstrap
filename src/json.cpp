// =========================================================================
// json.cpp - JSON Parser Implementation
// =========================================================================
// TEACHING NOTE: This is a recursive descent JSON parser. Recursive
// descent means we have one function per grammar rule, and they call
// each other recursively. For example, parse_object() calls parse_value()
// for each value, and parse_value() might call parse_object() if the
// value is itself an object. This naturally handles nested structures.
//
// The parser is simple but correct. It follows RFC 8259 (the JSON spec).
// It does NOT support:
//   - Comments (JSON does not have comments by spec, but JSON5 does)
//   - Trailing commas (not valid JSON)
//   - Single-quoted strings (not valid JSON)
//
// Error handling: we throw std::runtime_error on parse errors with a
// position indicator for debugging.
// =========================================================================

#include "json.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <cstring>

namespace chinstrap {

// =========================================================================
// JsonValue implementation
// =========================================================================

const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (!is_object()) {
        throw std::runtime_error("JsonValue is not an object, cannot index by key");
    }
    const auto& obj = as_object();
    auto it = obj.find(key);
    if (it == obj.end()) {
        throw std::runtime_error("Key not found: " + key);
    }
    return it->second;
}

const JsonValue& JsonValue::operator[](std::size_t index) const {
    if (!is_array()) {
        throw std::runtime_error("JsonValue is not an array, cannot index");
    }
    const auto& arr = as_array();
    if (index >= arr.size()) {
        throw std::runtime_error("Array index out of bounds: " + std::to_string(index));
    }
    return arr[index];
}

bool JsonValue::has(const std::string& key) const {
    if (!is_object()) return false;
    const auto& obj = as_object();
    return obj.find(key) != obj.end();
}

std::string JsonValue::to_string(int indent) const {
    // TEACHING NOTE: Serialization is the inverse of parsing. We walk
    // the value tree and produce JSON text. The indent parameter controls
    // pretty-printing (indent > 0 means add whitespace for readability).
    //
    // String escaping is important: we must escape double quotes,
    // backslashes, and control characters (\n, \r, \t, etc.).
    // RFC 8259 requires escaping: quotation mark, reverse solidus,
    // and U+0000 through U+001F (control characters).

    std::ostringstream out;

    if (is_null()) {
        out << "null";
    } else if (is_bool()) {
        out << (as_bool() ? "true" : "false");
    } else if (is_number()) {
        // TEACHING NOTE: We output numbers as doubles. This may produce
        // trailing zeros (e.g., "42.000000"). A more sophisticated
        // serializer would track whether the number was originally an
        // integer or float, but JSON only has one number type.
        double d = as_number();
        if (d == static_cast<double>(static_cast<long long>(d))) {
            out << static_cast<long long>(d);
        } else {
            out << d;
        }
    } else if (is_string()) {
        // Escape the string
        out << '"';
        for (char c : as_string()) {
            switch (c) {
                case '"':  out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n";  break;
                case '\r': out << "\\r";  break;
                case '\t': out << "\\t";  break;
                case '\b': out << "\\b";  break;
                case '\f': out << "\\f";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Control character: \uXXXX
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out << buf;
                    } else {
                        out << c;
                    }
            }
        }
        out << '"';
    } else if (is_array()) {
        out << '[';
        const auto& arr = as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) out << ", ";
            if (indent > 0) out << '\n' << std::string(indent + 2, ' ');
            out << arr[i].to_string(indent > 0 ? indent + 2 : 0);
        }
        if (indent > 0 && !arr.empty()) out << '\n' << std::string(indent, ' ');
        out << ']';
    } else if (is_object()) {
        out << '{';
        const auto& obj = as_object();
        std::size_t i = 0;
        for (const auto& [key, value] : obj) {
            if (i > 0) out << ',';
            ++i;
            if (indent > 0) out << '\n' << std::string(indent + 2, ' ');
            out << '"' << key << "\": ";
            out << value.to_string(indent > 0 ? indent + 2 : 0);
        }
        if (indent > 0 && !obj.empty()) out << '\n' << std::string(indent, ' ');
        out << '}';
    }

    return out.str();
}

JsonValue JsonValue::parse(const std::string& text) {
    JsonParser parser(text);
    return parser.parse();
}

JsonValue JsonValue::parse_file(const std::string& path) {
    // TEACHING NOTE: We use basic_ifstream to read the file. This is
    // C++ stdlib, not a third-party library. We read the entire file
    // into a string and then parse it.
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return parse(content);
}

// =========================================================================
// JsonParser implementation
// =========================================================================

void JsonParser::skip_whitespace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
        pos_++;
    }
}

char JsonParser::peek() const {
    if (pos_ >= text_.size()) {
        throw std::runtime_error("Unexpected end of JSON input at position " +
                                 std::to_string(pos_));
    }
    return text_[pos_];
}

char JsonParser::advance() {
    if (pos_ >= text_.size()) {
        throw std::runtime_error("Unexpected end of JSON input at position " +
                                 std::to_string(pos_));
    }
    return text_[pos_++];
}

bool JsonParser::at_end() const {
    return pos_ >= text_.size();
}

bool JsonParser::match(char expected) {
    if (at_end() || text_[pos_] != expected) return false;
    pos_++;
    return true;
}

void JsonParser::expect(char expected, const char* context) {
    if (!match(expected)) {
        std::string found = at_end() ? "end of input" : std::string(1, text_[pos_]);
        throw std::runtime_error(std::string("JSON parse error at position ") +
                                 std::to_string(pos_) + ": expected '" + expected +
                                 "' (" + context + ") but found '" + found + "'");
    }
}

JsonValue JsonParser::parse() {
    skip_whitespace();
    JsonValue result = parse_value();
    skip_whitespace();
    if (!at_end()) {
        throw std::runtime_error("JSON parse error: trailing characters at position " +
                                 std::to_string(pos_));
    }
    return result;
}

JsonValue JsonParser::parse_value() {
    // TEACHING NOTE: This is the heart of the parser. It dispatches
    // based on the first character:
    //   '{' -> object
    //   '[' -> array
    //   '"' -> string
    //   't' -> true
    //   'f' -> false
    //   'n' -> null
    //   '-' or digit -> number
    //
    // This is exactly how the JSON grammar works: the first character
    // tells you which type to parse.
    skip_whitespace();
    if (at_end()) {
        throw std::runtime_error("JSON parse error: unexpected end of input");
    }

    char c = peek();
    switch (c) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return parse_string();
        case 't': case 'f': case 'n': return parse_literal();
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number();
        default:
            throw std::runtime_error(std::string("JSON parse error: unexpected character '") +
                                     c + "' at position " + std::to_string(pos_));
    }
}

JsonValue JsonParser::parse_string() {
    return JsonValue(parse_string_body());
}

std::string JsonParser::parse_string_body() {
    // TEACHING NOTE: JSON strings are enclosed in double quotes and
    // can contain escape sequences:
    //   \"  - quotation mark
    //   \\  - reverse solidus
    //   \/  - solidus (optional to escape)
    //   \b  - backspace
    //   \f  - form feed
    //   \n  - line feed
    //   \r  - carriage return
    //   \t  - tab
    //   \uXXXX - unicode code point (4 hex digits)
    //
    // We handle all of these except \uXXXX (unicode escapes) is handled
    // but only for the BMP (Basic Multilingual Plane, U+0000 to U+FFFF).
    // Real browsers handle full unicode including surrogate pairs.

    expect('"', "string start");

    std::string result;
    while (true) {
        if (at_end()) {
            throw std::runtime_error("JSON parse error: unterminated string");
        }

        char c = advance();

        if (c == '"') {
            break;  // End of string
        }

        if (c == '\\') {
            // Escape sequence
            if (at_end()) {
                throw std::runtime_error("JSON parse error: unterminated escape sequence");
            }
            char escaped = advance();
            switch (escaped) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'u': {
                    // Unicode escape: \uXXXX
                    // TEACHING NOTE: We read 4 hex digits and convert to
                    // a character. For code points > 127, we encode as
                    // UTF-8. This is simplified: real JSON parsers handle
                    // surrogate pairs (\uD83D\uDE00 = emoji).
                    if (pos_ + 4 > text_.size()) {
                        throw std::runtime_error("JSON parse error: incomplete \\u escape");
                    }
                    std::string hex = text_.substr(pos_, 4);
                    pos_ += 4;
                    unsigned int code_point = 0;
                    try {
                        code_point = static_cast<unsigned int>(std::stoul(hex, nullptr, 16));
                    } catch (...) {
                        throw std::runtime_error("JSON parse error: invalid \\u escape: " + hex);
                    }

                    // Encode as UTF-8
                    // TEACHING NOTE: UTF-8 is a variable-length encoding.
                    // Code points 0-127 are 1 byte, 128-2047 are 2 bytes,
                    // 2048-65535 are 3 bytes, 65536+ are 4 bytes.
                    if (code_point < 0x80) {
                        result += static_cast<char>(code_point);
                    } else if (code_point < 0x800) {
                        result += static_cast<char>(0xC0 | (code_point >> 6));
                        result += static_cast<char>(0x80 | (code_point & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | (code_point >> 12));
                        result += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (code_point & 0x3F));
                    }
                    break;
                }
                default:
                    throw std::runtime_error(std::string("JSON parse error: invalid escape '\\") +
                                            escaped + "' at position " + std::to_string(pos_));
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            throw std::runtime_error("JSON parse error: unescaped control character in string");
        } else {
            result += c;
        }
    }

    return result;
}

JsonValue JsonParser::parse_number() {
    // TEACHING NOTE: JSON numbers follow this grammar:
    //   number = [ minus ] int [ frac ] [ exp ]
    //   int    = zero / ( digit1-9 *digit )
    //   frac   = "." 1*digit
    //   exp    = ("e" / "E") ["+" / "-"] 1*digit
    //
    // We read the number as a substring and convert with stod.
    // This is simple and correct. A more optimized parser would
    // compute the value during parsing.

    std::size_t start = pos_;

    // Optional minus
    if (peek() == '-') advance();

    // Integer part
    if (peek() == '0') {
        advance();
    } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
        while (!at_end() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            pos_++;
        }
    } else {
        throw std::runtime_error("JSON parse error: invalid number at position " +
                                 std::to_string(pos_));
    }

    // Fractional part
    if (!at_end() && text_[pos_] == '.') {
        pos_++;
        if (at_end() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            throw std::runtime_error("JSON parse error: digit expected after decimal point");
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            pos_++;
        }
    }

    // Exponent part
    if (!at_end() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
        pos_++;
        if (!at_end() && (text_[pos_] == '+' || text_[pos_] == '-')) {
            pos_++;
        }
        if (at_end() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            throw std::runtime_error("JSON parse error: digit expected in exponent");
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            pos_++;
        }
    }

    std::string num_str = text_.substr(start, pos_ - start);
    try {
        return JsonValue(std::stod(num_str));
    } catch (...) {
        throw std::runtime_error("JSON parse error: invalid number: " + num_str);
    }
}

JsonValue JsonParser::parse_literal() {
    // TEACHING NOTE: JSON has three literal values: true, false, null.
    // We read characters and match against these. This is straightforward.
    char c = peek();

    if (c == 't') {
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return JsonValue(true);
        }
    } else if (c == 'f') {
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return JsonValue(false);
        }
    } else if (c == 'n') {
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return JsonValue(nullptr);
        }
    }

    throw std::runtime_error("JSON parse error: invalid literal at position " +
                             std::to_string(pos_));
}

JsonValue JsonParser::parse_array() {
    // TEACHING NOTE: A JSON array is [ value, value, value, ... ]
    // We parse the opening bracket, then repeatedly parse values
    // separated by commas, until the closing bracket.
    expect('[', "array start");

    JsonArray arr;
    skip_whitespace();

    if (match(']')) {
        return JsonValue(arr);  // Empty array
    }

    while (true) {
        arr.push_back(parse_value());
        skip_whitespace();

        if (match(']')) break;
        expect(',', "array separator");
        skip_whitespace();
    }

    return JsonValue(arr);
}

JsonValue JsonParser::parse_object() {
    // TEACHING NOTE: A JSON object is { "key": value, "key": value, ... }
    // Keys must be strings. Values can be any JSON type.
    // We use a std::map which keeps keys sorted (not insertion order).
    // Real JSON objects preserve insertion order, but for config files,
    // sorted order is fine. A production parser would use an ordered map.
    expect('{', "object start");

    JsonObject obj;
    skip_whitespace();

    if (match('}')) {
        return JsonValue(obj);  // Empty object
    }

    while (true) {
        skip_whitespace();
        std::string key = parse_string_body();
        skip_whitespace();
        expect(':', "object key-value separator");
        obj[key] = parse_value();
        skip_whitespace();

        if (match('}')) break;
        expect(',', "object separator");
    }

    return JsonValue(obj);
}

} // namespace chinstrap