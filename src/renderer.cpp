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

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cmath>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <functional>

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
// only basic ASCII characters. For undefined characters, we draw a
// placeholder rectangle.
//
// A real browser would use a vector font (TrueType, OpenType) rendered
// via FreeType and HarfBuzz. That would require third-party libraries,
// which violates our zero-dependency rule. So we use this simple bitmap
// font. It is readable but not pretty.

const std::vector<std::uint8_t>& Renderer::font_data() {
    // TEACHING NOTE: We generate a basic 8x16 font for printable ASCII
    // characters (32-126). Each glyph is 16 bytes. For brevity, we
    // include a compact set. Undefined glyphs are all zeros (blank).
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

        // 'A' (65) - simplified
        static const std::uint8_t a_glyph[16] = {
            0x00, 0x00, 0x00, 0x18, 0x24, 0x42, 0x42, 0x42,
            0x7E, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00
        };
        set_char('A', a_glyph);

        // 'B' (66) - simplified
        static const std::uint8_t b_glyph[16] = {
            0x00, 0x00, 0x00, 0x7C, 0x42, 0x42, 0x42, 0x7C,
            0x42, 0x42, 0x42, 0x7C, 0x00, 0x00, 0x00, 0x00
        };
        set_char('B', b_glyph);

        // Use a generic block for all other characters for now
        // In a complete implementation, every printable character would
        // have its own glyph. For the educational purpose, we create
        // a generic visible block.
        for (int ch = 33; ch < 127; ch++) {
            if (ch == 'A' || ch == 'B') continue;  // Already set
            static std::uint8_t generic[16];
            // Create a simple block pattern for each character
            generic[4] = 0x7E;
            generic[5] = 0x42;
            generic[6] = 0x42;
            generic[7] = 0x42;
            generic[8] = 0x42;
            generic[9] = 0x42;
            generic[10] = 0x7E;
            set_char(ch, generic);
        }

        // '.' (46)
        static const std::uint8_t dot_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00
        };
        set_char('.', dot_glyph);

        // ' ' (32) - blank
        static const std::uint8_t space_glyph[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        set_char(' ', space_glyph);

        initialized = true;
    }

    return font;
}

// =========================================================================
// Renderer implementation
// =========================================================================

Renderer::Renderer() = default;

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

int Renderer::text_width(const std::string& text) const {
    // Each character is 8 pixels wide
    return static_cast<int>(text.size()) * 8;
}

void Renderer::draw_text(int x, int y, const std::string& text, const RenderColor& color) {
    // TEACHING NOTE: Drawing text with a bitmap font is simple:
    // For each character, look up its glyph (16 rows of 8 bits each).
    // For each row, for each bit that is set, draw a pixel.
    //
    // This is the most basic form of text rendering. Real browsers
    // use complex text shaping with FreeType (for glyph outlines) and
    // HarfBuzz (for text shaping - handling ligatures, diacritics,
    // bidirectional text, etc.).

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
    int chars_that_fit = max_width / 8;
    if (chars_that_fit <= 0) chars_that_fit = 1;

    std::string truncated = text.substr(0, static_cast<std::size_t>(chars_that_fit));
    draw_text(x, y, truncated, color);
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

    // Draw background
    RenderColor bg = get_color_style(box, "background-color", RenderColor::transparent());
    if (bg.a > 0) {
        fill_rect(abs_x, abs_y, w, h, bg);
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
        draw_rect(abs_x, abs_y, w, h, bw, border_color);
    }

    // Draw text
    if (box.type == Box::Type::Text && !box.text.empty()) {
        RenderColor text_color = get_color_style(box, "color", RenderColor::black());
        draw_text(abs_x, abs_y, box.text, text_color, w);
    }

    // Recurse into children
    for (const auto& child : box.children) {
        render_box(*child, abs_x, abs_y);
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

} // namespace chinstrap