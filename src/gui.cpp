// gui.cpp - GUI toolkit implementation
//
// TEACHING NOTE: This file implements the GUI toolkit for Chinstrap. It
// provides the widget hierarchy, event dispatch, and rendering. The
// toolkit is intentionally minimal - just enough for a browser UI.
//
// The architecture follows a standard retained-mode GUI model:
//   - Widgets are persistent objects that hold their state
//   - The event loop polls for input and dispatches to widgets
//   - Widgets draw themselves when the display needs updating
//
// This is how GTK, Qt, and most desktop GUI toolkits work. (Immediate-mode
// GUIs like Dear ImGui work differently - they redraw everything every
// frame and keep no persistent state.)

#include "gui.hpp"

#include <algorithm>
#include "x11.hpp"
#include <cstring>
#include <cmath>
#include <unistd.h>

namespace chinstrap {

// ============================================================================
// Widget base class
// ============================================================================

Widget::Widget()
    : x(0), y(0), width(0), height(0)
    , visible(true), focusable(false)
    , parent(nullptr), id(0) {}

Widget::~Widget() {}

void Widget::get_absolute_pos(int& ax, int& ay) const {
    ax = x;
    ay = y;
    if (parent) {
        int px, py;
        parent->get_absolute_pos(px, py);
        ax += px;
        ay += py;
    }
}

// ============================================================================
// Label
// ============================================================================

Label::Label(const std::string& text_, int font_size_)
    : text(text_), color(Color::BLACK), font_size(font_size_) {
    focusable = false;
    width = 100;
    height = font_size_ + 4;
}

void Label::draw(Display& display, Font* font) {
    if (!visible || text.empty()) return;
    (void)font;

    int ax, ay;
    get_absolute_pos(ax, ay);

    // Render text if font is available
    if (font && font->is_loaded()) {
        Font::TextLayout layout = font->rasterize_text(text, font_size);
        if (!layout.bitmap.empty()) {
            int px = ax;
            int py = ay;

            // Blit the text bitmap to the display
            for (int row = 0; row < layout.bitmap.height; ++row) {
                for (int col = 0; col < layout.bitmap.width; ++col) {
                    uint8_t alpha = layout.bitmap.pixels[(size_t)row * layout.bitmap.width + col];
                    if (alpha > 0) {
                        int dx = px + col;
                        int dy = py + row;
                        if (dx >= 0 && dx < display.get_width() &&
                            dy >= 0 && dy < display.get_height()) {
                            // Alpha blend with existing pixel
                            // For simplicity, just put the color with full opacity
                            // if alpha > 128
                            if (alpha > 128) {
                                display.put_pixel(dx, dy, color);
                            }
                        }
                    }
                }
            }
        }
    }
}

// ============================================================================
// Button
// ============================================================================

Button::Button(const std::string& text_, int font_size_)
    : text(text_)
    , bg_color(Color::LIGHT_GRAY)
    , text_color(Color::BLACK)
    , font_size(font_size_)
    , hovered(false)
    , pressed(false) {
    focusable = true;
    width = 100;
    height = font_size_ + 10;
}

void Button::update_hover_state(int mouse_x, int mouse_y) {
    bool was_hovered = hovered;
    hovered = hit_test(mouse_x, mouse_y);
    (void)was_hovered;
}

void Button::draw(Display& display, Font* font) {
    if (!visible) return;
    (void)font;

    int ax, ay;
    get_absolute_pos(ax, ay);

    // Choose color based on state
    Color fill_color = bg_color;
    if (pressed) {
        fill_color = Color(Color::DARK_GRAY.r + 40, Color::DARK_GRAY.g + 40, Color::DARK_GRAY.b + 40);
    } else if (hovered) {
        fill_color = Color(std::min(255, (int)bg_color.r + 20),
                          std::min(255, (int)bg_color.g + 20),
                          std::min(255, (int)bg_color.b + 20));
    }

    // Draw button background
    display.fill_rect(ax, ay, width, height, fill_color);

    // Draw border
    display.rect(ax, ay, width, height, Color::DARK_GRAY);

    // Draw text
    if (font && font->is_loaded() && !text.empty()) {
        Font::TextLayout layout = font->rasterize_text(text, font_size);
        if (!layout.bitmap.empty()) {
            // Center text in button
            int text_x = ax + (width - layout.bitmap.width) / 2;
            int text_y = ay + (height - layout.bitmap.height) / 2;

            for (int row = 0; row < layout.bitmap.height; ++row) {
                for (int col = 0; col < layout.bitmap.width; ++col) {
                    uint8_t alpha = layout.bitmap.pixels[(size_t)row * layout.bitmap.width + col];
                    if (alpha > 128) {
                        int dx = text_x + col;
                        int dy = text_y + row;
                        if (dx >= 0 && dx < display.get_width() &&
                            dy >= 0 && dy < display.get_height()) {
                            display.put_pixel(dx, dy, text_color);
                        }
                    }
                }
            }
        }
    }
}

bool Button::handle_event(const InputEvent& event) {
    switch (event.type) {
        case INPUT_MOUSE_MOVE:
            update_hover_state(event.mouse_x, event.mouse_y);
            return true;

        case INPUT_MOUSE_PRESS:
            if (event.button == MOUSE_LEFT && hit_test(event.mouse_x, event.mouse_y)) {
                pressed = true;
                return true;
            }
            return false;

        case INPUT_MOUSE_RELEASE:
            if (event.button == MOUSE_LEFT && pressed) {
                pressed = false;
                if (hit_test(event.mouse_x, event.mouse_y)) {
                    hovered = true;
                    if (on_click) on_click();
                }
                return true;
            }
            return false;

        case INPUT_KEY_PRESS:
            if (event.keysym == KEY_ENTER && on_click) {
                on_click();
                return true;
            }
            return false;

        default:
            return false;
    }
}

// ============================================================================
// TextInput
// ============================================================================

TextInput::TextInput(int font_size_)
    : bg_color(Color::WHITE)
    , text_color(Color::BLACK)
    , cursor_color(Color::BLACK)
    , font_size(font_size_)
    , cursor_pos(0)
    , focused(false)
    , scroll_offset(0) {
    focusable = true;
    width = 200;
    height = font_size_ + 10;
}

void TextInput::draw(Display& display, Font* font) {
    if (!visible) return;
    (void)font;

    int ax, ay;
    get_absolute_pos(ax, ay);

    // Draw background
    display.fill_rect(ax, ay, width, height, bg_color);

    // Draw border (thicker if focused)
    Color border_color = focused ? Color(0, 100, 200) : Color::GRAY;
    display.rect(ax, ay, width, height, border_color);

    // Draw text
    if (!text.empty()) {
        if (font && font->is_loaded()) {
            // Render the visible portion of text
            std::string visible_text = text;
            // For simplicity, render all text (a full implementation would clip)
            Font::TextLayout layout = font->rasterize_text(visible_text, font_size);
            if (!layout.bitmap.empty()) {
                int text_x = ax + 4;
                int text_y = ay + (height - layout.bitmap.height) / 2;

                for (int row = 0; row < layout.bitmap.height; ++row) {
                    for (int col = 0; col < layout.bitmap.width; ++col) {
                        uint8_t alpha = layout.bitmap.pixels[(size_t)row * layout.bitmap.width + col];
                        if (alpha > 128) {
                            int dx = text_x + col;
                            int dy = text_y + row;
                            if (dx >= ax && dx < ax + width &&
                                dy >= ay && dy < ay + height) {
                                display.put_pixel(dx, dy, text_color);
                            }
                        }
                    }
                }
            }
        }
    }

    // Draw cursor if focused
    if (focused) {
        // TEACHING NOTE: Blinking cursor
        // =================================================================
        // A real text cursor blinks. We would use a timer to toggle
        // visibility. For simplicity, we always draw it.

        // Estimate cursor x position based on text width
        int cursor_x = ax + 4;
        if (font && font->is_loaded() && cursor_pos > 0) {
            std::string before_cursor = text.substr(0, (size_t)cursor_pos);
            Font::TextLayout tl = font->rasterize_text(before_cursor, font_size);
            cursor_x += tl.advance_width;
        }

        display.vline(cursor_x, ay + 2, height - 4, cursor_color);
    }
}

bool TextInput::handle_event(const InputEvent& event) {
    switch (event.type) {
        case INPUT_MOUSE_PRESS:
            if (event.button == MOUSE_LEFT && hit_test(event.mouse_x, event.mouse_y)) {
                focused = true;
                return true;
            }
            focused = false;
            return false;

        case INPUT_KEY_PRESS:
            if (!focused) return false;

            switch (event.keysym) {
                case KEY_BACKSPACE:
                    if (cursor_pos > 0 && !text.empty()) {
                        text.erase((size_t)cursor_pos - 1, 1);
                        cursor_pos--;
                        if (on_change) on_change(text);
                    }
                    return true;

                case KEY_DELETE:
                    if ((size_t)cursor_pos < text.size()) {
                        text.erase((size_t)cursor_pos, 1);
                        if (on_change) on_change(text);
                    }
                    return true;

                case KEY_LEFT:
                    if (cursor_pos > 0) cursor_pos--;
                    return true;

                case KEY_RIGHT:
                    if ((size_t)cursor_pos < text.size()) cursor_pos++;
                    return true;

                case KEY_HOME:
                    cursor_pos = 0;
                    return true;

                case KEY_END:
                    cursor_pos = (int)text.size();
                    return true;

                case KEY_ENTER:
                    return false;  // let parent handle Enter

                default:
                    // Insert printable character
                    if (event.character >= 0x20 && event.character <= 0x7E) {
                        text.insert((size_t)cursor_pos, 1, event.character);
                        cursor_pos++;
                        if (on_change) on_change(text);
                        return true;
                    }
                    return false;
            }

        default:
            return false;
    }
}

// ============================================================================
// Scrollbar
// ============================================================================

Scrollbar::Scrollbar(bool horizontal_)
    : horizontal(horizontal_)
    , min_value(0), max_value(100), page_size(10)
    , current_value(0)
    , dragging(false), drag_start(0) {
    focusable = false;
    width = horizontal ? 200 : 16;
    height = horizontal ? 16 : 200;
}

int Scrollbar::get_thumb_size() const {
    int range = max_value - min_value;
    if (range <= 0) return horizontal ? width : height;
    int total = horizontal ? width : height;
    int size = (total * page_size) / range;
    if (size < 10) size = 10;  // minimum thumb size
    return size;
}

int Scrollbar::get_thumb_pos() const {
    int range = max_value - min_value - page_size;
    if (range <= 0) return 0;
    int total = (horizontal ? width : height) - get_thumb_size();
    int pos = (total * (current_value - min_value)) / range;
    if (pos < 0) pos = 0;
    return pos;
}

void Scrollbar::set_range(int min_val, int max_val, int page) {
    min_value = min_val;
    max_value = max_val;
    page_size = page;
    if (current_value < min_value) current_value = min_value;
    if (current_value > max_value - page_size) current_value = max_value - page_size;
}

void Scrollbar::set_value(int val) {
    if (val < min_value) val = min_value;
    if (val > max_value - page_size) val = max_value - page_size;
    if (val != current_value) {
        current_value = val;
        if (on_scroll) on_scroll(current_value);
    }
}

void Scrollbar::draw(Display& display, Font* font) {
    if (!visible) return;
    (void)font;

    int ax, ay;
    get_absolute_pos(ax, ay);

    // Draw track
    display.fill_rect(ax, ay, width, height, Color::LIGHT_GRAY);

    // Draw thumb
    int thumb_pos = get_thumb_pos();
    int thumb_size = get_thumb_size();

    if (horizontal) {
        display.fill_rect(ax + thumb_pos, ay, thumb_size, height, Color::GRAY);
    } else {
        display.fill_rect(ax, ay + thumb_pos, width, thumb_size, Color::GRAY);
    }

    // Draw border
    display.rect(ax, ay, width, height, Color::DARK_GRAY);
}

bool Scrollbar::handle_event(const InputEvent& event) {
    switch (event.type) {
        case INPUT_MOUSE_PRESS:
            if (event.button == MOUSE_LEFT) {
                int ax, ay;
                get_absolute_pos(ax, ay);
                int local_x = event.mouse_x - ax;
                int local_y = event.mouse_y - ay;

                if (horizontal) {
                    int thumb_pos = get_thumb_pos();
                    int thumb_size = get_thumb_size();
                    if (local_x >= thumb_pos && local_x < thumb_pos + thumb_size) {
                        dragging = true;
                        drag_start = local_x - thumb_pos;
                    } else {
                        // Page up/down
                        int dir = (local_x < thumb_pos) ? -page_size : page_size;
                        set_value(current_value + dir);
                    }
                } else {
                    int thumb_pos = get_thumb_pos();
                    int thumb_size = get_thumb_size();
                    if (local_y >= thumb_pos && local_y < thumb_pos + thumb_size) {
                        dragging = true;
                        drag_start = local_y - thumb_pos;
                    } else {
                        int dir = (local_y < thumb_pos) ? -page_size : page_size;
                        set_value(current_value + dir);
                    }
                }
                return true;
            }
            return false;

        case INPUT_MOUSE_RELEASE:
            if (event.button == MOUSE_LEFT) {
                dragging = false;
                return true;
            }
            return false;

        case INPUT_MOUSE_MOVE:
            if (dragging) {
                int ax, ay;
                get_absolute_pos(ax, ay);
                int total = (horizontal ? width : height) - get_thumb_size();
                int range = max_value - min_value - page_size;
                if (range <= 0 || total <= 0) return true;

                int local_pos = (horizontal ? event.mouse_x - ax - drag_start
                                             : event.mouse_y - ay - drag_start);
                int new_val = min_value + (local_pos * range) / total;
                set_value(new_val);
                return true;
            }
            return false;

        case INPUT_MOUSE_SCROLL:
            set_value(current_value + (event.scroll_delta > 0 ? -page_size / 2 : page_size / 2));
            return true;

        default:
            return false;
    }
}

// ============================================================================
// Container
// ============================================================================

Container::Container(Layout layout_)
    : layout(layout_)
    , padding(4)
    , spacing(4)
    , bg_color(Color::WHITE) {
    focusable = false;
}

void Container::add(Widget* widget) {
    widget->parent = this;
    children.emplace_back(widget);
}

void Container::remove(int widget_id) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if ((*it)->id == widget_id) {
            children.erase(it);
            return;
        }
    }
}

