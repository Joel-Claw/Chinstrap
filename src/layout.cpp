// =========================================================================
// layout.cpp - Layout Engine Implementation
// =========================================================================
// TEACHING NOTE: This file implements the layout engine. Layout is the
// process of computing positions and sizes for every element based on
// the DOM tree and computed CSS styles.
//
// We implement a simplified block layout algorithm:
//   1. Start at the top of the viewport (y = 0)
//   2. For each block element, place it below the previous one
//   3. Inside a block, lay out children recursively
//   4. For inline content, flow left to right with wrapping
//   5. Compute heights from content (or use specified height)
//
// The key insight is that layout is recursive: each block lays out its
// children within its content area. The children can themselves be
// blocks (which stack vertically) or inline (which flow horizontally).
// =========================================================================

#include "layout.hpp"
#include "html_parser.hpp"
#include "css_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <cmath>

namespace chinstrap {

// =========================================================================
// Box methods
// =========================================================================

std::string Box::get_style(const std::string& prop) const {
    if (!node) return "";
    // Use the StyleEngine to get computed style
    return chinstrap::StyleEngine::get_computed_style(*node)[prop];
}

// =========================================================================
// LayoutEngine implementation
// =========================================================================

std::unique_ptr<Box> LayoutEngine::layout(const Node& root, float vw, float vh) {
    viewport_width_ = vw;
    viewport_height_ = vh;
    return layout_node(root, 0, 0, vw);
}

float LayoutEngine::parse_length(const std::string& value, float reference, float default_value) const {
    // TEACHING NOTE: CSS length parsing. We support:
    //   - "123px" -> 123 pixels
    //   - "50%"   -> 50% of reference
    //   - "auto"  -> default_value (let the layout algorithm decide)
    //   - "123"   -> 123 pixels (unitless, treated as px)
    //
    // We do NOT support em, rem, vh, vw, or other units. This is a
    // significant simplification but is fine for our educational browser.

    if (value.empty() || value == "auto" || value == "inherit" || value == "initial") {
        return default_value;
    }

    // Find the numeric part
    std::string num_str;
    std::string unit;
    std::size_t i = 0;
    while (i < value.size() && (std::isdigit(static_cast<unsigned char>(value[i])) || value[i] == '.' || value[i] == '-')) {
        num_str += value[i];
        i++;
    }
    unit = value.substr(i);

    // Trim whitespace from unit
    while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.front()))) {
        unit.erase(unit.begin());
    }

    if (num_str.empty()) return default_value;

    try {
        float num = std::stof(num_str);
        if (unit == "%" || unit == "percent") {
            return (num / 100.0f) * reference;
        }
        // px, or unitless, or any other unit we treat as pixels
        return num;
    } catch (...) {
        return default_value;
    }
}

BoxEdges LayoutEngine::parse_edges(const std::string& top, const std::string& right,
                                     const std::string& bottom, const std::string& left,
                                     float reference) const {
    BoxEdges edges;
    edges.top = parse_length(top, reference, 0);
    edges.right = parse_length(right, reference, 0);
    edges.bottom = parse_length(bottom, reference, 0);
    edges.left = parse_length(left, reference, 0);
    return edges;
}

std::string LayoutEngine::get_display(const Node& node) const {
    if (node.type != NodeType::Element) return "inline";
    // Use computed style
    ComputedStyle style = StyleEngine::get_computed_style(node);
    auto it = style.find("display");
    if (it != style.end()) return it->second;

    // Default display based on tag
    // TEACHING NOTE: HTML elements have default display values.
    // Block elements: div, p, h1-h6, section, article, ul, ol, table, etc.
    // Inline elements: span, a, em, strong, code, img, etc.
    // This is defined in the HTML spec and the user agent stylesheet.
    static const std::vector<std::string> block_tags = {
        "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "aside", "header", "footer", "nav",
        "ul", "ol", "li", "table", "tr", "td", "th",
        "blockquote", "pre", "hr", "form", "fieldset",
        "address", "figure", "figcaption", "main", "details", "summary"
    };
    if (std::find(block_tags.begin(), block_tags.end(), node.tag_name) != block_tags.end()) {
        return "block";
    }
    return "inline";
}

