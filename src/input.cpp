// input.cpp - Input handling implementation
//
// TEACHING NOTE: This file implements the InputManager class which handles
// keyboard and mouse input in both framebuffer (evdev) and X11 modes.
//
// In framebuffer mode, we read raw events from /dev/input/event* files.
// The Linux input subsystem (evdev) provides a structured event format
// with type, code, and value fields. We convert these to our unified
// InputEvent structure.
//
// In X11 mode, the X11Window class already parses X11 events into
// X11Event structs. We provide a conversion function to turn those into
// InputEvent structs.

#include "input.hpp"
#include "x11.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <fstream>
#include <algorithm>

namespace chinstrap {

// ============================================================================
// Constructor / Destructor
// ============================================================================

InputManager::InputManager()
    : m_framebuffer_mode(false)
    , m_x11_mode(false)
    , m_initialized(false)
    , m_kb_fd(-1)
    , m_mouse_fd(-1)
    , m_screen_width(0)
    , m_screen_height(0)
    , m_mouse_x(0)
    , m_mouse_y(0)
    , m_mouse_buttons(0)
    , m_shift(false)
    , m_control(false)
    , m_alt(false) {}

InputManager::~InputManager() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

// TEACHING NOTE: Finding input devices in framebuffer mode
// =========================================================================
// The Linux kernel exposes input devices as /dev/input/eventN files.
// Each device has a name that can be read from
// /sys/class/input/eventN/device/name
//
// We search for devices with "keyboard" or "key" in their name for keyboard
// input, and "mouse" for mouse input. On many systems, a USB keyboard will
// be event0 or event1, and a USB mouse will be event2 or similar.
//
// Touchscreen devices appear as mice with absolute positioning.
//
// Reading from these devices requires root permissions or membership in
// the "input" group.

int InputManager::find_input_device(const std::string& name_pattern) {
    DIR* dir = opendir("/dev/input");
    if (!dir) return -1;

    int best_fd = -1;
    (void)best_fd;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string dev_name = entry->d_name;
        if (dev_name.find("event") != 0) continue;

        // Read the device name from /sys
        std::string sys_path = "/sys/class/input/" + dev_name + "/device/name";
        std::ifstream name_file(sys_path);
        std::string device_name;
        if (name_file.is_open()) {
            std::getline(name_file, device_name);
        }

        // Case-insensitive search
        std::string lower_name = device_name;
        std::string lower_pattern = name_pattern;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);

        if (lower_name.find(lower_pattern) != std::string::npos) {
            std::string dev_path = "/dev/input/" + dev_name;
            int fd = open(dev_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                closedir(dir);
                return fd;
            }
        }
    }
    closedir(dir);
    return -1;
}

bool InputManager::init_framebuffer(int screen_width, int screen_height) {
    m_framebuffer_mode = true;
    m_screen_width = screen_width;
    m_screen_height = screen_height;

    // Try to find keyboard device
    m_kb_fd = find_input_device("keyboard");
    if (m_kb_fd < 0) {
        m_kb_fd = find_input_device("key");
    }

    // Try to find mouse device
    m_mouse_fd = find_input_device("mouse");

    // If we could not find named devices, try opening event0 and event1
    if (m_kb_fd < 0) {
        m_kb_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    }
    if (m_mouse_fd < 0) {
        m_mouse_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    }

    // Initialize mouse position to center of screen
    m_mouse_x = screen_width / 2;
    m_mouse_y = screen_height / 2;

    m_initialized = (m_kb_fd >= 0 || m_mouse_fd >= 0);
    return m_initialized;
}

bool InputManager::init_x11() {
    m_x11_mode = true;
    m_initialized = true;
    return true;
}

void InputManager::shutdown() {
    if (m_kb_fd >= 0) {
        close(m_kb_fd);
        m_kb_fd = -1;
    }
    if (m_mouse_fd >= 0) {
        close(m_mouse_fd);
        m_mouse_fd = -1;
    }
    m_initialized = false;
}

