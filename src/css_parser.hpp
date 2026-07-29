// =========================================================================
// css_parser.hpp - CSS Parser and Style Engine
// =========================================================================
// TEACHING NOTE: CSS (Cascading Style Sheets) tells the browser how to
// present HTML. Without CSS, web pages would be plain text with no
// colors, fonts, layout, or animation. CSS is a separate language from
// HTML, with its own parser.
//
// CSS has three main concepts:
//   1. Selectors - patterns that match HTML elements
//   2. Declarations - property: value pairs (e.g., color: red)
//   3. Cascade - rules for resolving conflicts when multiple selectors
//      match the same element
//
// The cascade is what makes CSS powerful and complex. When multiple
// rules apply to an element, the browser uses specificity to determine
// which rule wins. Specificity is calculated from the selector:
//   - ID selectors (#myid) have high specificity
//   - Class selectors (.myclass) have medium specificity
//   - Type selectors (div, p) have low specificity
//   - Universal selector (*) has zero specificity
//
// We implement:
//   - Stylesheet parsing (rules, selectors, declarations)
//   - Selectors: type, class, ID, descendant, child, universal
//   - Specificity calculation
//   - Cascade: sort matching rules by specificity, apply highest
//   - Computed style: resolve inherited values
//
// What we do NOT implement:
//   - Pseudo-classes (:hover, :focus, :nth-child)
//   - Pseudo-elements (::before, ::after)
//   - Attribute selectors ([type="text"])
//   - Media queries (@media)
//   - CSS variables (var())
//   - Animations / transitions
//   - Flexbox / Grid layout
//
// TEACHING NOTE: How Chrome handles CSS:
// Chrome has a CSS parser (part of Blink) that tokenizes CSS and builds
// style rules. It then has a style resolver that matches selectors against
// DOM elements and computes styles. Chrome uses a rule set optimization
// where it indexes rules by tag name, class, and ID for fast matching.
// The style resolver produces a "ComputedStyle" for each element, which
// is the final set of property values after the cascade.
// =========================================================================

#ifndef CHINSTRAP_CSS_PARSER_HPP
#define CHINSTRAP_CSS_PARSER_HPP

#include <string>
#include <vector>
#include <map>
#include <memory>

// Forward declaration of Node from html_parser.hpp
namespace chinstrap {
class Node;
}

namespace chinstrap {

// -------------------------------------------------------------------------
// Selector - matches DOM elements
// -------------------------------------------------------------------------
// TEACHING NOTE: A CSS selector is a pattern that matches elements in
// the DOM tree. The simplest selectors are:
//   *     - universal (matches everything)
//   div   - type selector (matches <div> elements)
//   .cls  - class selector (matches elements with class "cls")
//   #id   - ID selector (matches the element with id "id")
//
// Combinators connect selectors:
//   A B   - descendant (B is a descendant of A)
//   A > B - child (B is a direct child of A)
//
// We represent a selector as a list of "simple selectors" connected by
// combinators. Each simple selector has a tag name, optional class, and
// optional ID. The combinator describes how this selector relates to
// the previous one.
// -------------------------------------------------------------------------

enum class Combinator {
    Descendant,  // " " (space) - any descendant
    Child,       // ">" - direct child
    None,        // First in the chain
};

struct SimpleSelector {
    std::string tag_name;      // empty = universal/type wildcard
    std::string id;             // empty = no ID constraint
    std::vector<std::string> classes;
};

struct Selector {
    std::vector<SimpleSelector> parts;
    std::vector<Combinator> combinators;  // combinators[i] connects parts[i] to parts[i+1]

    // Compute specificity as (a, b, c) where:
    //   a = number of ID selectors
    //   b = number of class selectors
    //   c = number of type selectors
    // TEACHING NOTE: Specificity is how the cascade resolves conflicts.
    // A rule with specificity (1,0,0) beats (0,1,0) which beats (0,0,1).
    // We compare specificity as a tuple: (a, b, c).
    // If two rules have the same specificity, the later one wins.
    void specificity(int& a, int& b, int& c) const;

