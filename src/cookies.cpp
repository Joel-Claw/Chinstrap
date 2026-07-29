// cookies.cpp - Cookie jar implementation from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Cookie implementation overview
//
// Cookies are one of the oldest and most important web technologies. They were
// invented in 1994 by Lou Montulli at Netscape as a way to add state to the
// otherwise stateless HTTP protocol. The original "magic cookie" concept came
// from UNIX programming where objects were passed between programs.
//
// The cookie specification has evolved over time:
// - Netscape Cookie Spec (1994): original implementation
// - RFC 2109 (1997): first IETF standard
// - RFC 2965 (2000): introduced version 2 cookies (never widely adopted)
// - RFC 6265 (2011): the current standard, simplified and consolidated
// - RFC 6265bis (2020s): proposed updates including SameSite attribute
//
// How cookies flow through a browser:
//
// 1. Browser sends a GET request to https://example.com/login
// 2. Server responds with:
//      Set-Cookie: session=abc123; Path=/; Secure; HttpOnly; SameSite=Lax
// 3. Browser stores the cookie in its cookie jar
// 4. On the next request to https://example.com/dashboard:
//      Cookie: session=abc123
// 5. Server reads the Cookie header and identifies the user
//
// TEACHING NOTE: Cookie security considerations
//
// Cookies are a prime target for attacks:
//
// 1. XSS (Cross-Site Scripting): An attacker injects JavaScript that reads
//    document.cookie and sends it to their server. Mitigation: HttpOnly flag
//    prevents JavaScript from accessing the cookie.
//
// 2. CSRF (Cross-Site Request Forgery): An attacker tricks the browser into
//    making a request to a site where the user is logged in. The browser
//    automatically includes cookies. Mitigation: SameSite=Strict or Lax
//    prevents cookies from being sent in cross-site requests.
//
// 3. Session hijacking: An attacker intercepts a cookie value and impersonates
//    the user. Mitigation: Secure flag ensures cookies are only sent over
//    HTTPS, preventing interception on unencrypted connections.
//
// 4. Cookie tossing: An attacker sets a cookie from a subdomain that overrides
//    a parent domain cookie. Mitigation: careful domain attribute handling.
//
// Chrome isolates cookies per browser profile and has been phasing out
// third-party cookies (cookies set by a different domain than the one in the
// URL bar). The Privacy Sandbox initiative provides alternative APIs like
// the Storage Access API and CHIPS (Cookies Having Independent Partitioned State).

#include "cookies.hpp"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cctype>
#include <iomanip>
#include <time.h>

namespace chinstrap {

// ============================================================================
// Cookie
// ============================================================================

bool Cookie::is_expired() const {
    if (session_cookie) {
        return false;  // Session cookies do not expire based on time
    }
    return std::chrono::system_clock::now() >= expires;
}

// TEACHING NOTE: Cookie matching logic
//
// When the browser is about to make a request to https://www.example.com/store/item,
// it needs to determine which cookies to include. For each stored cookie, it checks:
//
// 1. Domain match: Is www.example.com in the cookie domain?
//    - If the cookie domain is "example.com", www.example.com is a subdomain, so it matches.
//    - If the cookie domain is "www.example.com", it is an exact match.
//    - If the cookie domain is "other.com", it does not match.
//
// 2. Path match: Is /store/item in the cookie path?
//    - If the cookie path is "/", everything matches.
//    - If the cookie path is "/store", it matches /store and /store/*.
//    - If the cookie path is "/shop", it does not match /store/item.
//
// 3. Secure match: If the cookie has the Secure flag, the request must be over HTTPS.
//    A request to http://www.example.com would not include Secure cookies.
//
// 4. SameSite match: If the cookie has SameSite=Strict, it is only sent when
//    the request is same-site (the URL bar origin matches the request origin).
//    SameSite=Lax allows top-level navigations (GET requests from link clicks)
//    but not cross-site subrequests (images, iframes, fetch).
//    SameSite=None allows all cross-site requests (requires Secure).

bool Cookie::matches(const std::string& request_host, const std::string& request_path,
                      bool is_secure, bool is_cross_site) const {
    // Check secure flag
    if (secure && !is_secure) {
        return false;
    }

    // Check same-site
    if (is_cross_site) {
        if (same_site == SameSite::Strict) {
            return false;
        }
        if (same_site == SameSite::Lax) {
            // Lax allows top-level navigations but we do not distinguish
            // navigation from subrequests here. A full implementation would
            // check the request method and navigation type.
            return false;
        }
        // SameSite=None or Unspecified: allow cross-site
    }

    // Check domain match
    if (!CookieJar::domain_matches(request_host, domain)) {
        return false;
    }

    // Check path match
    if (!CookieJar::path_matches(request_path, path)) {
        return false;
    }

    // Check expiry
    if (is_expired()) {
        return false;
    }

    return true;
}

std::string Cookie::serialize() const {
    // TEACHING NOTE: Cookie storage format
    //
    // We store cookies in a tab-separated format:
    //   name<TAB>value<TAB>domain<TAB>path<TAB>expires_timestamp<TAB>secure<TAB>http_only<TAB>same_site<TAB>session_cookie
    //
    // The timestamp is stored as a Unix epoch value (seconds since 1970-01-01 UTC).
    // This format is simple to parse and inspect. We use tabs as delimiters
    // because cookie values can contain spaces and semicolons but typically not tabs.
    //
    // Chrome uses SQLite for cookie storage. SQLite provides atomic writes,
    // indexed lookups, and transactional integrity. Our flat file is simpler
    // but less robust - if the browser crashes during a write, cookies could
    // be lost. A production implementation would use a more durable format.

    std::ostringstream oss;

    // Escape tab and newline characters in values
    auto escape = [](const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '\t') { result += "\\t"; }
            else if (c == '\n') { result += "\\n"; }
            else if (c == '\\') { result += "\\\\"; }
            else { result += c; }
        }
        return result;
    };

    oss << escape(name) << '\t'
        << escape(value) << '\t'
        << escape(domain) << '\t'
        << escape(path) << '\t';

    if (session_cookie) {
        oss << "0";  // 0 means session cookie (no expiry)
    } else {
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            expires.time_since_epoch()).count();
        oss << epoch;
    }

    oss << '\t' << (secure ? "1" : "0")
        << '\t' << (http_only ? "1" : "0")
        << '\t' << static_cast<int>(same_site)
        << '\t' << (session_cookie ? "1" : "0");

    return oss.str();
}