// ============================================================================
// Event polling
// ============================================================================

// TEACHING NOTE: evdev event format
// =========================================================================
// The evdev interface returns events as struct input_event (defined in
// linux/input.h):
//   struct input_event {
//     struct timeval time;  // timestamp
//     uint16_t type;        // event type (EV_KEY, EV_REL, EV_ABS, EV_SYN)
//     uint16_t code;        // key code, axis code, etc.
//     int32_t value;         // value (1=press, 0=release, 2=repeat for keys)
//   };
//
// On 64-bit systems, sizeof(struct input_event) = 24 bytes.
// On 32-bit systems, it is 16 bytes.
//
// EV_KEY: keyboard key or mouse button
//   code: key/button code (e.g. KEY_A = 30, BTN_LEFT = 0x110)
//   value: 1=pressed, 0=released, 2=autorepeat
//
// EV_REL: relative movement (mouse)
//   code: REL_X (0), REL_Y (1), REL_WHEEL (8)
//   value: movement amount
//
// EV_ABS: absolute position (touchscreen)
//   code: ABS_X (0), ABS_Y (1)
//   value: absolute coordinate
//
// EV_SYN: synchronization marker (end of event frame)
//   We use this to know when a batch of related events is complete.

bool InputManager::read_evdev_event(int fd, uint16_t& type, uint16_t& code, int32_t& value) {
    struct input_event ev;
    ssize_t n = read(fd, &ev, sizeof(ev));
    if (n < (ssize_t)sizeof(ev)) return false;

    type = ev.type;
    code = ev.code;
    value = ev.value;
    return true;
}

// TEACHING NOTE: evdev key code mapping
// =========================================================================
// The Linux input subsystem uses key codes defined in linux/input-event-codes.h.
// These are different from X11 keycodes (X11 keycode = evdev keycode + 8).
// We map the common evdev key codes to our internal keysym values (which
// match X11 keysyms for compatibility).

