// =========================================================================
// renderer.cpp - Framebuffer Renderer Implementation
// =========================================================================
// TEACHING NOTE: This file implements drawing to the Linux framebuffer.
// The framebuffer is accessed via /dev/fb0. We open it, query its
// properties with ioctl, and memory-map it with mmap. Then we write
// pixels by setting memory values.
//
// The framebuffer is the simplest possible display interface on Linux.
// It predates X11, Wayland, and OpenGL. It is still used in embedded
// systems, game consoles, and the Raspberry Pi (with the vc4-kms driver
// disabled). Writing to it is as simple as setting memory values.
//
// For our educational browser, the framebuffer is perfect: it requires
// no third-party libraries, just POSIX system calls.
// =========================================================================

#include "renderer.hpp"
#include "layout.hpp"
#include "css_parser.hpp"
#include "font.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <sys/stat.h>

namespace chinstrap {

// =========================================================================
// RenderColor implementation
// =========================================================================

RenderColor RenderColor::parse(const std::string& str) {
    // TEACHING NOTE: CSS color parsing. We handle:
    //   - Hex: #rgb, #rrggbb, #rrggbbaa
    //   - rgb(r, g, b), rgba(r, g, b, a)
    //   - Named colors (basic set)
    //
    // Real browsers support many more formats: hsl(), hwb(), lab(),
    // lch(), oklch(), color(), and hundreds of named colors.

    std::string s = str;
    // Trim whitespace
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();

    if (s.empty()) return RenderColor(0, 0, 0, 0);

    // Named colors
    if (s == "transparent") return RenderColor(0, 0, 0, 0);
    if (s == "white") return RenderColor(255, 255, 255);
    if (s == "black") return RenderColor(0, 0, 0);
    if (s == "red") return RenderColor(255, 0, 0);
    if (s == "green") return RenderColor(0, 128, 0);
    if (s == "blue") return RenderColor(0, 0, 255);
    if (s == "yellow") return RenderColor(255, 255, 0);
    if (s == "cyan" || s == "aqua") return RenderColor(0, 255, 255);
    if (s == "magenta" || s == "fuchsia") return RenderColor(255, 0, 255);
    if (s == "gray" || s == "grey") return RenderColor(128, 128, 128);
    if (s == "silver") return RenderColor(192, 192, 192);
    if (s == "maroon") return RenderColor(128, 0, 0);
    if (s == "purple") return RenderColor(128, 0, 128);
    if (s == "lime") return RenderColor(0, 255, 0);
    if (s == "olive") return RenderColor(128, 128, 0);
    if (s == "navy") return RenderColor(0, 0, 128);
    if (s == "teal") return RenderColor(0, 128, 128);

    // Hex colors
    if (s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.size() == 3) {
            // #rgb -> #rrggbb
            int r = std::stoi(hex.substr(0, 1), nullptr, 16);
            int g = std::stoi(hex.substr(1, 1), nullptr, 16);
            int b = std::stoi(hex.substr(2, 1), nullptr, 16);
            return RenderColor(static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
                         static_cast<std::uint8_t>(b * 17));
        } else if (hex.size() == 6 || hex.size() == 8) {
            int r = std::stoi(hex.substr(0, 2), nullptr, 16);
            int g = std::stoi(hex.substr(2, 2), nullptr, 16);
            int b = std::stoi(hex.substr(4, 2), nullptr, 16);
            int a = (hex.size() == 8) ? std::stoi(hex.substr(6, 2), nullptr, 16) : 255;
            return RenderColor(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                         static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a));
        }
    }

    // rgb() / rgba()
    if (s.substr(0, 4) == "rgb(" || s.substr(0, 5) == "rgba(") {
        // Extract the values inside parentheses
        std::size_t open = s.find('(');
        std::size_t close = s.find(')');
        if (open != std::string::npos && close != std::string::npos) {
            std::string values = s.substr(open + 1, close - open - 1);
            std::replace(values.begin(), values.end(), ',', ' ');
            std::istringstream ss(values);
            int r, g, b;
            float a = 1.0f;
            ss >> r >> g >> b;
            if (ss >> a) {
                // alpha was specified
            }
            return RenderColor(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                         static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a * 255));
        }
    }

    // Unknown color: return black
    return RenderColor(0, 0, 0);
}

std::uint32_t RenderColor::to_pixel(std::uint8_t bpp, std::uint32_t rp, std::uint32_t gp,
                               std::uint32_t bp, std::uint32_t ap) const {
    // TEACHING NOTE: The framebuffer stores pixels in a specific format.
    // We need to place each color component at the right bit position.
    // The positions are queried from the framebuffer device.
    (void)bpp;  // bpp determines how we pack, but we always use 32-bit packing
    std::uint32_t pixel = 0;
    pixel |= (static_cast<std::uint32_t>(r) << rp);
    pixel |= (static_cast<std::uint32_t>(g) << gp);
    pixel |= (static_cast<std::uint32_t>(b) << bp);
    pixel |= (static_cast<std::uint32_t>(a) << ap);
    return pixel;
}

// =========================================================================
// Bitmap font (8x16 pixels)
// =========================================================================
// TEACHING NOTE: This is a simple 8x16 bitmap font. Each character is
// 16 bytes, one per row. Bit 7 (0x80) is the leftmost pixel. We define
// printable ASCII characters 32-126 with proper recognizable glyphs.
//
// A real browser would use a vector font (TrueType, OpenType) rendered
// via FreeType and HarfBuzz. That would require third-party libraries,
// which violates our zero-dependency rule. So we use this bitmap font.
// The glyphs are based on the classic 8x16 VGA/IBM PC ROM font.

