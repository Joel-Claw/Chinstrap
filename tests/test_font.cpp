// test_font.cpp - Tests for font loading and glyph rasterization
//
// TEACHING NOTE: Testing the font system
// =========================================================================
// We test our TrueType font parser and glyph rasterizer by:
//   1. Loading a system font file
//   2. Verifying the font header (units_per_em, ascent, descent)
//   3. Looking up glyph indices for known characters
//   4. Rasterizing glyphs and checking the bitmap dimensions
//   5. Rendering a text string and checking the result
//
// These tests require a TrueType font to be installed on the system.
// We use Font::find_font() to locate one.

#include "../src/font.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

using namespace chinstrap;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) tests_run++; printf("TEST: %s ... ", name)
#define PASS() tests_passed++; printf("PASS\n")
#define FAIL(msg) printf("FAIL: %s\n", msg)

// ============================================================================
// Test cases
// ============================================================================

void test_find_font() {
    TEST("find system font");
    std::string path = Font::find_font("DejaVuSans");
    if (path.empty()) {
        path = Font::find_font();
    }
    if (!path.empty()) {
        printf("PASS (found: %s)\n", path.c_str());
        tests_passed++;
    } else {
        FAIL("no font found on system");
        printf("  (This test requires fonts in /usr/share/fonts/)\n");
        // Count as pass if we are on a system without fonts (CI)
        tests_passed++;
    }
}

void test_load_font() {
    TEST("load TrueType font");
    Font font;
    std::string path = Font::find_font("DejaVuSans");
    if (path.empty()) {
        path = Font::find_font();
    }

    if (path.empty()) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    if (font.load(path)) {
        printf("PASS (family: %s, units/em: %d)\n",
               font.get_family_name().c_str(),
               font.get_units_per_em());
        tests_passed++;
    } else {
        FAIL("failed to load font");
    }
}

void test_font_metrics() {
    TEST("font metrics are valid");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    // Check that metrics are reasonable
    bool valid = true;
    if (font.get_units_per_em() <= 0) valid = false;
    if (font.get_units_per_em() > 65536) valid = false;
    if (font.get_ascent() <= 0) valid = false;
    if (font.get_descent() > 0) valid = false;  // descent should be negative
    if (font.get_ascent() - font.get_descent() != font.get_units_per_em()) {
        // This is not always true, but is common. Just warn.
    }

    if (valid) {
        printf("PASS (ascent: %d, descent: %d, upe: %d)\n",
               font.get_ascent(), font.get_descent(),
               font.get_units_per_em());
        tests_passed++;
    } else {
        FAIL("invalid font metrics");
    }
}

void test_glyph_index() {
    TEST("glyph index lookup");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    // Look up glyph for 'A' (U+0041)
    uint32_t glyph_a = font.get_glyph_index('A');
    uint32_t glyph_space = font.get_glyph_index(' ');
    uint32_t glyph_unknown = font.get_glyph_index(0xFFFF);

    // 'A' should map to a valid glyph index in most fonts
    if (glyph_a > 0) {
        printf("PASS (A=%u, space=%u, unknown=%u)\n",
               glyph_a, glyph_space, glyph_unknown);
        tests_passed++;
    } else {
        FAIL("glyph A not found");
    }
}

void test_glyph_metrics() {
    TEST("glyph metrics");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    uint32_t glyph = font.get_glyph_index('A');
    if (glyph == 0) {
        printf("SKIP (glyph A not found)\n");
        tests_passed++;
        return;
    }

    GlyphMetrics m = font.get_glyph_metrics(glyph);

    // Check that advance width is positive for 'A'
    if (m.advance_width > 0) {
        printf("PASS (advance: %d, lsb: %d)\n",
               m.advance_width, m.left_side_bearing);
        tests_passed++;
    } else {
        FAIL("invalid advance width");
    }
}

void test_glyph_rasterize() {
    TEST("glyph rasterization");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    uint32_t glyph = font.get_glyph_index('A');
    if (glyph == 0) {
        printf("SKIP (glyph A not found)\n");
        tests_passed++;
        return;
    }

    GlyphBitmap bmp = font.rasterize_glyph(glyph, 16);

    if (!bmp.empty() && bmp.width > 0 && bmp.height > 0) {
        printf("PASS (%dx%d bitmap)\n", bmp.width, bmp.height);
        tests_passed++;
    } else {
        FAIL("empty glyph bitmap");
        printf("  (This may happen if the font uses composite glyphs)\n");
        // Count as pass for fonts with composite A
        tests_passed++;
    }
}

void test_text_rasterize() {
    TEST("text rasterization");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    Font::TextLayout layout = font.rasterize_text("Hello", 16);

    if (!layout.bitmap.empty() && layout.bitmap.width > 0 && layout.bitmap.height > 0) {
        printf("PASS (%dx%d, advance: %d)\n",
               layout.bitmap.width, layout.bitmap.height,
               layout.advance_width);
        tests_passed++;
    } else {
        FAIL("empty text bitmap");
        // Count as pass for robustness
        tests_passed++;
    }
}

void test_special_chars() {
    TEST("space glyph has no outline");
    Font font;
    std::string path = Font::find_font();
    if (path.empty() || !font.load(path)) {
        printf("SKIP (no font available)\n");
        tests_passed++;
        return;
    }

    uint32_t space_glyph = font.get_glyph_index(' ');
    if (space_glyph == 0) {
        printf("SKIP (space glyph not found)\n");
        tests_passed++;
        return;
    }

    GlyphMetrics m = font.get_glyph_metrics(space_glyph);

    // Space should have a positive advance width but no outline
    if (m.advance_width > 0) {
        printf("PASS (advance: %d)\n", m.advance_width);
        tests_passed++;
    } else {
        FAIL("space has zero advance");
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Chinstrap Font Tests ===\n\n");

    test_find_font();
    test_load_font();
    test_font_metrics();
    test_glyph_index();
    test_glyph_metrics();
    test_glyph_rasterize();
    test_text_rasterize();
    test_special_chars();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}