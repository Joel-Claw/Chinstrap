// font.cpp - Font rendering from scratch (TrueType/OpenType)
//
// TEACHING NOTE: This file implements a TrueType font parser and glyph
// rasterizer from scratch. It reads .ttf font files, parses the binary
// table format, extracts glyph outlines as quadratic Bezier curves, and
// rasterizes them to anti-aliased bitmaps.
//
// This is a simplified implementation that covers the basics:
//   - TrueType outline parsing (glyf table, simple glyphs)
//   - cmap format 4 (16-bit Unicode BMP) and format 12 (full Unicode)
//   - Horizontal metrics (hmtx table)
//   - Basic scanline rasterization with anti-aliasing
//
// What we do NOT implement (for brevity):
//   - TrueType hinting instructions (very complex, ~100 instructions)
//   - Composite glyph transforms (we handle simple composites only)
//   - CFF/OpenType outlines (cubic Bezier curves instead of quadratic)
//   - Vertical text metrics
//   - Ligatures, kerning, complex text shaping (BiDi, etc.)
//   - Subpixel rendering (ClearType-style)

#include "font.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace chinstrap {

// ============================================================================
// Utility functions for reading big-endian font data
// ============================================================================

uint16_t Font::read_u16(size_t offset) const {
    if (offset + 2 > m_font_data.size()) return 0;
    return (uint16_t)((m_font_data[offset] << 8) | m_font_data[offset + 1]);
}

int16_t Font::read_s16(size_t offset) const {
    return (int16_t)read_u16(offset);
}

uint32_t Font::read_u32(size_t offset) const {
    if (offset + 4 > m_font_data.size()) return 0;
    return ((uint32_t)m_font_data[offset] << 24) |
           ((uint32_t)m_font_data[offset + 1] << 16) |
           ((uint32_t)m_font_data[offset + 2] << 8) |
           (uint32_t)m_font_data[offset + 3];
}

int32_t Font::read_s32(size_t offset) const {
    return (int32_t)read_u32(offset);
}

uint8_t Font::read_u8(size_t offset) const {
    if (offset >= m_font_data.size()) return 0;
    return m_font_data[offset];
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

Font::Font()
    : m_loaded(false)
    , m_units_per_em(0)
    , m_ascent(0)
    , m_descent(0)
    , m_line_gap(0)
    , m_num_glyphs(0)
    , m_cmap_format(0)
    , m_cmap_offset(0) {}

Font::~Font() {}

// ============================================================================
// Font loading
// ============================================================================

// TEACHING NOTE: TrueType/OpenType file format
// =========================================================================
// A font file starts with an offset table:
//   sfVersion (4 bytes): 0x00010000 for TrueType, 'OTTO' for OpenType with CFF
//   numTables (2 bytes): number of table directory entries
//   searchRange (2 bytes): (maximum power of 2 <= numTables) * 16
//   entrySelector (2 bytes): log2(maximum power of 2 <= numTables)
//   rangeShift (2 bytes): numTables * 16 - searchRange
//
// Then comes the table directory, with numTables entries, each 16 bytes:
//   tag (4 bytes): 4-character table identifier (e.g. 'cmap', 'glyf')
//   checksum (4 bytes): checksum of the table
//   offset (4 bytes): offset from start of file to table data
//   length (4 bytes): length of the table
//
// We read the directory and store the offset/length of each table by name.

bool Font::load(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    // Read entire file into memory
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return false;
    }

    m_font_data.resize(size);
    if (fread(m_font_data.data(), 1, size, fp) != (size_t)size) {
        fclose(fp);
        m_font_data.clear();
        return false;
    }
    fclose(fp);

    // Parse table directory
    parse_table_directory();

    // Verify we have the required tables
    if (m_tables.find("head") == m_tables.end() ||
        m_tables.find("cmap") == m_tables.end() ||
        m_tables.find("maxp") == m_tables.end()) {
        return false;
    }

    // Parse required tables
    parse_head();
    parse_maxp();
    parse_hhea();
    parse_name();
    parse_cmap();
    parse_hmtx();
    parse_loca();

    m_loaded = (m_units_per_em > 0 && m_num_glyphs > 0);
    return m_loaded;
}

// ============================================================================
// Table parsing
// ============================================================================

void Font::parse_table_directory() {
    if (m_font_data.size() < 12) return;

    // Read offset table header
    uint32_t sf_version = read_u32(0);
    (void)sf_version;  // We accept both 0x00010000 (TrueType) and 'OTTO'

    uint16_t num_tables = read_u16(4);

    // Each table directory entry is 16 bytes, starting at offset 12
    for (int i = 0; i < num_tables; ++i) {
        size_t entry_offset = 12 + (size_t)i * 16;
        if (entry_offset + 16 > m_font_data.size()) break;

        char tag[5] = {0};
        tag[0] = (char)m_font_data[entry_offset];
        tag[1] = (char)m_font_data[entry_offset + 1];
        tag[2] = (char)m_font_data[entry_offset + 2];
        tag[3] = (char)m_font_data[entry_offset + 3];

        TableEntry entry;
        entry.checksum = read_u32(entry_offset + 4);
        entry.offset = read_u32(entry_offset + 8);
        entry.length = read_u32(entry_offset + 12);

        m_tables[tag] = entry;
    }
}

