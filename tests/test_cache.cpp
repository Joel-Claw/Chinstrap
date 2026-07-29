// test_cache.cpp - Unit tests for HTTP cache logic
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Testing cache logic
//
// Cache testing involves:
// 1. Parsing Cache-Control headers
// 2. Determining cacheability
// 3. Checking freshness (max-age, Expires)
// 4. Building conditional request headers
// 5. Cache storage and retrieval
//
// These tests verify the cache logic without performing actual HTTP requests.

#include "../src/cache.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <chrono>
#include <unistd.h>
#include <sys/stat.h>   // for rmdir()

using namespace chinstrap;

// Test Cache-Control parsing
static void test_parse_cache_control() {
    printf("test_parse_cache_control... ");

    auto cc = HttpCache::parse_cache_control("max-age=3600, public");
    assert(cc.max_age == 3600);
    assert(cc.is_public);
    assert(!cc.is_private);
    assert(!cc.no_store);
    assert(!cc.no_cache);

    cc = HttpCache::parse_cache_control("no-store");
    assert(cc.no_store);
    assert(!cc.is_cacheable());

    cc = HttpCache::parse_cache_control("no-cache, must-revalidate");
    assert(cc.no_cache);
    assert(cc.must_revalidate);

    cc = HttpCache::parse_cache_control("private, max-age=0");
    assert(cc.is_private);
    assert(cc.max_age == 0);

    cc = HttpCache::parse_cache_control("stale-while-revalidate=600, max-age=300");
    assert(cc.max_age == 300);
    assert(cc.stale_while_revalidate == 600);

    printf("OK\n");
}

// Test cache entry freshness
static void test_freshness() {
    printf("test_freshness... ");

    CacheEntry entry;
    entry.stored_time = std::chrono::system_clock::now();

    // With max-age
    entry.cache_control.max_age = 3600;
    assert(entry.is_fresh());

    // Set stored time to 2 hours ago - should be stale
    entry.stored_time = std::chrono::system_clock::now() - std::chrono::hours(2);
    assert(!entry.is_fresh());

    // With max-age = 0 - always stale
    entry.stored_time = std::chrono::system_clock::now();
    entry.cache_control.max_age = 0;
    assert(!entry.is_fresh());

    printf("OK\n");
}

// Test can_serve logic
static void test_can_serve() {
    printf("test_can_serve... ");

    CacheEntry entry;
    entry.stored_time = std::chrono::system_clock::now();
    entry.cache_control.max_age = 3600;

    // Fresh and no revalidation needed - can serve
    assert(entry.can_serve());

    // no-cache requires revalidation
    entry.cache_control.no_cache = true;
    assert(!entry.can_serve());

    // must-revalidate requires revalidation
    entry.cache_control.no_cache = false;
    entry.cache_control.must_revalidate = true;
    assert(!entry.can_serve());

    // Stale entry cannot be served
    entry.cache_control.must_revalidate = false;
    entry.stored_time = std::chrono::system_clock::now() - std::chrono::hours(2);
    assert(!entry.can_serve());

    printf("OK\n");
}

// Test conditional request headers
static void test_conditional_headers() {
    printf("test_conditional_headers... ");

    CacheEntry entry;

    // With ETag only
    entry.etag = "\"abc123\"";
    auto headers = entry.build_conditional_headers();
    assert(headers.size() == 1);
    assert(headers[0].first == "If-None-Match");
    assert(headers[0].second == "\"abc123\"");

    // With Last-Modified only
    entry.etag = "";
    entry.last_modified = "Wed, 21 Oct 2025 07:28:00 GMT";
    headers = entry.build_conditional_headers();
    assert(headers.size() == 1);
    assert(headers[0].first == "If-Modified-Since");
    assert(headers[0].second == "Wed, 21 Oct 2025 07:28:00 GMT");

    // With both ETag and Last-Modified
    entry.etag = "\"abc123\"";
    headers = entry.build_conditional_headers();
    assert(headers.size() == 2);
    assert(headers[0].first == "If-None-Match");
    assert(headers[1].first == "If-Modified-Since");

    // With neither
    entry.etag = "";
    entry.last_modified = "";
    headers = entry.build_conditional_headers();
    assert(headers.empty());

    printf("OK\n");
}

// Test cacheability of different status codes
static void test_cacheability() {
    printf("test_cacheability... ");

    // We test this by storing responses and checking if they are cached
    std::string tmp_dir = "/tmp/chinstrap_test_cache_XXXXXX";
    // Create a temp directory
    char tmpl[] = "/tmp/chinstrap_test_XXXXXX";
    mkdtemp(tmpl);
    tmp_dir = tmpl;

    HttpCache cache;
    cache.set_cache_dir(tmp_dir);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Cache-Control", "max-age=3600");

    // 200 OK - cacheable
    cache.store("https://example.com/ok", "GET", 200, headers, {0x48, 0x49});
    auto entry = cache.lookup("https://example.com/ok");
    assert(entry.has_value());
    assert(entry->status_code == 200);

    // 404 Not Found - cacheable
    cache.store("https://example.com/notfound", "GET", 404, headers, {});;
    entry = cache.lookup("https://example.com/notfound");
    assert(entry.has_value());

    // 500 Internal Server Error - not cacheable
    cache.store("https://example.com/error", "GET", 500, headers, {});
    entry = cache.lookup("https://example.com/error");
    assert(!entry.has_value());

    // POST - not cacheable
    cache.store("https://example.com/submit", "POST", 200, headers, {});
    entry = cache.lookup("https://example.com/submit");
    assert(!entry.has_value());

    // no-store - not cacheable
    std::vector<std::pair<std::string, std::string>> no_store_headers;
    no_store_headers.emplace_back("Cache-Control", "no-store");
    cache.store("https://example.com/secret", "GET", 200, no_store_headers, {});
    entry = cache.lookup("https://example.com/secret");
    assert(!entry.has_value());

    // Cleanup
    cache.clear();
    rmdir(tmp_dir.c_str());

    printf("OK\n");
}

