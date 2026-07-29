// wayland.hpp - Minimal Wayland protocol implementation over raw socket
//
// TEACHING NOTE: What is Wayland?
// ===========================================================================
// Wayland is a display server protocol designed as a modern replacement for
// X11. It was created in 2008 by Kristian Hoegsberg. Unlike X11, which uses
// a client-server model with a central X server, Wayland compositors handle
// both display management and compositing in one process.
//
// Key differences from X11:
//   - X11 has a central server that manages all windows. Clients talk to the
//     server, and the server talks to the display hardware. In Wayland, the
//     compositor IS the server. It composites client surfaces directly.
//   - X11 clients can read any window (security risk). Wayland clients can
//     only see their own surfaces.
//   - X11 has thousands of request types. Wayland has a small core protocol
//     with optional protocol extensions.
//   - X11 uses big-endian wire format. Wayland uses native byte order
//     (little-endian on x86/ARM). This makes marshalling simpler.
//   - X11 sends events for things like window exposure. Wayland clients are
//     responsible for keeping their own content up to date.
//
// Why implement Wayland from scratch?
//   - Zero third-party library requirement (libwayland-client is third-party)
//   - Understanding the Wayland protocol is educational
//   - We need only a tiny subset: connect, create surface, attach buffer,
//     receive keyboard/pointer events
//
// TEACHING NOTE: Wayland wire protocol basics
// =========================================================================
// The Wayland wire protocol is a binary message format used for all
// communication between clients and the compositor. It is defined in
// the Wayland protocol specification.
//
// Message format:
//   - Each message starts with a 2-byte object ID (the target object)
//   - Followed by a 2-byte value that combines opcode (lower 16 bits)
//     with the message size (upper 16 bits)
//   - Then the payload: arguments specific to the opcode
//   - All values are little-endian (native on x86/ARM)
//   - Messages are padded to 4-byte boundaries
//
// The size field (upper 16 bits of the second 2-byte word) includes the
// header itself (4 bytes) plus all arguments, padded to 4 bytes.
//
// Data types in the protocol:
//   - int:    4-byte signed integer
//   - uint:   4-byte unsigned integer
//   - fixed:  4-byte fixed-point (24.8 format)
//   - object: 4-byte object ID (new_id or existing)
//   - string: 4-byte length + string data + null terminator + padding
//   - array:  4-byte length + array data + padding
//   - fd:     file descriptor (sent as ancillary data via SCM_RIGHTS)
//
// File descriptors are special: they are not sent in the message payload
// but as ancillary data in the same sendmsg() call. This is how we pass
// shared memory file descriptors to the compositor.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace chinstrap {

// TEACHING NOTE: Wayland event types
// =========================================================================
// Like our X11 abstraction, we simplify Wayland events into a single struct
// with a type tag. The events we care about for a browser are:
//   - Key press/release (keyboard input)
//   - Mouse button press/release
//   - Mouse motion (pointer movement)
//   - Close (the compositor asked us to close)
// We do not handle touch events or other advanced input in this minimal
// implementation.

enum WaylandEventType {
    WL_EVENT_NONE = 0,
    WL_EVENT_KEY_PRESS = 1,
    WL_EVENT_KEY_RELEASE = 2,
    WL_EVENT_BUTTON_PRESS = 3,
    WL_EVENT_BUTTON_RELEASE = 4,
    WL_EVENT_MOTION = 5,
    WL_EVENT_CLOSE = 6,
};

struct WaylandEvent {
    WaylandEventType type;

    // Key event fields
    uint32_t key;       // Linux evdev keycode (raw, not X11 keysym)
    uint32_t unicode;   // Unicode codepoint if we could decode it

    // Mouse event fields
    int mouse_x;
    int mouse_y;
    int button;  // 1=left, 2=middle, 3=right, 4=scroll up, 5=scroll down

    WaylandEvent() : type(WL_EVENT_NONE), key(0), unicode(0),
                     mouse_x(0), mouse_y(0), button(0) {}
};

