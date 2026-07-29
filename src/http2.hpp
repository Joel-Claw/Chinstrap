// http2.hpp - HTTP/2 client implementation from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: What is HTTP/2 and why does a browser need it?
//
// HTTP/2 (RFC 9113, published 2022 as the latest revision; originally RFC 7540 in 2015)
// is the second major version of the HTTP protocol. It was designed to address the
// performance problems of HTTP/1.1:
//
// 1. Head-of-line blocking: In HTTP/1.1, if a browser opens 6 connections to a server
//    (the typical limit), each connection can only handle one request at a time. If a
//    large response blocks one connection, all subsequent requests on that connection
//    must wait. HTTP/2 allows multiple concurrent requests (streams) on a single
//    connection.
//
// 2. Header overhead: HTTP/1.1 headers are sent as plain text on every request, often
//    repeating the same headers (User-Agent, Accept, Cookie, etc.). HTTP/2 uses HPACK
//    compression (RFC 7541) to compress headers, using a static table of common headers,
//    a dynamic table that learns from previous requests, and Huffman coding.
//
// 3. Connection overhead: HTTP/1.1 requires multiple TCP connections (typically 6 per
//    origin) to parallelize requests. Each connection has TCP handshake + TLS handshake
//    overhead. HTTP/2 uses a single connection per origin with multiplexed streams.
//
// Key features of HTTP/2:
// - Binary framing layer: All communication is in binary frames (not text like HTTP/1.1)
// - Stream multiplexing: Multiple concurrent request/response streams on one connection
// - Flow control: Per-stream and per-connection flow control (similar to TCP)
// - Server push: Server can proactively send resources (deprecated in later specs but
//   still implemented by many servers)
// - Header compression (HPACK): Reduces header overhead significantly
//
// HTTP/2 is typically negotiated via ALPN (Application-Layer Protocol Negotiation) during
// the TLS handshake. The client offers "h2" and the server selects it. HTTP/2 can also
// run over cleartext (h2c) using the Upgrade header, but this is rare in practice.
//
// TEACHING NOTE: Why we implement HTTP/2 from scratch
//
// Most HTTP/2 implementations use libraries like nghttp2, hyper, or okhttp. We implement
// it from scratch to understand the protocol deeply. The key challenges are:
// 1. HPACK: Implementing Huffman coding, static and dynamic tables
// 2. Frame protocol: Parsing and generating binary frames
// 3. Stream management: Tracking stream states, flow control, priorities
// 4. Connection management: Preface, settings negotiation, goaway handling
//
// Chrome maintains a connection pool of HTTP/2 connections. Once a connection to an
// origin is established, it is reused for all requests to that origin. This is called
// "connection coalescing". Chrome also implements "happy eyeballs" to prefer the fastest
// connection (IPv4 or IPv6) and can race connections.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>
#include <functional>