uint32_t InputManager::evdev_keycode_to_keysym(uint16_t keycode, bool shift) const {
    // TEACHING NOTE: Common evdev key codes
    // =================================================================
    // The evdev key codes are defined in linux/input-event-codes.h:
    //   KEY_A = 30, KEY_B = 48, ..., KEY_Z = 44
    //   KEY_1 = 2, KEY_2 = 3, ..., KEY_0 = 11
    //   KEY_SPACE = 57, KEY_ENTER = 28, KEY_BACKSPACE = 14
    //   KEY_TAB = 15, KEY_ESC = 1, KEY_DELETE = 111
    //   KEY_LEFT = 105, KEY_RIGHT = 106, KEY_UP = 103, KEY_DOWN = 108
    //   etc.

    static const struct {
        uint16_t code;
        uint32_t normal;
        uint32_t shifted;
    } keymap[] = {
        // Letters
        {30, 0x0061, 0x0041},  // a A
        {48, 0x0062, 0x0042},  // b B
        {46, 0x0063, 0x0043},  // c C
        {32, 0x0064, 0x0044},  // d D
        {18, 0x0065, 0x0045},  // e E
        {33, 0x0066, 0x0046},  // f F
        {34, 0x0067, 0x0047},  // g G
        {35, 0x0068, 0x0048},  // h H
        {23, 0x0069, 0x0049},  // i I
        {36, 0x006a, 0x004a},  // j J
        {37, 0x006b, 0x004b},  // k K
        {38, 0x006c, 0x004c},  // l L
        {50, 0x006d, 0x004d},  // m M
        {49, 0x006e, 0x004e},  // n N
        {24, 0x006f, 0x004f},  // o O
        {25, 0x0070, 0x0050},  // p P
        {16, 0x0071, 0x0051},  // q Q
        {19, 0x0072, 0x0052},  // r R
        {31, 0x0073, 0x0053},  // s S
        {20, 0x0074, 0x0054},  // t T
        {22, 0x0075, 0x0055},  // u U
        {47, 0x0076, 0x0056},  // v V
        {17, 0x0077, 0x0057},  // w W
        {45, 0x0078, 0x0058},  // x X
        {21, 0x0079, 0x0059},  // y Y
        {44, 0x007a, 0x005a},  // z Z
        // Numbers (top row)
        {2,  0x0031, 0x0021},  // 1 !
        {3,  0x0032, 0x0040},  // 2 @
        {4,  0x0033, 0x0023},  // 3 #
        {5,  0x0034, 0x0024},  // 4 $
        {6,  0x0035, 0x0025},  // 5 %
        {7,  0x0036, 0x005e},  // 6 ^
        {8,  0x0037, 0x0026},  // 7 &
        {9,  0x0038, 0x002a},  // 8 *
        {10, 0x0039, 0x0028},  // 9 (
        {11, 0x0030, 0x0029},  // 0 )
        {12, 0x002d, 0x005f},  // - _
        {13, 0x003d, 0x002b},  // = +
        // Special keys
        {57, 0x0020, 0x0020},  // Space
        {28, KEY_ENTER, KEY_ENTER},
        {14, KEY_BACKSPACE, KEY_BACKSPACE},
        {15, KEY_TAB, KEY_TAB},
        {1,  KEY_ESCAPE, KEY_ESCAPE},
        {111, KEY_DELETE, KEY_DELETE},
        {102, KEY_HOME, KEY_HOME},
        {103, KEY_UP, KEY_UP},
        {105, KEY_LEFT, KEY_LEFT},
        {106, KEY_RIGHT, KEY_RIGHT},
        {108, KEY_DOWN, KEY_DOWN},
        {104, KEY_PAGE_UP, KEY_PAGE_UP},
        {109, KEY_PAGE_DOWN, KEY_PAGE_DOWN},
        {107, KEY_END, KEY_END},
        // Modifiers
        {42, KEY_SHIFT_L, KEY_SHIFT_L},   // Left Shift
        {54, KEY_SHIFT_R, KEY_SHIFT_R},   // Right Shift
        {29, KEY_CONTROL_L, KEY_CONTROL_L}, // Left Ctrl
        {97, KEY_CONTROL_R, KEY_CONTROL_R}, // Right Ctrl
        {56, KEY_ALT_L, KEY_ALT_L},      // Left Alt
        {100, KEY_ALT_R, KEY_ALT_R},     // Right Alt
        // Punctuation
        {39, 0x003b, 0x003a},  // ; :
        {40, 0x0027, 0x0022},  // ' " (apostrophe, quote)
        {51, 0x005c, 0x007c},  // \ |
        {52, 0x002c, 0x003c},  // , <
        {53, 0x002e, 0x003e},  // . >
        {43, 0x002f, 0x003f},  // / ?
        {26, 0x005b, 0x007b},  // [ {
        {27, 0x005d, 0x007d},  // ] }
        {41, 0x0060, 0x007e},  // ` ~ (backtick, tilde)
        // Function keys
        {59, KEY_F1, KEY_F1},
        {60, KEY_F2, KEY_F2},
        {61, KEY_F3, KEY_F3},
        {62, KEY_F4, KEY_F4},
        {63, KEY_F5, KEY_F5},
        {64, KEY_F6, KEY_F6},
        {65, KEY_F7, KEY_F7},
        {66, KEY_F8, KEY_F8},
        {67, KEY_F9, KEY_F9},
        {68, KEY_F10, KEY_F10},
        {87, KEY_F11, KEY_F11},
        {88, KEY_F12, KEY_F12},
    };

    for (const auto& k : keymap) {
        if (k.code == keycode) {
            return shift ? k.shifted : k.normal;
        }
    }

    return 0;  // unmapped
}

char InputManager::evdev_keycode_to_char(uint16_t keycode, bool shift) const {
    uint32_t sym = evdev_keycode_to_keysym(keycode, shift);
    if (sym >= 0x20 && sym <= 0x7E) {
        return (char)sym;
    }
    return 0;  // non-printable
}

