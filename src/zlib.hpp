// zlib.hpp - Minimal zlib/DEFLATE decompressor implemented from scratch
//
// TEACHING NOTE: What is zlib and DEFLATE?
// ===========================================================================
// zlib is a software library used for data compression. It implements the
// DEFLATE compression algorithm, which is specified in RFC 1951.
// DEFLATE is the core compression method used by:
//   - PNG images (compresses the image data in IDAT chunks)
//   - HTTP gzip Content-Encoding (compresses web responses)
//   - ZIP file archives
//   - gzip file format
//
// DEFLATE combines two fundamental compression techniques:
//   1. LZ77 - a sliding-window dictionary compression that replaces repeated
//      byte sequences with (distance, length) references back to earlier data.
//   2. Huffman coding - a variable-length prefix code that assigns shorter
//      bit sequences to more frequent symbols, and longer ones to rarer symbols.
//
// The zlib wrapper (RFC 1950) adds a 2-byte header and an Adler-32 checksum
// around the raw DEFLATE stream. The gzip wrapper (RFC 1952) adds a more
// extensive header with optional fields and a CRC-32 checksum.
//
// This file implements INFLATION (decompression) only. We do not need
// compression because a browser only needs to DECODE compressed data
// (PNG images, gzip HTTP responses), never to encode it.
//
// Why implement this from scratch?
//   - No third-party library dependency (critical requirement for Chinstrap)
//   - Understanding how compression works is educational
//   - Browsers interact with compressed data constantly

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace chinstrap {

// TEACHING NOTE: TheInflater class
// =========================================================================
// This class implements the DEFLATE decompression algorithm. It maintains
// internal state across multiple calls, but for simplicity we provide a
// single-shot inflate() that takes all compressed bytes at once.
//
// The algorithm works by reading a stream of bits (not bytes!) and:
//   1. Determining the block type (stored, fixed Huffman, or dynamic Huffman)
//   2. For stored blocks: copying raw bytes directly (no compression)
//   3. For Huffman blocks: decoding symbols using Huffman trees, where some
//      symbols are literal bytes and others are back-references (length/distance)
//   4. Producing output until we hit the end-of-block marker

class Inflater {
public:
    Inflater();

    // Decompress a zlib-wrapped DEFLATE stream (RFC 1950).
    // The zlib format is: 2-byte header + DEFLATE data + 4-byte Adler-32.
    // Returns the decompressed bytes.
    std::vector<uint8_t> inflate_zlib(const std::vector<uint8_t>& compressed);

    // Decompress a raw DEFLATE stream (RFC 1951, no wrapper).
    // Returns the decompressed bytes.
    std::vector<uint8_t> inflate_raw(const std::vector<uint8_t>& compressed);

    // Decompress a gzip-wrapped DEFLATE stream (RFC 1952).
    // gzip format: 10-byte header + optional fields + DEFLATE + CRC-32 + size.
    // Returns the decompressed bytes.
    std::vector<uint8_t> inflate_gzip(const std::vector<uint8_t>& compressed);

private:
    // TEACHING NOTE: Bit reading
    // =====================================================================
    // DEFLATE stores data in a bit-oriented stream, not byte-oriented.
    // Huffman codes can be any number of bits (1-15), and the LZ77
    // distance/length codes also use variable bit lengths.
    //
    // DEFLATE uses LSB-first bit packing: the first bit of a code is in the
    // least significant bit of the first byte. This is the opposite of most
    // file formats, which are MSB-first. This choice was made because DEFLATE
    // was designed for Intel x86 processors which are little-endian.
    //
    // We maintain a bit buffer where we accumulate bits from the input stream
    // and extract them one at a time (or in small groups) from the LSB end.

    const uint8_t* m_data;       // pointer to compressed data
    size_t         m_data_size;  // size of compressed data
    size_t         m_byte_pos;   // current byte position in input
    uint32_t       m_bit_buf;    // bit accumulator
    uint32_t       m_bit_count;  // number of valid bits in m_bit_buf

    // Output buffer
    std::vector<uint8_t> m_output;

    // Sliding window for LZ77 back-references (we use the output buffer itself)

    // --- Bit-level reading ---
    uint32_t read_bits(int n);
    void     align_to_byte();

    // --- Huffman decoding ---
    // TEACHING NOTE: Huffman coding in DEFLATE
    // =====================================================================
    // A Huffman code assigns a variable-length bit pattern to each symbol,
    // such that no code is a prefix of any other (prefix-free code).
    // This allows unambiguous decoding from a bit stream.
    //
    // DEFLATE uses canonical Huffman codes: given the code lengths for each
    // symbol, the actual codes are deterministic (specified by RFC 1951).
    // This means the transmitter only needs to send the code lengths, not
    // the actual codes, which is more compact.
    //
    // We represent a Huffman tree as two arrays:
    //   - code_lengths[i]: the bit length of the code for symbol i
    //   - A lookup table or tree structure for fast decoding
    //
    // For small alphabets (like the literal/length alphabet with 286 symbols)
    // we build a simple tree-walk decoder.

    struct HuffmanTree {
        // We use the "count + sorted symbols" approach from RFC 1951
        // This is a fast O(1) per-bit decode method.
        std::vector<int> counts;   // counts[i] = number of codes of length i
        std::vector<int> symbols;  // symbols sorted by code length then value
        int max_bits;              // maximum code length

        HuffmanTree() : max_bits(0) {}

        bool empty() const { return symbols.empty(); }
    };

    HuffmanTree m_litlen_tree;   // literal/length tree (286 symbols)
    HuffmanTree m_dist_tree;     // distance tree (30 symbols)

    // Build a Huffman tree from code lengths.
    void build_huffman(HuffmanTree& tree, const std::vector<int>& code_lengths);

    // Decode one symbol from the bit stream using the given tree.
    int decode_huffman(const HuffmanTree& tree);

    // --- DEFLATE block processing ---
    void process_stored_block();
    void process_fixed_huffman_block();
    void process_dynamic_huffman_block();

    // The core decode loop: decode literal/length and distance codes.
    // Used by both fixed and dynamic Huffman blocks.
    void decode_block(const HuffmanTree& litlen, const HuffmanTree& dist);

    // --- Length and distance extra bits ---
    // TEACHING NOTE: Length and distance encoding in DEFLATE
    // =====================================================================
    // LZ77 back-references are encoded as (length, distance) pairs.
    // Length ranges from 3 to 258 bytes. Distance ranges from 1 to 32768.
    //
    // Rather than having a unique Huffman code for every possible length and
    // distance, DEFLATE uses a two-level encoding:
    //   1. A Huffman code selects a "base" value
    //   2. Extra raw bits are appended to reach the full range
    //
    // For example, length codes 257-264 all mean lengths 3-10 (no extra bits),
    // code 265 means length 11-12 (1 extra bit), etc.
    // Similarly for distances: code 0 = distance 1, code 1 = distance 2,
    // code 2 = distance 3, code 3 = distance 4, code 4 = distance 5-6 (1 extra), etc.
    //
    // The tables below are specified in RFC 1951 section 3.2.5.

    static const int LENGTH_BASE[29];
    static const int LENGTH_EXTRA[29];
    static const int DIST_BASE[30];
    static const int DIST_EXTRA[30];

    // Reverse helper for canonical code construction
    static uint32_t reverse_bits(uint32_t val, int n);
};

// TEACHING NOTE: Adler-32 checksum
// =========================================================================
// The zlib format uses Adler-32 as its checksum. It is simpler and faster
// than CRC-32 but provides weaker error detection. We include it for
// validation but do not strictly require it for decompression to work.
//
// Adler-32 is: s1 = sum of bytes mod 65521, s2 = sum of s1 values mod 65521
// Result = (s2 << 16) | s1

uint32_t adler32(const uint8_t* data, size_t len);
uint32_t adler32(const std::vector<uint8_t>& data);

// TEACHING NOTE: CRC-32 checksum
// =========================================================================
// The gzip format uses CRC-32 (ISO 3309). This is the same polynomial used
// by PNG. We need this for gzip validation.

uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32(const std::vector<uint8_t>& data);

} // namespace chinstrap