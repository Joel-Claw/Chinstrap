// cookies.hpp - Cookie jar from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: What are cookies and why does a browser need to manage them?
//
// Cookies (RFC 6265) are small pieces of data that a server sends to the browser
// via the Set-Cookie HTTP response header. The browser stores them and sends
// them back in the Cookie HTTP request header on subsequent requests to the
// same domain. Cookies are the foundation of web sessions: they allow a server
// to identify a user across multiple requests (login state, shopping cart,
// preferences, tracking, etc.).
//
// Cookie attributes:
// - Name and Value: the key-value pair (e.g., "sessionid=abc123")
// - Domain: which hosts the cookie applies to (e.g., ".example.com" matches
//   www.example.com and example.com). If not set, defaults to the request host.
// - Path: which URL paths the cookie applies to (e.g., "/store" matches
//   /store and /store/item). If not set, defaults to "/".
// - Expires: a date when the cookie should be deleted. If not set, the cookie
//   is a "session cookie" and is deleted when the browser closes.
// - Max-Age: number of seconds until the cookie expires. Overrides Expires if both set.
// - Secure: only send the cookie over HTTPS connections.
// - HttpOnly: JavaScript cannot access the cookie via document.cookie.
//   This prevents XSS attacks from stealing cookies.
// - SameSite: controls when the cookie is sent in cross-site requests.
//   Strict: only sent in same-site requests
//   Lax: sent in top-level navigations (like clicking a link) but not in
//     cross-site subrequests (like images or iframes)
//   None: sent in all cross-site requests (requires Secure)
//
// TEACHING NOTE: Same-Site policy and cookie isolation
//
// Cookies are powerful but also a privacy and security risk. Third-party
// cookies (cookies set by a domain other than the one in the URL bar) are
// used for tracking users across websites. Chrome is phasing out third-party
// cookies and introducing alternative APIs (like the Privacy Sandbox).
//
// Chrome isolates cookies per profile (each Chrome profile has its own
// cookie jar). It also supports "first-party sets" where related domains
// can share cookies. The SameSite attribute is the primary mechanism for
// controlling cross-site cookie behavior.

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <cstdint>

namespace chinstrap {

enum class SameSite {
    Strict,
    Lax,
    None,
    Unspecified,
};

// TEACHING NOTE: Cookie structure
//
// A cookie is a simple key-value pair with metadata. The metadata controls
// when and where the cookie is sent. We store cookies in a flat file
// (~/.config/chinstrap/cookies.txt) using a simple TSV (tab-separated) format.
// No SQLite, no binary format - just text that is easy to inspect and debug.
struct Cookie {
    std::string name;
    std::string value;
    std::string domain;       // Host or domain the cookie applies to
    std::string path;          // URL path prefix the cookie applies to
    std::chrono::system_clock::time_point expires;  // Expiry time
    bool secure = false;       // Only send over HTTPS
    bool http_only = false;    // Not accessible from JavaScript
    SameSite same_site = SameSite::Unspecified;  // Cross-site behavior
    bool session_cookie = false;  // True if no Expires/Max-Age (deleted on browser close)

    // Check if the cookie has expired
    bool is_expired() const;

    // Check if this cookie should be sent for the given request URL
    bool matches(const std::string& request_host, const std::string& request_path,
                 bool is_secure, bool is_cross_site) const;

    // Serialize to a string for storage (tab-separated)
    std::string serialize() const;

    // Deserialize from a stored string
    static std::optional<Cookie> deserialize(const std::string& line);
};

// TEACHING NOTE: Cookie jar
//
// The cookie jar is the collection of all stored cookies. It manages:
// 1. Parsing Set-Cookie response headers into Cookie objects
// 2. Matching cookies to outgoing requests (which cookies to send?)
// 3. Storing cookies persistently (so they survive browser restarts)
// 4. Expiring old cookies
// 5. Cookie eviction (when the jar is too large)
//
// Chrome stores cookies in an SQLite database, but we use a flat file.
// The flat file is read on startup and written when cookies change.
// For a from-scratch implementation, this is simpler and more transparent.

class CookieJar {
public:
    CookieJar();
    ~CookieJar();

    // Load cookies from the persistent storage file
    void load(const std::string& filepath);

    // Save cookies to the persistent storage file
    void save(const std::string& filepath) const;

    // TEACHING NOTE: Set-Cookie parsing
    //
    // The Set-Cookie header looks like:
    //   Set-Cookie: sessionid=abc123; Domain=.example.com; Path=/; Secure; HttpOnly; SameSite=Lax
    //
    // Multiple Set-Cookie headers can appear in a single response.
    // The name=value pair comes first, followed by attributes separated by semicolons.
    // We parse this into a Cookie struct, applying defaults for missing attributes.
    //
    // Edge cases:
    // - Values can contain "=" signs (e.g., "data=a=b")
    // - Attribute names are case-insensitive
    // - Expires uses the HTTP date format (RFC 7231)
    // - Max-Age is in seconds (a higher-priority alternative to Expires)

    // Parse a Set-Cookie header and add the cookie to the jar
    // Returns true if the cookie was valid and stored
    bool parse_set_cookie(const std::string& header_value, const std::string& request_host);

    // Get cookies to send for a request as a Cookie header value
    // Returns "name1=value1; name2=value2" or empty string if no cookies match
    std::string get_cookie_header(const std::string& request_host,
                                  const std::string& request_path,
                                  bool is_secure,
                                  bool is_cross_site) const;

    // Get all cookies (for inspection/debugging)
    std::vector<Cookie> get_all_cookies() const;

    // Remove expired cookies from the jar
    void cleanup_expired();

    // Clear all cookies
    void clear();

    // Delete cookies for a specific domain
    void delete_for_domain(const std::string& domain);

    // TEACHING NOTE: Domain matching (RFC 6265, Section 5.1.3)
    //
    // These static methods are public because Cookie::matches() needs to call them.
    // They are utility functions that implement the RFC 6265 matching rules.

    static bool domain_matches(const std::string& request_host, const std::string& cookie_domain);
    static bool path_matches(const std::string& request_path, const std::string& cookie_path);
    static std::string default_path(const std::string& request_path);
    static std::chrono::system_clock::time_point parse_http_date(const std::string& date_str);
    static std::string format_http_date(std::chrono::system_clock::time_point tp);

private:
    // Cookie storage: we use a map keyed by "domain\tname" for fast lookup
    std::unordered_map<std::string, Cookie> cookies_;

    static bool is_host_only(const std::string& cookie_domain);
};

} // namespace chinstrap