// TEACHING NOTE: Wayland core interfaces
// =========================================================================
// The Wayland protocol defines "interfaces" which are like classes. Each
// interface has a set of requests (client-to-server methods) and events
// (server-to-client messages). The core interfaces we need are:
//
//   wl_display    - The main display connection. Created on connect.
//                   Requests: sync, get_registry
//   wl_registry   - Binds server globals to client objects.
//                   Events: global, global_remove
//                   Requests: bind
//   wl_compositor - Creates surfaces. Request: create_surface
//   wl_shm        - Shared memory support. Request: create_pool
//   wl_shm_pool   - A shared memory buffer pool. Request: create_buffer
//   wl_buffer     - A pixel buffer. Events: release
//   wl_surface    - A drawable area. Requests: attach, damage, commit
//                   Events: enter, leave (for output)
//   wl_seat       - Input devices (keyboard, pointer, touch)
//                   Events: capabilities, name
//   wl_keyboard   - Keyboard input. Events: keymap, enter, leave, key,
//                   modifiers
//   wl_pointer    - Pointer (mouse) input. Events: enter, leave, motion,
//                   button, axis
//   wl_output     - Display output (monitor). Events: geometry, mode
//
// Each interface has a unique name string that the compositor advertises
// via the registry. We look up interfaces by name and bind them to
// object IDs.

// Wayland object IDs
// =========================================================================
// Object ID 0 is the null object (no object, used for errors).
// Object ID 1 is always the wl_display object (the display connection).
// Object IDs 2+ are assigned by the compositor for server-created objects
// (registry, globals) or by the client for client-created objects
// (surfaces, buffers, etc.).
//
// The ID space is split: the compositor assigns low IDs, the client assigns
// high IDs. The client starts at a configurable base (we use 1000).

// Core interface names (defined by the Wayland protocol specification)
// =========================================================================
// These name strings are part of the Wayland protocol. The compositor
// advertises them via wl_registry::global events. We match by name and
// bind to the interface version we support.
constexpr const char* WL_INTERFACE_COMPOSITOR = "wl_compositor";
constexpr const char* WL_INTERFACE_SHM         = "wl_shm";
constexpr const char* WL_INTERFACE_SEAT        = "wl_seat";
constexpr const char* WL_INTERFACE_OUTPUT     = "wl_output";
constexpr const char* WL_INTERFACE_SHM_POOL    = "wl_shm_pool";
constexpr const char* WL_INTERFACE_BUFFER      = "wl_buffer";
constexpr const char* WL_INTERFACE_SURFACE     = "wl_surface";
constexpr const char* WL_INTERFACE_KEYBOARD    = "wl_keyboard";
constexpr const char* WL_INTERFACE_POINTER    = "wl_pointer";

// xdg-shell interface names (for toplevel window management)
constexpr const char* WL_INTERFACE_XDG_WM_BASE  = "xdg_wm_base";
constexpr const char* WL_INTERFACE_XDG_SURFACE = "xdg_surface";
constexpr const char* WL_INTERFACE_XDG_TOPLEVEL = "xdg_toplevel";

// Core interface opcodes (from the Wayland protocol XML)
// =========================================================================
// These numeric opcodes identify which request or event is being sent.
// They are assigned in order within each interface definition.
//
// wl_display requests:
//   1 = sync       (get a callback for synchronization)
//   2 = get_registry (get the registry object)
//
// wl_display events:
//   1 = error      (fatal error)
//   2 = delete_id  (an object was destroyed on the server side)
//
// wl_registry requests:
//   1 = bind       (bind a global to a new client-side object)
//
// wl_registry events:
//   1 = global     (a new global is available)
//   2 = global_remove (a global was removed)
//
// wl_compositor requests:
//   1 = create_surface (create a new wl_surface)
//   2 = create_region  (create a wl_region for input/output clipping)
//
// wl_shm requests:
//   1 = create_pool   (create a wl_shm_pool from an fd)
//
// wl_shm events:
//   1 = format    (advertise a supported pixel format)
//
// wl_shm_pool requests:
//   1 = create_buffer (create a wl_buffer from a range of the pool)
//   2 = destroy    (destroy the pool)
//   3 = resize     (resize the pool)
//
// wl_buffer events:
//   1 = release   (compositor is done with the buffer)
//
// wl_surface requests:
//   1 = destroy
//   2 = attach    (attach a wl_buffer for the next commit)
//   3 = damage    (mark a region as damaged/needs redraw)
//   4 = frame     (request a frame callback for vsync)
//   5 = set_opaque_region
//   6 = set_input_region
//   7 = commit    (commit pending state: attach, damage, etc.)
//
// wl_surface events:
//   1 = enter     (surface entered an output)
//   2 = leave     (surface left an output)
//
// wl_seat events:
//   1 = capabilities  (bitfield: 1=pointer, 2=keyboard, 4=touch)
//   2 = name          (human-readable seat name)
//
// wl_seat requests:
//   1 = get_pointer
//   2 = get_keyboard
//   3 = get_touch
//
// wl_keyboard events:
//   1 = keymap    (keymap format and fd)
//   2 = enter     (keyboard focus entered our surface)
//   3 = leave     (keyboard focus left our surface)
//   4 = key       (key press or release)
//   5 = modifiers (modifier keys changed)
//
// wl_pointer events:
//   1 = enter     (pointer entered our surface)
//   2 = leave     (pointer left our surface)
//   3 = motion    (pointer moved)
//   4 = button    (mouse button press or release)
//   5 = axis      (scroll wheel motion)