// TEACHING NOTE: The 'head' table
// =========================================================================
// The head table contains global font information:
//   - units_per_em: the coordinate system scale (typically 1000 or 2048)
//   - index_to_loc_format: 0 for short offsets (loca), 1 for long offsets
//   - bounding box of all glyphs
//   - creation and modification dates
//   - macStyle flags (bold, italic, etc.)

void Font::parse_head() {
    auto it = m_tables.find("head");
    if (it == m_tables.end()) return;

    uint32_t offset = it->second.offset;

    // Skip version (4), fontRevision (4), checkSumAdjustment (4),
    // magicNumber (4), flags (2)
    // units_per_em is at offset 18
    m_units_per_em = read_u16(offset + 18);

    // Created (8), modified (8) - skip
    // xMin (2), yMin (2), xMax (2), yMax (2) - skip (font-wide bbox)
    // macStyle (2), lowestRecPPEM (2), fontDirectionHint (2)
    // indexToLocFormat (2) at offset 50
    // We read this later when parsing loca
}

// TEACHING NOTE: The 'hhea' table
// =========================================================================
// The hhea (horizontal header) table contains metrics for horizontal text:
//   - ascent: distance from baseline to top of ascenders
//   - descent: distance from baseline to bottom of descenders (negative)
//   - lineGap: extra space between lines
//   - numberOfHMetrics: number of entries in the hmtx table

void Font::parse_hhea() {
    auto it = m_tables.find("hhea");
    if (it == m_tables.end()) return;

    uint32_t offset = it->second.offset;

    // Skip version (4), ascender (2), descender (2)...
    // Actually, layout:
    //   0-3: version (fixed)
    //   4-5: ascent (int16)
    //   6-7: descent (int16)
    //   8-9: lineGap (int16)
    //   10-11: advanceWidthMax (uint16)
    //   ... more fields ...
    //   34-35: numberOfHMetrics (uint16)

    m_ascent = read_s16(offset + 4);
    m_descent = read_s16(offset + 6);
    m_line_gap = read_s16(offset + 8);
}

// TEACHING NOTE: The 'maxp' table
// =========================================================================
// The maxp (maximum profile) table tells us how many glyphs the font has.
// We need this to know the size of the hmtx and loca tables.

void Font::parse_maxp() {
    auto it = m_tables.find("maxp");
    if (it == m_tables.end()) return;

    uint32_t offset = it->second.offset;

    // Skip version (4 bytes), then numGlyphs (2 bytes at offset 4)
    m_num_glyphs = read_u16(offset + 4);
}

// TEACHING NOTE: The 'name' table
// =========================================================================
// The name table contains strings like the font family name, style name,
// copyright, etc. Each string has a platform, encoding, language, and name ID.
// We look for name ID 1 (family name) in platform 3 (Microsoft), encoding 1
// (Unicode BMP), language 0x0409 (English US). This is the standard location
// for font names on modern systems.

void Font::parse_name() {
    auto it = m_tables.find("name");
    if (it == m_tables.end()) return;

    uint32_t offset = it->second.offset;
    if (offset + 6 > m_font_data.size()) return;

    uint16_t format = read_u16(offset);
    uint16_t count = read_u16(offset + 2);
    uint16_t string_offset = read_u16(offset + 4);

    (void)format;

    // Name records start at offset + 6, each 12 bytes
    for (int i = 0; i < count; ++i) {
        size_t rec_offset = offset + 6 + (size_t)i * 12;
        if (rec_offset + 12 > m_font_data.size()) break;

        uint16_t platform_id = read_u16(rec_offset);
        uint16_t encoding_id = read_u16(rec_offset + 2);
        uint16_t language_id = read_u16(rec_offset + 4);
        (void)language_id;
        uint16_t name_id = read_u16(rec_offset + 6);
        uint16_t length = read_u16(rec_offset + 8);
        uint16_t str_offset = read_u16(rec_offset + 10);

        // Name ID 1 = Family Name
        // Prefer platform 3 (Microsoft), encoding 1 (Unicode BMP)
        if (name_id == 1 && platform_id == 3 && encoding_id == 1) {
            size_t abs_offset = offset + string_offset + str_offset;
            if (abs_offset + length > m_font_data.size()) continue;

            // Decode UTF-16BE string
            m_family_name.clear();
            for (uint16_t j = 0; j + 1 < length; j += 2) {
                uint16_t ch = (uint16_t)((m_font_data[abs_offset + j] << 8) |
                                          m_font_data[abs_offset + j + 1]);
                if (ch < 128) {
                    m_family_name += (char)ch;
                } else {
                    // For simplicity, skip non-ASCII characters in font name
                    m_family_name += '?';
                }
            }
            break;
        }
    }

    if (m_family_name.empty()) {
        m_family_name = "Unknown";
    }
}

