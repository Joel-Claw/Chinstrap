// gui.hpp - GUI toolkit primitives (widgets, layout, event loop)
//
// TEACHING NOTE: GUI architecture
// ===========================================================================
// A GUI toolkit provides reusable building blocks for creating user
// interfaces. The key concepts are:
//
//   1. Widgets: visual elements that the user interacts with. Each widget
//      has a position, size, and appearance. Widgets can contain other
//      widgets (compositional hierarchy). Examples: buttons, text inputs,
//      labels, scrollbars, containers.
//
//   2. Layout: the process of computing the position and size of each
//      widget. This can be manual (you specify x, y, w, h) or automatic
//      (using a layout manager that distributes space based on rules).
//
//   3. Events: user actions (mouse click, key press) are delivered to
//      the appropriate widget based on hit testing (which widget is under
//      the mouse?). The event loop dispatches events until the application
//      exits.
//
//   4. Focus: one widget has keyboard focus at a time. Key events go to
//      the focused widget. Focus moves with Tab key or mouse clicks.
//
//   5. Painting: widgets draw themselves to the display. Drawing is
//      triggered by the event loop when the display needs updating.
//
// How GTK and Qt work:
//   GTK uses a C-based object system (GObject) with signals and slots.
//   Qt uses C++ with its moc (meta-object compiler) for signals/slots.
//   Both provide a rich set of widgets, layout managers, themes, and
//   platform abstraction.
//
// We implement a minimal toolkit with the essential widgets needed for a
// browser: labels, buttons, text inputs, scrollbars, and containers.
// This is enough to render basic web pages and UI controls.

#pragma once

#include "display.hpp"
#include "font.hpp"
#include "input.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace chinstrap {

// TEACHING NOTE: Widget base class
// =========================================================================
// Every widget inherits from Widget and implements:
//   - draw(): paint the widget to the display
//   - handle_event(): process input events
//   - measure(): compute preferred size
//
// Widgets have bounds (x, y, width, height) relative to their parent.
// The parent container is responsible for positioning child widgets.

class Widget {
public:
    Widget();
    virtual ~Widget();

    // Position and size (relative to parent)
    int x, y, width, height;

    // Whether the widget is visible
    bool visible;

    // Whether the widget can receive focus
    bool focusable;

    // Parent widget (nullptr for top-level)
    Widget* parent;

    // User data for identification
    int id;

    // Virtual methods
    virtual void draw(Display& display, Font* font) = 0;
    virtual bool handle_event(const InputEvent& event) { (void)event; return false; }

    // Hit test: is the point inside this widget?
    virtual bool hit_test(int mx, int my) const {
        return mx >= x && mx < x + width && my >= y && my < y + height;
    }

    // Get absolute position (by walking up the parent chain)
    void get_absolute_pos(int& ax, int& ay) const;

    // Set bounds
    void set_bounds(int x_, int y_, int w_, int h_) {
        x = x_; y = y_; width = w_; height = h_;
    }
};

// TEACHING NOTE: Label widget
// =========================================================================
// A label displays non-interactive text. It is the simplest widget.

class Label : public Widget {
public:
    std::string text;
    Color color;
    int font_size;

    Label(const std::string& text_ = "", int font_size_ = 14);

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override { (void)event; return false; }
};

// TEACHING NOTE: Button widget
// =========================================================================
// A button is a clickable region with text. It has normal, hover, and
// pressed states. When clicked, it calls the on_click callback.

class Button : public Widget {
public:
    std::string text;
    Color bg_color;
    Color text_color;
    int font_size;

    bool hovered;
    bool pressed;

    std::function<void()> on_click;

    Button(const std::string& text_ = "Button", int font_size_ = 14);

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override;

private:
    void update_hover_state(int mouse_x, int mouse_y);
};

// TEACHING NOTE: TextInput widget
// =========================================================================
// A text input allows the user to type text. It has a cursor, supports
// backspace, delete, left/right arrows, and can hold focus.
// This is a simplified single-line text input.

