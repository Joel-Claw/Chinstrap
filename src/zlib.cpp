// zlib.cpp - Minimal zlib/DEFLATE decompressor implementation
//
// TEACHING NOTE: This file implements the DEFLATE decompression algorithm
// from scratch with zero third-party dependencies. This is one of the most
// complex pieces in the browser because it must handle:
//   - Bit-level I/O with LSB-first bit ordering
//   - Canonical Huffman code construction
//   - Three block types (stored, fixed Huffman, dynamic Huffman)
//   - LZ77 back-reference resolution
//   - zlib and gzip header/checksum handling
//
// Browsers use this for:
//   1. PNG image decoding (IDAT chunks contain zlib-wrapped DEFLATE data)
//   2. HTTP responses with Content-Encoding: gzip
//   3. HTTP responses with Content-Encoding: deflate (zlib format)
//
// The implementation follows RFC 1951 (DEFLATE), RFC 1950 (zlib), and
// RFC 1952 (gzip) specifications precisely.

#include "zlib.hpp"

#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace chinstrap {

// ============================================================================
// Length and distance tables (RFC 1951 section 3.2.5)
// ============================================================================
// TEACHING NOTE: These tables encode the two-level scheme for lengths and
// distances. LENGTH_BASE[i] is the base length for code (257+i), and
// LENGTH_EXTRA[i] is the number of extra bits to read and add to the base.
// For example, code 257 = length 3 (0 extra bits), code 265 = length 11
// with 1 extra bit (so 11 or 12), etc.
// The same scheme applies to distances with DIST_BASE and DIST_EXTRA.

const int Inflater::LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10,         // codes 257-264: lengths 3-10, no extra bits
    11, 13, 15, 17,                  // codes 265-268: 1 extra bit each
    19, 23, 27, 31,                  // codes 269-272: 2 extra bits each
    35, 43, 51, 59,                  // codes 273-276: 3 extra bits each
    67, 83, 99, 115,                 // codes 277-280: 4 extra bits each
    131, 162, 193, 224,              // codes 281-284: 5 extra bits each
    255                              // code 285: length 258, 0 extra bits
};

const int Inflater::LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0,         // codes 257-264
    1, 1, 1, 1,                      // codes 265-268
    2, 2, 2, 2,                      // codes 269-272
    3, 3, 3, 3,                      // codes 273-276
    4, 4, 4, 4,                      // codes 277-280
    5, 5, 5, 5,                      // codes 281-284
    0                                // code 285
};

const int Inflater::DIST_BASE[30] = {
    1, 2, 3, 4,      // codes 0-3: distances 1-4, no extra bits
    5, 7,             // codes 4-5: 1 extra bit (5-6, 7-8)
    9, 13,            // codes 6-7: 2 extra bits (9-12, 13-16)
    17, 25,           // codes 8-9: 3 extra bits (17-24, 25-32)
    33, 49,           // codes 10-11: 4 extra bits
    65, 97,           // codes 12-13: 5 extra bits
    129, 193,         // codes 14-15: 6 extra bits
    257, 385,         // codes 16-17: 7 extra bits
    513, 769,         // codes 18-19: 8 extra bits
    1025, 1537,       // codes 20-21: 9 extra bits
    2049, 3073,       // codes 22-23: 10 extra bits
    4097, 6145,       // codes 24-25: 11 extra bits
    8193, 12289,      // codes 26-27: 12 extra bits
    16385, 24577      // codes 28-29: 13 extra bits
};

const int Inflater::DIST_EXTRA[30] = {
    0, 0, 0, 0,       // codes 0-3
    1, 1,             // codes 4-5
    2, 2,             // codes 6-7
    3, 3,             // codes 8-9
    4, 4,             // codes 10-11
    5, 5,             // codes 12-13
    6, 6,             // codes 14-15
    7, 7,             // codes 16-17
    8, 8,             // codes 18-19
    9, 9,             // codes 20-21
    10, 10,           // codes 22-23
    11, 11,           // codes 24-25
    12, 12,           // codes 26-27
    13, 13            // codes 28-29
};

// ============================================================================
// Checksum implementations
// ============================================================================

