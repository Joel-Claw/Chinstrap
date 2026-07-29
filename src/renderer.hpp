// =========================================================================
// renderer.hpp - Framebuffer Renderer
// =========================================================================
// TEACHING NOTE: The renderer is the final stage of the browser pipeline.
// It takes the layout tree (boxes with positions and sizes) and draws
// pixels on the screen. We draw directly to the Linux framebuffer
// (/dev/fb0), which is the lowest-level display interface on Linux.
//
// The framebuffer is a memory-mapped region that represents the screen.
// Each pixel is typically 32 bits (ARGB or BGRA format). Writing to
// the framebuffer memory directly draws pixels on the screen.
//
// We use mmap() to map the framebuffer into our process address space,
// then write pixels by setting memory values. This is the simplest
// possible rendering approach. Real browsers use complex graphics
// pipelines:
//   - Chrome uses Skia for 2D drawing and GL/Vulkan for compositing
//   - Firefox uses WebRender (a GPU-based renderer)
//   - Safari uses CoreAnimation (macOS) and a custom engine on other platforms
//
// Our renderer draws:
//   - Background colors (fill rectangles)
//   - Borders (draw rectangle outlines)
//   - Text (using a simple 8x16 bitmap font)
//   - Images (not implemented yet)
//
// TEACHING NOTE: How Chrome renders:
// Chrome separates rendering into multiple stages:
//   1. Paint: generates a display list of drawing commands
//   2. Composite: layers are composited together (GPU-accelerated)
//   3. Raster: drawing commands are rasterized into pixels
//   4. Display: pixels are sent to the screen
//
// We skip all of this and just write pixels directly. This is how
// browsers worked in the 1990s before GPU acceleration.
// =========================================================================

#ifndef CHINSTRAP_RENDERER_HPP
#define CHINSTRAP_RENDERER_HPP

#include <string>
#include <cstdint>
#include <vector>

namespace chinstrap {

// Forward declaration
struct Box;

// -------------------------------------------------------------------------
// RenderColor - RGBA color value
// -------------------------------------------------------------------------
// TEACHING NOTE: Colors in CSS can be specified in many ways:
//   - Named: red, blue, transparent
//   - Hex: #rgb, #rrggbb, #rrggbbaa
//   - RGB: rgb(255, 0, 0), rgba(255, 0, 0, 0.5)
//   - HSL: hsl(0, 100%, 50%)
// We support hex and basic rgb/rgba parsing. Colors are stored as
// 32-bit values: 0xRRGGBBAA.
// -------------------------------------------------------------------------

struct RenderColor {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    RenderColor() = default;
    RenderColor(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
        : r(r), g(g), b(b), a(a) {}

    // Parse a CSS color string
    static RenderColor parse(const std::string& str);

    // Convert to framebuffer pixel value (BGRA for most Linux framebuffer setups)
    std::uint32_t to_pixel(std::uint8_t bytes_per_pixel, std::uint32_t r_pos,
                           std::uint32_t g_pos, std::uint32_t b_pos,
                           std::uint32_t a_pos) const;

    // Common colors
    static RenderColor white() { return RenderColor(255, 255, 255); }
    static RenderColor black() { return RenderColor(0, 0, 0); }
    static RenderColor transparent() { return RenderColor(0, 0, 0, 0); }
};

// -------------------------------------------------------------------------
// FramebufferInfo - Information about the Linux framebuffer
// -------------------------------------------------------------------------
// TEACHING NOTE: The framebuffer has a fixed resolution and pixel format.
// We query this information using the FBIOGET_VSCREENINFO ioctl. The
// key fields are:
//   - width, height: resolution in pixels
//   - bits_per_pixel: usually 16, 24, or 32
//   - red/green/blue offsets: tell us how pixels are packed
//
// We use mmap() to map the framebuffer memory into our process. This
// lets us write pixels by just setting memory values.
// -------------------------------------------------------------------------

struct FramebufferInfo {
    int width = 0;
    int height = 0;
    int bits_per_pixel = 32;
    int bytes_per_pixel = 4;
    int line_length = 0;  // Bytes per row (may be > width * bytes_per_pixel)

    // RenderColor component positions (bit offsets)
    std::uint32_t red_pos = 16;
    std::uint32_t green_pos = 8;
    std::uint32_t blue_pos = 0;
    std::uint32_t alpha_pos = 24;

    // Total size of framebuffer memory
    std::size_t smem_len = 0;
};

// -------------------------------------------------------------------------
// Renderer - draws to the Linux framebuffer
// -------------------------------------------------------------------------

class Renderer {
public:
    Renderer();
    ~Renderer();

    // Disable copy
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initialize the framebuffer
    // TEACHING NOTE: This opens /dev/fb0, queries its properties, and
    // memory-maps it. On systems without a framebuffer (like a terminal
    // session), this will fail. We provide a fallback that just outputs
    // rendering info to stdout.
    bool init();

    // Get framebuffer info
    const FramebufferInfo& info() const { return fb_info_; }

    // Clear the screen to a color
    void clear(const RenderColor& color);

    // Draw a filled rectangle
    // TEACHING NOTE: This is the most fundamental drawing operation.
    // Every visible element on the screen is ultimately drawn as
    // rectangles. Even text is drawn as small rectangles (one per pixel
    // of each glyph).
    void fill_rect(int x, int y, int width, int height, const RenderColor& color);

    // Draw a rectangle outline (for borders)
    void draw_rect(int x, int y, int width, int height, int border_width, const RenderColor& color);

    // Draw a single pixel
    void put_pixel(int x, int y, const RenderColor& color);

    // Draw text at a position using the built-in bitmap font
    // TEACHING NOTE: We use a simple 8x16 bitmap font. Each character
    // is 8 pixels wide and 16 pixels tall. The font is stored as an
    // array of bytes, where each byte represents one row of pixels.
    // Bit 7 is the leftmost pixel, bit 0 is the rightmost.
    // This is the classic PC BIOS font format.
    void draw_text(int x, int y, const std::string& text, const RenderColor& color);
    void draw_text(int x, int y, const std::string& text, const RenderColor& color, int max_width);

    // Get text width in pixels
    int text_width(const std::string& text) const;

    // Render the entire layout tree
    // TEACHING NOTE: This walks the box tree recursively and draws each
    // box. For each box, we:
    //   1. Draw the background (fill_rect with background-color)
    //   2. Draw the border (draw_rect with border-color)
    //   3. Draw text content (draw_text)
    //   4. Recurse into children
    void render(const Box& root);

    // Fallback: render to stdout (for when framebuffer is not available)
    void render_to_stdout(const Box& root);

private:
    int fb_fd_ = -1;
    FramebufferInfo fb_info_;
    std::uint8_t* fb_mem_ = nullptr;
    bool initialized_ = false;

    // Put a pixel using the correct framebuffer format
    void put_pixel_internal(int x, int y, const RenderColor& color);

    // Recursive render helper
    void render_box(const Box& box, int offset_x, int offset_y);

    // Parse a CSS color string for a specific property
    RenderColor get_color_style(const Box& box, const std::string& property, const RenderColor& default_color) const;

    // Simple bitmap font (8x16)
    // TEACHING NOTE: This is a simplified font with basic ASCII glyphs.
    // Each glyph is 16 bytes (one per row). The most significant bit of
    // each byte is the leftmost pixel. We only define a few characters
    // and use a fallback for undefined ones.
    static const std::vector<std::uint8_t>& font_data();
};

} // namespace chinstrap

#endif // CHINSTRAP_RENDERER_HPP