namespace chinstrap {

// TEACHING NOTE: HTTP/2 frame types (RFC 9113, Section 6)
//
// HTTP/2 communication consists of binary frames. Each frame has a 9-byte header:
//   - Length (3 bytes): payload length (max 2^24 - 1 = 16,777,215 bytes)
//   - Type (1 byte): frame type
//   - Flags (1 byte): type-specific flags
//   - Reserved (1 bit): must be 0
//   - Stream Identifier (31 bits): stream ID (0 for connection-level frames)
//
// Frame types:
//   0x00 DATA:          carries request/response body data
//   0x01 HEADERS:       carries compressed header block (HPACK)
//   0x02 PRIORITY:      stream priority (deprecated in later specs but still sent)
//   0x03 RST_STREAM:    reset a stream (error or cancellation)
//   0x04 SETTINGS:      connection-level settings (max concurrent streams, etc.)
//   0x05 PUSH_PROMISE:  server push promise
//   0x06 PING:          keepalive and RTT measurement
//   0x07 GOAWAY:        graceful connection shutdown
//   0x08 WINDOW_UPDATE: flow control credit update
//   0x09 CONTINUATION:  continuation of a HEADERS frame (for large header blocks)

enum class FrameType : uint8_t {
    DATA          = 0x00,
    HEADERS       = 0x01,
    PRIORITY      = 0x02,
    RST_STREAM    = 0x03,
    SETTINGS      = 0x04,
    PUSH_PROMISE  = 0x05,
    PING          = 0x06,
    GOAWAY        = 0x07,
    WINDOW_UPDATE = 0x08,
    CONTINUATION  = 0x09,
};

// Frame flags (common across frame types)
namespace FrameFlags {
    constexpr uint8_t END_STREAM  = 0x01;  // Last frame for this stream
    constexpr uint8_t ACK         = 0x01;  // Ack for SETTINGS and PING
    constexpr uint8_t END_HEADERS = 0x04;  // Last HEADERS/CONTINUATION frame
    constexpr uint8_t PADDED      = 0x08;  // Frame has padding
    constexpr uint8_t PRIORITY   = 0x20;  // HEADERS frame has priority info
}

// TEACHING NOTE: HTTP/2 error codes (RFC 9113, Section 7)
enum class ErrorCode : uint32_t {
    NO_ERROR            = 0x0,
    PROTOCOL_ERROR      = 0x1,
    INTERNAL_ERROR      = 0x2,
    FLOW_CONTROL_ERROR  = 0x3,
    SETTINGS_TIMEOUT    = 0x4,
    STREAM_CLOSED       = 0x5,
    FRAME_SIZE_ERROR    = 0x6,
    REFUSED_STREAM      = 0x7,
    CANCEL              = 0x8,
    COMPRESSION_ERROR   = 0x9,
    CONNECT_ERROR       = 0xa,
    ENHANCE_YOUR_CALM   = 0xb,
    INADEQUATE_SECURITY = 0xc,
    HTTP_1_1_REQUIRED   = 0xd,
};

// TEACHING NOTE: HTTP/2 stream states (RFC 9113, Section 5.1)
//
// Streams have a lifecycle:
//   idle -> open (after sending HEADERS)
//   open -> half-closed (local) (after sending END_STREAM)
//   open -> half-closed (remote) (after receiving END_STREAM)
//   half-closed (local) -> closed (after receiving END_STREAM)
//   half-closed (remote) -> closed (after sending END_STREAM)
//   open/half-closed -> closed (after RST_STREAM)
//   idle -> reserved (push) (after PUSH_PROMISE)
//
// Client-initiated streams use odd numbers (1, 3, 5, ...).
// Server-initiated streams (push) use even numbers (2, 4, 6, ...).
// Stream 0 is the connection itself (for SETTINGS, PING, GOAWAY).

enum class StreamState {
    IDLE,
    RESERVED_LOCAL,
    RESERVED_REMOTE,
    OPEN,
    HALF_CLOSED_LOCAL,
    HALF_CLOSED_REMOTE,
    CLOSED,
};

// A single HTTP/2 frame.
struct Http2Frame {
    FrameType type;
    uint8_t flags;
    uint32_t stream_id;  // 31 bits used
    std::vector<uint8_t> payload;
};

// TEACHING NOTE: HPACK (RFC 7541)
//
// HPACK is the header compression format used by HTTP/2. It has three mechanisms:
//
// 1. Static table: 61 predefined common headers (e.g., ":method GET", ":path /",
//    "content-type", "user-agent", etc.). These can be referenced by index.
//
// 2. Dynamic table: A per-connection table of headers seen in previous requests/
//    responses. New entries are added at index 0 (front) and old entries shift.
//    The table has a configurable maximum size (SETTINGS_HEADER_TABLE_SIZE, default 4096).
//
// 3. Huffman coding: Header names and values can be Huffman-encoded to save space.
//    HPACK defines a specific Huffman code table for common byte values.
//
// A header block is a sequence of "header field representations":
// - Indexed header (1 byte): top bit set, lower 7 bits are table index
// - Literal with incremental indexing: store in dynamic table
// - Literal without indexing: do not store
// - Literal never indexed: do not store and never index (for sensitive headers)
// - Dynamic table size update: change the max table size

// HPACK static table entry
struct HpackHeaderEntry {
    std::string name;
    std::string value;
};

// HPACK dynamic table
class HpackDynamicTable {
public:
    HpackDynamicTable(size_t max_size = 4096);

