// x11.cpp - Minimal X11 protocol implementation over raw socket
//
// TEACHING NOTE: This file implements just enough of the X11 protocol to
// create a window, draw pixels to it, and receive keyboard/mouse events.
// It does NOT use Xlib, XCB, or any other library. It talks directly to
// the X server over a socket using the raw binary protocol.
//
// The X11 protocol is specified in the X Window System Protocol document
// (X Consortium Standard). This implementation covers a tiny fraction
// of the full protocol - just what a browser display needs.

#include "x11.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <stdexcept>

namespace chinstrap {

// ============================================================================
// X11 protocol constants and structures
// ============================================================================

// TEACHING NOTE: X11 request opcodes
// These are the numeric opcodes for the X11 requests we need.
// Each request starts with its opcode byte, followed by request-specific data.

enum X11Opcode {
    X_CreateWindow        = 1,
    X_ChangeWindowAttributes = 2,
    X_GetWindowAttributes = 3,
    X_DestroyWindow       = 4,
    X_DestroySubwindows   = 5,
    X_ChangeSaveSet       = 6,
    X_ReparentWindow      = 7,
    X_MapWindow           = 8,
    X_MapSubwindows       = 9,
    X_UnmapWindow         = 10,
    X_UnmapSubwindows     = 11,
    X_ConfigureWindow     = 12,
    X_CirculateWindow     = 13,
    X_GetGeometry         = 14,
    X_QueryTree           = 15,
    X_InternAtom          = 16,
    X_GetAtomName         = 17,
    X_ChangeProperty      = 18,
    X_DeleteProperty      = 19,
    X_GetProperty         = 20,
    X_ListProperties      = 21,
    X_SetSelectionOwner   = 22,
    X_ConvertSelection    = 24,
    X_SendEvent           = 25,
    X_GrabPointer         = 26,
    X_UngrabPointer       = 27,
    X_GrabKey             = 33,
    X_UngrabKey           = 34,
    X_AllowEvents         = 35,
    X_GrabServer          = 36,
    X_UngrabServer        = 37,
    X_QueryPointer        = 38,
    X_GetMotionEvents     = 39,
    X_TranslateCoords     = 40,
    X_WarpPointer         = 41,
    X_SetInputFocus       = 42,
    X_GetInputFocus       = 43,
    X_QueryKeymap         = 44,
    X_OpenFont            = 45,
    X_CloseFont           = 46,
    X_QueryFont           = 47,
    X_CreatePixmap        = 53,
    X_FreePixmap          = 54,
    X_CreateGC            = 55,
    X_ChangeGC            = 56,
    X_CopyGC              = 57,
    X_FreeGC              = 60,
    X_ClearArea           = 61,
    X_CopyArea            = 62,
    X_CopyPlane           = 63,
    X_PolyPoint           = 64,
    X_PolyLine            = 65,
    X_PolySegment         = 66,
    X_PolyRectangle       = 67,
    X_PolyArc             = 68,
    X_FillPoly            = 69,
    X_PolyFillRectangle   = 70,
    X_PolyFillArc         = 71,
    X_PutImage            = 72,
    X_GetImage            = 73,
    X_PolyText8           = 74,
    X_PolyText16          = 75,
    X_ImageText8          = 76,
    X_ImageText16         = 77,
    X_CreateColormap      = 78,
    X_FreeColormap        = 79,
    X_CopyColormapAndFree = 80,
    X_InstallColormap     = 81,
    X_QueryColors        = 92,
    X_LookupColor         = 94,
    X_CreateCursor        = 93,
    X_CreateGlyphCursor   = 94,
    X_FreeCursor          = 95,
    X_QueryBestSize       = 97,
    X_Bell                = 104,
    X_ChangePointerControl= 105,
    X_GetScreenSaver       = 108,
    X_SetScreenSaver      = 107,
};

// X11 event type codes (as sent by the server)
enum X11EventTypeCode {
    X11_KeyPress_code        = 2,
    X11_KeyRelease_code      = 3,
    X11_ButtonPress_code     = 4,
    X11_ButtonRelease_code   = 5,
    X11_MotionNotify_code    = 6,
    X11_EnterNotify_code     = 7,
    X11_LeaveNotify_code     = 8,
    X11_FocusIn_code         = 9,
    X11_FocusOut_code        = 10,
    X11_Expose_code          = 12,
    X11_ClientMessage_code   = 33,
};

// X11 image format (for PutImage)
enum X11ImageFormat {
    X11_XYBitmap  = 0,
    X11_XYPixmap  = 1,
    X11_ZPixmap   = 2,  // we use this - it is the most convenient format
};

// X11 property modes
enum X11PropMode {
    X11_PropModeReplace = 0,
    X11_PropModePrepend = 1,
    X11_PropModeAppend  = 2,
};

// Window attribute masks (for CreateWindow)
enum X11WindowAttrMask {
    X11_CW_BackPixmap     = 0x00000001,
    X11_CW_BackPixel      = 0x00000002,
    X11_CW_BorderPixmap   = 0x00000004,
    X11_CW_BorderPixel    = 0x00000008,
    X11_CW_BitGravity     = 0x00000010,
    X11_CW_WinGravity     = 0x00000020,
    X11_CW_BackingStore   = 0x00000040,
    X11_CW_BackingPlanes  = 0x00000080,
    X11_CW_BackingPixel   = 0x00000100,
    X11_CW_OverrideRedirect = 0x00000200,
    X11_CW_SaveUnder      = 0x00000400,
    X11_CW_EventMask      = 0x00000800,
    X11_CW_DontPropagate  = 0x00001000,
    X11_CW_Colormap       = 0x00002000,
    X11_CW_Cursor         = 0x00004000,
};

// Event masks (for selecting which events the window receives)
enum X11EventMask {
    X11_NoEventMask        = 0,
    X11_KeyPressMask       = 0x00000001,
    X11_KeyReleaseMask     = 0x00000002,
    X11_ButtonPressMask    = 0x00000004,
    X11_ButtonReleaseMask  = 0x00000008,
    X11_EnterWindowMask    = 0x00000010,
    X11_LeaveWindowMask    = 0x00000020,
    X11_PointerMotionMask  = 0x00000040,
    X11_ExposureMask       = 0x00008000,
    X11_StructureNotifyMask = 0x00020000,
    X11_SubstructureNotifyMask = 0x00080000,
};

// Helper: pack a 16-bit value in little-endian
// TEACHING NOTE: We use little-endian byte order for all X11 communication.
// All modern systems (x86_64, aarch64) are little-endian, and the X11 protocol
// allows the client to choose its byte order. We declare 0x6C (little-endian)
// in the setup request. The server may use a different byte order in its
// replies, but in practice all modern X servers on x86_64/aarch64 are also
// little-endian, so we parse replies in little-endian too.
static inline void put_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

// Helper: pack a 32-bit value in little-endian
static inline void put_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

// Helper: read a 16-bit little-endian value
static inline uint16_t get_u16(const uint8_t* buf) {
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

// Helper: read a 32-bit little-endian value
static inline uint32_t get_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

// Helper: pad a length to 4-byte boundary
static inline size_t pad4(size_t len) {
    return (len + 3) & ~3u;
}

// ============================================================================
// X11Connection implementation
// ============================================================================

X11Connection::X11Connection()
    : m_fd(-1)
    , m_root_window(0)
    , m_root_visual(0)
    , m_depth(0)
    , m_resource_id_base(0)
    , m_resource_id_mask(0)
    , m_resource_id_next(1) {}

X11Connection::~X11Connection() {
    disconnect();
}

// TEACHING NOTE: Connecting to the X server
// =========================================================================
// The X server typically listens on:
//   - Unix domain socket: /tmp/.X11-unix/X<display_number>
//   - TCP port: 6000 + <display_number>
//
// The DISPLAY environment variable tells clients where to connect:
//   ":0"      -> display 0 on local host (Unix socket)
//   "host:0"  -> display 0 on host (TCP to host:6000)
//   ":0.1"    -> screen 1 of display 0 on local host
//
// We parse DISPLAY, try the Unix socket first (faster, local), and
// fall back to TCP if needed.

bool X11Connection::connect() {
    // Parse DISPLAY environment variable
    std::string display = std::getenv("DISPLAY") ? std::getenv("DISPLAY") : ":0";

    // Extract display number and optional screen
    int display_num = 0;
    std::string host;

    size_t colon = display.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    host = display.substr(0, colon);
    std::string rest = display.substr(colon + 1);

    // Parse display number (and optional screen after ".")
    size_t dot = rest.find('.');
    if (dot != std::string::npos) {
        display_num = atoi(rest.substr(0, dot).c_str());
    } else {
        display_num = atoi(rest.c_str());
    }

    // Try Unix domain socket first (if host is empty or "unix")
    if (host.empty() || host == "unix") {
        std::string path = "/tmp/.X11-unix/X" + std::to_string(display_num);
        if (open_unix_socket(path)) {
            return do_handshake();
        }
    }

    // Fall back to TCP
    if (host.empty()) host = "localhost";
    int port = 6000 + display_num;
    if (open_tcp_socket(host, port)) {
        return do_handshake();
    }

    return false;
}

bool X11Connection::open_unix_socket(const std::string& path) {
    m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_fd);
        m_fd = -1;
        return false;
    }

