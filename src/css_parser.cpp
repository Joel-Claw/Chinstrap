// =========================================================================
// css_parser.cpp - CSS Parser Implementation
// =========================================================================
// TEACHING NOTE: CSS parsing is simpler than HTML parsing. CSS has a
// regular grammar with clear delimiters: braces for rules, semicolons
// for declarations, colons for property-value pairs, and commas for
// selector lists. The main complexity is in selector parsing and the
// cascade algorithm.
//
// The CSS grammar (simplified):
//   stylesheet    = [ rule ]*
//   rule         = selector_list '{' declaration_list '}'
//   selector_list = selector [ ',' selector ]*
//   declaration   = property ':' value [ ';' ]
//   selector      = simple_selector [ combinator simple_selector ]*
//   simple_selector = [ tag_name ] [ '.' class ]* [ '#' id ]
//
// We parse this with recursive descent, same as JSON.
// =========================================================================

#include "css_parser.hpp"
#include "html_parser.hpp"

#include <cctype>
#include <algorithm>
#include <sstream>
#include <functional>

namespace chinstrap {

// =========================================================================
// Selector implementation
// =========================================================================

void Selector::specificity(int& a, int& b, int& c) const {
    // TEACHING NOTE: Specificity is a tuple (a, b, c) where:
    //   a = count of ID selectors
    //   b = count of class selectors
    //   c = count of type selectors
    //
    // Comparison is lexicographic: (1,0,0) > (0,5,0) > (0,0,10).
    // This means a single ID selector beats any number of class selectors,
    // and a single class selector beats any number of type selectors.
    //
    // We compute this by iterating over all simple selectors in the
    // compound selector and counting their components.
    a = 0; b = 0; c = 0;
    for (const auto& part : parts) {
        if (!part.id.empty()) a++;
        b += static_cast<int>(part.classes.size());
        if (!part.tag_name.empty() && part.tag_name != "*") c++;
    }
}

bool Selector::matches_simple(const Node& node) const {
    // TEACHING NOTE: This checks if the LAST simple selector in the
    // chain matches the node. The full matching (with combinators)
    // is done by matches_selector in StyleEngine.
    //
    // For a simple selector "div.cls#id":
    //   - node must be an element with tag "div"
    //   - node must have class "cls"
    //   - node must have id "id"

    if (parts.empty()) return false;
    const SimpleSelector& last = parts.back();

    if (node.type != NodeType::Element) return false;

    // Check tag name (if specified and not universal)
    if (!last.tag_name.empty() && last.tag_name != "*") {
        if (node.tag_name != last.tag_name) return false;
    }

    // Check ID (if specified)
    if (!last.id.empty()) {
        if (node.get_attribute("id") != last.id) return false;
    }

    // Check classes
    for (const auto& cls : last.classes) {
        if (!node.has_class(cls)) return false;
    }

    return true;
}

// =========================================================================
// Inherited properties
// =========================================================================

const std::vector<std::string>& inherited_properties() {
    // TEACHING NOTE: In CSS, some properties are inherited from parent
    // to child elements. For example, if you set color: red on <body>,
    // all text in the body will be red unless overridden. The inherited
    // properties include:
    //   - color, font-family, font-size, font-weight, line-height
    //   - text-align, text-indent, letter-spacing, word-spacing
    //   - visibility, cursor, direction, white-space
    //
    // Non-inherited properties include:
    //   - margin, padding, border, background
    //   - width, height, position, display
    //   - float, clear, overflow
    static const std::vector<std::string> inherited = {
        "color", "font-family", "font-size", "font-weight",
        "font-style", "line-height", "text-align", "text-indent",
        "letter-spacing", "word-spacing", "visibility", "cursor",
        "direction", "white-space", "text-decoration"
    };
    return inherited;
}

// =========================================================================
// CssParser implementation
// =========================================================================

CssParser::CssParser(std::string input) : input_(std::move(input)) {}

char CssParser::peek(std::size_t offset) const {
    if (pos_ + offset >= input_.size()) return '\0';
    return input_[pos_ + offset];
}

char CssParser::advance() {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_++];
}

bool CssParser::match(char expected) {
    if (at_end() || input_[pos_] != expected) return false;
    pos_++;
    return true;
}

bool CssParser::starts_with(const std::string& s) {
    if (pos_ + s.size() > input_.size()) return false;
    return input_.compare(pos_, s.size(), s) == 0;
}

