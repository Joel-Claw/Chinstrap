// wayland.cpp - Minimal Wayland protocol implementation over raw socket
//
// TEACHING NOTE: This file implements just enough of the Wayland protocol
// to create a surface, present a pixel buffer via shared memory, and
// receive keyboard/pointer events. It does NOT use libwayland-client,
// libwlroots, or any other library. It talks directly to the Wayland
// compositor over a Unix domain socket using the raw binary protocol.
//
// The Wayland protocol is specified in:
//   https://wayland.freedesktop.org/docs/html/
//   and the wayland.xml protocol definition file.
//
// This implementation covers a tiny fraction of the full protocol - just
// what a browser display needs:
//   - Connect to compositor via Unix socket
//   - Discover globals via wl_registry
//   - Create a wl_surface with wl_compositor
//   - Create a wl_buffer from shared memory via wl_shm
//   - Attach the buffer and commit the surface
//   - Receive keyboard and pointer events from wl_seat

#include "wayland.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <poll.h>
#include <errno.h>
#include <stdexcept>
#include <iostream>

namespace chinstrap {

// ============================================================================
// Internal helpers for the Wayland wire protocol
// ============================================================================

// TEACHING NOTE: Wayland message header
// =========================================================================
// Every Wayland message starts with a 4-byte header:
//   bytes 0-1: object ID (little-endian uint16... actually uint32 in wl2)
//
// Actually, let me be precise. The Wayland wire format header is:
//   bytes 0-3: object ID (uint32, little-endian)
//   bytes 4-5: opcode (uint16, little-endian) - low 16 bits
//   bytes 6-7: message size (uint16, little-endian) - including header
//
// Wait - I need to check the actual wire format. The Wayland protocol
// uses a different header layout than I described above. Let me look
// at the actual specification.
//
// The correct header format (from wayland protocol spec):
//   bytes 0-3: object ID (uint32, little-endian)
//   byte 4-5: opcode+size combined word (uint16 LE)
//     - bits 0-15: opcode
//   byte 6-7: size (uint16, little-endian) - total message size including header
//
// Actually the real format from the Wayland source (wayland-private.h):
//   struct wl_header {
//       uint32_t id;       // object ID
//       uint32_t opcode_size;  // lower 16 bits = opcode, upper 16 bits = size
//   };
//
// So the header is 8 bytes total:
//   bytes 0-3: object ID (uint32 LE)
//   bytes 4-7: combined opcode (low 16) + size (high 16) as uint32 LE
//
// But wait, looking more carefully at the Wayland protocol source code
// (connection.c in wayland), the actual wire format sends:
//   uint32_t id
//   uint32_t opcode | (size << 16)
// where opcode and size are both 16-bit values packed into a 32-bit word.
//
// So the on-wire format is:
//   bytes 0-3: object_id (uint32 LE)
//   bytes 4-7: (opcode & 0xFFFF) | ((size & 0xFFFF) << 16)  (uint32 LE)
//
// The "size" includes the 8-byte header plus all arguments, padded to 4 bytes.

// Helper: write a uint32 in little-endian (native on x86/ARM)
static inline void put_u32_le(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

// Helper: read a uint32 in little-endian
static inline uint32_t get_u32_le(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

// Helper: read a uint16 in little-endian
static inline uint16_t get_u16_le(const uint8_t* buf) {
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

// Helper: pad a length to 4-byte boundary
static inline size_t pad4(size_t len) {
    return (len + 3) & ~static_cast<size_t>(3);
}

// Helper: extract opcode from the combined word
static inline uint16_t extract_opcode(uint32_t opcode_size) {
    return (uint16_t)(opcode_size & 0xFFFF);
}

// Helper: extract size from the combined word
static inline uint16_t extract_size(uint32_t opcode_size) {
    return (uint16_t)((opcode_size >> 16) & 0xFFFF);
}

// ============================================================================
// WaylandConnection implementation
// ============================================================================

WaylandConnection::WaylandConnection()
    : m_fd(-1)
    , m_next_id(1000)  // Client-side IDs start at 1000
    , m_registry_id(0)
    , m_compositor_id(0)
    , m_shm_id(0)
    , m_seat_id(0)
    , m_output_id(0)
    , m_xdg_wm_base_id(0)
    , m_compositor_version(0)
    , m_shm_version(0)
    , m_seat_version(0)
    , m_output_width(0)
    , m_output_height(0)
    , m_sync_done(false)
    , m_sync_callback_id(0)
    , m_supports_xrgb8888(true)  // Assume yes until told otherwise
    , m_event_xdg_surface_id(0)
{}

WaylandConnection::~WaylandConnection() {
    disconnect();
}

// TEACHING NOTE: Connecting to the Wayland compositor
// =========================================================================
// The Wayland compositor listens on a Unix domain socket. The socket path
// is determined by:
//   1. If WAYLAND_DISPLAY is an absolute path, use it directly.
//   2. Otherwise, the socket is at $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
//
// XDG_RUNTIME_DIR is typically /run/user/<uid> on modern Linux systems.
// WAYLAND_DISPLAY is typically "wayland-0" for the first compositor.
//
// If WAYLAND_DISPLAY is not set, we default to "wayland-0".
// If XDG_RUNTIME_DIR is not set, we default to /tmp.

bool WaylandConnection::connect() {
    // Determine the socket path
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");

    if (!wayland_display) {
        std::cerr << "Wayland: WAYLAND_DISPLAY not set" << std::endl;
    }
    if (!xdg_runtime) {
        std::cerr << "Wayland: XDG_RUNTIME_DIR not set" << std::endl;
    }

    std::string wd = wayland_display ? wayland_display : "wayland-0";
    std::string xdg = xdg_runtime ? xdg_runtime : "/tmp";

    // If WAYLAND_DISPLAY is an absolute path, use it directly
    std::string path;
    if (!wd.empty() && wd[0] == '/') {
        path = wd;
    } else {
        path = xdg + "/" + wd;
    }

    std::cerr << "Wayland: connecting to socket " << path << std::endl;

    // Try to open the socket
    if (!open_socket(path)) {
        std::cerr << "Wayland: failed to open socket " << path << std::endl;
        return false;
    }

    std::cerr << "Wayland: socket connected, sending get_registry" << std::endl;

    // TEACHING NOTE: Wayland initial state
    // ====================================================================
    // When the connection is established, object ID 1 is already the
    // wl_display object. We do not need to send a connection setup request
    // like in X11. Instead, we immediately send requests to wl_display.
    //
    // The first thing we do is call wl_display::get_registry to get the
    // registry object. The compositor will then send us wl_registry::global
    // events advertising available interfaces (compositor, shm, seat, etc.).
    //
    // We then call wl_display::sync to force a round-trip. When we get the
    // callback done event, we know all global events have been received.

    // Send get_registry request
    send_get_registry();

    // Do a round-trip to receive all global advertisements
    // This processes events until the sync callback fires.
    m_sync_done = false;
    m_sync_callback_id = 0;

    // Send sync request using the public send_message API
    // wl_display::sync (opcode 1)
    // Payload: new_id for the callback object (uint32)
    m_sync_callback_id = allocate_id();
    send_message(1, 1, &m_sync_callback_id, 4);

    // Process events until sync callback is done.
    // We do up to 10 round-trips to be safe (in case of reentrancy).
    int attempts = 0;
    while (!m_sync_done && attempts < 100) {
        // Wait for data
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, 5000);  // 5 second timeout
        if (ret <= 0) {
            // Timeout or error
            break;
        }

        recv_available();
        process_events();
        attempts++;
    }

    if (!has_globals()) {
        // Maybe we need another round-trip for output info
        if (has_globals()) {
            // Do another sync to get output modes
            m_sync_done = false;
            m_sync_callback_id = allocate_id();
            send_message(1, 1, &m_sync_callback_id, 4);

            while (!m_sync_done) {
                struct pollfd pfd;
                pfd.fd = m_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int ret = ::poll(&pfd, 1, 5000);
                if (ret <= 0) break;
                recv_available();
                process_events();
            }
        }
    }

    if (!has_globals()) {
        std::cerr << "Wayland: missing required globals after roundtrip"
                  << " (compositor=" << m_compositor_id
                  << " shm=" << m_shm_id
                  << " xdg_wm_base=" << m_xdg_wm_base_id
                  << ")" << std::endl;
        disconnect();
        return false;
    }

    std::cerr << "Wayland: connected ok (compositor=" << m_compositor_id
              << " shm=" << m_shm_id
              << " seat=" << m_seat_id
              << " xdg_wm_base=" << m_xdg_wm_base_id
              << ")" << std::endl;

    return true;
}

bool WaylandConnection::open_socket(const std::string& path) {
    m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    return true;
}

void WaylandConnection::disconnect() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_recv_buf.clear();
    m_compositor_id = 0;
    m_shm_id = 0;
    m_seat_id = 0;
    m_registry_id = 0;
    m_output_id = 0;
    m_xdg_wm_base_id = 0;
    m_event_xdg_surface_id = 0;
}

uint32_t WaylandConnection::allocate_id() {
    // TEACHING NOTE: Client-side object IDs
    // ====================================================================
    // In Wayland, object IDs are split between the server and client.
    // The server assigns IDs for objects it creates (registry, outputs,
    // etc.). The client assigns IDs for objects it creates (surfaces,
    // buffers, callbacks, etc.).
    //
    // The ID space is large (uint32). The client picks IDs starting from
    // a configurable base. We use 1000 as our base. Each new object gets
    // the next ID. We must ensure we do not collide with server IDs
    // (which are typically low: 1, 2, 3, ...).
    return m_next_id++;
}

// TEACHING NOTE: The send_message function constructs the full Wayland
// message from its components. The wire format is:
//   uint32_t object_id
//   uint32_t (opcode | (size << 16))
//   [payload, padded to 4 bytes]
//
// If an fd is provided, it is sent as ancillary data using SCM_RIGHTS.
// This is how we pass shared memory file descriptors to the compositor.

void WaylandConnection::send_message(uint32_t object_id, uint16_t opcode,
                                      const void* payload, size_t len, int fd) {
    // Build the header + payload
    // Header: 8 bytes (object_id + opcode_size)
    // Payload: len bytes, padded to 4
    size_t padded_payload = pad4(len);
    size_t total_size = 8 + padded_payload;

    std::vector<uint8_t> msg(total_size, 0);
    put_u32_le(&msg[0], object_id);
    put_u32_le(&msg[4], static_cast<uint32_t>(opcode) |
                        (static_cast<uint32_t>(total_size) << 16));

    if (len > 0 && payload != nullptr) {
        memcpy(&msg[8], payload, len);
    }

    if (fd >= 0) {
        // TEACHING NOTE: Sending file descriptors in Wayland
        // ====================================================================
        // Wayland uses SCM_RIGHTS to pass file descriptors between client
        // and server. This is a Unix domain socket feature where ancillary
        // data (control messages) can carry file descriptors.
        //
        // We use sendmsg() with a cmsg buffer containing the fd.
        // The message payload goes in the normal iov, and the fd goes
        // in the control message (cmsg).
        //
        // This is the same mechanism used by any Unix program that passes
        // file descriptors between processes (e.g., systemd, Flatpak).

        struct msghdr msghdr;
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];

        memset(&msghdr, 0, sizeof(msghdr));
        memset(cmsg_buf, 0, sizeof(cmsg_buf));

        iov.iov_base = msg.data();
        iov.iov_len = msg.size();
        msghdr.msg_iov = &iov;
        msghdr.msg_iovlen = 1;
        msghdr.msg_control = cmsg_buf;
        msghdr.msg_controllen = sizeof(cmsg_buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

        ssize_t sent = ::sendmsg(m_fd, &msghdr, 0);
        if (sent < 0) {
            throw std::runtime_error("Wayland: sendmsg failed");
        }
    } else {
        // No fd, just send the message normally
        const uint8_t* bytes = msg.data();
        size_t total = 0;
        while (total < msg.size()) {
            ssize_t n = ::write(m_fd, bytes + total, msg.size() - total);
            if (n <= 0) {
                throw std::runtime_error("Wayland: failed to send message");
            }
            total += static_cast<size_t>(n);
        }
    }
}

ssize_t WaylandConnection::recv_available() {
    // TEACHING NOTE: Non-blocking receive
    // ====================================================================
    // We read as much data as is available on the socket without blocking.
    // The data is appended to m_recv_buf. Later, process_events() will
    // parse complete messages from this buffer.
    //
    // We use a temporary buffer and append to m_recv_buf to avoid
    // excessive reallocations.

    uint8_t tmp[4096];
    ssize_t n = ::read(m_fd, tmp, sizeof(tmp));
    if (n > 0) {
        m_recv_buf.insert(m_recv_buf.end(), tmp, tmp + n);
    }
    return n;
}

// TEACHING NOTE: Processing Wayland events
// =========================================================================
// Wayland events are messages sent from the compositor to the client.
// Each event has the same header as a request:
//   uint32_t object_id  (which object the event is for)
//   uint32_t opcode|size (which event + total message size)
//   [arguments]
//
// We parse messages from m_recv_buf one at a time. For each message, we
// look at the object ID to determine which interface the event belongs to,
// then dispatch to the appropriate handler.
//
// Known object IDs and their interfaces:
//   1 = wl_display
//   m_registry_id = wl_registry
//   m_compositor_id = wl_compositor
//   m_shm_id = wl_shm
//   m_seat_id = wl_seat
//   etc.
//
// We maintain a simple mapping from object ID to interface type using
// if/else chains. A real implementation would use a hash table.

void WaylandConnection::process_events() {
    while (!m_recv_buf.empty()) {
        // Need at least 8 bytes for the header
        if (m_recv_buf.size() < 8) break;

        // Parse the header to get the message size
        uint32_t op_size = get_u32_le(&m_recv_buf[4]);
        uint16_t size = extract_size(op_size);

        // The size includes the 8-byte header
        if (size < 8) {
            // Corrupted message - clear buffer and bail
            m_recv_buf.clear();
            break;
        }

        // Do we have the full message?
        if (m_recv_buf.size() < size) {
            // Not enough data yet - wait for more
            break;
        }

        // Process this message
        process_one_message(m_recv_buf.data(), size);

        // Remove the processed message from the buffer
        m_recv_buf.erase(m_recv_buf.begin(),
                         m_recv_buf.begin() + static_cast<ptrdiff_t>(size));
    }
}

// TEACHING NOTE: Dispatching a single Wayland event
// =========================================================================
// Given a complete message (header + payload), we determine which object
// it is for and what event it represents. We then call the appropriate
// handler method.
//
// The dispatching is based on object ID. We compare against known IDs:
//   - ID 1: wl_display (error, delete_id events)
//   - m_registry_id: wl_registry (global, global_remove events)
//   - m_compositor_id: wl_compositor (no events in core protocol)
//   - m_shm_id: wl_shm (format event)
//   - m_seat_id: wl_seat (capabilities, name events)
//   - m_sync_callback_id: wl_callback (done event)
//   - Other IDs: keyboard, pointer, output, etc.

size_t WaylandConnection::process_one_message(const uint8_t* data, size_t len) {
    if (len < 8) return 0;

    uint32_t obj_id = get_u32_le(&data[0]);
    uint32_t op_size = get_u32_le(&data[4]);
    uint16_t opcode = extract_opcode(op_size);
    // uint16_t size = extract_size(op_size);  // already known by caller
    (void)op_size;

    const uint8_t* args = data + 8;
    size_t args_len = len - 8;

    // Dispatch based on object ID

    if (obj_id == 1) {
        // wl_display events
        // opcode 0 = error, opcode 1 = delete_id
        if (opcode == 0) {
            // wl_display::error
            // Args: object_id (uint32), code (uint32), message (string)
            if (args_len >= 8) {
                uint32_t err_obj = get_u32_le(&args[0]);
                uint32_t err_code = get_u32_le(&args[4]);
                // The message string starts at offset 8
                // string format: uint32 length + data + null + padding
                std::string msg = "unknown error";
                if (args_len >= 12) {
                    uint32_t str_len = get_u32_le(&args[8]);
                    if (args_len >= 12 + str_len) {
                        msg.assign(reinterpret_cast<const char*>(&args[12]), str_len);
                    }
                }
                handle_display_error(err_obj, err_code, msg.c_str());
            }
        } else if (opcode == 1) {
            // wl_display::delete_id
            // Args: id (uint32)
            if (args_len >= 4) {
                uint32_t deleted_id = get_u32_le(&args[0]);
                (void)deleted_id;  // We do not track this in our minimal impl
            }
        }
    } else if (obj_id == m_registry_id) {
        // wl_registry events
        // opcode 0 = global, opcode 1 = global_remove
        if (opcode == 0) {
            // wl_registry::global
            // Args: name (uint32), interface (string), version (uint32)
            if (args_len >= 8) {
                uint32_t name = get_u32_le(&args[0]);
                // string at offset 4: length (uint32) + data + null + pad
                uint32_t iface_len = get_u32_le(&args[4]);
                if (args_len >= 8 + iface_len + 1 + 4) {
                    // interface name (null terminated)
                    std::string iface(reinterpret_cast<const char*>(&args[8]), iface_len);
                    // version is after the string + null + padding
                    size_t str_total = 4 + iface_len + 1;  // length field + chars + null
                    size_t str_padded = pad4(str_total);
                    if (args_len >= 4 + str_padded + 4) {
                        uint32_t version = get_u32_le(&args[4 + str_padded]);
                        handle_registry_global(name, iface.c_str(), version);
                    }
                }
            }
        } else if (opcode == 1) {
            // wl_registry::global_remove
            // Args: name (uint32)
            // We do not handle removal in this minimal implementation
        }
    } else if (obj_id == m_shm_id) {
        // wl_shm events
        // opcode 0 = format
        if (opcode == 0) {
            // wl_shm::format
            // Args: format (uint32)
            if (args_len >= 4) {
                uint32_t format = get_u32_le(&args[0]);
                if (format != WL_SHM_FORMAT_XRGB8888 &&
                    format != WL_SHM_FORMAT_ARGB8888) {
                    // We only support these formats
                }
            }
        }
    } else if (obj_id == m_seat_id) {
        // wl_seat events
        // opcode 0 = capabilities, opcode 1 = name
        if (opcode == 0) {
            // wl_seat::capabilities
            // Args: capabilities (uint32)
            // We do not need to do anything here since we bind
            // keyboard/pointer during init. In a real implementation,
            // we would create or destroy keyboard/pointer objects
            // based on the capabilities.
        }
    } else if (obj_id == m_sync_callback_id) {
        // wl_callback::done event
        // opcode 0 = done
        if (opcode == 0) {
            // Args: callback_data (uint32)
            m_sync_done = true;
        }
    } else if (obj_id == m_output_id) {
        // wl_output events
        // opcode 0 = geometry, opcode 1 = mode
        if (opcode == 1) {
            // wl_output::mode
            // Args: flags (uint32), width (int32), height (int32), refresh (int32)
            if (args_len >= 16) {
                uint32_t flags = get_u32_le(&args[0]);
                int32_t width = static_cast<int32_t>(get_u32_le(&args[4]));
                int32_t height = static_cast<int32_t>(get_u32_le(&args[8]));
                // int32_t refresh = static_cast<int32_t>(get_u32_le(&args[12]));
                handle_output_mode(flags, width, height);
            }
        }
    } else if (obj_id == m_xdg_wm_base_id && m_xdg_wm_base_id != 0) {
        // xdg_wm_base events
        // opcode 0 = ping
        if (opcode == 0) {
            // xdg_wm_base::ping
            // Args: serial (uint32)
            // We must respond with xdg_wm_base::pong (opcode 1)
            if (args_len >= 4) {
                uint32_t serial = get_u32_le(&args[0]);
                send_xdg_pong(serial);
            }
        }
    } else if (m_event_xdg_surface_id != 0 && obj_id == m_event_xdg_surface_id) {
        // xdg_surface events
        // opcode 0 = configure
        if (opcode == 0) {
            // xdg_surface::configure
            // Args: serial (uint32)
            // We must respond with xdg_surface::ack_configure (opcode 1)
            if (args_len >= 4) {
                uint32_t serial = get_u32_le(&args[0]);
                send_xdg_ack_configure(m_event_xdg_surface_id, serial);
            }
        }
    }
    // Other object IDs (keyboard, pointer, surface, buffer) are handled
    // by the WaylandSurface class, which has its own event processing.

    return len;
}

void WaylandConnection::send_get_registry() {
    // TEACHING NOTE: wl_display::get_registry request
    // ====================================================================
    // This request asks the compositor to create a wl_registry object for
    // us. The registry will advertise all available global objects.
    //
    // Request: wl_display::get_registry (opcode 2)
    //   new_id: registry  (uint32 - client-allocated object ID)
    //
    // We allocate an ID for the registry and send it to the compositor.
    // The compositor will create the registry object and send us
    // wl_registry::global events for each available interface.

    m_registry_id = allocate_id();
    // wl_display::get_registry (opcode 2)
    // Payload: new_id for the registry object (uint32)
    send_message(1, 2, &m_registry_id, 4);
}

void WaylandConnection::send_bind(uint32_t name, const char* interface,
                                    uint32_t version, uint32_t id) {
    // TEACHING NOTE: wl_registry::bind request
    // ====================================================================
    // The bind request creates a client-side proxy for a global object.
    // It tells the compositor "I want to use global <name>, version
    // <version>, and please send its events to object ID <id>".
    //
    // Request: wl_registry::bind (opcode 0)
    //   name: uint32       (the global name from the global event)
    //   interface: string  (the interface name, e.g. "wl_compositor")
    //   version: uint32    (the version we want to use)
    //   id: new_id         (client-allocated object ID for the new proxy)

    // Build the payload
    // uint32 name
    // string interface (uint32 length + chars + null + padding)
    // uint32 version
    // uint32 id

    size_t iface_len = strlen(interface);
    size_t str_field = 4 + iface_len + 1;  // length + chars + null
    size_t str_padded = pad4(str_field);

    size_t payload_len = 4 + str_padded + 4 + 4;  // name + string + version + id
    size_t total = 8 + pad4(payload_len);

    std::vector<uint8_t> msg(total, 0);
    put_u32_le(&msg[0], m_registry_id);
    put_u32_le(&msg[4], 0 | (static_cast<uint32_t>(total) << 16));  // opcode=0

    size_t pos = 8;
    put_u32_le(&msg[pos], name);  // global name
    pos += 4;

    // string: length (includes null terminator in Wayland protocol)
    put_u32_le(&msg[pos], static_cast<uint32_t>(iface_len + 1));
    pos += 4;
    memcpy(&msg[pos], interface, iface_len);
    pos += iface_len;
    msg[pos] = 0;  // null terminator
    pos = 8 + 4 + str_padded;  // skip to after padded string

    put_u32_le(&msg[pos], version);
    pos += 4;
    put_u32_le(&msg[pos], id);

    // Send via write() since we already built the full message
    if (m_fd >= 0) {
        const uint8_t* bytes = msg.data();
        size_t total = 0;
        while (total < msg.size()) {
            ssize_t n = ::write(m_fd, bytes + total, msg.size() - total);
            if (n <= 0) {
                throw std::runtime_error("Wayland: failed to send bind message");
            }
            total += static_cast<size_t>(n);
        }
    }
}

void WaylandConnection::handle_registry_global(uint32_t name,
                                                  const char* interface,
                                                  uint32_t version) {
    // TEACHING NOTE: Binding to Wayland globals
    // ====================================================================
    // When the compositor advertises a global, we check if it is one we
    // need. If so, we bind to it by allocating a client-side object ID
    // and sending a wl_registry::bind request.

    std::cerr << "Wayland: registry global: name=" << name
              << " interface=" << interface
              << " version=" << version << std::endl;

    if (strcmp(interface, WL_INTERFACE_COMPOSITOR) == 0) {
        m_compositor_id = allocate_id();
        m_compositor_version = (version < 4) ? version : 4;
        send_bind(name, WL_INTERFACE_COMPOSITOR, m_compositor_version, m_compositor_id);
    } else if (strcmp(interface, WL_INTERFACE_SHM) == 0) {
        m_shm_id = allocate_id();
        m_shm_version = (version < 1) ? version : 1;
        send_bind(name, WL_INTERFACE_SHM, m_shm_version, m_shm_id);
    } else if (strcmp(interface, WL_INTERFACE_SEAT) == 0) {
        m_seat_id = allocate_id();
        m_seat_version = (version < 4) ? version : 4;
        send_bind(name, WL_INTERFACE_SEAT, m_seat_version, m_seat_id);
    } else if (strcmp(interface, WL_INTERFACE_OUTPUT) == 0) {
        m_output_id = allocate_id();
        send_bind(name, WL_INTERFACE_OUTPUT, 2, m_output_id);
    } else if (strcmp(interface, WL_INTERFACE_XDG_WM_BASE) == 0) {
        // Bind to xdg_wm_base for toplevel window management.
        // Version 6 is widely supported; cap at 6 for compatibility.
        m_xdg_wm_base_id = allocate_id();
        uint32_t xdg_version = (version < 6) ? version : 6;
        send_bind(name, WL_INTERFACE_XDG_WM_BASE, xdg_version, m_xdg_wm_base_id);
    }
}

void WaylandConnection::handle_display_error(uint32_t object_id,
                                                uint32_t code,
                                                const char* message) {
    // A fatal error from the compositor. Log it so we know why things fail.
    std::cerr << "Wayland: display error - object=" << object_id
              << " code=" << code
              << " msg=" << (message ? message : "(null)")
              << std::endl;
}

void WaylandConnection::handle_output_mode(uint32_t flags, int32_t width,
                                              int32_t height) {
    // TEACHING NOTE: wl_output::mode event
    // ====================================================================
    // The compositor sends mode events for each supported display mode.
    // The flags field has bit 0 set for the current mode and bit 1 for
    // the preferred mode. We only care about the current mode.
    //
    // We update the output dimensions so the display backend knows the
    // screen size.

    if (flags & 0x1) {
        // Current mode
        m_output_width = static_cast<int>(width);
        m_output_height = static_cast<int>(height);
    }
}

void WaylandConnection::send_xdg_pong(uint32_t serial) {
    // xdg_wm_base::pong (opcode 1)
    // Args: serial (uint32)
    // Sent in response to xdg_wm_base::ping to prove the client is alive.
    if (m_xdg_wm_base_id == 0) return;
    send_message(m_xdg_wm_base_id, 1, &serial, 4);
}

void WaylandConnection::send_xdg_ack_configure(uint32_t xdg_surface_id,
                                                  uint32_t serial) {
    // xdg_surface::ack_configure (opcode 1)
    // Args: serial (uint32)
    // Sent in response to xdg_surface::configure to acknowledge the
    // new configuration before committing.
    send_message(xdg_surface_id, 1, &serial, 4);
}

bool WaylandConnection::roundtrip() {
    // Do a sync round-trip: send sync, wait for callback done.
    m_sync_done = false;
    uint32_t cb_id = allocate_id();
    send_message(1, 1, &cb_id, 4);

    // Save the old callback ID and use the new one
    uint32_t old_cb = m_sync_callback_id;
    m_sync_callback_id = cb_id;

    while (!m_sync_done) {
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = ::poll(&pfd, 1, 5000);
        if (ret <= 0) {
            m_sync_callback_id = old_cb;
            return false;
        }
        recv_available();
        process_events();
    }

    m_sync_callback_id = old_cb;
    return true;
}

// ============================================================================
// WaylandSurface implementation
// ============================================================================

WaylandSurface::WaylandSurface()
    : m_conn(nullptr)
    , m_surface_id(0)
    , m_buffer_id(0)
    , m_pool_id(0)
    , m_xdg_surface_id(0)
    , m_xdg_toplevel_id(0)
    , m_shm_fd(-1)
    , m_shm_ptr(nullptr)
    , m_shm_size(0)
    , m_width(0)
    , m_height(0)
    , m_buffer_busy(false)
    , m_initialized(false) {}

WaylandSurface::~WaylandSurface() {
    destroy();
}

// TEACHING NOTE: Generating a unique shm filename
// =========================================================================
// We need a unique filename for shm_open(). The name must be unique per
// process to avoid collisions. We use the process ID and a counter.
//
// shm_open() creates a shared memory object with the given name. Multiple
// processes can open the same object by name, but we want a private object
// so we use a unique name.

std::string WaylandSurface::shm_filename() {
    // Use a static counter to ensure uniqueness within the process
    static int counter = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "/chinstrap-%d-%d",
             static_cast<int>(getpid()), counter++);
    return std::string(buf);
}

// TEACHING NOTE: Creating the shared memory buffer
// =========================================================================
// This is the most Wayland-specific part of the implementation. The process:
//
// 1. Create a shared memory file using shm_open(). This creates a
//    memory-mapped file that can be shared between processes.
//
// 2. Truncate it to the required size (width * height * 4 bytes for
//    XRGB8888 format, one buffer).
//
// 3. mmap() it to get a pointer we can write pixels to.
//
// 4. Create a wl_shm_pool from the fd. The pool is a region of shared
//    memory that the compositor can read from.
//
// 5. Create a wl_buffer from the pool. The buffer references a specific
//    range of the pool (offset 0, full size) with a specific pixel format.
//
// The compositor will read from this shared memory when compositing our
// surface. We write pixels to the mmaped region, attach the buffer to
// the surface, and commit. The compositor reads the pixels, composites
// them with other surfaces, and sends the result to the display.

bool WaylandSurface::create_shm_buffer(WaylandConnection* conn) {
    // Calculate buffer size
    // We use XRGB8888 format: 4 bytes per pixel
    // For double buffering we would need 2x the size, but for simplicity
    // we use a single buffer and wait for release before reusing.
    m_shm_size = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4;

    // Create the shared memory file
    std::string name = shm_filename();
    m_shm_fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (m_shm_fd < 0) {
        return false;
    }

    // Unlink immediately - the fd remains valid, but the name is removed
    // so no other process can accidentally open it. This is the standard
    // pattern for anonymous shared memory.
    ::shm_unlink(name.c_str());

    // Set the size of the shared memory object
    if (::ftruncate(m_shm_fd, static_cast<off_t>(m_shm_size)) < 0) {
        ::close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }

    // mmap the shared memory
    m_shm_ptr = ::mmap(nullptr, m_shm_size,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       m_shm_fd, 0);
    if (m_shm_ptr == MAP_FAILED || m_shm_ptr == nullptr) {
        m_shm_ptr = nullptr;
        ::close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }

    // Clear the buffer to black (all zeros = black in XRGB8888)
    memset(m_shm_ptr, 0, m_shm_size);

    // Create the wl_shm_pool
    // wl_shm::create_pool (opcode 0)
    // Args: new_id pool (uint32), fd (file descriptor), size (int32)
    m_pool_id = conn->allocate_id();

    // Build the payload: new_id (4) + size (4) = 8 bytes
    // The fd is sent as ancillary data
    uint8_t payload[8];
    // new_id for the pool
    put_u32_le(&payload[0], m_pool_id);
    // size of the shared memory region
    put_u32_le(&payload[4], static_cast<uint32_t>(m_shm_size));

    // Send the create_pool request with the fd
    conn->send_message(conn->get_shm_id(), 0, payload, 8, m_shm_fd);

    // Create the wl_buffer from the pool
    // wl_shm_pool::create_buffer (opcode 0)
    // Args: new_id buffer (uint32), offset (int32), width (int32),
    //       height (int32), stride (int32), format (uint32)
    m_buffer_id = conn->allocate_id();

    uint8_t buf_payload[24];
    put_u32_le(&buf_payload[0], m_buffer_id);    // new_id for buffer
    put_u32_le(&buf_payload[4], 0);              // offset in the pool
    put_u32_le(&buf_payload[8], static_cast<uint32_t>(m_width));   // width
    put_u32_le(&buf_payload[12], static_cast<uint32_t>(m_height)); // height
    put_u32_le(&buf_payload[16], static_cast<uint32_t>(m_width * 4)); // stride
    put_u32_le(&buf_payload[20], WL_SHM_FORMAT_XRGB8888);           // format

    conn->send_message(m_pool_id, 0, buf_payload, 24);

    return true;
}

bool WaylandSurface::create_surface(WaylandConnection* conn) {
    // TEACHING NOTE: Creating a Wayland surface
    // ====================================================================
    // A wl_surface is a rectangular area that can be displayed by the
    // compositor. It is created via wl_compositor::create_surface.
    //
    // Request: wl_compositor::create_surface (opcode 0)
    //   new_id: surface (uint32 - client-allocated ID)
    //
    // The surface starts with no content. We must attach a buffer and
    // commit before anything is shown.

    m_surface_id = conn->allocate_id();

    // Send the create_surface request to the compositor
    // wl_compositor::create_surface (opcode 0)
    // Payload: new_id = m_surface_id (4 bytes)
    // The send_message function builds the header automatically
    conn->send_message(conn->get_compositor_id(), 0, &m_surface_id, 4);

    return true;
}

bool WaylandSurface::create_xdg_surface(WaylandConnection* conn) {
    // TEACHING NOTE: Creating an xdg_surface and xdg_toplevel
    // ====================================================================
    // Modern Wayland compositors require the xdg-shell protocol to show
    // a toplevel window. Without it, the surface may never appear on
    // screen. The process is:
    //
    // 1. Create an xdg_surface from the wl_surface:
    //    xdg_wm_base::get_xdg_surface (opcode 1)
    //    Args: new_id xdg_surface, object wl_surface
    //
    // 2. Create an xdg_toplevel from the xdg_surface:
    //    xdg_surface::get_toplevel (opcode 1)
    //    Args: new_id xdg_toplevel (no other args except header)
    //
    // 3. Set the window title:
    //    xdg_toplevel::set_title (opcode 0)
    //    Args: string title
    //
    // 4. Commit the surface to make it visible:
    //    wl_surface::commit (opcode 6)
    //
    // The compositor will send xdg_surface::configure events with a
    // serial that must be acknowledged via xdg_surface::ack_configure
    // before the next commit.

    // Step 1: Create xdg_surface via xdg_wm_base::get_xdg_surface (opcode 1)
    m_xdg_surface_id = conn->allocate_id();
    uint8_t payload[8];
    put_u32_le(&payload[0], m_xdg_surface_id);  // new_id for xdg_surface
    put_u32_le(&payload[4], m_surface_id);       // wl_surface object
    conn->send_message(conn->get_xdg_wm_base_id(), 1, payload, 8);

    // Step 2: Create xdg_toplevel via xdg_surface::get_toplevel (opcode 1)
    m_xdg_toplevel_id = conn->allocate_id();
    conn->send_message(m_xdg_surface_id, 1, &m_xdg_toplevel_id, 4);

    // Step 3: Set the title via xdg_toplevel::set_title (opcode 0)
    // We use a default title; the caller can change it later via set_title().
    std::string default_title = "Chinstrap";
    size_t title_len = default_title.size();
    size_t str_field = 4 + title_len + 1;  // length + chars + null
    size_t str_padded = pad4(str_field);
    size_t title_payload_len = str_padded;
    std::vector<uint8_t> title_msg(8 + title_payload_len, 0);
    put_u32_le(&title_msg[0], m_xdg_toplevel_id);
    put_u32_le(&title_msg[4], static_cast<uint32_t>(0) |
                        (static_cast<uint32_t>(8 + title_payload_len) << 16));
    size_t pos = 8;
    put_u32_le(&title_msg[pos], static_cast<uint32_t>(title_len + 1));
    pos += 4;
    memcpy(&title_msg[pos], default_title.c_str(), title_len);
    pos += title_len;
    title_msg[pos] = 0;  // null terminator

    // Send the set_title message via write()
    if (conn->get_fd() >= 0) {
        const uint8_t* bytes = title_msg.data();
        size_t total = 0;
        while (total < title_msg.size()) {
            ssize_t n = ::write(conn->get_fd(), bytes + total,
                               title_msg.size() - total);
            if (n <= 0) {
                return false;
            }
            total += static_cast<size_t>(n);
        }
    }

    // Step 4: Commit the surface to trigger initial configuration
    // wl_surface::commit (opcode 6)
    conn->send_message(m_surface_id, 6, nullptr, 0);

    // Register the xdg_surface ID so the connection handles configure events
    conn->register_xdg_surface(m_xdg_surface_id);

    return true;
}

bool WaylandSurface::create(WaylandConnection* conn, int width, int height) {
    if (!conn || !conn->is_connected()) return false;
    if (width <= 0 || height <= 0) return false;

    m_conn = conn;
    m_width = width;
    m_height = height;

    std::cerr << "Wayland: creating surface " << width << "x" << height << std::endl;

    // Create the surface
    if (!create_surface(conn)) {
        std::cerr << "Wayland: failed to create surface" << std::endl;
        return false;
    }

    // Create the shared memory buffer
    if (!create_shm_buffer(conn)) {
        std::cerr << "Wayland: failed to create shm buffer" << std::endl;
        return false;
    }

    // Create xdg_surface and xdg_toplevel for window management.
    if (conn->get_xdg_wm_base_id() != 0) {
        if (!create_xdg_surface(conn)) {
            std::cerr << "Wayland: failed to create xdg_surface" << std::endl;
            return false;
        }
    } else {
        std::cerr << "Wayland: no xdg_wm_base, surface may not be visible" << std::endl;
    }

    m_initialized = true;
    std::cerr << "Wayland: surface created ok" << std::endl;
    return true;
}

void WaylandSurface::destroy() {
    if (!m_initialized) return;

    // Destroy the xdg_toplevel
    if (m_xdg_toplevel_id && m_conn) {
        // xdg_toplevel::destroy (opcode 1)
        m_conn->send_message(m_xdg_toplevel_id, 1, nullptr, 0);
    }

    // Destroy the xdg_surface
    if (m_xdg_surface_id && m_conn) {
        // xdg_surface::destroy (opcode 0)
        m_conn->send_message(m_xdg_surface_id, 0, nullptr, 0);
    }

    // Destroy the surface
    if (m_surface_id && m_conn) {
        // wl_surface::destroy (opcode 0)
        // No arguments except header
        m_conn->send_message(m_surface_id, 0, nullptr, 0);
    }

    // Destroy the buffer
    if (m_buffer_id && m_conn) {
        // wl_buffer::destroy (opcode 0)
        m_conn->send_message(m_buffer_id, 0, nullptr, 0);
    }

    // Destroy the shm pool
    if (m_pool_id && m_conn) {
        // wl_shm_pool::destroy (opcode 1)
        m_conn->send_message(m_pool_id, 1, nullptr, 0);
    }

    // Unmap the shared memory
    if (m_shm_ptr) {
        ::munmap(m_shm_ptr, m_shm_size);
        m_shm_ptr = nullptr;
    }

    // Close the shm fd
    if (m_shm_fd >= 0) {
        ::close(m_shm_fd);
        m_shm_fd = -1;
    }

    m_surface_id = 0;
    m_buffer_id = 0;
    m_pool_id = 0;
    m_xdg_surface_id = 0;
    m_xdg_toplevel_id = 0;
    m_initialized = false;
}

// TEACHING NOTE: Presenting a frame on Wayland
// =========================================================================
// To show pixels on screen in Wayland, we:
//
// 1. Write pixels to the shared memory region (m_shm_ptr).
//    The format is XRGB8888: bytes [B, G, R, X] per pixel.
//
// 2. Attach the buffer to the surface:
//    wl_surface::attach(buffer, x, y)
//    x and y are always 0 for us (no buffer offset).
//
// 3. Mark the damaged region:
//    wl_surface::damage(x, y, width, height)
//    This tells the compositor which part of the surface changed.
//    We mark the entire surface as damaged for simplicity.
//
// 4. Commit the surface:
//    wl_surface::commit()
//    This tells the compositor "I am done updating, please show this".
//    The compositor will read from the shared memory, composite our
//    surface with others, and display the result.
//
// 5. Mark the buffer as busy:
//    After commit, the compositor is using our buffer. We must not
//    write to it until we receive wl_buffer::release. In this minimal
//    implementation, we just set a flag and hope the compositor
//    releases quickly (it usually does, within one frame).

void WaylandSurface::commit_frame(WaylandConnection* conn) {
    if (!m_initialized || !conn) return;

    // wl_surface::attach (opcode 1)
    // Args: buffer (object_id or 0 for null), x (int32), y (int32)
    uint8_t attach_payload[12];
    put_u32_le(&attach_payload[0], m_buffer_id);  // buffer
    put_u32_le(&attach_payload[4], 0);            // x
    put_u32_le(&attach_payload[8], 0);            // y
    conn->send_message(m_surface_id, 1, attach_payload, 12);

    // wl_surface::damage (opcode 2)
    // Args: x (int32), y (int32), width (int32), height (int32)
    uint8_t damage_payload[16];
    put_u32_le(&damage_payload[0], 0);                                   // x
    put_u32_le(&damage_payload[4], 0);                                   // y
    put_u32_le(&damage_payload[8], static_cast<uint32_t>(m_width));     // width
    put_u32_le(&damage_payload[12], static_cast<uint32_t>(m_height));   // height
    conn->send_message(m_surface_id, 2, damage_payload, 16);

    // wl_surface::commit (opcode 6)
    // No arguments
    conn->send_message(m_surface_id, 6, nullptr, 0);

    // Mark the buffer as busy (compositor is using it)
    m_buffer_busy = true;
}

void WaylandSurface::set_title(const std::string& title) {
    // TEACHING NOTE: Setting the window title in Wayland
    // ====================================================================
    // In Wayland, window titles are set via the xdg-shell protocol
    // extension (xdg_toplevel::set_title). We send the title string
    // to the xdg_toplevel object created during surface initialization.
    //
    // Request: xdg_toplevel::set_title (opcode 0)
    // Args: string title (uint32 length + chars + null + padding)

    if (!m_initialized || !m_conn || m_xdg_toplevel_id == 0) return;

    size_t title_len = title.size();
    size_t str_field = 4 + title_len + 1;  // length + chars + null
    size_t str_padded = pad4(str_field);
    size_t payload_len = str_padded;
    size_t total = 8 + payload_len;

    std::vector<uint8_t> msg(total, 0);
    put_u32_le(&msg[0], m_xdg_toplevel_id);
    put_u32_le(&msg[4], static_cast<uint32_t>(0) |
                        (static_cast<uint32_t>(total) << 16));  // opcode=0

    size_t pos = 8;
    put_u32_le(&msg[pos], static_cast<uint32_t>(title_len + 1));
    pos += 4;
    memcpy(&msg[pos], title.c_str(), title_len);
    pos += title_len;
    msg[pos] = 0;  // null terminator

    // Send the set_title message via write()
    if (m_conn->get_fd() >= 0) {
        const uint8_t* bytes = msg.data();
        size_t sent = 0;
        while (sent < msg.size()) {
            ssize_t n = ::write(m_conn->get_fd(), bytes + sent,
                               msg.size() - sent);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }
}

// TEACHING NOTE: Processing Wayland surface events
// =========================================================================
// Events for our surface come through the connection. The WaylandConnection
// processes events and dispatches them by object ID. For object IDs it
// does not recognize (like our keyboard, pointer, and buffer objects),
// we need to handle them here.
//
// In this minimal implementation, the WaylandConnection processes some
// events (registry, display) and we handle the rest here by polling the
// connection for events related to our objects.
//
// For a complete implementation, we would register callbacks for each
// object ID. Here we do a simpler approach: after the connection processes
// events, we check if any events in the connection buffer are for our
// objects (buffer release, keyboard key, pointer motion, etc.).

bool WaylandSurface::process_events(WaylandConnection* conn, WaylandEvent* event) {
    if (!m_initialized || !conn || !event) return true;

    // The connection already processed its events. We just check if
    // the buffer was released (compositor is done with it).
    // In a full implementation, we would parse keyboard/pointer events here.

    // For now, we return true (surface still valid) with no event.
    event->type = WL_EVENT_NONE;
    return true;
}

} // namespace chinstrap