// TEACHING NOTE: The 'cmap' table
// =========================================================================
// The cmap table is the most important table for text rendering - it maps
// Unicode code points to glyph indices. Without it, we cannot render text.
//
// The cmap table contains one or more subtables, each for a different
// platform/encoding combination. We look for:
//   - Platform 3 (Microsoft), Encoding 1 (Unicode BMP): format 4 subtable
//   - Platform 3 (Microsoft), Encoding 10 (Unicode full): format 12 subtable
//   - Platform 0 (Unicode): any format
//
// Format 4 is the most common and handles the Basic Multilingual Plane (BMP,
// code points 0-65535). Format 12 handles all of Unicode including
// supplementary planes.
//
// Format 4 structure:
//   - Segments of contiguous characters, each with start/end code points
//   - An optional offset to a glyph ID array for custom mappings
//   - The mapping for a code point C in segment i:
//     glyph_id = idDelta[i] + C (modulo 65536)
//     or glyph_id = glyphIdArray[C - startCount[i] + idRangeOffset[i]/2]
//     depending on whether idRangeOffset is zero

void Font::parse_cmap() {
    auto it = m_tables.find("cmap");
    if (it == m_tables.end()) return;

    uint32_t offset = it->second.offset;
    if (offset + 4 > m_font_data.size()) return;

    uint16_t version = read_u16(offset);
    uint16_t num_subtables = read_u16(offset + 2);
    (void)version;

    m_cmap_subtables.clear();

    for (int i = 0; i < num_subtables; ++i) {
        size_t rec_offset = offset + 4 + (size_t)i * 8;
        if (rec_offset + 8 > m_font_data.size()) break;

        CmapSubtable sub;
        sub.platform_id = read_u16(rec_offset);
        sub.encoding_id = read_u16(rec_offset + 2);
        sub.offset = read_u32(rec_offset + 4);

        // Read the format at the subtable offset
        if (offset + sub.offset + 2 > m_font_data.size()) continue;
        sub.format = read_u16(offset + sub.offset);

        m_cmap_subtables.push_back(sub);
    }

    // Choose the best subtable
    // Priority: format 12 > format 4 > anything else
    // Prefer platform 3 (Microsoft) or platform 0 (Unicode)
    m_cmap_format = 0;
    m_cmap_offset = 0;

    int best_priority = 0;
    for (const auto& sub : m_cmap_subtables) {
        int priority = 0;
        if (sub.platform_id == 3 && sub.encoding_id == 10 && sub.format == 12) {
            priority = 100;
        } else if (sub.platform_id == 0 && sub.format == 12) {
            priority = 90;
        } else if (sub.platform_id == 3 && sub.encoding_id == 1 && sub.format == 4) {
            priority = 80;
        } else if (sub.platform_id == 0 && sub.format == 4) {
            priority = 70;
        } else if (sub.format == 4) {
            priority = 50;
        } else if (sub.format == 12) {
            priority = 40;
        }

        if (priority > best_priority) {
            best_priority = priority;
            m_cmap_format = sub.format;
            m_cmap_offset = offset + sub.offset;
        }
    }
}

// TEACHING NOTE: cmap format 4 lookup
// =========================================================================
// Format 4 uses segments to represent contiguous character ranges.
// The structure (after the 14-byte header) contains parallel arrays:
//   endCode[numSegments]: last code point of each segment
//   (2 bytes padding)
//   startCode[numSegments]: first code point of each segment
//   idDelta[numSegments]: signed delta to add to code point
//   idRangeOffset[numSegments]: offset to glyph ID array
//
// For a code point C in segment i:
//   if idRangeOffset[i] == 0: glyph_id = (C + idDelta[i]) & 0xFFFF
//   else: glyph_id = glyphIdArray[...] (complex offset calculation)

uint32_t Font::cmap_lookup_format4(uint32_t codepoint) const {
    if (codepoint > 0xFFFF) return 0;
    if (m_cmap_offset == 0) return 0;

    uint16_t seg_count_x2 = read_u16(m_cmap_offset + 6);
    int seg_count = seg_count_x2 / 2;

    // endCode array starts at offset 14
    size_t end_code_start = m_cmap_offset + 14;
    size_t start_code_start = end_code_start + (size_t)seg_count * 2 + 2;  // +2 for padding
    size_t id_delta_start = start_code_start + (size_t)seg_count * 2;
    size_t id_range_offset_start = id_delta_start + (size_t)seg_count * 2;

    // Find the segment containing this code point
    for (int i = 0; i < seg_count; ++i) {
        uint16_t end_code = read_u16(end_code_start + (size_t)i * 2);
        if (codepoint <= end_code) {
            uint16_t start_code = read_u16(start_code_start + (size_t)i * 2);
            if (codepoint < start_code) {
                return 0;  // not in this segment
            }

            int16_t id_delta = read_s16(id_delta_start + (size_t)i * 2);
            uint16_t id_range_offset = read_u16(id_range_offset_start + (size_t)i * 2);

            if (id_range_offset == 0) {
                // Direct mapping: glyph = (codepoint + idDelta) mod 65536
                return (uint32_t)((codepoint + id_delta) & 0xFFFF);
            } else {
                // Indirect mapping through glyph ID array
                // The offset calculation is bizarre but specified in the standard:
                //   glyphIdArray[codepoint - startCode + idRangeOffset/2]
                // where the "address" of idRangeOffset[i] is used as a base
                size_t glyph_addr = id_range_offset_start + (size_t)i * 2 +
                                    id_range_offset +
                                    (size_t)(codepoint - start_code) * 2;

                if (glyph_addr + 2 > m_font_data.size()) return 0;
                uint16_t glyph_id = read_u16(glyph_addr);
                if (glyph_id == 0) return 0;
                return (uint32_t)((glyph_id + id_delta) & 0xFFFF);
            }
        }
    }

    return 0;  // not found
}