    return true;
}

bool X11Connection::open_tcp_socket(const std::string& host, int port) {
    m_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // For simplicity, only handle localhost and 127.0.0.1
    if (host == "localhost" || host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            close(m_fd);
            m_fd = -1;
            return false;
        }
    }

    if (::connect(m_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_fd);
        m_fd = -1;
        return false;
    }

    return true;
}

// TEACHING NOTE: X11 connection handshake
// =========================================================================
// The X11 connection setup is the most complex part of the protocol:
//
// Client sends:
//   - 1 byte: byte order (0x6C = little-endian, 0x42 = big-endian)
//   - 1 byte: unused (0)
//   - 2 bytes: protocol major version (11)
//   - 2 bytes: protocol minor version (0)
//   - 2 bytes: length of authorization data (in 4-byte units)
//   - 2 bytes: unused (0)
//   - authorization data (if any)
//
// Server responds with:
//   - 1 byte: success (1 = success, 0 = failure, 2 = authenticate)
//   - For success:
//     - 1 byte: unused
//     - 2 bytes: protocol major version
//     - 2 bytes: protocol minor version
//     - 2 bytes: length of additional data (in 4-byte units)
//     - Then: vendor string, pixmap formats, screens, visuals, etc.
//
// We need to parse enough of the server response to get:
//   - root window ID (for creating our window as a child of it)
//   - root visual ID (for matching our visual to the display)
//   - resource ID base/mask (for generating unique XIDs)
//   - depth (color depth of the root screen)

