// =========================================================================
// html_parser.cpp - HTML Parser Implementation
// =========================================================================
// TEACHING NOTE: This file implements the HTML tokenizer and tree builder.
// The tokenizer is a simplified version of the HTML5 tokenization state
// machine. The tree builder is a simplified version of the HTML5 tree
// construction algorithm.
//
// HTML parsing is famously error-tolerant. The HTML5 spec defines exactly
// how to recover from malformed input (missing tags, wrong nesting,
// unclosed elements). This error recovery is what makes the web work:
// billions of pages are technically malformed, but browsers render them
// all because the spec defines recovery rules.
//
// We implement basic error recovery:
//   - Auto-insert <html>, <head>, <body> if missing
//   - Auto-close <p> when a block-level element starts
//   - Auto-close <li> when a new <li> starts
//   - Ignore end tags for elements not on the open stack
// =========================================================================

#include "html_parser.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <string>

namespace chinstrap {

// =========================================================================
// Node methods
// =========================================================================

bool Node::has_class(const std::string& cls) const {
    std::string class_attr = get_attribute("class");
    // Split class attribute by whitespace and check if cls is present
    std::istringstream ss(class_attr);
    std::string c;
    while (ss >> c) {
        if (c == cls) return true;
    }
    return false;
}

std::string Node::text() const {
    // TEACHING NOTE: Recursively collect all text from descendants.
    // This is how textContent works in the DOM API.
    std::string result;
    if (type == NodeType::Text) {
        result = text_content;
    }
    for (const auto& child : children) {
        result += child->text();
    }
    return result;
}

const Node* Node::find_first(const std::string& tag) const {
    for (const auto& child : children) {
        if (child->type == NodeType::Element && child->tag_name == tag) {
            return child.get();
        }
        const Node* found = child->find_first(tag);
        if (found) return found;
    }
    return nullptr;
}

std::vector<const Node*> Node::child_elements() const {
    std::vector<const Node*> result;
    for (const auto& child : children) {
        if (child->type == NodeType::Element) {
            result.push_back(child.get());
        }
    }
    return result;
}

std::vector<const Node*> Node::get_elements_by_tag(const std::string& tag) const {
    std::vector<const Node*> result;
    for (const auto& child : children) {
        if (child->type == NodeType::Element) {
            if (tag == "*" || child->tag_name == tag) {
                result.push_back(child.get());
            }
            auto nested = child->get_elements_by_tag(tag);
            result.insert(result.end(), nested.begin(), nested.end());
        }
    }
    return result;
}

// =========================================================================
// Void elements
// =========================================================================

const std::vector<std::string>& void_elements() {
    // TEACHING NOTE: These are HTML void elements. They never have
    // closing tags or children. The HTML5 spec defines these in
    // section 12.1.2 (Elements).
    static const std::vector<std::string> voids = {
        "area", "base", "br", "col", "embed", "hr",
        "img", "input", "link", "meta", "param", "source", "track", "wbr"
    };
    return voids;
}

// =========================================================================
// HtmlParser constructor
// =========================================================================

HtmlParser::HtmlParser(const std::string& input) : input_(input) {}

// =========================================================================
// Tokenizer helpers
// =========================================================================

char HtmlParser::peek(std::size_t offset) const {
    if (pos_ + offset >= input_.size()) return '\0';
    return input_[pos_ + offset];
}

char HtmlParser::advance() {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_++];
}

bool HtmlParser::starts_with(const std::string& s) {
    if (pos_ + s.size() > input_.size()) return false;
    return input_.compare(pos_, s.size(), s) == 0;
}

bool HtmlParser::match(const std::string& s) {
    if (!starts_with(s)) return false;
    pos_ += s.size();
    return true;
}

// =========================================================================
// Tokenizer
// =========================================================================

std::vector<HtmlToken> HtmlParser::tokenize() {
    // TEACHING NOTE: The tokenizer reads the input character by character
    // and produces a stream of tokens. It is a simplified version of the
    // HTML5 tokenization state machine.
    //
    // States (simplified):
    //   DATA: normal text, looking for '<'
    //   TAG_OPEN: just saw '<', deciding what kind of tag
    //   TAG_NAME: reading a tag name
    //   ATTRIBUTE: reading attributes
    //   COMMENT: reading a comment
    //
    // We do not implement all states (there are about 30 in the spec)
    // but handle the most common cases.

    std::vector<HtmlToken> tokens;

    while (!at_end()) {
        HtmlToken token = parse_token();
        if (token.type != HtmlTokenType::EndOfFile) {
            tokens.push_back(std::move(token));
        }
    }

    tokens.push_back(HtmlToken{HtmlTokenType::EndOfFile, "", {}, "", false});
    return tokens;
}