bool LayoutEngine::is_block(const Node& node) const {
    return get_display(node) == "block";
}

bool LayoutEngine::is_inline(const Node& node) const {
    return get_display(node) == "inline" || get_display(node) == "inline-block";
}

float LayoutEngine::text_width(const std::string& text) const {
    // TEACHING NOTE: This is a very rough estimate. Real browsers use
    // the font metrics of the actual font being used. We assume an
    // average character width of 8 pixels (for a 16px font).
    // This affects text wrapping accuracy.
    return static_cast<float>(text.size()) * 8.0f;
}

std::unique_ptr<Box> LayoutEngine::layout_node(const Node& node, float x, float y, float available_width) {
    auto box = std::make_unique<Box>();
    box->node = &node;
    box->x = x;
    box->y = y;

    if (node.type == NodeType::Text) {
        // Text node
        box->type = Box::Type::Text;
        box->text = node.text_content;
        box->width = available_width;
        box->height = line_height();
        return box;
    }

    if (node.type == NodeType::Comment || node.type == NodeType::Document) {
        // Comments and document nodes do not produce visible boxes
        // but we recurse into their children for document nodes
        box->width = available_width;
        box->height = 0;
        if (node.type == NodeType::Document) {
            layout_block_children(*box, node);
        }
        return box;
    }

    // Element node
    ComputedStyle style = StyleEngine::get_computed_style(node);

    // Determine if block or inline
    std::string display = get_display(node);
    box->type = (display == "block") ? Box::Type::Block : Box::Type::Inline;

    // Parse box model values
    // TEACHING NOTE: We read margin, padding, and border from the
    // computed style. Each can be specified as 1-4 values (top right
    // bottom left). We simplify by reading each edge separately.
    box->margin = parse_edges(
        style.count("margin-top") ? style.at("margin-top") : "0",
        style.count("margin-right") ? style.at("margin-right") : "0",
        style.count("margin-bottom") ? style.at("margin-bottom") : "0",
        style.count("margin-left") ? style.at("margin-left") : "0",
        available_width
    );

    box->padding = parse_edges(
        style.count("padding-top") ? style.at("padding-top") : "0",
        style.count("padding-right") ? style.at("padding-right") : "0",
        style.count("padding-bottom") ? style.at("padding-bottom") : "0",
        style.count("padding-left") ? style.at("padding-left") : "0",
        available_width
    );

    // Parse border width from the border shorthand
    // TEACHING NOTE: Border values look like "1px solid black".
    // We extract the pixel width. If no width is found, default to 1px.
    auto get_border_width = [&](const std::string& border_val) -> float {
        if (border_val.empty() || border_val == "none" || border_val == "hidden") return 0;
        std::istringstream ss(border_val);
        std::string token;
        while (ss >> token) {
            if (token.find("px") != std::string::npos || std::isdigit(static_cast<unsigned char>(token[0]))) {
                return parse_length(token, available_width, 0);
            }
        }
        return 1.0f;
    };
    (void)get_border_width;  // Will be used when border-width property is fully supported

    box->border = parse_edges(
        style.count("border-top") ? style.at("border-top") : "",
        style.count("border-right") ? style.at("border-right") : "",
        style.count("border-bottom") ? style.at("border-bottom") : "",
        style.count("border-left") ? style.at("border-left") : "",
        available_width
    );

    // Parse width
    // TEACHING NOTE: If width is specified, use it. Otherwise, block
    // elements default to 100% of available width, inline elements
    // size to their content.
    if (style.count("width") && style.at("width") != "auto") {
        box->width = parse_length(style.at("width"), available_width, available_width);
    } else if (box->type == Box::Type::Block) {
        box->width = available_width - box->margin.left - box->margin.right
                     - box->border.left - box->border.right
                     - box->padding.left - box->padding.right;
    } else {
        box->width = available_width;  // Inline: will be adjusted by content
    }

    // Parse height
    if (style.count("height") && style.at("height") != "auto") {
        box->height = parse_length(style.at("height"), viewport_height_, 0);
    }

    // Layout children
    // TEACHING NOTE: Block children stack vertically inside the content
    // area. Inline children flow horizontally. We determine the
    // formatting context from the display property.
    float content_width = box->width;

    if (box->type == Box::Type::Block) {
        layout_block_children(*box, node);
    } else {
        layout_inline_children(*box, node, content_width);
    }

    // If height was not specified, compute from children
    if (!style.count("height") || style.at("height") == "auto") {
        float max_bottom = 0;
        for (const auto& child : box->children) {
            float child_bottom = child->y + child->outer_height();
            if (child_bottom > max_bottom) max_bottom = child_bottom;
        }
        box->height = max_bottom + box->padding.top + box->padding.bottom;
    }

    return box;
}

