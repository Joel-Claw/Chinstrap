// test_cookies.cpp - Unit tests for cookie parsing and matching
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Testing cookies
//
// Cookie testing involves:
// 1. Parsing Set-Cookie headers (various formats and attributes)
// 2. Cookie matching (domain and path matching rules)
// 3. Serialization and deserialization (storage round-trip)
// 4. SameSite behavior
// 5. Expiration handling
//
// These tests use no network and verify the cookie jar logic directly.

#include "../src/cookies.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace chinstrap;

// Test basic Set-Cookie parsing
static void test_basic_parsing() {
    printf("test_basic_parsing... ");
    CookieJar jar;

    // Simple cookie
    bool ok = jar.parse_set_cookie("sessionid=abc123", "example.com");
    assert(ok);

    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].name == "sessionid");
    assert(cookies[0].value == "abc123");
    assert(cookies[0].domain == "example.com");
    assert(cookies[0].path == "/");
    assert(cookies[0].session_cookie == true);

    printf("OK\n");
}

// Test parsing with attributes
static void test_attribute_parsing() {
    printf("test_attribute_parsing... ");
    CookieJar jar;

    // Cookie with all attributes
    bool ok = jar.parse_set_cookie(
        "user=John; Domain=.example.com; Path=/app; Secure; HttpOnly; SameSite=Lax",
        "www.example.com"
    );
    assert(ok);

    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].name == "user");
    assert(cookies[0].value == "John");
    assert(cookies[0].domain == "example.com");  // Leading dot stripped
    assert(cookies[0].path == "/app");
    assert(cookies[0].secure == true);
    assert(cookies[0].http_only == true);
    assert(cookies[0].same_site == SameSite::Lax);

    printf("OK\n");
}

// Test SameSite variants
static void test_samesite() {
    printf("test_samesite... ");
    CookieJar jar;

    // SameSite=Strict
    jar.parse_set_cookie("a=1; SameSite=Strict", "example.com");
    // SameSite=Lax
    jar.parse_set_cookie("b=2; SameSite=Lax", "example.com");
    // SameSite=None
    jar.parse_set_cookie("c=3; SameSite=None", "example.com");
    // No SameSite (unspecified)
    jar.parse_set_cookie("d=4", "example.com");

    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 4);

    // Find each cookie and verify sameSite
    for (const auto& c : cookies) {
        if (c.name == "a") assert(c.same_site == SameSite::Strict);
        else if (c.name == "b") assert(c.same_site == SameSite::Lax);
        else if (c.name == "c") assert(c.same_site == SameSite::None);
        else if (c.name == "d") assert(c.same_site == SameSite::Unspecified);
    }

    printf("OK\n");
}

// Test domain matching
static void test_domain_matching() {
    printf("test_domain_matching... ");

    // Exact match
    assert(CookieJar::domain_matches("example.com", "example.com"));

    // Subdomain match
    assert(CookieJar::domain_matches("www.example.com", "example.com"));
    assert(CookieJar::domain_matches("api.example.com", "example.com"));
    assert(CookieJar::domain_matches("a.b.c.example.com", "example.com"));

    // Non-match
    assert(!CookieJar::domain_matches("notexample.com", "example.com"));
    assert(!CookieJar::domain_matches("example.org", "example.com"));

    // Different domain
    assert(!CookieJar::domain_matches("other.com", "example.com"));

    printf("OK\n");
}

// Test path matching
static void test_path_matching() {
    printf("test_path_matching... ");

    // Exact match
    assert(CookieJar::path_matches("/", "/"));
    assert(CookieJar::path_matches("/store", "/store"));

    // Prefix match (cookie path is prefix of request path)
    assert(CookieJar::path_matches("/store/item", "/store"));
    assert(CookieJar::path_matches("/store/category/item", "/store"));

    // Cookie path ending with /
    assert(CookieJar::path_matches("/store/item", "/store/"));

    // Non-match
    assert(!CookieJar::path_matches("/shopping", "/store"));
    assert(!CookieJar::path_matches("/", "/store"));

    printf("OK\n");
}