HtmlToken HtmlParser::parse_token() {
    // TEACHING NOTE: In the DATA state, we read text until we hit '<'.
    // If the next character after '<' is a letter, it is a start tag.
    // If it is '/', it is an end tag. If it is '!', it is a comment or
    // doctype. Otherwise, it is just text (the '<' is literal).

    if (at_end()) {
        return HtmlToken{HtmlTokenType::EndOfFile, "", {}, "", false};
    }

    // Check for tag start
    if (peek() == '<') {
        // Look ahead to determine the type
        char next = peek(1);

        if (std::isalpha(static_cast<unsigned char>(next))) {
            // Start tag
            return parse_start_tag();
        } else if (next == '/') {
            // End tag
            return parse_end_tag();
        } else if (next == '!') {
            // Comment or DOCTYPE
            return parse_doctype_or_comment();
        } else {
            // Literal '<' in text
            return HtmlToken{HtmlTokenType::Text, "", {}, std::string(1, advance()), false};
        }
    }

    // Text content
    return HtmlToken{HtmlTokenType::Text, "", {}, parse_text(), false};
}

HtmlToken HtmlParser::parse_doctype_or_comment() {
    // TEACHING NOTE: DOCTYPE declarations look like <!DOCTYPE html>.
    // Comments look like <!-- text -->. Both start with "<!".
    // We distinguish by looking at the next characters after "!".

    pos_++;  // Skip '<'
    pos_++;  // Skip '!'

    if (starts_with("--")) {
        // Comment
        return HtmlToken{HtmlTokenType::Comment, "", {}, parse_comment(), false};
    }

    // DOCTYPE
    // Read until '>'
    std::string doctype;
    while (!at_end() && peek() != '>') {
        doctype += advance();
    }
    if (peek() == '>') pos_++;  // Skip '>'

    return HtmlToken{HtmlTokenType::Doctype, "", {}, doctype, false};
}

std::string HtmlParser::parse_comment() {
    // TEACHING NOTE: HTML comments are <!-- ... -->.
    // We already consumed '<!'. Now we expect '--'.
    // We read until we find '-->'.
    pos_ += 2;  // Skip '--'

    std::string result;
    while (!at_end()) {
        if (starts_with("-->")) {
            pos_ += 3;
            break;
        }
        result += advance();
    }
    return result;
}

HtmlToken HtmlParser::parse_start_tag() {
    // TEACHING NOTE: A start tag looks like:
    //   <tagname attr="value" attr2='value2' attr3=value3>
    //   <br/>
    //   <img src="photo.jpg" alt="A photo">
    //
    // We read the tag name, then attributes, then check for self-closing
    // (ends with />) or normal closing (>).

    pos_++;  // Skip '<'
    std::string name = parse_tag_name();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    HtmlToken token{HtmlTokenType::StartTag, name, {}, "", false};

    // Skip whitespace before attributes
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
        pos_++;
    }

    // Parse attributes
    parse_attributes(token.attributes, token.self_closing);

    return token;
}

HtmlToken HtmlParser::parse_end_tag() {
    // TEACHING NOTE: End tags look like </tagname>. We read the tag
    // name and skip everything until '>'. We do not need to parse
    // attributes in end tags (they are invalid but ignored by browsers).

    pos_ += 2;  // Skip '</'
    std::string name = parse_tag_name();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Skip anything until '>'
    while (!at_end() && peek() != '>') {
        pos_++;
    }
    if (peek() == '>') pos_++;

    return HtmlToken{HtmlTokenType::EndTag, name, {}, "", false};
}

std::string HtmlParser::parse_tag_name() {
    // TEACHING NOTE: Tag names consist of letters, digits, hyphens,
    // and underscores. We read until we hit a character that is not
    // part of a tag name.
    std::string name;
    while (!at_end()) {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':') {
            name += advance();
        } else {
            break;
        }
    }
    return name;
}