    // Add a header to the dynamic table (evicts from the back if needed)
    void add(const std::string& name, const std::string& value);

    // Get entry at 1-based index (index 1 is most recently added)
    std::optional<HpackHeaderEntry> get(size_t index) const;

    // Total number of entries
    size_t size() const { return entries_.size(); }

    // Current size in bytes (sum of name + value lengths + 32 bytes overhead per entry)
    size_t current_size() const { return current_size_; }

    // Set maximum table size (may evict entries)
    void set_max_size(size_t max_size);

    size_t max_size() const { return max_size_; }

private:
    std::vector<HpackHeaderEntry> entries_;
    size_t max_size_;
    size_t current_size_;

    void evict();
};

// TEACHING NOTE: Huffman coding in HPACK
//
// HPACK uses a static Huffman code defined in RFC 7541, Appendix B. This is a
// specific code optimized for HTTP headers (frequent characters like lowercase
// letters, digits, hyphens, etc. get short codes).
//
// Huffman encoding replaces each byte with a variable-length bit sequence.
// More common bytes get shorter codes. The EOS (end of string) symbol is
// code 30 bits long and is used for padding the final byte.
//
// For example, the letter 'e' (very common) might be encoded as just 3 bits,
// while a rare byte like 0x00 would be encoded as 13 bits.
//
// We implement both encoding and decoding. The decoder reads bits from the
// input and walks a binary tree (or uses a lookup table) to find the matching
// symbol.

class HpackHuffman {
public:
    // Encode a string using HPACK Huffman coding
    static std::vector<uint8_t> encode(const std::string& input);

    // Decode a Huffman-encoded byte sequence
    static std::string decode(const uint8_t* data, size_t length);

    // Check if a string would benefit from Huffman encoding
    // (shorter than the raw string)
    static bool should_encode(const std::string& input);
};

// HPACK encoder: compresses a list of headers into a binary header block
class HpackEncoder {
public:
    HpackEncoder(size_t max_table_size = 4096);

    // Encode a list of headers
    std::vector<uint8_t> encode(const std::vector<HpackHeaderEntry>& headers);

private:
    HpackDynamicTable dynamic_table_;

    // Find a header in the static or dynamic table
    // Returns: (index, full_match) where index is the table index
    // and full_match is true if both name and value match
    std::pair<size_t, bool> find_header(const std::string& name, const std::string& value);

    // Encode an integer with the given prefix length (in bits)
    static std::vector<uint8_t> encode_integer(uint64_t value, uint8_t prefix_bits, uint8_t prefix_byte);

    // Encode a string (with optional Huffman encoding)
    static std::vector<uint8_t> encode_string(const std::string& str);
};

// HPACK decoder: decompresses a binary header block into headers
class HpackDecoder {
public:
    HpackDecoder(size_t max_table_size = 4096);

    // Decode a header block
    std::vector<HpackHeaderEntry> decode(const uint8_t* data, size_t length);

private:
    HpackDynamicTable dynamic_table_;

    // Decode an integer with the given prefix length
    static uint64_t decode_integer(const uint8_t* data, size_t& offset, uint8_t prefix_bits);

    // Decode a string (may be Huffman-encoded)
    static std::string decode_string(const uint8_t* data, size_t& offset);
};

// TEACHING NOTE: HTTP/2 stream
//
// A stream is a bidirectional sequence of frames between the client and server
// within a single HTTP/2 connection. Each stream has:
// - A unique ID (odd for client-initiated, even for server-push)
// - A state (idle, open, half-closed, closed)
// - Its own flow control window (in addition to the connection-level window)
// - Headers (sent as HEADERS frame, possibly followed by CONTINUATION frames)
// - Body data (sent as DATA frames)
// - Optional trailers (sent as another HEADERS frame with END_STREAM)
struct Http2Stream {
    uint32_t id;
    StreamState state;
    int32_t send_window;    // Flow control window for sending data to server
    int32_t recv_window;    // Flow control window for receiving data from server

