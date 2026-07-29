// =========================================================================
// layout.hpp - Layout Engine
// =========================================================================
// TEACHING NOTE: The layout engine takes the DOM tree (with computed
// styles) and computes the position and size of every element. This is
// one of the most complex parts of a browser engine.
//
// The layout process is also called "reflow". When you resize the
// browser window, or when JavaScript changes the DOM, the browser
// re-runs layout to update positions and sizes.
//
// CSS defines two main formatting contexts:
//   1. Block formatting context (BFC) - elements stack vertically
//      (div, p, h1, section, etc.)
//   2. Inline formatting context (IFC) - elements flow horizontally
//      (span, a, em, text nodes)
//
// We implement:
//   - Block layout (vertical stacking of block elements)
//   - Inline layout (horizontal flow of inline elements and text)
//   - Basic text wrapping (break long lines at the viewport width)
//   - Box model: content, padding, border, margin
//   - Width/height from computed styles
//   - Basic text alignment
//
// What we do NOT implement:
//   - Flexbox (display: flex)
//   - CSS Grid (display: grid)
//   - Floats (float: left/right)
//   - Absolute positioning (position: absolute/fixed)
//   - Transforms
//   - Overflow / scrolling
//   - Table layout
//
// TEACHING NOTE: How Chrome does layout:
// Chrome uses a layout engine called "LayoutNG" (Next Generation).
// It replaced the old layout engine in 2019. LayoutNG represents
// the layout as a tree of LayoutBox objects, each with a position
// and size. It processes the tree in a single pass, computing
// positions top-to-bottom, left-to-right.
//
// The box model is fundamental:
//   +---------------------------+
//   |         margin            |
//   |  +---------------------+  |
//   |  |      border         |  |
//   |  |  +---------------+  |  |
//   |  |  |    padding    |  |  |
//   |  |  |  +---------+  |  |  |
//   |  |  |  | content  |  |  |  |
//   |  |  |  +---------+  |  |  |
//   |  |  +---------------+  |  |
//   |  +---------------------+  |
//   +---------------------------+
//
// The content area is where text and child elements go.
// Padding is inside the border, margin is outside.
// =========================================================================

#ifndef CHINSTRAP_LAYOUT_HPP
#define CHINSTRAP_LAYOUT_HPP

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace chinstrap {

// Forward declarations
class Node;

// -------------------------------------------------------------------------
// Box - a laid-out element with position and size
// -------------------------------------------------------------------------
// TEACHING NOTE: After layout, every element gets a Box with:
//   - x, y: position relative to the parent (or viewport for root)
//   - width, height: content area dimensions
//   - margin, padding, border: box model values
//   - children: child boxes (for elements with children)
//
// The box tree mirrors the DOM tree but only includes visible elements
// (not comments, scripts, etc.) and has computed geometry.
// -------------------------------------------------------------------------

struct BoxEdges {
    float top = 0, right = 0, bottom = 0, left = 0;
};

struct Box {
    // Position of the content area (relative to parent)
    float x = 0;
    float y = 0;

    // Content area dimensions
    float width = 0;
    float height = 0;

    // Box model
    BoxEdges margin;
    BoxEdges border;
    BoxEdges padding;

    // The DOM node this box corresponds to
    const Node* node = nullptr;

    // Child boxes
    std::vector<std::unique_ptr<Box>> children;

    // Box type: block or inline
    // TEACHING NOTE: We distinguish block and inline boxes because they
    // participate in different formatting contexts. Block boxes stack
    // vertically; inline boxes flow horizontally.
    enum class Type { Block, Inline, Text };
    Type type = Type::Block;

    // Text content (for text boxes)
    std::string text;

    // Computed style values (convenience accessors)
    std::string get_style(const std::string& prop) const;

    // Get the total width including margin, border, padding
    float outer_width() const {
        return width + margin.left + margin.right + border.left + border.right + padding.left + padding.right;
    }
    float outer_height() const {
        return height + margin.top + margin.bottom + border.top + border.bottom + padding.top + padding.bottom;
    }

    // Get the x position of the content area (after margin + border + padding)
    float content_x() const {
        return x + margin.left + border.left + padding.left;
    }
    float content_y() const {
        return y + margin.top + border.top + padding.top;
    }
};

// -------------------------------------------------------------------------
// LayoutEngine - computes box positions from DOM + styles
// -------------------------------------------------------------------------

class LayoutEngine {
public:
    // Layout a DOM tree into a box tree.
    // viewport_width and viewport_height are the visible area dimensions.
    // TEACHING NOTE: The viewport is the visible area of the page. On a
    // framebuffer display, this is the screen resolution. In a windowed
    // browser, it is the window size. Layout must respect the viewport
    // width (for text wrapping) and height.
    std::unique_ptr<Box> layout(const Node& root, float viewport_width, float viewport_height);

private:
    float viewport_width_ = 800;
    float viewport_height_ = 600;

    // Layout a single node and its descendants
    std::unique_ptr<Box> layout_node(const Node& node, float x, float y, float available_width);

    // Layout block children (stack vertically)
    void layout_block_children(Box& parent_box, const Node& parent_node);

    // Layout inline children (flow horizontally with wrapping)
    void layout_inline_children(Box& parent_box, const Node& parent_node, float available_width);

    // Layout inline content within a block (word wrapping)
    void layout_inline_content(Box& parent_box, const Node& parent_node, float available_width);

    // Parse a CSS length value (px, em, %, etc.)
    // TEACHING NOTE: CSS length values can be in different units:
    //   - px: pixels (device-dependent)
    //   - em: relative to parent font-size
    //   - rem: relative to root font-size
    //   - %: relative to parent dimension
    //   - vh/vw: relative to viewport
    // We only support px and % for simplicity.
    float parse_length(const std::string& value, float reference, float default_value) const;

    // Parse box edges (margin, padding, border)
    BoxEdges parse_edges(const std::string& top, const std::string& right,
                          const std::string& bottom, const std::string& left,
                          float reference) const;

    // Check if a node is a block-level element
    bool is_block(const Node& node) const;

    // Check if a node is inline
    bool is_inline(const Node& node) const;

    // Get the display property from computed style
    std::string get_display(const Node& node) const;

    // Estimate text width (very basic)
    // TEACHING NOTE: Real browsers use a complex text shaping engine
    // (HarfBuzz in Chrome) to compute text width. This involves font
    // metrics, kerning, ligatures, and complex script handling. We
    // use a simple character count times average character width.
    float text_width(const std::string& text) const;
    float text_width(const std::string& text, float font_size) const;

    // Estimate text height (line height)
    float line_height() const { return 20.0f; }  // Simple default
};

} // namespace chinstrap

#endif // CHINSTRAP_LAYOUT_HPP