const std::vector<std::uint8_t>& Renderer::font_data() {
    // TEACHING NOTE: We generate a basic 8x16 font for printable ASCII
    // characters (32-126). Each glyph is 16 bytes. The font vector is
    // 128 * 16 = 2048 bytes total, covering all 128 ASCII entries.
    static std::vector<std::uint8_t> font(128 * 16, 0);  // 128 chars * 16 rows

    static bool initialized = false;
    if (!initialized) {
        // Space (32) - all zeros (already zero)

        // Basic letters: we use a simple pattern
        // This is NOT a proper font. It is a minimal placeholder.
        // In a real implementation, you would embed a complete font table.

        // For each printable character, define a simple pattern
        // We use a very basic approach: most characters get a box outline
        auto set_char = [&](int ch, const std::uint8_t rows[16]) {
            for (int i = 0; i < 16; i++) {
                font[static_cast<std::size_t>(ch) * 16 + static_cast<std::size_t>(i)] = rows[i];
            }
        };

        // ' ' (32) - blank
        static const std::uint8_t space_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char(' ', space_glyph);

        // '!' (33)
        static const std::uint8_t excl_glyph[16] = {
            0x00, 0x00, 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x18,
            0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00
        };
        set_char('!', excl_glyph);

        // '"' (34)
        static const std::uint8_t quote_glyph[16] = {
            0x00, 0x66, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('"', quote_glyph);

        // '#' (35)
        static const std::uint8_t hash_glyph[16] = {
            0x00, 0x00, 0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C,
            0x6C, 0xFE, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('#', hash_glyph);

        // '$' (36)
        static const std::uint8_t dollar_glyph[16] = {
            0x18, 0x18, 0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6,
            0x7C, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('$', dollar_glyph);

        // '%' (37)
        static const std::uint8_t percent_glyph[16] = {
            0x00, 0x00, 0x00, 0xC2, 0xC6, 0x0C, 0x18, 0x30,
            0x60, 0xC6, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('%', percent_glyph);

        // '&' (38)
        static const std::uint8_t amp_glyph[16] = {
            0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x76, 0xDC,
            0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('&', amp_glyph);

        // ''' (39)
        static const std::uint8_t squote_glyph[16] = {
            0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('\'', squote_glyph);

        // '(' (40)
        static const std::uint8_t lparen_glyph[16] = {
            0x00, 0x00, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30,
            0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('(', lparen_glyph);

        // ')' (41)
        static const std::uint8_t rparen_glyph[16] = {
            0x00, 0x00, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C,
            0x0C, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char(')', rparen_glyph);

        // '*' (42)
        static const std::uint8_t star_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x18, 0x3C, 0x7E, 0x18,
            0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('*', star_glyph);

        // '+' (43)
        static const std::uint8_t plus_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0xFF,
            0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('+', plus_glyph);

        // ',' (44)
        static const std::uint8_t comma_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x38, 0x38, 0x18, 0x30, 0x00, 0x00, 0x00
        };
        set_char(',', comma_glyph);

        // '-' (45)
        static const std::uint8_t minus_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('-', minus_glyph);

        // '.' (46)
        static const std::uint8_t dot_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00
        };
        set_char('.', dot_glyph);

        // '/' (47)
        static const std::uint8_t slash_glyph[16] = {
            0x00, 0x00, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60,
            0xC0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('/', slash_glyph);

        // '0' (48)
        static const std::uint8_t zero_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xCE, 0xD6, 0xD6,
            0xE6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('0', zero_glyph);

        // '1' (49)
        static const std::uint8_t one_glyph[16] = {
            0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18,
            0x18, 0x18, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('1', one_glyph);

        // '2' (50)
        static const std::uint8_t two_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0x06, 0x0C, 0x18, 0x30,
            0x60, 0xC6, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('2', two_glyph);

        // '3' (51)
        static const std::uint8_t three_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0x06, 0x1C, 0x06, 0x06,
            0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('3', three_glyph);

        // '4' (52)
        static const std::uint8_t four_glyph[16] = {
            0x00, 0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0xCC, 0xFE,
            0x0C, 0x0C, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('4', four_glyph);

        // '5' (53)
        static const std::uint8_t five_glyph[16] = {
            0x00, 0x00, 0xFE, 0xC0, 0xC0, 0xFC, 0x06, 0x06,
            0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('5', five_glyph);

        // '6' (54)
        static const std::uint8_t six_glyph[16] = {
            0x00, 0x00, 0x38, 0x60, 0xC0, 0xFC, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('6', six_glyph);

        // '7' (55)
        static const std::uint8_t seven_glyph[16] = {
            0x00, 0x00, 0xFE, 0xC6, 0x06, 0x0C, 0x18, 0x30,
            0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('7', seven_glyph);

        // '8' (56)
        static const std::uint8_t eight_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('8', eight_glyph);

        // '9' (57)
        static const std::uint8_t nine_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7E, 0x06,
            0x06, 0x0C, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('9', nine_glyph);

        // ':' (58)
        static const std::uint8_t colon_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00,
            0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char(':', colon_glyph);

        // ';' (59)
        static const std::uint8_t semicolon_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00,
            0x00, 0x38, 0x38, 0x18, 0x30, 0x00, 0x00, 0x00
        };
        set_char(';', semicolon_glyph);

        // '<' (60)
        static const std::uint8_t lt_glyph[16] = {
            0x00, 0x00, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60,
            0x30, 0x18, 0x0C, 0x06, 0x00, 0x00, 0x00, 0x00
        };
        set_char('<', lt_glyph);

        // '=' (61)
        static const std::uint8_t eq_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00,
            0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('=', eq_glyph);

        // '>' (62)
        static const std::uint8_t gt_glyph[16] = {
            0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x06,
            0x0C, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00
        };
        set_char('>', gt_glyph);

        // '?' (63)
        static const std::uint8_t question_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x0C, 0x18, 0x18,
            0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00
        };
        set_char('?', question_glyph);

        // '@' (64)
        static const std::uint8_t at_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xDE, 0xDE, 0xDE,
            0xDC, 0xC0, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('@', at_glyph);

        // 'A' (65)
        static const std::uint8_t a_glyph[16] = {
            0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
            0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('A', a_glyph);

        // 'B' (66)
        static const std::uint8_t b_glyph[16] = {
            0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x66,
            0x66, 0x66, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('B', b_glyph);

        // 'C' (67)
        static const std::uint8_t c_glyph[16] = {
            0x00, 0x00, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xC0,
            0xC0, 0xC2, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('C', c_glyph);

        // 'D' (68)
        static const std::uint8_t d_glyph[16] = {
            0x00, 0x00, 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x66,
            0x66, 0x66, 0x6C, 0xF8, 0x00, 0x00, 0x00, 0x00
        };
        set_char('D', d_glyph);

        // 'E' (69)
        static const std::uint8_t e_glyph[16] = {
            0x00, 0x00, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68,
            0x60, 0x62, 0x66, 0xFE, 0x00, 0x00, 0x00, 0x00
        };
        set_char('E', e_glyph);

        // 'F' (70)
        static const std::uint8_t f_glyph[16] = {
            0x00, 0x00, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68,
            0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00
        };
        set_char('F', f_glyph);

        // 'G' (71)
        static const std::uint8_t g_glyph[16] = {
            0x00, 0x00, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xDE,
            0xC6, 0xC6, 0x66, 0x3A, 0x00, 0x00, 0x00, 0x00
        };
        set_char('G', g_glyph);

        // 'H' (72)
        static const std::uint8_t h_glyph[16] = {
            0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6,
            0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('H', h_glyph);

        // 'I' (73)
        static const std::uint8_t i_glyph[16] = {
            0x00, 0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18,
            0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('I', i_glyph);

        // 'J' (74)
        static const std::uint8_t j_glyph[16] = {
            0x00, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0xCC, 0xCC, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('J', j_glyph);

        // 'K' (75)
        static const std::uint8_t k_glyph[16] = {
            0x00, 0x00, 0xE6, 0x66, 0x6C, 0x78, 0x78, 0x6C,
            0x66, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('K', k_glyph);

        // 'L' (76)
        static const std::uint8_t l_glyph[16] = {
            0x00, 0x00, 0xF0, 0x60, 0x60, 0x60, 0x60, 0x60,
            0x60, 0x62, 0x66, 0xFE, 0x00, 0x00, 0x00, 0x00
        };
        set_char('L', l_glyph);

        // 'M' (77)
        static const std::uint8_t m_glyph[16] = {
            0x00, 0x00, 0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xD6,
            0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('M', m_glyph);

        // 'N' (78)
        static const std::uint8_t n_glyph[16] = {
            0x00, 0x00, 0xC6, 0xE6, 0xF6, 0xFE, 0xDE, 0xCE,
            0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('N', n_glyph);

        // 'O' (79)
        static const std::uint8_t o_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('O', o_glyph);

        // 'P' (80)
        static const std::uint8_t p_glyph[16] = {
            0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x60,
            0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00
        };
        set_char('P', p_glyph);

        // 'Q' (81)
        static const std::uint8_t q_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6,
            0xD6, 0xDE, 0x7C, 0x06, 0x0C, 0x00, 0x00, 0x00
        };
        set_char('Q', q_glyph);

        // 'R' (82)
        static const std::uint8_t r_glyph[16] = {
            0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x6C,
            0x66, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('R', r_glyph);

        // 'S' (83)
        static const std::uint8_t s_glyph[16] = {
            0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x60, 0x38, 0x0C,
            0x06, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('S', s_glyph);

        // 'T' (84)
        static const std::uint8_t t_glyph[16] = {
            0x00, 0x00, 0xFF, 0xDB, 0x99, 0x18, 0x18, 0x18,
            0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('T', t_glyph);

        // 'U' (85)
        static const std::uint8_t u_glyph[16] = {
            0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('U', u_glyph);

        // 'V' (86)
        static const std::uint8_t v_glyph[16] = {
            0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6,
            0x6C, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('V', v_glyph);

        // 'W' (87)
        static const std::uint8_t w_glyph[16] = {
            0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0xD6,
            0xFE, 0xEE, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('W', w_glyph);

        // 'X' (88)
        static const std::uint8_t x_glyph[16] = {
            0x00, 0x00, 0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x38,
            0x38, 0x6C, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
        };
        set_char('X', x_glyph);

        // 'Y' (89)
        static const std::uint8_t y_glyph[16] = {
            0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18,
            0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('Y', y_glyph);

        // 'Z' (90)
        static const std::uint8_t z_glyph[16] = {
            0x00, 0x00, 0xFE, 0xC6, 0x86, 0x0C, 0x18, 0x30,
            0x60, 0xC2, 0xC6, 0xFE, 0x00, 0x00, 0x00, 0x00
        };
        set_char('Z', z_glyph);

        // '[' (91)
        static const std::uint8_t lbracket_glyph[16] = {
            0x00, 0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30,
            0x30, 0x30, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('[', lbracket_glyph);

        // '\' (92)
        static const std::uint8_t bslash_glyph[16] = {
            0x00, 0x00, 0x00, 0x80, 0xC0, 0x60, 0x30, 0x18,
            0x0C, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('\\', bslash_glyph);

        // ']' (93)
        static const std::uint8_t rbracket_glyph[16] = {
            0x00, 0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0x0C, 0x0C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char(']', rbracket_glyph);

        // '^' (94)
        static const std::uint8_t caret_glyph[16] = {
            0x00, 0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('^', caret_glyph);

        // '_' (95)
        static const std::uint8_t underscore_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00
        };
        set_char('_', underscore_glyph);

        // '`' (96)
        static const std::uint8_t backtick_glyph[16] = {
            0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('`', backtick_glyph);

        // 'a' (97)
        static const std::uint8_t la_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x06, 0x7E,
            0xC6, 0xC6, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('a', la_glyph);

        // 'b' (98)
        static const std::uint8_t lb_glyph[16] = {
            0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6,
            0xC6, 0xC6, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('b', lb_glyph);

        // 'c' (99)
        static const std::uint8_t lc_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC0,
            0xC0, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('c', lc_glyph);

        // 'd' (100)
        static const std::uint8_t ld_glyph[16] = {
            0x00, 0x00, 0x06, 0x06, 0x06, 0x7E, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('d', ld_glyph);

        // 'e' (101)
        static const std::uint8_t le_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xFE,
            0xC0, 0xC0, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('e', le_glyph);

        // 'f' (102)
        static const std::uint8_t lf_glyph[16] = {
            0x00, 0x00, 0x1C, 0x36, 0x30, 0x78, 0x30, 0x30,
            0x30, 0x30, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('f', lf_glyph);

        // 'g' (103)
        static const std::uint8_t lg_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xC6, 0xC6,
            0xC6, 0x7E, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00
        };
        set_char('g', lg_glyph);

        // 'h' (104)
        static const std::uint8_t lh_glyph[16] = {
            0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6,
            0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('h', lh_glyph);

        // 'i' (105)
        static const std::uint8_t li_glyph[16] = {
            0x00, 0x00, 0x18, 0x18, 0x00, 0x38, 0x18, 0x18,
            0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('i', li_glyph);

        // 'j' (106)
        static const std::uint8_t lj_glyph[16] = {
            0x00, 0x00, 0x0C, 0x0C, 0x00, 0x1C, 0x0C, 0x0C,
            0x0C, 0x0C, 0xCC, 0x78, 0x00, 0x00, 0x00, 0x00
        };
        set_char('j', lj_glyph);

        // 'k' (107)
        static const std::uint8_t lk_glyph[16] = {
            0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xCC, 0xD8, 0xF0,
            0xD8, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('k', lk_glyph);

        // 'l' (108)
        static const std::uint8_t ll_glyph[16] = {
            0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18,
            0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('l', ll_glyph);

        // 'm' (109)
        static const std::uint8_t lm_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xD6, 0xD6,
            0xD6, 0xD6, 0xD6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('m', lm_glyph);

        // 'n' (110)
        static const std::uint8_t ln_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xC6, 0xC6,
            0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('n', ln_glyph);

        // 'o' (111)
        static const std::uint8_t lo_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('o', lo_glyph);

        // 'p' (112)
        static const std::uint8_t lp_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xC6, 0xC6,
            0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00, 0x00, 0x00
        };
        set_char('p', lp_glyph);

        // 'q' (113)
        static const std::uint8_t lq_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xC6, 0xC6,
            0xC6, 0x7E, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00
        };
        set_char('q', lq_glyph);

        // 'r' (114)
        static const std::uint8_t lr_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xF0, 0xC0,
            0xC0, 0xC0, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('r', lr_glyph);

        // 's' (115)
        static const std::uint8_t ls_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xC0, 0x7C,
            0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('s', ls_glyph);

        // 't' (116)
        static const std::uint8_t lt2_glyph[16] = {
            0x00, 0x00, 0x30, 0x30, 0x30, 0x78, 0x30, 0x30,
            0x30, 0x36, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('t', lt2_glyph);

        // 'u' (117)
        static const std::uint8_t lu_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6,
            0xC6, 0xC6, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('u', lu_glyph);

        // 'v' (118)
        static const std::uint8_t lv_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6,
            0xC6, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('v', lv_glyph);

        // 'w' (119)
        static const std::uint8_t lw_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xD6,
            0xD6, 0xFE, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('w', lw_glyph);

        // 'x' (120)
        static const std::uint8_t lx_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x6C, 0x38,
            0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('x', lx_glyph);

        // 'y' (121)
        static const std::uint8_t ly_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6,
            0xC6, 0x7E, 0x06, 0x06, 0x7C, 0x00, 0x00, 0x00
        };
        set_char('y', ly_glyph);

        // 'z' (122)
        static const std::uint8_t lz_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x0C, 0x18,
            0x30, 0x60, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('z', lz_glyph);

        // '{' (123)
        static const std::uint8_t lbrace_glyph[16] = {
            0x00, 0x00, 0x0E, 0x18, 0x18, 0x18, 0x70, 0x18,
            0x18, 0x18, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('{', lbrace_glyph);

        // '|' (124)
        static const std::uint8_t pipe_glyph[16] = {
            0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
            0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00
        };
        set_char('|', pipe_glyph);

        // '}' (125)
        static const std::uint8_t rbrace_glyph[16] = {
            0x00, 0x00, 0x70, 0x18, 0x18, 0x18, 0x0E, 0x18,
            0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('}', rbrace_glyph);

        // '~' (126)
        static const std::uint8_t tilde_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x76, 0xDC, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char('~', tilde_glyph);

        initialized = true;
    }

    return font;
}

// =========================================================================
// Renderer implementation
// =========================================================================

Renderer::Renderer() {
    init_fonts();
}

Renderer::~Renderer() {
    if (fb_mem_) {
        munmap(fb_mem_, fb_info_.smem_len);
    }
    if (fb_fd_ >= 0) {
        ::close(fb_fd_);
    }
}

bool Renderer::init() {
    // TEACHING NOTE: We open /dev/fb0 and memory-map it. This gives us
    // direct access to the screen pixels. The steps are:
    //   1. open("/dev/fb0", O_RDWR) - get a file descriptor
    //   2. ioctl(FBIOGET_VSCREENINFO) - get variable screen info
    //   3. ioctl(FBIOGET_FSCREENINFO) - get fixed screen info
    //   4. mmap() - map the framebuffer memory into our address space
    //
    // After this, writing to fb_mem_[y * line_length + x * bytes_per_pixel]
    // sets the pixel at (x, y).

    fb_fd_ = ::open("/dev/fb0", O_RDWR);
    if (fb_fd_ < 0) {
        // Framebuffer not available
        return false;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (::ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    if (::ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    fb_info_.width = static_cast<int>(vinfo.xres);
    fb_info_.height = static_cast<int>(vinfo.yres);
    fb_info_.bits_per_pixel = vinfo.bits_per_pixel;
    fb_info_.bytes_per_pixel = (vinfo.bits_per_pixel + 7) / 8;
    fb_info_.line_length = static_cast<int>(finfo.line_length);
    fb_info_.smem_len = finfo.smem_len;
    fb_info_.red_pos = vinfo.red.offset;
    fb_info_.green_pos = vinfo.green.offset;
    fb_info_.blue_pos = vinfo.blue.offset;
    fb_info_.alpha_pos = vinfo.transp.offset;

    // Memory-map the framebuffer
    fb_mem_ = static_cast<std::uint8_t*>(
        ::mmap(nullptr, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0)
    );

    if (fb_mem_ == MAP_FAILED) {
        fb_mem_ = nullptr;
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    initialized_ = true;
    return true;
}

void Renderer::put_pixel_internal(int x, int y, const RenderColor& color) {
    if (!initialized_ || !fb_mem_) return;
    if (x < 0 || x >= fb_info_.width || y < 0 || y >= fb_info_.height) return;

    std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(fb_info_.line_length)
                       + static_cast<std::size_t>(x) * static_cast<std::size_t>(fb_info_.bytes_per_pixel);

    if (offset + fb_info_.bytes_per_pixel > fb_info_.smem_len) return;

    std::uint32_t pixel = color.to_pixel(
        static_cast<std::uint8_t>(fb_info_.bits_per_pixel),
        fb_info_.red_pos, fb_info_.green_pos,
        fb_info_.blue_pos, fb_info_.alpha_pos
    );

    std::memcpy(fb_mem_ + offset, &pixel, static_cast<std::size_t>(fb_info_.bytes_per_pixel));
}

void Renderer::put_pixel(int x, int y, const RenderColor& color) {
    put_pixel_internal(x, y, color);
}

void Renderer::clear(const RenderColor& color) {
    // TEACHING NOTE: Clearing the screen is just filling every pixel
    // with the same color. We optimize by writing rows at a time.
    if (!initialized_ || !fb_mem_) return;

    std::uint32_t pixel = color.to_pixel(
        static_cast<std::uint8_t>(fb_info_.bits_per_pixel),
        fb_info_.red_pos, fb_info_.green_pos,
        fb_info_.blue_pos, fb_info_.alpha_pos
    );

    for (int y = 0; y < fb_info_.height; y++) {
        std::uint8_t* row = fb_mem_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(fb_info_.line_length);
        for (int x = 0; x < fb_info_.width; x++) {
            std::memcpy(row + static_cast<std::size_t>(x) * static_cast<std::size_t>(fb_info_.bytes_per_pixel),
                        &pixel, static_cast<std::size_t>(fb_info_.bytes_per_pixel));
        }
    }
}

void Renderer::fill_rect(int x, int y, int width, int height, const RenderColor& color) {
    // TEACHING NOTE: Filling a rectangle is the most common rendering
    // operation. Every background, every text glyph, every border is
    // ultimately rectangles. We clip to the screen bounds and write
    // pixels in a tight loop.

    // Clip to screen
    int x1 = std::max(0, x);
    int y1 = std::max(0, y);
    int x2 = std::min(fb_info_.width, x + width);
    int y2 = std::min(fb_info_.height, y + height);

    for (int py = y1; py < y2; py++) {
        for (int px = x1; px < x2; px++) {
            put_pixel_internal(px, py, color);
        }
    }
}

void Renderer::draw_rect(int x, int y, int width, int height, int border_width, const RenderColor& color) {
    // TEACHING NOTE: Drawing a border is four filled rectangles (top,
    // bottom, left, right). We draw them separately to handle the
    // border width correctly.

    // Top border
    fill_rect(x, y, width, border_width, color);
    // Bottom border
    fill_rect(x, y + height - border_width, width, border_width, color);
    // Left border
    fill_rect(x, y, border_width, height, color);
    // Right border
    fill_rect(x + width - border_width, y, border_width, height, color);
}

// =========================================================================
// Font management - CC0 TrueType font loading with bitmap fallback
// =========================================================================
// TEACHING NOTE: The browser bundles CC0 (public domain) TrueType fonts
// in assets/fonts/. On startup, we try to load the default sans-serif
// font (Aileron). If it loads successfully, text is rendered using TTF
// outlines with anti-aliasing via our from-scratch TTF parser (font.cpp).
// If TTF loading fails (missing files, parse error, etc.), we fall back
// to the built-in 8x16 bitmap font defined later in this file.
//
// The CSS font-family property is mapped to bundled CC0 fonts:
//   Arial, Helvetica, sans-serif -> Aileron
//   Times New Roman, Times, serif -> OSerif
//   Courier New, Courier, monospace -> Unitblock
//   Verdana, Geneva -> Vegur
//
// All fonts are CC0 1.0 Universal (Public Domain Dedication).
// See assets/fonts/README.md for full attribution.
// =========================================================================

bool Renderer::init_fonts() {
    // TEACHING NOTE: We try to find the assets/fonts directory relative
    // to the executable or in common system locations. If we cannot find
    // it, TTF rendering is disabled and the bitmap fallback is used.

    // Try several possible locations for the fonts directory
    std::vector<std::string> search_paths = {
        "assets/fonts/",
        "../assets/fonts/",
        "./assets/fonts/",
        "/usr/share/chinstrap/fonts/",
        "/usr/local/share/chinstrap/fonts/"
    };

    std::string fonts_dir;
    for (const auto& path : search_paths) {
        std::string test = path + "Aileron-Regular.ttf";
        struct stat st;
        if (stat(test.c_str(), &st) == 0) {
            fonts_dir = path;
            break;
        }
    }

    if (fonts_dir.empty()) {
        // No fonts directory found; bitmap fallback will be used
        ttf_available_ = false;
        return false;
    }

    // Try to load the default sans-serif font (Aileron)
    std::string font_path = fonts_dir + "Aileron-Regular.ttf";
    if (load_ttf_font(font_path)) {
        ttf_available_ = true;
        return true;
    }

    // If Aileron fails, try OSerif (serif) as a fallback TTF
    font_path = fonts_dir + "OSerif-Regular.ttf";
    if (load_ttf_font(font_path)) {
        ttf_available_ = true;
        return true;
    }

    // If all TTF loads fail, bitmap fallback will be used
    ttf_available_ = false;
    return false;
}

bool Renderer::load_ttf_font(const std::string& path) {
    // TEACHING NOTE: We use our from-scratch TTF parser (font.cpp) to load
    // the font file. The parser reads the TrueType tables (head, cmap,
    // glyf, loca, hmtx, hhea, maxp, name) and can rasterize glyph outlines
    // with anti-aliasing. This is a significant achievement: we can render
    // Unicode text without any third-party font library (no FreeType).

    auto font = std::make_unique<Font>();
    if (font->load(path)) {
        ttf_font_ = std::move(font);
        return true;
    }
    return false;
}

std::string Renderer::map_font_family(const std::string& family) const {
    // TEACHING NOTE: CSS font-family can contain multiple font names as
    // a comma-separated list (e.g. "Arial, Helvetica, sans-serif"). We
    // parse the first name and map it to a bundled CC0 font file.
    //
    // Mapping:
    //   Arial, Helvetica -> Aileron-Regular.ttf
    //   sans-serif -> Aileron-Regular.ttf
    //   Times New Roman, Times -> OSerif-Regular.ttf
    //   serif -> OSerif-Regular.ttf
    //   Courier New, Courier -> Unitblock-Regular.ttf
    //   monospace -> Unitblock-Regular.ttf
    //   Verdana, Geneva -> Vegur-Regular.ttf
    //   default -> Aileron-Regular.ttf

    // Extract the first font name (before any comma)
    std::string first = family;
    size_t comma = first.find(',');
    if (comma != std::string::npos) {
        first = first.substr(0, comma);
    }

    // Trim whitespace and quotes
    while (!first.empty() && (first.front() == ' ' || first.front() == '"' || first.front() == '\'')) {
        first.erase(first.begin());
    }
    while (!first.empty() && (first.back() == ' ' || first.back() == '"' || first.back() == '\'')) {
        first.pop_back();
    }

    // Convert to lowercase for comparison
    std::string lower = first;
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Check for sans-serif families
    if (lower == "arial" || lower == "helvetica" || lower == "sans-serif" ||
        lower == "sans serif" || lower == "helvetica neue" ||
        lower == "segoe ui" || lower == "tahoma" || lower == "trebuchet ms" ||
        lower == "verdana" || lower == "geneva") {
        return "Aileron-Regular.ttf";
    }

    // Check for serif families
    if (lower == "times" || lower == "times new roman" || lower == "serif" ||
        lower == "georgia" || lower == "garamond" || lower == "palatino") {
        return "OSerif-Regular.ttf";
    }

    // Check for monospace families
    if (lower == "courier" || lower == "courier new" || lower == "monospace" ||
        lower == "consolas" || lower == "monaco" || lower == "menlo") {
        return "Unitblock-Regular.ttf";
    }

    // Default to sans-serif (Aileron)
    return "Aileron-Regular.ttf";
}

// =========================================================================
// Text rendering - TTF path with bitmap fallback
// =========================================================================

int Renderer::text_width(const std::string& text) const {
    // If TTF font is available, use its metrics for accurate width
    if (ttf_available_ && ttf_font_) {
        // TTF text width: sum of advance widths for each character
        int width = 0;
        for (std::size_t i = 0; i < text.size(); i++) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch >= 128) ch = '?';
            uint32_t glyph_index = ttf_font_->get_glyph_index(ch);
            GlyphMetrics metrics = ttf_font_->get_glyph_metrics(glyph_index);
            // Scale from font units to pixels at 16px size
            int advance_px = static_cast<int>(
                static_cast<float>(metrics.advance_width) /
                static_cast<float>(ttf_font_->get_units_per_em()) * 16.0f);
            if (advance_px <= 0) advance_px = 8;  // Minimum width
            width += advance_px;
        }
        return width;
    }
    // Bitmap font: each character is 8 pixels wide
    return static_cast<int>(text.size()) * 8;
}

void Renderer::draw_text(int x, int y, const std::string& text, const RenderColor& color) {
    // TEACHING NOTE: Text rendering has two paths:
    //   1. TTF path: If a TrueType font is loaded, we rasterize each
    //      glyph using our from-scratch TTF parser (font.cpp). This
    //      produces anti-aliased glyphs with proper advance widths.
    //   2. Bitmap path: If TTF is not available, we use the built-in
    //      8x16 bitmap font (font_data()). Each glyph is 16 bytes,
    //      one per row. Bit 7 is the leftmost pixel.
    //
    // The bitmap font is the fallback and is always available.

    if (ttf_available_ && ttf_font_) {
        // TTF rendering path: rasterize each glyph and blit it
        int pen_x = x;
        int pixel_size = current_font_size_;  // Font size from CSS

        // Compute baseline: y is the top of the text box.
        // baseline = top + ascent_pixels
        float scale = static_cast<float>(pixel_size) /
                      static_cast<float>(ttf_font_->get_units_per_em());
        int ascent_px = static_cast<int>(static_cast<float>(ttf_font_->get_ascent()) * scale);
        int baseline_y = y + ascent_px;

        for (std::size_t i = 0; i < text.size(); i++) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch >= 128) ch = '?';

            uint32_t glyph_index = ttf_font_->get_glyph_index(ch);
            GlyphBitmap bitmap = ttf_font_->rasterize_glyph(glyph_index, pixel_size);

            // Blit the glyph bitmap to the framebuffer
            // bitmap.y_offset is relative to baseline (negative = above baseline)
            // bitmap rows are top-to-bottom (row 0 = top of glyph)
            for (int row = 0; row < bitmap.height; row++) {
                for (int col = 0; col < bitmap.width; col++) {
                    uint8_t coverage = bitmap.pixels[static_cast<std::size_t>(row) * bitmap.width + col];
                    if (coverage > 0) {
                        // Blend with the text color based on coverage
                        RenderColor blended = color;
                        if (coverage < 255) {
                            // Simple alpha blend: scale color by coverage
                            blended.a = static_cast<uint8_t>(
                                static_cast<int>(color.a) * coverage / 255);
                        }
                        put_pixel_internal(
                            pen_x + bitmap.x_offset + col,
                            baseline_y + bitmap.y_offset + row,
                            blended);
                    }
                }
            }

            // Advance the pen
            GlyphMetrics metrics = ttf_font_->get_glyph_metrics(glyph_index);
            int advance_px = static_cast<int>(
                static_cast<float>(metrics.advance_width) /
                static_cast<float>(ttf_font_->get_units_per_em()) * pixel_size);
            if (advance_px <= 0) advance_px = 8;
            pen_x += advance_px;
        }
        return;
    }

    // Bitmap font fallback path
    // TEACHING NOTE: Drawing text with a bitmap font is simple:
    // For each character, look up its glyph (16 rows of 8 bits each).
    // For each row, for each bit that is set, draw a pixel.

    const auto& font = font_data();

    for (std::size_t i = 0; i < text.size(); i++) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch >= 128) ch = '?';  // Non-ASCII fallback

        int char_x = x + static_cast<int>(i) * 8;

        for (int row = 0; row < 16; row++) {
            std::uint8_t bits = font[static_cast<std::size_t>(ch) * 16 + static_cast<std::size_t>(row)];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    put_pixel_internal(char_x + col, y + row, color);
                }
            }
        }
    }
}

void Renderer::draw_text(int x, int y, const std::string& text, const RenderColor& color, int max_width) {
    // Draw text with width clipping (for text wrapping)
    if (ttf_available_ && ttf_font_) {
        // TTF path: truncate based on TTF text width
        int total_width = 0;
        std::size_t chars_that_fit = 0;
        int pixel_size = current_font_size_;
        for (std::size_t i = 0; i < text.size(); i++) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch >= 128) ch = '?';
            uint32_t glyph_index = ttf_font_->get_glyph_index(ch);
            GlyphMetrics metrics = ttf_font_->get_glyph_metrics(glyph_index);
            int advance_px = static_cast<int>(
                static_cast<float>(metrics.advance_width) /
                static_cast<float>(ttf_font_->get_units_per_em()) * pixel_size);
            if (advance_px <= 0) advance_px = 8;
            if (total_width + advance_px > max_width) break;
            total_width += advance_px;
            chars_that_fit++;
        }
        if (chars_that_fit == 0) chars_that_fit = 1;
        std::string truncated = text.substr(0, chars_that_fit);
        draw_text(x, y, truncated, color);
    } else {
        // Bitmap path
        int chars_that_fit = max_width / 8;
        if (chars_that_fit <= 0) chars_that_fit = 1;
        std::string truncated = text.substr(0, static_cast<std::size_t>(chars_that_fit));
        draw_text(x, y, truncated, color);
    }
}

// =========================================================================
// Box rendering
// =========================================================================

RenderColor Renderer::get_color_style(const Box& box, const std::string& property, const RenderColor& default_color) const {
    std::string value = box.get_style(property);
    if (value.empty()) return default_color;
    return RenderColor::parse(value);
}

void Renderer::render_box(const Box& box, int offset_x, int offset_y) {
    // TEACHING NOTE: This is the paint step. For each box, we draw:
    //   1. Background color (fill_rect)
    //   2. Border (draw_rect)
    //   3. Text content (draw_text)
    //   4. Then recurse into children
    //
    // The offset is the absolute position of the parent. Box positions
    // are relative to their parent, so we add the offset to get absolute
    // screen coordinates.

    int abs_x = offset_x + static_cast<int>(box.x);
    int abs_y = offset_y + static_cast<int>(box.y);
    int w = static_cast<int>(box.width);
    int h = static_cast<int>(box.height);

    // Content area position (inside margin, border, padding)
    int content_x = abs_x + static_cast<int>(box.margin.left + box.border.left + box.padding.left);
    int content_y = abs_y + static_cast<int>(box.margin.top + box.border.top + box.padding.top);
    int content_w = w - static_cast<int>(box.padding.right + box.border.right);
    if (content_w < 1) content_w = 1;

    // Draw background (in the border area, inside margin)
    // Check background shorthand first, then background-color
    RenderColor bg = RenderColor::transparent();
    std::string bg_shorthand = box.get_style("background");
    if (!bg_shorthand.empty() && bg_shorthand != "none") {
        bg = RenderColor::parse(bg_shorthand);
    }
    if (bg.a == 0) {
        bg = get_color_style(box, "background-color", RenderColor::transparent());
    }
    if (bg.a > 0) {
        int bg_x = abs_x + static_cast<int>(box.margin.left);
        int bg_y = abs_y + static_cast<int>(box.margin.top);
        int bg_w = w + static_cast<int>(box.padding.left + box.padding.right + box.border.left + box.border.right);
        int bg_h = h + static_cast<int>(box.padding.top + box.padding.bottom + box.border.top + box.border.bottom);
        fill_rect(bg_x, bg_y, bg_w, bg_h, bg);
    }


    // Draw border
    std::string border_val = box.get_style("border");
    if (!border_val.empty() && border_val != "none") {
        RenderColor border_color = get_color_style(box, "border-color", RenderColor::black());
        int bw = 1;  // Default border width
        // Try to parse border width from shorthand
        std::istringstream ss(border_val);
        std::string token;
        while (ss >> token) {
            if (token.find("px") != std::string::npos) {
                bw = static_cast<int>(std::stof(token));
            }
        }
        int border_x = abs_x + static_cast<int>(box.margin.left);
        int border_y = abs_y + static_cast<int>(box.margin.top);
        int border_w = w + static_cast<int>(box.padding.left + box.padding.right + box.border.left + box.border.right);
        int border_h = h + static_cast<int>(box.padding.top + box.padding.bottom + box.border.top + box.border.bottom);
        draw_rect(border_x, border_y, border_w, border_h, bw, border_color);
    }

    // Draw text
    if (box.type == Box::Type::Text && !box.text.empty()) {
        RenderColor text_color = get_color_style(box, "color", RenderColor::black());

        // Set font size from CSS
        std::string font_size_str = box.get_style("font-size");
        if (!font_size_str.empty()) {
            float fs = static_cast<float>(std::atof(font_size_str.c_str()));
            if (fs > 4.0f && fs < 200.0f) {
                current_font_size_ = static_cast<int>(fs);
            }
        } else {
            current_font_size_ = 16;  // Default
        }

        // Check if this box has a different font-family and switch fonts
        // TEACHING NOTE: We check the CSS font-family property and load
        // the corresponding CC0 TTF font. For example, if font-family is
        // "serif", we load OSerif. If it is "monospace", we load Unitblock.
        // If no font-family is specified, we keep the default (Aileron).
        std::string font_family = box.get_style("font-family");
        if (!font_family.empty() && ttf_available_) {
            std::string mapped = map_font_family(font_family);
            // Only reload if the mapped font is different from current
            if (ttf_font_) {
                std::string current_name = ttf_font_->get_family_name();
                // Simple heuristic: if the mapped font name does not match
                // the current font family, try to load it
                std::string mapped_lower = mapped;
                for (auto& c : mapped_lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                std::string current_lower = current_name;
                for (auto& c : current_lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                // If mapped font starts with a different name, try loading
                bool need_reload = false;
                if (mapped_lower.find("oserif") != std::string::npos &&
                    current_lower.find("oserif") == std::string::npos) {
                    need_reload = true;
                } else if (mapped_lower.find("unitblock") != std::string::npos &&
                           current_lower.find("unitblock") == std::string::npos) {
                    need_reload = true;
                } else if (mapped_lower.find("vegur") != std::string::npos &&
                           current_lower.find("vegur") == std::string::npos) {
                    need_reload = true;
                } else if (mapped_lower.find("aileron") != std::string::npos &&
                           current_lower.find("aileron") == std::string::npos &&
                           current_lower.find("oserif") == std::string::npos &&
                           current_lower.find("unitblock") == std::string::npos &&
                           current_lower.find("vegur") == std::string::npos) {
                    need_reload = true;
                }
                if (need_reload) {
                    // Try to find the fonts directory
                    std::vector<std::string> search_paths = {
                        "assets/fonts/",
                        "../assets/fonts/",
                        "./assets/fonts/",
                        "/usr/share/chinstrap/fonts/",
                        "/usr/local/share/chinstrap/fonts/"
                    };
                    for (const auto& sp : search_paths) {
                        std::string full_path = sp + mapped;
                        if (load_ttf_font(full_path)) break;
                    }
                }
            }
        }

        // For text boxes (word boxes from inline layout), use a large max width
        // to avoid truncation since the layout engine already handles wrapping
        int text_max_w = static_cast<int>(box.width);
        if (text_max_w < 1) text_max_w = content_w;
        // Add generous padding to avoid cutting off last character
        text_max_w += 16;
        draw_text(content_x, content_y, box.text, text_color, text_max_w);
    }

    // Recurse into children
    // TEACHING NOTE: Children are positioned relative to the content area
    // of the parent, so we pass the content origin as the offset.
    for (const auto& child : box.children) {
        render_box(*child, content_x, content_y);
    }
}

void Renderer::render(const Box& root) {
    // TEACHING NOTE: This is the entry point for rendering. If the
    // framebuffer is available, we render to it. Otherwise, we render
    // a text representation to stdout (for testing and headless use).

    if (initialized_) {
        clear(RenderColor::white());
        render_box(root, 0, 0);
    } else {
        render_to_stdout(root);
    }
}

void Renderer::render_to_stdout(const Box& root) {
    // TEACHING NOTE: When no framebuffer is available (e.g., in a terminal
    // or CI environment), we output a text representation of the layout
    // tree. This is useful for debugging and testing.

    std::function<void(const Box&, int)> print_box = [&](const Box& box, int indent) {
        std::string pad(static_cast<std::size_t>(indent), ' ');
        std::cout << pad << "Box[";

        switch (box.type) {
            case Box::Type::Block: std::cout << "block"; break;
            case Box::Type::Inline: std::cout << "inline"; break;
            case Box::Type::Text: std::cout << "text"; break;
        }

        std::cout << "] pos=(" << box.x << "," << box.y << ")"
                  << " size=(" << box.width << "x" << box.height << ")";

        if (!box.text.empty()) {
            std::cout << " text=\"" << box.text << "\"";
        }

        std::string bg = box.get_style("background-color");
        if (!bg.empty()) std::cout << " bg=" << bg;

        std::string color = box.get_style("color");
        if (!color.empty()) std::cout << " color=" << color;

        std::cout << std::endl;

        for (const auto& child : box.children) {
            print_box(*child, indent + 2);
        }
    };

    print_box(root, 0);
}

// =========================================================================
// render_to_ppm - Render to a PPM screenshot file
// =========================================================================
// TEACHING NOTE: PPM (Portable PixMap) is the simplest possible image
// format. The file is:
//   P6\n            (magic number for binary PPM)
//   W H\n          (width and height in ASCII)
//   255\n          (max color value)
//   <raw RGB bytes> (W * H * 3 bytes)
//
// We render to an off-screen buffer, then write it as PPM. This lets us
// take screenshots without any image encoding library. Convert to PNG
// with: convert screenshot.ppm screenshot.png
// =========================================================================

// =========================================================================
// UI helper functions for modern browser chrome
// =========================================================================
// TEACHING NOTE: These helpers draw the pixel-art icons and rounded
// rectangles that make up the browser UI. Real browsers use vector
// graphics (SVG paths) for icons, rendered via Skia or similar. We
// draw pixels directly since we have no graphics library.
// =========================================================================

void Renderer::fill_rounded_rect(int x, int y, int w, int h, int radius, const RenderColor& color) {
    // Fill the inner area, then fill the four corner quadrants
    int r = std::min(radius, std::min(w / 2, h / 2));
    if (r < 1) { fill_rect(x, y, w, h, color); return; }

    // Middle horizontal bands (full width)
    fill_rect(x, y + r, w, h - 2 * r, color);
    // Top band (between corners)
    fill_rect(x + r, y, w - 2 * r, r, color);
    // Bottom band (between corners)
    fill_rect(x + r, y + h - r, w - 2 * r, r, color);

    // Four rounded corners using distance check
    int r2 = r * r;

    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            // Top-left corner
            int ddx = r - 1 - dx;
            int ddy = r - 1 - dy;
            if (ddx * ddx + ddy * ddy <= r2) {
                put_pixel_internal(x + dx, y + dy, color);
            }
            // Top-right corner
            ddx = dx;
            ddy = r - 1 - dy;
            if (ddx * ddx + ddy * ddy <= r2) {
                put_pixel_internal(x + w - r + dx, y + dy, color);
            }
            // Bottom-left corner
            ddx = r - 1 - dx;
            ddy = dy;
            if (ddx * ddx + ddy * ddy <= r2) {
                put_pixel_internal(x + dx, y + h - r + dy, color);
            }
            // Bottom-right corner
            ddx = dx;
            ddy = dy;
            if (ddx * ddx + ddy * ddy <= r2) {
                put_pixel_internal(x + w - r + dx, y + h - r + dy, color);
            }
        }
    }
}

void Renderer::draw_h_line(int x0, int x1, int y, const RenderColor& color) {
    int xs = std::min(x0, x1);
    int xe = std::max(x0, x1);
    for (int x = xs; x <= xe; x++) {
        put_pixel_internal(x, y, color);
    }
}

void Renderer::draw_v_line(int x, int y0, int y1, const RenderColor& color) {
    int ys = std::min(y0, y1);
    int ye = std::max(y0, y1);
    for (int y = ys; y <= ye; y++) {
        put_pixel_internal(x, y, color);
    }
}

// Back arrow icon: left-pointing triangle with stem
void Renderer::draw_icon_back(int cx, int cy, int size, const RenderColor& color) {
    int half = size / 2;
    // Triangle part (left-pointing)
    for (int row = -half; row <= half; row++) {
        int tri_width = half + 1 - std::abs(row);
        for (int col = 0; col < tri_width; col++) {
            put_pixel_internal(cx - half + col, cy + row, color);
        }
    }
    // Stem (horizontal bar to the right of triangle)
    int stem_y0 = cy - size / 6;
    int stem_y1 = cy + size / 6;
    int stem_x0 = cx;
    int stem_x1 = cx + half;
    for (int y = stem_y0; y <= stem_y1; y++) {
        for (int x = stem_x0; x <= stem_x1; x++) {
            put_pixel_internal(x, y, color);
        }
    }
}

// Forward arrow icon: right-pointing triangle with stem
void Renderer::draw_icon_forward(int cx, int cy, int size, const RenderColor& color) {
    int half = size / 2;
    // Triangle part (right-pointing)
    for (int row = -half; row <= half; row++) {
        int tri_width = half + 1 - std::abs(row);
        for (int col = 0; col < tri_width; col++) {
            put_pixel_internal(cx + half - col, cy + row, color);
        }
    }
    // Stem (horizontal bar to the left of triangle)
    int stem_y0 = cy - size / 6;
    int stem_y1 = cy + size / 6;
    int stem_x0 = cx - half;
    int stem_x1 = cx;
    for (int y = stem_y0; y <= stem_y1; y++) {
        for (int x = stem_x0; x <= stem_x1; x++) {
            put_pixel_internal(x, y, color);
        }
    }
}

// Reload icon: circular arrow (partial circle with arrowhead)
void Renderer::draw_icon_reload(int cx, int cy, int size, const RenderColor& color) {
    int r = size / 2;
    // Draw most of a circle (skip top-right quadrant where arrowhead goes)
    for (int angle_deg = 30; angle_deg < 360; angle_deg += 3) {
        double rad = static_cast<double>(angle_deg) * 3.14159265 / 180.0;
        int px = cx + static_cast<int>(r * std::cos(rad));
        int py = cy + static_cast<int>(r * std::sin(rad));
        put_pixel_internal(px, py, color);
    }
    // Arrowhead at the top (where the circle breaks)
    // Two short diagonal lines forming a chevron pointing right
    for (int i = 0; i < 4; i++) {
        put_pixel_internal(cx + r - i, cy - i, color);
        put_pixel_internal(cx + r - i, cy + i, color);
    }
}

// Home icon: house shape (triangle roof + square body)
void Renderer::draw_icon_home(int cx, int cy, int size, const RenderColor& color) {
    int half = size / 2;
    // Roof: triangle pointing up
    for (int row = 0; row <= half; row++) {
        int span = row;
        for (int col = -span; col <= span; col++) {
            put_pixel_internal(cx + col, cy - half + row, color);
        }
    }
    // Body: filled rectangle below roof
    int body_top = cy + 1;
    int body_bot = cy + half;
    int body_left = cx - half + 2;
    int body_right = cx + half - 2;
    // Draw outline of body (left and right walls)
    for (int y = body_top; y <= body_bot; y++) {
        put_pixel_internal(body_left, y, color);
        put_pixel_internal(body_right, y, color);
    }
    // Bottom wall
    for (int x = body_left; x <= body_right; x++) {
        put_pixel_internal(x, body_bot, color);
    }
    // Door in center bottom
    int door_w = std::max(2, (body_right - body_left) / 4);
    int door_l = cx - door_w / 2;
    int door_r = cx + door_w / 2;
    for (int x = door_l; x <= door_r; x++) {
        for (int y = body_bot - door_w; y <= body_bot; y++) {
            put_pixel_internal(x, y, color);
        }
    }
}

// Menu/hamburger icon: three horizontal lines
void Renderer::draw_icon_menu(int cx, int cy, int size, const RenderColor& color) {
    int half = size / 2;
    int gap = size / 3;
    for (int i = -1; i <= 1; i++) {
        int y = cy + i * gap;
        for (int dx = -half; dx < half; dx++) {
            put_pixel_internal(cx + dx, y, color);
        }
    }
}

// Plus icon: horizontal and vertical bars forming a +
void Renderer::draw_icon_plus(int cx, int cy, int size, const RenderColor& color) {
    int half = size / 3;
    int bar_len = half;
    int bar_thick = std::max(1, size / 6);
    // Horizontal bar
    for (int dx = -bar_len; dx <= bar_len; dx++) {
        for (int dy = -bar_thick / 2; dy <= bar_thick / 2 + 1; dy++) {
            put_pixel_internal(cx + dx, cy + dy, color);
        }
    }
    // Vertical bar
    for (int dy = -bar_len; dy <= bar_len; dy++) {
        for (int dx = -bar_thick / 2; dx < bar_thick / 2 + 1; dx++) {
            put_pixel_internal(cx + dx, cy + dy, color);
        }
    }
}

// =========================================================================
// draw_browser_ui - Draw modern browser chrome
// =========================================================================
// TEACHING NOTE: A real browser has UI elements around the page content:
// tab bar, address bar, back/forward buttons, menu, etc. We draw a
// modern-looking browser chrome with:
//   - Tab bar with rounded active tab and inactive tabs (36px)
//   - Navigation toolbar with pixel-art icons (44px)
//   - Rounded address bar with URL text and favicon placeholder
//   - Hamburger menu button on the right
//   - Clean light color scheme with subtle shadows
// =========================================================================

void Renderer::draw_browser_ui(int width, int ui_height) {
    (void)ui_height;  // We use fixed heights for tab bar and toolbar

    // -- Color scheme --
    RenderColor tab_bar_bg(0xDE, 0xDE, 0xDE);    // #DEDEDE light gray
    RenderColor toolbar_bg(0xF5, 0xF5, 0xF5);    // #F5F5F5 very light
    RenderColor active_tab_bg(255, 255, 255);    // white
    RenderColor inactive_tab_bg(0xE8, 0xE8, 0xE8); // #E8E8E8
    RenderColor icon_color(0x55, 0x55, 0x55);    // #555555 dark gray
    RenderColor addr_bar_bg(255, 255, 255);      // white
    RenderColor addr_bar_border(0xD0, 0xD0, 0xD0); // #D0D0D0
    RenderColor url_text(0x33, 0x33, 0x33);      // #333333
    RenderColor shadow(0xC8, 0xC8, 0xC8);        // subtle shadow
    RenderColor separator(0xD8, 0xD8, 0xD8);     // thin separator
    RenderColor favicon_bg(0x4A, 0x90, 0xD9);    // blue circle
    RenderColor new_tab_color(0x88, 0x88, 0x88); // gray plus

    const int tab_bar_h = 36;
    const int toolbar_h = 44;
    const int total_ui_h = tab_bar_h + toolbar_h;  // 80

    // -- Tab bar background --
    fill_rect(0, 0, width, tab_bar_h, tab_bar_bg);

    // -- Draw tabs --
    // Active tab (first tab) - white with rounded top corners
    int tab_w = 180;
    int tab_h = tab_bar_h;
    int tab_x = 8;
    int tab_y = 0;

    // Active tab: white background with rounded top corners
    fill_rect(tab_x, tab_y, tab_w, tab_h, active_tab_bg);
    // Round the top-left and top-right corners of active tab
    // Top-left corner: fill with tab_bar_bg to carve the corner
    put_pixel_internal(tab_x, tab_y, tab_bar_bg);
    put_pixel_internal(tab_x + 1, tab_y, tab_bar_bg);
    put_pixel_internal(tab_x, tab_y + 1, tab_bar_bg);
    // Top-right corner
    put_pixel_internal(tab_x + tab_w - 1, tab_y, tab_bar_bg);
    put_pixel_internal(tab_x + tab_w - 2, tab_y, tab_bar_bg);
    put_pixel_internal(tab_x + tab_w - 1, tab_y + 1, tab_bar_bg);

    // Active tab text
    draw_text(tab_x + 12, tab_y + 11, "Example", url_text);

    // Inactive tab (second tab) - light gray, slightly shorter
    int tab2_x = tab_x + tab_w + 2;
    int tab2_w = 140;
    // Inactive tab background (same as tab bar, so just draw text)
    // Add subtle left separator
    draw_v_line(tab2_x, 6, tab_bar_h - 6, separator);
    draw_text(tab2_x + 12, tab_y + 11, "New Tab", RenderColor(0x77, 0x77, 0x77));

    // New tab button (+ icon)
    int plus_x = tab2_x + tab2_w + 12;
    int plus_y = tab_bar_h / 2;
    draw_icon_plus(plus_x, plus_y, 10, new_tab_color);

    // -- Subtle shadow below tab bar --
    draw_h_line(0, width - 1, tab_bar_h, shadow);
    draw_h_line(0, width - 1, tab_bar_h + 1, RenderColor(0xE8, 0xE8, 0xE8));

    // -- Navigation toolbar --
    int tb_y = tab_bar_h;
    fill_rect(0, tb_y, width, toolbar_h, toolbar_bg);

    // Navigation buttons: back, forward, reload, home
    // Each button is a clickable area with an icon centered in it
    int btn_size = 28;
    int btn_spacing = 4;
    int btn_y = tb_y + (toolbar_h - btn_size) / 2;
    int btn_x = 8;

    // Back button
    draw_icon_back(btn_x + btn_size / 2, btn_y + btn_size / 2, 14, icon_color);
    btn_x += btn_size + btn_spacing;

    // Forward button
    draw_icon_forward(btn_x + btn_size / 2, btn_y + btn_size / 2, 14, icon_color);
    btn_x += btn_size + btn_spacing;

    // Reload button
    draw_icon_reload(btn_x + btn_size / 2, btn_y + btn_size / 2, 14, icon_color);
    btn_x += btn_size + btn_spacing;

    // Home button
    draw_icon_home(btn_x + btn_size / 2, btn_y + btn_size / 2, 14, icon_color);
    btn_x += btn_size + btn_spacing + 8;  // extra gap before address bar

    // -- Address bar (rounded rectangle) --
    int addr_x = btn_x;
    int addr_y = tb_y + (toolbar_h - 28) / 2;
    int addr_w = width - addr_x - 40;  // leave room for menu button
    int addr_h = 28;
    int addr_radius = 14;  // fully rounded ends

    // Address bar shadow (1px below and right)
    fill_rounded_rect(addr_x + 1, addr_y + 1, addr_w, addr_h, addr_radius,
                      RenderColor(0xE0, 0xE0, 0xE0));
    // Address bar background
    fill_rounded_rect(addr_x, addr_y, addr_w, addr_h, addr_radius, addr_bar_bg);
    // Address bar border (subtle outline)
    // Top and bottom borders
    draw_h_line(addr_x + addr_radius, addr_x + addr_w - addr_radius - 1, addr_y, addr_bar_border);
    draw_h_line(addr_x + addr_radius, addr_x + addr_w - addr_radius - 1, addr_y + addr_h - 1, addr_bar_border);
    // Left and right borders
    draw_v_line(addr_x + addr_radius - 1, addr_y + 1, addr_y + addr_h - 2, addr_bar_border);
    draw_v_line(addr_x + addr_w - addr_radius, addr_y + 1, addr_y + addr_h - 2, addr_bar_border);

    // Favicon placeholder (colored circle on left side of address bar)
    int fav_cx = addr_x + 14;
    int fav_cy = addr_y + addr_h / 2;
    int fav_r = 6;
    for (int dy = -fav_r; dy <= fav_r; dy++) {
        for (int dx = -fav_r; dx <= fav_r; dx++) {
            if (dx * dx + dy * dy <= fav_r * fav_r) {
                put_pixel_internal(fav_cx + dx, fav_cy + dy, favicon_bg);
            }
        }
    }

    // URL text in the address bar
    if (!screenshot_url_.empty()) {
        int text_x = addr_x + 26;  // after favicon
        int text_y = addr_y + (addr_h - 16) / 2 + 1;  // vertically centered
        int max_url_width = addr_w - 36;  // padding on both sides
        draw_text(text_x, text_y, screenshot_url_, url_text, max_url_width);
    }

    // -- Menu/hamburger button (right side) --
    int menu_x = width - 24;
    int menu_y = tb_y + toolbar_h / 2;
    draw_icon_menu(menu_x, menu_y, 16, icon_color);

    // -- Shadow below toolbar --
    draw_h_line(0, width - 1, total_ui_h - 1, RenderColor(0xD0, 0xD0, 0xD0));
    draw_h_line(0, width - 1, total_ui_h, RenderColor(0xE0, 0xE0, 0xE0));
}

void Renderer::render_to_ppm(const Box& root, const std::string& filename,
                               int width, int height) {
    // Create an off-screen RGB buffer
    std::vector<uint8_t> buffer(width * height * 3, 255);  // White background

    // Save the current framebuffer state
    bool was_initialized = initialized_;
    uint8_t* saved_fb = fb_mem_;
    FramebufferInfo saved_info = fb_info_;

    // Set up a virtual framebuffer in the buffer
    // We fake the framebuffer to point at our buffer
    fb_mem_ = buffer.data();
    fb_info_.width = width;
    fb_info_.height = height;
    fb_info_.bits_per_pixel = 24;
    fb_info_.bytes_per_pixel = 3;
    fb_info_.line_length = width * 3;
    fb_info_.red_pos = 0;
    fb_info_.green_pos = 8;
    fb_info_.blue_pos = 16;
    fb_info_.alpha_pos = 24;
    fb_info_.smem_len = width * height * 3;
    initialized_ = true;

    // Draw the browser UI frame at the top
    // TEACHING NOTE: We draw a modern browser chrome with:
    //   - Tab bar with active/inactive tabs (36px)
    //   - Navigation toolbar with pixel-art icons (44px)
    //   - Rounded address bar with URL and favicon
    //   - Menu button on the right
    //   - Page content starts at y=80
    const int ui_height = 80;  // 36px tab bar + 44px toolbar
    draw_browser_ui(width, ui_height);

    // Render the page content below the UI bar
    // We use render_box with a y-offset so content starts at ui_height
    render_box(root, 0, ui_height);

    // Write PPM file
    std::ofstream out(filename, std::ios::binary);
    if (!out) {
        std::cerr << "Error: Cannot write " << filename << std::endl;
        // Restore state
        fb_mem_ = saved_fb;
        fb_info_ = saved_info;
        initialized_ = was_initialized;
        return;
    }

    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(buffer.size()));
    out.close();

    std::cout << "Screenshot saved: " << filename << " (" << width
              << "x" << height << ")" << std::endl;

    // Restore state
    fb_mem_ = saved_fb;
    fb_info_ = saved_info;
    initialized_ = was_initialized;
}

// =========================================================================
// render_to_buffer - Render page to an off-screen RGB pixel buffer
// =========================================================================
// TEACHING NOTE: This method renders the full page (including browser UI)
// into a raw RGB pixel buffer. It works exactly like render_to_ppm but
// returns the buffer instead of writing a file. The buffer is in RGB
// format (3 bytes per pixel, row-major, no padding). This buffer can
// then be converted to BGRA for X11 put_image or Wayland shm.

std::vector<uint8_t> Renderer::render_to_buffer(const Box& root, int width, int height) {
    // Create an off-screen RGB buffer (white background)
    std::vector<uint8_t> buffer(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 255);

    // Save the current framebuffer state
    bool was_initialized = initialized_;
    uint8_t* saved_fb = fb_mem_;
    FramebufferInfo saved_info = fb_info_;

    // Set up a virtual framebuffer pointing at our buffer
    fb_mem_ = buffer.data();
    fb_info_.width = width;
    fb_info_.height = height;
    fb_info_.bits_per_pixel = 24;
    fb_info_.bytes_per_pixel = 3;
    fb_info_.line_length = width * 3;
    fb_info_.red_pos = 0;
    fb_info_.green_pos = 8;
    fb_info_.blue_pos = 16;
    fb_info_.alpha_pos = 24;
    fb_info_.smem_len = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    initialized_ = true;

    // Draw the browser UI frame at the top
    const int ui_height = 80;
    draw_browser_ui(width, ui_height);

    // Render the page content below the UI bar
    render_box(root, 0, ui_height);

    // Restore state
    fb_mem_ = saved_fb;
    fb_info_ = saved_info;
    initialized_ = was_initialized;

    return buffer;
}

} // namespace chinstrap