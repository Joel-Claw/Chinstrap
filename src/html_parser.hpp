// =========================================================================
// html_parser.hpp - HTML Tokenizer and DOM Tree Builder
// =========================================================================
// TEACHING NOTE: HTML parsing is one of the most complex parts of a
// browser. The HTML5 specification defines a tokenizer (which breaks
// the input into tokens: tags, text, comments) and a tree construction
// stage (which builds a DOM tree from the tokens, handling the implicit
// tag creation and error recovery that HTML is famous for).
//
// The HTML5 spec is huge (the parsing section alone is over 100 pages).
// We implement a simplified version that handles:
//   - Start tags: <p>, <div>, <h1 class="title">
//   - End tags: </p>, </div>, </h1>
//   - Self-closing tags: <br/>, <img src="x"/>
//   - Void elements: <br>, <hr>, <img>, <input>, <meta>, <link>
//   - Attributes with quoted and unquoted values
//   - Text nodes
//   - Comments: <!-- comment -->
//   - DOCTYPE: <!DOCTYPE html>
//   - Basic tree construction with implicit body/html elements
//   - Basic auto-closing of <p>, <li>, <td>, etc.
//
// What we do NOT handle (but real browsers do):
//   - Script execution (we parse <script> content as text)
//   - Style element content (we parse it as text)
//   - Template element content (special parsing context)
//   - Foreign content (SVG, MathML) with different parsing rules
//   - Full error recovery (HTML5 has very specific error handling)
//   - Adoption agency algorithm (misnested tags)
//
// TEACHING NOTE: How Chrome's HTML parser works:
// Chrome uses a state machine defined by the HTML5 spec. The tokenizer
// has states like "data", "tag open", "tag name", "attribute name",
// "attribute value (double-quoted)", etc. Each character transitions
// between states. The tree builder receives tokens from the tokenizer
// and maintains an "open element stack" that tracks which elements are
// currently being parsed. When an end tag is received, it pops the
// stack. When a start tag is received, it pushes onto the stack. The
// stack also handles implicit closes (e.g., a new <li> closes the
// previous <li>).
// =========================================================================

#ifndef CHINSTRAP_HTML_PARSER_HPP
#define CHINSTRAP_HTML_PARSER_HPP

#include <string>
#include <vector>
#include <memory>
#include <map>

namespace chinstrap {

// -------------------------------------------------------------------------
// DOM Node types
// -------------------------------------------------------------------------
// TEACHING NOTE: The DOM (Document Object Model) is a tree of nodes
// representing the HTML document. Each HTML element becomes an element
// node. Text between elements becomes text nodes. The root is the
// document node, which contains the <html> element, which contains
// <head> and <body>, and so on.
//
// The DOM is the browser's internal representation of the page. CSS
// applies styles to DOM elements. JavaScript manipulates the DOM.
// Layout computes positions from the DOM. Everything starts here.
//
// We use a simple node type that can be an element or text. Real DOM
// implementations have many node types (Document, DocumentFragment,
// Comment, ProcessingInstruction, etc.), but for our browser, two
// types suffice: elements and text.
// -------------------------------------------------------------------------

enum class NodeType {
    Element,
    Text,
    Document,
    Comment,
};

// DOM node in the tree
class Node {
public:
    NodeType type;

    // For element nodes
    std::string tag_name;                    // lowercase tag name
    std::map<std::string, std::string> attributes;
    std::vector<std::unique_ptr<Node>> children;

    // For text and comment nodes
    std::string text_content;

    // Parent pointer (non-owning, for traversal up the tree)
    // TEACHING NOTE: We use a raw parent pointer because the parent
    // owns the child (via unique_ptr), not the other way around.
    // This avoids reference cycles that would break unique_ptr.
    Node* parent = nullptr;

    // Constructors
    explicit Node(NodeType t) : type(t) {}
    Node(NodeType t, const std::string& content) : type(t), text_content(content) {}

