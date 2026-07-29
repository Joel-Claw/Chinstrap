// =========================================================================
// test_css_parser.cpp - Unit tests for CSS parser
// =========================================================================
// TEACHING NOTE: These tests exercise the CSS parser and style engine.
// We test:
//   - Stylesheet parsing (rules, selectors, declarations)
//   - Selector parsing (type, class, ID, descendant, child)
//   - Specificity calculation
//   - Declaration parsing (property: value, !important)
//   - Style application (cascade, inheritance)
// =========================================================================

#include "test_framework.hpp"
#include "css_parser.hpp"
#include "html_parser.hpp"

using namespace chinstrap;

// --- Basic parsing ---

TEST(parse_empty_stylesheet) {
    CssParser parser("");
    Stylesheet sheet = parser.parse();
    ASSERT_EQ(sheet.rules.size(), static_cast<std::size_t>(0));
}

TEST(parse_simple_rule) {
    CssParser parser("p { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "p");
    ASSERT_EQ(sheet.rules[0].declarations.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].declarations[0].property, "color");
    ASSERT_STREQ(sheet.rules[0].declarations[0].value, "red");
}

TEST(parse_multiple_rules) {
    CssParser parser("p { color: red; } div { background: blue; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules.size(), static_cast<std::size_t>(2));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "p");
    ASSERT_STREQ(sheet.rules[1].selector.parts[0].tag_name, "div");
}

// --- Selectors ---

TEST(parse_type_selector) {
    CssParser parser("div { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules[0].selector.parts.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "div");
}

TEST(parse_class_selector) {
    CssParser parser(".warning { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules[0].selector.parts[0].classes.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].classes[0], "warning");
}

TEST(parse_id_selector) {
    CssParser parser("#main { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_STREQ(sheet.rules[0].selector.parts[0].id, "main");
}

TEST(parse_compound_selector) {
    CssParser parser("div.warning#main { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "div");
    ASSERT_EQ(sheet.rules[0].selector.parts[0].classes.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].classes[0], "warning");
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].id, "main");
}

TEST(parse_descendant_selector) {
    CssParser parser("div p { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules[0].selector.parts.size(), static_cast<std::size_t>(2));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "div");
    ASSERT_STREQ(sheet.rules[0].selector.parts[1].tag_name, "p");
    ASSERT_EQ(static_cast<int>(sheet.rules[0].selector.combinators[1]),
              static_cast<int>(Combinator::Descendant));
}

TEST(parse_child_selector) {
    CssParser parser("div > p { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules[0].selector.parts.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(static_cast<int>(sheet.rules[0].selector.combinators[1]),
              static_cast<int>(Combinator::Child));
}

// --- Declarations ---

TEST(parse_multiple_declarations) {
    CssParser parser("p { color: red; font-size: 16px; margin: 0; }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules[0].declarations.size(), static_cast<std::size_t>(3));
    ASSERT_STREQ(sheet.rules[0].declarations[0].property, "color");
    ASSERT_STREQ(sheet.rules[0].declarations[0].value, "red");
    ASSERT_STREQ(sheet.rules[0].declarations[1].property, "font-size");
    ASSERT_STREQ(sheet.rules[0].declarations[1].value, "16px");
    ASSERT_STREQ(sheet.rules[0].declarations[2].property, "margin");
    ASSERT_STREQ(sheet.rules[0].declarations[2].value, "0");
}

TEST(parse_declaration_with_important) {
    CssParser parser("p { color: red !important; }");
    Stylesheet sheet = parser.parse();

    ASSERT_TRUE(sheet.rules[0].declarations[0].important);
    // The value should NOT contain !important
    ASSERT_STREQ(sheet.rules[0].declarations[0].value, "red");
}

TEST(parse_declaration_without_important) {
    CssParser parser("p { color: red; }");
    Stylesheet sheet = parser.parse();

    ASSERT_FALSE(sheet.rules[0].declarations[0].important);
}

// --- Specificity ---

TEST(specificity_type_selector) {
    CssParser parser("p { color: red; }");
    Stylesheet sheet = parser.parse();

    int a, b, c;
    sheet.rules[0].selector.specificity(a, b, c);
    ASSERT_EQ(a, 0);
    ASSERT_EQ(b, 0);
    ASSERT_EQ(c, 1);
}

TEST(specificity_class_selector) {
    CssParser parser(".cls { color: red; }");
    Stylesheet sheet = parser.parse();

    int a, b, c;
    sheet.rules[0].selector.specificity(a, b, c);
    ASSERT_EQ(a, 0);
    ASSERT_EQ(b, 1);
    ASSERT_EQ(c, 0);
}

TEST(specificity_id_selector) {
    CssParser parser("#id { color: red; }");
    Stylesheet sheet = parser.parse();

    int a, b, c;
    sheet.rules[0].selector.specificity(a, b, c);
    ASSERT_EQ(a, 1);
    ASSERT_EQ(b, 0);
    ASSERT_EQ(c, 0);
}

TEST(specificity_compound_selector) {
    CssParser parser("div.cls#id { color: red; }");
    Stylesheet sheet = parser.parse();

    int a, b, c;
    sheet.rules[0].selector.specificity(a, b, c);
    ASSERT_EQ(a, 1);
    ASSERT_EQ(b, 1);
    ASSERT_EQ(c, 1);
}

// --- CSS comments ---

TEST(parse_css_with_comments) {
    CssParser parser("/* comment */ p { color: red; /* inline */ }");
    Stylesheet sheet = parser.parse();

    ASSERT_EQ(sheet.rules.size(), static_cast<std::size_t>(1));
    ASSERT_STREQ(sheet.rules[0].selector.parts[0].tag_name, "p");
}

// --- Style application ---

TEST(apply_styles_basic) {
    HtmlParser html("<p>text</p>");
    auto doc = html.parse();

    CssParser css("p { color: red; }");
    Stylesheet sheet = css.parse();

    StyleEngine::apply_styles(sheet, *doc);

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ComputedStyle style = StyleEngine::get_computed_style(*p);
    ASSERT_STREQ(style["color"], "red");
}

TEST(apply_styles_specificity) {
    HtmlParser html("<p class=\"warning\">text</p>");
    auto doc = html.parse();

    // Two rules: type selector (low specificity) and class selector (higher)
    CssParser css("p { color: black; } .warning { color: red; }");
    Stylesheet sheet = css.parse();

    StyleEngine::apply_styles(sheet, *doc);

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ComputedStyle style = StyleEngine::get_computed_style(*p);
    // Class selector should win (higher specificity)
    ASSERT_STREQ(style["color"], "red");
}

TEST(apply_styles_inheritance) {
    HtmlParser html("<div><p>text</p></div>");
    auto doc = html.parse();

    // color is inherited
    CssParser css("div { color: blue; }");
    Stylesheet sheet = css.parse();

    StyleEngine::apply_styles(sheet, *doc);

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ComputedStyle style = StyleEngine::get_computed_style(*p);
    // p should inherit color from div
    ASSERT_STREQ(style["color"], "blue");
}

TEST(apply_inline_styles) {
    HtmlParser html("<p style=\"color: green;\">text</p>");
    auto doc = html.parse();

    CssParser css("");
    Stylesheet sheet = css.parse();

    StyleEngine::apply_styles(sheet, *doc);

    const Node* p = doc->find_first("p");
    ASSERT_TRUE(p != nullptr);
    ComputedStyle style = StyleEngine::get_computed_style(*p);
    // Inline style should apply
    ASSERT_STREQ(style["color"], "green");
}

RUN_TESTS()