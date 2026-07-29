// cache.cpp - HTTP cache implementation from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Cache implementation overview
//
// Our HTTP cache stores responses as files on disk. Each cached response
// is stored as a file containing:
// 1. A header with metadata (URL, method, status code, stored time, expires)
// 2. The response headers
// 3. The response body
//
// The filename is derived from a hash of the URL. We use FNV-1a, a simple
// and fast hash function, to convert the URL into a hex string.
//
// On startup, we scan the cache directory and build an in-memory index
// (URL -> filename) for fast lookups. When a new response is cached, we
// write the file and update the index.
//
// Cache freshness model:
//
// When a request comes in:
// 1. Look up the URL in the index
// 2. Read the cached entry from disk
// 3. Check if the entry is fresh:
//    a. If Cache-Control max-age is set, check if age < max_age
//    b. If Expires is set, check if now < expires
//    c. If no freshness info, use heuristic (10% of time since Last-Modified)
// 4. If fresh, return the cached entry (cache hit - no network)
// 5. If stale:
//    a. Send a conditional request with If-None-Match (ETag) and
//       If-Modified-Since (Last-Modified)
//    b. If server returns 304 Not Modified, update headers and return cached body
//    c. If server returns 200, replace the cached entry with the new response
//    d. If no validator (no ETag/Last-Modified), do a normal request
//
// TEACHING NOTE: Why caching matters for performance
//
// The web is slow. A typical page load involves dozens of HTTP requests:
// HTML, CSS, JavaScript, images, fonts, API calls. Each request has
// network latency (RTT to the server, typically 20-200ms), transfer time,
// and server processing time. Caching eliminates all of these for cached
// resources.
//
// Example: A news website might have a 2MB CSS file that rarely changes.
// Without caching, every page load downloads 2MB. With caching, it is
// downloaded once and reused on subsequent visits, reducing load time from
// seconds to milliseconds.
//
// Chrome's cache is sophisticated:
// - Disk cache: stores responses persistently (survives browser restart)
// - Memory cache: stores recently used responses for fast access
// - Service Worker cache: allows web apps to control caching programmatically
// - HTTP cache: the standard HTTP caching we implement here
// - Push cache: temporary cache for HTTP/2 server push (per connection)
//
// Our implementation focuses on the disk-backed HTTP cache.

#include "cache.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <cctype>

namespace chinstrap {

// ============================================================================
// CacheEntry
// ============================================================================

bool CacheEntry::is_fresh() const {
    auto now = std::chrono::system_clock::now();

    // If max-age is set, use it
    if (cache_control.max_age >= 0) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - stored_time).count();
        return age < cache_control.max_age;
    }

    // If Expires is set, check it
    if (expires != std::chrono::system_clock::time_point{}) {
        return now < expires;
    }

    // No freshness info - stale by default
    return false;
}

// TEACHING NOTE: When can we serve a cached response?
//
// A cached response can be served without contacting the server if:
// 1. It is fresh (within its max-age or Expires window)
// 2. Cache-Control does not require revalidation (no no-cache or must-revalidate)
// 3. The request does not have Cache-Control: no-cache
//
// If the entry has no-cache or must-revalidate, it must be revalidated even
// if it is still within its freshness window. This allows servers to force
// revalidation by setting Cache-Control: no-cache (the response can be stored
// but must be checked before use).
//
// If the entry is stale but has a validator (ETag or Last-Modified), we can
// send a conditional request. If the server says 304, we can reuse the body.

bool CacheEntry::can_serve() const {
    if (needs_revalidation || cache_control.no_cache || cache_control.must_revalidate) {
        return false;
    }
    return is_fresh();
}

std::vector<std::pair<std::string, std::string>> CacheEntry::build_conditional_headers() const {
    // TEACHING NOTE: Conditional request headers
    //
    // When revalidating a cached response, we send the validators we have:
    // - If-None-Match: The ETag of the cached response. The server compares
    //   it with the current ETag. If they match, the resource has not changed
    //   and the server returns 304 Not Modified (no body).
    // - If-Modified-Since: The Last-Modified date. The server compares it
    //   with the current modification date.
    //
    // We send both if available (the server can use either). If-None-Match
    // takes precedence over If-Modified-Since per RFC 7232.
    //
    // The 304 response includes updated headers (e.g., a new Cache-Control
    // or Expires) which we merge into the cached entry.

    std::vector<std::pair<std::string, std::string>> headers;

    if (!etag.empty()) {
        headers.emplace_back("If-None-Match", etag);
    }

    if (!last_modified.empty()) {
        headers.emplace_back("If-Modified-Since", last_modified);
    }

    return headers;
}