// wl_shm pixel format codes (from the Wayland protocol)
// =========================================================================
// These are 32-bit codes identifying pixel formats. They are the CRTC
// (color) format of the buffer data. We use XRGB8888 which is the most
// widely supported format: 32 bits per pixel, 8 bits each for R, G, B,
// and a padding byte (X) that the compositor ignores.
//
// The format codes are the FOURCC format identifiers from drm_fourcc.h.
// They are encoded as 4 ASCII characters packed into a 32-bit integer.
// WL_SHM_FORMAT_XRGB8888 = 0x34325858 = "XX24" reversed = "XRGB"
// Actually the encoding is: the 4 characters in order are packed as
// byte 0 | byte 1 << 8 | byte 2 << 16 | byte 3 << 24
// So "XR24" would be: 'X' | 'R'<<8 | '2'<<16 | '4'<<24
// But the correct value for XRGB8888 is:
//   X = 0x58, R = 0x52, 2 = 0x32, 4 = 0x34
//   fourcc_code('X','R','2','4') = 0x58 | (0x52<<8) | (0x32<<16) | (0x34<<24)
//   = 0x34325858
// Wait, let me recheck. The drm_fourcc.h macro is:
//   fourcc_code(a,b,c,d) = (a) | (b<<8) | (c<<16) | (d<<24)
// So XRGB8888 = fourcc_code('X','R','2','4')
//   = 0x58 | (0x52<<8) | (0x32<<16) | (0x34<<24)
//   = 0x34325858
//
// For ARGB8888 (with alpha), it would be:
//   fourcc_code('A','R','2','4') = 0x34324141
//
// We use XRGB8888 (no alpha) because Wayland compositors composite
// surfaces and typically do not need per-pixel alpha for a browser
// window. The X byte is ignored by the compositor.

// Wayland SHM format constants
// We only support the most common format: XRGB8888 (32-bit, no alpha)
constexpr uint32_t WL_SHM_FORMAT_XRGB8888 = 0x34325858;
constexpr uint32_t WL_SHM_FORMAT_ARGB8888 = 0x34324141;

// wl_seat capability flags
constexpr uint32_t WL_SEAT_CAPABILITY_POINTER  = 1;
constexpr uint32_t WL_SEAT_CAPABILITY_KEYBOARD = 2;
constexpr uint32_t WL_SEAT_CAPABILITY_TOUCH     = 4;

// TEACHING NOTE: WaylandConnection class
// =========================================================================
// This class manages the socket connection to the Wayland compositor.
// It handles:
//   - Opening the socket (Unix domain socket in XDG_RUNTIME_DIR)
//   - The initial handshake (wl_registry to discover globals)
//   - Sending requests and receiving events
//   - Object ID management (client-side ID allocation)
//
// Unlike X11, the Wayland connection starts with object ID 1 already
// assigned to wl_display. The first thing we do is send a get_registry
// request to get the registry object, then read global events to discover
// the compositor, shm, and seat.

class WaylandConnection {
public:
    WaylandConnection();
    ~WaylandConnection();

    // Connect to the Wayland compositor.
    // Uses WAYLAND_DISPLAY env var (e.g. "wayland-0").
    // Falls back to "wayland-0" if not set.
    // The socket is in XDG_RUNTIME_DIR.
    bool connect();

    // Disconnect and close socket
    void disconnect();

