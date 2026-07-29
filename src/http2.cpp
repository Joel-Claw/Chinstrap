// http2.cpp - HTTP/2 client implementation from scratch
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: HPACK implementation overview
//
// HPACK (Header Compression for HTTP/2, RFC 7541) is the most complex part of
// HTTP/2. It compresses HTTP headers using:
//
// 1. Static table: 61 predefined header field entries (RFC 7541, Appendix A).
//    These cover the most common HTTP headers (":method", ":path", ":scheme",
//    ":authority", "content-type", "user-agent", etc.).
//
// 2. Dynamic table: A sliding window of headers seen in previous requests/responses
//    on the same connection. New entries are added at the front (index 0) and
//    old entries are evicted from the back when the table exceeds its max size.
//    The table size is measured as: sum of (name.length + value.length + 32) for each entry.
//    The 32 bytes per entry is overhead for the entry structure itself.
//
// 3. Huffman coding: A variable-length code that assigns shorter bit patterns
//    to more common characters. The HPACK Huffman code is defined in RFC 7541,
//    Appendix B and is specific to HTTP header data.
//
// Header field representations in a header block:
//
// - Indexed Header Field (1 byte, top bit = 1):
//     1 TTTTTTT  (7-bit index into static+dynamic table)
//     The header is the entry at that index. No new data is sent.
//
// - Literal with Incremental Indexing (name + value, stored in dynamic table):
//     01 NNNNNN  (6-bit index for name, 0 = new name follows)
//     [name string if index is 0]
//     value string
//     The header is added to the dynamic table for future reuse.
//
// - Literal without Indexing (name + value, not stored):
//     0000 NNNN  (4-bit index for name, 0 = new name follows)
//     [name string if index is 0]
//     value string
//     The header is not stored in the dynamic table.
//
// - Literal Never Indexed (same as above but with 0001 prefix):
//     0001 NNNN  (4-bit index for name, 0 = new name follows)
//     [name string if index is 0]
//     value string
//     The header must never be indexed (for sensitive headers like Authorization).
//
// - Dynamic Table Size Update:
//     001 NNNNN  (5-bit new max size)
//     Updates the maximum size of the dynamic table.

#include "http2.hpp"

#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <sstream>

