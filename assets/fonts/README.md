# CC0 Fonts bundled with Chinstrap

All fonts in this directory are licensed under CC0 1.0 Universal (Public Domain Dedication).

See LICENSE-CC0 for the full CC0 legal code.

## Font Inventory

### Aileron (sans-serif)
- **License:** CC0 1.0 Universal
- **Source:** https://github.com/reinhart1010/aileron (originally from dot colon)
- **Author:** Sora Sagano (dot colon)
- **Replaces:** Arial, Helvetica, sans-serif
- **Files:** Aileron-Regular.ttf, Aileron-Bold.ttf, Aileron-Italic.ttf, Aileron-Light.ttf
- **Units per em:** 1000

### Vegur (sans-serif)
- **License:** CC0 1.0 Universal
- **Source:** https://github.com/font-archive/Vegur (originally from dot colon)
- **Author:** Sora Sagano (dot colon)
- **Replaces:** Verdana, Geneva, sans-serif (secondary sans-serif)
- **Files:** Vegur-Regular.ttf, Vegur-Bold.ttf
- **Units per em:** 1000
- **Note:** Converted from OTF (CFF outlines) to TTF (TrueType outlines) using fonttools.

### OSerif (serif)
- **License:** CC0 1.0 Universal
- **Source:** https://github.com/ggbotnet/fonts-cc0 (GGBotNet CC0 Fonts collection)
- **Author:** GGBotNet
- **Replaces:** Times New Roman, Times, serif
- **Files:** OSerif-Regular.ttf, OSerif-Italic.ttf
- **Units per em:** 2048

### Unitblock (monospace)
- **License:** CC0 1.0 Universal
- **Source:** https://github.com/ggbotnet/fonts-cc0 (GGBotNet CC0 Fonts collection)
- **Author:** GGBotNet
- **Replaces:** Courier New, Courier, monospace
- **Files:** Unitblock-Regular.ttf
- **Units per em:** 8192

## CSS Font-Family Mapping

The Chinstrap browser maps CSS font-family names to bundled CC0 fonts:

| CSS font-family             | Bundled CC0 Font | Category    |
|-----------------------------|------------------|-------------|
| Arial, Helvetica            | Aileron          | sans-serif  |
| sans-serif                  | Aileron          | sans-serif  |
| Verdana, Geneva             | Vegur            | sans-serif  |
| Times New Roman, Times      | OSerif           | serif       |
| serif                       | OSerif           | serif       |
| Courier New, Courier        | Unitblock        | monospace   |
| monospace                   | Unitblock        | monospace   |
| (default / unknown)         | Aileron          | sans-serif  |

## Fallback Chain

1. Try loading the mapped TTF font from assets/fonts/
2. If TTF loading fails, fall back to the built-in 8x16 bitmap font
   (see src/renderer.cpp font_data())

The bitmap font covers printable ASCII (codes 32-126) and is always
available as a last resort.