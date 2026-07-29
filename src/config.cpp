// =========================================================================
// config.cpp - Browser Configuration Implementation
// =========================================================================
// TEACHING NOTE: This file loads and saves browser configuration using
// our own JSON parser. It demonstrates how to use the JSON parser for
// real work, and how to handle missing/invalid config files gracefully.
//
// The pattern is:
//   1. Try to open and parse the config file
//   2. If it fails, use default values
//   3. Extract each setting from the JSON object
//   4. If a setting is missing, use the default for that setting
//
// This is exactly how real browsers handle their config files. They
// never crash because of a bad config. They log a warning and use defaults.
// =========================================================================

#include "config.hpp"
#include "json.hpp"

#include <fstream>
#include <iostream>

namespace chinstrap {

// Helper: safely get a string from a JSON object
// TEACHING NOTE: We use a helper function to safely extract values from
// the JSON object. If the key does not exist or the type is wrong, we
// return the default value. This is defensive programming.
static std::string get_string(const JsonObject& obj, const std::string& key, const std::string& def) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (!it->second.is_string()) return def;
    return it->second.as_string();
}

static int get_int(const JsonObject& obj, const std::string& key, int def) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (!it->second.is_number()) return def;
    return it->second.as_int();
}

Config Config::load(const std::string& path) {
    Config config;  // Start with defaults

    // Try to open the config file
    // TEACHING NOTE: We use std::ifstream to check if the file exists.
    // If it does not, we silently use defaults. If it exists but is
    // invalid JSON, we print a warning and use defaults.
    std::ifstream file(path);
    if (!file.is_open()) {
        // No config file: use defaults
        return config;
    }

    // Read the file
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    // Parse JSON
    JsonValue json;
    try {
        json = JsonValue::parse(content);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to parse config file " << path
                  << ": " << e.what() << ". Using defaults." << std::endl;
        return config;
    }

    if (!json.is_object()) {
        std::cerr << "Warning: Config file is not a JSON object. Using defaults." << std::endl;
        return config;
    }

    const JsonObject& obj = json.as_object();

    // Extract settings
    config.homepage = get_string(obj, "homepage", config.homepage);
    config.user_agent = get_string(obj, "user_agent", config.user_agent);
    config.background_color = get_string(obj, "background_color", config.background_color);
    config.text_color = get_string(obj, "text_color", config.text_color);
    config.font_family = get_string(obj, "font_family", config.font_family);
    config.font_size = get_int(obj, "font_size", config.font_size);
    config.http_timeout = get_int(obj, "http_timeout", config.http_timeout);
    config.viewport_width = get_int(obj, "viewport_width", config.viewport_width);
    config.viewport_height = get_int(obj, "viewport_height", config.viewport_height);

    // Extract nested viewport object (if present)
    auto vp_it = obj.find("viewport");
    if (vp_it != obj.end() && vp_it->second.is_object()) {
        const JsonObject& vp = vp_it->second.as_object();
        config.viewport_width = get_int(vp, "width", config.viewport_width);
        config.viewport_height = get_int(vp, "height", config.viewport_height);
    }

    // Extract nested font object (if present)
    auto font_it = obj.find("font");
    if (font_it != obj.end() && font_it->second.is_object()) {
        const JsonObject& font = font_it->second.as_object();
        config.font_family = get_string(font, "family", config.font_family);
        config.font_size = get_int(font, "size", config.font_size);
    }

    // Extract nested colors object (if present)
    auto colors_it = obj.find("colors");
    if (colors_it != obj.end() && colors_it->second.is_object()) {
        const JsonObject& colors = colors_it->second.as_object();
        config.background_color = get_string(colors, "background", config.background_color);
        config.text_color = get_string(colors, "text", config.text_color);
    }

    return config;
}

void Config::save(const std::string& path) const {
    // TEACHING NOTE: We build a JSON object and serialize it. This is
    // the inverse of load(). We create a nested structure that matches
    // the config file format documented in config.hpp.

    JsonObject root;
    root["homepage"] = JsonValue(homepage);
    root["user_agent"] = JsonValue(user_agent);
    root["background_color"] = JsonValue(background_color);
    root["text_color"] = JsonValue(text_color);
    root["font_family"] = JsonValue(font_family);
    root["font_size"] = JsonValue(static_cast<double>(font_size));
    root["http_timeout"] = JsonValue(static_cast<double>(http_timeout));
    root["viewport_width"] = JsonValue(static_cast<double>(viewport_width));
    root["viewport_height"] = JsonValue(static_cast<double>(viewport_height));

    // Nested viewport object
    JsonObject viewport;
    viewport["width"] = JsonValue(static_cast<double>(viewport_width));
    viewport["height"] = JsonValue(static_cast<double>(viewport_height));
    root["viewport"] = JsonValue(viewport);

    // Nested font object
    JsonObject font;
    font["family"] = JsonValue(font_family);
    font["size"] = JsonValue(static_cast<double>(font_size));
    root["font"] = JsonValue(font);

    // Nested colors object
    JsonObject colors;
    colors["background"] = JsonValue(background_color);
    colors["text"] = JsonValue(text_color);
    root["colors"] = JsonValue(colors);

    // Write to file
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Cannot write config file: " << path << std::endl;
        return;
    }

    file << JsonValue(root).to_string(2);
    file.close();
}

void Config::create_default(const std::string& path) {
    Config config;  // Use defaults
    config.save(path);
}

} // namespace chinstrap