bool X11Connection::do_handshake() {
    // TEACHING NOTE: X11 authentication
    // ====================================================================
    // The X server requires authentication. The most common method is
    // MIT-MAGIC-COOKIE-1, which uses a 16-byte cookie stored in the
    // .Xauthority file. We read the file, find the cookie for our display,
    // and send it in the setup request.
    //
    // The .Xauthority file format (all big-endian):
    //   CARD16 family
    //   STRING address (CARD16 len + data)
    //   For FamilyLocal (256): STRING display (CARD16 len + data)
    //   For other families: CARD16 display_number
    //   STRING name (CARD16 len + data)
    //   STRING data (CARD16 len + data)

    // Read .Xauthority
    std::string xauth_path;
    const char* xauth_env = std::getenv("XAUTHORITY");
    if (xauth_env) {
        xauth_path = xauth_env;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            xauth_path = std::string(home) + "/.Xauthority";
        }
    }

    std::vector<uint8_t> auth_name;
    std::vector<uint8_t> auth_data;
    bool have_auth = false;

    if (!xauth_path.empty()) {
        int fd = ::open(xauth_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            std::vector<uint8_t> file_data;
            uint8_t fbuf[4096];
            ssize_t n;
            while ((n = ::read(fd, fbuf, sizeof(fbuf))) > 0) {
                file_data.insert(file_data.end(), fbuf, fbuf + n);
            }
            ::close(fd);

            std::string display_env = std::getenv("DISPLAY") ? std::getenv("DISPLAY") : ":0";
            int our_display = 0;
            size_t colon = display_env.find(':');
            if (colon != std::string::npos) {
                std::string rest = display_env.substr(colon + 1);
                size_t dot = rest.find('.');
                if (dot != std::string::npos) {
                    our_display = atoi(rest.substr(0, dot).c_str());
                } else {
                    our_display = atoi(rest.c_str());
                }
            }

            size_t pos = 0;
            while (pos + 2 <= file_data.size() && !have_auth) {
                uint16_t family = static_cast<uint16_t>((file_data[pos] << 8) | file_data[pos + 1]);
                pos += 2;

                if (pos + 2 > file_data.size()) break;
                uint16_t addr_len = static_cast<uint16_t>((file_data[pos] << 8) | file_data[pos + 1]);
                pos += 2;
                if (pos + addr_len > file_data.size()) break;
                pos += addr_len;

                int rec_display = -1;
                if (family == 256) {
                    // FamilyLocal: display is stored as STRING
                    if (pos + 2 > file_data.size()) break;
                    uint16_t disp_len = static_cast<uint16_t>((file_data[pos] << 8) | file_data[pos + 1]);
                    pos += 2;
                    if (pos + disp_len > file_data.size()) break;
                    std::string disp_str(file_data.begin() + pos, file_data.begin() + pos + disp_len);
                    pos += disp_len;
                    rec_display = atoi(disp_str.c_str());
                } else {
                    if (pos + 2 > file_data.size()) break;
                    rec_display = (file_data[pos] << 8) | file_data[pos + 1];
                    pos += 2;
                }

                if (pos + 2 > file_data.size()) break;
                uint16_t name_len = static_cast<uint16_t>((file_data[pos] << 8) | file_data[pos + 1]);
                pos += 2;
                if (pos + name_len > file_data.size()) break;
                std::vector<uint8_t> rec_name(file_data.begin() + pos, file_data.begin() + pos + name_len);
                pos += name_len;

                if (pos + 2 > file_data.size()) break;
                uint16_t data_len = static_cast<uint16_t>((file_data[pos] << 8) | file_data[pos + 1]);
                pos += 2;
                if (pos + data_len > file_data.size()) break;
                std::vector<uint8_t> rec_data(file_data.begin() + pos, file_data.begin() + pos + data_len);
                pos += data_len;

                if (family == 0 || rec_display == our_display) {
                    auth_name = rec_name;
                    auth_data = rec_data;
                    have_auth = true;
                }
            }
        }
    }

    // Try with auth first, then without auth (XWayland often needs no auth)
    for (int auth_attempt = 0; auth_attempt < 2; auth_attempt++) {
        bool use_auth = (auth_attempt == 0) && have_auth;

        // Reconnect socket for retry (first attempt uses existing socket)
        if (auth_attempt > 0) {
            if (m_fd >= 0) {
                ::close(m_fd);
                m_fd = -1;
            }
            // Reopen the same socket
            std::string display_env = std::getenv("DISPLAY") ? std::getenv("DISPLAY") : ":0";
            int disp_num = 0;
            size_t colon2 = display_env.find(':');
            if (colon2 != std::string::npos) {
                std::string rest2 = display_env.substr(colon2 + 1);
                size_t dot2 = rest2.find('.');
                if (dot2 != std::string::npos) {
                    disp_num = atoi(rest2.substr(0, dot2).c_str());
                } else {
                    disp_num = atoi(rest2.c_str());
                }
            }
            std::string path = "/tmp/.X11-unix/X" + std::to_string(disp_num);
            if (!open_unix_socket(path)) {
                return false;
            }
        }

        uint16_t name_len = use_auth ? static_cast<uint16_t>(auth_name.size()) : 0;
        uint16_t data_len = use_auth ? static_cast<uint16_t>(auth_data.size()) : 0;
        size_t name_padded = (name_len + 3) & ~static_cast<size_t>(3);
        size_t data_padded = (data_len + 3) & ~static_cast<size_t>(3);
        size_t setup_size = 12 + name_padded + data_padded;
        std::vector<uint8_t> setup(setup_size, 0);

        setup[0] = 0x6C;  // 'l' = little-endian
        setup[1] = 0x00;
        // Protocol major version 11 in little-endian
        setup[2] = 11; setup[3] = 0;
        // Protocol minor version 0 in little-endian
        setup[4] = 0; setup[5] = 0;
        // Auth name length in little-endian
        setup[6] = static_cast<uint8_t>(name_len & 0xFF);
        setup[7] = static_cast<uint8_t>(name_len >> 8);
        // Auth data length in little-endian
        setup[8] = static_cast<uint8_t>(data_len & 0xFF);
        setup[9] = static_cast<uint8_t>(data_len >> 8);
        setup[10] = 0; setup[11] = 0;

        if (use_auth) {
            std::memcpy(setup.data() + 12, auth_name.data(), auth_name.size());
            std::memcpy(setup.data() + 12 + name_padded, auth_data.data(), auth_data.size());
        }

        m_output_buf.clear();
        send(setup.data(), setup_size);
        flush();

        uint8_t reply[8];
        receive(reply, 8);

        uint8_t status = reply[0];
        if (status == 0) {
            // Failed - read reason
            uint8_t reason_len = reply[1];
            uint16_t extra_units = get_u16(&reply[6]);
            size_t extra_bytes = static_cast<size_t>(extra_units) * 4;
            if (extra_bytes > 0) {
                std::vector<uint8_t> extra_data(extra_bytes, 0);
                receive(extra_data.data(), extra_bytes);
                std::string reason_str(extra_data.begin(), extra_data.begin() + reason_len);
                std::cerr << "X11 connection failed: " << reason_str << std::endl;
            }
            if (use_auth) continue;  // retry without auth
            return false;
        } else if (status == 2) {
            // Authenticate
            uint8_t auth_reply[8] = {0};
            send(auth_reply, 8);
            flush();
            receive(reply, 8);
            if (reply[0] != 1) return false;
        } else if (status != 1) {
            return false;
        }

        // Success - parse setup response
        uint16_t additional_len = get_u16(&reply[6]);
        size_t additional_bytes = static_cast<size_t>(additional_len) * 4;
        std::vector<uint8_t> data(additional_bytes);
        if (additional_bytes > 0) {
            receive(data.data(), additional_bytes);
        }

        size_t pos = 0;

        // release_number (4 bytes)
        uint32_t release = get_u32(&data[pos]);
        (void)release;
        pos += 4;

        m_resource_id_base = get_u32(&data[pos]);
        pos += 4;
        m_resource_id_mask = get_u32(&data[pos]);
        pos += 4;
        pos += 4;  // motion buffer size

        uint16_t vendor_len = get_u16(&data[pos]);
        pos += 2;
        pos += 2;  // max request length
        pos += pad4(vendor_len);  // skip vendor string

        uint8_t num_formats = data[pos];
        pos += 1;
        pos += 1;  // padding
        for (uint8_t i = 0; i < num_formats; ++i) {
            if (data[pos] == 24 || data[pos] == 32) {
                m_depth = data[pos];
            }
            pos += 8;
        }
        if (m_depth == 0) m_depth = 24;

        m_root_window = get_u32(&data[pos]);
        pos += 4;
        pos += 4;  // default colormap
        pos += 4;  // white pixel
        pos += 4;  // black pixel
        pos += 4;  // current input masks
        pos += 2;  // width
        pos += 2;  // height
        pos += 2;  // width mm
        pos += 2;  // height mm
        pos += 2;  // min installed maps
        pos += 2;  // max installed maps
        m_root_visual = get_u32(&data[pos]);
        pos += 4;
        pos += 1;  // backing stores
        pos += 1;  // save unders
        pos += 1;  // root depth
        pos += 1;  // number of allowed depths

        if (m_depth == 0) m_depth = 24;
        return true;
    }

    return false;
}

