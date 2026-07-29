// =========================================================================
// http.cpp - HTTP/1.1 Client Implementation
// =========================================================================
// TEACHING NOTE: This file implements raw HTTP over POSIX sockets.
// When you read this code, you are seeing exactly what happens between
// your browser and the web server. There is no library hiding the
// details. Every send() and recv() call is visible.
//
// The flow is:
//   1. DNS lookup (getaddrinfo) - resolve "example.com" to an IP
//   2. Create socket (socket) - get a file descriptor for TCP
//   3. Connect (connect) - establish TCP connection to the server
//   4. Send request (send) - write the HTTP request bytes
//   5. Read response (recv) - read the HTTP response bytes
//   6. Parse response - split status line, headers, body
//   7. Handle chunked encoding if present
//   8. Close or keep-alive the connection
//
// The POSIX socket API is the foundation of all network programming in C.
// It was designed in the 1980s and has not changed much since. Every
// networking library (curl, libuv, Boost.Asio) is built on top of these
// same system calls. Understanding them is understanding networking.
// =========================================================================

#include "http.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace chinstrap {

// =========================================================================
// HttpRequest implementation
// =========================================================================

std::string HttpRequest::to_raw_string() const {
    // TEACHING NOTE: This builds the exact bytes that go over the network.
    // HTTP/1.1 lines are terminated with CRLF (\r\n), not just LF.
    // This is a historical artifact from the 1980s when HTTP was
    // designed to work on systems that used CRLF line endings (like
    // the early internet protocols it was modeled after: SMTP, FTP).
    //
    // A minimal HTTP/1.1 request looks like:
    //   GET /path HTTP/1.1\r\n
    //   Host: example.com\r\n
    //   \r\n
    //
    // The Host header is REQUIRED in HTTP/1.1. Without it, virtual
    // hosting (multiple websites on one IP) would not work. The server
    // needs to know which site you want.

    std::ostringstream raw;

    // Request line: METHOD PATH HTTP/1.1
    raw << method << " " << path << " HTTP/1.1\r\n";

    // Mandatory Host header (always first, as servers expect it)
    raw << "Host: " << host;
    if (port != 80 && port != 443) {
        raw << ":" << port;
    }
    raw << "\r\n";

    // User-Agent so servers know who we are
    // TEACHING NOTE: Many servers check User-Agent to decide what to
    // send. Some block unknown agents. We identify ourselves honestly.
    raw << "User-Agent: Chinstrap/0.1\r\n";
    raw << "Accept: text/html,application/xhtml+xml,text/css,*/*\r\n";
    raw << "Accept-Encoding: identity\r\n";  // No compression (we cannot decompress)
    raw << "Connection: close\r\n";           // Simple: no keep-alive for now

    // Custom headers
    for (const auto& [name, value] : headers) {
        raw << name << ": " << value << "\r\n";
    }

    // Content-Length for POST bodies
    if (!body.empty()) {
        raw << "Content-Length: " << body.size() << "\r\n";
    }

    // End of headers
    raw << "\r\n";

    // Body (if any)
    raw << body;

    return raw.str();
}

// =========================================================================
// HttpResponse implementation
// =========================================================================

std::string HttpResponse::header(const std::string& name) const {
    // TEACHING NOTE: HTTP header names are case-insensitive. A server
    // might send "Content-Type" or "content-type" or "CONTENT-TYPE".
    // Real browsers normalize header names to lowercase for lookup.
    // We do a case-insensitive search here.
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    for (const auto& [key, value] : headers) {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if (lower_key == lower_name) {
            return value;
        }
    }
    return "";
}

std::size_t HttpResponse::content_length() const {
    // TEACHING NOTE: Content-Length tells the client how many bytes the
    // body has. If the server uses chunked encoding, there is no
    // Content-Length header, and the body size is determined by reading
    // chunks until a zero-length chunk is received.
    std::string cl = header("Content-Length");
    if (cl.empty()) return 0;
    try {
        return static_cast<std::size_t>(std::stoul(cl));
    } catch (...) {
        return 0;
    }
}

