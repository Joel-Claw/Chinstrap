// font.hpp - Font rendering from scratch (TrueType/OpenType parsing + glyph rasterization)
//
// TEACHING NOTE: How font rendering works
// ===========================================================================
// Text rendering in a browser is one of the most complex subsystems. Here
// is what happens when you display a character on screen:
//
//   1. The character code (Unicode code point, e.g. U+0041 for 'A') is
//      mapped to a "glyph index" using the font cmap (character to glyph
//      mapping) table.
//
//   2. The glyph outline is loaded from the glyf (TrueType) or CFF (OpenType)
//      table. This outline is a set of quadratic (TrueType) or cubic (OpenType)
//      Bezier curves and lines that define the shape of the glyph.
//
//   3. The outline is "rasterized" - converted from vector curves to a bitmap
//      of pixels. This involves:
//      a. Scaling the outline to the desired pixel size
//      b. "Hinting" - adjusting the outline to align with the pixel grid
//         for crisp rendering at small sizes (TrueType hinting is extremely
//         complex; we implement a simplified version)
//      c. Scanline conversion - determining which pixels are inside the
//         outline and what fraction of each pixel is covered
//      d. Anti-aliasing - using sub-pixel coverage to smooth edges
//
//   4. The resulting bitmap is blitted to the display at the text position.
//
// Font file formats:
//   TrueType (.ttf): uses quadratic Bezier curves, has hinting instructions
//   OpenType (.otf): can use quadratic Bezier (TrueType outlines) or cubic
//     Bezier (PostScript/CFF outlines). CFF fonts do not have hinting
//     instructions in the same way; they use CFF hinting.
//   WOFF/WOFF2: web font formats, essentially compressed TrueType/OpenType
//
// We implement TrueType outline parsing and a simple scanline rasterizer
// with basic anti-aliasing. This is enough for a browser to render text
// legibly, though it will not match the quality of FreeType + Pango.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace chinstrap {

// TEACHING NOTE: Glyph metrics
// =========================================================================
// Each glyph has metrics that determine how it is positioned relative to
// other glyphs in a line of text:
//
//   advance_width: how far the pen moves after drawing this glyph
//   left_side_bearing: horizontal offset from the pen position to the
//     left edge of the glyph
//   bbox: bounding box of the glyph (min x/y, max x/y)
//
// For vertical text, there are similar vertical metrics, but we only
// support horizontal left-to-right text for simplicity.

struct GlyphMetrics {
    int advance_width;
    int left_side_bearing;
    int xmin, ymin, xmax, ymax;  // bounding box in font units
    bool has_outline;

    GlyphMetrics()
        : advance_width(0)
        , left_side_bearing(0)
        , xmin(0), ymin(0), xmax(0), ymax(0)
        , has_outline(false) {}
};

// TEACHING NOTE: Rasterized glyph bitmap
// =========================================================================
// After rasterization, each glyph is an 8-bit grayscale bitmap where each
// pixel value represents the coverage (0 = fully transparent, 255 = fully
// opaque). The bitmap has a width, height, and a bearing offset (x, y)
// that tells where to place it relative to the current pen position.

struct GlyphBitmap {
    int width;
    int height;
    int x_offset;  // offset from pen position to left edge of bitmap
    int y_offset;  // offset from pen position (baseline) to top of bitmap
    std::vector<uint8_t> pixels;  // width * height, 8-bit coverage values

    GlyphBitmap() : width(0), height(0), x_offset(0), y_offset(0) {}

    bool empty() const { return width == 0 || height == 0; }
    size_t size() const { return (size_t)width * height; }
};

// TEACHING NOTE: A point in a glyph outline
// =========================================================================
// Glyph outlines consist of on-curve points and off-curve points.
// On-curve points are vertices of the outline. Off-curve points are
// control points for quadratic Bezier curves.
// In TrueType, two consecutive off-curve points imply an on-curve point
// at their midpoint. This is a form of compression.

struct GlyphPoint {
    float x, y;
    bool on_curve;  // true = on curve (line/curve endpoint), false = control point
};

// TEACHING NOTE: A contour is a closed path of points
// =========================================================================
// A glyph consists of one or more contours. Each contour is a closed
// loop of line segments and quadratic Bezier curves. The direction of
// the contour matters: outer contours go clockwise, inner contours
// (holes) go counter-clockwise. The rasterizer uses the fill rule
// (non-zero or even-odd) to determine which regions are filled.

struct GlyphContour {
    std::vector<GlyphPoint> points;
};

// TEACHING NOTE: The Font class
// =========================================================================
// The Font class loads a TrueType/OpenType font file and provides:
//   - Glyph index lookup (Unicode code point -> glyph index via cmap)
//   - Glyph metrics (advance width, bounding box)
//   - Glyph outline extraction (contours with points)
//   - Glyph rasterization (outline -> bitmap with anti-aliasing)
//   - Text layout (string of characters -> positioned glyph sequence)

class Font {
public:
    Font();
    ~Font();

    // Load a font from a file path
    // Loads the TrueType/OpenType tables and parses the key tables:
    //   - head: font header (units per em, etc.)
    //   - cmap: character to glyph mapping
    //   - hmtx: horizontal metrics
    //   - hhea: horizontal header
    //   - glyf: glyph outlines (TrueType)
    //   - loca: glyph location index (into glyf)
    //   - maxp: maximum profile (number of glyphs)
    //   - name: font name strings
    bool load(const std::string& path);

