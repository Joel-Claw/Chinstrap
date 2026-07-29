// image.cpp - Image decoders (PNG, JPEG, GIF) from scratch
//
// TEACHING NOTE: This file implements three image decoders from scratch:
//   1. PNG decoder: parses PNG chunks, decompresses IDAT with our zlib
//      inflater, applies filter reconstruction, outputs RGBA.
//   2. JPEG decoder: parses JPEG markers, builds Huffman tables, performs
//      IDCT (Inverse Discrete Cosine Transform), converts YCbCr to RGB.
//   3. GIF decoder: parses GIF blocks, performs LZW decompression, applies
//      color palette, outputs RGBA.
//
// Each decoder is self-contained and uses only our own zlib decompressor
// for PNG. No external image libraries (libpng, libjpeg, giflib) are used.

#include "image.hpp"
#include "zlib.hpp"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <algorithm>

namespace chinstrap {

// ============================================================================
// Helpers
// ============================================================================

static uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

// ============================================================================
// Format detection
// ============================================================================

std::string detect_format(const std::vector<uint8_t>& data) {
    if (data.size() < 6) return "unknown";

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (data.size() >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return "png";
    }

    // JPEG: FF D8 FF
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "jpeg";
    }

    // GIF: "GIF8"
    if (data.size() >= 4 && data[0] == 'G' && data[1] == 'I' &&
        data[2] == 'F' && data[3] == '8') {
        return "gif";
    }

    return "unknown";
}

Image load_image(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return Image();

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return Image();
    }

    std::vector<uint8_t> data(size);
    if (fread(data.data(), 1, size, fp) != (size_t)size) {
        fclose(fp);
        return Image();
    }
    fclose(fp);

    return load_image_from_memory(data);
}

Image load_image_from_memory(const std::vector<uint8_t>& data) {
    std::string fmt = detect_format(data);

    if (fmt == "png") return decode_png(data);
    if (fmt == "jpeg") return decode_jpeg(data);
    if (fmt == "gif") return decode_gif(data);

    return Image();
}

// ============================================================================
// PNG decoder
// ============================================================================
//
// TEACHING NOTE: PNG file format
// =========================================================================
// PNG (Portable Network Graphics, RFC 2083) is a lossless image format.
//
// File structure:
//   1. 8-byte signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A
//   2. Series of chunks, each with:
//      - 4 bytes: length (big-endian)
//      - 4 bytes: type (ASCII, e.g. "IHDR", "IDAT", "IEND")
//      - length bytes: data
//      - 4 bytes: CRC-32 (of type + data)
//
// Key chunks:
//   IHDR: image header (width, height, bit depth, color type, etc.)
//   IDAT: compressed image data (zlib/DEFLATE, multiple IDATs concatenated)
//   PLTE: palette (for palette images)
//   tRNS: transparency info
//   IEND: end of file marker
//
// Color types:
//   0: grayscale (1 channel)
//   2: RGB (3 channels)
//   3: palette (1 channel, index into PLTE)
//   4: grayscale + alpha (2 channels)
//   6: RGBA (4 channels)
//
// Filter types (applied to each scanline before compression):
//   0: None - raw bytes
//   1: Sub - each byte is delta from the byte to the left
//   2: Up - each byte is delta from the byte above
//   3: Average - average of left and up
//   4: Paeth - linear predictor (best predictor based on neighbors)
//
// The PNG spec uses "filtering" (not "filter" in the DSP sense) to make
// the data more compressible. Each scanline can use a different filter.
// We must reconstruct the original bytes by reversing the filter.