class TextInput : public Widget {
public:
    std::string text;
    Color bg_color;
    Color text_color;
    Color cursor_color;
    int font_size;

    int cursor_pos;  // character index of cursor
    bool focused;
    int scroll_offset;  // horizontal scroll in pixels

    std::function<void(const std::string&)> on_change;

    TextInput(int font_size_ = 14);

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override;

    void set_text(const std::string& t) { text = t; cursor_pos = (int)text.size(); }
    std::string get_text() const { return text; }
};

// TEACHING NOTE: Scrollbar widget
// =========================================================================
// A scrollbar allows scrolling through content larger than the viewport.
// It has a track, a thumb (draggable handle), and optional arrows.
// The thumb position represents the scroll position as a fraction of
// the total scrollable range.

class Scrollbar : public Widget {
public:
    bool horizontal;  // true = horizontal, false = vertical
    int min_value;
    int max_value;
    int page_size;
    int current_value;

    bool dragging;
    int drag_start;

    std::function<void(int)> on_scroll;

    Scrollbar(bool horizontal_ = false);

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override;

    int get_thumb_pos() const;
    int get_thumb_size() const;
    void set_range(int min_val, int max_val, int page);
    void set_value(int val);
};

// TEACHING NOTE: Container widget
// =========================================================================
// A container holds child widgets and can apply a layout. We support a
// simple vertical layout (stack) and manual positioning.

class Container : public Widget {
public:
    enum Layout {
        LAYOUT_NONE,       // manual positioning
        LAYOUT_VERTICAL,  // stack top to bottom
        LAYOUT_HORIZONTAL // arrange left to right
    };

    Layout layout;
    int padding;
    int spacing;
    Color bg_color;

    std::vector<std::unique_ptr<Widget>> children;

    Container(Layout layout_ = LAYOUT_NONE);

    void add(Widget* widget);
    void remove(int widget_id);
    void clear();

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override;

    void do_layout();
    Widget* find_widget_at(int mx, int my);

private:
    void draw_clipped(Display& display, Font* font, int clip_x, int clip_y, int clip_w, int clip_h);
};

// TEACHING NOTE: Window widget (top-level window content)
// =========================================================================
// A Window is a top-level container that represents the browser content
// area. It has a title bar and holds a content widget.

class Window : public Widget {
public:
    std::string title;
    Color bg_color;
    Color title_bar_color;
    int title_bar_height;

    Container content;

    Window(const std::string& title_ = "Chinstrap");

    void draw(Display& display, Font* font) override;
    bool handle_event(const InputEvent& event) override;
};

// TEACHING NOTE: GUI event loop
// =========================================================================
// The event loop is the heart of the GUI system. It:
//   1. Polls for input events (from evdev or X11)
//   2. Dispatches events to the appropriate widget (hit testing)
//   3. Redraws the display when needed
//   4. Synchronizes to the display refresh (vsync)
//
// The loop runs until the application exits.

class GUI {
public:
    GUI();
    ~GUI();

    // Initialize the GUI system with a display and input manager
    bool init(DisplayBackend backend, int width = 0, int height = 0);
    void shutdown();

    // Run the event loop
    void run();

    // Stop the event loop
    void stop() { m_running = false; }

    // Get the root window
    Window& get_root() { return m_root; }

    // Get the display
    Display& get_display() { return m_display; }

    // Get the font
    Font& get_font() { return m_font; }

    // Force a redraw
    void redraw() { m_need_redraw = true; }

private:
    Display m_display;
    InputManager m_input;
    Font m_font;
    Window m_root;
    bool m_running;
    bool m_need_redraw;

    Widget* m_focused;  // widget with keyboard focus
    Widget* m_hovered;  // widget under mouse

    void dispatch_event(const InputEvent& event);
    void draw_all();
};

} // namespace chinstrap