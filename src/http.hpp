// =========================================================================
// http.hpp - HTTP/1.1 Client over Raw POSIX Sockets
// =========================================================================
// TEACHING NOTE: This is where the browser meets the network. When a user
// types a URL, the browser:
//   1. Parses the URL (our url.hpp)
//   2. Resolves the hostname to an IP address (DNS - we use getaddrinfo,
//      which is part of POSIX, not a third-party library)
//   3. Opens a TCP connection (socket + connect)
//   4. Sends an HTTP request (send)
//   5. Reads the response (recv)
//   6. Parses the HTTP response (status line, headers, body)
//
// Real browsers like Chrome use much more sophisticated networking:
//   - Connection pooling (reuse connections across requests)
//   - HTTP/2 and HTTP/3 (multiplexed streams over a single connection)
//   - TLS for HTTPS (we do not implement TLS; it requires a crypto
//     library, which would break our zero-dependency rule)
//   - DNS prefetching and caching
//   - Cookie management
//   - Content negotiation
//   - Redirect following with cycle detection
//
// We implement HTTP/1.1 with:
//   - GET and POST methods
//   - Chunked transfer encoding (for streaming responses)
//   - Content-Length for fixed-length responses
//   - Connection: keep-alive support (we keep the socket open)
//   - Basic redirect following
//   - Custom headers
//
// What we do NOT implement:
//   - TLS (no OpenSSL, no mbedTLS, no GnuTLS)
//   - HTTP/2 or HTTP/3
//   - Compression (no gzip, no brotli - would need decompression libraries)
//   - Multipart bodies
//   - WebSocket
//
// TEACHING NOTE: getaddrinfo() is a POSIX function for DNS resolution.
// It is defined in <netdb.h> and is part of the POSIX standard, not a
// third-party library. We use it because implementing DNS from scratch
// (parsing /etc/resolv.conf, sending UDP queries, parsing DNS packets)
// would be an entire project on its own. DNS is fascinating but is not
// the focus of Chinstrap. The POSIX socket API (socket, connect, send,
// recv, close) is also part of POSIX, not a library.
// =========================================================================

#ifndef CHINSTRAP_HTTP_HPP
#define CHINSTRAP_HTTP_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace chinstrap {

// -------------------------------------------------------------------------
// HttpResponse - Parsed HTTP response
// -------------------------------------------------------------------------
// TEACHING NOTE: An HTTP response has three parts:
//   1. Status line: "HTTP/1.1 200 OK\r\n"
//   2. Headers: "Content-Type: text/html\r\nContent-Length: 1234\r\n"
//   3. Blank line: "\r\n" (separates headers from body)
//   4. Body: the actual content (HTML, CSS, images, etc.)
//
// The headers are key-value pairs. We store them in a map for easy lookup.
// Some headers can appear multiple times (like Set-Cookie), but we keep
// only the last value for simplicity. Real browsers store multi-valued
// headers differently.
// -------------------------------------------------------------------------

struct HttpResponse {
    int status_code = 0;        // 200, 404, 500, etc.
    std::string status_text;    // "OK", "Not Found", etc.
    std::string http_version;   // "HTTP/1.1"
    std::map<std::string, std::string> headers;
    std::string body;           // Response body (decompressed, decoded)

    // Convenience accessors
    std::string header(const std::string& name) const;
    bool is_success() const { return status_code >= 200 && status_code < 300; }
    bool is_redirect() const { return status_code >= 300 && status_code < 400; }
    std::string content_type() const { return header("Content-Type"); }
    std::size_t content_length() const;
};

// -------------------------------------------------------------------------
// HttpRequest - Builder for HTTP requests
// -------------------------------------------------------------------------
// TEACHING NOTE: An HTTP request has the same structure as a response:
//   1. Request line: "GET /path HTTP/1.1\r\n"
//   2. Headers: "Host: example.com\r\nUser-Agent: Chinstrap/0.1\r\n"
//   3. Blank line: "\r\n"
//   4. Optional body (for POST)
//
// The Host header is mandatory in HTTP/1.1. Without it, the server
// does not know which virtual host to serve (many websites share the
// same IP address).
// -------------------------------------------------------------------------

struct HttpRequest {
    std::string method = "GET";     // "GET" or "POST"
    std::string path = "/";        // Path component of the URL
    std::string host;              // Host header value
    std::uint16_t port = 80;       // Port to connect to
    std::map<std::string, std::string> headers;  // Custom headers
    std::string body;              // Request body (for POST)

    // Build the raw HTTP request string
    // TEACHING NOTE: This is what actually goes over the wire.
    // We construct it as a string and send it with the send() syscall.
    std::string to_raw_string() const;

    // Set a header (overwrites existing)
    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }
};

// -------------------------------------------------------------------------
// HttpClient - Manages HTTP connections
// -------------------------------------------------------------------------
// TEACHING NOTE: This class manages a TCP connection to a server. In a
// real browser, the HTTP client is part of the network stack, which
// manages connection pools, proxy connections, cache, and more.
// We keep it simple: one connection per request (for now). The socket
// file descriptor is stored and reused if keep-alive is negotiated.
//
// The HTTP/1.1 keep-alive mechanism lets us reuse a TCP connection for
// multiple requests. This is important for performance: TCP handshake
// and DNS lookup take time. Chrome takes this further with connection
// pooling (up to 6 connections per origin) and HTTP/2 multiplexing
// (many streams over one connection).
// -------------------------------------------------------------------------

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Disable copy (we own a socket fd)
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Send an HTTP request and return the response.
    // Follows up to max_redirects redirects (default 5).
    // Throws std::runtime_error on network errors.
    HttpResponse send(const HttpRequest& request, int max_redirects = 5);

    // Convenience: simple GET request
    HttpResponse get(const std::string& host, std::uint16_t port,
                     const std::string& path);

    // Convenience: simple POST request
    HttpResponse post(const std::string& host, std::uint16_t port,
                      const std::string& path, const std::string& body,
                      const std::string& content_type = "application/x-www-form-urlencoded");

private:
    int socket_fd_ = -1;  // TCP socket file descriptor (-1 = not connected)
    std::string connected_host_;
    std::uint16_t connected_port_ = 0;

    // Internal: open a TCP connection to host:port
    bool connect(const std::string& host, std::uint16_t port);

    // Internal: send raw bytes over the socket
    bool send_raw(const std::string& data);

    // Internal: read the full HTTP response from the socket
    std::string read_response();

    // Internal: parse a raw HTTP response string into HttpResponse
    static HttpResponse parse_response(const std::string& raw);

    // Internal: decode chunked transfer encoding
    // TEACHING NOTE: When a server uses "Transfer-Encoding: chunked",
    // the body is sent as a series of chunks, each prefixed by its
    // size in hex. The end is marked by a zero-length chunk. This
    // allows the server to stream data without knowing the total
    // size in advance. The format is:
    //   <hex-size>\r\n<data>\r\n<hex-size>\r\n<data>\r\n0\r\n\r\n
    static std::string decode_chunked(const std::string& body);

    // Internal: close the socket
    void close();

    // Internal: check if we have an alive keep-alive connection to this host
    bool is_connected_to(const std::string& host, std::uint16_t port) const;
};

} // namespace chinstrap

#endif // CHINSTRAP_HTTP_HPP