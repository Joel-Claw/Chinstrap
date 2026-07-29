// =========================================================================
// url.cpp - URL Parser Implementation
// =========================================================================
// TEACHING NOTE: This file implements the URL parser declared in url.hpp.
// It parses URLs according to RFC 3986. The parser is a manual character-
// by-character parser - no regex, no string streams. This is how real
// URL parsers work because URL parsing has complex state transitions
// that are hard to express with regex.
// =========================================================================

#include "url.hpp"

#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <cctype>

namespace chinstrap {

// =========================================================================
// Url implementation
// =========================================================================

Url::Url(const std::string& url_string) {
    parse(url_string);
}

std::uint16_t Url::default_port() const {
    // TEACHING NOTE: Well-known port numbers are defined by IANA.
    // Browsers know these by heart. When a URL does not specify a port,
    // the browser uses the default for the scheme.
    if (scheme_ == "http") return 80;
    if (scheme_ == "https") return 443;
    if (scheme_ == "ftp") return 21;
    if (scheme_ == "ws") return 80;
    if (scheme_ == "wss") return 443;
    return 0;
}

void Url::parse(const std::string& url_string) {
    // TEACHING NOTE: RFC 3986 parsing. The grammar is:
    //   URI = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
    //   hier-part = "//" authority path-abempty / path-absolute / ...
    //   authority = [ userinfo "@" ] host [ ":" port ]
    //
    // We parse left to right:
    //   1. Scheme (before ":")
    //   2. "//" (indicates authority follows)
    //   3. Authority (userinfo@host:port)
    //   4. Path
    //   5. Query (after "?")
    //   6. Fragment (after "#")

    if (url_string.empty()) {
        valid_ = false;
        return;
    }

    std::size_t pos = 0;

    // 1. Parse scheme
    // TEACHING NOTE: The scheme is the protocol identifier (http, https,
    // ftp, etc). It must start with a letter and can contain letters,
    // digits, +, -, and . It ends with a colon.
    std::size_t colon = url_string.find(':');
    if (colon == std::string::npos || colon == 0) {
        // No scheme: could be a relative URL. For now, treat as invalid.
        // TEACHING NOTE: Real browsers handle scheme-less URLs by
        // defaulting to http. But for strict RFC 3986 parsing, a scheme
        // is required. We could add a default-scheme feature later.
        valid_ = false;
        return;
    }

    scheme_ = url_string.substr(0, colon);
    // Lowercase the scheme (schemes are case-insensitive)
    std::transform(scheme_.begin(), scheme_.end(), scheme_.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Validate scheme: must start with letter, contain only [a-z0-9+.-]
    if (!std::isalpha(static_cast<unsigned char>(scheme_[0]))) {
        valid_ = false;
        return;
    }
    for (char c : scheme_) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
            valid_ = false;
            return;
        }
    }

    pos = colon + 1;