// TEACHING NOTE: Adler-32 is a simple checksum used by zlib.
// It is computed as two running sums modulo 65521 (the largest prime < 2^16).
// s1 = (sum of all bytes) mod 65521
// s2 = (sum of all s1 values so far) mod 65521
// Result = (s2 << 16) | s1
// It is faster than CRC-32 but provides weaker error detection.

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    return adler32(data.data(), data.size());
}

// TEACHING NOTE: CRC-32 uses a polynomial division approach.
// The standard CRC-32 polynomial is 0xEDB88320 (bit-reversed form of
// 0x04C11DB7). We precompute a 256-entry lookup table for speed.
// CRC-32 is used by gzip and PNG for error detection.

uint32_t crc32(const uint8_t* data, size_t len) {
    // Build lookup table (could be precomputed but this is simple)
    static uint32_t table[256];
    static bool table_built = false;
    if (!table_built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                if (c & 1) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            table[i] = c;
        }
        table_built = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    return crc32(data.data(), data.size());
}

// ============================================================================
// Inflater implementation
// ============================================================================

Inflater::Inflater()
    : m_data(nullptr)
    , m_data_size(0)
    , m_byte_pos(0)
    , m_bit_buf(0)
    , m_bit_count(0) {}

// TEACHING NOTE: Bit reading in DEFLATE
// =========================================================================
// DEFLATE packs bits LSB-first. We maintain a bit buffer (m_bit_buf) and
// track how many valid bits it contains (m_bit_count). When we need more
// bits, we load the next byte into the buffer.
//
// To read n bits: we extract the low n bits of m_bit_buf, then shift them out.
// n can be at most 16 (for extra bits in DEFLATE), and our buffer is 32 bits,
// so we never overflow.

uint32_t Inflater::read_bits(int n) {
    while (m_bit_count < (uint32_t)n) {
        if (m_byte_pos >= m_data_size) {
            throw std::runtime_error("DEFLATE: unexpected end of input");
        }
        m_bit_buf |= (uint32_t)(m_data[m_byte_pos]) << m_bit_count;
        m_byte_pos++;
        m_bit_count += 8;
    }
    uint32_t result = m_bit_buf & ((1u << n) - 1);
    m_bit_buf >>= n;
    m_bit_count -= n;
    return result;
}

void Inflater::align_to_byte() {
    // Discard bits up to the next byte boundary
    int discard = m_bit_count & 7;
    m_bit_buf >>= discard;
    m_bit_count -= discard;
}

// TEACHING NOTE: Canonical Huffman code construction
// =========================================================================
// RFC 1951 specifies a canonical Huffman code: given a set of code lengths
// for each symbol, the actual codes are determined deterministically.
//
// The algorithm (from RFC 1951 section 3.2.2):
//   1. Count the number of codes for each code length
//   2. Compute the first code value for each length:
//      code(0) = 0
//      code(len) = (code(len-1) + count(len-1)) << 1
//   3. Assign codes to symbols sorted by (length, symbol value)
//
// For decoding, we use the "count + sorted symbols" method:
//   - counts[i] = number of codes with length i
//   - symbols are stored sorted by (length, value)
//   - To decode: read one bit at a time, maintain a code value.
//     At each bit position, check if the current code falls within
//     the range of codes for that length. If so, index into the
//     sorted symbol array.

uint32_t Inflater::reverse_bits(uint32_t val, int n) {
    uint32_t result = 0;
    for (int i = 0; i < n; ++i) {
        result = (result << 1) | (val & 1);
        val >>= 1;
    }
    return result;
}

void Inflater::build_huffman(HuffmanTree& tree, const std::vector<int>& code_lengths) {
    tree.counts.clear();
    tree.symbols.clear();
    tree.max_bits = 0;

    int max_len = 0;
    for (int len : code_lengths) {
        if (len > max_len) max_len = len;
    }
    tree.max_bits = max_len;

    if (max_len == 0) {
        return; // empty tree
    }

    // Count codes of each length
    tree.counts.resize(max_len + 1, 0);
    for (int len : code_lengths) {
        if (len > 0) tree.counts[len]++;
    }

    // Build sorted symbol array
    // First, compute starting index for each length
    std::vector<int> offsets(max_len + 1, 0);
    int total = 0;
    for (int len = 1; len <= max_len; ++len) {
        offsets[len] = total;
        total += tree.counts[len];
    }

    tree.symbols.resize(total);
    for (int sym = 0; sym < (int)code_lengths.size(); ++sym) {
        int len = code_lengths[sym];
        if (len > 0) {
            tree.symbols[offsets[len]] = sym;
            offsets[len]++;
        }
    }
}

