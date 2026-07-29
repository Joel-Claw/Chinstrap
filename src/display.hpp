// display.hpp - Display abstraction with framebuffer and X11 backends
//
// TEACHING NOTE: How computer displays work
// ===========================================================================
// A computer display is a grid of pixels, each represented by red, green,
// and blue color channels (RGB). The display controller reads from a region
// of memory called the "framebuffer" and converts those bytes into the
// physical signals sent to the screen.
//
// Framebuffer mode (/dev/fb0):
//   The Linux kernel exposes the display as a memory-mapped file. When you
//   mmap() /dev/fb0, you get a pointer to the raw pixel memory. Writing a
//   byte at offset (y * stride + x * bytes_per_pixel) changes the color of
//   pixel (x, y) on the screen. The kernel handles the actual hardware
//   communication. This is the simplest possible display interface.
//
// X11 mode (X Window System):
//   In a desktop environment, we cannot write directly to the framebuffer
//   because the X server manages the screen and multiple applications share
//   it. Instead, we connect to the X server via a socket and send drawing
//   commands. The X server composites our window with other windows and
//   sends the final pixels to the display. This is more complex but allows
//   windowed operation alongside other applications.
//
// Double buffering and vsync:
//   If we draw directly to the visible framebuffer, the user might see
//   partially-drawn frames (tearing). Double buffering uses two buffers:
//   a "back buffer" where we draw, and a "front buffer" which is displayed.
//   When the back buffer is complete, we swap them (flip). The swap is
//   synchronized to the vertical retrace period (vsync) to avoid tearing.
//   On the framebuffer, we use the FBIOPAN_DISPLAY ioctl to pan the display
//   to the other buffer. In X11, we draw to a pixmap and then copy it to
//   the window on Expose events.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace chinstrap {

// Forward declaration for friend access
class GUI;

// TEACHING NOTE: Color representation
// =========================================================================
// We use 32-bit ARGB colors (alpha, red, green, blue), each 8 bits.
// This is the most common format used by modern display hardware.
// The alpha channel is used for compositing (transparency). Even if the
// display does not support alpha, we keep it for internal compositing
// and blend onto opaque colors for final display.

struct Color {
    uint8_t r, g, b, a;

    constexpr Color() : r(0), g(0), b(0), a(0) {}
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}

    // Convert to packed 32-bit value (0xAARRGGBB or 0xBBGGRRFF depending on format)
    uint32_t to_uint32() const {
        return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    // Common colors
    static const Color BLACK;
    static const Color WHITE;
    static const Color RED;
    static const Color GREEN;
    static const Color BLUE;
    static const Color GRAY;
    static const Color DARK_GRAY;
    static const Color LIGHT_GRAY;
    static const Color YELLOW;
    static const Color CYAN;
    static const Color MAGENTA;
};

// TEACHING NOTE: Display backend types
// =========================================================================
// The browser needs to support two modes:
//   1. FRAMEBUFFER - direct /dev/fb0 access, for kiosk/embedded/headless use
//   2. X11 - for desktop use within an existing X session
// The abstraction lets the rest of the browser code not care which backend
// is active. Both provide the same interface: put_pixel, fill_rect, etc.

enum class DisplayBackend {
    FRAMEBUFFER,
    X11
};

class Display {
public:
    Display();
    ~Display();

    // Initialize the display with the given backend
    // For framebuffer: opens /dev/fb0, mmaps it
    // For X11: connects to X server, creates a window
    bool init(DisplayBackend backend, int width = 0, int height = 0);

    // Shut down and release resources
    void shutdown();

    // --- Drawing primitives ---
    // All coordinates are in pixels, origin at top-left

    // Set a single pixel. In framebuffer mode, writes directly to mapped memory.
    // In X11 mode, writes to the back buffer (a pixmap).
    void put_pixel(int x, int y, Color color);

    // Fill a rectangular area with a solid color.
    // This is much faster than calling put_pixel for each pixel because we
    // can use memset or memcpy for the fill.
    void fill_rect(int x, int y, int w, int h, Color color);

    // Copy a rectangular region from src to dst (useful for scrolling)
    void copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h);

    // Draw a horizontal line
    void hline(int x, int y, int w, Color color);

    // Draw a vertical line
    void vline(int x, int y, int h, Color color);

    // Draw a rectangle outline
    void rect(int x, int y, int w, int h, Color color);

    // --- Buffer management ---

    // Swap back buffer to front (make drawn content visible)
    // In framebuffer mode: uses FBIOPAN_DISPLAY for double-buffered flipping
    // In X11 mode: copies pixmap to window and flushes
    void flip();

    // Get the display dimensions
    int get_width() const { return m_width; }
    int get_height() const { return m_height; }

    // Get the current backend
    DisplayBackend get_backend() const { return m_backend; }

    // Check if initialized
    bool is_initialized() const { return m_initialized; }

    // Get raw back buffer pointer (for direct pixel manipulation)
    uint8_t* get_buffer() { return m_back_buffer; }

    // Get bytes per pixel
    int get_bpp() const { return m_bpp; }

    // Get line stride (bytes per row, may be > width * bpp due to alignment)
    int get_stride() const { return m_stride; }

    // Set window title (X11 mode only, no-op in framebuffer mode)
    void set_title(const std::string& title);

    // Process display events (X11 mode: handle Expose, etc.)
    // Returns true if the display is still valid, false if window was closed.
    bool process_events();

private:
    DisplayBackend m_backend;
    bool m_initialized;

    int m_width;
    int m_height;
    int m_bpp;        // bytes per pixel (typically 4 for 32-bit RGB)
    int m_stride;     // bytes per scanline

    // --- Framebuffer-specific ---
    int m_fb_fd;           // file descriptor for /dev/fb0
    uint8_t* m_fb_mem;     // mmaped framebuffer memory
    long m_fb_mem_size;    // total mmaped size
    int m_fb_y_offset;     // current y offset for double buffering

    // --- Back buffer ---
    // We always use a back buffer for double buffering. In framebuffer mode,
    // this is the second half of the mmaped memory (if the FB has enough
    // memory for two screens) or a separate malloced buffer. In X11 mode,
    // this is a local buffer we copy to the window on flip.
    uint8_t* m_back_buffer;
    bool m_back_buffer_owned; // true if we malloced it

    // --- X11-specific ---
    // These are stored as void* to avoid exposing X11 types in the header.
    // The actual X11 connection is managed in x11.cpp
    void* m_x11_conn;     // opaque pointer to X11 connection state
    void* m_x11_window;  // opaque pointer to window info

    // Allow GUI to access X11 internals for event dispatch
    friend class GUI;

    // Helper: convert Color to the framebuffer pixel format
    uint32_t color_to_native(Color c) const;

    // Helper: initialize framebuffer backend
    bool init_framebuffer(int width, int height);

    // Helper: initialize X11 backend
    bool init_x11(int width, int height);

    // Helper: flip for framebuffer backend
    void flip_framebuffer();

    // Helper: flip for X11 backend
    void flip_x11();

    // Helper: process events for X11 backend
    bool process_x11_events();
};

} // namespace chinstrap