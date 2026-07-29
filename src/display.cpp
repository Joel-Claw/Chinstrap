// display.cpp - Display abstraction implementation
//
// TEACHING NOTE: This file implements the display abstraction for Chinstrap.
// It supports three backends:
//   1. Linux framebuffer (/dev/fb0) - direct pixel access via mmap
//   2. X11 protocol over socket - windowed mode via raw X11
//   3. Wayland protocol over socket - windowed mode via raw Wayland
//
// The framebuffer backend is simpler: we open /dev/fb0, query its geometry
// with the FBIOGET_VSCREENINFO ioctl, mmap the memory, and write pixels
// directly. Double buffering uses the virtual screen height being 2x the
// physical height, and we pan between the two halves with FBIOPAN_DISPLAY.
//
// The X11 backend delegates to our minimal X11 implementation (x11.cpp).
// We create a window, draw to a local back buffer, and copy it to the
// window using XPutImage equivalent on Expose events.

#include "display.hpp"
#include "x11.hpp"
#include "wayland.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace chinstrap {

// Common color constants
const Color Color::BLACK(0, 0, 0, 255);
const Color Color::WHITE(255, 255, 255, 255);
const Color Color::RED(255, 0, 0, 255);
const Color Color::GREEN(0, 255, 0, 255);
const Color Color::BLUE(0, 0, 255, 255);
const Color Color::GRAY(128, 128, 128, 255);
const Color Color::DARK_GRAY(64, 64, 64, 255);
const Color Color::LIGHT_GRAY(192, 192, 192, 255);
const Color Color::YELLOW(255, 255, 0, 255);
const Color Color::CYAN(0, 255, 255, 255);
const Color Color::MAGENTA(255, 0, 255, 255);

// ============================================================================
// Constructor / Destructor
// ============================================================================

Display::Display()
    : m_backend(DisplayBackend::FRAMEBUFFER)
    , m_initialized(false)
    , m_width(0)
    , m_height(0)
    , m_bpp(4)
    , m_stride(0)
    , m_fb_fd(-1)
    , m_fb_mem(nullptr)
    , m_fb_mem_size(0)
    , m_fb_y_offset(0)
    , m_back_buffer(nullptr)
    , m_back_buffer_owned(false)
    , m_x11_conn(nullptr)
    , m_x11_window(nullptr)
    , m_wl_conn(nullptr)
    , m_wl_surface(nullptr) {}

Display::~Display() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool Display::init(DisplayBackend backend, int width, int height) {
    if (m_initialized) {
        shutdown();
    }

    m_backend = backend;

    if (backend == DisplayBackend::FRAMEBUFFER) {
        return init_framebuffer(width, height);
    } else if (backend == DisplayBackend::X11) {
        return init_x11(width, height);
    } else {
        return init_wayland(width, height);
    }
}

// TEACHING NOTE: Framebuffer initialization
// =========================================================================
// The Linux framebuffer API (linux/fb.h) provides:
//
//   FBIOGET_VSCREENINFO - get variable screen info (resolution, bpp, etc.)
//   FBIOPUT_VSCREENINFO - set variable screen info
//   FBIOGET_FSCREENINFO - get fixed screen info (memory size, line length)
//   FBIOPAN_DISPLAY     - pan the display to a different y offset
//
// Variable screen info includes:
//   xres, yres         - visible resolution
//   xres_virtual, yres_virtual - total virtual resolution (can be larger
//                               for double buffering)
//   bits_per_pixel     - color depth (16, 24, or 32)
//
// For double buffering, we set yres_virtual = 2 * yres. The display shows
// yres lines at a time, starting at y_offset. We draw to the other half
// and then pan to it.