Image decode_png(const std::vector<uint8_t>& data) {
    Image img;

    if (data.size() < 8) return img;

    // Verify signature
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(data.data(), png_sig, 8) != 0) return img;

    size_t pos = 8;

    // Image header fields
    int width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<uint8_t> idat_data;
    std::vector<uint8_t> palette;     // PLTE chunk (for color type 3)
    std::vector<uint8_t> trns_data;  // tRNS chunk (transparency)

    while (pos + 8 <= data.size()) {
        uint32_t chunk_len = read_be32(&data[pos]);
        std::string chunk_type((const char*)&data[pos + 4], 4);
        size_t chunk_data_start = pos + 8;

        if (chunk_data_start + chunk_len + 4 > data.size()) break;

        const uint8_t* chunk_data_ptr = &data[chunk_data_start];

        if (chunk_type == "IHDR") {
            // Image header
            width = (int)read_be32(chunk_data_ptr);
            height = (int)read_be32(chunk_data_ptr + 4);
            bit_depth = chunk_data_ptr[8];
            color_type = chunk_data_ptr[9];
            // chunk_data_ptr[10] = compression method (always 0)
            // chunk_data_ptr[11] = filter method (always 0)
            interlace = chunk_data_ptr[12];
        } else if (chunk_type == "IDAT") {
            // Compressed image data - append to buffer
            idat_data.insert(idat_data.end(), chunk_data_ptr, chunk_data_ptr + chunk_len);
        } else if (chunk_type == "PLTE") {
            palette.assign(chunk_data_ptr, chunk_data_ptr + chunk_len);
        } else if (chunk_type == "tRNS") {
            trns_data.assign(chunk_data_ptr, chunk_data_ptr + chunk_len);
        } else if (chunk_type == "IEND") {
            break;
        }

        pos = chunk_data_start + chunk_len + 4;  // skip data + CRC
    }

    if (width <= 0 || height <= 0 || idat_data.empty()) return img;

    // TEACHING NOTE: Decompressing the image data
    // =================================================================
    // All IDAT chunks contain a single zlib stream that spans all IDAT
    // chunks. We concatenate the IDAT data and decompress it with our
    // zlib inflater. The decompressed data contains one filter byte
    // per scanline, followed by the filtered pixel data.

    Inflater inflater;
    std::vector<uint8_t> raw;
    try {
        raw = inflater.inflate_zlib(idat_data);
    } catch (...) {
        return img;
    }

    // TEACHING NOTE: Unfiltering
    // =================================================================
    // PNG applies a per-scanline filter to improve compression. Each
    // scanline starts with a filter type byte, followed by the filtered
    // pixel data. We reverse the filter to get the original pixel data.
    //
    // For filter type 0 (None): no change
    // For filter type 1 (Sub): pixel = filtered + pixel_to_the_left
    // For filter type 2 (Up): pixel = filtered + pixel_above
    // For filter type 3 (Average): pixel = filtered + (left + up) / 2
    // For filter type 4 (Paeth): pixel = filtered + Paeth(left, up, up_left)
    //
    // The Paeth predictor is defined as:
    //   p = left + up - up_left
    //   pa = abs(p - left), pb = abs(p - up), pc = abs(p - up_left)
    //   if pa <= pb and pa <= pc: predict = left
    //   elif pb <= pc: predict = up
    //   else: predict = up_left

    // Determine channels per pixel
    int channels;
    switch (color_type) {
        case 0: channels = 1; break;  // grayscale
        case 2: channels = 3; break;  // RGB
        case 3: channels = 1; break;  // palette index
        case 4: channels = 2; break;  // grayscale + alpha
        case 6: channels = 4; break;  // RGBA
        default: return img;
    }

    int bpp = channels * (bit_depth / 8);  // bytes per pixel
    int stride = width * bpp;              // bytes per scanline

    // For sub-byte bit depths, stride is calculated differently
    if (bit_depth < 8) {
        stride = (width * bit_depth * channels + 7) / 8;
    }

    if (interlace != 0) {
        // Adam7 interlacing not implemented for simplicity
        // Most PNGs are non-interlaced
        return img;
    }

    // Allocate unfiltered pixel data
    std::vector<uint8_t> unfiltered((size_t)(stride + 1) * height);

    // Paeth predictor function
    auto paeth = [](int a, int b, int c) -> int {
        int p = a + b - c;
        int pa = std::abs(p - a);
        int pb = std::abs(p - b);
        int pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        if (pb <= pc) return b;
        return c;
    };

    for (int y = 0; y < height; ++y) {
        size_t row_start = (size_t)(y) * (stride + 1);
        if (row_start + stride + 1 > raw.size()) break;

        uint8_t filter_type = raw[row_start];
        const uint8_t* filtered = &raw[row_start + 1];
        uint8_t* output = &unfiltered[(size_t)(y) * (stride + 1) + 1];

        // Previous row
        uint8_t* prev_row = (y > 0) ? &unfiltered[(size_t)(y - 1) * (stride + 1) + 1] : nullptr;

        for (int x = 0; x < stride; ++x) {
            int left = (x >= bpp) ? output[x - bpp] : 0;
            int up = prev_row ? prev_row[x] : 0;
            int up_left = (prev_row && x >= bpp) ? prev_row[x - bpp] : 0;

            switch (filter_type) {
                case 0: // None
                    output[x] = filtered[x];
                    break;
                case 1: // Sub
                    output[x] = (uint8_t)(filtered[x] + left);
                    break;
                case 2: // Up
                    output[x] = (uint8_t)(filtered[x] + up);
                    break;
                case 3: // Average
                    output[x] = (uint8_t)(filtered[x] + (left + up) / 2);
                    break;
                case 4: // Paeth
                    output[x] = (uint8_t)(filtered[x] + paeth(left, up, up_left));
                    break;
                default:
                    output[x] = filtered[x];
                    break;
            }
        }

        // Store the filter type byte too (for reference)
        unfiltered[(size_t)y * (stride + 1)] = filter_type;
    }

    // TEACHING NOTE: Converting to RGBA
    // =================================================================
    // We convert all supported color types to 32-bit RGBA for the display
    // layer. This handles:
    //   - Grayscale (0): R=G=B=gray, A=255
    //   - RGB (2): A=255
    //   - Palette (3): look up in PLTE
    //   - Grayscale+Alpha (4): R=G=B=gray
    //   - RGBA (6): direct copy
    //
    // For sub-byte bit depths (1, 2, 4), we also need to extract individual
    // pixel values from packed bytes.

    img.width = width;
    img.height = height;
    img.pixels.resize((size_t)width * height * 4);

    auto get_byte = [&](int x, int y, int ch) -> uint8_t {
        size_t row_start = (size_t)(y) * (stride + 1) + 1;
        if (bit_depth == 8) {
            return unfiltered[row_start + x * bpp + ch];
        } else if (bit_depth == 16) {
            // 16-bit: take the high byte (for display, 8-bit is enough)
            return unfiltered[row_start + x * bpp + ch * 2];
        }
        // Sub-byte bit depths (1, 2, 4) - only for grayscale and palette
        uint8_t byte = unfiltered[row_start + (x * bit_depth) / 8];
        int shift = 8 - bit_depth - ((x * bit_depth) % 8);
        uint8_t mask = (uint8_t)((1 << bit_depth) - 1);
        return (uint8_t)((byte >> shift) & mask);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t r, g, b, a;
            r = g = b = 0;
            a = 255;

            switch (color_type) {
                case 0: {  // Grayscale
                    uint8_t gray = get_byte(x, y, 0);
                    r = g = b = gray;
                    break;
                }
                case 2: {  // RGB
                    r = get_byte(x, y, 0);
                    g = get_byte(x, y, 1);
                    b = get_byte(x, y, 2);
                    break;
                }
                case 3: {  // Palette
                    uint8_t idx = get_byte(x, y, 0);
                    if ((int)idx * 3 + 2 < (int)palette.size()) {
                        r = palette[idx * 3];
                        g = palette[idx * 3 + 1];
                        b = palette[idx * 3 + 2];
                    }
                    // Check tRNS for palette transparency
                    if (!trns_data.empty() && idx < trns_data.size()) {
                        a = trns_data[idx];
                    }
                    break;
                }
                case 4: {  // Grayscale + Alpha
                    uint8_t gray = get_byte(x, y, 0);
                    a = get_byte(x, y, 1);
                    r = g = b = gray;
                    break;
                }
                case 6: {  // RGBA
                    r = get_byte(x, y, 0);
                    g = get_byte(x, y, 1);
                    b = get_byte(x, y, 2);
                    a = get_byte(x, y, 3);
                    break;
                }
                default:
                    break;
            }

            img.set_pixel(x, y, r, g, b, a);
        }
    }

    return img;
}