// TEACHING NOTE: cmap format 12 lookup
// =========================================================================
// Format 12 is simpler than format 4. It uses 32-bit values and a flat
// array of groups (startCharCode, endCharCode, startGlyphID).
// For a code point C in group i: glyph_id = C - startCharCode + startGlyphID

uint32_t Font::cmap_lookup_format12(uint32_t codepoint) const {
    if (m_cmap_offset == 0) return 0;

    // Format 12 header:
    //   0-1: format (12)
    //   2-3: reserved
    //   4-7: length
    //   8-11: language
    //   12-15: numGroups
    uint32_t num_groups = read_u32(m_cmap_offset + 12);

    // Groups start at offset 16, each 12 bytes
    for (uint32_t i = 0; i < num_groups; ++i) {
        size_t group_offset = m_cmap_offset + 16 + (size_t)i * 12;
        if (group_offset + 12 > m_font_data.size()) break;

        uint32_t start_char = read_u32(group_offset);
        uint32_t end_char = read_u32(group_offset + 4);
        uint32_t start_glyph = read_u32(group_offset + 8);

        if (codepoint >= start_char && codepoint <= end_char) {
            return codepoint - start_char + start_glyph;
        }
    }

    return 0;  // not found
}

uint32_t Font::get_glyph_index(uint32_t codepoint) const {
    if (!m_loaded) return 0;

    if (m_cmap_format == 12) {
        return cmap_lookup_format12(codepoint);
    } else if (m_cmap_format == 4) {
        return cmap_lookup_format4(codepoint);
    }

    return 0;
}

// TEACHING NOTE: The 'hmtx' table
// =========================================================================
// The hmtx (horizontal metrics) table contains the advance width and
// left side bearing for each glyph. It has numberOfHMetrics entries of
// (advance_width, lsb), followed by (numGlyphs - numberOfHMetrics) entries
// of just (lsb) - the remaining glyphs all use the last advance_width.
//
// We need this to know how much space to allocate between glyphs when
// laying out text.

void Font::parse_hmtx() {
    auto it = m_tables.find("hmtx");
    auto hhea_it = m_tables.find("hhea");
    if (it == m_tables.end() || hhea_it == m_tables.end()) return;

    // Read numberOfHMetrics from hhea table
    uint32_t hhea_offset = hhea_it->second.offset;
    uint16_t num_hmetrics = read_u16(hhea_offset + 34);

    uint32_t offset = it->second.offset;

    m_hmtx.clear();
    m_hmtx.reserve(num_hmetrics);

    for (int i = 0; i < num_hmetrics; ++i) {
        LongHorMetric metric;
        metric.advance_width = read_u16(offset + (size_t)i * 4);
        metric.lsb = read_s16(offset + (size_t)i * 4 + 2);
        m_hmtx.push_back(metric);
    }

    // Remaining glyphs use the last advance_width but have their own lsb
    // We store them as additional entries with the last advance_width
    if (!m_hmtx.empty()) {
        uint16_t last_advance = m_hmtx.back().advance_width;
        for (int i = num_hmetrics; i < m_num_glyphs; ++i) {
            LongHorMetric metric;
            metric.advance_width = last_advance;
            metric.lsb = read_s16(offset + (size_t)num_hmetrics * 4 + (size_t)(i - num_hmetrics) * 2);
            m_hmtx.push_back(metric);
        }
    }
}

Font::LongHorMetric Font::get_hmetric(uint32_t glyph_index) const {
    if (glyph_index < m_hmtx.size()) {
        return m_hmtx[glyph_index];
    }
    LongHorMetric m = {0, 0};
    return m;
}

GlyphMetrics Font::get_glyph_metrics(uint32_t glyph_index) const {
    GlyphMetrics m;
    LongHorMetric hmetric = get_hmetric(glyph_index);
    m.advance_width = hmetric.advance_width;
    m.left_side_bearing = hmetric.lsb;

    // Bounding box would come from the glyf table, but we do not parse
    // it here for metrics - we get it when parsing the outline.
    m.has_outline = false;
    return m;
}

// TEACHING NOTE: The 'loca' table
// =========================================================================
// The loca table is an index into the glyf table. It has numGlyphs + 1
// entries, each pointing to the start of a glyph in the glyf table.
// The format can be short (uint16, offset * 2) or long (uint32, direct offset),
// determined by the indexToLocFormat field in the head table.

void Font::parse_loca() {
    auto it = m_tables.find("loca");
    auto head_it = m_tables.find("head");
    if (it == m_tables.end() || head_it == m_tables.end()) return;

    uint32_t offset = it->second.offset;

    // Read indexToLocFormat from head table (at offset 50)
    uint32_t head_offset = head_it->second.offset;
    int16_t loc_format = read_s16(head_offset + 50);

    m_loca.clear();
    m_loca.reserve(m_num_glyphs + 1);

    if (loc_format == 0) {
        // Short format: entries are uint16, actual offset = value * 2
        for (int i = 0; i <= m_num_glyphs; ++i) {
            uint16_t val = read_u16(offset + (size_t)i * 2);
            m_loca.push_back((uint32_t)val * 2);
        }
    } else {
        // Long format: entries are uint32, direct offsets
        for (int i = 0; i <= m_num_glyphs; ++i) {
            m_loca.push_back(read_u32(offset + (size_t)i * 4));
        }
    }
}