void Container::clear() {
    children.clear();
}

// TEACHING NOTE: Layout
// =========================================================================
// We implement two simple layouts:
//   - VERTICAL: stack children top to bottom with spacing
//   - HORIZONTAL: arrange children left to right with spacing
// Each child gets its preferred height (vertical) or width (horizontal),
// and the full available width (vertical) or height (horizontal).
// A real toolkit would have more sophisticated layout managers (flexbox,
// grid, etc.) but this is sufficient for a basic browser UI.

void Container::do_layout() {
    if (layout == LAYOUT_NONE) return;

    int inner_x = padding;
    int inner_y = padding;
    int inner_w = width - 2 * padding;
    int inner_h = height - 2 * padding;

    if (layout == LAYOUT_VERTICAL) {
        int current_y = inner_y;
        for (auto& child : children) {
            if (!child->visible) continue;
            child->x = inner_x;
            child->y = current_y;
            child->width = inner_w;
            current_y += child->height + spacing;
        }
    } else if (layout == LAYOUT_HORIZONTAL) {
        int current_x = inner_x;
        for (auto& child : children) {
            if (!child->visible) continue;
            child->x = current_x;
            child->y = inner_y;
            child->height = inner_h;
            current_x += child->width + spacing;
        }
    }

    // Recursively layout child containers
    for (auto& child : children) {
        Container* cont = dynamic_cast<Container*>(child.get());
        if (cont) cont->do_layout();
    }
}