void HtmlParser::parse_attributes(std::map<std::string, std::string>& attrs,
                                   bool& self_closing) {
    // TEACHING NOTE: Attributes come in name=value pairs separated by
    // whitespace. The value can be:
    //   - Double-quoted: attr="value"
    //   - Single-quoted: attr='value'
    //   - Unquoted: attr=value (no spaces allowed in value)
    //
    // Boolean attributes (like <input disabled>) have no value. We
    // store them with an empty string value.
    //
    // The self-closing flag is set when we see /> at the end.

    while (!at_end() && peek() != '>') {
        // Skip whitespace
        while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
            pos_++;
        }

        if (at_end() || peek() == '>') break;

        // Check for self-closing
        if (peek() == '/' && peek(1) == '>') {
            self_closing = true;
            pos_ += 2;  // Skip />
            return;
        }

        if (peek() == '/') {
            pos_++;  // Skip stray /
            continue;
        }

        // Read attribute name
        std::string attr_name;
        while (!at_end() && !std::isspace(static_cast<unsigned char>(peek())) &&
               peek() != '=' && peek() != '>' && peek() != '/') {
            attr_name += advance();
        }

        if (attr_name.empty()) {
            pos_++;  // Skip stray character
            continue;
        }

        // Lowercase attribute name (HTML is case-insensitive for attr names)
        std::transform(attr_name.begin(), attr_name.end(), attr_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Skip whitespace before '='
        while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
            pos_++;
        }

        std::string attr_value;
        if (peek() == '=') {
            pos_++;  // Skip '='

            // Skip whitespace after '='
            while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
                pos_++;
            }

            if (peek() == '"') {
                attr_value = parse_attribute_value('"');
            } else if (peek() == '\'') {
                attr_value = parse_attribute_value('\'');
            } else {
                // Unquoted value
                while (!at_end() && !std::isspace(static_cast<unsigned char>(peek())) &&
                       peek() != '>') {
                    attr_value += advance();
                }
            }
        }

        attrs[attr_name] = attr_value;
    }

    // Skip the closing '>'
    if (peek() == '>') pos_++;
}

std::string HtmlParser::parse_attribute_value(char quote_char) {
    // TEACHING NOTE: Quoted attribute values can contain any character
    // except the quote character. HTML does NOT require escaping of '<'
    // or '>' in attribute values (unlike XML). We read until the closing
    // quote.
    pos_++;  // Skip opening quote
    std::string value;
    while (!at_end() && peek() != quote_char) {
        value += advance();
    }
    if (peek() == quote_char) pos_++;  // Skip closing quote
    return value;
}

std::string HtmlParser::parse_text() {
    // TEACHING NOTE: Text nodes contain everything between tags. In
    // HTML, text is NOT preprocessed (no entity decoding for now, though
    // we should do this for a real browser). We read until we hit '<'.
    //
    // Browsers decode HTML entities like &amp; &lt; &gt; &quot; &nbsp;
    // We skip this for simplicity but note it as a TODO.

    std::string text;
    while (!at_end() && peek() != '<') {
        text += advance();
    }

    // Decode basic HTML entities
    // TEACHING NOTE: HTML entities are character references like &amp;
    // We decode the most common ones. A full implementation would handle
    // all named entities (there are hundreds) and numeric entities
    // (&#65; for 'A', &#x41; for 'A').
    auto replace = [&](const std::string& from, const std::string& to) {
        std::size_t p = 0;
        while ((p = text.find(from, p)) != std::string::npos) {
            text.replace(p, from.size(), to);
            p += to.size();
        }
    };
    replace("&amp;", "&");
    replace("&lt;", "<");
    replace("&gt;", ">");
    replace("&quot;", "\"");
    replace("&apos;", "'");
    replace("&nbsp;", " ");

    return text;
}

// =========================================================================
// Tree builder
// =========================================================================

bool HtmlParser::is_void_element(const std::string& tag) const {
    const auto& voids = void_elements();
    return std::find(voids.begin(), voids.end(), tag) != voids.end();
}

bool HtmlParser::auto_closes(const std::string& new_tag, const std::string& open_tag) const {
    // TEACHING NOTE: HTML has rules about which elements auto-close when
    // a new element starts. For example:
    //   <li>item1<li>item2  -> the second <li> closes the first
    //   <p>text<div>        -> <div> closes <p> (p cannot contain div)
    //   <td>cell<td>cell    -> second <td> closes first
    //
    // These rules are defined in the HTML5 spec under "generate implied
    // end tags". We implement a simplified version.

    // <li> auto-closes another <li>
    if (new_tag == "li" && open_tag == "li") return true;
    // <td> auto-closes <td> or <th>
    if (new_tag == "td" && (open_tag == "td" || open_tag == "th")) return true;
    if (new_tag == "th" && (open_tag == "td" || open_tag == "th")) return true;
    // <tr> auto-closes <tr>
    if (new_tag == "tr" && open_tag == "tr") return true;
    // <option> auto-closes <option>
    if (new_tag == "option" && open_tag == "option") return true;

    // Block elements close <p>
    // TEACHING NOTE: In HTML, a <p> element is implicitly closed when
    // a block-level element starts. This is because <p> can only contain
    // inline content (phrasing content). When the parser sees <div>
    // inside <p>, it closes the <p> first.
    if (open_tag == "p") {
        static const std::vector<std::string> block_tags = {
            "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
            "ul", "ol", "table", "blockquote", "pre", "hr",
            "section", "article", "aside", "header", "footer", "nav"
        };
        if (std::find(block_tags.begin(), block_tags.end(), new_tag) != block_tags.end()) {
            return true;
        }
    }

    return false;
}