bool Display::init_framebuffer(int width, int height) {
    // Open the framebuffer device
    m_fb_fd = open("/dev/fb0", O_RDWR);
    if (m_fb_fd < 0) {
        // Try /dev/fb1 or /dev/fb/0 as fallbacks
        m_fb_fd = open("/dev/fb/0", O_RDWR);
        if (m_fb_fd < 0) {
            return false;
        }
    }

    // Get variable screen info
    struct fb_var_screeninfo vinfo;
    if (ioctl(m_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(m_fb_fd);
        m_fb_fd = -1;
        return false;
    }

    // Get fixed screen info
    struct fb_fix_screeninfo finfo;
    if (ioctl(m_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(m_fb_fd);
        m_fb_fd = -1;
        return false;
    }

    // Store display dimensions
    m_width = (width > 0) ? width : (int)vinfo.xres;
    m_height = (height > 0) ? height : (int)vinfo.yres;
    m_bpp = (int)vinfo.bits_per_pixel / 8;
    m_stride = (int)finfo.line_length;

    // TEACHING NOTE: Setting up double buffering
    // =================================================================
    // We try to set the virtual height to 2x the visible height so we have
    // room for two screens. If the hardware does not have enough memory,
    // we fall back to a malloced back buffer.

    // Try to enable double buffering by setting yres_virtual
    vinfo.yres_virtual = vinfo.yres * 2;
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;

    bool double_buffered = false;
    if (ioctl(m_fb_fd, FBIOPUT_VSCREENINFO, &vinfo) >= 0) {
        // Re-read to get actual values
        if (ioctl(m_fb_fd, FBIOGET_VSCREENINFO, &vinfo) >= 0) {
            if (vinfo.yres_virtual >= vinfo.yres * 2) {
                double_buffered = true;
            }
        }
    }

    // Recalculate stride after potential vinfo changes
    if (ioctl(m_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(m_fb_fd);
        m_fb_fd = -1;
        return false;
    }
    m_stride = (int)finfo.line_length;

    // Calculate total memory size
    m_fb_mem_size = finfo.smem_len;

    // mmap the framebuffer
    m_fb_mem = (uint8_t*)mmap(nullptr, m_fb_mem_size,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               m_fb_fd, 0);
    if (m_fb_mem == MAP_FAILED) {
        m_fb_mem = nullptr;
        close(m_fb_fd);
        m_fb_fd = -1;
        return false;
    }

    // Set up back buffer
    if (double_buffered) {
        // TEACHING NOTE: In double-buffered mode, the front buffer is the
        // first m_height lines and the back buffer is the second m_height
        // lines. We draw to the back buffer and then pan to it.
        m_back_buffer = m_fb_mem + m_stride * m_height;
        m_back_buffer_owned = false;
        m_fb_y_offset = 0; // currently showing first half
    } else {
        // TEACHING NOTE: Without hardware double buffering, we allocate
        // a separate buffer in system RAM. We draw to this buffer and then
        // memcpy it to the framebuffer on flip. This is slower because of
        // the copy, but it still prevents partial updates from being visible
        // (at least on the parts that have been copied before the next vblank).
        size_t buf_size = (size_t)m_stride * m_height;
        m_back_buffer = (uint8_t*)malloc(buf_size);
        if (!m_back_buffer) {
            munmap(m_fb_mem, m_fb_mem_size);
            m_fb_mem = nullptr;
            close(m_fb_fd);
            m_fb_fd = -1;
            return false;
        }
        m_back_buffer_owned = true;
        m_fb_y_offset = 0;
    }

    m_initialized = true;
    return true;
}

// TEACHING NOTE: X11 initialization
// =========================================================================
// In X11 mode, we create a window on the X server using our minimal X11
// protocol implementation. The X server manages the actual framebuffer
// and composites our window with other windows. We draw to a local back
// buffer and send the pixel data to the X server on flip.

bool Display::init_x11(int width, int height) {
    // Create X11 connection and window via our X11 module
    X11Connection* conn = new X11Connection();
    if (!conn->connect()) {
        delete conn;
        return false;
    }

    int win_w = (width > 0) ? width : 800;
    int win_h = (height > 0) ? height : 600;

    X11Window* win = new X11Window();
    if (!win->create(conn, win_w, win_h, "Chinstrap")) {
        conn->disconnect();
        delete conn;
        delete win;
        return false;
    }

    m_x11_conn = conn;
    m_x11_window = win;
    m_width = win_w;
    m_height = win_h;
    m_bpp = 4;
    m_stride = m_width * m_bpp;

    // Allocate back buffer
    size_t buf_size = (size_t)m_stride * m_height;
    m_back_buffer = (uint8_t*)malloc(buf_size);
    if (!m_back_buffer) {
        delete win;
        conn->disconnect();
        delete conn;
        m_x11_conn = nullptr;
        m_x11_window = nullptr;
        return false;
    }
    m_back_buffer_owned = true;

    m_initialized = true;
    return true;
}

// TEACHING NOTE: Wayland initialization
// =========================================================================
// In Wayland mode, we connect to the Wayland compositor using our minimal
// Wayland protocol implementation. The compositor manages the actual
// framebuffer and composites our surface with other surfaces. We draw to
// a shared memory buffer and attach it to the surface on flip.

bool Display::init_wayland(int width, int height) {
    // Create Wayland connection
    WaylandConnection* conn = new WaylandConnection();
    if (!conn->connect()) {
        delete conn;
        return false;
    }

    // Determine window dimensions
    int win_w = (width > 0) ? width : 800;
    int win_h = (height > 0) ? height : 600;

    // If no explicit size, try to use output dimensions
    if (width <= 0 && conn->get_output_width() > 0) {
        win_w = conn->get_output_width();
        win_h = conn->get_output_height();
    }

    // Create the surface with shared memory buffer
    WaylandSurface* surf = new WaylandSurface();
    if (!surf->create(conn, win_w, win_h)) {
        delete surf;
        conn->disconnect();
        delete conn;
        return false;
    }

    m_wl_conn = conn;
    m_wl_surface = surf;
    m_width = win_w;
    m_height = win_h;
    m_bpp = 4;
    m_stride = m_width * m_bpp;

    // Use the Wayland shared memory buffer as our back buffer.
    // The Wayland surface shared memory IS our pixel buffer.
    // We write to it directly and then call commit_frame() to present.
    m_back_buffer = surf->get_buffer();
    m_back_buffer_owned = false;  // owned by WaylandSurface

    m_initialized = true;
    return true;
}

// ============================================================================
// Shutdown
// ============================================================================

void Display::shutdown() {
    if (!m_initialized) return;

    if (m_back_buffer_owned && m_back_buffer) {
        free(m_back_buffer);
    }
    m_back_buffer = nullptr;
    m_back_buffer_owned = false;

    if (m_backend == DisplayBackend::FRAMEBUFFER) {
        if (m_fb_mem) {
            munmap(m_fb_mem, m_fb_mem_size);
            m_fb_mem = nullptr;
        }
        if (m_fb_fd >= 0) {
            close(m_fb_fd);
            m_fb_fd = -1;
        }
    } else if (m_backend == DisplayBackend::X11) {
        if (m_x11_window) {
            delete static_cast<X11Window*>(m_x11_window);
            m_x11_window = nullptr;
        }
        if (m_x11_conn) {
            X11Connection* conn = static_cast<X11Connection*>(m_x11_conn);
            conn->disconnect();
            delete conn;
            m_x11_conn = nullptr;
        }
    } else if (m_backend == DisplayBackend::WAYLAND) {
        // Wayland surface must be destroyed before the connection
        if (m_wl_surface) {
            WaylandSurface* surf = static_cast<WaylandSurface*>(m_wl_surface);
            surf->destroy();
            delete surf;
            m_wl_surface = nullptr;
        }
        if (m_wl_conn) {
            WaylandConnection* conn = static_cast<WaylandConnection*>(m_wl_conn);
            conn->disconnect();
            delete conn;
            m_wl_conn = nullptr;
        }
    }

    m_initialized = false;
}

// ============================================================================
// Color conversion
// ============================================================================

// TEACHING NOTE: Color format conversion
// =========================================================================
// The framebuffer may use different pixel formats. Common formats:
//   - 32-bit BGRA (most common on x86): blue in lowest byte, then green,
//     red, alpha (or unused)
//   - 32-bit RGBA: red in lowest byte (less common for framebuffer)
//   - 24-bit BGR: no alpha, 3 bytes per pixel
//   - 16-bit RGB565: 5 bits red, 6 bits green, 5 bits blue
//
// We store the framebuffer bitfield offsets from fb_var_screeninfo and
// pack the color accordingly. For simplicity, we assume 32-bit BGRA which
// is by far the most common format on modern Linux systems.

uint32_t Display::color_to_native(Color c) const {
    // For 32-bit framebuffer: typically BGRA or BGRX
    // We output 0xBBGGRRAA or 0xBBGGRRxx depending on whether alpha is used
    if (m_bpp >= 4) {
        return ((uint32_t)c.b) | ((uint32_t)c.g << 8) |
               ((uint32_t)c.r << 16) | ((uint32_t)c.a << 24);
    } else if (m_bpp == 3) {
        // 24-bit BGR
        return ((uint32_t)c.b) | ((uint32_t)c.g << 8) | ((uint32_t)c.r << 16);
    } else if (m_bpp == 2) {
        // 16-bit RGB565
        uint32_t r5 = c.r >> 3;
        uint32_t g6 = c.g >> 2;
        uint32_t b5 = c.b >> 3;
        return (r5 << 11) | (g6 << 5) | b5;
    }
    // Fallback
    return c.to_uint32();
}

// ============================================================================
// Drawing primitives
// ============================================================================

void Display::put_pixel(int x, int y, Color color) {
    if (!m_initialized || !m_back_buffer) return;
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;

    size_t offset = (size_t)y * m_stride + (size_t)x * m_bpp;
    uint32_t native = color_to_native(color);

    // Write the pixel in the correct format
    if (m_bpp == 4) {
        *(uint32_t*)(m_back_buffer + offset) = native;
    } else if (m_bpp == 3) {
        m_back_buffer[offset] = (uint8_t)(native & 0xFF);
        m_back_buffer[offset + 1] = (uint8_t)((native >> 8) & 0xFF);
        m_back_buffer[offset + 2] = (uint8_t)((native >> 16) & 0xFF);
    } else if (m_bpp == 2) {
        *(uint16_t*)(m_back_buffer + offset) = (uint16_t)native;
    }
}

void Display::fill_rect(int x, int y, int w, int h, Color color) {
    if (!m_initialized || !m_back_buffer) return;

    // Clip
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > m_width) ? m_width : (x + w);
    int y1 = (y + h > m_height) ? m_height : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t native = color_to_native(color);

    // TEACHING NOTE: Optimized fill
    // For 32-bit pixels, we can fill 4 bytes at a time with memset_pattern4
    // or a simple loop. For 16-bit, we use memset_pattern2. This is much
    // faster than calling put_pixel for each pixel because we avoid the
    // per-pixel clipping and format conversion overhead.

    if (m_bpp == 4) {
        for (int row = y0; row < y1; ++row) {
            uint32_t* dst = (uint32_t*)(m_back_buffer + (size_t)row * m_stride + (size_t)x0 * 4);
            for (int col = x0; col < x1; ++col) {
                *dst++ = native;
            }
        }
    } else if (m_bpp == 2) {
        uint16_t val16 = (uint16_t)native;
        for (int row = y0; row < y1; ++row) {
            uint16_t* dst = (uint16_t*)(m_back_buffer + (size_t)row * m_stride + (size_t)x0 * 2);
            for (int col = x0; col < x1; ++col) {
                *dst++ = val16;
            }
        }
    } else if (m_bpp == 3) {
        uint8_t b0 = (uint8_t)(native & 0xFF);
        uint8_t b1 = (uint8_t)((native >> 8) & 0xFF);
        uint8_t b2 = (uint8_t)((native >> 16) & 0xFF);
        for (int row = y0; row < y1; ++row) {
            uint8_t* dst = m_back_buffer + (size_t)row * m_stride + (size_t)x0 * 3;
            for (int col = x0; col < x1; ++col) {
                dst[0] = b0;
                dst[1] = b1;
                dst[2] = b2;
                dst += 3;
            }
        }
    }
}

void Display::copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if (!m_initialized || !m_back_buffer) return;

    // TEACHING NOTE: Copying within the back buffer
    // We must handle overlapping regions correctly. If dst is above src,
    // we should copy top-to-bottom. If dst is below src, copy bottom-to-top.
    // This is the same issue as memmove vs memcpy.

    // For simplicity, we copy row by row. If source and dest overlap,
    // we use memmove direction logic.

    bool copy_up = (dst_y < src_y);

    if (copy_up) {
        for (int row = 0; row < h; ++row) {
            uint8_t* src = m_back_buffer + (size_t)(src_y + row) * m_stride + (size_t)src_x * m_bpp;
            uint8_t* dst = m_back_buffer + (size_t)(dst_y + row) * m_stride + (size_t)dst_x * m_bpp;
            memmove(dst, src, (size_t)w * m_bpp);
        }
    } else {
        for (int row = h - 1; row >= 0; --row) {
            uint8_t* src = m_back_buffer + (size_t)(src_y + row) * m_stride + (size_t)src_x * m_bpp;
            uint8_t* dst = m_back_buffer + (size_t)(dst_y + row) * m_stride + (size_t)dst_x * m_bpp;
            memmove(dst, src, (size_t)w * m_bpp);
        }
    }
}

void Display::hline(int x, int y, int w, Color color) {
    fill_rect(x, y, w, 1, color);
}

void Display::vline(int x, int y, int h, Color color) {
    fill_rect(x, y, 1, h, color);
}

void Display::rect(int x, int y, int w, int h, Color color) {
    hline(x, y, w, color);
    hline(x, y + h - 1, w, color);
    vline(x, y, h, color);
    vline(x + w - 1, y, h, color);
}

// ============================================================================
// Buffer flipping
// ============================================================================

void Display::flip() {
    if (!m_initialized) return;

    if (m_backend == DisplayBackend::FRAMEBUFFER) {
        flip_framebuffer();
    } else if (m_backend == DisplayBackend::X11) {
        flip_x11();
    } else {
        flip_wayland();
    }
}

// TEACHING NOTE: Framebuffer flip (double buffering)
// =========================================================================
// When using hardware double buffering (yres_virtual = 2 * yres):
//   1. We have been drawing to the back buffer (second half of mmaped memory)
//   2. We call FBIOPAN_DISPLAY to move the visible area to the back buffer
//   3. We update m_fb_y_offset to track which half is visible
//   4. The old front buffer becomes the new back buffer
//
// When using a malloced back buffer (no hardware double buffering):
//   1. We memcpy the back buffer to the framebuffer
//   2. This is not truly double buffered - tearing can occur - but it is
//      better than per-pixel writes because at least the copy is fast
//      and might complete within a single vblank.

void Display::flip_framebuffer() {
    if (m_back_buffer_owned) {
        // No hardware double buffering - memcpy back buffer to framebuffer
        if (m_fb_mem && m_back_buffer) {
            memcpy(m_fb_mem, m_back_buffer, (size_t)m_stride * m_height);
        }
    } else {
        // Hardware double buffering - pan the display
        int new_y = (m_fb_y_offset == 0) ? m_height : 0;

        struct fb_var_screeninfo vinfo;
        if (ioctl(m_fb_fd, FBIOGET_VSCREENINFO, &vinfo) >= 0) {
            vinfo.yoffset = new_y;
            vinfo.xoffset = 0;
            if (ioctl(m_fb_fd, FBIOPAN_DISPLAY, &vinfo) >= 0) {
                m_fb_y_offset = new_y;
                // Switch back buffer pointer to the other half
                m_back_buffer = m_fb_mem + (size_t)m_stride * new_y;
            }
        }
    }
}

void Display::flip_x11() {
    if (!m_x11_conn || !m_x11_window || !m_back_buffer) return;

    X11Connection* conn = static_cast<X11Connection*>(m_x11_conn);
    X11Window* win = static_cast<X11Window*>(m_x11_window);

    // Send the back buffer contents to the X server
    win->put_image(conn, m_back_buffer, m_width, m_height, m_stride);
    win->flush(conn);
}

// TEACHING NOTE: Wayland flip (buffer commit)
// =========================================================================
// In Wayland, flipping means attaching our shared memory buffer to the
// surface, marking the whole surface as damaged, and committing.
//
// Unlike X11 where we copy pixel data to the server, in Wayland the
// compositor reads directly from our shared memory. We just tell it
// "this buffer is ready, please show it" via the commit request.
//
// After commit, the compositor owns the buffer until it sends
// wl_buffer::release. We must not write to the buffer while the
// compositor is using it. In this minimal implementation, we simply
// mark the buffer as busy and clear the flag when we receive the
// release event (or on the next frame if we do not get one).

void Display::flip_wayland() {
    if (!m_wl_conn || !m_wl_surface || !m_back_buffer) return;

    WaylandConnection* conn = static_cast<WaylandConnection*>(m_wl_conn);
    WaylandSurface* surf = static_cast<WaylandSurface*>(m_wl_surface);

    // If the buffer is still busy from the last frame, skip this flip.
    // In a proper implementation we would use double buffering (two pools
    // or one pool split into two buffers) to avoid stalls. For simplicity
    // we just wait for the compositor to release.
    if (surf->is_busy()) {
        // Try to process events to get the release
        conn->recv_available();
        conn->process_events();
        if (surf->is_busy()) {
            // Still busy - skip this frame. This causes a frame drop but
            // is better than corrupting the buffer.
            return;
        }
    }

    // Commit the frame: attach buffer, mark damage, commit surface
    surf->commit_frame(conn);
}

// ============================================================================
// Window management
// ============================================================================

void Display::set_title(const std::string& title) {
    if (m_backend == DisplayBackend::X11 && m_x11_conn && m_x11_window) {
        X11Connection* conn = static_cast<X11Connection*>(m_x11_conn);
        X11Window* win = static_cast<X11Window*>(m_x11_window);
        win->set_title(conn, title);
    } else if (m_backend == DisplayBackend::WAYLAND && m_wl_surface) {
        WaylandSurface* surf = static_cast<WaylandSurface*>(m_wl_surface);
        surf->set_title(title);
    }
    // Framebuffer mode: no title to set
}

bool Display::process_events() {
    if (m_backend == DisplayBackend::X11) {
        return process_x11_events();
    } else if (m_backend == DisplayBackend::WAYLAND) {
        return process_wayland_events();
    }
    // Framebuffer mode: no events to process (input handled separately)
    return true;
}

int Display::get_fd() const {
    if (!m_initialized) return -1;
    if (m_backend == DisplayBackend::X11 && m_x11_conn) {
        return static_cast<X11Connection*>(m_x11_conn)->get_fd();
    } else if (m_backend == DisplayBackend::WAYLAND && m_wl_conn) {
        return static_cast<WaylandConnection*>(m_wl_conn)->get_fd();
    }
    return -1;
}

bool Display::process_x11_events() {
    if (!m_x11_conn || !m_x11_window) return true;

    X11Connection* conn = static_cast<X11Connection*>(m_x11_conn);
    X11Window* win = static_cast<X11Window*>(m_x11_window);

    // Handle X11 events
    X11Event event;
    while (win->next_event(conn, &event)) {
        if (event.type == X11_EVENT_EXPOSE) {
            // Redraw on expose
            if (m_back_buffer) {
                win->put_image(conn, m_back_buffer, m_width, m_height, m_stride);
            }
        } else if (event.type == X11_EVENT_CLOSE) {
            return false;
        }
    }

    return true;
}

// TEACHING NOTE: Processing Wayland events
// =========================================================================
// In Wayland mode, we need to process events from the compositor. This
// includes keyboard events, pointer events, and buffer release events.
// We call the connection to receive and process available data, then
// check the surface for any pending events.
//
// Unlike X11 where we poll for events, in Wayland the compositor pushes
// events to us. We read available data from the socket and dispatch
// messages to their target objects.

bool Display::process_wayland_events() {
    if (!m_wl_conn || !m_wl_surface) return true;

    WaylandConnection* conn = static_cast<WaylandConnection*>(m_wl_conn);
    WaylandSurface* surf = static_cast<WaylandSurface*>(m_wl_surface);

    // Receive any available data from the compositor
    conn->recv_available();
    conn->process_events();

    // Check for surface events (buffer release, etc.)
    WaylandEvent event;
    bool valid = surf->process_events(conn, &event);

    // In a full implementation, we would dispatch keyboard/pointer events
    // to the GUI/input system. For now, we just check if the surface
    // is still valid.

    return valid;
}

} // namespace chinstrap