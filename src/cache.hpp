// cache.hpp - HTTP cache from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: What is HTTP caching and why does it matter?
//
// HTTP caching is one of the most important performance optimizations for web
// browsers. When a user visits a website, the browser downloads HTML, CSS,
// JavaScript, images, fonts, and other resources. Without caching, every
// page load would download all resources from scratch, causing significant
// latency and bandwidth usage.
//
// With caching, the browser stores responses locally and can reuse them for
// subsequent requests without contacting the server. This dramatically reduces
// load times and bandwidth. Studies show that cached content can make page
// loads 2-10x faster.
//
// The HTTP/1.1 caching model (RFC 7234) defines:
//
// 1. Cache-Control header: The primary caching directive
//    - max-age=<seconds>: how long the response is fresh
//    - no-cache: must revalidate before using a cached copy
//    - no-store: do not cache at all
//    - public: can be cached by shared caches (CDNs, proxies)
//    - private: only the browser can cache (not shared caches)
//    - must-revalidate: must revalidate when stale (never serve stale)
//    - stale-while-revalidate: serve stale while revalidating in background
//
// 2. Expires header: An absolute date after which the response is stale
//    (Cache-Control max-age takes precedence over Expires)
//
// 3. Validation headers: Allow conditional requests to check freshness
//    - ETag: An opaque string identifying the resource version
//    - Last-Modified: The date the resource was last modified
//
// 4. Conditional request headers:
//    - If-None-Match: Send the ETag, server returns 304 if unchanged
//    - If-Modified-Since: Send the date, server returns 304 if unchanged
//
// Cache key: The browser uses the request URL as the cache key. Different
// methods (GET, HEAD) are cached separately. Vary header specifies additional
// request headers that affect the cached response (e.g., Vary: Accept-Encoding
// means the cache must store separate responses for gzip vs identity).
//
// TEACHING NOTE: How Chrome implements caching
//
// Chrome uses a disk cache (~/.cache/google-chrome/Default/Cache/) backed by
// a custom block-file format. It stores cached entries with metadata including:
// - Request URL
// - Response headers
// - Response body
// - Creation and expiration times
// - Validation data (ETag, Last-Modified)
// - Vary header information
//
// Chrome also uses an in-memory cache for recently accessed resources
// (especially images and decoded data). The memory cache is faster but
// limited in size (typically a few megabytes).
//
// Cache eviction: When the disk cache is full, Chrome uses an LRU (Least
// Recently Used) eviction policy. Entries that have not been accessed for
// the longest time are evicted first.
//
// Our implementation stores cached responses as files in ~/.cache/chinstrap/.
// Each cache entry is a file containing the cached response headers and body.
// The filename is derived from a hash of the URL, providing O(1) lookup.

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <cstdint>

namespace chinstrap {

// TEACHING NOTE: Cache-Control directives
//
// Cache-Control is the primary caching header. It can appear in both
// requests and responses with different meanings:
//
// Response Cache-Control:
//   max-age=<seconds>: The response is fresh for this many seconds
//   s-maxage=<seconds>: Fresh for shared caches (overrides max-age for proxies)
//   no-cache: Can cache, but must revalidate before using
//   no-store: Must not cache at all
//   private: Only the end user browser can cache
//   public: Shared caches can also cache
//   must-revalidate: Must revalidate when stale (never serve stale)
//   proxy-revalidate: Like must-revalidate but only for shared caches
//   stale-while-revalidate: Serve stale while revalidating
//   stale-if-error: Serve stale if revalidation fails
//
// Request Cache-Control:
//   max-age=<seconds>: Do not accept cached responses older than this
//   min-fresh=<seconds>: Cached response must have at least this much freshness
//   no-cache: Do not use cached response without revalidation
//   no-store: Do not cache the response
//   only-if-cached: Only return cached responses (no network)

struct CacheControl {
    bool no_store = false;
    bool no_cache = false;
    bool must_revalidate = false;
    bool is_public = false;
    bool is_private = false;
    int64_t max_age = -1;       // -1 means not specified
    int64_t s_maxage = -1;      // -1 means not specified
    int64_t stale_while_revalidate = -1;