void X11Connection::disconnect() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    m_output_buf.clear();
}

void X11Connection::send(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    m_output_buf.insert(m_output_buf.end(), bytes, bytes + len);
}

void X11Connection::send_request(uint8_t opcode, const void* data, size_t len) {
    // X11 request format:
    //   byte 0: opcode
    //   byte 1: request-specific (often 0)
    //   bytes 2-3: request length in 4-byte units (including these 4 bytes)
    //   data...
    // The request length must be padded to 4 bytes

    size_t padded_len = pad4(len + 4);
    uint16_t req_len = (uint16_t)(padded_len / 4);

    uint8_t header[4] = {0};
    header[0] = opcode;
    header[1] = 0;
    put_u16(&header[2], req_len);

    send(header, 4);
    send(data, len);

    // Add padding if needed
    size_t pad = padded_len - len - 4;
    if (pad > 0) {
        std::vector<uint8_t> zeros(pad, 0);
        send(zeros.data(), pad);
    }
}

void X11Connection::receive(void* data, size_t len) {
    uint8_t* bytes = static_cast<uint8_t*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(m_fd, bytes + total, len - total);
        if (n <= 0) {
            throw std::runtime_error("X11: connection closed during receive");
        }
        total += (size_t)n;
    }
}

void X11Connection::flush() {
    if (m_output_buf.empty()) return;

    size_t total = 0;
    while (total < m_output_buf.size()) {
        ssize_t n = write(m_fd, m_output_buf.data() + total, m_output_buf.size() - total);
        if (n <= 0) {
            throw std::runtime_error("X11: connection closed during send");
        }
        total += (size_t)n;
    }
    m_output_buf.clear();
}

uint32_t X11Connection::allocate_id() {
    // TEACHING NOTE: X11 resource IDs are generated by the client using
    // the base and mask from the server. An XID is (base | (next & mask)),
    // where next is incremented by the client. This avoids the need for
    // a round-trip to the server for each new resource.
    uint32_t id = m_resource_id_base | (m_resource_id_next & m_resource_id_mask);
    m_resource_id_next++;
    return id;
}