std::optional<Cookie> Cookie::deserialize(const std::string& line) {
    // Parse tab-separated cookie data
    auto unescape = [](const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (s[i+1] == 't') { result += '\t'; ++i; }
                else if (s[i+1] == 'n') { result += '\n'; ++i; }
                else if (s[i+1] == '\\') { result += '\\'; ++i; }
                else { result += s[i]; }
            } else {
                result += s[i];
            }
        }
        return result;
    };

    // Split by tabs
    std::vector<std::string> fields;
    std::string current;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current += line[i];
        }
    }
    fields.push_back(current);

    if (fields.size() < 8) {
        return std::nullopt;
    }

    Cookie cookie;
    cookie.name = unescape(fields[0]);
    cookie.value = unescape(fields[1]);
    cookie.domain = unescape(fields[2]);
    cookie.path = unescape(fields[3]);

    int64_t epoch = std::stoll(fields[4]);
    if (epoch == 0) {
        cookie.session_cookie = true;
    } else {
        cookie.session_cookie = false;
        cookie.expires = std::chrono::system_clock::time_point(
            std::chrono::seconds(epoch));
    }

    cookie.secure = (fields[5] == "1");
    cookie.http_only = (fields[6] == "1");

    int ss_val = std::stoi(fields[7]);
    cookie.same_site = static_cast<SameSite>(ss_val);

    if (fields.size() >= 9) {
        cookie.session_cookie = (fields[8] == "1");
    }

    return cookie;
}

// ============================================================================
// CookieJar
// ============================================================================

CookieJar::CookieJar() = default;
CookieJar::~CookieJar() = default;

void CookieJar::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto cookie = Cookie::deserialize(line);
        if (cookie && !cookie->is_expired()) {
            std::string key = cookie->domain + "\t" + cookie->name;
            cookies_[key] = std::move(*cookie);
        }
    }
}

void CookieJar::save(const std::string& filepath) const {
    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    file << "# Chinstrap cookie jar - do not edit manually\n";
    file << "# Format: name\\tvalue\\tdomain\\tpath\\texpires\\tsecure\\thttp_only\\tsame_site\\tsession\n";

    for (const auto& [key, cookie] : cookies_) {
        (void)key;
        if (!cookie.is_expired()) {
            file << cookie.serialize() << '\n';
        }
    }
}