void LayoutEngine::layout_block_children(Box& parent_box, const Node& parent_node) {
    // TEACHING NOTE: Block children stack vertically. Each child is
    // placed below the previous one. The available width is the
    // parent content width.
    float current_y = parent_box.padding.top;
    float content_width = parent_box.width;
    float content_x = parent_box.padding.left;

    for (const auto& child : parent_node.children) {
        if (child->type == NodeType::Comment) continue;

        // Skip whitespace-only text nodes between block elements
        if (child->type == NodeType::Text) {
            bool all_space = true;
            for (char c : child->text_content) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    all_space = false;
                    break;
                }
            }
            if (all_space) continue;
        }

        std::unique_ptr<Box> child_box = layout_node(*child, content_x, current_y, content_width);
        current_y += child_box->outer_height();
        parent_box.children.push_back(std::move(child_box));
    }
}

void LayoutEngine::layout_inline_children(Box& parent_box, const Node& parent_node, float available_width) {
    // TEACHING NOTE: Inline children flow horizontally. When a line
    // is full, we wrap to the next line. This is the most complex part
    // of layout. We simplify by treating all inline content as text
    // and wrapping at word boundaries.
    //
    // Real browsers handle inline layout with a "line box" abstraction:
    //   - Inline boxes are placed into line boxes
    //   - Line boxes are stacked vertically
    //   - Text is broken into "runs" (segments with the same style)
    //   - Word wrapping happens at the line box boundary
    //
    // We simplify: we collect all text content and wrap it as a single
    // text block. This does not handle mixed inline elements correctly
    // (e.g., <p>text <b>bold</b> more text</p>) but handles the common
    // case of paragraphs with text.

    float current_y = parent_box.padding.top;
    float current_x = parent_box.padding.left;
    float max_width = available_width - parent_box.padding.left - parent_box.padding.right;

    // Collect text content and wrap
    std::string text = parent_node.text();

    // Simple word wrapping
    // TEACHING NOTE: We split text into words and place them one by one.
    // When a word does not fit on the current line, we wrap to the next
    // line. This is a greedy algorithm and does not handle hyphenation
    // or CJK text (which does not use spaces for word boundaries).
    std::istringstream words(text);
    std::string word;
    float line_height = this->line_height();

    while (words >> word) {
        float word_width = text_width(word);
        float space_width = text_width(" ");

        if (current_x + word_width > max_width && current_x > parent_box.padding.left) {
            // Wrap to next line
            current_y += line_height;
            current_x = parent_box.padding.left;
        }

        // Create a text box for this word
        auto word_box = std::make_unique<Box>();
        word_box->type = Box::Type::Text;
        word_box->node = &parent_node;
        word_box->text = word;
        word_box->x = current_x;
        word_box->y = current_y;
        word_box->width = word_width;
        word_box->height = line_height;
        parent_box.children.push_back(std::move(word_box));

        current_x += word_width + space_width;
    }

    // Update parent height
    if (current_y + line_height > parent_box.height) {
        parent_box.height = current_y + line_height + parent_box.padding.bottom;
    }
}

} // namespace chinstrap