    std::vector<HpackHeaderEntry> response_headers;
    std::vector<uint8_t> response_body;
    bool headers_complete;
    bool body_complete;

    // Request data (set by the client before sending)
    std::vector<HpackHeaderEntry> request_headers;
    std::vector<uint8_t> request_body;

    Http2Stream() : id(0), state(StreamState::IDLE),
                    send_window(65535), recv_window(65535),
                    headers_complete(false), body_complete(false) {}
};

// TEACHING NOTE: HTTP/2 connection settings (RFC 9113, Section 6.5.2)
//
// SETTINGS frames negotiate connection parameters. The defined settings are:
// - HEADER_TABLE_SIZE (0x1): Max HPACK dynamic table size (default 4096)
// - ENABLE_PUSH (0x2): Whether server push is allowed (default 1, now often 0)
// - MAX_CONCURRENT_STREAMS (0x3): Max simultaneous streams (default unlimited)
// - INITIAL_WINDOW_SIZE (0x4): Initial flow control window for new streams (default 65535)
// - MAX_FRAME_SIZE (0x5): Max frame payload size (default 16384, max 16777215)
// - MAX_HEADER_LIST_SIZE (0x6): Max size of uncompressed header list (advisory)

enum class SettingsId : uint16_t {
    HEADER_TABLE_SIZE      = 0x1,
    ENABLE_PUSH            = 0x2,
    MAX_CONCURRENT_STREAMS = 0x3,
    INITIAL_WINDOW_SIZE    = 0x4,
    MAX_FRAME_SIZE         = 0x5,
    MAX_HEADER_LIST_SIZE   = 0x6,
};

struct Http2Settings {
    uint32_t header_table_size = 4096;
    bool enable_push = true;
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 0;  // 0 = unlimited
};

// TEACHING NOTE: HTTP/2 connection lifecycle
//
// 1. The client sends the "connection preface": the string
//    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" followed by a SETTINGS frame.
//    This magic string identifies the connection as HTTP/2.
// 2. The server sends its own SETTINGS frame.
// 3. Both sides ACK each others SETTINGS frames.
// 4. The client sends HEADERS frames for its requests.
// 5. The server responds with HEADERS and DATA frames.
// 6. Either side can send GOAWAY to initiate graceful shutdown.
//
// The connection preface is 24 bytes:
//   PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n
// This was chosen to be an invalid HTTP/1.1 request so that a server
// can detect if a client accidentally uses HTTP/1.1 on an HTTP/2 port.

class Http2Connection {
public:
    Http2Connection();
    ~Http2Connection();

    // TEACHING NOTE: The connection preface
    // This magic string is sent by the client as the first thing on the
    // connection. It is designed to be ignored by HTTP/1.1 servers and
    // to clearly identify an HTTP/2 connection.
    static const std::string CONNECTION_PREFACE;

    // Get the connection preface bytes to send
    std::vector<uint8_t> get_preface() const;

    // Get initial settings frame to send
    std::vector<uint8_t> get_initial_settings() const;

    // Build a SETTINGS ACK frame
    static std::vector<uint8_t> build_settings_ack();

    // Build a HEADERS frame for a request
    // stream_id must be odd (client-initiated)
    std::vector<uint8_t> build_headers_frame(
        uint32_t stream_id,
        const std::vector<HpackHeaderEntry>& headers,
        bool end_stream
    );

    // Build a DATA frame
    std::vector<uint8_t> build_data_frame(
        uint32_t stream_id,
        const std::vector<uint8_t>& data,
        bool end_stream
    );

    // Build a RST_STREAM frame
    static std::vector<uint8_t> build_rst_stream(uint32_t stream_id, ErrorCode code);