void InputManager::evdev_to_input(uint16_t code, int32_t value, InputEvent& event) {
    event.keycode = code;
    event.keysym = evdev_keycode_to_keysym(code, m_shift);
    event.character = evdev_keycode_to_char(code, m_shift);
    event.shift = m_shift;
    event.control = m_control;
    event.alt = m_alt;

    if (value == 1) {
        event.type = INPUT_KEY_PRESS;
    } else if (value == 0) {
        event.type = INPUT_KEY_RELEASE;
    } else {
        event.type = INPUT_KEY_PRESS;  // autorepeat
    }
}

bool InputManager::poll(InputEvent& event) {
    event.type = INPUT_NONE;

    if (!m_initialized) return false;

    if (m_framebuffer_mode) {
        // Poll keyboard
        if (m_kb_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = m_kb_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint16_t type, code;
                int32_t value;

                // Read events until we get a key event
                while (read_evdev_event(m_kb_fd, type, code, value)) {
                    if (type == EV_KEY) {
                        // Update modifier state
                        if (code == 42 || code == 54) m_shift = (value != 0);
                        if (code == 29 || code == 97) m_control = (value != 0);
                        if (code == 56 || code == 100) m_alt = (value != 0);

                        evdev_to_input(code, value, event);
                        if (event.type != INPUT_NONE) return true;
                    }
                }
            }
        }

        // Poll mouse
        if (m_mouse_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = m_mouse_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint16_t type, code;
                int32_t value;

                while (read_evdev_event(m_mouse_fd, type, code, value)) {
                    if (type == EV_KEY) {
                        // Mouse button event
                        // evdev mouse button codes:
                        //   BTN_LEFT = 0x110 (272)
                        //   BTN_RIGHT = 0x111 (273)
                        //   BTN_MIDDLE = 0x112 (274)

                        event.type = (value == 1) ? INPUT_MOUSE_PRESS : INPUT_MOUSE_RELEASE;
                        event.mouse_x = m_mouse_x;
                        event.mouse_y = m_mouse_y;

                        if (code == 0x110) {
                            event.button = MOUSE_LEFT;
                        } else if (code == 0x111) {
                            event.button = MOUSE_RIGHT;
                        } else if (code == 0x112) {
                            event.button = MOUSE_MIDDLE;
                        } else {
                            event.button = MOUSE_NONE;
                        }

                        m_mouse_buttons = value ? event.button : MOUSE_NONE;
                        return true;
                    } else if (type == EV_REL) {
                        // Relative mouse movement
                        if (code == 0) {  // REL_X
                            m_mouse_x += value;
                        } else if (code == 1) {  // REL_Y
                            m_mouse_y += value;
                        } else if (code == 8) {  // REL_WHEEL
                            event.type = INPUT_MOUSE_SCROLL;
                            event.scroll_delta = value;
                            event.mouse_x = m_mouse_x;
                            event.mouse_y = m_mouse_y;
                            return true;
                        }

                        // Clamp to screen
                        if (m_screen_width > 0) {
                            if (m_mouse_x < 0) m_mouse_x = 0;
                            if (m_mouse_x >= m_screen_width) m_mouse_x = m_screen_width - 1;
                        }
                        if (m_screen_height > 0) {
                            if (m_mouse_y < 0) m_mouse_y = 0;
                            if (m_mouse_y >= m_screen_height) m_mouse_y = m_screen_height - 1;
                        }

                        event.type = INPUT_MOUSE_MOVE;
                        event.mouse_x = m_mouse_x;
                        event.mouse_y = m_mouse_y;
                        return true;
                    }
                }
            }
        }

        return false;
    }

    // X11 mode: events are handled by the GUI event loop via convert_x11_event
    return false;
}