// ============================================================================
// Glyph outline parsing
// ============================================================================

// TEACHING NOTE: Simple glyph parsing
// =========================================================================
// A simple glyph in TrueType format has this structure:
//   0-1: numberOfContours (int16, >= 0 for simple glyphs)
//   2-3: xMin (int16)
//   4-5: yMin (int16)
//   6-7: xMax (int16)
//   8-9: yMax (int16)
//
// Then for each contour: endPoints[i] (uint16) - index of the last point
// in contour i (points are shared by contours as a flat array).
//
// Then: instructionLength (uint16) + instructions (bytes) - hinting bytecode.
//
// Then the point data is stored in a compressed format with flags:
//   - x-coordinate: can be 1 byte, 2 bytes, or 0 bytes (same as previous)
//   - y-coordinate: same
//   - on_curve: flag bit indicating if the point is on the curve
//
// The flags byte has these bits:
//   0x01: on curve
//   0x02: x is short (1 byte) - if set, x-delta is 1 byte, else 2 bytes or 0
//   0x04: y is short
//   0x08: repeat flag (next byte = repeat count)
//   0x10: x is same/positive (if short, 0=negative, 1=positive; if long, 0=same, 1=different)
//   0x20: y is same/positive
//   0x40, 0x80: reserved

Font::SimpleGlyphData Font::parse_simple_glyph(uint32_t offset, uint32_t length) const {
    SimpleGlyphData data;
    data.num_contours = 0;

    if (length == 0) return data;

    // Find the glyf table
    auto it = m_tables.find("glyf");
    if (it == m_tables.end()) return data;

    uint32_t glyf_base = it->second.offset;
    size_t glyph_start = glyf_base + offset;

    if (glyph_start + 10 > m_font_data.size()) return data;

    int16_t num_contours = read_s16(glyph_start);
    data.x_min = read_s16(glyph_start + 2);
    data.y_min = read_s16(glyph_start + 4);
    data.x_max = read_s16(glyph_start + 6);
    data.y_max = read_s16(glyph_start + 8);

    if (num_contours < 0) {
        // Composite glyph - we do not fully support these.
        // Return empty outline.
        return data;
    }

    data.num_contours = num_contours;

    if (num_contours == 0) {
        // No outline (e.g. space character)
        return data;
    }

    // Read end points of contours
    std::vector<uint16_t> end_points(num_contours);
    for (int i = 0; i < num_contours; ++i) {
        end_points[i] = read_u16(glyph_start + 10 + (size_t)i * 2);
    }

    int num_points = end_points[num_contours - 1] + 1;

    // Skip past end points to get to instructions
    size_t pos = glyph_start + 10 + (size_t)num_contours * 2;
    uint16_t instr_len = read_u16(pos);
    pos += 2 + instr_len;  // skip instructions

    // Read flags
    std::vector<uint8_t> flags(num_points);
    for (int i = 0; i < num_points; ) {
        if (pos >= m_font_data.size()) return data;
        uint8_t flag = m_font_data[pos++];
        flags[i] = flag;
        i++;

        // Check repeat flag
        if (flag & 0x08) {
            if (pos >= m_font_data.size()) return data;
            uint8_t repeat = m_font_data[pos++];
            for (int r = 0; r < repeat && i < num_points; ++r) {
                flags[i] = flag;
                i++;
            }
        }
    }

    // Read x-coordinates (delta-encoded)
    std::vector<int32_t> x_coords(num_points);
    int32_t x = 0;
    for (int i = 0; i < num_points; ++i) {
        if (flags[i] & 0x02) {
            // Short format: 1 byte, unsigned
            uint8_t dx = m_font_data[pos++];
            x += (flags[i] & 0x10) ? dx : -dx;
        } else if (!(flags[i] & 0x10)) {
            // Long format: 2 bytes, signed
            int16_t dx = (int16_t)((m_font_data[pos] << 8) | m_font_data[pos + 1]);
            pos += 2;
            x += dx;
        }
        // else: same as previous (no change)
        x_coords[i] = x;
    }

    // Read y-coordinates (delta-encoded)
    std::vector<int32_t> y_coords(num_points);
    int32_t y = 0;
    for (int i = 0; i < num_points; ++i) {
        if (flags[i] & 0x04) {
            // Short format: 1 byte, unsigned
            uint8_t dy = m_font_data[pos++];
            y += (flags[i] & 0x20) ? dy : -dy;
        } else if (!(flags[i] & 0x20)) {
            // Long format: 2 bytes, signed
            int16_t dy = (int16_t)((m_font_data[pos] << 8) | m_font_data[pos + 1]);
            pos += 2;
            y += dy;
        }
        y_coords[i] = y;
    }

    // Build contours
    data.contours.resize(num_contours);
    int point_index = 0;
    for (int c = 0; c < num_contours; ++c) {
        int contour_end = end_points[c];
        for (; point_index <= contour_end; ++point_index) {
            GlyphPoint pt;
            pt.x = (float)x_coords[point_index];
            pt.y = (float)y_coords[point_index];
            pt.on_curve = (flags[point_index] & 0x01) != 0;
            data.contours[c].points.push_back(pt);
        }
    }

    return data;
}