void CssParser::skip_whitespace_and_comments() {
    // TEACHING NOTE: CSS comments are /* ... */. We skip them along
    // with whitespace. Real CSS parsers also skip CDO/CDC tokens
    // (<!-- and -->) for historical reasons.
    while (!at_end()) {
        if (std::isspace(static_cast<unsigned char>(peek()))) {
            pos_++;
        } else if (starts_with("/*")) {
            // Skip comment
            pos_ += 2;
            while (!at_end() && !starts_with("*/")) {
                pos_++;
            }
            if (!at_end()) pos_ += 2;  // Skip */
        } else {
            break;
        }
    }
}

Stylesheet CssParser::parse() {
    return parse_stylesheet();
}

Stylesheet CssParser::parse_stylesheet() {
    // TEACHING NOTE: A stylesheet is a list of rules. We parse rules
    // until we reach the end of the input. We also skip @-rules
    // (like @media, @import) that we do not support.
    //
    // Safety limit: we cap iterations to prevent infinite loops on
    // malformed or very complex CSS (e.g., Google home page has 80KB
    // of styles). Real browsers have similar safety limits.
    Stylesheet sheet;
    skip_whitespace_and_comments();

    int iterations = 0;
    const int MAX_ITERATIONS = 5000;

    while (!at_end()) {
        if (++iterations > MAX_ITERATIONS) {
            break;
        }

        // Skip @-rules (at-rules)
        // TEACHING NOTE: @-rules like @media, @import, @keyframes are
        // CSS features we do not support. We skip them by finding the
        // matching closing brace (or semicolon for @import).
        if (peek() == '@') {
            // Find the end of the at-rule
            // Skip the @keyword
            pos_++;
            while (!at_end() && !std::isspace(static_cast<unsigned char>(peek())) && peek() != '{' && peek() != ';') {
                pos_++;
            }
            // Skip to matching brace or semicolon
            int brace_depth = 0;
            while (!at_end()) {
                if (peek() == '{') brace_depth++;
                else if (peek() == '}') {
                    brace_depth--;
                    if (brace_depth <= 0) { pos_++; break; }
                }
                else if (peek() == ';' && brace_depth == 0) { pos_++; break; }
                pos_++;
            }
            skip_whitespace_and_comments();
            continue;
        }

        // Try to parse a rule
        skip_whitespace_and_comments();
        if (at_end()) break;

        std::size_t rule_start = pos_;
        StyleRule rule = parse_rule();
        sheet.rules.push_back(std::move(rule));

        // Progress check: if parse_rule did not advance position,
        // skip one char to avoid infinite loops on unparseable input
        if (pos_ == rule_start) {
            pos_++;
        }
        skip_whitespace_and_comments();
    }

    return sheet;
}

StyleRule CssParser::parse_rule() {
    // TEACHING NOTE: A CSS rule is: selector { declarations }
    // We parse the selector first, then the declarations.
    StyleRule rule;
    rule.selector = parse_selector();

    skip_whitespace_and_comments();
    match('{');  // Skip opening brace

    rule.declarations = parse_declarations();

    skip_whitespace_and_comments();
    match('}');  // Skip closing brace

    return rule;
}

Selector CssParser::parse_selector() {
    // TEACHING NOTE: A selector is a sequence of simple selectors
    // connected by combinators. We parse simple selectors and combinators
    // alternately until we reach '{' or ',' or end of input.
    //
    // Combinators:
    //   '>'  -> child
    //   ' '  -> descendant (whitespace)
    //   '+'  -> adjacent sibling (not implemented)
    //   '~'  -> general sibling (not implemented)

    Selector selector;
    skip_whitespace_and_comments();

    // Parse first simple selector
    selector.parts.push_back(parse_simple_selector());
    selector.combinators.push_back(Combinator::None);

    while (!at_end() && peek() != '{' && peek() != ',' && peek() != ')') {
        // Skip whitespace (potential descendant combinator)
        bool had_space = false;
        while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
            had_space = true;
            pos_++;
        }

        if (at_end() || peek() == '{' || peek() == ',' || peek() == ')') break;

        // Check for explicit combinator
        if (peek() == '>') {
            pos_++;
            skip_whitespace_and_comments();
            selector.combinators.push_back(Combinator::Child);
        } else if (had_space) {
            selector.combinators.push_back(Combinator::Descendant);
        } else {
            // No combinator but more selector parts (should not happen
            // with valid CSS, but handle gracefully)
            selector.combinators.push_back(Combinator::Descendant);
        }

        selector.parts.push_back(parse_simple_selector());
    }

    return selector;
}

