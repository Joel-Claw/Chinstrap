// input.hpp - Input handling for framebuffer and X11 modes
//
// TEACHING NOTE: How input devices work on Linux
// ===========================================================================
// On Linux, input devices (keyboard, mouse, touchscreen) are accessed via:
//
//   1. /dev/input/event* - the evdev interface
//      Each input device has a character device file in /dev/input/.
//      Reading from these files returns structured events with:
//        - type (EV_KEY, EV_ABS, EV_REL, EV_SYN, etc.)
//        - code (which key/button/axis)
//        - value (pressed=1, released=0, repeat=2 for keys; movement for axes)
//      This is the raw kernel interface, used in framebuffer mode.
//
//   2. X11 events - in X11 mode, the X server reads input devices and
//      sends events to clients. We parse X11 KeyPress, KeyRelease,
//      ButtonPress, ButtonRelease, and MotionNotify events.
//
// Keyboard input pipeline:
//   Raw scancode (from hardware) -> keycode (evdev) -> keysym (X11) ->
//   character (Unicode). We need to convert keycodes to characters for
//   text input. This requires a keyboard layout mapping (e.g. US QWERTY).
//
// Mouse input pipeline:
//   Raw events (button press/release, relative/absolute movement) ->
//   coordinates and button state. For framebuffer mode with a USB mouse,
//   we get relative movement from /dev/input/event* and track the position
//   ourselves. For X11 mode, the X server tracks absolute position.
//
// We implement both paths and provide a unified event interface.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace chinstrap {

// TEACHING NOTE: Unified input event
// =========================================================================
// We abstract over both input modes (evdev and X11) with a unified event
// structure. This lets the GUI toolkit work the same way regardless of
// how input is received.

enum InputEventType {
    INPUT_NONE = 0,
    INPUT_KEY_PRESS = 1,
    INPUT_KEY_RELEASE = 2,
    INPUT_MOUSE_MOVE = 3,
    INPUT_MOUSE_PRESS = 4,
    INPUT_MOUSE_RELEASE = 5,
    INPUT_MOUSE_SCROLL = 6,
    INPUT_MOUSE_DOUBLE_CLICK = 7,
};

enum MouseButton {
    MOUSE_NONE = 0,
    MOUSE_LEFT = 1,
    MOUSE_MIDDLE = 2,
    MOUSE_RIGHT = 3,
    MOUSE_SCROLL_UP = 4,
    MOUSE_SCROLL_DOWN = 5,
};

// Special key codes (for non-printable keys)
enum SpecialKey {
    KEY_INVALID = 0,
    KEY_BACKSPACE = 0xFF08,
    KEY_TAB = 0xFF09,
    KEY_ENTER = 0xFF0D,
    KEY_ESCAPE = 0xFF1B,
    KEY_DELETE = 0xFFFF,
    KEY_HOME = 0xFF50,
    KEY_LEFT = 0xFF51,
    KEY_UP = 0xFF52,
    KEY_RIGHT = 0xFF53,
    KEY_DOWN = 0xFF54,
    KEY_PAGE_UP = 0xFF55,
    KEY_PAGE_DOWN = 0xFF56,
    KEY_END = 0xFF57,
    KEY_SHIFT_L = 0xFFE1,
    KEY_SHIFT_R = 0xFFE2,
    KEY_CONTROL_L = 0xFFE3,
    KEY_CONTROL_R = 0xFFE4,
    KEY_ALT_L = 0xFFE9,
    KEY_ALT_R = 0xFFEA,
    KEY_CAPS_LOCK = 0xFFE5,
    KEY_F1 = 0xFFBE,
    KEY_F2 = 0xFFBF,
    KEY_F3 = 0xFFC0,
    KEY_F4 = 0xFFC1,
    KEY_F5 = 0xFFC2,
    KEY_F6 = 0xFFC3,
    KEY_F7 = 0xFFC4,
    KEY_F8 = 0xFFC5,
    KEY_F9 = 0xFFC6,
    KEY_F10 = 0xFFC7,
    KEY_F11 = 0xFFC8,
    KEY_F12 = 0xFFC9,
};

