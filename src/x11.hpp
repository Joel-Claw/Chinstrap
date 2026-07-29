// x11.hpp - Minimal X11 protocol implementation over raw socket
//
// TEACHING NOTE: What is X11?
// ===========================================================================
// X11 (X Window System protocol, version 11) is the traditional windowing
// system for Unix-like operating systems. It was created in 1987 at MIT.
//
// How X11 works:
//   - An X server manages the display hardware (screen, keyboard, mouse)
//   - Client applications connect to the server via a socket (usually
//     a Unix domain socket at /tmp/.X11-unix/X0, or TCP port 6000+display)
//   - Clients send requests (create window, draw, etc.) to the server
//   - The server sends events (key press, mouse move, expose) to clients
//   - All communication is binary, using a well-defined wire format
//
// Xlib is the standard C library that wraps the X11 protocol. It provides
// convenient functions like XOpenDisplay(), XCreateWindow(), etc. But Xlib
// is just a thin wrapper over the wire protocol - everything it does can
// be done by sending the right bytes over a socket.
//
// Why implement X11 from scratch?
//   - Zero third-party library requirement (Xlib is a third-party library)
//   - Understanding the X11 protocol is educational
//   - We need only a tiny subset: connect, create window, map, draw, events
//
// Why is Wayland replacing X11?
//   X11 is a 35-year-old protocol with massive complexity, security issues
//   (any client can read any other client key events), and a bloated
//   codebase. Wayland is a simpler, more secure protocol where the compositor
//   directly manages clients without the intermediary server model.
//   However, X11 remains widely used and is the default on most Linux desktops.
//
// This implementation supports only the minimum needed by Chinstrap:
//   - Establish connection and authenticate
//   - Create a window
//   - Map (show) the window
//   - Set window title (WM_PROTOCOLS + WM_DELETE_WINDOW for close button)
//   - Receive Expose, KeyPress, KeyRelease, ButtonPress, ButtonRelease,
//     MotionNotify, and ClientMessage events
//   - Copy pixel data to the window (PutImage)
//
// TEACHING NOTE: X11 wire format basics
// =========================================================================
// X11 uses a binary format where:
//   - All multi-byte values are big-endian (network byte order)
//   - Data is padded to 4-byte boundaries
//   - Requests start with a 1-byte opcode
//   - The second byte is either a parameter or zero
//   - The next 2 bytes are the request length in 4-byte units
//   - The rest is request-specific data
//
// Replies (for requests that expect them) have a similar format:
//   - 1 byte: type (1 = reply)
//   - 1 byte: unused
//   - 2 bytes: sequence number
//   - 4 bytes: length of remaining data in 4-byte units
//
// Events:
//   - 1 byte: type code (2-34 for core events)
//   - Rest depends on event type
//   - Always 32 bytes (8 longwords)

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace chinstrap {

// TEACHING NOTE: X11 event types
// =========================================================================
// X11 defines many event types. We only need a subset for a browser:
//   - Expose: the window needs redrawing (part was covered/uncovered)
//   - KeyPress/KeyRelease: keyboard input
//   - ButtonPress/ButtonRelease: mouse button input
//   - MotionNotify: mouse movement
//   - ClientMessage: used for WM_DELETE_WINDOW (window close)
// We simplify these into a single struct with a type tag.

enum X11EventType {
    X11_EVENT_NONE = 0,
    X11_EVENT_EXPOSE = 1,
    X11_EVENT_KEY_PRESS = 2,
    X11_EVENT_KEY_RELEASE = 3,
    X11_EVENT_BUTTON_PRESS = 4,
    X11_EVENT_BUTTON_RELEASE = 5,
    X11_EVENT_MOTION = 6,
    X11_EVENT_CLOSE = 7,
};

struct X11Event {
    X11EventType type;

    // Key event fields
    uint32_t keycode;
    uint32_t keysym;

    // Mouse event fields
    int mouse_x;
    int mouse_y;
    int button;  // 1=left, 2=middle, 3=right, 4=scroll up, 5=scroll down

    // Expose fields
    int expose_x;
    int expose_y;
    int expose_w;
    int expose_h;

    X11Event() : type(X11_EVENT_NONE), keycode(0), keysym(0),
                 mouse_x(0), mouse_y(0), button(0),
                 expose_x(0), expose_y(0), expose_w(0), expose_h(0) {}
};

// TEACHING NOTE: X11 connection state
// =========================================================================
// The X11Connection class manages the socket connection to the X server.
// It handles:
//   - Opening the socket (Unix domain or TCP)
//   - The initial handshake (send client setup, receive server info)
//   - Authentication (if the server requires it, via MIT-MAGIC-COOKIE-1)
//   - Sending requests and receiving replies
//   - Tracking the sequence number (for matching replies to requests)
//
// The connection must be established before any X11 operations can be done.

class X11Connection {
public:
    X11Connection();
    ~X11Connection();

    // Connect to the X server.
    // Uses the DISPLAY environment variable (e.g. ":0" means display 0 on local host)
    // Falls back to ":0" if not set.
    bool connect();

    // Disconnect and close socket
    void disconnect();

    // Check if connected
    bool is_connected() const { return m_fd >= 0; }

    // Get the file descriptor (for select/poll)
    int get_fd() const { return m_fd; }

    // --- Low-level protocol ---
    // Send raw bytes (padded to 4-byte boundary)
    void send(const void* data, size_t len);

    // Send a request with a 4-byte aligned length
    void send_request(uint8_t opcode, const void* data, size_t len);

    // Receive data (blocking read of exactly len bytes)
    void receive(void* data, size_t len);

    // Get the default screen root window
    uint32_t get_root_window() const { return m_root_window; }

    // Get the allocated resource ID base (for generating XIDs)
    uint32_t get_resource_base() const { return m_resource_id_base; }
    uint32_t get_resource_mask() const { return m_resource_id_mask; }

    // Allocate a new XID (resource ID for windows, pixmaps, etc.)
    uint32_t allocate_id();

    // Get visual info
    uint32_t get_root_visual() const { return m_root_visual; }
    int get_depth() const { return m_depth; }

    // Flush pending output
    void flush();

private:
    int m_fd;
    uint32_t m_root_window;
    uint32_t m_root_visual;
    int m_depth;
    uint32_t m_resource_id_base;
    uint32_t m_resource_id_mask;
    uint32_t m_resource_id_next;

    // Buffer for output (X11 allows batching requests)
    std::vector<uint8_t> m_output_buf;

    bool open_unix_socket(const std::string& path);
    bool open_tcp_socket(const std::string& host, int port);
    bool do_handshake();
};

// TEACHING NOTE: X11 window management
// =========================================================================
// X11Window wraps the X11 window-related protocol requests:
//   - CreateWindow: allocate a window XID and create it as a child of the
//     root window with the given dimensions and visual
//   - MapWindow: make the window visible (it appears on screen)
//   - ChangeProperty: set properties like WM_NAME (window title) and
//     WM_PROTOCOLS (to receive close-button events)
//   - PutImage: copy a rectangular block of pixel data to the window
//
// The window gets a back buffer implicitly through the PutImage request:
// we draw to our local buffer and then send it to the X server.

class X11Window {
public:
    X11Window();
    ~X11Window();

    // Create a window on the given connection
    bool create(X11Connection* conn, int width, int height, const std::string& title);

    // Destroy the window
    void destroy(X11Connection* conn);

    // Map (show) the window
    void map(X11Connection* conn);

    // Unmap (hide) the window
    void unmap(X11Connection* conn);

    // Set the window title (WM_NAME property)
    void set_title(X11Connection* conn, const std::string& title);

    // Copy pixel data to the window (PutImage request)
    // data is in 32-bit BGRA format, stride in bytes
    void put_image(X11Connection* conn, uint8_t* data, int w, int h, int stride);

    // Flush pending requests to the server
    void flush(X11Connection* conn);

    // Get the next event (returns true if an event was available)
    bool next_event(X11Connection* conn, X11Event* event);

    // Get the window XID
    uint32_t get_id() const { return m_window_id; }

private:
    uint32_t m_window_id;
    uint32_t m_pixmap_id;   // for double buffering on the server side
    uint32_t m_gc_id;       // graphics context ID
    int m_width;
    int m_height;

    // Buffer for pending events from the server
    std::vector<uint8_t> m_event_buf;

    // Helper: send CreateWindow request
    void send_create_window(X11Connection* conn, int w, int h);

    // Helper: send CreateGC request
    void send_create_gc(X11Connection* conn);

    // Helper: send CreatePixmap request
    void send_create_pixmap(X11Connection* conn, int w, int h);

    // Helper: parse an X11 event from raw bytes
    bool parse_event(const uint8_t* data, size_t len, X11Event* event);

    // Helper: convert X11 keycode to keysym (basic US keyboard mapping)
    uint32_t keycode_to_keysym(uint8_t keycode, bool shift);
};

} // namespace chinstrap