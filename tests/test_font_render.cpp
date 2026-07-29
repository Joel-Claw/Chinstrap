// test_font_render.cpp - Visual test for font rasterization
//
// This test loads a bundled CC0 font, rasterizes several glyphs,
// and prints them as ASCII art to verify the output is recognizable.

#include "../src/font.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace chinstrap;

static void print_glyph_ascii(const GlyphBitmap& bmp, const char* label) {
    printf("=== %s (%dx%d, x_offset=%d, y_offset=%d) ===\n",
           label, bmp.width, bmp.height, bmp.x_offset, bmp.y_offset);
    if (bmp.empty()) {
        printf("(empty bitmap)\n\n");
        return;
    }
    for (int row = 0; row < bmp.height; ++row) {
        for (int col = 0; col < bmp.width; ++col) {
            uint8_t cov = bmp.pixels[(size_t)row * bmp.width + col];
            if (cov > 170) {
                putchar('#');
            } else if (cov > 85) {
                putchar('+');
            } else if (cov > 20) {
                putchar('.');
            } else {
                putchar(' ');
            }
        }
        putchar('\n');
    }
    putchar('\n');
}

int main(int argc, char* argv[]) {
    std::string font_path;
    if (argc > 1) {
        font_path = argv[1];
    } else {
        // Try bundled fonts
        const char* paths[] = {
            "assets/fonts/Aileron-Regular.ttf",
            "../assets/fonts/Aileron-Regular.ttf",
            "./assets/fonts/Aileron-Regular.ttf",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            FILE* fp = fopen(paths[i], "rb");
            if (fp) {
                fclose(fp);
                font_path = paths[i];
                break;
            }
        }
    }

    if (font_path.empty()) {
        // Try system font
        font_path = Font::find_font();
    }

    if (font_path.empty()) {
        fprintf(stderr, "No font found!\n");
        return 1;
    }

    printf("Loading font: %s\n", font_path.c_str());

    Font font;
    if (!font.load(font_path)) {
        fprintf(stderr, "Failed to load font!\n");
        return 1;
    }

    printf("Family: %s\n", font.get_family_name().c_str());
    printf("Units per em: %d\n", font.get_units_per_em());
    printf("Ascent: %d, Descent: %d, Line gap: %d\n\n",
           font.get_ascent(), font.get_descent(), font.get_line_gap());

    // Test glyph index lookups
    const char* test_chars = "ABCabc012Helo";
    printf("Glyph index lookups:\n");
    for (const char* p = test_chars; *p; ++p) {
        uint32_t idx = font.get_glyph_index((unsigned char)*p);
        GlyphMetrics m = font.get_glyph_metrics(idx);
        printf("  '%c' (0x%02X) -> glyph %u, advance=%d, lsb=%d\n",
               *p, (unsigned)*p, idx, m.advance_width, m.left_side_bearing);
    }
    printf("\n");

    // Rasterize and print each test character
    int pixel_size = 24;  // Larger size for better visibility
    for (const char* p = test_chars; *p; ++p) {
        uint32_t idx = font.get_glyph_index((unsigned char)*p);
        if (idx == 0) {
            printf("'%c' -> glyph index 0 (not found)\n\n", *p);
            continue;
        }
        GlyphBitmap bmp = font.rasterize_glyph(idx, pixel_size);
        char label[32];
        snprintf(label, sizeof(label), "Glyph '%c'", *p);
        print_glyph_ascii(bmp, label);
    }

    // Also test text rasterization
    printf("=== Text rasterization test: \"Hello\" ===\n");
    Font::TextLayout layout = font.rasterize_text("Hello", pixel_size);
    if (layout.bitmap.empty()) {
        printf("(empty text bitmap)\n");
    } else {
        printf("Text bitmap: %dx%d, advance=%d\n\n",
               layout.bitmap.width, layout.bitmap.height, layout.advance_width);
        for (int row = 0; row < layout.bitmap.height; ++row) {
            for (int col = 0; col < layout.bitmap.width; ++col) {
                uint8_t cov = layout.bitmap.pixels[(size_t)row * layout.bitmap.width + col];
                if (cov > 170) putchar('#');
                else if (cov > 85) putchar('+');
                else if (cov > 20) putchar('.');
                else putchar(' ');
            }
            putchar('\n');
        }
    }

    return 0;
}