    // Check if connected
    bool is_connected() const { return m_fd >= 0; }

    // Get the file descriptor (for poll/select)
    int get_fd() const { return m_fd; }

    // --- Object ID management ---
    // Allocate a new client-side object ID
    uint32_t allocate_id();

    // --- Low-level protocol ---
    // Send a message (object ID, opcode, payload) to the compositor.
    // The size is computed from the payload length.
    // If fd is valid (>= 0), it is sent as ancillary data (SCM_RIGHTS).
    void send_message(uint32_t object_id, uint16_t opcode,
                       const void* payload, size_t len, int fd = -1);

    // Receive available data from the compositor (non-blocking).
    // Returns the number of bytes read, or -1 on error.
    // Data is appended to m_recv_buf.
    ssize_t recv_available();

    // Process pending events from the compositor. This calls the
    // appropriate event handler for each complete message in the
    // receive buffer.
    void process_events();

    // --- Registry and globals ---
    // After connect(), we need to round-trip to the compositor to get
    // the registry and bind globals. This function does that.
    bool roundtrip();

    // Check if we have all required globals
    // Seat is optional - some compositors may not advertise it immediately,
    // and it is not needed for display-only purposes.
    bool has_globals() const {
        return m_compositor_id != 0 && m_shm_id != 0;
    }

    // Getters for global object IDs
    uint32_t get_compositor_id() const { return m_compositor_id; }
    uint32_t get_shm_id() const { return m_shm_id; }
    uint32_t get_seat_id() const { return m_seat_id; }
    uint32_t get_registry_id() const { return m_registry_id; }
    uint32_t get_xdg_wm_base_id() const { return m_xdg_wm_base_id; }

    // Register an xdg_surface ID so the connection can handle
    // configure events automatically (by sending ack_configure).
    void register_xdg_surface(uint32_t id) { m_event_xdg_surface_id = id; }

    // Get the output dimensions (from wl_output)
    int get_output_width() const { return m_output_width; }
    int get_output_height() const { return m_output_height; }

private:
    int m_fd;

    // Object ID allocation
    // Client-side IDs start at 1000 and go up.
    // Server-side IDs start at 2 (registry) and go up.
    uint32_t m_next_id;

    // Registry object ID (assigned by server via get_registry)
    uint32_t m_registry_id;

    // Global object IDs (bound from registry)
    uint32_t m_compositor_id;  // wl_compositor
    uint32_t m_shm_id;         // wl_shm
    uint32_t m_seat_id;        // wl_seat
    uint32_t m_output_id;      // wl_output (may be 0 if no output)
    uint32_t m_xdg_wm_base_id; // xdg_wm_base (may be 0 if not advertised)

    // Global version numbers
    uint32_t m_compositor_version;
    uint32_t m_shm_version;
    uint32_t m_seat_version;

    // Output dimensions
    int m_output_width;
    int m_output_height;

    // Receive buffer for incoming messages
    std::vector<uint8_t> m_recv_buf;

    // Helper: open the Wayland socket
    bool open_socket(const std::string& path);

    // Helper: send the get_registry request
    void send_get_registry();

    // Helper: send a bind request to the registry
    void send_bind(uint32_t name, const char* interface,
                   uint32_t version, uint32_t id);

    // Helper: process a single incoming message
    // Returns the number of bytes consumed, or 0 if incomplete.
    size_t process_one_message(const uint8_t* data, size_t len);

    // Helper: process a wl_registry global event
    void handle_registry_global(uint32_t name, const char* interface,
                                 uint32_t version);

    // Helper: process a wl_display error event
    void handle_display_error(uint32_t object_id, uint32_t code,
                               const char* message);

    // Helper: process a wl_output mode event
    void handle_output_mode(uint32_t flags, int32_t width, int32_t height);

    // Helper: send xdg_wm_base pong response to a ping
    void send_xdg_pong(uint32_t serial);

    // Helper: send xdg_surface ack_configure response
    void send_xdg_ack_configure(uint32_t xdg_surface_id, uint32_t serial);

    // We use a callback mechanism for sync round-trips.
    // When we send wl_display::sync, we get a callback done event.
    bool m_sync_done;
    uint32_t m_sync_callback_id;

    // SHM format support
    bool m_supports_xrgb8888;