    // Add a child node
    // TEACHING NOTE: We take ownership of the child via unique_ptr.
    // The child gets a raw back-pointer to its parent.
    void add_child(std::unique_ptr<Node> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

    // Get an attribute, or empty string if not present
    std::string get_attribute(const std::string& name) const {
        auto it = attributes.find(name);
        return (it != attributes.end()) ? it->second : "";
    }

    // Check if element has a class
    bool has_class(const std::string& cls) const;

    // Get all text content (recursive)
    std::string text() const;

    // Get visible text content (recursive, but skips script/style/
    // head/title/meta/link/noscript elements whose text should never
    // appear as rendered page content)
    std::string visible_text() const;

    // Find first child element with tag name
    const Node* find_first(const std::string& tag) const;

    // Get all child elements (not text/comment nodes)
    std::vector<const Node*> child_elements() const;

    // Get all descendants matching a tag
    std::vector<const Node*> get_elements_by_tag(const std::string& tag) const;
};

// -------------------------------------------------------------------------
// HTML HtmlToken types
// -------------------------------------------------------------------------
// TEACHING NOTE: The tokenizer breaks the input stream into tokens.
// Each token is one of:
//   - StartTag: <tag attr="value">
//   - EndTag: </tag>
//   - Text: characters between tags
//   - Comment: <!-- ... -->
//   - Doctype: <!DOCTYPE html>
//
// The tokenizer is a state machine. It starts in the "data" state.
// When it sees '<', it transitions to "tag open" state. When it
// sees a letter after '<', it starts accumulating a tag name. When
// it sees '>', the tag is complete and it goes back to "data" state.
// -------------------------------------------------------------------------

enum class HtmlTokenType {
    StartTag,
    EndTag,
    Text,
    Comment,
    Doctype,
    EndOfFile,
};

struct HtmlToken {
    HtmlTokenType type;
    std::string tag_name;                    // For StartTag/EndTag
    std::map<std::string, std::string> attributes;  // For StartTag
    std::string text;                        // For Text/Comment/Doctype
    bool self_closing = false;              // For StartTag (<br/>)
};

// -------------------------------------------------------------------------
// HtmlParser - HTML tokenizer and tree builder
// -------------------------------------------------------------------------

class HtmlParser {
public:
    explicit HtmlParser(const std::string& input);

    // Parse the input and return the document tree
    // TEACHING NOTE: This method runs the full pipeline:
    //   1. Tokenize the input (break into tokens)
    //   2. Build a DOM tree from the tokens
    //   3. Return the root document node
    std::unique_ptr<Node> parse();

private:
    std::string input_;
    std::size_t pos_ = 0;

    // --- Tokenizer ---
    std::vector<HtmlToken> tokenize();
    HtmlToken parse_token();
    HtmlToken parse_doctype_or_comment();
    HtmlToken parse_start_tag();
    HtmlToken parse_end_tag();
    std::string parse_tag_name();
    void parse_attributes(std::map<std::string, std::string>& attrs, bool& self_closing);
    std::string parse_attribute_value(char quote_char);
    std::string parse_text();
    std::string parse_comment();

    // Helpers
    bool starts_with(const std::string& s);
    bool match(const std::string& s);
    char peek(std::size_t offset = 0) const;
    char advance();
    bool at_end() const { return pos_ >= input_.size(); }

    // --- Tree builder ---
    // TEACHING NOTE: The tree builder maintains a stack of open elements.
    // When we see a start tag, we create an element and push it on the
    // stack. When we see an end tag, we pop the stack. The stack tells
    // us where to insert new elements in the tree.
    //
    // HTML has special rules for auto-closing tags. For example:
    //   <li>item1<li>item2  - the second <li> implicitly closes the first
    //   <p>text<div>more    - the <div> implicitly closes the <p>
    // We implement a simplified version of these rules.
    std::vector<Node*> open_stack_;

    void build_tree(const std::vector<HtmlToken>& tokens);
    void insert_element(const HtmlToken& token);
    void close_element(const std::string& tag_name);
    void insert_text(const std::string& text);
    void insert_comment(const std::string& text);

    bool is_void_element(const std::string& tag) const;
    bool auto_closes(const std::string& new_tag, const std::string& open_tag) const;

    // Storage for the built document tree (moved out by parse())
    std::unique_ptr<Node> document_;
};

// Set of HTML void elements (no closing tag, no content)
// TEACHING NOTE: Void elements are defined in the HTML spec. They cannot
// have content or end tags. The full list: area, base, br, col, embed,
// hr, img, input, link, meta, param, source, track, wbr.
const std::vector<std::string>& void_elements();

} // namespace chinstrap

#endif // CHINSTRAP_HTML_PARSER_HPP