SimpleSelector CssParser::parse_simple_selector() {
    // TEACHING NOTE: A simple selector is:
    //   [tag_name] [.class]* [#id]*
    // For example: div.warning#main
    //
    // We parse the tag name first (if present), then classes and IDs.

    SimpleSelector sel;
    skip_whitespace_and_comments();

    // Parse tag name or universal selector
    if (peek() == '*') {
        sel.tag_name = "*";
        pos_++;
    } else if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_') {
        sel.tag_name = parse_identifier();
        std::transform(sel.tag_name.begin(), sel.tag_name.end(),
                       sel.tag_name.begin(), [](unsigned char c) { return std::tolower(c); });
    }

    // Parse classes, IDs, and pseudo-classes
    while (!at_end()) {
        if (peek() == '.') {
            pos_++;
            sel.classes.push_back(parse_identifier());
        } else if (peek() == '#') {
            pos_++;
            sel.id = parse_identifier();
        } else if (peek() == ':') {
            // Skip pseudo-class or pseudo-element (e.g., :hover, :link, ::before)
            pos_++;
            if (peek() == ':') pos_++;  // pseudo-element (::)
            parse_identifier();  // skip the pseudo-class name
            // Handle functional pseudo-classes like :not(...), :nth-child(...)
            // TEACHING NOTE: Some pseudo-classes take arguments in
            // parentheses, e.g., :not(.foo), :nth-child(2n+1). We need
            // to skip the entire parenthesized expression to avoid
            // getting stuck on the opening parenthesis.
            if (!at_end() && peek() == '(') {
                pos_++;  // skip opening paren
                int paren_depth = 1;
                while (!at_end() && paren_depth > 0) {
                    if (peek() == '(') paren_depth++;
                    else if (peek() == ')') paren_depth--;
                    pos_++;
                }
            }
        } else {
            break;
        }
    }

    return sel;
}

std::string CssParser::parse_identifier() {
    // TEACHING NOTE: CSS identifiers can contain letters, digits,
    // hyphens, and underscores, but must start with a letter or
    // hyphen or underscore. We parse until we hit a non-identifier
    // character.
    std::string ident;
    while (!at_end()) {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            ident += advance();
        } else {
            break;
        }
    }
    return ident;
}

std::vector<Declaration> CssParser::parse_declarations_internal() {
    // TEACHING NOTE: Declarations are property: value pairs separated
    // by semicolons. We parse them until we reach '}'.
    std::vector<Declaration> decls;
    skip_whitespace_and_comments();

    while (!at_end() && peek() != '}') {
        skip_whitespace_and_comments();
        if (peek() == '}') break;

        Declaration decl = parse_declaration();
        if (!decl.property.empty()) {
            decls.push_back(decl);
        }

        skip_whitespace_and_comments();
        match(';');  // Skip semicolon (optional for last declaration)
        skip_whitespace_and_comments();
    }

    return decls;
}

Declaration CssParser::parse_declaration() {
    // TEACHING NOTE: A declaration is: property : value ;
    // We parse the property name, skip the colon, then parse the value.
    // The value can contain almost any character except ';' and '}'.
    Declaration decl;
    skip_whitespace_and_comments();

    // Parse property name
    decl.property = parse_identifier();
    std::transform(decl.property.begin(), decl.property.end(),
                   decl.property.begin(), [](unsigned char c) { return std::tolower(c); });

    skip_whitespace_and_comments();

    if (!match(':')) {
        // Malformed declaration: skip to next semicolon
        while (!at_end() && peek() != ';' && peek() != '}') pos_++;
        return decl;
    }

    skip_whitespace_and_comments();

    // Parse value
    decl.value = parse_value();

    // Check for !important
    // TEACHING NOTE: !important is a CSS feature that overrides the
    // cascade. A declaration with !important always wins over normal
    // declarations, regardless of specificity. We handle this in the
    // cascade by sorting !important rules last.
    std::string value_trimmed = decl.value;
    // Find !important (case-insensitive)
    std::string lower = value_trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::size_t imp_pos = lower.find("!important");
    if (imp_pos != std::string::npos) {
        decl.important = true;
        // Remove !important from value
        decl.value = value_trimmed.substr(0, imp_pos);
        // Trim trailing whitespace
        while (!decl.value.empty() && std::isspace(static_cast<unsigned char>(decl.value.back()))) {
            decl.value.pop_back();
        }
    }

    return decl;
}