    // xdg_surface event handling: when the connection receives an
    // xdg_surface::configure event, it needs to ack it. The WaylandSurface
    // registers its xdg_surface_id here so the connection can dispatch.
    uint32_t m_event_xdg_surface_id;

    // Pending events for the GUI layer
    std::vector<WaylandEvent> m_pending_events;

    friend class WaylandSurface;
};

// TEACHING NOTE: WaylandSurface class
// =========================================================================
// This class manages a Wayland surface and its associated buffer.
// It is analogous to X11Window but for Wayland.
//
// The lifecycle is:
//   1. Create a wl_surface via wl_compositor::create_surface
//   2. Create a shared memory pool (wl_shm::create_pool) with an fd
//   3. Create a wl_buffer from the pool (wl_shm_pool::create_buffer)
//   4. Attach the buffer to the surface (wl_surface::attach)
//   5. Mark the whole surface as damaged (wl_surface::damage)
//   6. Commit the surface (wl_surface::commit)
//
// For double buffering, we use two buffers (or one large pool split into
// two regions). The compositor sends wl_buffer::release when it is done
// with a buffer, at which point we can reuse it.
//
// Unlike X11 where the server keeps a copy of the window contents, in
// Wayland the client owns the buffer. The compositor reads from it and
// returns it when done. This means we must not write to a buffer that
// the compositor is currently using. We track this with m_buffer_busy.

class WaylandSurface {
public:
    WaylandSurface();
    ~WaylandSurface();

    // Create a surface on the given connection with the given dimensions.
    // This creates the wl_surface, allocates the shared memory pool,
    // and creates the wl_buffer.
    bool create(WaylandConnection* conn, int width, int height);

    // Destroy the surface and free resources
    void destroy();

    // Attach the shared memory buffer and commit (present the frame).
    // This sends the pixel data to the compositor.
    void commit_frame(WaylandConnection* conn);

    // Get a pointer to the shared memory pixel buffer.
    // The caller writes pixels here, then calls commit_frame().
    // The buffer is width * height * 4 bytes (XRGB8888 format).
    // The first byte of each pixel is B, then G, then R, then X (unused).
    // This matches the common BGRA/X format used by most display hardware.
    uint8_t* get_buffer() { return static_cast<uint8_t*>(m_shm_ptr); }

    // Get the buffer stride in bytes (width * 4)
    int get_stride() const { return m_width * 4; }

    // Get dimensions
    int get_width() const { return m_width; }
    int get_height() const { return m_height; }

    // Mark the buffer as released (compositor is done with it).
    // Called when we receive wl_buffer::release.
    void mark_released() { m_buffer_busy = false; }

    // Check if the buffer is currently in use by the compositor
    bool is_busy() const { return m_buffer_busy; }

    // Get the surface object ID
    uint32_t get_surface_id() const { return m_surface_id; }

    // Set the title (using xdg_toplevel::set_title).
    void set_title(const std::string& title);

    // Process events from the compositor for this surface.
    // This should be called after WaylandConnection::process_events().
    // Returns true if the surface is still valid, false if closed.
    bool process_events(WaylandConnection* conn, WaylandEvent* event);

private:
    WaylandConnection* m_conn;

    // Object IDs
    uint32_t m_surface_id;      // wl_surface
    uint32_t m_buffer_id;       // wl_buffer
    uint32_t m_pool_id;         // wl_shm_pool
    uint32_t m_xdg_surface_id;  // xdg_surface
    uint32_t m_xdg_toplevel_id; // xdg_toplevel

    // Shared memory
    int m_shm_fd;             // file descriptor for the shared memory
    void* m_shm_ptr;          // mmaped pointer to the shared memory
    size_t m_shm_size;        // size of the shared memory region

    // Dimensions
    int m_width;
    int m_height;

    // Buffer state
    bool m_buffer_busy;       // true if compositor is using our buffer
    bool m_initialized;

    // Helper: create the shared memory pool and buffer
    bool create_shm_buffer(WaylandConnection* conn);

    // Helper: create the wl_surface
    bool create_surface(WaylandConnection* conn);

    // Helper: create xdg_surface and xdg_toplevel for window management
    bool create_xdg_surface(WaylandConnection* conn);

    // Helper: generate a unique filename for shm_open
    static std::string shm_filename();
};

} // namespace chinstrap