    // 2. Check for "//" (authority indicator)
    if (pos + 1 < url_string.size() && url_string[pos] == '/' && url_string[pos + 1] == '/') {
        pos += 2;  // Skip "//"

        // 3. Parse authority (userinfo@host:port)
        // The authority ends at the next '/', '?', or '#'.
        std::size_t auth_end = pos;
        while (auth_end < url_string.size() &&
               url_string[auth_end] != '/' && url_string[auth_end] != '?' && url_string[auth_end] != '#') {
            auth_end++;
        }

        std::string authority = url_string.substr(pos, auth_end - pos);

        // Parse userinfo (if present)
        // TEACHING NOTE: Userinfo is rarely used in modern web URLs.
        // It looks like: http://user:pass@example.com/. It is mainly
        // used in FTP URLs. We parse it but it does not affect the
        // connection.
        std::size_t at = authority.find('@');
        if (at != std::string::npos) {
            userinfo_ = authority.substr(0, at);
            host_ = authority.substr(at + 1);
        } else {
            host_ = authority;
        }

        // Parse port (if present)
        // TEACHING NOTE: The port comes after the host, separated by
        // a colon. For example: example.com:8080. IPv6 addresses are
        // enclosed in brackets: [::1]:8080.
        std::size_t host_end = host_.rfind(':');
        // Check for IPv6 address (host is like [::1])
        if (!host_.empty() && host_[0] == '[') {
            // IPv6: find closing bracket
            std::size_t bracket = host_.find(']');
            if (bracket != std::string::npos) {
                if (bracket + 1 < host_.size() && host_[bracket + 1] == ':') {
                    std::string port_str = host_.substr(bracket + 2);
                    auto p = parse_port(port_str);
                    if (p) port_ = p;
                    host_ = host_.substr(0, bracket + 1);
                }
            }
        } else if (host_end != std::string::npos) {
            // Regular host:port
            std::string port_str = host_.substr(host_end + 1);
            auto p = parse_port(port_str);
            if (p) {
                port_ = p;
                host_ = host_.substr(0, host_end);
            }
        }

        // Lowercase the host (hostnames are case-insensitive)
        // TEACHING NOTE: Domain names are case-insensitive. DNS lookups
        // normalize to lowercase. The WHATWG URL spec mandates lowercasing.
        std::transform(host_.begin(), host_.end(), host_.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        pos = auth_end;
    }

    // 4. Parse path
    // TEACHING NOTE: The path identifies a specific resource on the server.
    // For example, /index.html, /images/photo.png, /api/users/123.
    // An empty path means the root (/). Servers typically treat empty
    // paths as "/".
    std::size_t path_end = pos;
    while (path_end < url_string.size() &&
           url_string[path_end] != '?' && url_string[path_end] != '#') {
        path_end++;
    }
    path_ = url_string.substr(pos, path_end - pos);
    pos = path_end;

    // 5. Parse query (after '?')
    // TEACHING NOTE: The query string is used for passing parameters
    // to the server. For example: ?key=value&foo=bar. Browsers use it
    // for form submissions (GET method) and API calls.
    if (pos < url_string.size() && url_string[pos] == '?') {
        pos++;  // Skip '?'
        std::size_t query_end = pos;
        while (query_end < url_string.size() && url_string[query_end] != '#') {
            query_end++;
        }
        query_ = url_string.substr(pos, query_end - pos);
        pos = query_end;
    }

    // 6. Parse fragment (after '#')
    // TEACHING NOTE: The fragment identifies a specific part of the
    // resource. For example, #section1 jumps to an element with id
    // "section1". Fragments are NOT sent to the server - they are
    // processed entirely by the browser. This is why changing the
    // fragment does not trigger a page reload.
    if (pos < url_string.size() && url_string[pos] == '#') {
        pos++;  // Skip '#'
        fragment_ = url_string.substr(pos);
    }

    // If path is empty, default to "/"
    if (path_.empty() && !host_.empty()) {
        path_ = "/";
    }

    valid_ = !scheme_.empty();
}

std::optional<std::uint16_t> Url::parse_port(const std::string& str) {
    // TEACHING NOTE: We parse the port as a number. Valid ports are
    // 0-65535. We use optional to distinguish "no port" from "port 0".
    if (str.empty()) return std::nullopt;
    try {
        unsigned long val = std::stoul(str);
        if (val > 65535) return std::nullopt;
        return static_cast<std::uint16_t>(val);
    } catch (...) {
        return std::nullopt;
    }
}

std::string Url::to_string() const {
    // TEACHING NOTE: Reconstruct the URL from components. We omit the
    // port if it is the default for the scheme (normalization). This
    // is how browsers normalize URLs.
    std::ostringstream out;

    out << scheme_ << "://";

    if (!userinfo_.empty()) {
        out << userinfo_ << "@";
    }

    out << host_;

    if (port_.has_value() && port_.value() != default_port()) {
        out << ":" << port_.value();
    }

    out << path_;

    if (!query_.empty()) {
        out << "?" << query_;
    }

    if (!fragment_.empty()) {
        out << "#" << fragment_;
    }

    return out.str();
}

std::string Url::percent_encode(const std::string& input) {
    // TEACHING NOTE: Percent encoding converts characters that are not
    // allowed in URLs to %XX format. For example, space becomes %20.
    // The set of unreserved characters (not needing encoding) is:
    //   A-Z a-z 0-9 - . _ ~
    // Everything else should be encoded in the path and query.
    //
    // Real browsers have complex encoding rules that differ per URL
    // component. For example, the path allows more unencoded characters
    // than the query. We use a simple approach: encode everything except
    // unreserved characters.

    std::string result;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            result += static_cast<char>(c);
        } else {
            // Format as %XX (hexadecimal)
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

std::string Url::percent_decode(const std::string& input) {
    // TEACHING NOTE: Percent decoding converts %XX back to the original
    // character. For example, %20 becomes space, %2F becomes /.
    std::string result;
    for (std::size_t i = 0; i < input.size(); i++) {
        if (input[i] == '%' && i + 2 < input.size()) {
            // Parse two hex digits
            std::string hex = input.substr(i + 1, 2);
            try {
                unsigned int val = std::stoul(hex, nullptr, 16);
                result += static_cast<char>(val);
                i += 2;
            } catch (...) {
                result += input[i];
            }
        } else {
            result += input[i];
        }
    }
    return result;
}

Url Url::resolve(const std::string& relative) const {
    // TEACHING NOTE: Relative URL resolution (RFC 3986 Section 5.3).
    // When a page at "http://example.com/foo/bar" has a link to "baz",
    // the browser resolves it to "http://example.com/foo/baz".
    // Rules:
    //   - If relative starts with a scheme, it is absolute - use it directly
    //   - If relative starts with "//", use same scheme + new authority
    //   - If relative starts with "/", use same authority + new path
    //   - If relative starts with "?", use same authority + path + new query
    //   - If relative starts with "#", use same URL + new fragment
    //   - Otherwise, merge paths (remove last segment of base path, append relative)

    // Check if relative is actually absolute
    std::size_t colon = relative.find(':');
    if (colon != std::string::npos) {
        std::string scheme = relative.substr(0, colon);
        // Check if it looks like a scheme (starts with letter, all alpha/digit/+/-/.)
        bool is_scheme = std::isalpha(static_cast<unsigned char>(scheme[0]));
        for (char c : scheme) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
                is_scheme = false;
                break;
            }
        }
        if (is_scheme) {
            return Url(relative);
        }
    }