// ============================================================================
// JPEG decoder (baseline, sequential)
// ============================================================================
//
// TEACHING NOTE: JPEG file format and decoding
// =========================================================================
// JPEG (Joint Photographic Experts Group) is the most common format for
// photographs on the web. We implement baseline (sequential) JPEG, which
// is the most common type. Progressive JPEG (which uses spectral
// selection) is not implemented.
//
// File structure:
//   SOI marker: FF D8 (start of image)
//   APP0 marker: FF E0 (JFIF header, usually)
//   DQT marker: FF DB (Define Quantization Table)
//   SOF0 marker: FF C0 (Start of Frame, baseline DCT)
//   DHT marker: FF C4 (Define Huffman Table)
//   SOS marker: FF DA (Start of Scan - compressed data follows)
//   EOI marker: FF D9 (End of image)
//
// The decoding process:
//   1. Parse markers to get image dimensions, components, quantization
//      tables, and Huffman tables.
//   2. Read entropy-coded data (Huffman + run-length encoding).
//   3. For each 8x8 block:
//      a. DC coefficient: Huffman decode + differential decode
//      b. AC coefficients: Huffman decode + run-length decode
//      c. Dequantize: multiply by quantization table
//      d. IDCT (Inverse Discrete Cosine Transform): convert frequency
//         domain to spatial domain
//      e. Level shift: add 128 to bring range from [-128,127] to [0,255]
//   4. Convert YCbCr to RGB (most JPEGs use YCbCr color space)
//   5. Handle subsampling (e.g. 4:2:0 chroma subsampling)