std::string CssParser::parse_value() {
    // TEACHING NOTE: CSS values can be complex (multi-value, functions,
    // etc.). We read until ';' or '}', handling parentheses for function
    // calls like rgb(255, 0, 0) or url(image.png).
    std::string value;
    int paren_depth = 0;

    while (!at_end()) {
        char c = peek();
        if (c == ';' && paren_depth == 0) break;
        if (c == '}' && paren_depth == 0) break;
        if (c == '(') paren_depth++;
        if (c == ')') paren_depth--;
        value += advance();
    }

    // Trim whitespace
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }

    return value;
}

// =========================================================================
// StyleEngine implementation
// =========================================================================

bool StyleEngine::matches_selector(const Selector& selector, const Node& node) {
    // TEACHING NOTE: Full selector matching with combinators is recursive.
    // We start from the last simple selector (which must match the node)
    // and walk backwards through the selector parts, checking ancestors.
    //
    // For "A > B": node matches B, then we check if node.parent matches A.
    // For "A B": node matches B, then we check if any ancestor matches A.
    //
    // This backwards matching is how real browsers do it - it is more
    // efficient because we can short-circuit early.

    if (selector.parts.empty()) return false;

    // Check last simple selector against the node
    const SimpleSelector& last = selector.parts.back();
    if (node.type != NodeType::Element) return false;

    // Check tag
    if (!last.tag_name.empty() && last.tag_name != "*") {
        if (node.tag_name != last.tag_name) return false;
    }
    // Check ID
    if (!last.id.empty()) {
        if (node.get_attribute("id") != last.id) return false;
    }
    // Check classes
    for (const auto& cls : last.classes) {
        if (!node.has_class(cls)) return false;
    }

    // If only one part, we are done
    if (selector.parts.size() == 1) return true;

    // Walk backwards through the selector parts
    // TEACHING NOTE: We use an index-based approach. For each part
    // (except the last), we check the combinator and find a matching
    // ancestor.
    const Node* current = &node;
    for (int i = static_cast<int>(selector.parts.size()) - 2; i >= 0; --i) {
        Combinator comb = selector.combinators[static_cast<std::size_t>(i + 1)];
        const SimpleSelector& part = selector.parts[static_cast<std::size_t>(i)];

        if (comb == Combinator::Child) {
            // Direct parent must match
            const Node* parent = current->parent;
            if (!parent || parent->type != NodeType::Element) return false;

            // Check parent against this part
            if (!part.tag_name.empty() && part.tag_name != "*") {
                if (parent->tag_name != part.tag_name) return false;
            }
            if (!part.id.empty() && parent->get_attribute("id") != part.id) return false;
            for (const auto& cls : part.classes) {
                if (!parent->has_class(cls)) return false;
            }
            current = parent;
        } else if (comb == Combinator::Descendant) {
            // Any ancestor must match
            const Node* ancestor = current->parent;
            bool found = false;
            while (ancestor) {
                if (ancestor->type == NodeType::Element) {
                    bool matches = true;
                    if (!part.tag_name.empty() && part.tag_name != "*") {
                        if (ancestor->tag_name != part.tag_name) matches = false;
                    }
                    if (matches && !part.id.empty()) {
                        if (ancestor->get_attribute("id") != part.id) matches = false;
                    }
                    if (matches) {
                        for (const auto& cls : part.classes) {
                            if (!ancestor->has_class(cls)) { matches = false; break; }
                        }
                    }
                    if (matches) {
                        current = ancestor;
                        found = true;
                        break;
                    }
                }
                ancestor = ancestor->parent;
            }
            if (!found) return false;
        }
    }

    return true;
}

ComputedStyle StyleEngine::get_computed_style(const Node& node) {
    // TEACHING NOTE: We store computed styles in the node attributes
    // under a special key "__computed_style__". This is a hack for
    // simplicity. Real browsers have a separate ComputedStyle object
    // for each element.
    //
    // We serialize the style as "prop1:val1;prop2:val2;..." which is
    // similar to the inline style attribute format.
    ComputedStyle result;
    std::string stored = node.get_attribute("__computed_style__");
    if (stored.empty()) return result;

    // Parse the stored style string
    std::istringstream ss(stored);
    std::string pair;
    while (std::getline(ss, pair, ';')) {
        std::size_t colon = pair.find(':');
        if (colon != std::string::npos) {
            std::string prop = pair.substr(0, colon);
            std::string val = pair.substr(colon + 1);
            result[prop] = val;
        }
    }
    return result;
}

