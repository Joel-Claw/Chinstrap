// =========================================================================
// config.hpp - Browser Configuration
// =========================================================================
// TEACHING NOTE: Browsers have configuration files that store user
// preferences: homepage URL, window size, font settings, proxy settings,
// etc. Chrome uses a JSON-based config (Preferences file). Firefox uses
// a JavaScript-based config (prefs.js).
//
// We use our own JSON parser (from json.hpp) to load a JSON config file.
// This demonstrates how to use the JSON parser and shows how browsers
// manage configuration.
//
// Config file format (chinstrap.json):
// {
//   "homepage": "http://example.com",
//   "viewport": {
//     "width": 1024,
//     "height": 768
//   },
//   "font": {
//     "family": "monospace",
//     "size": 16
//   },
//   "colors": {
//     "background": "#ffffff",
//     "text": "#000000"
//   },
//   "user_agent": "Chinstrap/0.1"
// }
//
// We provide default values for all settings, so the config file is
// optional. If the file does not exist or cannot be parsed, we use
// defaults. This is how real browsers handle corrupt config files:
// they fall back to defaults rather than failing.
// =========================================================================

#ifndef CHINSTRAP_CONFIG_HPP
#define CHINSTRAP_CONFIG_HPP

#include <string>
#include <cstdint>

namespace chinstrap {

// -------------------------------------------------------------------------
// Config - Browser configuration settings
// -------------------------------------------------------------------------
// TEACHING NOTE: We store config as a simple struct with fields for
// each setting. This is the simplest approach. Real browsers use more
// sophisticated config systems (Chrome's PrefService, Firefox's nsIPrefBranch)
// that support dynamic updating, change notifications, and type checking.
// -------------------------------------------------------------------------

struct Config {
    // Homepage URL (loaded on startup if no URL is given on command line)
    std::string homepage = "http://example.com";

    // Viewport dimensions (for layout)
    int viewport_width = 1024;
    int viewport_height = 768;

    // Font settings
    std::string font_family = "monospace";
    int font_size = 16;

    // Colors
    std::string background_color = "#ffffff";
    std::string text_color = "#000000";

    // Custom user agent string
    std::string user_agent = "Chinstrap/0.1";

    // HTTP timeout in seconds
    int http_timeout = 30;

    // Load config from a JSON file
    // TEACHING NOTE: We use our JSON parser to read the config file.
    // If the file does not exist or is invalid, we keep the defaults.
    // This is the standard approach for config files: be lenient on
    // parse errors, never crash the browser because of a bad config.
    static Config load(const std::string& path);

    // Save config to a JSON file
    // TEACHING NOTE: We serialize the config back to JSON using our
    // JSON parser's serialization. This round-trip (load -> modify ->
    // save) is a common pattern in config management.
    void save(const std::string& path) const;

    // Create a default config file
    // TEACHING NOTE: If no config file exists, we create one with
    // default values. This makes it easy for users to discover and
    // modify settings.
    static void create_default(const std::string& path);
};

} // namespace chinstrap

#endif // CHINSTRAP_CONFIG_HPP