namespace jpeg {

// JPEG component info
struct Component {
    int id;
    int h_sampling;  // horizontal sampling factor
    int v_sampling;  // vertical sampling factor
    int quant_table_id;
    int dc_table_id;
    int ac_table_id;
};

// Huffman table
struct HuffmanTable {
    int lengths[16];      // number of codes of each length
    int values[256];      // symbol values
    int count;            // total number of symbols
    int max_code[16];     // largest code of each length
    int val_offset[16];   // offset into values for each length
};

// Quantization table
struct QuantTable {
    int precision;        // 0 = 8-bit, 1 = 16-bit
    int values[64];        // quantization values (zigzag order)
};

// 64-entry JPEG zigzag order (maps zigzag index to natural row-major index)
static const int ZIGZAG[64] = {
    0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// JPEG decoder state
struct Decoder {
    const uint8_t* data;
    size_t data_size;
    size_t pos;
    int bit_buf;
    int bit_count;

    int width;
    int height;
    int num_components;
    Component components[4];

    QuantTable quant_tables[4];
    HuffmanTable dc_tables[4];
    HuffmanTable ac_tables[4];

    int prev_dc[4];  // previous DC coefficient for each component

    // --- Bit reading ---
    void init_bits() { bit_buf = 0; bit_count = 0; }

    int read_bits(int n) {
        while (bit_count < n) {
            if (pos >= data_size) return 0;
            int byte = data[pos++];
            if (byte == 0xFF) {
                // Skip padding (FF 00)
                if (pos < data_size && data[pos] == 0x00) {
                    pos++;
                } else {
                    // Marker found - error or end of data
                    return 0;
                }
            }
            bit_buf = (bit_buf << 8) | byte;
            bit_count += 8;
        }
        int result = (bit_buf >> (bit_count - n)) & ((1 << n) - 1);
        bit_count -= n;
        return result;
    }

    // --- Huffman decoding ---
    // TEACHING NOTE: JPEG Huffman decoding
    // =================================================================
    // JPEG Huffman tables are specified in the DHT marker. Each table has
    // a 16-entry array of code counts (BITS) and an array of symbol values
    // (HUFFVAL). The codes are generated in canonical form (similar to
    // DEFLATE but MSB-first, unlike DEFLATE which is LSB-first).
    //
    // We decode by reading one bit at a time (MSB-first) and checking if
    // the accumulated code matches any code of the current length.

    int decode_huffman(const HuffmanTable& table) {
        int code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | read_bits(1);
            if (len < 16 && code <= table.max_code[len]) {
                int offset = table.val_offset[len] + (code - (table.max_code[len] - table.lengths[len]));
                if (offset >= 0 && offset < table.count) {
                    return table.values[offset];
                }
            }
        }
        // Try the last length
        if (table.lengths[15] > 0) {
            int offset = table.val_offset[15] + (code - (table.max_code[15] - table.lengths[15]));
            if (offset >= 0 && offset < table.count) {
                return table.values[offset];
            }
        }
        return 0;
    }

    // Extend a decoded value to the full range
    int extend(int value, int length) {
        if (length == 0) return 0;
        if (value < (1 << (length - 1))) {
            return value - (1 << length) + 1;
        }
        return value;
    }
};

// TEACHING NOTE: Inverse Discrete Cosine Transform (IDCT)
// =========================================================================
// The IDCT converts 8x8 frequency-domain coefficients back to spatial-domain
// pixels. The 2D IDCT is separable: we first do a 1D IDCT on each row, then
// a 1D IDCT on each column.
//
// The 1D IDCT formula:
//   f(x) = sum_{u=0}^{7} C(u) * F(u) * cos((2x+1)*u*pi/16)
// where C(0) = 1/sqrt(2), C(u) = 1 for u > 0
//
// The 2D IDCT:
//   f(x,y) = (1/4) * sum_{u,v} C(u)*C(v)*F(u,v)*cos((2x+1)*u*pi/16)*cos((2y+1)*v*pi/16)
//
// We use a simplified floating-point implementation. A production JPEG decoder
// would use fixed-point integer arithmetic for speed, but the floating-point
// version is correct and easier to understand.

static void idct_8x8(const double block[64], double out[64]) {
    static double cos_table[8][8];
    static bool cos_init = false;

    if (!cos_init) {
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                cos_table[x][u] = std::cos((2.0 * x + 1.0) * (double)u * 3.14159265358979 / 16.0);
            }
        }
        cos_init = true;
    }

    // 2D IDCT via row-column decomposition
    double temp[64];

    // Row transform
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u) {
                double cu = (u == 0) ? 0.70710678 : 1.0;
                sum += cu * block[y * 8 + u] * cos_table[x][u];
            }
            temp[y * 8 + x] = sum * 0.5;
        }
    }

    // Column transform
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            double sum = 0.0;
            for (int v = 0; v < 8; ++v) {
                double cv = (v == 0) ? 0.70710678 : 1.0;
                sum += cv * temp[v * 8 + x] * cos_table[y][v];
            }
            out[y * 8 + x] = sum * 0.5;
        }
    }
}