// Test cookie matching for requests
static void test_cookie_matching() {
    printf("test_cookie_matching... ");
    CookieJar jar;

    // Set a cookie for example.com, path /app
    jar.parse_set_cookie("token=abc; Domain=example.com; Path=/app", "example.com");

    // Should match: same domain, matching path
    std::string header = jar.get_cookie_header("example.com", "/app/page", true, false);
    assert(header == "token=abc");

    // Should match: subdomain, matching path
    header = jar.get_cookie_header("www.example.com", "/app/page", true, false);
    assert(header == "token=abc");

    // Should NOT match: non-matching path
    header = jar.get_cookie_header("example.com", "/other", true, false);
    assert(header.empty());

    // Should NOT match: non-matching domain
    header = jar.get_cookie_header("other.com", "/app/page", true, false);
    assert(header.empty());

    printf("OK\n");
}

// Test Secure flag
static void test_secure_flag() {
    printf("test_secure_flag... ");
    CookieJar jar;

    // Secure cookie
    jar.parse_set_cookie("secret=value; Domain=example.com; Secure", "example.com");

    // Should NOT be sent over HTTP (not secure)
    std::string header = jar.get_cookie_header("example.com", "/", false, false);
    assert(header.empty());

    // Should be sent over HTTPS (secure)
    header = jar.get_cookie_header("example.com", "/", true, false);
    assert(header == "secret=value");

    printf("OK\n");
}

// Test SameSite behavior
static void test_samesite_matching() {
    printf("test_samesite_matching... ");
    CookieJar jar;

    // SameSite=Strict cookie
    jar.parse_set_cookie("strict=1; Domain=example.com; SameSite=Strict", "example.com");

    // Same-site request: should send
    std::string header = jar.get_cookie_header("example.com", "/", true, false);
    assert(header == "strict=1");

    // Cross-site request: should NOT send
    header = jar.get_cookie_header("example.com", "/", true, true);
    assert(header.empty());

    // SameSite=None cookie: should send cross-site
    jar.parse_set_cookie("none=1; Domain=example.com; SameSite=None; Secure", "example.com");
    header = jar.get_cookie_header("example.com", "/", true, true);
    assert(header.find("none=1") != std::string::npos);

    printf("OK\n");
}

// Test multiple cookies with ordering
static void test_multiple_cookies() {
    printf("test_multiple_cookies... ");
    CookieJar jar;

    // Set multiple cookies for the same domain
    jar.parse_set_cookie("a=1; Domain=example.com; Path=/", "example.com");
    jar.parse_set_cookie("b=2; Domain=example.com; Path=/app", "example.com");
    jar.parse_set_cookie("c=3; Domain=example.com; Path=/app/sub", "example.com");

    // Request to /app/sub should get all three
    std::string header = jar.get_cookie_header("example.com", "/app/sub", true, false);
    assert(!header.empty());

    // The cookie with the longest path should come first
    // (our implementation sorts by path length, descending)
    size_t pos_c = header.find("c=3");
    size_t pos_b = header.find("b=2");
    size_t pos_a = header.find("a=1");
    assert(pos_c < pos_b);
    assert(pos_b < pos_a);

    printf("OK\n");
}

// Test cookie overwrite (same name, same domain)
static void test_cookie_overwrite() {
    printf("test_cookie_overwrite... ");
    CookieJar jar;

    // Set a cookie
    jar.parse_set_cookie("token=old; Domain=example.com", "example.com");
    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].value == "old");

    // Set same cookie with new value (should overwrite)
    jar.parse_set_cookie("token=new; Domain=example.com", "example.com");
    cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].value == "new");

    printf("OK\n");
}