std::vector<GlyphContour> Font::get_glyph_outline(uint32_t glyph_index) const {
    std::vector<GlyphContour> contours;

    if (!m_loaded || glyph_index >= (uint32_t)m_num_glyphs) {
        return contours;
    }

    if (glyph_index >= m_loca.size()) return contours;

    uint32_t glyph_offset = m_loca[glyph_index];
    uint32_t next_offset = m_loca[glyph_index + 1];
    uint32_t glyph_length = next_offset - glyph_offset;

    if (glyph_length == 0) return contours;  // empty glyph (e.g. space)

    SimpleGlyphData gd = parse_simple_glyph(glyph_offset, glyph_length);
    return gd.contours;
}

// ============================================================================
// Glyph rasterization
// ============================================================================

// TEACHING NOTE: Quadratic Bezier curve evaluation
// =========================================================================
// A quadratic Bezier curve has one control point (off-curve) between two
// endpoints (on-curve). The curve is evaluated as:
//   B(t) = (1-t)^2 * P0 + 2*(1-t)*t * P1 + t^2 * P2
// where t goes from 0 to 1.
//
// TrueType uses quadratic Beziers (one control point per segment).
// PostScript/CFF uses cubic Beziers (two control points per segment).
// We only support TrueType (quadratic).

void Font::quad_bezier(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float t,
    float& out_x, float& out_y
) {
    float u = 1.0f - t;
    float a = u * u;
    float b = 2.0f * u * t;
    float c = t * t;
    out_x = a * x0 + b * x1 + c * x2;
    out_y = a * y0 + b * y1 + c * y2;
}

// TEACHING NOTE: Scanline rasterization with anti-aliasing
// =========================================================================
// The rasterizer converts vector outlines to pixel bitmaps. The basic
// algorithm for anti-aliased scanline rendering:
//
//   1. For each scanline (row of pixels):
//      a. Find all intersections of the outline with the scanline
//      b. Sort intersections left to right
//      c. Fill between pairs of intersections (using the even-odd or
//         non-zero winding rule)
//      d. For anti-aliasing: supersample by evaluating the outline at
//         multiple sub-pixel y positions and averaging the coverage
//
//   2. The coverage of each pixel determines its alpha value (0-255).
//
// We use a simple approach: for each pixel row, we sample at multiple
// y positions (e.g. 4 sub-rows) and compute coverage for each. The final
// pixel value is the average coverage across the sub-rows.
//
// For the outline intersection, we flatten the Bezier curves into line
// segments at a fixed resolution, then do a simple line-scanline test.

void Font::rasterize_scanline(
    const std::vector<GlyphContour>& contours,
    float scale, float y,
    std::vector<float>& coverage
) const {
    // Fill coverage array with 0
    std::fill(coverage.begin(), coverage.end(), 0.0f);

    // For each contour, flatten the Bezier curves into line segments
    // and find x-intersections with the scanline y.
    // An intersection occurs where a line segment crosses y.
    // For each crossing, we toggle coverage from left to right.

    std::vector<float> crossings;

    for (const auto& contour : contours) {
        if (contour.points.empty()) continue;

        // Flatten the contour into line segments
        // We insert implied on-curve points between consecutive off-curve points
        std::vector<GlyphPoint> flat_points;
        for (size_t i = 0; i < contour.points.size(); ++i) {
            const GlyphPoint& pt = contour.points[i];
            const GlyphPoint& next = contour.points[(i + 1) % contour.points.size()];

            flat_points.push_back(pt);

            // If both current and next are off-curve, insert midpoint
            if (!pt.on_curve && !next.on_curve) {
                GlyphPoint mid;
                mid.x = (pt.x + next.x) * 0.5f;
                mid.y = (pt.y + next.y) * 0.5f;
                mid.on_curve = true;
                flat_points.push_back(mid);
            }
        }

        // Now walk through the flattened points and generate line segments
        // For each segment (p0, p1), if it crosses y, compute the x intersection
        for (size_t i = 0; i < flat_points.size(); ++i) {
            GlyphPoint p0 = flat_points[i];
            GlyphPoint p1;

            const GlyphPoint& next = flat_points[(i + 1) % flat_points.size()];

            if (p0.on_curve && next.on_curve) {
                // Line segment from p0 to next
                p1 = next;
            } else if (p0.on_curve && !next.on_curve) {
                // Start of a Bezier curve - skip, handled below
                continue;
            } else if (!p0.on_curve) {
                // p0 is a control point, p1 is the next on-curve point
                // This is the end of a Bezier curve
                // Find the start (previous on-curve point)
                const GlyphPoint& prev = flat_points[(i - 1 + flat_points.size()) % flat_points.size()];
                // Flatten the Bezier curve from prev -> p0 -> next into segments
                const int STEPS = 8;
                float prev_x = prev.x * scale;
                float prev_y = prev.y * scale;
                float ctrl_x = p0.x * scale;
                float ctrl_y = p0.y * scale;
                float next_x = next.x * scale;
                float next_y = next.y * scale;
                float bx = prev_x, by = prev_y;
                for (int s = 1; s <= STEPS; ++s) {
                    float t = (float)s / STEPS;
                    float cx, cy;
                    quad_bezier(prev_x, prev_y, ctrl_x, ctrl_y, next_x, next_y, t, cx, cy);
                    // Line segment from (bx, by) to (cx, cy)
                    if ((by <= y && cy > y) || (by > y && cy <= y)) {
                        float alpha = (y - by) / (cy - by);
                        float x_intersect = bx + alpha * (cx - bx);
                        crossings.push_back(x_intersect);
                    }
                    bx = cx;
                    by = cy;
                }
                continue;
            } else {
                continue;
            }

            // Line segment from p0 to p1 (scaled)
            float x0f = p0.x * scale;
            float y0f = p0.y * scale;
            float x1f = p1.x * scale;
            float y1f = p1.y * scale;

            if ((y0f <= y && y1f > y) || (y0f > y && y1f <= y)) {
                float alpha = (y - y0f) / (y1f - y0f);
                float x_intersect = x0f + alpha * (x1f - x0f);
                crossings.push_back(x_intersect);
            }
        }
    }

    // Sort crossings
    std::sort(crossings.begin(), crossings.end());

    // Fill between pairs of crossings (even-odd rule)
    for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
        float x0 = crossings[i];
        float x1 = crossings[i + 1];

        // Convert to pixel coordinates
        int px0 = (int)floor(x0);
        int px1 = (int)ceil(x1);

        for (int x = std::max(0, px0); x < std::min((int)coverage.size(), px1); ++x) {
            float left = std::max((float)x, x0);
            float right = std::min((float)(x + 1), x1);
            if (right > left) {
                coverage[x] += (right - left);
            }
        }
    }
}

