// =========================================================================
// test_url.cpp - Unit tests for URL parser
// =========================================================================
// TEACHING NOTE: These tests exercise the URL parser. We test:
//   - Basic URL parsing (scheme, host, port, path, query, fragment)
//   - Default ports (80 for http, 443 for https)
//   - URL reconstruction (to_string)
//   - Percent encoding/decoding
//   - Relative URL resolution
//   - Edge cases (empty paths, IPv6, userinfo)
//
// We use our own test framework (test_framework.hpp) with simple macros.
// =========================================================================

#include "test_framework.hpp"
#include "url.hpp"

using namespace chinstrap;

// --- Basic parsing ---

TEST(parse_simple_url) {
    Url url("http://example.com/path");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.scheme(), "http");
    ASSERT_STREQ(url.host(), "example.com");
    ASSERT_EQ(url.port(), 80);  // Default for http
    ASSERT_STREQ(url.path(), "/path");
    ASSERT_TRUE(url.query().empty());
    ASSERT_TRUE(url.fragment().empty());
}

TEST(parse_url_with_port) {
    Url url("http://example.com:8080/path");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.scheme(), "http");
    ASSERT_STREQ(url.host(), "example.com");
    ASSERT_TRUE(url.has_port());
    ASSERT_EQ(url.port(), 8080);
    ASSERT_STREQ(url.path(), "/path");
}

TEST(parse_url_with_query) {
    Url url("http://example.com/search?q=hello&page=2");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.path(), "/search");
    ASSERT_STREQ(url.query(), "q=hello&page=2");
}

TEST(parse_url_with_fragment) {
    Url url("http://example.com/page#section1");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.path(), "/page");
    ASSERT_STREQ(url.fragment(), "section1");
}

TEST(parse_url_with_query_and_fragment) {
    Url url("http://example.com/search?q=test#results");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.query(), "q=test");
    ASSERT_STREQ(url.fragment(), "results");
}

TEST(parse_https_url) {
    Url url("https://secure.example.com/login");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.scheme(), "https");
    ASSERT_EQ(url.port(), 443);  // Default for https
}

TEST(parse_url_empty_path_defaults_to_root) {
    Url url("http://example.com");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.path(), "/");
}

// --- Scheme case insensitivity ---

TEST(parse_url_scheme_case_insensitive) {
    Url url("HTTP://Example.COM/Path");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.scheme(), "http");
    ASSERT_STREQ(url.host(), "example.com");
}

// --- to_string reconstruction ---

TEST(to_string_basic) {
    Url url("http://example.com/path");
    ASSERT_STREQ(url.to_string(), "http://example.com/path");
}

TEST(to_string_with_port) {
    Url url("http://example.com:8080/path");
    ASSERT_STREQ(url.to_string(), "http://example.com:8080/path");
}

TEST(to_string_omits_default_port) {
    Url url("http://example.com:80/path");
    ASSERT_STREQ(url.to_string(), "http://example.com/path");
}

TEST(to_string_with_query_and_fragment) {
    Url url("http://example.com/search?q=test#results");
    ASSERT_STREQ(url.to_string(), "http://example.com/search?q=test#results");
}

// --- Percent encoding/decoding ---

TEST(percent_encode_space) {
    ASSERT_STREQ(Url::percent_encode("hello world"), "hello%20world");
}

TEST(percent_encode_special_chars) {
    ASSERT_STREQ(Url::percent_encode("a+b=c"), "a%2Bb%3Dc");
}

TEST(percent_decode_space) {
    ASSERT_STREQ(Url::percent_decode("hello%20world"), "hello world");
}

TEST(percent_decode_special_chars) {
    ASSERT_STREQ(Url::percent_decode("a%2Bb%3Dc"), "a+b=c");
}

TEST(percent_encode_decode_roundtrip) {
    std::string original = "hello world & special chars <>\"";
    std::string encoded = Url::percent_encode(original);
    std::string decoded = Url::percent_decode(encoded);
    ASSERT_STREQ(decoded, original);
}

// --- Relative URL resolution ---

TEST(resolve_absolute_path) {
    Url base("http://example.com/foo/bar");
    Url resolved = base.resolve("/baz");
    ASSERT_STREQ(resolved.to_string(), "http://example.com/baz");
}

TEST(resolve_relative_path) {
    Url base("http://example.com/foo/bar");
    Url resolved = base.resolve("baz");
    ASSERT_STREQ(resolved.to_string(), "http://example.com/foo/baz");
}

TEST(resolve_query_only) {
    Url base("http://example.com/page");
    Url resolved = base.resolve("?q=test");
    ASSERT_STREQ(resolved.to_string(), "http://example.com/page?q=test");
}

TEST(resolve_fragment_only) {
    Url base("http://example.com/page");
    Url resolved = base.resolve("#section");
    ASSERT_STREQ(resolved.to_string(), "http://example.com/page#section");
}

TEST(resolve_absolute_url) {
    Url base("http://example.com/page");
    Url resolved = base.resolve("http://other.com/path");
    ASSERT_STREQ(resolved.to_string(), "http://other.com/path");
}

// --- Edge cases ---

TEST(parse_invalid_empty_url) {
    Url url("");
    ASSERT_FALSE(url.is_valid());
}

TEST(parse_url_with_userinfo) {
    Url url("http://user:pass@example.com/path");
    ASSERT_TRUE(url.is_valid());
    ASSERT_STREQ(url.userinfo(), "user:pass");
    ASSERT_STREQ(url.host(), "example.com");
}

RUN_TESTS()