// ============================================================================
// X11Window implementation
// ============================================================================

X11Window::X11Window()
    : m_window_id(0)
    , m_pixmap_id(0)
    , m_gc_id(0)
    , m_width(0)
    , m_height(0) {}

X11Window::~X11Window() {
    // Window should be explicitly destroyed via destroy(conn)
}

// TEACHING NOTE: Creating an X11 window
// =========================================================================
// To create a window we:
//   1. Allocate a window XID (resource ID) using the connection base/mask
//   2. Send a CreateWindow request with:
//      - depth (use same as root, typically 24)
//      - window ID (our allocated XID)
//      - parent (root window)
//      - position and size
//      - border width
//      - visual (root visual)
//      - attribute mask and values (event mask, background color)
//   3. Allocate a graphics context (GC) for drawing
//   4. Optionally create a pixmap for double buffering
//   5. Set WM_PROTOCOLS to receive close-window events
//   6. Map the window (make it visible)

bool X11Window::create(X11Connection* conn, int width, int height, const std::string& title) {
    if (!conn || !conn->is_connected()) return false;

    m_width = width;
    m_height = height;

    // Allocate window XID
    m_window_id = conn->allocate_id();

    // Create the window
    send_create_window(conn, width, height);

    // Create a graphics context
    m_gc_id = conn->allocate_id();
    send_create_gc(conn);

    // Create a pixmap for double buffering
    m_pixmap_id = conn->allocate_id();
    send_create_pixmap(conn, width, height);

    // Set window title (WM_NAME property)
    set_title(conn, title);

    // Set WM_PROTOCOLS to get close-window notification
    // We need to intern WM_PROTOCOLS and WM_DELETE_WINDOW atoms
    // For simplicity, we skip this in the minimal implementation and
    // handle the window close via other means.

    // Select input events
    // We want: Exposure, KeyPress, KeyRelease, ButtonPress, ButtonRelease, Motion
    uint32_t event_mask =
        X11_ExposureMask |
        X11_KeyPressMask |
        X11_KeyReleaseMask |
        X11_ButtonPressMask |
        X11_ButtonReleaseMask |
        X11_PointerMotionMask |
        X11_StructureNotifyMask;

    // ChangeWindowAttributes request to set event mask
    {
        uint8_t req[12] = {0};
        // opcode 2 = ChangeWindowAttributes
        // This is a special case - we need to build it manually
        uint16_t req_len = 12 / 4;  // 3 longwords
        req[0] = X_ChangeWindowAttributes;
        req[1] = 0;
        put_u16(&req[2], req_len);
        put_u32(&req[4], m_window_id);
        put_u32(&req[8], X11_CW_EventMask);
        // value: event_mask follows in the same buffer
        // But we need more space. Let us use the send_request approach.

        // Actually, let us just build the full request
        uint8_t full_req[16] = {0};
        full_req[0] = X_ChangeWindowAttributes;
        full_req[1] = 0;
        put_u16(&full_req[2], 4);  // 4 longwords = 16 bytes
        put_u32(&full_req[4], m_window_id);
        put_u32(&full_req[8], X11_CW_EventMask);
        put_u32(&full_req[12], event_mask);
        conn->send(full_req, 16);
    }

    // Map the window (make it visible)
    map(conn);

    // Flush all pending requests
    flush(conn);

    return true;
}

void X11Window::send_create_window(X11Connection* conn, int w, int h) {
    // TEACHING NOTE: CreateWindow request format
    // ====================================================================
    // opcode: 1 (CreateWindow)
    // depth: CARD8
    // request length: CARD16 (in 4-byte units)
    // wid: CARD32 (window ID)
    // parent: CARD32 (root window)
    // x: INT16
    // y: INT16
    // width: CARD16
    // height: CARD16
    // border-width: CARD16
    // class: CARD16 (0=CopyFromParent, 1=InputOutput, 2=InputOnly)
    // visual: CARD32 (0=CopyFromParent)
    // attribute-mask: CARD32
    // attribute-values: varies (CARD32 each)

    // We use InputOutput class with the root visual
    // Attributes: background pixel (black) + event mask (set later)

    uint8_t req[40] = {0};
    req[0] = X_CreateWindow;
    req[1] = 24;  // depth (same as root)
    put_u16(&req[2], 40 / 4);  // 10 longwords
    put_u32(&req[4], m_window_id);
    put_u32(&req[8], conn->get_root_window());
    // x, y: centered roughly
    int16_t x = 100;
    int16_t y = 100;
    put_u16(&req[12], static_cast<uint16_t>(x));
    put_u16(&req[14], static_cast<uint16_t>(y));
    put_u16(&req[16], (uint16_t)w);
    put_u16(&req[18], (uint16_t)h);
    put_u16(&req[20], 0);  // border width
    put_u16(&req[22], 1);  // class = InputOutput
    put_u32(&req[24], conn->get_root_visual());
    // attribute mask: background pixel + bit gravity
    put_u32(&req[28], X11_CW_BackPixel | X11_CW_BitGravity);
    // background pixel: black (0)
    put_u32(&req[32], 0x00000000);
    // bit gravity: NorthWestGravity (1) - keeps contents when window resizes
    put_u32(&req[36], 1);

    conn->send(req, 40);
}