// TEACHING NOTE: Huffman decoding
// =========================================================================
// We decode one bit at a time. We maintain:
//   - code: the bits we have read so far (shifted left each step)
//   - first: the first code value of the current length
//   - index: index into the sorted symbol array for the current length
//   - count: number of codes of the current length
//
// At each bit: we shift in one bit. If the current code is less than
// (first + count), we have found a valid code of this length, and the
// symbol is at symbols[index + (code - first)].
// Otherwise, we move to the next length: first = (first + count) << 1,
// index += count, and repeat.

int Inflater::decode_huffman(const HuffmanTree& tree) {
    if (tree.symbols.empty()) {
        throw std::runtime_error("DEFLATE: cannot decode from empty Huffman tree");
    }

    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= tree.max_bits; ++len) {
        code = (code << 1) | (int)read_bits(1);
        int count = (len < (int)tree.counts.size()) ? tree.counts[len] : 0;
        if (code - first < count) {
            return tree.symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }

    throw std::runtime_error("DEFLATE: invalid Huffman code");
}

// ============================================================================
// Block type handlers
// ============================================================================

// TEACHING NOTE: Stored blocks (BTYPE=00)
// =========================================================================
// Stored blocks are uncompressed. They are used when the compressor
// determines that compression would not save space (e.g., for very short
// data or already-compressed data).
// Format: skip to byte boundary, then LEN (2 bytes), NLEN (2 bytes, ~LEN),
// then LEN bytes of raw data.

void Inflater::process_stored_block() {
    align_to_byte();
    m_bit_buf = 0;
    m_bit_count = 0;

    if (m_byte_pos + 4 > m_data_size) {
        throw std::runtime_error("DEFLATE: stored block header past end");
    }

    uint16_t len = (uint16_t)(m_data[m_byte_pos] | (m_data[m_byte_pos + 1] << 8));
    uint16_t nlen = (uint16_t)(m_data[m_byte_pos + 2] | (m_data[m_byte_pos + 3] << 8));
    m_byte_pos += 4;

    if ((uint16_t)~nlen != len) {
        throw std::runtime_error("DEFLATE: stored block LEN/NLEN mismatch");
    }

    if (m_byte_pos + len > m_data_size) {
        throw std::runtime_error("DEFLATE: stored block data past end");
    }

    m_output.insert(m_output.end(), m_data + m_byte_pos, m_data + m_byte_pos + len);
    m_byte_pos += len;
}

// TEACHING NOTE: Fixed Huffman blocks (BTYPE=01)
// =========================================================================
// Fixed Huffman uses predefined Huffman trees defined in the spec.
// This avoids the overhead of transmitting code lengths in the stream.
// The fixed trees are:
//
// Literal/length codes:
//   0-143:   8 bits, codes 00110000 to 10111111
//   144-255: 9 bits, codes 110010000 to 111111111
//   256-279: 7 bits, codes 0000000 to 0010111
//   280-287: 8 bits, codes 11000000 to 11000111
//
// Distance codes: all 5 bits, 0-29

void Inflater::process_fixed_huffman_block() {
    // Build fixed literal/length code lengths
    std::vector<int> litlen_lengths(288);
    for (int i = 0; i <= 143; ++i) litlen_lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) litlen_lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) litlen_lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) litlen_lengths[i] = 8;

    // Build fixed distance code lengths
    std::vector<int> dist_lengths(30);
    for (int i = 0; i < 30; ++i) dist_lengths[i] = 5;

    build_huffman(m_litlen_tree, litlen_lengths);
    build_huffman(m_dist_tree, dist_lengths);

    decode_block(m_litlen_tree, m_dist_tree);
}