// =========================================================================
// HttpClient implementation
// =========================================================================

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() {
    close();
}

bool HttpClient::connect(const std::string& host, std::uint16_t port) {
    // TEACHING NOTE: This is a raw TCP connection using the POSIX socket
    // API. The steps are:
    //
    //   1. getaddrinfo() - Resolve hostname to an IP address.
    //      This does DNS lookup. We pass AI_NUMERICHOST hint to allow
    //      both names and IPs. getaddrinfo is better than gethostbyname
    //      because it supports IPv6 and gives us a linked list of results.
    //
    //   2. socket() - Create a TCP socket (SOCK_STREAM).
    //      This returns a file descriptor (an integer) that we use for
    //      all subsequent operations (send, recv, close).
    //
    //   3. connect() - Establish the TCP connection.
    //      This does the TCP three-way handshake (SYN, SYN-ACK, ACK).
    //      If the server is unreachable, this fails with an error.
    //
    // We iterate through all addresses returned by getaddrinfo in case
    // the first one does not work (e.g., IPv6 address but no IPv6 route).

    if (is_connected_to(host, port)) {
        return true;  // Already connected
    }

    close();  // Close any existing connection

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);

    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        // getaddrinfo returns error codes as strings via gai_strerror
        return false;
    }

    // Try each address until one connects
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        socket_fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd_ < 0) {
            continue;  // Try next address
        }

        // Set a receive timeout so we do not hang forever
        // TEACHING NOTE: SO_RCVTIMEO sets a timeout on recv(). If the
        // server does not send data within 30 seconds, recv() returns
        // with an error. Real browsers have much more sophisticated
        // timeout handling (stalled connections, slow start, etc.).
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (::connect(socket_fd_, rp->ai_addr, rp->ai_addrlen) == 0) {
            // Success!
            connected_host_ = host;
            connected_port_ = port;
            break;
        }

        // Failed, close and try next
        ::close(socket_fd_);
        socket_fd_ = -1;
    }

    freeaddrinfo(result);
    return socket_fd_ >= 0;
}