    // Check if this Cache-Control allows caching at all
    bool is_cacheable() const {
        return !no_store;
    }

    // Check if the response is fresh at the given time
    bool is_fresh(std::chrono::system_clock::time_point stored_time,
                  std::chrono::system_clock::time_point now) const {
        if (max_age < 0) return false;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - stored_time).count();
        return age < max_age;
    }
};

// A cached HTTP response entry
struct CacheEntry {
    std::string url;                          // The request URL (cache key)
    std::string method;                       // HTTP method (GET, HEAD)
    int status_code = 0;                       // HTTP status code (200, 304, etc.)
    std::vector<std::pair<std::string, std::string>> headers;  // Response headers
    std::vector<uint8_t> body;                // Response body
    std::chrono::system_clock::time_point stored_time;  // When the response was cached
    std::chrono::system_clock::time_point expires;     // When the response expires
    std::string etag;                         // ETag for validation
    std::string last_modified;                // Last-Modified for validation
    CacheControl cache_control;               // Parsed Cache-Control
    std::vector<std::string> vary_headers;    // Vary header values
    bool needs_revalidation = false;          // True if must revalidate before use

    // Check if the entry is fresh at the given time
    bool is_fresh() const;

    // Check if the entry can be served without revalidation
    bool can_serve() const;

    // Build conditional request headers for revalidation
    std::vector<std::pair<std::string, std::string>> build_conditional_headers() const;
};

class HttpCache {
public:
    HttpCache();
    ~HttpCache();

    // Set the cache directory (e.g., "~/.cache/chinstrap/")
    void set_cache_dir(const std::string& dir);

    // TEACHING NOTE: Cache lookup
    //
    // When the browser needs a resource, it first checks the cache.
    // If the cached entry is fresh, it is returned immediately (cache hit).
    // If the entry is stale, the browser sends a conditional request
    // (with If-None-Match or If-Modified-Since) to revalidate.
    // If the server returns 304 Not Modified, the cached body is used
    // with updated headers. If the server returns 200, the new response
    // replaces the cached entry.

    // Look up a URL in the cache. Returns the cached entry if it exists.
    std::optional<CacheEntry> lookup(const std::string& url, const std::string& method = "GET");

    // Store a response in the cache
    void store(const std::string& url, const std::string& method,
               int status_code,
               const std::vector<std::pair<std::string, std::string>>& headers,
               const std::vector<uint8_t>& body);

    // Remove an entry from the cache
    void invalidate(const std::string& url);

    // Clear the entire cache
    void clear();

    // Get cache size in bytes
    size_t size() const;

    // Clean up expired entries
    void cleanup_expired();

    // Public static methods for testing and external access
    static CacheControl parse_cache_control(const std::string& header_value);
    static std::optional<std::string> get_header(
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& name);

private:
    std::string cache_dir_;

    // In-memory index of cache entries (URL -> filename)
    std::unordered_map<std::string, std::string> index_;

    // TEACHING NOTE: Cache key hashing
    //
    // We use a simple hash of the URL as the cache filename. This avoids
    // issues with special characters in URLs and keeps filenames short.
    // We use FNV-1a hash (a simple, fast, well-distributed hash function).
    // A production implementation might use SHA-256 for collision resistance.
    static std::string url_to_filename(const std::string& url);

    // Parse Expires header value
    static std::chrono::system_clock::time_point parse_expires(const std::string& header_value);

    // Parse HTTP date
    static std::chrono::system_clock::time_point parse_http_date(const std::string& date_str);

    // Serialize/deserialize a cache entry to/from a file
    void write_entry(const std::string& filepath, const CacheEntry& entry) const;
    std::optional<CacheEntry> read_entry(const std::string& filepath) const;

    // Load the cache index from disk
    void load_index();

    // Compute cache file path for a URL
    std::string get_cache_path(const std::string& url) const;
};

} // namespace chinstrap