namespace chinstrap {

// TEACHING NOTE: HTTP/2 connection preface
//
// The magic string "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" is the HTTP/2 connection
// preface. It was deliberately designed to be an invalid HTTP/1.1 request so
// that an HTTP/2 server can distinguish an HTTP/2 connection from an HTTP/1.1
// connection attempting to upgrade.
//
// The string looks like an HTTP/1.1 request with method "PRI" and path "*",
// which are not valid in HTTP/1.1. This clever design means that an HTTP/1.1
// server would reject this and close the connection, while an HTTP/2 server
// recognizes it and proceeds with the HTTP/2 protocol.
const std::string Http2Connection::CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// ============================================================================
// HPACK Static Table (RFC 7541, Appendix A)
// ============================================================================
//
// TEACHING NOTE: The static table contains 61 common header field entries.
// Index 1-61 are static entries. Index 62+ are dynamic table entries.
// The static table was designed based on analysis of real HTTP traffic.
// It includes pseudo-headers (:method, :path, :scheme, :authority, :status)
// and the most common regular headers (content-type, accept, user-agent, etc.).
// Many entries have empty values because the value is request-specific, but
// the name is still common enough to be worth including.

namespace {

const HpackHeaderEntry STATIC_TABLE[] = {
    // Index 1-10: Authority and method pseudo-headers
    {":authority",                  ""},
    {":method",                      "GET"},
    {":method",                      "POST"},
    {":path",                        "/"},
    {":path",                        "/index.html"},
    {":scheme",                      "http"},
    {":scheme",                      "https"},
    {":status",                      "200"},
    {":status",                      "204"},
    {":status",                      "206"},
    // Index 11-20
    {":status",                      "304"},
    {":status",                      "400"},
    {":status",                      "404"},
    {":status",                      "500"},
    {"accept-charset",               ""},
    {"accept-encoding",              "gzip, deflate"},
    {"accept-language",              ""},
    {"accept-ranges",                ""},
    {"accept",                       ""},
    {"access-control-allow-origin",  ""},
    // Index 21-30
    {"age",                          ""},
    {"allow",                        ""},
    {"authorization",                ""},
    {"cache-control",                ""},
    {"content-disposition",          ""},
    {"content-encoding",             ""},
    {"content-language",             ""},
    {"content-length",               ""},
    {"content-location",             ""},
    {"content-range",                 ""},
    // Index 31-40
    {"content-type",                 ""},
    {"cookie",                       ""},
    {"date",                         ""},
    {"etag",                         ""},
    {"expect",                       ""},
    {"expires",                       ""},
    {"from",                         ""},
    {"host",                         ""},
    {"if-match",                     ""},
    {"if-modified-since",            ""},
    // Index 41-50
    {"if-none-match",                ""},
    {"if-range",                     ""},
    {"if-unmodified-since",          ""},
    {"last-modified",                ""},
    {"link",                         ""},
    {"location",                     ""},
    {"max-forwards",                 ""},
    {"proxy-authenticate",           ""},
    {"proxy-authorization",          ""},
    {"range",                        ""},
    // Index 51-61
    {"referer",                      ""},
    {"refresh",                      ""},
    {"retry-after",                  ""},
    {"server",                       ""},
    {"set-cookie",                   ""},
    {"strict-transport-security",    ""},
    {"transfer-encoding",            ""},
    {"user-agent",                   ""},
    {"vary",                         ""},
    {"via",                          ""},
    {"www-authenticate",             ""},
};

constexpr size_t STATIC_TABLE_SIZE = sizeof(STATIC_TABLE) / sizeof(STATIC_TABLE[0]);

} // anonymous namespace

// ============================================================================
// HpackDynamicTable
// ============================================================================

HpackDynamicTable::HpackDynamicTable(size_t max_size)
    : max_size_(max_size), current_size_(0) {}

// TEACHING NOTE: Dynamic table entry size calculation
//
// Each entry in the dynamic table consumes: name.length + value.length + 32 bytes.
// The 32 bytes is overhead for the entry structure (pointers, pointers, etc.).
// When adding an entry, if the table would exceed max_size, entries are evicted
// from the back (oldest entries) until there is enough room. If a single entry
// is larger than max_size, the table is cleared entirely.
//
// The dynamic table is a FIFO structure: new entries are added at the front
// (index 0 in our vector), and old entries are evicted from the back.
// When looking up by index, index 62 refers to the front of the dynamic table
// (most recently added), and higher indices refer to older entries.

void HpackDynamicTable::add(const std::string& name, const std::string& value) {
    size_t entry_size = name.size() + value.size() + 32;

    // If the entry itself is larger than the max table size, clear the table
    if (entry_size > max_size_) {
        entries_.clear();
        current_size_ = 0;
        return;
    }

    // Evict entries from the back until there is room
    while (current_size_ + entry_size > max_size_ && !entries_.empty()) {
        const auto& back = entries_.back();
        current_size_ -= back.name.size() + back.value.size() + 32;
        entries_.pop_back();
    }

    entries_.insert(entries_.begin(), {name, value});
    current_size_ += entry_size;
}

std::optional<HpackHeaderEntry> HpackDynamicTable::get(size_t index) const {
    // Dynamic table uses 0-based indexing internally
    // (the HPACK encoder/decoder adjusts for static table offset)
    if (index >= entries_.size()) {
        return std::nullopt;
    }
    return entries_[index];
}

void HpackDynamicTable::set_max_size(size_t max_size) {
    max_size_ = max_size;
    evict();
}

void HpackDynamicTable::evict() {
    while (current_size_ > max_size_ && !entries_.empty()) {
        const auto& back = entries_.back();
        current_size_ -= back.name.size() + back.value.size() + 32;
        entries_.pop_back();
    }
}

// ============================================================================
// HPACK Huffman Coding (RFC 7541, Appendix B)
// ============================================================================
//
// TEACHING NOTE: The HPACK Huffman code
//
// The HPACK specification defines a specific Huffman code table for 257 symbols
// (0-255 byte values plus an EOS symbol with value 256). This code was derived
// from statistical analysis of HTTP header data to minimize the average
// encoded length.
//
// For example:
// - 'e' (0x65): code 0b010, 3 bits (very common in headers)
// - 'a' (0x61): code 0b00011, 5 bits
// - 0x00:      code 0b11111111000, 11 bits (rare)
// - EOS (256): code 0b111111111111111111101010, 30 bits
//
// The EOS symbol is used only for padding the final byte. When encoding, we
// pad the remaining bits with 1s (which approximates the EOS prefix).
//
// Our implementation uses a lookup table for fast decoding. For each byte of
// encoded data, we look up the current state and transition to a new state.
// This is much faster than walking a binary tree bit by bit.

namespace {

struct HuffmanCode {
    uint32_t code;
    uint8_t  bits;
};

// TEACHING NOTE: HPACK Huffman code table from RFC 7541, Appendix B
// This table maps byte values (0-255) and EOS (256) to their Huffman codes.
// Each code is the bit pattern (right-justified) and its length in bits.
// For example, symbol 0x30 ('0') has code 0x0A and 5 bits.
// We store this as a large static array. In a production implementation,
// you might use a more compact representation or build the decode table
// at initialization time.
const HuffmanCode HUFFMAN_CODES[257] = {
    {0x1ff8, 13}, {0x7fffd8, 23}, {0xfffffe2, 28}, {0xfffffe3, 28},
    {0xfffffe4, 28}, {0xfffffe5, 28}, {0xfffffe6, 28}, {0xfffffe7, 28},
    {0xfffffe8, 28}, {0xffffea, 24}, {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28},
    {0xfffffed, 28}, {0xfffffee, 28}, {0xfffffef, 28}, {0xffffff0, 28},
    {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28}, {0xffffff6, 28}, {0xffffff7, 28},
    {0xffffff8, 28}, {0xffffff9, 28}, {0xffffffa, 28}, {0xffffffb, 28},
    {0x14, 6}, {0x3f8, 10}, {0x3f9, 10}, {0xffa, 12},
    {0x1ff9, 13}, {0x15, 6}, {0xf8, 8}, {0x7fa, 11},
    {0x3fa, 10}, {0x3fb, 10}, {0xf9, 8}, {0x7fb, 11},
    {0xfa, 8}, {0x16, 6}, {0x17, 6}, {0x18, 6},
    {0x0, 5}, {0x1, 5}, {0x2, 5}, {0x19, 6},
    {0x1a, 6}, {0x1b, 6}, {0x1c, 6}, {0x1d, 6},
    {0x1e, 6}, {0x1f, 6}, {0x5c, 7}, {0xfb, 8},
    {0x7ffc, 15}, {0x20, 6}, {0xffb, 12}, {0x3fc, 10},
    {0x1ffa, 13}, {0x21, 6}, {0x5d, 7}, {0x5e, 7},
    {0x5f, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7},
    {0x63, 7}, {0x64, 7}, {0x65, 7}, {0x66, 7},
    {0x67, 7}, {0x68, 7}, {0x69, 7}, {0x6a, 7},
    {0x6b, 7}, {0x6c, 7}, {0x6d, 7}, {0x6e, 7},
    {0x6f, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7},
    {0xfc, 8}, {0x73, 7}, {0xfd, 8}, {0x1ffb, 13},
    {0x7fff0, 19}, {0x1ffc, 13}, {0x3ffc, 14}, {0x22, 6},
    {0x7ffd, 15}, {0x3, 5}, {0x23, 6}, {0x4, 5},
    {0x24, 6}, {0x5, 5}, {0x25, 6}, {0x26, 6},
    {0x27, 6}, {0x6, 5}, {0x74, 7}, {0x75, 7},
    {0x28, 6}, {0x29, 6}, {0x2a, 6}, {0x7, 5},
    {0x2b, 6}, {0x76, 7}, {0x2c, 6}, {0x8, 5},
    {0x9, 5}, {0x2d, 6}, {0x77, 7}, {0x78, 7},
    {0x79, 7}, {0x7a, 7}, {0x7b, 7}, {0x7ffe, 15},
    {0x7fc, 11}, {0x3ffd, 14}, {0x1ffd, 13}, {0xffffffc, 28},
    {0xfffe6, 20}, {0x3fffd2, 22}, {0xfffe7, 20}, {0xfffe8, 20},
    {0x3fffd3, 22}, {0x3fffd4, 22}, {0x3fffd5, 22}, {0x7fffd9, 23},
    {0x3fffd6, 22}, {0x7fffda, 23}, {0x7fffdb, 23}, {0x7fffdc, 23},
    {0x7fffdd, 23}, {0x7fffde, 23}, {0xffffeb, 24}, {0x7fffdf, 23},
    {0xffffec, 24}, {0xffffed, 24}, {0x3fffd7, 22}, {0x7fffe0, 23},
    {0xffffee, 24}, {0x7fffe1, 23}, {0x7fffe2, 23}, {0x7fffe3, 23},
    {0x7fffe4, 23}, {0x1fffdc, 21}, {0x3fffd8, 22}, {0x7fffe5, 23},
    {0x3fffd9, 22}, {0x7fffe6, 23}, {0x7fffe7, 23}, {0xffffef, 24},
    {0x3fffda, 22}, {0x1fffdd, 21}, {0xfffe9, 20}, {0x3fffdb, 22},
    {0x3fffdc, 22}, {0x7fffe8, 23}, {0x7fffe9, 23}, {0x1fffde, 21},
    {0x7fffea, 23}, {0x3fffdd, 22}, {0x3fffde, 22}, {0xfffff0, 24},
    {0x1fffdf, 21}, {0x3fffdf, 22}, {0x7fffeb, 23}, {0x7fffec, 23},
    {0x1fffe0, 21}, {0x1fffe1, 21}, {0x3fffe0, 22}, {0x1fffe2, 21},
    {0x7fffed, 23}, {0x3fffe1, 22}, {0x7fffee, 23}, {0x7fffef, 23},
    {0xfffea, 20}, {0x3fffe2, 22}, {0x3fffe3, 22}, {0x3fffe4, 22},
    {0x7ffff0, 23}, {0x3fffe5, 22}, {0x3fffe6, 22}, {0x7ffff1, 23},
    {0x3ffffe0, 26}, {0x3ffffe1, 26}, {0xfffeb, 20}, {0x7fff1, 19},
    {0x3fffe7, 22}, {0x7ffff2, 23}, {0x3fffe8, 22}, {0x1ffffec, 25},
    {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26}, {0x7ffffde, 27},
    {0x7ffffdf, 27}, {0x3ffffe5, 26}, {0xfffff1, 24}, {0x1ffffed, 25},
    {0x7fff2, 19}, {0x1fffe3, 21}, {0x3ffffe6, 26}, {0x7ffffe0, 27},
    {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27}, {0xfffff2, 24},
    {0x1fffe4, 21}, {0x1fffe5, 21}, {0x3ffffe8, 26}, {0x3ffffe9, 26},
    {0xffffffd, 28}, {0x7ffffe3, 27}, {0x7ffffe4, 27}, {0x7ffffe5, 27},
    {0xfffec, 20}, {0xfffff3, 24}, {0xfffed, 20}, {0x1fffe6, 21},
    {0x3fffe9, 22}, {0x1fffe7, 21}, {0x1fffe8, 21}, {0x7ffff3, 23},
    {0x3fffea, 22}, {0x3fffeb, 22}, {0x1ffffee, 25}, {0x1ffffef, 25},
    {0xfffff4, 24}, {0xfffff5, 24}, {0x3ffffea, 26}, {0x7ffff4, 23},
    {0x3ffffeb, 26}, {0x7ffffe6, 27}, {0x3ffffec, 26}, {0x3ffffed, 26},
    {0x7ffffe7, 27}, {0x7ffffe8, 27}, {0x7ffffe9, 27}, {0x7ffffea, 27},
    {0x7ffffeb, 27}, {0xffffffe, 28}, {0x7ffffec, 27}, {0x7ffffed, 27},
    {0x7ffffee, 27}, {0x7ffffef, 27}, {0x7fffff0, 27}, {0x3ffffee, 26},
    // EOS (256) - end of string symbol, 30 bits
    {0x3fffffff, 30},
};

} // anonymous namespace

// TEACHING NOTE: Huffman encoding process
//
// To encode a string:
// 1. For each character, look up its Huffman code and bit length
// 2. Append the code bits to a bit buffer (accumulating partial bytes)
// 3. After all characters, pad the remaining bits with 1s to fill the last byte
// 4. The padding with 1s is important: it ensures the decoder does not try to
//    decode beyond the string. The EOS symbol (all 1s for 30 bits) serves as
//    a marker, but we just pad with 1s which is equivalent for our purposes.

std::vector<uint8_t> HpackHuffman::encode(const std::string& input) {
    // First calculate the encoded size to decide if encoding is worth it
    size_t encoded_bits = 0;
    for (unsigned char c : input) {
        encoded_bits += HUFFMAN_CODES[c].bits;
    }
    size_t encoded_bytes = (encoded_bits + 7) / 8;

    // If Huffman encoding does not help, return empty (caller should use raw)
    if (encoded_bytes >= input.size()) {
        return {};
    }

    std::vector<uint8_t> output;
    output.reserve(encoded_bytes);

    uint32_t bit_buffer = 0;
    uint8_t  bit_count = 0;

    for (unsigned char c : input) {
        const auto& hc = HUFFMAN_CODES[c];
        bit_buffer = (bit_buffer << hc.bits) | hc.code;
        bit_count += hc.bits;

        while (bit_count >= 8) {
            bit_count -= 8;
            output.push_back(static_cast<uint8_t>((bit_buffer >> bit_count) & 0xFF));
        }
    }

    // Pad remaining bits with 1s (EOS prefix)
    if (bit_count > 0) {
        bit_buffer |= (0xFF >> bit_count);
        output.push_back(static_cast<uint8_t>(bit_buffer & 0xFF));
    }

    return output;
}

// TEACHING NOTE: Huffman decoding process
//
// Decoding Huffman codes is more complex because codes have variable length.
// We use a simple approach: accumulate bits and match against the code table.
// For each position in the input, we try adding bits one at a time and check
// if the accumulated bits match any Huffman code.
//
// A more efficient implementation would use a precomputed state machine
// (a table of [state][byte] -> new_state, symbol, bits_consumed). But for
// clarity, we use the simple bit-by-bit approach here.

std::string HpackHuffman::decode(const uint8_t* data, size_t length) {
    std::string result;
    uint32_t bit_buffer = 0;
    uint8_t  bit_count = 0;

    for (size_t i = 0; i < length; ++i) {
        bit_buffer = (bit_buffer << 8) | data[i];
        bit_count += 8;

        while (bit_count > 0) {
            // Try to match the current bits against Huffman codes
            for (int sym = 0; sym < 256; ++sym) {
                const auto& hc = HUFFMAN_CODES[sym];
                if (hc.bits <= bit_count) {
                    uint32_t candidate = (bit_buffer >> (bit_count - hc.bits)) & ((1u << hc.bits) - 1);
                    if (candidate == hc.code) {
                        result += static_cast<char>(sym);
                        bit_count -= hc.bits;
                        bit_buffer &= (1u << bit_count) - 1;
                        goto next_bit;
                    }
                }
            }
            // No match found - could be padding bits
            // If remaining bits are all 1s, it is padding
            if (bit_count <= 7) {
                uint32_t padding = bit_buffer & ((1u << bit_count) - 1);
                uint32_t all_ones = (1u << bit_count) - 1;
                if (padding == all_ones) {
                    bit_count = 0;
                    break;
                }
            }
            // Move to next byte if no match
            break;
            next_bit:;
        }
    }

    return result;
}

bool HpackHuffman::should_encode(const std::string& input) {
    size_t encoded_bits = 0;
    for (unsigned char c : input) {
        encoded_bits += HUFFMAN_CODES[c].bits;
    }
    size_t encoded_bytes = (encoded_bits + 7) / 8;
    return encoded_bytes < input.size();
}

// ============================================================================
// HPACK Integer Encoding/Decoding
// ============================================================================
//
// TEACHING NOTE: HPACK integer representation (RFC 7541, Section 5.1)
//
// HPACK uses a variable-length integer encoding with a prefix:
// The first byte has some bits used for the prefix (to indicate the
// representation type) and the remaining bits are the start of the integer.
//
// If the value fits in the prefix bits (value < 2^prefix - 1), it is
// stored directly in the prefix. Otherwise, the prefix bits are all 1s,
// and the remaining value is stored in subsequent bytes using a 7-bit
// continuation encoding (each byte has a continuation bit in the MSB).
//
// Example: encoding 1337 with a 5-bit prefix:
//   1337 >= 31 (2^5 - 1), so prefix is 31 (all 1s)
//   1337 - 31 = 1306
//   1306 in 7-bit continuations:
//     1306 & 0x7F = 0x1A, continuation bit set: 0x9A
//     1306 >> 7 = 10, no continuation: 0x0A
//   Result: [prefix | 31] 0x9A 0x0A
//
// This encoding is compact for small values and extends gracefully for large ones.

std::vector<uint8_t> HpackEncoder::encode_integer(uint64_t value, uint8_t prefix_bits, uint8_t prefix_byte) {
    std::vector<uint8_t> result;
    uint64_t max_prefix = (1ull << prefix_bits) - 1;

    if (value < max_prefix) {
        // Value fits in prefix
        result.push_back(prefix_byte | static_cast<uint8_t>(value));
    } else {
        // Value does not fit - fill prefix with 1s and use continuation bytes
        result.push_back(prefix_byte | static_cast<uint8_t>(max_prefix));
        value -= max_prefix;

        while (value >= 128) {
            result.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        result.push_back(static_cast<uint8_t>(value));
    }

    return result;
}

uint64_t HpackDecoder::decode_integer(const uint8_t* data, size_t& offset, uint8_t prefix_bits) {
    uint64_t max_prefix = (1ull << prefix_bits) - 1;
    uint64_t value = data[offset] & max_prefix;
    offset += 1;

    if (value < max_prefix) {
        return value;
    }

    // Read continuation bytes
    uint8_t shift = 0;
    while (true) {
        uint8_t b = data[offset];
        offset += 1;
        value += static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            break;
        }
        shift += 7;

        // Safety limit: integers should not be larger than 2^32
        if (shift > 35) {
            throw std::runtime_error("HPACK integer too large");
        }
    }

    return value;
}

// ============================================================================
// HPACK String Encoding/Decoding
// ============================================================================
//
// TEACHING NOTE: HPACK string representation (RFC 7541, Section 5.2)
//
// A string is encoded as:
//   1 bit (H) | 7-bit integer (length)
//   [string bytes]
//
// The H bit (top bit of the first byte) indicates whether the string is
// Huffman-encoded (1) or raw (0). The 7-bit integer is the length of the
// string in bytes (after encoding if Huffman is used).
//
// If the length is >= 127, the integer uses the HPACK variable-length encoding
// with a 7-bit prefix.

std::vector<uint8_t> HpackEncoder::encode_string(const std::string& str) {
    std::vector<uint8_t> result;

    // Try Huffman encoding
    auto huff = HpackHuffman::encode(str);
    if (!huff.empty() && huff.size() < str.size()) {
        // Use Huffman encoding (H bit = 1)
        auto length_bytes = encode_integer(huff.size(), 7, 0x80);
        result.insert(result.end(), length_bytes.begin(), length_bytes.end());
        result.insert(result.end(), huff.begin(), huff.end());
    } else {
        // Use raw string (H bit = 0)
        auto length_bytes = encode_integer(str.size(), 7, 0x00);
        result.insert(result.end(), length_bytes.begin(), length_bytes.end());
        result.insert(result.end(), str.begin(), str.end());
    }

    return result;
}

std::string HpackDecoder::decode_string(const uint8_t* data, size_t& offset) {
    bool huffman = (data[offset] & 0x80) != 0;
    uint64_t length = decode_integer(data, offset, 7);

    std::string result;
    if (huffman) {
        result = HpackHuffman::decode(data + offset, static_cast<size_t>(length));
    } else {
        result = std::string(reinterpret_cast<const char*>(data + offset), static_cast<size_t>(length));
    }
    offset += static_cast<size_t>(length);

    return result;
}

// ============================================================================
// HpackEncoder
// ============================================================================

HpackEncoder::HpackEncoder(size_t max_table_size)
    : dynamic_table_(max_table_size) {}

std::pair<size_t, bool> HpackEncoder::find_header(const std::string& name, const std::string& value) {
    // TEACHING NOTE: Header lookup in HPACK
    //
    // When encoding a header, we search the static table (indices 1-61)
    // and the dynamic table (indices 62+) for a match.
    //
    // A full match (both name and value) is best: we can send a single
    // indexed header field (1 byte).
    //
    // A name-only match is also useful: we send the index for the name
    // and a literal value. This saves sending the name string.
    //
    // We prefer static table matches over dynamic table matches because
    // static entries never change and are always available.

    // Search static table for full match
    for (size_t i = 0; i < STATIC_TABLE_SIZE; ++i) {
        if (STATIC_TABLE[i].name == name && STATIC_TABLE[i].value == value) {
            return {i + 1, true};
        }
    }

    // Search dynamic table for full match
    for (size_t i = 0; i < dynamic_table_.size(); ++i) {
        auto entry = dynamic_table_.get(i);
        if (entry && entry->name == name && entry->value == value) {
            return {STATIC_TABLE_SIZE + i + 1, true};
        }
    }

    // Search static table for name-only match
    for (size_t i = 0; i < STATIC_TABLE_SIZE; ++i) {
        if (STATIC_TABLE[i].name == name) {
            return {i + 1, false};
        }
    }

    // Search dynamic table for name-only match
    for (size_t i = 0; i < dynamic_table_.size(); ++i) {
        auto entry = dynamic_table_.get(i);
        if (entry && entry->name == name) {
            return {STATIC_TABLE_SIZE + i + 1, false};
        }
    }

    // No match at all
    return {0, false};
}

std::vector<uint8_t> HpackEncoder::encode(const std::vector<HpackHeaderEntry>& headers) {
    std::vector<uint8_t> result;

    for (const auto& header : headers) {
        auto [index, full_match] = find_header(header.name, header.value);

        if (full_match) {
            // Indexed header field (top bit = 1, 7-bit index)
            auto bytes = encode_integer(index, 7, 0x80);
            result.insert(result.end(), bytes.begin(), bytes.end());
        } else if (index > 0) {
            // Literal with incremental indexing, name from table
            // 01 prefix, 6-bit index
            auto bytes = encode_integer(index, 6, 0x40);
            result.insert(result.end(), bytes.begin(), bytes.end());
            auto value_bytes = encode_string(header.value);
            result.insert(result.end(), value_bytes.begin(), value_bytes.end());
            dynamic_table_.add(header.name, header.value);
        } else {
            // Literal with incremental indexing, new name
            // 01 prefix, index = 0 (new name follows)
            result.push_back(0x40); // 01000000: literal with indexing, index = 0
            auto name_bytes = encode_string(header.name);
            result.insert(result.end(), name_bytes.begin(), name_bytes.end());
            auto value_bytes = encode_string(header.value);
            result.insert(result.end(), value_bytes.begin(), value_bytes.end());
            dynamic_table_.add(header.name, header.value);
        }
    }

    return result;
}

// ============================================================================
// HpackDecoder
// ============================================================================

HpackDecoder::HpackDecoder(size_t max_table_size)
    : dynamic_table_(max_table_size) {}

std::vector<HpackHeaderEntry> HpackDecoder::decode(const uint8_t* data, size_t length) {
    std::vector<HpackHeaderEntry> headers;
    size_t offset = 0;

    while (offset < length) {
        uint8_t first_byte = data[offset];

        if ((first_byte & 0x80) != 0) {
            // TEACHING NOTE: Indexed header field
            //
            //   1   T T T T T T T
            //
            // The top bit is 1. The remaining 7 bits are the index into the
            // static + dynamic table. The header is the entry at that index.
            // Index 0 is invalid (it would be all zeros with the top bit set).

            uint64_t index = decode_integer(data, offset, 7);
            if (index == 0) {
                throw std::runtime_error("HPACK: invalid index 0 in indexed header");
            }

            if (index <= STATIC_TABLE_SIZE) {
                headers.push_back(STATIC_TABLE[index - 1]);
            } else {
                auto entry = dynamic_table_.get(index - STATIC_TABLE_SIZE - 1);
                if (!entry) {
                    throw std::runtime_error("HPACK: dynamic table index out of range");
                }
                headers.push_back(*entry);
            }
        } else if ((first_byte & 0xC0) == 0x40) {
            // TEACHING NOTE: Literal with incremental indexing
            //
            //   01 N N N N N N
            //
            // The top 2 bits are 01. The remaining 6 bits are the name index
            // (0 = new name follows). After the name, the value string follows.
            // The header is added to the dynamic table.

            uint64_t name_index = decode_integer(data, offset, 6);

            std::string name;
            if (name_index == 0) {
                name = decode_string(data, offset);
            } else {
                if (name_index <= STATIC_TABLE_SIZE) {
                    name = STATIC_TABLE[name_index - 1].name;
                } else {
                    auto entry = dynamic_table_.get(name_index - STATIC_TABLE_SIZE - 1);
                    if (!entry) {
                        throw std::runtime_error("HPACK: dynamic table name index out of range");
                    }
                    name = entry->name;
                }
            }

            std::string value = decode_string(data, offset);
            dynamic_table_.add(name, value);
            headers.push_back({name, value});
        } else if ((first_byte & 0xF0) == 0x00) {
            // TEACHING NOTE: Literal without indexing
            //
            //   0000 N N N N
            //
            // The top 4 bits are 0000. The remaining 4 bits are the name index.
            // The header is NOT added to the dynamic table.

            uint64_t name_index = decode_integer(data, offset, 4);

            std::string name;
            if (name_index == 0) {
                name = decode_string(data, offset);
            } else if (name_index <= STATIC_TABLE_SIZE) {
                name = STATIC_TABLE[name_index - 1].name;
            } else {
                auto entry = dynamic_table_.get(name_index - STATIC_TABLE_SIZE - 1);
                if (!entry) {
                    throw std::runtime_error("HPACK: dynamic table name index out of range");
                }
                name = entry->name;
            }

            std::string value = decode_string(data, offset);
            headers.push_back({name, value});
        } else if ((first_byte & 0xF0) == 0x10) {
            // TEACHING NOTE: Literal never indexed
            //
            //   0001 N N N N
            //
            // Same as literal without indexing, but the header must NEVER be
            // added to the dynamic table, even by a proxy. This is used for
            // sensitive headers like Authorization or Cookie.

            uint64_t name_index = decode_integer(data, offset, 4);

            std::string name;
            if (name_index == 0) {
                name = decode_string(data, offset);
            } else if (name_index <= STATIC_TABLE_SIZE) {
                name = STATIC_TABLE[name_index - 1].name;
            } else {
                auto entry = dynamic_table_.get(name_index - STATIC_TABLE_SIZE - 1);
                if (!entry) {
                    throw std::runtime_error("HPACK: dynamic table name index out of range");
                }
                name = entry->name;
            }

            std::string value = decode_string(data, offset);
            headers.push_back({name, value});
        } else if ((first_byte & 0xE0) == 0x20) {
            // TEACHING NOTE: Dynamic table size update
            //
            //   001 N N N N N
            //
            // The top 3 bits are 001. The remaining 5 bits are the new max
            // table size. This reduces the dynamic table size, potentially
            // evicting entries.

            uint64_t new_size = decode_integer(data, offset, 5);
            dynamic_table_.set_max_size(new_size);
        } else {
            throw std::runtime_error("HPACK: unknown header field representation");
        }
    }

    return headers;
}

// ============================================================================
// Http2Connection - Frame Building
// ============================================================================
//
// TEACHING NOTE: HTTP/2 frame format (RFC 9113, Section 4.1)
//
// All frames have the following format:
//
//   +-----------------------------------------------+
//   |                 Length (24)                    |
//   +---------------+---------------+---------------+
//   |   Type (8)    |   Flags (8)   |
//   +-+-------------+---------------+-------------------------------+
//   |R|                 Stream Identifier (31)                     |
//   +=+=============================================================+
//   |                   Frame Payload (0...)                       |
//   +---------------------------------------------------------------+
//
// Length is 3 bytes (24 bits), max 16,777,215 bytes (but limited by
// MAX_FRAME_SIZE setting, default 16384, max 16777215).
// Type is 1 byte. Flags is 1 byte (type-specific).
// R is a reserved bit (must be 0). Stream ID is 31 bits.
// Stream ID 0 is for connection-level frames (SETTINGS, PING, GOAWAY).

std::vector<uint8_t> Http2Connection::build_frame(
    FrameType type,
    uint8_t flags,
    uint32_t stream_id,
    const std::vector<uint8_t>& payload
) {
    std::vector<uint8_t> frame;

    // Length (3 bytes, big-endian)
    uint32_t length = static_cast<uint32_t>(payload.size());
    frame.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(length & 0xFF));