bool HttpClient::send_raw(const std::string& data) {
    // TEACHING NOTE: The send() system call may not send all the data
    // in one call. On Linux, send() might return a value less than the
    // total length, meaning only part of the data was sent. We need to
    // loop until all data is sent. This is a common pattern in network
    // programming. Real browsers use non-blocking I/O with event loops
    // (epoll on Linux, kqueue on BSD) for efficiency, but blocking I/O
    // is fine for a single request.

    if (socket_fd_ < 0) return false;

    std::size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::send(socket_fd_, data.data() + total_sent,
                              data.size() - total_sent, 0);
        if (sent < 0) {
            return false;  // Error
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string HttpClient::read_response() {
    // TEACHING NOTE: We read from the socket in chunks (4KB at a time).
    // recv() returns whatever data has arrived, which may be less than
    // the buffer size. We keep reading until we have the complete response.
    //
    // For HTTP, we know we have the complete response when:
    //   - The connection is closed (Connection: close), OR
    //   - We have read Content-Length bytes after the headers, OR
    //   - We have received the terminating zero-length chunk (chunked)
    //
    // For simplicity, we read until the connection is closed. This works
    // because we send "Connection: close" in our requests. A production
    // browser would handle keep-alive and chunked encoding during reading,
    // but for teaching, reading until close is simplest.

    std::string response;
    char buffer[4096];

    while (true) {
        ssize_t n = ::recv(socket_fd_, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;  // Connection closed or error
        }
        response.append(buffer, static_cast<std::size_t>(n));
    }

    return response;
}

HttpResponse HttpClient::parse_response(const std::string& raw) {
    // TEACHING NOTE: HTTP responses are text-based (well, mostly).
    // The format is:
    //   HTTP/1.1 200 OK\r\n
    //   Header: Value\r\n
    //   Header: Value\r\n
    //   \r\n
    //   <body bytes>
    //
    // We parse this by finding \r\n delimiters. The first line is the
    // status line. Subsequent lines are headers until we hit an empty
    // line (just \r\n). Everything after that is the body.
    //
    // In practice, HTTP responses can be binary (images, video, etc.).
    // We store the body as a std::string, which can hold arbitrary bytes,
    // but this is not ideal for binary data. A real browser would use
    // a byte buffer type. For our educational browser, we mostly deal
    // with text (HTML, CSS), so std::string is fine.

    HttpResponse response;
    std::size_t pos = 0;

    // Find the end of the status line
    std::size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) {
        return response;  // Malformed
    }

    // Parse status line: HTTP/1.1 200 OK
    std::string status_line = raw.substr(pos, line_end - pos);
    // TEACHING NOTE: We use string streams for parsing. This is standard
    // C++ and works well for simple tokenization.
    {
        std::istringstream ss(status_line);
        ss >> response.http_version >> response.status_code >> response.status_text;
    }
    pos = line_end + 2;  // Skip \r\n

    // Parse headers
    // TEACHING NOTE: Each header line is "Name: Value". The space after
    // the colon is optional but conventional. We split on the first colon.
    while (pos < raw.size()) {
        line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) {
            break;
        }

        // Empty line = end of headers
        if (line_end == pos) {
            pos += 2;  // Skip the empty line
            break;
        }

        std::string header_line = raw.substr(pos, line_end - pos);
        std::size_t colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string name = header_line.substr(0, colon);
            // Skip the colon and any spaces/tabs after it
            std::size_t value_start = colon + 1;
            while (value_start < header_line.size() &&
                   (header_line[value_start] == ' ' || header_line[value_start] == '\t')) {
                value_start++;
            }
            std::string value = header_line.substr(value_start);
            response.headers[name] = value;
        }

        pos = line_end + 2;
    }

    // The rest is the body
    if (pos < raw.size()) {
        response.body = raw.substr(pos);
    }

    // Handle chunked transfer encoding
    // TEACHING NOTE: When the server sends "Transfer-Encoding: chunked",
    // the body is encoded as chunks. Each chunk is:
    //   <hex-size>\r\n<chunk-data>\r\n
    // The last chunk has size 0:
    //   0\r\n\r\n
    // We decode this into a single contiguous body. This encoding is
    // used when the server does not know the total content length in
    // advance (e.g., dynamically generated pages).
    std::string te = response.header("Transfer-Encoding");
    std::string te_lower = te;
    std::transform(te_lower.begin(), te_lower.end(), te_lower.begin(), ::tolower);
    if (te_lower.find("chunked") != std::string::npos) {
        response.body = decode_chunked(response.body);
    }

    return response;
}

std::string HttpClient::decode_chunked(const std::string& body) {
    // TEACHING NOTE: Chunked transfer encoding is defined in HTTP/1.1
    // (RFC 7230 Section 4.1). The body looks like:
    //
    //   4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n
    //
    // Which decodes to: "Wikipedia"
    //
    // Each chunk starts with a hex number indicating the chunk size,
    // followed by \r\n, then the chunk data, then \r\n. The end is
    // marked by a zero-size chunk (0\r\n\r\n).
    //
    // We parse this manually. Real browsers handle this in their network
    // stack with streaming, so they can start parsing HTML before the
    // full response arrives. We wait for the full response for simplicity.

    std::string result;
    std::size_t pos = 0;

    while (pos < body.size()) {
        // Find the chunk size line
        std::size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos) break;

        std::string size_str = body.substr(pos, line_end - pos);
        // Remove any chunk extensions (after a semicolon)
        std::size_t semi = size_str.find(';');
        if (semi != std::string::npos) {
            size_str = size_str.substr(0, semi);
        }

        // Parse hex size
        // TEACHING NOTE: We use strtoul with base 16 to parse the hex
        // chunk size. This is the standard C way to parse hex numbers.
        unsigned long chunk_size = 0;
        try {
            chunk_size = std::stoul(size_str, nullptr, 16);
        } catch (...) {
            break;  // Parse error
        }

        if (chunk_size == 0) {
            break;  // End of chunks
        }

        pos = line_end + 2;  // Skip \r\n after size

        // Read chunk data
        if (pos + chunk_size > body.size()) {
            // Truncated chunk
            result.append(body, pos, body.size() - pos);
            break;
        }

        result.append(body, pos, chunk_size);
        pos += chunk_size;

        // Skip trailing \r\n after chunk data
        if (pos + 1 < body.size() && body[pos] == '\r' && body[pos + 1] == '\n') {
            pos += 2;
        }
    }

    return result;
}