void parse_huffman_table(Decoder& dec, const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos + 17 <= len) {
        uint8_t tc_th = data[pos++];  // table class (0=DC, 1=AC) + table ID
        int table_class = (tc_th >> 4) & 0x0F;
        int table_id = tc_th & 0x0F;

        if (table_id > 3) break;

        HuffmanTable& table = (table_class == 0)
            ? dec.dc_tables[table_id]
            : dec.ac_tables[table_id];

        // Read 16 bytes of code lengths
        table.count = 0;
        for (int i = 0; i < 16; ++i) {
            table.lengths[i] = data[pos++];
            table.count += table.lengths[i];
        }

        // Read symbol values
        for (int i = 0; i < table.count && pos < len; ++i) {
            table.values[i] = data[pos++];
        }

        // Build lookup tables for fast decoding
        int code = 0;
        int offset = 0;
        for (int len = 1; len <= 16; ++len) {
            int num = table.lengths[len - 1];
            table.val_offset[len - 1] = offset;
            table.max_code[len - 1] = code + num - 1;
            code = (code + num) << 1;
            offset += num;
        }

        if (pos >= len) break;
    }
}

void parse_quant_table(Decoder& dec, const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos + 1 <= len) {
        uint8_t pq_tq = data[pos++];  // precision (0=8bit, 1=16bit) + table ID
        int precision = (pq_tq >> 4) & 0x0F;
        int table_id = pq_tq & 0x0F;

        if (table_id > 3) break;

        QuantTable& qt = dec.quant_tables[table_id];
        qt.precision = precision;

        for (int i = 0; i < 64; ++i) {
            if (precision == 0) {
                if (pos >= len) break;
                qt.values[i] = data[pos++];
            } else {
                if (pos + 1 >= len) break;
                qt.values[i] = read_be16(&data[pos]);
                pos += 2;
            }
        }
    }
}

} // namespace jpeg