void HtmlParser::insert_element(const HtmlToken& token) {
    // TEACHING NOTE: When we see a start tag, we create a new element
    // node and add it as a child of the current open element (top of
    // the stack). Then we push the new element onto the stack (unless
    // it is a void element or self-closing).
    //
    // The open stack is how we track where we are in the tree. The
    // bottom of the stack is the document node. The top is the element
    // we are currently inside.

    auto node = std::make_unique<Node>(NodeType::Element);
    node->tag_name = token.tag_name;
    node->attributes = token.attributes;

    // Get the current open element (parent)
    Node* parent = open_stack_.empty() ? nullptr : open_stack_.back();

    // Insert into the tree
    Node* raw_ptr = node.get();
    if (parent) {
        parent->add_child(std::move(node));
    } else {
        // No parent on stack. This should not happen if we have a
        // document root, but handle it gracefully.
        // Store as a dangling node (will be lost). For a real browser
        // this would be an error condition.
        // For now, just return without inserting.
        return;
    }

    // Push onto stack unless void or self-closing
    if (!is_void_element(token.tag_name) && !token.self_closing) {
        open_stack_.push_back(raw_ptr);
    }
}

void HtmlParser::close_element(const std::string& tag_name) {
    // TEACHING NOTE: When we see an end tag, we need to find the
    // matching start tag on the open stack and close it. The HTML5
    // spec has complex rules for this (the "adoption agency algorithm")
    // to handle misnested tags. We do a simple version: search from
    // the top of the stack for a matching tag, and pop everything
    // up to and including it. If we do not find it, we ignore the
    // end tag (error recovery).

    for (int i = static_cast<int>(open_stack_.size()) - 1; i >= 0; --i) {
        if (open_stack_[static_cast<std::size_t>(i)]->tag_name == tag_name) {
            // Pop everything from i to the top
            open_stack_.erase(open_stack_.begin() + i);
            return;
        }
    }
    // Tag not found on stack: ignore (HTML error recovery)
}

void HtmlParser::insert_text(const std::string& text) {
    if (text.empty()) return;

    // TEACHING NOTE: Text is added as a child of the current open
    // element (top of the stack). If the top is an element that
    // cannot contain text (like <br>), this would be an error in
    // strict XML, but HTML is lenient.

    Node* parent = open_stack_.empty() ? nullptr : open_stack_.back();
    if (parent) {
        auto node = std::make_unique<Node>(NodeType::Text, text);
        parent->add_child(std::move(node));
    }
}

void HtmlParser::insert_comment(const std::string& text) {
    Node* parent = open_stack_.empty() ? nullptr : open_stack_.back();
    if (parent) {
        auto node = std::make_unique<Node>(NodeType::Comment, text);
        parent->add_child(std::move(node));
    }
}