void Container::draw_clipped(Display& display, Font* font, int clip_x, int clip_y, int clip_w, int clip_h) {
    (void)clip_x; (void)clip_y; (void)clip_w; (void)clip_h;

    int ax, ay;
    get_absolute_pos(ax, ay);

    // Draw background
    display.fill_rect(ax, ay, width, height, bg_color);

    // Draw children
    for (auto& child : children) {
        if (child->visible) {
            child->draw(display, font);
        }
    }
}

void Container::draw(Display& display, Font* font) {
    if (!visible) return;
    draw_clipped(display, font, 0, 0, display.get_width(), display.get_height());
}

bool Container::handle_event(const InputEvent& event) {
    // TEACHING NOTE: Event dispatch in containers
    // =================================================================
    // When an event arrives, the container must determine which child
    // should receive it. For mouse events, we do hit testing: find the
    // topmost child whose bounds contain the mouse position.
    // For keyboard events, they go to the focused widget.
    // If no child handles the event, the container itself may handle it.

    // Convert absolute mouse coords to local
    int local_x = event.mouse_x;
    int local_y = event.mouse_y;

    // Try to find a child that handles the event
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (!(*it)->visible) continue;
        if ((*it)->hit_test(local_x, local_y) || event.type == INPUT_KEY_PRESS || event.type == INPUT_KEY_RELEASE) {
            if ((*it)->handle_event(event)) {
                return true;
            }
        }
    }

    return false;
}