bool HttpClient::is_connected_to(const std::string& host, std::uint16_t port) const {
    return socket_fd_ >= 0 && connected_host_ == host && connected_port_ == port;
}

void HttpClient::close() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_host_.clear();
    connected_port_ = 0;
}

HttpResponse HttpClient::send(const HttpRequest& request, int max_redirects) {
    // TEACHING NOTE: This is the main entry point for making HTTP requests.
    // It handles connection, sending, receiving, and redirect following.
    //
    // Redirects are important: when a server responds with 301 or 302,
    // it includes a Location header with the new URL. The browser must
    // follow that URL to get the actual content. Real browsers limit
    // redirects to prevent infinite loops (Chrome uses 20, we use 5).
    //
    // HTTP status codes for redirects:
    //   301 - Moved Permanently
    //   302 - Found (temporary redirect)
    //   303 - See Other (redirect to GET)
    //   307 - Temporary Redirect (preserve method)
    //   308 - Permanent Redirect (preserve method)

    HttpRequest current = request;
    HttpResponse response;

    for (int i = 0; i <= max_redirects; ++i) {
        // Connect to the server
        if (!connect(current.host, current.port)) {
            throw std::runtime_error("Failed to connect to " + current.host);
        }

        // Send the request
        std::string raw_request = current.to_raw_string();
        if (!send_raw(raw_request)) {
            close();
            throw std::runtime_error("Failed to send request to " + current.host);
        }

        // NOTE: We do NOT call shutdown(SHUT_WR) here because some servers
        // close the connection immediately upon receiving FIN, before
        // sending their response. Instead, we rely on Connection: close
        // in the request headers to tell the server to close after responding.

        // Read the response
        std::string raw_response = read_response();
        close();

        // Parse the response
        response = parse_response(raw_response);

        // Check for redirect
        if (response.is_redirect() && i < max_redirects) {
            std::string location = response.header("Location");
            if (!location.empty()) {
                // Handle relative redirect URLs
                if (location.substr(0, 7) == "http://" || location.substr(0, 8) == "https://") {
                    // Absolute URL - reparse
                    // TEACHING NOTE: For simplicity, we only handle
                    // absolute redirect URLs. Relative redirects would
                    // require URL resolution against the current URL.
                    // We could use our Url class for this, but keep it
                    // simple for now.
                    // Parse host and path from the absolute URL
                    std::string scheme;
                    std::string rest;
                    auto scheme_end = location.find("://");
                    if (scheme_end != std::string::npos) {
                        scheme = location.substr(0, scheme_end);
                        rest = location.substr(scheme_end + 3);
                    } else {
                        rest = location;
                    }

                    std::string host_part;
                    std::string path = "/";
                    auto slash = rest.find('/');
                    if (slash != std::string::npos) {
                        host_part = rest.substr(0, slash);
                        path = rest.substr(slash);
                    } else {
                        host_part = rest;
                    }

                    current.host = host_part;
                    current.path = path;
                    current.port = 80;  // HTTP only
                    continue;
                }
            }
        }

        break;  // Not a redirect or max reached
    }

    return response;
}

HttpResponse HttpClient::get(const std::string& host, std::uint16_t port,
                             const std::string& path) {
    HttpRequest req;
    req.method = "GET";
    req.host = host;
    req.port = port;
    req.path = path;
    return send(req);
}

HttpResponse HttpClient::post(const std::string& host, std::uint16_t port,
                              const std::string& path, const std::string& body,
                              const std::string& content_type) {
    HttpRequest req;
    req.method = "POST";
    req.host = host;
    req.port = port;
    req.path = path;
    req.body = body;
    req.set_header("Content-Type", content_type);
    return send(req);
}

} // namespace chinstrap