    // Check if font is loaded
    bool is_loaded() const { return m_loaded; }

    // Get font family name
    const std::string& get_family_name() const { return m_family_name; }

    // Get units per em (the coordinate system scale of the font)
    int get_units_per_em() const { return m_units_per_em; }

    // Get ascent in font units (distance from baseline to top of ascenders)
    int get_ascent() const { return m_ascent; }

    // Get descent in font units (distance from baseline to bottom of descenders,
    // typically negative)
    int get_descent() const { return m_descent; }

    // Get line gap in font units
    int get_line_gap() const { return m_line_gap; }

    // Map a Unicode code point to a glyph index
    uint32_t get_glyph_index(uint32_t codepoint) const;

    // Get glyph metrics (in font units)
    GlyphMetrics get_glyph_metrics(uint32_t glyph_index) const;

    // Get glyph outline (contours)
    std::vector<GlyphContour> get_glyph_outline(uint32_t glyph_index) const;

    // Rasterize a glyph at the given pixel size
    // pixel_size is the desired height of the em square in pixels
    GlyphBitmap rasterize_glyph(uint32_t glyph_index, int pixel_size) const;

    // Rasterize a text string and return a bitmap + layout info
    // Returns combined bitmap of all glyphs laid out horizontally.
    // Also returns the total advance width (in pixels).
    struct TextLayout {
        GlyphBitmap bitmap;
        int advance_width;
    };
    TextLayout rasterize_text(const std::string& text, int pixel_size) const;

    // Find a font file on the system
    // Tries common font directories and font names
    static std::string find_font(const std::string& family = "");

private:
    bool m_loaded;
    std::string m_family_name;

    // Raw font file data
    std::vector<uint8_t> m_font_data;

    // Font table directory (offset + length for each table)
    struct TableEntry {
        uint32_t offset;
        uint32_t length;
        uint32_t checksum;
    };
    std::map<std::string, TableEntry> m_tables;

    // Font metrics (from head, hhea tables)
    int m_units_per_em;
    int m_ascent;
    int m_descent;
    int m_line_gap;
    int m_num_glyphs;

    // --- Table parsers ---
    void parse_table_directory();
    void parse_head();
    void parse_hhea();
    void parse_maxp();
    void parse_name();

    // --- cmap table (character to glyph mapping) ---
    // TEACHING NOTE: The cmap table maps Unicode code points to glyph
    // indices. There can be multiple subtables for different platforms
    // and encodings. We look for format 4 (16-bit Unicode) or format 12
    // (32-bit Unicode) subtables, which cover the vast majority of cases.
    struct CmapSubtable {
        uint16_t platform_id;
        uint16_t encoding_id;
        uint32_t offset;
        uint16_t format;
    };
    std::vector<CmapSubtable> m_cmap_subtables;
    int m_cmap_format;        // format of the subtable we use
    uint32_t m_cmap_offset;   // offset of the chosen subtable

    void parse_cmap();
    uint32_t cmap_lookup_format4(uint32_t codepoint) const;
    uint32_t cmap_lookup_format12(uint32_t codepoint) const;

    // --- hmtx table (horizontal metrics) ---
    struct LongHorMetric {
        uint16_t advance_width;
        int16_t lsb;
    };
    std::vector<LongHorMetric> m_hmtx;

    void parse_hmtx();
    LongHorMetric get_hmetric(uint32_t glyph_index) const;

    // --- loca table (glyph locations in glyf) ---
    std::vector<uint32_t> m_loca;  // offsets into glyf table

    void parse_loca();

    // --- glyf table (glyph outlines) ---
    // TEACHING NOTE: The glyf table contains the actual glyph outlines.
    // Each glyph is either:
    //   - Simple: a set of contours with on-curve and off-curve points
    //   - Composite: references to other glyphs with optional transforms
    // We handle simple glyphs fully and do a basic handling of composites.

    struct SimpleGlyphData {
        int num_contours;
        int x_min, y_min, x_max, y_max;
        std::vector<GlyphContour> contours;
    };

    SimpleGlyphData parse_simple_glyph(uint32_t offset, uint32_t length) const;
    GlyphBitmap rasterize_outline(const std::vector<GlyphContour>& contours,
                                   int pixel_size,
                                   const GlyphMetrics& metrics) const;

    // --- Helpers ---
    uint16_t read_u16(size_t offset) const;
    int16_t read_s16(size_t offset) const;
    uint32_t read_u32(size_t offset) const;
    int32_t read_s32(size_t offset) const;
    uint8_t read_u8(size_t offset) const;

    // Rasterizer helper: render a scanline using coverage values
    void rasterize_scanline(
        const std::vector<GlyphContour>& contours,
        float scale, float y,
        std::vector<float>& coverage
    ) const;

    // Evaluate a quadratic Bezier curve at parameter t
    static void quad_bezier(
        float x0, float y0,
        float x1, float y1,
        float x2, float y2,
        float t,
        float& out_x, float& out_y
    );
};

} // namespace chinstrap