GlyphBitmap Font::rasterize_outline(
    const std::vector<GlyphContour>& contours,
    int pixel_size,
    const GlyphMetrics& /*metrics*/
) const {
    GlyphBitmap bitmap;

    if (contours.empty() || m_units_per_em == 0) return bitmap;

    // Scale factor: convert from font units to pixels
    float scale = (float)pixel_size / (float)m_units_per_em;

    // Compute bounding box in pixels
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (const auto& contour : contours) {
        for (const auto& pt : contour.points) {
            int px = (int)(pt.x * scale);
            int py = (int)(pt.y * scale);
            if (px < min_x) min_x = px;
            if (px > max_x) max_x = px;
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
        }
    }

    // Add padding for anti-aliasing
    int pad = 2;
    min_x -= pad;
    max_x += pad;
    min_y -= pad;
    max_y += pad;

    int width = max_x - min_x + 1;
    int height = max_y - min_y + 1;

    if (width <= 0 || height <= 0) return bitmap;

    bitmap.width = width;
    bitmap.height = height;
    bitmap.x_offset = min_x;
    bitmap.y_offset = -max_y;  // Position relative to baseline (font Y-up to screen Y-down)
    bitmap.pixels.resize((size_t)width * height, 0);

    // TEACHING NOTE: Anti-aliasing via supersampling
    // =================================================================
    // For each pixel row, we sample the outline at multiple sub-pixel
    // y positions (4 samples) and average the coverage. This gives us
    // 4x4 = 16 levels of gray (plus we can do 4x horizontal supersampling
    // too for smoother edges, but vertical is the most important).
    //
    // A more sophisticated approach would use the "coverage" algorithm
    // from FreeType, which computes exact area coverage for each pixel.

    const int AA_SAMPLES = 4;

    for (int row = 0; row < height; ++row) {
        std::vector<float> coverage(width, 0.0f);

        for (int s = 0; s < AA_SAMPLES; ++s) {
            // Sub-pixel y position
            float y = (float)(min_y + row) + (float)s / AA_SAMPLES + 0.5f / AA_SAMPLES;
            std::vector<float> row_coverage(width, 0.0f);
            rasterize_scanline(contours, scale, y, row_coverage);
            for (int x = 0; x < width; ++x) {
                coverage[x] += row_coverage[x];
            }
        }

        // Average and convert to 0-255
        // Flip rows: font coords have Y going up (min_y at bottom),
        // but bitmap rows go top-to-bottom (row 0 = top of glyph = max_y).
        // So we store row 0 (which rasterizes at min_y + row = min_y)
        // at the BOTTOM of the bitmap, and the last row at the top.
        int flipped_row = height - 1 - row;
        for (int x = 0; x < width; ++x) {
            float avg = coverage[x] / AA_SAMPLES;
            if (avg < 0.0f) avg = 0.0f;
            if (avg > 1.0f) avg = 1.0f;
            bitmap.pixels[(size_t)flipped_row * width + x] = (uint8_t)(avg * 255.0f + 0.5f);
        }
    }

    return bitmap;
}

GlyphBitmap Font::rasterize_glyph(uint32_t glyph_index, int pixel_size) const {
    if (!m_loaded) return GlyphBitmap();

    std::vector<GlyphContour> contours = get_glyph_outline(glyph_index);
    if (contours.empty()) return GlyphBitmap();

    GlyphMetrics metrics = get_glyph_metrics(glyph_index);
    metrics.xmin = 0;
    metrics.ymin = 0;
    metrics.xmax = 0;
    metrics.ymax = 0;

    return rasterize_outline(contours, pixel_size, metrics);
}

// ============================================================================
// Text layout and rendering
// ============================================================================