void StyleEngine::apply_styles(const Stylesheet& sheet, Node& root) {
    // TEACHING NOTE: This is the cascade algorithm. For each element
    // in the DOM tree, we:
    //   1. Find all rules whose selector matches the element
    //   2. Sort matching rules by specificity (and !important)
    //   3. Apply declarations in order (lowest to highest specificity)
    //   4. Inline styles always win (they have the highest specificity)
    //   5. Inherited properties are resolved from the parent
    //
    // The cascade order (from lowest to highest priority):
    //   1. User agent stylesheet (browser defaults) - not implemented
    //   2. User stylesheet (user preferences) - not implemented
    //   3. Author stylesheet (the page CSS) - this is what we apply
    //   4. Author inline styles (style attribute)
    //   5. Author !important declarations
    //   6. User !important declarations - not implemented
    //   7. User agent !important - not implemented
    //
    // We implement: author stylesheet, author inline, and !important.

    // Recursively process each element in the tree
    std::function<void(Node*)> process = [&](Node* node) {
        if (!node) return;
        if (node->type == NodeType::Element) {
            // Collect all matching rules
            // TEACHING NOTE: We iterate over all rules in the stylesheet
            // and check if the selector matches. This is O(n*m) where n
            // is the number of elements and m is the number of rules.
            // Real browsers optimize this with rule indexes.
            struct MatchedRule {
                const StyleRule* rule;
                int spec_a, spec_b, spec_c;
                bool important;
            };
            std::vector<MatchedRule> matched;

            for (const auto& rule : sheet.rules) {
                if (matches_selector(rule.selector, *node)) {
                    int a, b, c;
                    rule.selector.specificity(a, b, c);
                    // Check if any declaration is !important
                    bool has_important = false;
                    for (const auto& decl : rule.declarations) {
                        if (decl.important) has_important = true;
                    }
                    matched.push_back({&rule, a, b, c, has_important});
                }
            }

            // Sort matching rules by specificity
            // TEACHING NOTE: We sort so that lower specificity comes first,
            // meaning higher specificity overrides it. !important rules
            // always come last.
            std::sort(matched.begin(), matched.end(), [](const MatchedRule& a, const MatchedRule& b) {
                if (a.important != b.important) return !a.important;
                if (a.spec_a != b.spec_a) return a.spec_a < b.spec_a;
                if (a.spec_b != b.spec_b) return a.spec_b < b.spec_b;
                return a.spec_c < b.spec_c;
            });

            // Build computed style
            ComputedStyle computed;

            // First, inherit from parent
            // TEACHING NOTE: Inheritance happens before the cascade.
            // The parent computed style provides default values for
            // inherited properties.
            if (node->parent && node->parent->type == NodeType::Element) {
                ComputedStyle parent_style = get_computed_style(*node->parent);
                for (const auto& prop : inherited_properties()) {
                    auto it = parent_style.find(prop);
                    if (it != parent_style.end()) {
                        computed[prop] = it->second;
                    }
                }
            }

            // Apply matched rules in specificity order
            for (const auto& m : matched) {
                for (const auto& decl : m.rule->declarations) {
                    computed[decl.property] = decl.value;
                }
            }

            // Apply inline styles (highest non-!important specificity)
            // TEACHING NOTE: Inline styles (style="color: red") have
            // specificity (1,0,0,0) which beats any selector. We parse
            // the style attribute as a mini CSS declaration block.
            std::string inline_style = node->get_attribute("style");
            if (!inline_style.empty()) {
                CssParser inline_parser(inline_style);
                auto decls = inline_parser.parse_declarations();
                for (const auto& decl : decls) {
                    computed[decl.property] = decl.value;
                }
            }

            // Store computed style
            // TEACHING NOTE: We serialize the computed style into a string
            // and store it in a special attribute. This is a hack but
            // keeps the code simple.
            std::ostringstream style_str;
            bool first = true;
            for (const auto& [prop, val] : computed) {
                if (!first) style_str << ";";
                style_str << prop << ":" << val;
                first = false;
            }
            node->attributes["__computed_style__"] = style_str.str();
        }

        // Process children
        for (const auto& child : node->children) {
            process(child.get());
        }
    };

    process(&root);
}

} // namespace chinstrap