// =========================================================================
// test_html_parser.cpp - Unit tests for HTML parser
// =========================================================================
// TEACHING NOTE: These tests exercise the HTML tokenizer and tree builder.
// We test:
//   - Basic tag parsing (open, close, self-closing)
//   - Attribute parsing (quoted, unquoted values)
//   - Text nodes
//   - Comments
//   - Void elements (br, img, etc.)
//   - Auto-closing behavior (li, p)
//   - DOM tree structure
// =========================================================================

#include "test_framework.hpp"
#include "html_parser.hpp"

using namespace chinstrap;

// --- Basic parsing ---

TEST(parse_simple_html) {
    HtmlParser parser("<html><body><p>Hello</p></body></html>");
    auto doc = parser.parse();

    ASSERT_TRUE(doc != nullptr);
    ASSERT_EQ(static_cast<int>(doc->type), static_cast<int>(NodeType::Document));
}

TEST(parse_text_node) {
    HtmlParser parser("<p>Hello World</p>");
    auto doc = parser.parse();

    // Find the p element
    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ASSERT_STREQ(p->text(), "Hello World");
}

TEST(parse_nested_elements) {
    HtmlParser parser("<div><p>text</p></div>");
    auto doc = parser.parse();

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
    ASSERT_EQ(div->children.size(), static_cast<std::size_t>(1));

    const Node* p = div->find_first("p");
    ASSERT_TRUE(p != nullptr);
}

// --- Attributes ---

TEST(parse_attributes_double_quoted) {
    HtmlParser parser("<a href=\"http://example.com\">link</a>");
    auto doc = parser.parse();

    const Node* a = doc->find_first("a");
    ASSERT_TRUE(a != nullptr);
    ASSERT_STREQ(a->get_attribute("href"), "http://example.com");
}

TEST(parse_attributes_single_quoted) {
    HtmlParser parser("<div class='container'>text</div>");
    auto doc = parser.parse();

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
    ASSERT_STREQ(div->get_attribute("class"), "container");
}

TEST(parse_attributes_unquoted) {
    HtmlParser parser("<div class=container>text</div>");
    auto doc = parser.parse();

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
    ASSERT_STREQ(div->get_attribute("class"), "container");
}

TEST(parse_multiple_attributes) {
    HtmlParser parser("<input type=\"text\" name=\"query\" value=\"test\" id=\"search\">");
    auto doc = parser.parse();

    const Node* input = doc->find_first("input");
    ASSERT_TRUE(input != nullptr);
    ASSERT_STREQ(input->get_attribute("type"), "text");
    ASSERT_STREQ(input->get_attribute("name"), "query");
    ASSERT_STREQ(input->get_attribute("value"), "test");
    ASSERT_STREQ(input->get_attribute("id"), "search");
}

// --- Void elements ---

TEST(parse_void_element_br) {
    HtmlParser parser("<p>line1<br>line2</p>");
    auto doc = parser.parse();

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    // br should be a child of p
    const Node* br = p->find_first("br");
    ASSERT_TRUE(br != nullptr);
}

TEST(parse_void_element_img) {
    HtmlParser parser("<div><img src=\"photo.png\" alt=\"Photo\"></div>");
    auto doc = parser.parse();

    const Node* img = doc->find_first("img");
    ASSERT_TRUE(img != nullptr);
    ASSERT_STREQ(img->get_attribute("src"), "photo.png");
    ASSERT_STREQ(img->get_attribute("alt"), "Photo");
}

// --- Self-closing tags ---

TEST(parse_self_closing_tag) {
    HtmlParser parser("<div><br/></div>");
    auto doc = parser.parse();

    const Node* br = doc->find_first("br");
    ASSERT_TRUE(br != nullptr);
}

// --- Comments ---

TEST(parse_comment) {
    HtmlParser parser("<div><!-- a comment --></div>");
    auto doc = parser.parse();

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
    // Comment should be a child
    bool found_comment = false;
    for (const auto& child : div->children) {
        if (child->type == NodeType::Comment) {
            found_comment = true;
            ASSERT_STREQ(child->text_content, " a comment ");
        }
    }
    ASSERT_TRUE(found_comment);
}

// --- HTML entity decoding ---

TEST(parse_html_entities) {
    HtmlParser parser("<p>&amp;&lt;&gt;&quot;</p>");
    auto doc = parser.parse();

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ASSERT_STREQ(p->text(), "&<>\"");
}

// --- Class checking ---

TEST(has_class) {
    HtmlParser parser("<div class=\"foo bar baz\">text</div>");
    auto doc = parser.parse();

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
    ASSERT_TRUE(div->has_class("foo"));
    ASSERT_TRUE(div->has_class("bar"));
    ASSERT_TRUE(div->has_class("baz"));
    ASSERT_FALSE(div->has_class("qux"));
}

// --- Auto-closing ---

TEST(auto_close_li) {
    HtmlParser parser("<ul><li>one<li>two<li>three</ul>");
    auto doc = parser.parse();

    auto lis = doc->get_elements_by_tag("li");
    ASSERT_EQ(lis.size(), static_cast<std::size_t>(3));
    ASSERT_STREQ(lis[0]->text(), "one");
    ASSERT_STREQ(lis[1]->text(), "two");
    ASSERT_STREQ(lis[2]->text(), "three");
}

TEST(auto_close_p_with_div) {
    HtmlParser parser("<p>text<div>block</div>");
    auto doc = parser.parse();

    // The <p> should be auto-closed when <div> starts
    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ASSERT_STREQ(p->text(), "text");

    const Node* div = doc->find_first("div");
    ASSERT_TRUE(div != nullptr);
}

// --- Multiple elements ---

TEST(parse_multiple_paragraphs) {
    HtmlParser parser("<div><p>First</p><p>Second</p><p>Third</p></div>");
    auto doc = parser.parse();

    auto ps = doc->get_elements_by_tag("p");
    ASSERT_EQ(ps.size(), static_cast<std::size_t>(3));
}

TEST(parse_mixed_content) {
    HtmlParser parser("<div><h1>Title</h1><p>Paragraph</p><ul><li>Item</li></ul></div>");
    auto doc = parser.parse();

    ASSERT_TRUE(doc->find_first("h1") != nullptr);
    ASSERT_TRUE(doc->find_first("p") != nullptr);
    ASSERT_TRUE(doc->find_first("ul") != nullptr);
    ASSERT_TRUE(doc->find_first("li") != nullptr);
}

// --- DOCTYPE ---

TEST(parse_doctype) {
    HtmlParser parser("<!DOCTYPE html><html><body><p>text</p></body></html>");
    auto doc = parser.parse();
    ASSERT_TRUE(doc != nullptr);
}

// --- Empty input ---

TEST(parse_empty_string) {
    HtmlParser parser("");
    auto doc = parser.parse();
    ASSERT_TRUE(doc != nullptr);
}

// --- Head element ---

TEST(parse_title_in_head) {
    HtmlParser parser("<html><head><title>My Page</title></head><body>content</body></html>");
    auto doc = parser.parse();

    const Node* title = doc->find_first("title");
    ASSERT_TRUE(title != nullptr);
    ASSERT_STREQ(title->text(), "My Page");
}

RUN_TESTS()