Widget* Container::find_widget_at(int mx, int my) {
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (!(*it)->visible) continue;
        if ((*it)->hit_test(mx, my)) {
            Container* cont = dynamic_cast<Container*>(it->get());
            if (cont) {
                Widget* found = cont->find_widget_at(mx, my);
                if (found) return found;
            }
            return it->get();
        }
    }
    return nullptr;
}

// ============================================================================
// Window
// ============================================================================

Window::Window(const std::string& title_)
    : title(title_)
    , bg_color(Color::WHITE)
    , title_bar_color(Color(60, 60, 140))
    , title_bar_height(24) {
    focusable = false;
    content.parent = this;
    content.bg_color = bg_color;
}

void Window::draw(Display& display, Font* font) {
    if (!visible) return;

    int ax = x;
    int ay = y;

    // Draw title bar
    display.fill_rect(ax, ay, width, title_bar_height, title_bar_color);

    // Draw title text
    if (font && font->is_loaded() && !title.empty()) {
        Font::TextLayout layout = font->rasterize_text(title, 14);
        if (!layout.bitmap.empty()) {
            int text_x = ax + 8;
            int text_y = ay + (title_bar_height - layout.bitmap.height) / 2;

            for (int row = 0; row < layout.bitmap.height; ++row) {
                for (int col = 0; col < layout.bitmap.width; ++col) {
                    uint8_t alpha = layout.bitmap.pixels[(size_t)row * layout.bitmap.width + col];
                    if (alpha > 128) {
                        display.put_pixel(text_x + col, text_y + row, Color::WHITE);
                    }
                }
            }
        }
    }

    // Draw content area
    content.x = 0;
    content.y = title_bar_height;
    content.width = width;
    content.height = height - title_bar_height;
    content.draw(display, font);
}