// TEACHING NOTE: Set-Cookie header parsing
//
// The Set-Cookie header format (RFC 6265, Section 4.1):
//
//   set-cookie = cookie-pair *( ";" SP cookie-av )
//   cookie-pair = cookie-name "=" cookie-value
//   cookie-av = expires-av / max-age-av / domain-av /
//               path-av / secure-av / httponly-av / samesite-av /
//               extension-av
//
// Example:
//   Set-Cookie: user=John; Domain=.example.com; Path=/; Expires=Wed, 09 Jun 2021 10:18:14 GMT; Secure; HttpOnly; SameSite=Lax
//
// Parsing steps:
// 1. Split on first "=" to get name and value
// 2. Split remaining on ";" to get attribute-value pairs
// 3. Parse each attribute (case-insensitive name)
// 4. Apply defaults for missing attributes
//
// Edge cases:
// - Values can contain "=" (split on FIRST "=" only)
// - Values can contain ";" if quoted (RFC 6265 allows quoted-string values)
// - Attribute names are case-insensitive ("Secure" and "secure" are the same)
// - Expires date can be in various formats (we try RFC 7231 IMF-fixdate)

bool CookieJar::parse_set_cookie(const std::string& header_value, const std::string& request_host) {
    Cookie cookie;
    cookie.session_cookie = true;  // Default to session cookie
    cookie.path = "/";  // Default path

    // Split on first "=" to get name and value
    size_t eq_pos = header_value.find('=');
    if (eq_pos == std::string::npos) {
        return false;  // Invalid cookie, no "=" found
    }

    // Find the first semicolon to separate the cookie-pair from attributes
    size_t first_semi = header_value.find(';');
    std::string cookie_pair;
    std::string attributes;

    if (first_semi == std::string::npos) {
        cookie_pair = header_value;
    } else {
        cookie_pair = header_value.substr(0, first_semi);
        attributes = header_value.substr(first_semi);
    }

    // Parse name and value
    cookie.name = cookie_pair.substr(0, eq_pos);
    cookie.value = cookie_pair.substr(eq_pos + 1);

    // Trim whitespace from name
    size_t name_start = cookie.name.find_first_not_of(" \t");
    size_t name_end = cookie.name.find_last_not_of(" \t");
    if (name_start == std::string::npos) {
        return false;
    }
    cookie.name = cookie.name.substr(name_start, name_end - name_start + 1);

    // Trim whitespace from value
    size_t val_start = cookie.value.find_first_not_of(" \t");
    size_t val_end = cookie.value.find_last_not_of(" \t");
    if (val_start != std::string::npos) {
        cookie.value = cookie.value.substr(val_start, val_end - val_start + 1);
    } else {
        cookie.value = "";
    }

    if (cookie.name.empty()) {
        return false;
    }

    // Parse attributes
    std::istringstream attr_stream(attributes);
    std::string attr;
    while (std::getline(attr_stream, attr, ';')) {
        // Trim leading whitespace
        size_t start = attr.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        attr = attr.substr(start);

        // Convert to lowercase for comparison (attribute names are case-insensitive)
        std::string lower_attr = attr;
        std::transform(lower_attr.begin(), lower_attr.end(), lower_attr.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (lower_attr.substr(0, 7) == "expires") {
            // Expires attribute
            size_t eq = attr.find('=');
            if (eq != std::string::npos) {
                std::string date_str = attr.substr(eq + 1);
                // Trim whitespace
                size_t s = date_str.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    date_str = date_str.substr(s);
                }
                cookie.expires = parse_http_date(date_str);
                cookie.session_cookie = false;
            }
        } else if (lower_attr.substr(0, 7) == "max-age") {
            // Max-Age attribute
            size_t eq = attr.find('=');
            if (eq != std::string::npos) {
                std::string age_str = attr.substr(eq + 1);
                try {
                    int max_age = std::stoi(age_str);
                    if (max_age <= 0) {
                        // Cookie expires immediately - do not store
                        return false;
                    }
                    cookie.expires = std::chrono::system_clock::now() +
                                     std::chrono::seconds(max_age);
                    cookie.session_cookie = false;
                } catch (...) {
                    // Invalid max-age, ignore
                }
            }
        } else if (lower_attr.substr(0, 6) == "domain") {
            // Domain attribute
            size_t eq = attr.find('=');
            if (eq != std::string::npos) {
                cookie.domain = attr.substr(eq + 1);
                // Trim whitespace
                size_t s = cookie.domain.find_first_not_of(" \t");
                size_t e = cookie.domain.find_last_not_of(" \t");
                if (s != std::string::npos) {
                    cookie.domain = cookie.domain.substr(s, e - s + 1);
                }
                // Remove leading dot (modern RFC 6265 treats domain with or
                // without leading dot the same way)
                if (!cookie.domain.empty() && cookie.domain[0] == '.') {
                    cookie.domain = cookie.domain.substr(1);
                }
            }
        } else if (lower_attr.substr(0, 4) == "path") {
            // Path attribute
            size_t eq = attr.find('=');
            if (eq != std::string::npos) {
                cookie.path = attr.substr(eq + 1);
                size_t s = cookie.path.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    cookie.path = cookie.path.substr(s);
                }
                if (cookie.path.empty()) {
                    cookie.path = "/";
                }
            }
        } else if (lower_attr == "secure") {
            cookie.secure = true;
        } else if (lower_attr == "httponly") {
            cookie.http_only = true;
        } else if (lower_attr.substr(0, 8) == "samesite") {
            size_t eq = attr.find('=');
            if (eq != std::string::npos) {
                std::string ss_val = attr.substr(eq + 1);
                size_t s = ss_val.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    ss_val = ss_val.substr(s);
                }
                std::transform(ss_val.begin(), ss_val.end(), ss_val.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (ss_val == "strict") {
                    cookie.same_site = SameSite::Strict;
                } else if (ss_val == "lax") {
                    cookie.same_site = SameSite::Lax;
                } else if (ss_val == "none") {
                    cookie.same_site = SameSite::None;
                } else {
                    cookie.same_site = SameSite::Unspecified;
                }
            }
        }
    }

    // Apply defaults
    if (cookie.domain.empty()) {
        // No Domain attribute: host-only cookie for the request host
        cookie.domain = request_host;
    }

    // Store the cookie
    std::string key = cookie.domain + "\t" + cookie.name;
    cookies_[key] = std::move(cookie);

    return true;
}