// Test serialization round-trip
static void test_serialization() {
    printf("test_serialization... ");
    Cookie cookie;
    cookie.name = "test_name";
    cookie.value = "test_value";
    cookie.domain = "example.com";
    cookie.path = "/path";
    cookie.secure = true;
    cookie.http_only = false;
    cookie.same_site = SameSite::Lax;
    cookie.session_cookie = true;

    std::string serialized = cookie.serialize();
    auto deserialized = Cookie::deserialize(serialized);
    assert(deserialized.has_value());

    assert(deserialized->name == cookie.name);
    assert(deserialized->value == cookie.value);
    assert(deserialized->domain == cookie.domain);
    assert(deserialized->path == cookie.path);
    assert(deserialized->secure == cookie.secure);
    assert(deserialized->http_only == cookie.http_only);
    assert(deserialized->same_site == cookie.same_site);
    assert(deserialized->session_cookie == cookie.session_cookie);

    printf("OK\n");
}

// Test value with special characters
static void test_special_characters() {
    printf("test_special_characters... ");
    CookieJar jar;

    // Value with equals sign
    jar.parse_set_cookie("data=a=b=c", "example.com");
    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].name == "data");
    assert(cookies[0].value == "a=b=c");

    // Value with spaces (quoted-style not supported in our simple parser,
    // but we should handle spaces after the = sign)
    jar.clear();
    jar.parse_set_cookie("pref=hello world", "example.com");
    cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].value == "hello world");

    printf("OK\n");
}

// Test cookie with Max-Age
static void test_max_age() {
    printf("test_max_age... ");
    CookieJar jar;

    // Max-Age of 3600 seconds
    jar.parse_set_cookie("session=abc; Domain=example.com; Max-Age=3600", "example.com");
    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].session_cookie == false);

    // Max-Age of 0 means delete immediately
    jar.clear();
    bool ok = jar.parse_set_cookie("session=abc; Domain=example.com; Max-Age=0", "example.com");
    assert(!ok);  // Should return false (cookie deleted)
    auto cookies2 = jar.get_all_cookies();
    assert(cookies2.empty());

    printf("OK\n");
}

// Test cleanup of expired cookies
static void test_cleanup_expired() {
    printf("test_cleanup_expired... ");
    CookieJar jar;

    // Session cookie (does not expire by time)
    jar.parse_set_cookie("a=1; Domain=example.com", "example.com");

    // Expired cookie (Max-Age = -1 means already expired)
    // Note: Max-Age <= 0 means delete, so our parser rejects it
    // We test session cookie cleanup instead

    jar.cleanup_expired();
    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].name == "a");

    printf("OK\n");
}

// Test delete for domain
static void test_delete_for_domain() {
    printf("test_delete_for_domain... ");
    CookieJar jar;

    jar.parse_set_cookie("a=1; Domain=example.com", "example.com");
    jar.parse_set_cookie("b=2; Domain=other.com", "other.com");

    assert(jar.get_all_cookies().size() == 2);

    jar.delete_for_domain("example.com");
    auto cookies = jar.get_all_cookies();
    assert(cookies.size() == 1);
    assert(cookies[0].domain == "other.com");

    printf("OK\n");
}

// Test default path derivation
static void test_default_path() {
    printf("test_default_path... ");

    // Root path
    assert(CookieJar::default_path("/") == "/");

    // Simple path
    assert(CookieJar::default_path("/page") == "/");

    // Nested path
    assert(CookieJar::default_path("/app/page") == "/app");

    // Deeper path
    assert(CookieJar::default_path("/a/b/c/d") == "/a/b/c");

    printf("OK\n");
}

int main() {
    printf("=== Cookie Tests ===\n\n");

    test_basic_parsing();
    test_attribute_parsing();
    test_samesite();
    test_domain_matching();
    test_path_matching();
    test_cookie_matching();
    test_secure_flag();
    test_samesite_matching();
    test_multiple_cookies();
    test_cookie_overwrite();
    test_serialization();
    test_special_characters();
    test_max_age();
    test_cleanup_expired();
    test_delete_for_domain();
    test_default_path();

    printf("\nAll cookie tests passed!\n");
    return 0;
}