bool Window::handle_event(const InputEvent& event) {
    // Check if event is in title bar or content area
    if (event.type == INPUT_MOUSE_PRESS || event.type == INPUT_MOUSE_RELEASE ||
        event.type == INPUT_MOUSE_MOVE || event.type == INPUT_MOUSE_SCROLL) {
        // Mouse event - delegate to content area
        return content.handle_event(event);
    } else {
        // Keyboard event - delegate to content
        return content.handle_event(event);
    }
}

// ============================================================================
// GUI
// ============================================================================

GUI::GUI()
    : m_running(false)
    , m_need_redraw(true)
    , m_focused(nullptr)
    , m_hovered(nullptr) {}

GUI::~GUI() {
    shutdown();
}

bool GUI::init(DisplayBackend backend, int width, int height) {
    // Initialize display
    if (!m_display.init(backend, width, height)) {
        return false;
    }

    // Initialize input
    if (backend == DisplayBackend::FRAMEBUFFER) {
        m_input.init_framebuffer(m_display.get_width(), m_display.get_height());
    } else {
        m_input.init_x11();
    }

    // Load font
    std::string font_path = Font::find_font("DejaVuSans");
    if (font_path.empty()) {
        font_path = Font::find_font();
    }
    if (!font_path.empty()) {
        m_font.load(font_path);
    }

    // Set up root window
    m_root.x = 0;
    m_root.y = 0;
    m_root.width = m_display.get_width();
    m_root.height = m_display.get_height();
    m_root.content.x = 0;
    m_root.content.y = m_root.title_bar_height;
    m_root.content.width = m_root.width;
    m_root.content.height = m_root.height - m_root.title_bar_height;

    return true;
}