Image decode_jpeg(const std::vector<uint8_t>& data) {
    Image img;

    if (data.size() < 4) return img;
    if (data[0] != 0xFF || data[1] != 0xD8) return img;

    jpeg::Decoder dec;
    dec.data = data.data();
    dec.data_size = data.size();
    dec.pos = 2;
    dec.num_components = 0;
    for (int i = 0; i < 4; ++i) dec.prev_dc[i] = 0;

    // Parse markers until we find SOS (start of scan)
    while (dec.pos + 1 < dec.data_size) {
        if (dec.data[dec.pos] != 0xFF) {
            dec.pos++;
            continue;
        }

        uint8_t marker = dec.data[dec.pos + 1];
        dec.pos += 2;

        // Markers without length
        if (marker == 0xD9) {  // EOI
            break;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            // RST markers have no length
            continue;
        }

        if (dec.pos + 2 > dec.data_size) break;
        uint16_t seg_len = read_be16(&dec.data[dec.pos]);
        const uint8_t* seg_data = &dec.data[dec.pos + 2];
        size_t seg_data_len = seg_len - 2;

        if (marker == 0xDB) {  // DQT - Quantization table
            jpeg::parse_quant_table(dec, seg_data, seg_data_len);
        } else if (marker == 0xC4) {  // DHT - Huffman table
            jpeg::parse_huffman_table(dec, seg_data, seg_data_len);
        } else if (marker == 0xC0) {  // SOF0 - Start of frame (baseline)
            if (seg_data_len >= 8) {
                int precision = seg_data[0];
                dec.height = read_be16(&seg_data[1]);
                dec.width = read_be16(&seg_data[3]);
                dec.num_components = seg_data[5];

                (void)precision;

                for (int c = 0; c < dec.num_components && c < 4; ++c) {
                    size_t off = 6 + (size_t)c * 3;
                    if (off + 3 > seg_data_len) break;
                    dec.components[c].id = seg_data[off];
                    dec.components[c].h_sampling = (seg_data[off + 1] >> 4) & 0x0F;
                    dec.components[c].v_sampling = seg_data[off + 1] & 0x0F;
                    dec.components[c].quant_table_id = seg_data[off + 2];
                }
            }
        } else if (marker == 0xDA) {  // SOS - Start of scan
            // Parse scan header
            if (seg_data_len >= 4) {
                int num_comp = seg_data[0];
                for (int c = 0; c < num_comp && c < 4; ++c) {
                    size_t off = 1 + (size_t)c * 2;
                    if (off + 2 > seg_data_len) break;
                    int comp_id = seg_data[off];
                    int table_sel = seg_data[off + 1];

                    // Find component by ID
                    for (int i = 0; i < dec.num_components; ++i) {
                        if (dec.components[i].id == comp_id) {
                            dec.components[i].dc_table_id = (table_sel >> 4) & 0x0F;
                            dec.components[i].ac_table_id = table_sel & 0x0F;
                            break;
                        }
                    }
                }

                // Skip spectral selection + successive approximation (3 bytes)
            }

            // Compressed data starts after the SOS header
            dec.pos += seg_len;

            // TEACHING NOTE: Entropy-coded data decoding
            // =================================================================
            // We now read the entropy-coded scan data. For each MCU
            // (Minimum Coded Unit, typically 8x8 or 16x16 depending on
            // subsampling), we:
            //   1. For each component in the MCU:
            //      a. Decode DC coefficient: Huffman decode + extend + add prev_dc
            //      b. Decode 63 AC coefficients: for each, Huffman decode the
            //         (run_length, size) pair, read 'size' extra bits, extend,
            //         and skip 'run_length' zeros
            //   2. Dequantize: multiply each coefficient by quantization table
            //   3. IDCT: convert 8x8 frequency block to 8x8 spatial block
            //   4. Level shift: add 128
            //   5. Convert YCbCr to RGB

            dec.init_bits();

            img.width = dec.width;
            img.height = dec.height;
            img.pixels.resize((size_t)dec.width * dec.height * 4);

            // Compute MCU dimensions
            int max_h = 1, max_v = 1;
            for (int c = 0; c < dec.num_components; ++c) {
                if (dec.components[c].h_sampling > max_h) max_h = dec.components[c].h_sampling;
                if (dec.components[c].v_sampling > max_v) max_v = dec.components[c].v_sampling;
            }

            int mcu_w = max_h * 8;
            int mcu_h = max_v * 8;
            int mcus_x = (dec.width + mcu_w - 1) / mcu_w;
            int mcus_y = (dec.height + mcu_h - 1) / mcu_h;

            for (int my = 0; my < mcus_y; ++my) {
                for (int mx = 0; mx < mcus_x; ++mx) {
                    for (int c = 0; c < dec.num_components; ++c) {
                        int h = dec.components[c].h_sampling;
                        int v = dec.components[c].v_sampling;

                        for (int by = 0; by < v; ++by) {
                            for (int bx = 0; bx < h; ++bx) {
                                // Decode one 8x8 block
                                double block[64] = {0};

                                // DC coefficient
                                int dc_table = dec.components[c].dc_table_id;
                                int dc_size = dec.decode_huffman(dec.dc_tables[dc_table]);
                                if (dc_size > 0) {
                                    int dc_val = dec.extend(dec.read_bits(dc_size), dc_size);
                                    dec.prev_dc[c] += dc_val;
                                }
                                block[0] = (double)dec.prev_dc[c];

                                // AC coefficients
                                int ac_table = dec.components[c].ac_table_id;
                                int k = 1;
                                while (k < 64) {
                                    int rs = dec.decode_huffman(dec.ac_tables[ac_table]);
                                    int run = (rs >> 4) & 0x0F;
                                    int size = rs & 0x0F;

                                    if (size == 0) {
                                        if (run == 0) {
                                            // End of block
                                            break;
                                        } else if (run == 15) {
                                            // ZRL - skip 16 zeros
                                            k += 16;
                                            continue;
                                        }
                                        break;
                                    }

                                    k += run;
                                    if (k >= 64) break;

                                    int ac_val = dec.extend(dec.read_bits(size), size);
                                    block[jpeg::ZIGZAG[k]] = (double)ac_val;
                                    k++;
                                }

                                // Dequantize
                                int qt_id = dec.components[c].quant_table_id;
                                for (int i = 0; i < 64; ++i) {
                                    block[i] *= dec.quant_tables[qt_id].values[i];
                                }

                                // IDCT
                                double spatial[64];
                                jpeg::idct_8x8(block, spatial);

                                // Write pixels to image
                                // For simplicity, handle 1:1 sampling (no subsampling)
                                // For subsampled chroma, we just replicate
                                int px = mx * mcu_w + bx * 8;
                                int py = my * mcu_h + by * 8;

                                for (int py2 = 0; py2 < 8; ++py2) {
                                    for (int px2 = 0; px2 < 8; ++px2) {
                                        int img_x = px + px2;
                                        int img_y = py + py2;

                                        if (img_x >= dec.width || img_y >= dec.height) continue;

                                        double val = spatial[py2 * 8 + px2] + 128.0;
                                        if (val < 0) val = 0;
                                        if (val > 255) val = 255;

                                        if (dec.num_components == 1) {
                                            // Grayscale
                                            img.set_pixel(img_x, img_y, (uint8_t)val, (uint8_t)val, (uint8_t)val, 255);
                                        } else {
                                            // YCbCr - store component, convert later
                                            int pix_idx = ((size_t)img_y * dec.width + img_x) * 4;
                                            img.pixels[(size_t)c + 0 == 0 ? pix_idx : pix_idx] = (uint8_t)val;
                                            // We need a different approach for YCbCr
                                            // Store Y, Cb, Cr in separate arrays
                                            if (c == 0) img.pixels[pix_idx] = (uint8_t)val;
                                            else if (c == 1) img.pixels[pix_idx + 1] = (uint8_t)val;
                                            else if (c == 2) img.pixels[pix_idx + 2] = (uint8_t)val;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // TEACHING NOTE: YCbCr to RGB conversion
            // =================================================================
            // JPEG stores color images in YCbCr (luminance, blue-difference,
            // red-difference) color space. We convert to RGB:
            //   R = Y + 1.402 * (Cr - 128)
            //   G = Y - 0.344 * (Cb - 128) - 0.714 * (Cr - 128)
            //   B = Y + 1.772 * (Cb - 128)

            if (dec.num_components >= 3) {
                for (int y = 0; y < dec.height; ++y) {
                    for (int x = 0; x < dec.width; ++x) {
                        size_t idx = ((size_t)y * dec.width + x) * 4;
                        double y_val = img.pixels[idx];
                        double cb_val = img.pixels[idx + 1];
                        double cr_val = img.pixels[idx + 2];

                        double r = y_val + 1.402 * (cr_val - 128.0);
                        double g = y_val - 0.344136 * (cb_val - 128.0) - 0.714136 * (cr_val - 128.0);
                        double b = y_val + 1.772 * (cb_val - 128.0);

                        img.pixels[idx] = (uint8_t)(std::max(0.0, std::min(255.0, r)));
                        img.pixels[idx + 1] = (uint8_t)(std::max(0.0, std::min(255.0, g)));
                        img.pixels[idx + 2] = (uint8_t)(std::max(0.0, std::min(255.0, b)));
                        img.pixels[idx + 3] = 255;
                    }
                }
            }

            return img;
        }

        dec.pos += seg_len;
    }

    return img;
}

// ============================================================================
// GIF decoder (first frame)
// ============================================================================
//
// TEACHING NOTE: GIF file format and LZW decompression
// =========================================================================
// GIF (Graphics Interchange Format) is a palette-based image format.
//
// File structure:
//   Header block (6 bytes): "GIF87a" or "GIF89a"
//   Logical Screen Descriptor (7 bytes):
//     - width, height (2 bytes each, little-endian)
//     - packed byte: global color table flag, color resolution, sort, size
//     - background color index
//     - pixel aspect ratio
//   Global Color Table (if present): 3 bytes per color (R,G,B)
//   Image Descriptor (10 bytes): starts with 0x2C
//     - left, top, width, height (2 bytes each)
//     - packed: local color table flag, interlace, sort, size
//   Local Color Table (if present)
//   Image Data:
//     - LZW minimum code size (1 byte)
//     - Sub-blocks: length byte + data bytes (length 0 = end)
//   Trailer: 0x3B
//
// LZW (Lempel-Ziv-Welch) compression:
//   LZW builds a dictionary of sequences on the fly. Each code is an
//   index into this dictionary. The dictionary starts with entries for
//   every possible value (0-255 for 8-bit codes) plus a clear code and
//   end-of-information code. As the data is decompressed, new dictionary
//   entries are created from sequences that have been seen.
//
// GIF uses variable-length LZW codes: codes start at (min_code_size + 1)
// bits and grow as the dictionary grows, up to 12 bits maximum.

Image decode_gif(const std::vector<uint8_t>& data) {
    Image img;

    if (data.size() < 13) return img;

    // Verify signature
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return img;
    // GIF87a or GIF89a
    if (data[3] != '8') return img;

    size_t pos = 6;

    // Logical Screen Descriptor
    int screen_width = read_le16(&data[pos]);
    int screen_height = read_le16(&data[pos + 2]);
    (void)screen_width;
    (void)screen_height;
    uint8_t packed = data[pos + 4];
    pos += 7;

    bool has_global_table = (packed & 0x80) != 0;
    int global_table_size = 1 << ((packed & 0x07) + 1);

    // Read Global Color Table
    std::vector<uint8_t> color_table(768, 0);  // 256 colors * 3 bytes
    if (has_global_table) {
        for (int i = 0; i < global_table_size; ++i) {
            if (pos + 3 > data.size()) return img;
            color_table[i * 3] = data[pos];
            color_table[i * 3 + 1] = data[pos + 1];
            color_table[i * 3 + 2] = data[pos + 2];
            pos++;
        }
    }

    // Find image descriptor (0x2C)
    while (pos < data.size()) {
        uint8_t block = data[pos];
        if (block == 0x2C) {
            // Image descriptor found
            pos++;
            break;
        } else if (block == 0x21) {
            // Extension block - skip
            pos++;
            if (pos >= data.size()) break;
            uint8_t label = data[pos++];
            (void)label;
            // Skip sub-blocks
            while (pos < data.size()) {
                uint8_t sub_len = data[pos++];
                if (sub_len == 0) break;
                pos += sub_len;
            }
        } else if (block == 0x3B) {
            // Trailer - no image found
            return img;
        } else {
            pos++;
        }
    }

    if (pos + 9 > data.size()) return img;

    // Image descriptor
    int img_left = read_le16(&data[pos]);
    int img_top = read_le16(&data[pos + 2]);
    int img_width = read_le16(&data[pos + 4]);
    int img_height = read_le16(&data[pos + 6]);
    uint8_t img_packed = data[pos + 8];
    pos += 9;

    (void)img_left;
    (void)img_top;

    bool has_local_table = (img_packed & 0x80) != 0;
    int local_table_size = 1 << ((img_packed & 0x07) + 1);

    std::vector<uint8_t> active_table = color_table;

    if (has_local_table) {
        for (int i = 0; i < local_table_size; ++i) {
            if (pos + 3 > data.size()) return img;
            active_table[i * 3] = data[pos];
            active_table[i * 3 + 1] = data[pos + 1];
            active_table[i * 3 + 2] = data[pos + 2];
            pos++;
        }
    }

    if (pos >= data.size()) return img;

    // LZW minimum code size
    int min_code_size = data[pos++];
    int clear_code = 1 << min_code_size;
    int end_code = clear_code + 1;
    int next_code = end_code + 1;
    int code_size = min_code_size + 1;

    // TEACHING NOTE: LZW dictionary
    // =================================================================
    // The LZW dictionary is an array of byte sequences. Each entry is a
    // prefix (pointer to a previous entry) plus one byte. We store the
    // dictionary as arrays of prefix codes and suffix bytes.
    //
    // The dictionary is initialized with:
    //   - Codes 0 to (clear_code - 1): single bytes 0 to 255
    //   - Code clear_code: special "clear" code (reset dictionary)
    //   - Code end_code: special "end of information" code
    //   - Codes (end_code + 1) onward: new sequences created during decoding

    std::vector<int> dict_prefix(4096, 0);
    std::vector<int> dict_suffix(4096, 0);
    std::vector<int> dict_length(4096, 1);

    // Initialize dictionary
    for (int i = 0; i < clear_code; ++i) {
        dict_prefix[i] = -1;
        dict_suffix[i] = i;
        dict_length[i] = 1;
    }
    dict_prefix[clear_code] = -1;
    dict_suffix[clear_code] = -1;
    dict_prefix[end_code] = -1;
    dict_suffix[end_code] = -1;

    // Read sub-blocks
    std::vector<uint8_t> lzw_data;
    while (pos < data.size()) {
        uint8_t sub_len = data[pos++];
        if (sub_len == 0) break;
        if (pos + sub_len > data.size()) break;
        lzw_data.insert(lzw_data.end(), data.begin() + pos, data.begin() + pos + sub_len);
        pos += sub_len;
    }

    // LZW decode
    std::vector<int> indices;
    int bit_buf = 0;
    int bit_count = 0;
    size_t data_pos = 0;
    int prev_code = -1;

    while (data_pos < lzw_data.size() || bit_count >= code_size) {
        // Read code
        while (bit_count < code_size && data_pos < lzw_data.size()) {
            bit_buf |= (int)lzw_data[data_pos] << bit_count;
            bit_count += 8;
            data_pos++;
        }

        if (bit_count < code_size) break;

        int code = bit_buf & ((1 << code_size) - 1);
        bit_buf >>= code_size;
        bit_count -= code_size;

        if (code == clear_code) {
            // Reset dictionary
            next_code = end_code + 1;
            code_size = min_code_size + 1;
            prev_code = -1;
            continue;
        }

        if (code == end_code) {
            break;
        }

        // Decode the code
        std::vector<int> output;

        if (code < next_code) {
            // Code is in dictionary - output the sequence
            int c = code;
            while (c >= 0 && c < (int)dict_prefix.size()) {
                output.push_back(dict_suffix[c]);
                c = dict_prefix[c];
            }
            std::reverse(output.begin(), output.end());
        } else if (code == next_code && prev_code >= 0) {
            // Special case: code = next_code, output = prev + first_byte_of_prev
            int c = prev_code;
            output.push_back(dict_suffix[c]);
            while (c >= 0 && c < (int)dict_prefix.size()) {
                output.push_back(dict_suffix[c]);
                c = dict_prefix[c];
            }
            std::reverse(output.begin(), output.end());
            output.push_back(output[0]);
        } else {
            break;
        }

        // Output indices
        for (int idx : output) {
            indices.push_back(idx);
        }

        // Add to dictionary
        if (prev_code >= 0 && next_code < 4096) {
            dict_prefix[next_code] = prev_code;
            dict_suffix[next_code] = output[0];
            dict_length[next_code] = dict_length[prev_code] + 1;
            next_code++;
        }

        // Increase code size if needed
        if (next_code >= (1 << code_size) && code_size < 12) {
            code_size++;
        }

        prev_code = code;
    }

    // Convert indices to RGBA pixels
    img.width = img_width;
    img.height = img_height;
    img.pixels.resize((size_t)img_width * img_height * 4);

    for (int i = 0; i < (int)indices.size() && i < img_width * img_height; ++i) {
        int idx = indices[i];
        if (idx < 0 || idx > 255) idx = 0;

        int y = i / img_width;
        int x = i % img_width;

        uint8_t r = active_table[idx * 3];
        uint8_t g = active_table[idx * 3 + 1];
        uint8_t b = active_table[idx * 3 + 2];

        img.set_pixel(x, y, r, g, b, 255);
    }

    return img;
}

} // namespace chinstrap