void X11Window::send_create_gc(X11Connection* conn) {
    // TEACHING NOTE: Graphics Context (GC)
    // ====================================================================
    // A GC is a server-side object that holds drawing attributes like
    // foreground color, background color, line width, fill style, etc.
    // We create a GC for our window and use it for PutImage and other
    // drawing requests. We use default values for most attributes.

    uint8_t req[16] = {0};
    req[0] = X_CreateGC;
    req[1] = 0;
    put_u16(&req[2], 16 / 4);  // 4 longwords
    put_u32(&req[4], m_gc_id);
    put_u32(&req[8], m_window_id);  // drawable
    put_u32(&req[12], 0);  // no attributes (use defaults)
    // Note: we need at least 4 longwords even with no attributes.
    // Actually the minimum is 4 bytes header + 4 cid + 4 drawable + 4 mask = 16
    // But the request length must include the header (4 bytes).
    // 16 / 4 = 4 longwords. But the spec says minimum length includes
    // opcode(1) + unused(1) + length(2) + cid(4) + drawable(4) + mask(4) = 16
    // That is 4 longwords. OK.

    conn->send(req, 16);
}

void X11Window::send_create_pixmap(X11Connection* conn, int w, int h) {
    // CreatePixmap: allocate a server-side pixmap for double buffering
    // Format:
    //   opcode: 53
    //   depth: CARD8
    //   length: CARD16
    //   pid: CARD32 (pixmap ID)
    //   drawable: CARD32 (window - used for screen/visual matching)
    //   width: CARD16
    //   height: CARD16

    uint8_t req[16] = {0};
    req[0] = X_CreatePixmap;
    req[1] = 24;  // depth
    put_u16(&req[2], 16 / 4);
    put_u32(&req[4], m_pixmap_id);
    put_u32(&req[8], m_window_id);
    put_u16(&req[12], (uint16_t)w);
    put_u16(&req[14], (uint16_t)h);

    conn->send(req, 16);
}

void X11Window::destroy(X11Connection* conn) {
    if (m_pixmap_id) {
        uint8_t req[8] = {0};
        req[0] = X_FreePixmap;
        req[1] = 0;
        put_u16(&req[2], 8 / 4);
        put_u32(&req[4], m_pixmap_id);
        conn->send(req, 8);
        m_pixmap_id = 0;
    }
    if (m_gc_id) {
        uint8_t req[8] = {0};
        req[0] = X_FreeGC;
        req[1] = 0;
        put_u16(&req[2], 8 / 4);
        put_u32(&req[4], m_gc_id);
        conn->send(req, 8);
        m_gc_id = 0;
    }
    if (m_window_id) {
        uint8_t req[8] = {0};
        req[0] = X_DestroyWindow;
        req[1] = 0;
        put_u16(&req[2], 8 / 4);
        put_u32(&req[4], m_window_id);
        conn->send(req, 8);
        m_window_id = 0;
    }
    if (conn) conn->flush();
}

void X11Window::map(X11Connection* conn) {
    uint8_t req[8] = {0};
    req[0] = X_MapWindow;
    req[1] = 0;
    put_u16(&req[2], 8 / 4);
    put_u32(&req[4], m_window_id);
    conn->send(req, 8);
}

void X11Window::unmap(X11Connection* conn) {
    uint8_t req[8] = {0};
    req[0] = X_UnmapWindow;
    req[1] = 0;
    put_u16(&req[2], 8 / 4);
    put_u32(&req[4], m_window_id);
    conn->send(req, 8);
}

void X11Window::set_title(X11Connection* conn, const std::string& title) {
    // TEACHING NOTE: Setting the window title
    // ====================================================================
    // The window title is stored as the WM_NAME property of the window.
    // Properties are set with the ChangeProperty request (opcode 18).
    // We also set _NET_WM_NAME (a newer UTF-8 version) for modern
    // window managers, but that requires interning an atom first.
    //
    // For simplicity, we just set WM_NAME (which is Latin-1 encoded).
    // Most window managers still support this.

    // ChangeProperty: WM_NAME
    // Format:
    //   opcode: 18
    //   mode: 0 (Replace)
    //   length: 4 + 8 + pad4(title_len) ... let us compute
    //   window: CARD32
    //   property: CARD32 (WM_NAME = 39, a pre-defined atom)
    //   type: CARD32 (STRING = 31, a pre-defined atom)
    //   format: 8 (8 bits per element)
    //   data length: CARD32 (number of elements)
    //   data: the title string, padded to 4 bytes

    size_t title_len = title.size();
    size_t padded_title = pad4(title_len);
    size_t req_size = 24 + padded_title;

    std::vector<uint8_t> req(req_size, 0);
    req[0] = X_ChangeProperty;
    req[1] = X11_PropModeReplace;
    put_u16(&req[2], (uint16_t)(req_size / 4));
    put_u32(&req[4], m_window_id);
    put_u32(&req[8], 39);   // WM_NAME atom (pre-defined)
    put_u32(&req[12], 31);  // STRING atom (pre-defined)
    put_u32(&req[16], 8);   // format = 8 bits
    put_u32(&req[20], (uint32_t)title_len);
    memcpy(&req[24], title.c_str(), title_len);

    conn->send(req.data(), req_size);
}

// TEACHING NOTE: PutImage request
// =========================================================================
// PutImage (opcode 72) copies a block of pixel data from the client to a
// drawable (window or pixmap) on the server. We use it to send our back
// buffer to the window.
//
// Format:
//   opcode: 72
//   format: 2 (ZPixmap - each pixel is a full color value)
//   request length: CARD16
//   drawable: CARD32
//   gc: CARD32
//   width: CARD16
//   height: CARD16
//   dst_x: INT16
//   dst_y: INT16
//   left_pad: CARD8 (0 for ZPixmap)
//   depth: CARD8
//   data: pixel data, padded to 4 bytes
//
// For a 24-bit display, each pixel is 4 bytes (32-bit) in ZPixmap format
// with the low byte being blue. We need to convert from our BGRA format
// to the X server format. For most X servers, the format is BGRX which
// matches our back buffer layout (ignoring alpha).