    // Type (1 byte)
    frame.push_back(static_cast<uint8_t>(type));

    // Flags (1 byte)
    frame.push_back(flags);

    // Stream ID (4 bytes, big-endian, top bit must be 0)
    frame.push_back(static_cast<uint8_t>((stream_id >> 24) & 0x7F));
    frame.push_back(static_cast<uint8_t>((stream_id >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((stream_id >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(stream_id & 0xFF));

    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

std::vector<uint8_t> Http2Connection::get_preface() const {
    return std::vector<uint8_t>(CONNECTION_PREFACE.begin(), CONNECTION_PREFACE.end());
}

// TEACHING NOTE: Initial SETTINGS frame
//
// The client sends a SETTINGS frame as the first frame after the connection
// preface. This frame contains the client settings for the connection:
// - HEADER_TABLE_SIZE: max HPACK dynamic table size we will accept
// - ENABLE_PUSH: whether we accept server push (often set to 0 to disable)
// - MAX_CONCURRENT_STREAMS: max concurrent streams we will allow
// - INITIAL_WINDOW_SIZE: initial flow control window for new streams
// - MAX_FRAME_SIZE: max frame payload size we will accept
//
// The server responds with its own SETTINGS frame, and both sides must
// send a SETTINGS ACK frame to acknowledge the received settings.

std::vector<uint8_t> Http2Connection::get_initial_settings() const {
    std::vector<uint8_t> payload;

    // Helper to write a setting (ID + value)
    auto write_setting = [&](SettingsId id, uint32_t value) {
        payload.push_back(static_cast<uint8_t>((static_cast<uint16_t>(id) >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(static_cast<uint16_t>(id) & 0xFF));
        payload.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        payload.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        payload.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(value & 0xFF));
    };

    write_setting(SettingsId::HEADER_TABLE_SIZE, local_settings_.header_table_size);
    write_setting(SettingsId::ENABLE_PUSH, local_settings_.enable_push ? 1 : 0);
    write_setting(SettingsId::MAX_CONCURRENT_STREAMS, local_settings_.max_concurrent_streams);
    write_setting(SettingsId::INITIAL_WINDOW_SIZE, local_settings_.initial_window_size);
    write_setting(SettingsId::MAX_FRAME_SIZE, local_settings_.max_frame_size);
    if (local_settings_.max_header_list_size > 0) {
        write_setting(SettingsId::MAX_HEADER_LIST_SIZE, local_settings_.max_header_list_size);
    }

    return build_frame(FrameType::SETTINGS, 0, 0, payload);
}

std::vector<uint8_t> Http2Connection::build_settings_ack() {
    return build_frame(FrameType::SETTINGS, FrameFlags::ACK, 0, {});
}

// TEACHING NOTE: HEADERS frame construction
//
// The HEADERS frame carries a compressed header block (HPACK encoded).
// It may include:
// - Stream priority (if PRIORITY flag is set): 5 bytes with stream dependency,
//   weight, and exclusive flag. This is deprecated in later HTTP/2 specs but
//   still commonly sent.
// - Padding (if PADDED flag is set): 1 byte pad length + pad bytes
// - The HPACK-encoded header block
//
// If the headers fit in one frame (within MAX_FRAME_SIZE), the END_HEADERS
// flag is set. If they do not fit, CONTINUATION frames follow.
//
// The END_STREAM flag indicates that no DATA frames will follow (for GET
// requests with no body).

std::vector<uint8_t> Http2Connection::build_headers_frame(
    uint32_t stream_id,
    const std::vector<HpackHeaderEntry>& headers,
    bool end_stream
) {
    // Encode headers using HPACK
    std::vector<uint8_t> header_block = hpack_encoder_.encode(headers);

    uint8_t flags = FrameFlags::END_HEADERS;
    if (end_stream) {
        flags |= FrameFlags::END_STREAM;
    }

    return build_frame(FrameType::HEADERS, flags, stream_id, header_block);
}

std::vector<uint8_t> Http2Connection::build_data_frame(
    uint32_t stream_id,
    const std::vector<uint8_t>& data,
    bool end_stream
) {
    uint8_t flags = 0;
    if (end_stream) {
        flags |= FrameFlags::END_STREAM;
    }

    return build_frame(FrameType::DATA, flags, stream_id, data);
}

std::vector<uint8_t> Http2Connection::build_rst_stream(uint32_t stream_id, ErrorCode code) {
    std::vector<uint8_t> payload(4);
    uint32_t code_val = static_cast<uint32_t>(code);
    payload[0] = static_cast<uint8_t>((code_val >> 24) & 0xFF);
    payload[1] = static_cast<uint8_t>((code_val >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((code_val >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(code_val & 0xFF);

    return build_frame(FrameType::RST_STREAM, 0, stream_id, payload);
}

std::vector<uint8_t> Http2Connection::build_ping(uint64_t opaque_data, bool ack) {
    std::vector<uint8_t> payload(8);
    for (int i = 0; i < 8; ++i) {
        payload[i] = static_cast<uint8_t>((opaque_data >> (56 - i * 8)) & 0xFF);
    }

    uint8_t flags = ack ? FrameFlags::ACK : 0;
    return build_frame(FrameType::PING, flags, 0, payload);
}

std::vector<uint8_t> Http2Connection::build_goaway(
    uint32_t last_stream_id,
    ErrorCode code,
    const std::string& debug_data
) {
    std::vector<uint8_t> payload;

    // Last stream ID (4 bytes, top bit reserved)
    payload.push_back(static_cast<uint8_t>((last_stream_id >> 24) & 0x7F));
    payload.push_back(static_cast<uint8_t>((last_stream_id >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((last_stream_id >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(last_stream_id & 0xFF));

    // Error code (4 bytes)
    uint32_t code_val = static_cast<uint32_t>(code);
    payload.push_back(static_cast<uint8_t>((code_val >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code_val >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code_val >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code_val & 0xFF));

    // Debug data (optional)
    payload.insert(payload.end(), debug_data.begin(), debug_data.end());

    return build_frame(FrameType::GOAWAY, 0, 0, payload);
}

std::vector<uint8_t> Http2Connection::build_window_update(uint32_t stream_id, uint32_t increment) {
    std::vector<uint8_t> payload(4);
    payload[0] = static_cast<uint8_t>((increment >> 24) & 0x7F);
    payload[1] = static_cast<uint8_t>((increment >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((increment >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(increment & 0xFF);

    return build_frame(FrameType::WINDOW_UPDATE, 0, stream_id, payload);
}

// ============================================================================
// Http2Connection - Frame Parsing
// ============================================================================

std::optional<Http2Frame> Http2Connection::parse_frame(
    const uint8_t* data,
    size_t length,
    size_t& offset
) {
    if (offset + 9 > length) {
        return std::nullopt;  // Not enough data for frame header
    }

    Http2Frame frame;

    // Length (3 bytes, big-endian)
    uint32_t frame_length = (static_cast<uint32_t>(data[offset]) << 16) |
                           (static_cast<uint32_t>(data[offset + 1]) << 8) |
                           static_cast<uint32_t>(data[offset + 2]);

    // Type (1 byte)
    frame.type = static_cast<FrameType>(data[offset + 3]);

    // Flags (1 byte)
    frame.flags = data[offset + 4];

    // Stream ID (4 bytes, top bit reserved and must be 0)
    frame.stream_id = (static_cast<uint32_t>(data[offset + 5] & 0x7F) << 24) |
                      (static_cast<uint32_t>(data[offset + 6]) << 16) |
                      (static_cast<uint32_t>(data[offset + 7]) << 8) |
                      static_cast<uint32_t>(data[offset + 8]);

    offset += 9;

    // Check we have enough data for the payload
    if (offset + frame_length > length) {
        offset -= 9;  // Rewind so caller can wait for more data
        return std::nullopt;
    }

    // Copy payload
    frame.payload.assign(data + offset, data + offset + frame_length);
    offset += frame_length;

    return frame;
}

// ============================================================================
// Http2Connection - Stream Management
// ============================================================================

Http2Connection::Http2Connection()
    : hpack_encoder_(4096),
      hpack_decoder_(4096),
      connection_send_window_(65535),
      connection_recv_window_(65535),
      next_stream_id_(1) {}

Http2Connection::~Http2Connection() = default;

Http2Stream* Http2Connection::get_stream(uint32_t id) {
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return nullptr;
    }
    return &it->second;
}

Http2Stream* Http2Connection::create_stream(uint32_t id) {
    if (id == 0) {
        return nullptr;  // Stream 0 is the connection itself
    }
    auto& stream = streams_[id];
    stream.id = id;
    stream.state = StreamState::IDLE;
    stream.send_window = static_cast<int32_t>(local_settings_.initial_window_size);
    stream.recv_window = static_cast<int32_t>(local_settings_.initial_window_size);
    return &stream;
}

void Http2Connection::close_stream(uint32_t id) {
    streams_.erase(id);
}

std::vector<uint8_t> Http2Connection::encode_headers(const std::vector<HpackHeaderEntry>& headers) {
    return hpack_encoder_.encode(headers);
}

std::vector<HpackHeaderEntry> Http2Connection::decode_headers(const uint8_t* data, size_t length) {
    return hpack_decoder_.decode(data, length);
}

} // namespace chinstrap