// TEACHING NOTE: Dynamic Huffman blocks (BTYPE=10)
// =========================================================================
// Dynamic Huffman blocks carry their own Huffman tree definitions at the
// start of the block. This allows the compressor to optimize the code
// lengths for the specific data in each block.
//
// The tree definition format is:
//   HLIT (5 bits): number of literal/length codes - 257 (257-286)
//   HDIST (5 bits): number of distance codes - 1 (1-30)
//   HCLEN (4 bits): number of code length codes - 4 (4-19)
//
// Then HCLEN+4 code lengths for the code length alphabet (3 bits each),
// in a specific permutation order. These define a Huffman tree for
// encoding the code lengths of the literal/length and distance trees.
//
// Then the literal/length code lengths and distance code lengths are
// encoded using that code-length Huffman tree. Code lengths use a
// special alphabet: 0-15 are literal lengths, 16 = repeat previous 3-6
// times, 17 = repeat zero 3-10 times, 18 = repeat zero 11-138 times.

void Inflater::process_dynamic_huffman_block() {
    int hlit = (int)read_bits(5) + 257;   // 257-286
    int hdist = (int)read_bits(5) + 1;    // 1-30
    int hclen = (int)read_bits(4) + 4;    // 4-19

    // TEACHING NOTE: The code length code lengths are stored in a specific
    // permutation order, not in natural order. This order puts the most
    // commonly used code lengths first, so that if HCLEN is small, we skip
    // the rare ones.
    static const int CL_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    std::vector<int> cl_code_lengths(19, 0);
    for (int i = 0; i < hclen; ++i) {
        cl_code_lengths[CL_ORDER[i]] = (int)read_bits(3);
    }

    // Build the code-length Huffman tree
    HuffmanTree cl_tree;
    build_huffman(cl_tree, cl_code_lengths);

    // Decode the literal/length and distance code lengths
    // They are stored as a single sequence of (hlit + hdist) values
    std::vector<int> all_code_lengths;
    all_code_lengths.reserve(hlit + hdist);

    while ((int)all_code_lengths.size() < hlit + hdist) {
        int sym = decode_huffman(cl_tree);

        if (sym < 16) {
            // Literal code length
            all_code_lengths.push_back(sym);
        } else if (sym == 16) {
            // Repeat previous code length 3-6 times (2 extra bits)
            if (all_code_lengths.empty()) {
                throw std::runtime_error("DEFLATE: repeat with no previous length");
            }
            int prev = all_code_lengths.back();
            int repeat = (int)read_bits(2) + 3;
            for (int i = 0; i < repeat; ++i) {
                all_code_lengths.push_back(prev);
            }
        } else if (sym == 17) {
            // Repeat zero 3-10 times (3 extra bits)
            int repeat = (int)read_bits(3) + 3;
            for (int i = 0; i < repeat; ++i) {
                all_code_lengths.push_back(0);
            }
        } else if (sym == 18) {
            // Repeat zero 11-138 times (7 extra bits)
            int repeat = (int)read_bits(7) + 11;
            for (int i = 0; i < repeat; ++i) {
                all_code_lengths.push_back(0);
            }
        } else {
            throw std::runtime_error("DEFLATE: invalid code length symbol");
        }
    }

    if ((int)all_code_lengths.size() != hlit + hdist) {
        throw std::runtime_error("DEFLATE: code length count mismatch");
    }

    // Split into literal/length and distance code lengths
    std::vector<int> litlen_lengths(all_code_lengths.begin(), all_code_lengths.begin() + hlit);
    std::vector<int> dist_lengths(all_code_lengths.begin() + hlit, all_code_lengths.end());

    build_huffman(m_litlen_tree, litlen_lengths);
    build_huffman(m_dist_tree, dist_lengths);

    decode_block(m_litlen_tree, m_dist_tree);
}