void X11Window::put_image(X11Connection* conn, uint8_t* data, int w, int h, int stride) {
    if (!conn || !conn->is_connected() || !m_window_id || !m_gc_id) return;

    // For large images, we need to split into chunks because X11 has a
    // maximum request size. We send one row at a time as a simple approach.
    // A more efficient approach would be to send the whole image if it fits.
    //
    // The maximum request size is at least 16384 longwords (65536 bytes).
    // For a 32-bit pixel, one row of 800 pixels = 3200 bytes = 800 longwords.
    // So we can send the whole image if it is not too large.

    // For the full image, the request size is:
    //   24 bytes header + w * 4 bytes per row
    // For 800x600: 24 + 800*600*4 = 24 + 1920000 = way too big.
    // X11 maximum request is typically 262144 bytes.
    // 262144 / 4 = 65536 longwords. So max data = 65536*4 - 24 = 262120 bytes.
    // At 4 bytes/pixel: max ~65530 pixels per request.
    // For a 800-wide window: max ~81 rows per request.

    int max_rows_per_chunk = 80;
    int depth = conn->get_depth();
    int bpp = 4;  // we always send 32-bit ZPixmap

    for (int y = 0; y < h; y += max_rows_per_chunk) {
        int chunk_h = std::min(max_rows_per_chunk, h - y);
        int data_len = w * chunk_h * bpp;
        size_t padded_data = pad4((size_t)data_len);
        size_t req_size = 24 + padded_data;

        std::vector<uint8_t> req(req_size, 0);
        req[0] = X_PutImage;
        req[1] = X11_ZPixmap;
        put_u16(&req[2], (uint16_t)(req_size / 4));
        put_u32(&req[4], m_window_id);
        put_u32(&req[8], m_gc_id);
        put_u16(&req[12], (uint16_t)w);
        put_u16(&req[14], (uint16_t)chunk_h);
        put_u16(&req[16], 0);  // dst_x
        put_u16(&req[18], (uint16_t)y);  // dst_y
        req[20] = 0;  // left_pad
        req[21] = (uint8_t)depth;
        // data starts at offset 24
        for (int row = 0; row < chunk_h; ++row) {
            uint8_t* src = data + (size_t)(y + row) * stride;
            uint8_t* dst = req.data() + 24 + (size_t)row * w * bpp;
            memcpy(dst, src, (size_t)w * bpp);
        }

        conn->send(req.data(), req_size);
    }

    conn->flush();
}

void X11Window::flush(X11Connection* conn) {
    if (conn) conn->flush();
}

// TEACHING NOTE: X11 event handling
// =========================================================================
// X11 events are 32 bytes each. The server sends them when input occurs or
// the window needs redrawing. We read the first byte to determine the event
// type, then parse the rest accordingly.
//
// Events are only sent for windows that have selected for them (via the
// event mask in CreateWindow or ChangeWindowAttributes).
//
// We use poll() to check if data is available without blocking, so the
// event loop can also handle other I/O (like network for HTTP).

bool X11Window::next_event(X11Connection* conn, X11Event* event) {
    if (!conn || !conn->is_connected() || !event) return false;

    // Check if data is available
    struct pollfd pfd;
    pfd.fd = conn->get_fd();
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, 0) <= 0) {
        return false;  // no data available
    }

    // Read the 32-byte event
    uint8_t raw[32];
    try {
        conn->receive(raw, 32);
    } catch (...) {
        return false;
    }

    return parse_event(raw, 32, event);
}

// TEACHING NOTE: Keycode to keysym mapping
// =========================================================================
// X11 keycodes are hardware-dependent scancode numbers. Keysyms are
// abstract identifiers for characters and keys (like XK_A = 0x0061 for 'a').
// Normally this mapping requires the X11 keyboard extension (XKB), which
// is very complex. For our minimal implementation, we provide a basic
// US keyboard mapping for common keys.

