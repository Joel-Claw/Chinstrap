// =========================================================================
// url.hpp - URL Parser (RFC 3986)
// =========================================================================
// TEACHING NOTE: Every browser journey starts with a URL. When you type
// "example.com" in the address bar, the browser first parses that into
// structured components: scheme, host, port, path, query, fragment.
// Then it knows to use HTTP (or HTTPS) to connect to that host on the
// right port and request that path.
//
// This parser follows RFC 3986, the IETF standard for URIs. Real browsers
// actually follow the WHATWG URL Living Standard, which has some
// differences (noted below). We implement RFC 3986 because it is a stable,
// well-documented standard. The WHATWG spec is what Chrome and Firefox
// actually implement, but it is a living document that changes frequently.
//
// Key RFC 3986 grammar:
//   URI = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
//   hier-part = "//" authority path-abempty
//             / path-absolute
//             / path-rootless
//             / path-empty
//   authority = [ userinfo "@" ] host [ ":" port ]
//
// WHATWG vs RFC 3986 differences:
// - WHATWG treats some schemes as "special" (http, https, ftp, ws, wss,
//   file) with special parsing rules.
// - WHATWG handles IDNA (international domain names) via punycode.
// - WHATWG is more lenient with malformed input (it tries to fix it).
// - WHATWG separates the URL parser into "basic" and "non-basic" paths.
// - RFC 3986 is stricter and more regular.
// We implement RFC 3986 with practical extensions for HTTP.
// =========================================================================

#ifndef CHINSTRAP_URL_HPP
#define CHINSTRAP_URL_HPP

#include <string>
#include <cstdint>
#include <optional>

namespace chinstrap {

// =========================================================================
// class Url - A parsed URL with all components accessible
// =========================================================================
// TEACHING NOTE: We use std::optional for components that may or may not
// be present. A URL like "http://example.com" has no fragment, no query,
// no port, and no userinfo. std::optional cleanly represents "absent"
// without using magic values like -1 or empty strings.
//
// In a real browser, the URL object is one of the most-used data
// structures. It gets passed everywhere: to the network stack, the
// history manager, the bookmark system, the security origin checker.
// Chrome uses the GURL class (in src/url/gurl.h). Firefox uses
// nsStandardURL. Both are much more complex than this, but the concept
// is the same: parse a string into components, provide accessors, and
// reconstruct the string when needed.
// =========================================================================

class Url {
public:
    // Default constructor creates an empty/invalid URL
    Url() = default;

    // Parse a URL string. Throws std::invalid_argument if the URL is
    // malformed to the point we cannot parse it.
    // TEACHING NOTE: We could return std::optional<Url> instead of
    // throwing, but throwing is more explicit about errors and is
    // what RFC-style parsers typically do. The caller can catch.
    explicit Url(const std::string& url_string);

    // --- Component accessors ---
    // Each returns the component as a string, or empty string if absent.
    // For port, use has_port() + port() since port 0 is different from
    // no port specified.

    const std::string& scheme() const { return scheme_; }
    const std::string& userinfo() const { return userinfo_; }
    const std::string& host() const { return host_; }
    const std::string& path() const { return path_; }
    const std::string& query() const { return query_; }
    const std::string& fragment() const { return fragment_; }

    bool has_port() const { return port_.has_value(); }
    std::uint16_t port() const { return port_.value_or(default_port()); }

    // The default port for this URL scheme (80 for http, 443 for https, etc.)
    // Returns 0 if unknown scheme.
    std::uint16_t default_port() const;

    // True if this URL is valid (was parsed successfully)
    bool is_valid() const { return valid_; }

    // Reconstruct the URL string from components.
    // TEACHING NOTE: This is important because browsers often normalize
    // URLs. For example, "HTTP://Example.COM:80/path" should become
    // "http://example.com/path" after normalization. Our to_string does
    // basic normalization (lowercase scheme, omit default port) but is
    // not a full normalizer.
    std::string to_string() const;

    // Percent-encode a string for use in a URL component.
    // TEACHING NOTE: URLs can only contain ASCII characters in a limited
    // set. Other characters must be percent-encoded (e.g., space -> %20).
    // Real browsers have complex encoding rules per component (e.g., the
    // path uses different encoding rules than the query). We implement
    // a general-purpose encoder.
    static std::string percent_encode(const std::string& input);
    static std::string percent_decode(const std::string& input);

    // Resolve a relative URL against this base URL.
    // TEACHING NOTE: When a browser finds <a href="/page"> on a page at
    // "http://example.com/foo/bar", it must resolve "/page" to
    // "http://example.com/page". This is called relative URL resolution
    // and is defined in RFC 3986 Section 5.3.
    Url resolve(const std::string& relative) const;

private:
    std::string scheme_;       // e.g., "http", "https"
    std::string userinfo_;     // e.g., "user:pass" (empty if absent)
    std::string host_;         // e.g., "example.com" (lowercase)
    std::optional<std::uint16_t> port_;  // e.g., 8080 (absent if not specified)
    std::string path_;         // e.g., "/index.html" (empty or "/" if root)
    std::string query_;        // e.g., "key=value" (empty if no "?")
    std::string fragment_;     // e.g., "section1" (empty if no "#")
    bool valid_ = false;

    // Internal parsing helpers
    void parse(const std::string& url_string);
    static std::optional<std::uint16_t> parse_port(const std::string& str);
};

} // namespace chinstrap

#endif // CHINSTRAP_URL_HPP