    // Relative URL resolution
    if (relative.size() >= 2 && relative[0] == '/' && relative[1] == '/') {
        // Same scheme, new authority
        return Url(scheme_ + ":" + relative);
    }

    if (!relative.empty() && relative[0] == '/') {
        // Same authority, new absolute path
        std::string url = scheme_ + "://" + host_;
        if (port_.has_value() && port_.value() != default_port()) {
            url += ":" + std::to_string(port_.value());
        }
        url += relative;
        return Url(url);
    }

    if (!relative.empty() && relative[0] == '?') {
        // Same authority and path, new query
        std::string url = scheme_ + "://" + host_;
        if (port_.has_value() && port_.value() != default_port()) {
            url += ":" + std::to_string(port_.value());
        }
        url += path_ + relative;
        return Url(url);
    }

    if (!relative.empty() && relative[0] == '#') {
        // Same everything, new fragment
        std::string url = to_string();
        std::size_t hash = url.find('#');
        if (hash != std::string::npos) {
            url = url.substr(0, hash);
        }
        url += relative;
        return Url(url);
    }

    // Merge paths: remove last segment of base path, append relative
    std::string base_path = path_;
    std::size_t last_slash = base_path.rfind('/');
    if (last_slash != std::string::npos) {
        base_path = base_path.substr(0, last_slash + 1);
    } else {
        base_path = "/";
    }

    std::string url = scheme_ + "://" + host_;
    if (port_.has_value() && port_.value() != default_port()) {
        url += ":" + std::to_string(port_.value());
    }
    url += base_path + relative;
    return Url(url);
}

} // namespace chinstrap