// ============================================================================
// HttpCache
// ============================================================================

HttpCache::HttpCache() = default;
HttpCache::~HttpCache() = default;

void HttpCache::set_cache_dir(const std::string& dir) {
    cache_dir_ = dir;

    // Create the directory if it does not exist
    mkdir(cache_dir_.c_str(), 0755);

    load_index();
}

// TEACHING NOTE: Cache index loading
//
// On startup, we scan the cache directory for .cache files and build
// an index mapping URLs to filenames. This allows O(1) lookup without
// scanning the directory on every request.
//
// We store the URL in the cache file itself, so we read each file to
// extract the URL. Alternatively, we could maintain a separate index file
// (like Chrome's "Index" file), but reading all files on startup is
// simpler and the cache directory is typically small enough.

void HttpCache::load_index() {
    index_.clear();

    DIR* dir = opendir(cache_dir_.c_str());
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string filepath = cache_dir_ + "/" + name;
        auto cached = read_entry(filepath);
        if (cached) {
            index_[cached->url] = name;
        }
    }

    closedir(dir);
}

// TEACHING NOTE: FNV-1a hash for cache filenames
//
// FNV-1a (Fowler-Noll-Vo) is a simple, fast, non-cryptographic hash function
// with good distribution. We use the 64-bit variant and convert to hex.
// This gives us filenames like "a1b2c3d4e5f67890.cache".
//
// FNV-1a algorithm:
//   hash = 14695981039346656037 (FNV offset basis for 64-bit)
//   for each byte:
//     hash = hash XOR byte
//     hash = hash * 1099511628211 (FNV prime for 64-bit)
//
// Collisions are extremely unlikely for URLs in practice. A production
// implementation might use SHA-256 for guaranteed collision resistance.

std::string HttpCache::url_to_filename(const std::string& url) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : url) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }

    std::string result;
    result.reserve(20);
    const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        result += hex[(hash >> (i * 4)) & 0xF];
    }
    result += ".cache";
    return result;
}

std::string HttpCache::get_cache_path(const std::string& url) const {
    return cache_dir_ + "/" + url_to_filename(url);
}

// TEACHING NOTE: Parsing Cache-Control header
//
// The Cache-Control header value is a comma-separated list of directives:
//   Cache-Control: max-age=3600, public, no-cache
//
// We parse each directive (case-insensitive) and set the corresponding field
// in our CacheControl struct. Unknown directives are ignored.

CacheControl HttpCache::parse_cache_control(const std::string& header_value) {
    CacheControl cc;

    std::string lower;
    lower.reserve(header_value.size());
    for (char c : header_value) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::istringstream iss(lower);
    std::string directive;
    while (std::getline(iss, directive, ',')) {
        // Trim whitespace
        size_t s = directive.find_first_not_of(" \t");
        size_t e = directive.find_last_not_of(" \t");
        if (s == std::string::npos) continue;
        directive = directive.substr(s, e - s + 1);

        if (directive == "no-store") {
            cc.no_store = true;
        } else if (directive == "no-cache") {
            cc.no_cache = true;
        } else if (directive == "must-revalidate") {
            cc.must_revalidate = true;
        } else if (directive == "public") {
            cc.is_public = true;
        } else if (directive == "private") {
            cc.is_private = true;
        } else if (directive.substr(0, 8) == "max-age=") {
            try {
                cc.max_age = std::stoll(directive.substr(8));
            } catch (...) {}
        } else if (directive.substr(0, 8) == "s-maxage=") {
            try {
                cc.s_maxage = std::stoll(directive.substr(8));
            } catch (...) {}
        } else if (directive.substr(0, 23) == "stale-while-revalidate=") {
            try {
                cc.stale_while_revalidate = std::stoll(directive.substr(23));
            } catch (...) {}
        }
    }

    return cc;
}