void GUI::shutdown() {
    m_display.shutdown();
    m_input.shutdown();
}

// TEACHING NOTE: Event loop
// =========================================================================
// The event loop is the core of the GUI system. Each iteration:
//   1. Poll for input events (non-blocking)
//   2. For X11 mode: also process X11 display events
//   3. Dispatch events to the appropriate widget
//   4. If anything changed, redraw the display
//   5. Flip the display buffer (show the drawn frame)
//
// In X11 mode, we need to handle both X11 events (from the window) and
// converted InputEvents. In framebuffer mode, we just poll evdev.
//
// The loop runs at the display refresh rate (typically 60 FPS). We do
// not need explicit vsync because the framebuffer flip handles it,
// and in X11 mode the server composites at its own rate.

void GUI::run() {
    m_running = true;

    while (m_running) {
        // Process display events (X11 mode: expose, close)
        if (!m_display.process_events()) {
            m_running = false;
            break;
        }

        // Poll input
        if (m_display.get_backend() == DisplayBackend::FRAMEBUFFER) {
            InputEvent event;
            while (m_input.poll(event)) {
                dispatch_event(event);
                m_need_redraw = true;
            }
        } else {
            // X11 mode: read X11 events and convert
            if (m_display.get_backend() == DisplayBackend::X11) {
                X11Connection* conn = static_cast<X11Connection*>(m_display.m_x11_conn);
                X11Window* win = static_cast<X11Window*>(m_display.m_x11_window);
                if (conn && win) {
                    X11Event xevent;
                    while (win->next_event(conn, &xevent)) {
                        InputEvent event;
                        if (m_input.convert_x11_event(&xevent, event)) {
                            dispatch_event(event);
                            m_need_redraw = true;
                        }
                    }
                }
            }
        }

        // Draw if needed
        if (m_need_redraw) {
            draw_all();
            m_display.flip();
            m_need_redraw = false;
        }

        // Small sleep to avoid 100% CPU
        usleep(1000);  // 1ms
    }
}

void GUI::dispatch_event(const InputEvent& event) {
    // TEACHING NOTE: Event dispatch
    // =================================================================
    // Mouse events: find the widget under the mouse (hit test from
    // topmost to bottommost) and deliver the event. If the widget
    // handles it, stop.
    // Keyboard events: deliver to the focused widget.

    if (event.type == INPUT_KEY_PRESS || event.type == INPUT_KEY_RELEASE) {
        if (m_focused) {
            if (m_focused->handle_event(event)) {
                return;
            }
        }
        // Fall through to root window
        m_root.handle_event(event);
    } else {
        // Mouse event - convert to local coordinates
        // The root window covers the entire display
        int mx = event.mouse_x;
        int my = event.mouse_y;

        InputEvent local_event = event;
        local_event.mouse_x = mx;
        local_event.mouse_y = my;

        if (m_root.hit_test(mx, my)) {
            m_root.handle_event(local_event);
        }
    }
}

void GUI::draw_all() {
    m_root.draw(m_display, &m_font);
}

} // namespace chinstrap