bool InputManager::convert_x11_event(const void* x11_event, InputEvent& event) {
    // TEACHING NOTE: X11 event conversion
    // =========================================================================
    // In X11 mode, the GUI event loop reads events from the X11Window class
    // (which returns X11Event structs). We convert those to InputEvent.
    // This keeps the input handling unified for the rest of the browser.

    const X11Event* xev = static_cast<const X11Event*>(x11_event);
    if (!xev) return false;

    event.type = INPUT_NONE;
    event.shift = m_shift;
    event.control = m_control;
    event.alt = m_alt;

    switch (xev->type) {
        case X11_EVENT_KEY_PRESS:
            event.type = INPUT_KEY_PRESS;
            event.keycode = xev->keycode;
            event.keysym = xev->keysym;
            // Check for shift
            if (xev->keysym == KEY_SHIFT_L || xev->keysym == KEY_SHIFT_R) m_shift = true;
            if (xev->keysym == KEY_CONTROL_L || xev->keysym == KEY_CONTROL_R) m_control = true;
            if (xev->keysym == KEY_ALT_L || xev->keysym == KEY_ALT_R) m_alt = true;
            event.shift = m_shift;
            event.control = m_control;
            event.alt = m_alt;
            // Convert keysym to character
            if (xev->keysym >= 0x20 && xev->keysym <= 0x7E) {
                event.character = (char)xev->keysym;
            }
            return true;

        case X11_EVENT_KEY_RELEASE:
            event.type = INPUT_KEY_RELEASE;
            event.keycode = xev->keycode;
            event.keysym = xev->keysym;
            if (xev->keysym == KEY_SHIFT_L || xev->keysym == KEY_SHIFT_R) m_shift = false;
            if (xev->keysym == KEY_CONTROL_L || xev->keysym == KEY_CONTROL_R) m_control = false;
            if (xev->keysym == KEY_ALT_L || xev->keysym == KEY_ALT_R) m_alt = false;
            event.shift = m_shift;
            event.control = m_control;
            event.alt = m_alt;
            if (xev->keysym >= 0x20 && xev->keysym <= 0x7E) {
                event.character = (char)xev->keysym;
            }
            return true;

        case X11_EVENT_BUTTON_PRESS:
            event.type = INPUT_MOUSE_PRESS;
            event.mouse_x = xev->mouse_x;
            event.mouse_y = xev->mouse_y;
            m_mouse_x = xev->mouse_x;
            m_mouse_y = xev->mouse_y;
            if (xev->button == 1) event.button = MOUSE_LEFT;
            else if (xev->button == 2) event.button = MOUSE_MIDDLE;
            else if (xev->button == 3) event.button = MOUSE_RIGHT;
            else if (xev->button == 4) {
                event.type = INPUT_MOUSE_SCROLL;
                event.scroll_delta = 1;
            } else if (xev->button == 5) {
                event.type = INPUT_MOUSE_SCROLL;
                event.scroll_delta = -1;
            }
            return true;

        case X11_EVENT_BUTTON_RELEASE:
            event.type = INPUT_MOUSE_RELEASE;
            event.mouse_x = xev->mouse_x;
            event.mouse_y = xev->mouse_y;
            m_mouse_x = xev->mouse_x;
            m_mouse_y = xev->mouse_y;
            if (xev->button == 1) event.button = MOUSE_LEFT;
            else if (xev->button == 2) event.button = MOUSE_MIDDLE;
            else if (xev->button == 3) event.button = MOUSE_RIGHT;
            return true;

        case X11_EVENT_MOTION:
            event.type = INPUT_MOUSE_MOVE;
            event.mouse_x = xev->mouse_x;
            event.mouse_y = xev->mouse_y;
            m_mouse_x = xev->mouse_x;
            m_mouse_y = xev->mouse_y;
            return true;

        case X11_EVENT_EXPOSE:
        case X11_EVENT_CLOSE:
        case X11_EVENT_NONE:
            return false;
    }

    return false;
}

} // namespace chinstrap