// Test cache storage and retrieval
static void test_store_and_retrieve() {
    printf("test_store_and_retrieve... ");

    char tmpl[] = "/tmp/chinstrap_cache_XXXXXX";
    mkdtemp(tmpl);
    std::string tmp_dir = tmpl;

    HttpCache cache;
    cache.set_cache_dir(tmp_dir);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Cache-Control", "max-age=3600");
    headers.emplace_back("Content-Type", "text/html");
    headers.emplace_back("ETag", "\"v1\"");

    std::vector<uint8_t> body = {'<', 'h', 't', 'm', 'l', '>'};

    cache.store("https://example.com/page", "GET", 200, headers, body);

    auto entry = cache.lookup("https://example.com/page");
    assert(entry.has_value());
    assert(entry->url == "https://example.com/page");
    assert(entry->method == "GET");
    assert(entry->status_code == 200);
    assert(entry->body == body);
    assert(entry->etag == "\"v1\"");

    // Check that headers are preserved
    bool found_content_type = false;
    bool found_cache_control = false;
    for (const auto& [name, value] : entry->headers) {
        if (name == "Content-Type" && value == "text/html") found_content_type = true;
        if (name == "Cache-Control" && value == "max-age=3600") found_cache_control = true;
    }
    assert(found_content_type);
    assert(found_cache_control);

    // Cleanup
    cache.clear();
    rmdir(tmp_dir.c_str());

    printf("OK\n");
}

// Test cache invalidation
static void test_invalidation() {
    printf("test_invalidation... ");

    char tmpl[] = "/tmp/chinstrap_inval_XXXXXX";
    mkdtemp(tmpl);
    std::string tmp_dir = tmpl;

    HttpCache cache;
    cache.set_cache_dir(tmp_dir);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Cache-Control", "max-age=3600");

    cache.store("https://example.com/page1", "GET", 200, headers, {1, 2, 3});
    cache.store("https://example.com/page2", "GET", 200, headers, {4, 5, 6});

    // Both should be in cache
    assert(cache.lookup("https://example.com/page1").has_value());
    assert(cache.lookup("https://example.com/page2").has_value());

    // Invalidate page1
    cache.invalidate("https://example.com/page1");

    assert(!cache.lookup("https://example.com/page1").has_value());
    assert(cache.lookup("https://example.com/page2").has_value());

    // Clear all
    cache.clear();
    assert(!cache.lookup("https://example.com/page2").has_value());

    rmdir(tmp_dir.c_str());

    printf("OK\n");
}

// Test header lookup (case-insensitive)
static void test_header_lookup() {
    printf("test_header_lookup... ");

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "text/html");
    headers.emplace_back("cache-control", "max-age=60");
    headers.emplace_back("ETAG", "\"xyz\"");

    // Case-insensitive lookup
    auto val = HttpCache::get_header(headers, "content-type");
    assert(val.has_value());
    assert(*val == "text/html");

    val = HttpCache::get_header(headers, "Cache-Control");
    assert(val.has_value());
    assert(*val == "max-age=60");

    val = HttpCache::get_header(headers, "etag");
    assert(val.has_value());
    assert(*val == "\"xyz\"");

    val = HttpCache::get_header(headers, "nonexistent");
    assert(!val.has_value());

    printf("OK\n");
}

// Test URL to filename hashing
static void test_url_hashing() {
    printf("test_url_hashing... ");

    // The url_to_filename function is private, but we can test it indirectly
    // by storing two different URLs and verifying they get different cache files.

    char tmpl[] = "/tmp/chinstrap_hash_XXXXXX";
    mkdtemp(tmpl);
    std::string tmp_dir = tmpl;

    HttpCache cache;
    cache.set_cache_dir(tmp_dir);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Cache-Control", "max-age=3600");

    cache.store("https://example.com/page1", "GET", 200, headers, {1});
    cache.store("https://example.com/page2", "GET", 200, headers, {2});

    // Both should be retrievable separately
    auto e1 = cache.lookup("https://example.com/page1");
    auto e2 = cache.lookup("https://example.com/page2");
    assert(e1.has_value());
    assert(e2.has_value());
    assert(e1->body[0] == 1);
    assert(e2->body[0] == 2);

    cache.clear();
    rmdir(tmp_dir.c_str());

    printf("OK\n");
}

// Test Vary: * prevents caching
static void test_vary_star() {
    printf("test_vary_star... ");

    char tmpl[] = "/tmp/chinstrap_vary_XXXXXX";
    mkdtemp(tmpl);
    std::string tmp_dir = tmpl;

    HttpCache cache;
    cache.set_cache_dir(tmp_dir);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Cache-Control", "max-age=3600");
    headers.emplace_back("Vary", "*");

    cache.store("https://example.com/vary-star", "GET", 200, headers, {});

    // Should NOT be cached due to Vary: *
    auto entry = cache.lookup("https://example.com/vary-star");
    assert(!entry.has_value());

    cache.clear();
    rmdir(tmp_dir.c_str());

    printf("OK\n");
}

int main() {
    printf("=== Cache Tests ===\n\n");

    test_parse_cache_control();
    test_freshness();
    test_can_serve();
    test_conditional_headers();
    test_cacheability();
    test_store_and_retrieve();
    test_invalidation();
    test_header_lookup();
    test_url_hashing();
    test_vary_star();

    printf("\nAll cache tests passed!\n");
    return 0;
}