// TEACHING NOTE: Building the Cookie request header
//
// When making a request, the browser collects all matching cookies and
// formats them as a single Cookie header:
//
//   Cookie: name1=value1; name2=value2; name3=value3
//
// Cookies are separated by "; " (semicolon space). The order is not
// defined by the spec, but Chrome sorts cookies by path length (longest
// first) to ensure more specific cookies appear first. If two cookies
// have the same path, they are sorted by creation time.
//
// We iterate over all stored cookies, check if each matches the request,
// and collect the matching ones. Expired cookies are skipped (and could
// be cleaned up at this point).

std::string CookieJar::get_cookie_header(
    const std::string& request_host,
    const std::string& request_path,
    bool is_secure,
    bool is_cross_site
) const {
    std::vector<const Cookie*> matching;

    for (const auto& [key, cookie] : cookies_) {
        (void)key;
        if (cookie.matches(request_host, request_path, is_secure, is_cross_site)) {
            matching.push_back(&cookie);
        }
    }

    if (matching.empty()) {
        return "";
    }

    // Sort by path length (longest first) for deterministic ordering
    std::sort(matching.begin(), matching.end(),
              [](const Cookie* a, const Cookie* b) {
                  return a->path.length() > b->path.length();
              });

    std::string result;
    for (size_t i = 0; i < matching.size(); ++i) {
        if (i > 0) {
            result += "; ";
        }
        result += matching[i]->name + "=" + matching[i]->value;
    }

    return result;
}

std::vector<Cookie> CookieJar::get_all_cookies() const {
    std::vector<Cookie> result;
    for (const auto& [key, cookie] : cookies_) {
        (void)key;
        result.push_back(cookie);
    }
    return result;
}

void CookieJar::cleanup_expired() {
    for (auto it = cookies_.begin(); it != cookies_.end();) {
        if (it->second.is_expired()) {
            it = cookies_.erase(it);
        } else {
            ++it;
        }
    }
}

void CookieJar::clear() {
    cookies_.clear();
}