// TEACHING NOTE: Text layout
// =========================================================================
// Text layout is the process of converting a string of characters into a
// sequence of positioned glyphs. For simple Latin text, this is just:
//   1. For each character, look up the glyph index
//   2. Rasterize the glyph at the current pen position
//   3. Advance the pen by the glyph advance width
//
// For complex scripts (Arabic, Indic, etc.), text layout requires:
//   - Unicode Bidirectional Algorithm (UAX #9) for RTL text
//   - Character clustering and reordering
//   - Contextual shaping (different glyphs for the same character based
//     on position - e.g. Arabic initial/medial/final forms)
//   - Ligatures (e.g. fi, fl in Latin, or lam-alef in Arabic)
//   - Kerning (adjusting spacing between specific glyph pairs)
//
// We implement only basic Latin LTR layout. A real browser would use
// HarfBuzz or Pango for complex text shaping.

Font::TextLayout Font::rasterize_text(const std::string& text, int pixel_size) const {
    TextLayout layout;
    layout.advance_width = 0;

    if (!m_loaded || text.empty()) return layout;

    float scale = (float)pixel_size / (float)m_units_per_em;

    // First pass: compute total width
    int total_width = 0;
    for (unsigned char ch : text) {
        uint32_t glyph = get_glyph_index(ch);
        GlyphMetrics m = get_glyph_metrics(glyph);
        total_width += (int)(m.advance_width * scale);
    }

    if (total_width <= 0) return layout;

    layout.advance_width = total_width;
    layout.bitmap.width = total_width;
    layout.bitmap.height = pixel_size;
    layout.bitmap.x_offset = 0;
    layout.bitmap.y_offset = 0;
    layout.bitmap.pixels.resize((size_t)total_width * pixel_size, 0);

    // Second pass: rasterize each glyph and composite into the layout
    int pen_x = 0;
    for (unsigned char ch : text) {
        uint32_t glyph = get_glyph_index(ch);
        GlyphBitmap gb = rasterize_glyph(glyph, pixel_size);

        if (!gb.empty()) {
            // Composite the glyph bitmap into the text bitmap
            // The glyph y_offset is from the baseline, which is at
            // (pixel_size - descent * scale) in the text bitmap
            int descent_px = (int)(m_descent * scale);
            int baseline_y = pixel_size - std::max(0, descent_px);

            int dst_x = pen_x + gb.x_offset;
            int dst_y = baseline_y + gb.y_offset;

            for (int y = 0; y < gb.height; ++y) {
                int dy = dst_y + y;
                if (dy < 0 || dy >= pixel_size) continue;
                for (int x = 0; x < gb.width; ++x) {
                    int dx = dst_x + x;
                    if (dx < 0 || dx >= total_width) continue;
                    uint8_t src = gb.pixels[(size_t)y * gb.width + x];
                    if (src > 0) {
                        uint8_t& dst = layout.bitmap.pixels[(size_t)dy * total_width + dx];
                        // Alpha blend: dst = max(dst, src) for simple compositing
                        if (src > dst) dst = src;
                    }
                }
            }
        }

        // Advance pen
        GlyphMetrics m = get_glyph_metrics(glyph);
        pen_x += (int)(m.advance_width * scale);
    }

    return layout;
}

// ============================================================================
// System font discovery
// ============================================================================

// TEACHING NOTE: Finding system fonts
// =========================================================================
// On Linux, fonts are typically installed in:
//   /usr/share/fonts/           - system-wide fonts
//   /usr/local/share/fonts/     - local system fonts
//   ~/.fonts/                   - user fonts (legacy location)
//   ~/.local/share/fonts/       - user fonts (modern location)
//
// We search these directories for .ttf files matching the requested family.
// If no family is specified, we look for common default fonts like
// DejaVu Sans, Liberation Sans, or any available .ttf file.

std::string Font::find_font(const std::string& family) {
    std::vector<std::string> search_dirs = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        std::string(getenv("HOME") ? getenv("HOME") : "") + "/.fonts",
        std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share/fonts",
    };

    // If a family is specified, look for files containing the family name
    std::vector<std::string> family_patterns;
    if (!family.empty()) {
        family_patterns.push_back(family);
    }
    // Default fonts to look for (in order of preference)
    family_patterns.push_back("DejaVuSans");
    family_patterns.push_back("dejavu");
    family_patterns.push_back("Liberation");
    family_patterns.push_back("FreeSans");
    family_patterns.push_back("NotoSans");

    for (const auto& dir_path : search_dirs) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.length() < 4) continue;

            std::string ext = name.substr(name.length() - 4);
            if (ext != ".ttf" && ext != ".TTF") continue;

            // Check if filename matches any pattern
            for (const auto& pattern : family_patterns) {
                // Case-insensitive substring match
                std::string lower_name = name;
                std::string lower_pattern = pattern;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);

                if (lower_name.find(lower_pattern) != std::string::npos) {
                    closedir(dir);
                    return dir_path + "/" + name;
                }
            }
        }
        closedir(dir);
    }

    // If nothing matched, return the first .ttf file found
    for (const auto& dir_path : search_dirs) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.length() < 4) continue;
            std::string ext = name.substr(name.length() - 4);
            if (ext == ".ttf" || ext == ".TTF") {
                closedir(dir);
                return dir_path + "/" + name;
            }
        }
        closedir(dir);
    }

    return "";  // no font found
}

} // namespace chinstrap