void HtmlParser::build_tree(const std::vector<HtmlToken>& tokens) {
    // TEACHING NOTE: This is the main tree construction loop. We
    // iterate over tokens from the tokenizer and build the DOM tree.
    //
    // The HTML5 spec defines "insertion modes" for the tree builder
    // (initial, before html, before head, in head, after head, in body,
    // after body, after after body). Each mode handles tokens differently.
    // We simplify this: we auto-insert <html>, <head>, <body> as needed.

    // Create the document root
    auto doc = std::make_unique<Node>(NodeType::Document);
    doc->tag_name = "#document";

    // Create <html> root element
    auto html = std::make_unique<Node>(NodeType::Element);
    html->tag_name = "html";
    Node* html_ptr = html.get();
    doc->add_child(std::move(html));

    // Create <head>
    auto head = std::make_unique<Node>(NodeType::Element);
    head->tag_name = "head";
    Node* head_ptr = head.get();
    html_ptr->add_child(std::move(head));

    // Create <body>
    auto body = std::make_unique<Node>(NodeType::Element);
    body->tag_name = "body";
    Node* body_ptr = body.get();
    html_ptr->add_child(std::move(body));

    // Initialize the open stack
    // TEACHING NOTE: We start with body as the current open element.
    // A real browser would start in "before html" mode and auto-insert
    // html/head/body as needed. We just pre-create them.
    open_stack_.push_back(body_ptr);

    bool in_head = false;

    for (const HtmlToken& token : tokens) {
        if (token.type == HtmlTokenType::EndOfFile) break;

        // Handle DOCTYPE: ignore (we already have our document structure)
        if (token.type == HtmlTokenType::Doctype) continue;

        // Handle head-related tags specially
        if (token.type == HtmlTokenType::StartTag) {
            const std::string& tag = token.tag_name;

            // html tag: already created, just copy attributes
            if (tag == "html") {
                html_ptr->attributes = token.attributes;
                continue;
            }
            // Head elements
            if (tag == "head") {
                open_stack_.push_back(head_ptr);
                in_head = true;
                continue;
            }
            if (tag == "title" || tag == "meta" || tag == "link" || tag == "style" || tag == "base") {
                if (in_head) {
                    // Insert into head
                    auto node = std::make_unique<Node>(NodeType::Element);
                    node->tag_name = tag;
                    node->attributes = token.attributes;
                    Node* raw = node.get();
                    head_ptr->add_child(std::move(node));
                    if (!is_void_element(tag) && !token.self_closing) {
                        open_stack_.push_back(raw);
                    }
                    continue;
                }
            }
            if (tag == "body") {
                // Body tag: switch to body context
                if (in_head) {
                    // Close head first
                    open_stack_.pop_back();
                    in_head = false;
                }
                open_stack_ = {body_ptr};
                // Copy body attributes
                body_ptr->attributes = token.attributes;
                continue;
            }
        }

        if (token.type == HtmlTokenType::EndTag) {
            const std::string& tag = token.tag_name;
            if (tag == "head") {
                if (in_head) {
                    open_stack_.pop_back();
                    in_head = false;
                    open_stack_ = {body_ptr};
                }
                continue;
            }
            if (tag == "html" || tag == "body") {
                // Ignore end tags for html and body (they are implicit)
                continue;
            }
        }

        // If we see content while in head, switch to body
        // But NOT if we are inside a head child element like title/style
        // that is on the open stack. Text inside title should go into title.
        if (in_head && (token.type == HtmlTokenType::Text || token.type == HtmlTokenType::StartTag)) {
            // Check if we are inside a head child element (title, style, script)
            bool inside_head_child = false;
            if (!open_stack_.empty() && open_stack_.back() != head_ptr) {
                Node* top = open_stack_.back();
                if (top->tag_name == "title" || top->tag_name == "style" ||
                    top->tag_name == "script" || top->tag_name == "textarea") {
                    inside_head_child = true;
                }
            }
            if (!inside_head_child) {
                const std::string& text = token.type == HtmlTokenType::Text ? token.text : token.tag_name;
                bool is_whitespace = true;
                for (char c : text) {
                    if (!std::isspace(static_cast<unsigned char>(c))) {
                        is_whitespace = false;
                        break;
                    }
                }
                if (!is_whitespace || token.type == HtmlTokenType::StartTag) {
                    // Pop head, go to body
                    open_stack_.pop_back();
                    in_head = false;
                    open_stack_ = {body_ptr};
                }
            }
        }

        // Process token
        switch (token.type) {
            case HtmlTokenType::StartTag: {
                // Check for auto-closing
                while (open_stack_.size() > 1) {
                    Node* top = open_stack_.back();
                    if (top == body_ptr || top == html_ptr) break;
                    if (auto_closes(token.tag_name, top->tag_name)) {
                        open_stack_.pop_back();
                    } else {
                        break;
                    }
                }
                insert_element(token);
                break;
            }
            case HtmlTokenType::EndTag:
                close_element(token.tag_name);
                break;
            case HtmlTokenType::Text:
                insert_text(token.text);
                break;
            case HtmlTokenType::Comment:
                insert_comment(token.text);
                break;
            default:
                break;
        }
    }

    // Store the document as our result
    // We need to keep a reference to it. We use a member to store it.
    // Actually, we return it from parse(). Let us restructure.
    document_ = std::move(doc);
}

// We need to store the document. Add a member.
// TEACHING NOTE: We add document_ as a member of HtmlParser.
// This is because build_tree creates the document but parse() needs
// to return it. We store it temporarily.

std::unique_ptr<Node> HtmlParser::parse() {
    auto tokens = tokenize();
    build_tree(tokens);
    return std::move(document_);
}

} // namespace chinstrap