std::chrono::system_clock::time_point HttpCache::parse_expires(const std::string& header_value) {
    // Parse HTTP date format
    struct tm tm_val{};
    const char* format = "%a, %d %b %Y %H:%M:%S GMT";
    if (strptime(header_value.c_str(), format, &tm_val) != nullptr) {
        time_t t = timegm(&tm_val);
        return std::chrono::system_clock::from_time_t(t);
    }
    // Invalid date - treat as already expired
    return std::chrono::system_clock::time_point{};
}

std::chrono::system_clock::time_point HttpCache::parse_http_date(const std::string& date_str) {
    struct tm tm_val{};
    const char* format = "%a, %d %b %Y %H:%M:%S GMT";
    if (strptime(date_str.c_str(), format, &tm_val) != nullptr) {
        time_t t = timegm(&tm_val);
        return std::chrono::system_clock::from_time_t(t);
    }
    return std::chrono::system_clock::time_point{};
}

std::optional<std::string> HttpCache::get_header(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& name
) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& [h_name, h_value] : headers) {
        std::string lower_h = h_name;
        std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_h == lower_name) {
            return h_value;
        }
    }

    return std::nullopt;
}

// TEACHING NOTE: Cache entry storage format
//
// We store cache entries as binary files with the following layout:
//
// [4 bytes magic: "CSTR"]
// [8 bytes stored_time (Unix timestamp)]
// [8 bytes expires (Unix timestamp)]
// [4 bytes URL length]
// [URL bytes]
// [4 bytes method length]
// [method bytes]
// [4 bytes status_code]
// [4 bytes num_headers]
// For each header:
//   [4 bytes name length] [name bytes]
//   [4 bytes value length] [value bytes]
// [4 bytes ETag length] [ETag bytes]
// [4 bytes Last-Modified length] [Last-Modified bytes]
// [4 bytes num_vary_headers]
// For each vary header:
//   [4 bytes length] [value bytes]
// [8 bytes body length]
// [body bytes]
//
// This format is simple, self-describing, and easy to parse. All integers
// are stored in network byte order (big-endian) for portability.
// We use 4-byte length prefixes for strings, which allows strings up to 4GB.
// The body can be up to 8GB (8-byte length prefix).

void HttpCache::write_entry(const std::string& filepath, const CacheEntry& entry) const {
    std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;

    // Helper to write big-endian values
    auto write_u32 = [&](uint32_t val) {
        file.put(static_cast<char>((val >> 24) & 0xFF));
        file.put(static_cast<char>((val >> 16) & 0xFF));
        file.put(static_cast<char>((val >> 8) & 0xFF));
        file.put(static_cast<char>(val & 0xFF));
    };
    auto write_u64 = [&](uint64_t val) {
        file.put(static_cast<char>((val >> 56) & 0xFF));
        file.put(static_cast<char>((val >> 48) & 0xFF));
        file.put(static_cast<char>((val >> 40) & 0xFF));
        file.put(static_cast<char>((val >> 32) & 0xFF));
        file.put(static_cast<char>((val >> 24) & 0xFF));
        file.put(static_cast<char>((val >> 16) & 0xFF));
        file.put(static_cast<char>((val >> 8) & 0xFF));
        file.put(static_cast<char>(val & 0xFF));
    };
    auto write_string = [&](const std::string& s) {
        write_u32(static_cast<uint32_t>(s.size()));
        file.write(s.data(), static_cast<std::streamsize>(s.size()));
    };

    // Magic
    file.write("CSTR", 4);

    // Timestamps
    auto stored_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        entry.stored_time.time_since_epoch()).count();
    auto expires_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        entry.expires.time_since_epoch()).count();
    write_u64(static_cast<uint64_t>(stored_epoch));
    write_u64(static_cast<uint64_t>(expires_epoch));

    // URL and method
    write_string(entry.url);
    write_string(entry.method);

    // Status code
    write_u32(static_cast<uint32_t>(entry.status_code));

    // Headers
    write_u32(static_cast<uint32_t>(entry.headers.size()));
    for (const auto& [name, value] : entry.headers) {
        write_string(name);
        write_string(value);
    }

    // Validators
    write_string(entry.etag);
    write_string(entry.last_modified);

    // Vary headers
    write_u32(static_cast<uint32_t>(entry.vary_headers.size()));
    for (const auto& v : entry.vary_headers) {
        write_string(v);
    }

    // Body
    write_u64(static_cast<uint64_t>(entry.body.size()));
    file.write(reinterpret_cast<const char*>(entry.body.data()),
               static_cast<std::streamsize>(entry.body.size()));
}