void CookieJar::delete_for_domain(const std::string& domain) {
    for (auto it = cookies_.begin(); it != cookies_.end();) {
        if (it->second.domain == domain) {
            it = cookies_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// Domain matching (RFC 6265, Section 5.1.3)
// ============================================================================
//
// TEACHING NOTE: Domain matching rules
//
// A request host "www.example.com" matches a cookie domain "example.com" because:
// 1. The cookie domain is a suffix of the request host
// 2. The character before the suffix in the request host is a dot
// (The dot is between "www" and "example.com")
//
// A request host "example.com" matches a cookie domain "example.com" because:
// 1. They are exactly equal
//
// A request host "notexample.com" does NOT match cookie domain "example.com"
// because the character before "example.com" in "notexample.com" is "t", not a dot.
//
// We also handle the case where the cookie domain has a leading dot (older
// cookies use ".example.com"). The leading dot is stripped and the same
// rules apply.

bool CookieJar::domain_matches(const std::string& request_host, const std::string& cookie_domain) {
    if (cookie_domain.empty()) {
        return false;
    }

    // Case-insensitive comparison (DNS is case-insensitive)
    std::string host = request_host;
    std::string domain = cookie_domain;
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(domain.begin(), domain.end(), domain.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Exact match
    if (host == domain) {
        return true;
    }

    // Suffix match: host ends with "." + domain
    if (host.length() > domain.length() + 1) {
        std::string suffix = host.substr(host.length() - domain.length());
        if (suffix == domain && host[host.length() - domain.length() - 1] == '.') {
            return true;
        }
    }

    return false;
}

bool CookieJar::is_host_only(const std::string& cookie_domain) {
    // A cookie is "host-only" if its domain attribute was not set (or was
    // set to the exact request host without a leading dot). We cannot
    // distinguish this from our stored representation (we strip the leading
    // dot), so we always treat cookies as domain cookies. This is a
    // simplification - a full implementation would track the host-only flag.
    (void)cookie_domain;
    return false;
}

// ============================================================================
// Path matching (RFC 6265, Section 5.1.4)
// ============================================================================

bool CookieJar::path_matches(const std::string& request_path, const std::string& cookie_path) {
    if (cookie_path.empty()) {
        return true;  // Empty path matches everything
    }

    // Exact match
    if (request_path == cookie_path) {
        return true;
    }

    // Prefix match: request path starts with cookie path
    if (request_path.length() > cookie_path.length()) {
        if (request_path.substr(0, cookie_path.length()) == cookie_path) {
            // The character after the cookie path must be "/" or the cookie
            // path must end with "/"
            if (cookie_path.back() == '/' ||
                request_path[cookie_path.length()] == '/') {
                return true;
            }
        }
    }

    return false;
}

std::string CookieJar::default_path(const std::string& request_path) {
    // TEACHING NOTE: Default cookie path (RFC 6265, Section 5.1.4)
    //
    // When a Set-Cookie header does not include a Path attribute, the default
    // path is derived from the request URI:
    // 1. If the path is empty or does not start with "/", default to "/"
    // 2. If the path is just "/", default to "/"
    // 3. Otherwise, take everything up to but not including the last "/"
    //    Example: "/a/b/c" -> "/a/b"
    //             "/a/b/" -> "/a/b"

    if (request_path.empty() || request_path[0] != '/') {
        return "/";
    }

    // Strip query string and fragment if present
    std::string path = request_path;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    size_t f = path.find('#');
    if (f != std::string::npos) path = path.substr(0, f);

    if (path == "/") {
        return "/";
    }

    size_t last_slash = path.rfind('/');
    if (last_slash == 0) {
        return "/";
    }

    return path.substr(0, last_slash);
}

// ============================================================================
// HTTP date parsing
// ============================================================================
//
// TEACHING NOTE: HTTP date format (RFC 7231, Section 7.1.1.1)
//
// HTTP dates use the IMF-fixdate format:
//   Day, DD Mon YYYY HH:MM:SS GMT
//   Example: "Sun, 06 Nov 1994 08:49:37 GMT"
//
// Other formats (RFC 850, asctime) are also accepted for backward compatibility
// but IMF-fixdate is preferred. We parse using strptime which handles all these.

std::chrono::system_clock::time_point CookieJar::parse_http_date(const std::string& date_str) {
    // Try IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    struct tm tm_val{};
    const char* format1 = "%a, %d %b %Y %H:%M:%S GMT";
    if (strptime(date_str.c_str(), format1, &tm_val) != nullptr) {
        time_t t = timegm(&tm_val);
        return std::chrono::system_clock::from_time_t(t);
    }

    // Try RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"
    const char* format2 = "%A, %d-%b-%y %H:%M:%S GMT";
    if (strptime(date_str.c_str(), format2, &tm_val) != nullptr) {
        time_t t = timegm(&tm_val);
        return std::chrono::system_clock::from_time_t(t);
    }

    // Try asctime: "Sun Nov  6 08:49:37 1994"
    const char* format3 = "%a %b %d %H:%M:%S %Y";
    if (strptime(date_str.c_str(), format3, &tm_val) != nullptr) {
        time_t t = timegm(&tm_val);
        return std::chrono::system_clock::from_time_t(t);
    }

    // Fallback: return current time (cookie will be treated as expired)
    return std::chrono::system_clock::now();
}

std::string CookieJar::format_http_date(std::chrono::system_clock::time_point tp) {
    time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_val{};
    gmtime_r(&t, &tm_val);

    char buf[128];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
    return std::string(buf);
}

} // namespace chinstrap