struct InputEvent {
    InputEventType type;

    // Keyboard
    uint32_t keycode;   // hardware keycode or evdev code
    uint32_t keysym;    // X11 keysym or our internal mapping
    char character;     // printable character (0 if non-printable)
    bool shift;
    bool control;
    bool alt;

    // Mouse
    int mouse_x;
    int mouse_y;
    MouseButton button;
    int scroll_delta;   // positive = up, negative = down

    InputEvent()
        : type(INPUT_NONE)
        , keycode(0)
        , keysym(0)
        , character(0)
        , shift(false)
        , control(false)
        , alt(false)
        , mouse_x(0)
        , mouse_y(0)
        , button(MOUSE_NONE)
        , scroll_delta(0) {}
};

// TEACHING NOTE: InputManager
// =========================================================================
// The InputManager abstracts input handling. In framebuffer mode, it opens
// /dev/input/event* files and reads evdev events. In X11 mode, it wraps
// the X11 event system (delegating to X11Connection/X11Window).
//
// For framebuffer mode, we need to:
//   1. Find and open the keyboard device (usually /dev/input/event0 or
//      /dev/input/event* where the device name contains "keyboard")
//   2. Find and open the mouse device (usually /dev/input/event* where
//      the device name contains "mouse")
//   3. Poll both for events and convert to our unified format
//
// For X11 mode, the X11Window already parses events into X11Event structs.
// We just convert those to InputEvent.

class InputManager {
public:
    InputManager();
    ~InputManager();

    // Initialize for framebuffer mode (evdev)
    // Returns true if at least one input device was found
    bool init_framebuffer(int screen_width, int screen_height);

    // Initialize for X11 mode
    // The X11 event handling is done via the X11Window, so we just
    // set up the modifier state tracking.
    bool init_x11();

    // Shutdown
    void shutdown();

    // Poll for input events (non-blocking)
    // Returns true if an event was available, fills in the event struct.
    // Returns false if no events pending.
    bool poll(InputEvent& event);

    // For X11 mode: convert an X11 event to an InputEvent
    // (used by the GUI event loop which already reads X11 events)
    bool convert_x11_event(const void* x11_event, InputEvent& event);

    // Get current mouse position
    void get_mouse_position(int& x, int& y) const { x = m_mouse_x; y = m_mouse_y; }

    // Get modifier state
    bool is_shift_pressed() const { return m_shift; }
    bool is_control_pressed() const { return m_control; }
    bool is_alt_pressed() const { return m_alt; }

private:
    bool m_framebuffer_mode;
    bool m_x11_mode;
    bool m_initialized;

    // --- Framebuffer (evdev) mode ---
    int m_kb_fd;    // keyboard input device fd
    int m_mouse_fd;  // mouse input device fd
    int m_screen_width;
    int m_screen_height;

    // Mouse tracking (for relative mouse in framebuffer mode)
    int m_mouse_x;
    int m_mouse_y;
    int m_mouse_buttons;

    // Modifier state
    bool m_shift;
    bool m_control;
    bool m_alt;

    // Helper: find input device by name pattern
    int find_input_device(const std::string& name_pattern);

    // Helper: read one evdev event from fd
    bool read_evdev_event(int fd, uint16_t& type, uint16_t& code, int32_t& value);

    // Helper: convert evdev keycode to keysym/character
    void evdev_to_input(uint16_t code, int32_t value, InputEvent& event);

    // TEACHING NOTE: evdev key code to keysym mapping
    // =========================================================================
    // The evdev interface uses its own keycode set, which is different
    // from X11 keycodes. The mapping is:
    //   X11 keycode = evdev keycode + 8
    // We use the evdev keycodes directly and map to our internal keysyms.
    // This is a simplified US keyboard layout mapping.

    uint32_t evdev_keycode_to_keysym(uint16_t keycode, bool shift) const;
    char evdev_keycode_to_char(uint16_t keycode, bool shift) const;
};

} // namespace chinstrap