std::optional<CacheEntry> HttpCache::read_entry(const std::string& filepath) const {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    auto read_u32 = [&]() -> uint32_t {
        unsigned char b[4];
        file.read(reinterpret_cast<char*>(b), 4);
        return (static_cast<uint32_t>(b[0]) << 24) |
               (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) |
               static_cast<uint32_t>(b[3]);
    };
    auto read_u64 = [&]() -> uint64_t {
        unsigned char b[8];
        file.read(reinterpret_cast<char*>(b), 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | b[i];
        }
        return v;
    };
    auto read_string = [&]() -> std::string {
        uint32_t len = read_u32();
        std::string s(len, '\0');
        file.read(&s[0], static_cast<std::streamsize>(len));
        return s;
    };

    // Check magic
    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "CSTR", 4) != 0) {
        return std::nullopt;
    }

    CacheEntry entry;

    // Timestamps
    uint64_t stored_epoch = read_u64();
    uint64_t expires_epoch = read_u64();
    entry.stored_time = std::chrono::system_clock::time_point(
        std::chrono::seconds(stored_epoch));
    entry.expires = std::chrono::system_clock::time_point(
        std::chrono::seconds(expires_epoch));

    // URL and method
    entry.url = read_string();
    entry.method = read_string();

    // Status code
    entry.status_code = static_cast<int>(read_u32());

    // Headers
    uint32_t num_headers = read_u32();
    for (uint32_t i = 0; i < num_headers; ++i) {
        std::string name = read_string();
        std::string value = read_string();
        entry.headers.emplace_back(name, value);
    }

    // Validators
    entry.etag = read_string();
    entry.last_modified = read_string();

    // Vary headers
    uint32_t num_vary = read_u32();
    for (uint32_t i = 0; i < num_vary; ++i) {
        entry.vary_headers.push_back(read_string());
    }

    // Body
    uint64_t body_len = read_u64();
    entry.body.resize(static_cast<size_t>(body_len));
    file.read(reinterpret_cast<char*>(entry.body.data()),
              static_cast<std::streamsize>(body_len));

    // Parse Cache-Control from headers
    auto cc = get_header(entry.headers, "cache-control");
    if (cc) {
        entry.cache_control = parse_cache_control(*cc);
    }

    // Check if revalidation is needed
    entry.needs_revalidation = entry.cache_control.no_cache ||
                                entry.cache_control.must_revalidate;

    return entry;
}

// TEACHING NOTE: Cache lookup and storage
//
// The lookup function:
// 1. Computes the cache filename from the URL
// 2. Reads the cache entry from disk
// 3. Returns the entry (the caller checks is_fresh/can_serve)
//
// The store function:
// 1. Parses response headers for caching directives
// 2. If no-store, does not cache
// 3. Creates a CacheEntry and writes it to disk
// 4. Updates the in-memory index
//
// Cacheability decision:
// We only cache responses that are cacheable per HTTP spec:
// - Response to GET or HEAD (not POST, PUT, DELETE)
// - Status codes that are cacheable: 200, 203, 204, 206, 300, 301, 404, 405, 410, 414, 501
// - No Cache-Control: no-store
// - No Vary: * (which means do not cache)

std::optional<CacheEntry> HttpCache::lookup(const std::string& url, const std::string& method) {
    auto it = index_.find(url);
    if (it == index_.end()) {
        return std::nullopt;
    }

    std::string filepath = cache_dir_ + "/" + it->second;
    auto entry = read_entry(filepath);
    if (!entry) {
        // Entry is corrupted - remove from index
        index_.erase(it);
        unlink(filepath.c_str());
        return std::nullopt;
    }

    // Check method matches
    if (entry->method != method) {
        return std::nullopt;
    }

    return entry;
}

