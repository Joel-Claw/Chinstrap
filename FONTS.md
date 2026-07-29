# Fonts

Chinstrap bundles CC0 (Public Domain) Unicode fonts and maps them to common
web font families. No third-party font libraries are required.

## Bundled CC0 Fonts

All fonts are in `assets/fonts/` and are licensed under CC0 1.0 Universal.

| Font       | Type       | Replaces              | Source                            |
|------------|------------|-----------------------|-----------------------------------|
| Aileron    | Sans-serif | Arial, Helvetica      | dot colon (via GitHub)            |
| Vegur      | Sans-serif | Verdana, Geneva       | dot colon (via GitHub)            |
| OSerif     | Serif      | Times New Roman, Times | GGBotNet CC0 Fonts collection     |
| Unitblock  | Monospace  | Courier New, Courier  | GGBotNet CC0 Fonts collection     |

## Font Loading Order

1. The renderer maps CSS font-family names to the appropriate bundled TTF font.
2. If the TTF font loads successfully, text is rendered using TrueType outlines
   with anti-aliasing (see `src/font.cpp` for the TTF parser implementation).
3. If TTF loading fails, the renderer falls back to the built-in 8x16 bitmap
   font defined in `src/renderer.cpp` (font_data()). This bitmap font covers
   printable ASCII characters 32-126.

## Attribution

- Aileron and Vegur are by Sora Sagano / dot colon, CC0 1.0
- OSerif and Unitblock are by GGBotNet, CC0 1.0
- The GGBotNet CC0 Fonts collection: https://github.com/ggbotnet/fonts-cc0

See `assets/fonts/README.md` for the full font mapping table and details.