    // Check if this selector matches a specific node
    // (Does not handle combinators; use matches_with_ancestors for full matching)
    bool matches_simple(const Node& node) const;
};

// -------------------------------------------------------------------------
// Declaration - a single property: value pair
// -------------------------------------------------------------------------

struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
};

// -------------------------------------------------------------------------
// StyleRule - a selector + declarations
// -------------------------------------------------------------------------

struct StyleRule {
    Selector selector;
    std::vector<Declaration> declarations;
};

// -------------------------------------------------------------------------
// Stylesheet - a collection of rules
// -------------------------------------------------------------------------

struct Stylesheet {
    std::vector<StyleRule> rules;
};

// -------------------------------------------------------------------------
// ComputedStyle - final resolved styles for an element
// -------------------------------------------------------------------------
// TEACHING NOTE: After the cascade resolves all matching rules, each
// element gets a "computed style" - the final value for every property.
// Some properties are inherited (like font-size, color), meaning they
// are passed from parent to child. Others are not (like border, margin).
//
// We store computed styles as a map of property -> value. We also handle
// inheritance: if a property is not set on an element, we check its parent.
// -------------------------------------------------------------------------

using ComputedStyle = std::map<std::string, std::string>;

// Properties that are inherited from parent to child
const std::vector<std::string>& inherited_properties();

// -------------------------------------------------------------------------
// CssParser - CSS parser
// -------------------------------------------------------------------------

class CssParser {
public:
    explicit CssParser(std::string input);

    // Parse a CSS stylesheet
    Stylesheet parse();

    // Parse a declaration block (for inline styles)
    // TEACHING NOTE: This is public so StyleEngine can use it to parse
    // inline style attributes like style="color: red; font-size: 16px".
    std::vector<Declaration> parse_declarations() {
        return parse_declarations_internal();
    }

private:
    std::string input_;
    std::size_t pos_ = 0;

    // Parsing methods
    void skip_whitespace_and_comments();
    Stylesheet parse_stylesheet();
    StyleRule parse_rule();
    Selector parse_selector();
    std::vector<Declaration> parse_declarations_internal();
    Declaration parse_declaration();
    SimpleSelector parse_simple_selector();

    // Helpers
    char peek(std::size_t offset = 0) const;
    char advance();
    bool at_end() const { return pos_ >= input_.size(); }
    bool match(char expected);
    bool starts_with(const std::string& s);
    std::string parse_identifier();
    std::string parse_value();
};

// -------------------------------------------------------------------------
// StyleEngine - applies CSS to the DOM
// -------------------------------------------------------------------------

class StyleEngine {
public:
    // Apply a stylesheet to a DOM tree, computing styles for each element.
    // TEACHING NOTE: This walks the DOM tree, and for each element, finds
    // all matching rules from the stylesheet, sorts them by specificity,
    // and applies them in order (lowest specificity first, so highest
    // specificity overrides). Inherited properties are resolved from
    // the parent.
    static void apply_styles(const Stylesheet& sheet, Node& root);

    // Get the computed style for a node
    // TEACHING NOTE: We store computed styles in the node's attributes
    // under a special key. This is a simplification - real browsers store
    // computed styles in a separate data structure attached to each node.
    static ComputedStyle get_computed_style(const Node& node);

private:
    // Check if a full selector (with combinators) matches a node
    // TEACHING NOTE: Selector matching with combinators is recursive.
    // For a selector "A > B", we check if the node matches B, then check
    // if its parent matches A. For "A B" (descendant), we check if any
    // ancestor matches A.
    static bool matches_selector(const Selector& selector, const Node& node);
    static bool matches_descendant(const SimpleSelector& ancestor_selector, const Node& node);
    static bool matches_child(const SimpleSelector& parent_selector, const Node& node);
};

} // namespace chinstrap

#endif // CHINSTRAP_CSS_PARSER_HPP