uint32_t X11Window::keycode_to_keysym(uint8_t keycode, bool shift) {
    // Basic US keyboard keycode-to-keysym mapping
    // These are the standard X11 keysyms (from keysymdef.h)
    // The keycodes are typical for a US PC keyboard.
    // This is a simplified mapping and may not be correct for all keyboards.

    static const struct {
        uint8_t keycode;
        uint32_t normal;
        uint32_t shifted;
    } keymap[] = {
        {24, 0x0071, 0x0051},  // q Q
        {25, 0x0077, 0x0057},  // w W
        {26, 0x0065, 0x0045},  // e E
        {27, 0x0072, 0x0052},  // r R
        {28, 0x0074, 0x0054},  // t T
        {29, 0x0079, 0x0059},  // y Y
        {30, 0x0075, 0x0055},  // u U
        {31, 0x0069, 0x0049},  // i I
        {32, 0x006f, 0x004f},  // o O
        {33, 0x0070, 0x0050},  // p P
        {34, 0x0061, 0x0041},  // a A
        {35, 0x0073, 0x0053},  // s S
        {36, 0x0064, 0x0044},  // d D
        {37, 0x0066, 0x0046},  // f F
        {38, 0x0067, 0x0047},  // g G
        {39, 0x0068, 0x0048},  // h H
        {40, 0x006a, 0x004a},  // j J
        {41, 0x006b, 0x004b},  // k K
        {42, 0x006c, 0x004c},  // l L
        {43, 0x007a, 0x005a},  // z Z
        {44, 0x0078, 0x0058},  // x X
        {45, 0x0063, 0x0043},  // c C
        {46, 0x0076, 0x0056},  // v V
        {47, 0x0062, 0x0042},  // b B
        {48, 0x006e, 0x004e},  // n N
        {49, 0x006d, 0x004d},  // m M
        {10, 0x0031, 0x0021},  // 1 !
        {11, 0x0032, 0x0040},  // 2 @
        {12, 0x0033, 0x0023},  // 3 #
        {13, 0x0034, 0x0024},  // 4 $
        {14, 0x0035, 0x0025},  // 5 %
        {15, 0x0036, 0x005e},  // 6 ^
        {16, 0x0037, 0x0026},  // 7 &
        {17, 0x0038, 0x002a},  // 8 *
        {18, 0x0039, 0x0028},  // 9 (
        {19, 0x0030, 0x0029},  // 0 )
        {20, 0x002d, 0x005f},  // - _
        {21, 0x003d, 0x002b},  // = +
        {22, 0xff08, 0xff08},  // BackSpace
        {23, 0xff09, 0xff09},  // Tab
        {36, 0xff0d, 0xff0d},  // Enter/Return
        {37, 0xffe1, 0xffe1},  // Shift_L
        {50, 0xffe2, 0xffe2},  // Shift_R
        {64, 0xffe9, 0xffe9},  // Alt_L
        {65, 0x0020, 0x0020},  // Space
        {66, 0xffe3, 0xffe3},  // Control_L
        {67, 0xff1b, 0xff1b},  // Escape
        {9,  0xff1b, 0xff1b},  // Escape (alt)
        {24, 0x0071, 0x0051},  // duplicate to avoid gaps
    };

    for (const auto& k : keymap) {
        if (k.keycode == keycode) {
            return shift ? k.shifted : k.normal;
        }
    }

    // Return 0 for unmapped keys
    return 0;
}

bool X11Window::parse_event(const uint8_t* data, size_t len, X11Event* event) {
    if (len < 32 || !event) return false;

    uint8_t type = data[0] & 0x7F;  // bit 7 set means this is a sent event

    switch (type) {
        case X11_Expose_code: {
            // Expose event format:
            //   0: type (12)
            //   1: unused
            //   2-3: sequence
            //   4-7: window
            //   8-9: x
            //   10-11: y
            //   12-13: width
            //   14-15: height
            //   16-19: count (number of remaining expose events)
            event->type = X11_EVENT_EXPOSE;
            event->expose_x = (int16_t)get_u16(&data[8]);
            event->expose_y = (int16_t)get_u16(&data[10]);
            event->expose_w = get_u16(&data[12]);
            event->expose_h = get_u16(&data[14]);
            return true;
        }
        case X11_KeyPress_code: {
            // KeyPress event format:
            //   0: type (2)
            //   1: detail (keycode)
            //   2-3: sequence
            //   4-7: time
            //   8-11: root
            //   12-15: event (window)
            //   16-19: child
            //   20-21: root_x
            //   22-23: root_y
            //   24-25: event_x
            //   26-27: event_y
            //   28-29: state (modifier mask)
            //   30: same_screen
            event->type = X11_EVENT_KEY_PRESS;
            event->keycode = data[1];
            uint16_t state = get_u16(&data[28]);
            bool shift = (state & 1) != 0;
            event->keysym = keycode_to_keysym(data[1], shift);
            event->mouse_x = (int16_t)get_u16(&data[24]);
            event->mouse_y = (int16_t)get_u16(&data[26]);
            return true;
        }
        case X11_KeyRelease_code: {
            event->type = X11_EVENT_KEY_RELEASE;
            event->keycode = data[1];
            uint16_t state = get_u16(&data[28]);
            bool shift = (state & 1) != 0;
            event->keysym = keycode_to_keysym(data[1], shift);
            return true;
        }
        case X11_ButtonPress_code: {
            // ButtonPress event:
            //   0: type (4)
            //   1: detail (button)
            //   2-3: sequence
            //   4-7: time
            //   8-11: root
            //   12-15: event (window)
            //   ...
            //   24-25: event_x
            //   26-27: event_y
            //   28-29: state
            event->type = X11_EVENT_BUTTON_PRESS;
            event->button = data[1];
            event->mouse_x = (int16_t)get_u16(&data[24]);
            event->mouse_y = (int16_t)get_u16(&data[26]);
            return true;
        }
        case X11_ButtonRelease_code: {
            event->type = X11_EVENT_BUTTON_RELEASE;
            event->button = data[1];
            event->mouse_x = (int16_t)get_u16(&data[24]);
            event->mouse_y = (int16_t)get_u16(&data[26]);
            return true;
        }
        case X11_MotionNotify_code: {
            // MotionNotify event:
            //   similar structure to ButtonPress
            event->type = X11_EVENT_MOTION;
            event->mouse_x = (int16_t)get_u16(&data[24]);
            event->mouse_y = (int16_t)get_u16(&data[26]);
            event->button = 0;
            return true;
        }
        case X11_ClientMessage_code: {
            // ClientMessage - could be WM_DELETE_WINDOW
            // Check if the message type is WM_PROTOCOLS and the data is WM_DELETE_WINDOW
            // For simplicity, treat any ClientMessage as a close event
            event->type = X11_EVENT_CLOSE;
            return true;
        }
        default:
            // Unknown event type - skip
            return false;
    }
}

} // namespace chinstrap