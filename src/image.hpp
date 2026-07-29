// image.hpp - Image decoders (PNG, JPEG, GIF) from scratch
//
// TEACHING NOTE: Why browsers need multiple image formats
// ===========================================================================
// The web uses three main raster image formats:
//
//   PNG (Portable Network Graphics):
//     - Lossless compression (DEFLATE)
//     - Supports alpha transparency (RGBA)
//     - Supports 1/2/4/8/16 bit per channel
//     - Uses CRC-32 for error detection
//     - Best for: graphics with sharp edges, text, transparency
//     - File structure: series of chunks (IHDR, IDAT, IEND)
//     - Pixel data is compressed with zlib (DEFLATE) in IDAT chunks
//
//   JPEG (Joint Photographic Experts Group):
//     - Lossy compression (DCT-based)
//     - No alpha transparency
//     - 8-bit per channel, RGB or grayscale
//     - Best for: photographs, natural images
//     - File structure: markers (SOI, SOF, DHT, SOS, EOI)
//     - Uses Huffman coding + Discrete Cosine Transform (DCT)
//     - Compression artifacts (blockiness) at high compression
//
//   GIF (Graphics Interchange Format):
//     - Lossless for 256-color (palette-based) images
//     - Supports animation (multiple frames)
//     - 1-bit transparency (single color is transparent)
//     - Best for: simple animations, small graphics
//     - Uses LZW compression
//     - File structure: header, logical screen, image descriptors, blocks
//
//   WebP and AVIF are newer formats not implemented here.
//
// This file implements all three decoders from scratch with zero third-party
// dependencies. The PNG decoder uses our zlib decompressor. The JPEG decoder
// implements baseline JPEG (sequential, DCT-based). The GIF decoder
// implements LZW decompression.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace chinstrap {

// TEACHING NOTE: Image representation
// =========================================================================
// We store decoded images as 32-bit RGBA (8 bits per channel, 4 channels).
// This is the most convenient format for the display layer, which works
// in 32-bit BGRA. We convert from the source format (e.g. RGB, grayscale,
// palette) to RGBA during decoding.

struct Image {
    int width;
    int height;
    std::vector<uint8_t> pixels;  // RGBA, row-major, top to bottom

    Image() : width(0), height(0) {}

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
    size_t size() const { return (size_t)width * height * 4; }

    // Get a pixel as RGBA
    void get_pixel(int x, int y, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
        size_t offset = ((size_t)y * width + x) * 4;
        r = pixels[offset];
        g = pixels[offset + 1];
        b = pixels[offset + 2];
        a = pixels[offset + 3];
    }

    // Set a pixel as RGBA
    void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        size_t offset = ((size_t)y * width + x) * 4;
        pixels[offset] = r;
        pixels[offset + 1] = g;
        pixels[offset + 2] = b;
        pixels[offset + 3] = a;
    }
};

// TEACHING NOTE: Image decoder API
// =========================================================================
// The API is simple: load_image() takes a file path and auto-detects the
// format by checking the magic bytes at the start of the file. It returns
// an Image struct with the decoded pixels.
//
// Format detection by magic bytes:
//   PNG:  0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A
//   JPEG: 0xFF 0xD8 0xFF
//   GIF:  0x47 0x49 0x46 0x38 ("GIF8")

// Auto-detect format and decode image
Image load_image(const std::string& path);

// Load from raw data (already read into memory)
Image load_image_from_memory(const std::vector<uint8_t>& data);

// PNG decoder
Image decode_png(const std::vector<uint8_t>& data);

// JPEG decoder (baseline only)
Image decode_jpeg(const std::vector<uint8_t>& data);

// GIF decoder (first frame only)
Image decode_gif(const std::vector<uint8_t>& data);

// Get format name for debugging
std::string detect_format(const std::vector<uint8_t>& data);

} // namespace chinstrap