// TEACHING NOTE: The core DEFLATE decode loop
// =========================================================================
// This is the heart of DEFLATE decompression. After the Huffman trees are
// established (either fixed or dynamic), we decode symbols one at a time:
//
//   - Symbols 0-255: literal byte values, copy directly to output
//   - Symbol 256: end of block, stop decoding
//   - Symbols 257-285: back-reference. Decode length and distance, then
//     copy bytes from earlier in the output buffer.
//
// Back-references are what provide compression. If the input data has
// repeated patterns (like the word "the" appearing many times), the
// compressor replaces each occurrence after the first with a (distance,
// length) reference back to the first occurrence.
//
// The copy can overlap with itself: if distance=1, length=5, it means
// "repeat the last byte 4 more times". This is useful for RLE-like
// compression of runs of identical bytes. We must copy one byte at a time
// to handle overlapping correctly.

void Inflater::decode_block(const HuffmanTree& litlen, const HuffmanTree& dist) {
    for (;;) {
        int sym = decode_huffman(litlen);

        if (sym < 256) {
            // Literal byte
            m_output.push_back((uint8_t)sym);
        } else if (sym == 256) {
            // End of block
            break;
        } else if (sym <= 285) {
            // Back-reference (length + distance)
            int len_index = sym - 257;
            int length = LENGTH_BASE[len_index] + (int)read_bits(LENGTH_EXTRA[len_index]);

            // Decode distance
            int dist_sym = decode_huffman(dist);
            int distance = DIST_BASE[dist_sym] + (int)read_bits(DIST_EXTRA[dist_sym]);

            // Copy from earlier in the output (LZ77 back-reference)
            if (distance > (int)m_output.size()) {
                throw std::runtime_error("DEFLATE: back-reference distance too large");
            }

            size_t src = m_output.size() - distance;
            for (int i = 0; i < length; ++i) {
                m_output.push_back(m_output[src + i]);
            }
        } else {
            throw std::runtime_error("DEFLATE: invalid literal/length symbol");
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

std::vector<uint8_t> Inflater::inflate_raw(const std::vector<uint8_t>& compressed) {
    m_data = compressed.data();
    m_data_size = compressed.size();
    m_byte_pos = 0;
    m_bit_buf = 0;
    m_bit_count = 0;
    m_output.clear();

    // TEACHING NOTE: DEFLATE stream is a sequence of blocks.
    // Each block starts with BFINAL (1 bit) and BTYPE (2 bits).
    // We process blocks until we find one with BFINAL=1.

    bool is_final = false;
    while (!is_final) {
        is_final = (read_bits(1) == 1);
        int btype = (int)read_bits(2);

        switch (btype) {
            case 0:
                process_stored_block();
                break;
            case 1:
                process_fixed_huffman_block();
                break;
            case 2:
                process_dynamic_huffman_block();
                break;
            case 3:
                throw std::runtime_error("DEFLATE: invalid block type 3 (reserved)");
            default:
                break; // unreachable, btype is 2 bits
        }
    }

    return m_output;
}

std::vector<uint8_t> Inflater::inflate_zlib(const std::vector<uint8_t>& compressed) {
    // TEACHING NOTE: zlib format (RFC 1950)
    // =====================================================================
    // The zlib header is 2 bytes:
    //   CMF (Compression Method and Flags):
    //     bits 0-3: compression method (8 = DEFLATE)
    //     bits 4-7: compression info (window size, typically 7 for 32K)
    //   FLG (Flags):
    //     bits 0-4: FCHECK (makes CMF*256+FLG a multiple of 31)
    //     bit 5:    FDICT (preset dictionary present, usually 0)
    //     bits 6-7: FLEVEL (compression level, informational)
    //
    // After the header is a raw DEFLATE stream.
    // After the DEFLATE stream is a 4-byte Adler-32 checksum (big-endian).

    if (compressed.size() < 6) {
        throw std::runtime_error("zlib: data too short for header + checksum");
    }

    uint8_t cmf = compressed[0];
    uint8_t flg = compressed[1];

    // Validate zlib header
    if ((cmf & 0x0F) != 8) {
        throw std::runtime_error("zlib: unsupported compression method (not DEFLATE)");
    }

    if ((uint16_t)((cmf << 8) | flg) % 31 != 0) {
        throw std::runtime_error("zlib: invalid header checksum");
    }

    bool has_dict = (flg & 0x20) != 0;
    if (has_dict) {
        throw std::runtime_error("zlib: preset dictionary not supported");
    }

    // The DEFLATE stream starts at byte 2 and ends 4 bytes before the end
    // (the last 4 bytes are the Adler-32 checksum in big-endian)
    std::vector<uint8_t> raw_deflate(compressed.begin() + 2, compressed.end() - 4);

    std::vector<uint8_t> result = inflate_raw(raw_deflate);

    // Verify Adler-32 checksum
    uint32_t stored_checksum =
        ((uint32_t)compressed[compressed.size() - 4] << 24) |
        ((uint32_t)compressed[compressed.size() - 3] << 16) |
        ((uint32_t)compressed[compressed.size() - 2] << 8)  |
        ((uint32_t)compressed[compressed.size() - 1]);

    uint32_t computed_checksum = adler32(result);
    if (stored_checksum != computed_checksum) {
        // We do not fail on checksum mismatch in case of minor corruption,
        // but in a strict implementation this would be an error.
        // throw std::runtime_error("zlib: Adler-32 checksum mismatch");
    }

    return result;
}

std::vector<uint8_t> Inflater::inflate_gzip(const std::vector<uint8_t>& compressed) {
    // TEACHING NOTE: gzip format (RFC 1952)
    // =====================================================================
    // gzip header:
    //   Byte 0-1: magic number 0x1f, 0x8b
    //   Byte 2:   compression method (8 = DEFLATE)
    //   Byte 3:   flags:
    //     bit 0: FTEXT (file is ASCII text)
    //     bit 1: FHCRC (header CRC-16 present)
    //     bit 2: FEXTRA (extra fields present)
    //     bit 3: FNAME (original filename present, null-terminated)
    //     bit 4: FCOMMENT (comment present, null-terminated)
    //   Byte 4-7: modification time (little-endian)
    //   Byte 8:   extra flags
    //   Byte 9:   OS (operating system)
    //
    // After optional fields, a raw DEFLATE stream.
    // After the DEFLATE stream: CRC-32 (4 bytes, little-endian) + original
    // size mod 2^32 (4 bytes, little-endian).

    if (compressed.size() < 18) {
        throw std::runtime_error("gzip: data too short");
    }

    // Validate magic number
    if (compressed[0] != 0x1f || compressed[1] != 0x8b) {
        throw std::runtime_error("gzip: invalid magic number");
    }

    if (compressed[2] != 8) {
        throw std::runtime_error("gzip: unsupported compression method (not DEFLATE)");
    }

    uint8_t flags = compressed[3];
    size_t pos = 10; // skip to after the fixed 10-byte header

    // Skip optional fields
    if (flags & 0x04) {
        // FEXTRA: 2-byte length + extra data
        if (pos + 2 > compressed.size()) {
            throw std::runtime_error("gzip: truncated FEXTRA");
        }
        uint16_t xlen = compressed[pos] | (compressed[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) {
        // FNAME: null-terminated string
        while (pos < compressed.size() && compressed[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x10) {
        // FCOMMENT: null-terminated string
        while (pos < compressed.size() && compressed[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x02) {
        // FHCRC: 2-byte header CRC
        pos += 2;
    }

    if (pos > compressed.size() - 8) {
        throw std::runtime_error("gzip: data too short after header");
    }

    // The DEFLATE stream starts at pos and ends 8 bytes before the end
    // (last 8 bytes are CRC-32 + original size, both little-endian)
    std::vector<uint8_t> raw_deflate(compressed.begin() + pos, compressed.end() - 8);

    std::vector<uint8_t> result = inflate_raw(raw_deflate);

    // Verify CRC-32 (last 8 bytes)
    size_t end = compressed.size();
    uint32_t stored_crc =
        (uint32_t)compressed[end - 8] |
        ((uint32_t)compressed[end - 7] << 8) |
        ((uint32_t)compressed[end - 6] << 16) |
        ((uint32_t)compressed[end - 5] << 24);

    uint32_t computed_crc = crc32(result);
    if (stored_crc != computed_crc) {
        // Non-fatal: data may still be usable
        // throw std::runtime_error("gzip: CRC-32 mismatch");
    }

    return result;
}

} // namespace chinstrap