void HttpCache::store(
    const std::string& url,
    const std::string& method,
    int status_code,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<uint8_t>& body
) {
    // Parse Cache-Control
    auto cc_str = get_header(headers, "cache-control");
    CacheControl cc;
    if (cc_str) {
        cc = parse_cache_control(*cc_str);
    }

    // Check if response is cacheable
    // TEACHING NOTE: Which responses are cacheable?
    //
    // Per RFC 7234, a response is cacheable if:
    // 1. The method is GET or HEAD
    // 2. The status code is one of: 200, 203, 204, 206, 300, 301, 404, 405, 410, 414, 501
    // 3. Cache-Control does not say no-store
    // 4. There is no Vary: * header
    //
    // Some status codes are cacheable by default (200, 203, 204, 206, 300, 301)
    // while others require explicit caching directives (404, 405, 410, 414, 501).

    if (cc.no_store) return;

    if (method != "GET" && method != "HEAD") return;

    // Check cacheable status codes
    static const int cacheable_codes[] = {200, 203, 204, 206, 300, 301, 404, 405, 410, 414, 501};
    bool cacheable = false;
    for (int code : cacheable_codes) {
        if (status_code == code) { cacheable = true; break; }
    }
    if (!cacheable) return;

    // Check Vary: *
    auto vary = get_header(headers, "vary");
    std::vector<std::string> vary_headers;
    if (vary) {
        // Parse comma-separated Vary values
        std::istringstream iss(*vary);
        std::string token;
        while (std::getline(iss, token, ',')) {
            size_t s = token.find_first_not_of(" \t");
            size_t e = token.find_last_not_of(" \t");
            if (s != std::string::npos) {
                std::string v = token.substr(s, e - s + 1);
                if (v == "*") return;  // Vary: * means do not cache
                vary_headers.push_back(v);
            }
        }
    }

    // Build the cache entry
    CacheEntry entry;
    entry.url = url;
    entry.method = method;
    entry.status_code = status_code;
    entry.headers = headers;
    entry.body = body;
    entry.stored_time = std::chrono::system_clock::now();
    entry.cache_control = cc;
    entry.vary_headers = vary_headers;

    // Set expiration time
    if (cc.max_age >= 0) {
        entry.expires = entry.stored_time + std::chrono::seconds(cc.max_age);
    } else {
        auto expires_str = get_header(headers, "expires");
        if (expires_str) {
            entry.expires = parse_expires(*expires_str);
        } else {
            // TEACHING NOTE: Heuristic freshness
            //
            // If no explicit freshness is given, we use a heuristic:
            // 10% of the time since Last-Modified. This is the standard
            // heuristic suggested by RFC 7234. For example, if a resource
            // was last modified 100 days ago, we cache it for 10 days.
            auto lm_str = get_header(headers, "last-modified");
            if (lm_str) {
                auto lm = parse_http_date(*lm_str);
                if (lm != std::chrono::system_clock::time_point{}) {
                    auto delta = std::chrono::duration_cast<std::chrono::seconds>(
                        entry.stored_time - lm).count();
                    if (delta > 0) {
                        entry.expires = entry.stored_time +
                                       std::chrono::seconds(delta / 10);
                    }
                }
            }
        }
    }

    // Set validators
    auto etag_str = get_header(headers, "etag");
    if (etag_str) entry.etag = *etag_str;

    auto lm_str = get_header(headers, "last-modified");
    if (lm_str) entry.last_modified = *lm_str;

    // Set revalidation flag
    entry.needs_revalidation = cc.no_cache || cc.must_revalidate;

    // Write to disk
    std::string filename = url_to_filename(url);
    std::string filepath = cache_dir_ + "/" + filename;
    write_entry(filepath, entry);

    // Update index
    index_[url] = filename;
}

void HttpCache::invalidate(const std::string& url) {
    auto it = index_.find(url);
    if (it != index_.end()) {
        std::string filepath = cache_dir_ + "/" + it->second;
        unlink(filepath.c_str());
        index_.erase(it);
    }
}

void HttpCache::clear() {
    for (const auto& [url, filename] : index_) {
        std::string filepath = cache_dir_ + "/" + filename;
        unlink(filepath.c_str());
    }
    index_.clear();
}

size_t HttpCache::size() const {
    size_t total = 0;
    for (const auto& [url, filename] : index_) {
        (void)url;
        std::string filepath = cache_dir_ + "/" + filename;
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0) {
            total += static_cast<size_t>(st.st_size);
        }
    }
    return total;
}

void HttpCache::cleanup_expired() {
    std::vector<std::string> to_remove;

    for (const auto& [url, filename] : index_) {
        std::string filepath = cache_dir_ + "/" + filename;
        auto entry = read_entry(filepath);
        if (entry && entry->is_fresh()) {
            continue;
        }
        to_remove.push_back(url);
    }

    for (const auto& url : to_remove) {
        invalidate(url);
    }
}

} // namespace chinstrap