    // Build a PING frame
    static std::vector<uint8_t> build_ping(uint64_t opaque_data, bool ack = false);

    // Build a GOAWAY frame
    static std::vector<uint8_t> build_goaway(uint32_t last_stream_id, ErrorCode code, const std::string& debug_data = "");

    // Build a WINDOW_UPDATE frame
    static std::vector<uint8_t> build_window_update(uint32_t stream_id, uint32_t increment);

    // Parse a frame from raw bytes
    // Returns the parsed frame and advances the offset
    // Throws std::runtime_error on parse errors
    static std::optional<Http2Frame> parse_frame(const uint8_t* data, size_t length, size_t& offset);

    // Settings management
    const Http2Settings& local_settings() const { return local_settings_; }
    const Http2Settings& remote_settings() const { return remote_settings_; }

    // Flow control
    int32_t connection_send_window() const { return connection_send_window_; }
    int32_t connection_recv_window() const { return connection_recv_window_; }

    // Stream management
    Http2Stream* get_stream(uint32_t id);
    Http2Stream* create_stream(uint32_t id);
    void close_stream(uint32_t id);

    // Encode/decode headers using HPACK
    std::vector<uint8_t> encode_headers(const std::vector<HpackHeaderEntry>& headers);
    std::vector<HpackHeaderEntry> decode_headers(const uint8_t* data, size_t length);

    // TEACHING NOTE: Flow control in HTTP/2
    //
    // HTTP/2 has two levels of flow control:
    // 1. Connection-level: limits total data sent on the connection
    // 2. Stream-level: limits data sent on each individual stream
    //
    // Both start at INITIAL_WINDOW_SIZE (default 65535 bytes) and are
    // updated via WINDOW_UPDATE frames. When a receiver consumes data,
    // it sends a WINDOW_UPDATE to increase the sender window.
    //
    // This prevents a fast sender from overwhelming a slow receiver.
    // It is similar to TCP flow control but operates at the HTTP layer.
    //
    // Flow control only applies to DATA frames, not HEADERS or other
    // frame types. This ensures that control frames are never blocked.

private:
    Http2Settings local_settings_;
    Http2Settings remote_settings_;
    HpackEncoder hpack_encoder_;
    HpackDecoder hpack_decoder_;
    int32_t connection_send_window_;
    int32_t connection_recv_window_;
    std::unordered_map<uint32_t, Http2Stream> streams_;
    uint32_t next_stream_id_;

    // Build a frame from type, flags, stream_id, and payload
    static std::vector<uint8_t> build_frame(
        FrameType type,
        uint8_t flags,
        uint32_t stream_id,
        const std::vector<uint8_t>& payload
    );
};

// TEACHING NOTE: HTTP/2 client - putting it all together
//
// A full HTTP/2 client would:
// 1. Open a TCP connection (using our DNS resolver to resolve the hostname)
// 2. Perform a TLS handshake with ALPN negotiation (offered: "h2", "http/1.1")
// 3. If "h2" is negotiated, send the connection preface + SETTINGS
// 4. Send HEADERS frames for requests
// 5. Receive HEADERS + DATA frames for responses
// 6. Handle flow control, stream management, error handling
// 7. Reuse the connection for subsequent requests (connection pooling)
//
// Our implementation provides the building blocks (frame parsing/building,
// HPACK, stream management). The actual network I/O is handled by the TLS
// and core subsystems. This separation keeps concerns clean and makes
// testing easier (we can test frame parsing without a network connection).
//
// How Chrome manages HTTP/2 connections:
// - Chrome maintains a pool of connections per origin
// - It reuses connections for multiple requests (keep-alive)
// - It limits the number of connections per proxy (32 by default)
// - It can multiplex up to 100 concurrent streams per connection
// - It implements connection coalescing: if multiple origins resolve to
//   the same IP with the same certificate, they share a connection
// - It closes connections after 15 minutes of inactivity (idle timeout)

} // namespace chinstrap