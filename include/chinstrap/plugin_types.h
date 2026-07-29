#ifndef CHINSTRAP_PLUGIN_TYPES_H
#define CHINSTRAP_PLUGIN_TYPES_H

// =========================================================================
// Chinstrap Plugin C API
// =========================================================================
// TEACHING NOTE: This header defines a C ABI (Application Binary Interface)
// for plugins. By using C function pointers and plain structs (no C++
// classes, no std::string, no virtual methods), any compiled plugin shared
// library (.so file) can work with the browser regardless of which compiler
// or C++ standard library version was used to build it.
//
// The plugin system works like this:
//   1. The browser loads a .so file at runtime with dlopen()
//   2. It looks for a function called "chinstrap_plugin_init" via dlsym()
//   3. That function returns a pointer to a ChinstrapPlugin struct
//   4. The browser calls the function pointers in that struct at appropriate times
//
// This is the same pattern used by many real applications:
//   - Web browsers (NPAPI, PPAPI plugins)
//   - Audio software (LV2, VST plugins)
//   - Image editors (GIMP plugins)
//   - Text editors (Vim plugins, though those use scripts not .so)
//
// The plugin lifecycle is:
//   1. init()     - Called once when the plugin is loaded
//   2. on_load()  - Called for each page load (if provided)
//   3. on_render()- Called before each render (if provided)
//   4. on_unload()- Called for each page unload (if provided)
//   5. destroy()  - Called once when the plugin is unloaded
// =========================================================================

#include <cstdint>
#include <cstddef>

// -------------------------------------------------------------------------
// Version macros
// -------------------------------------------------------------------------
// TEACHING NOTE: Version checking is critical for plugin safety. If the
// browser was compiled against plugin API v2 but a plugin was compiled
// against v1, the struct layout may differ and calling its function
// pointers would crash or corrupt memory.
//
// CHINSTRAP_PLUGIN_API_VERSION is a single integer that increments when
// the struct layout or function signatures change. Plugins check this at
// init time and refuse to load if the version does not match.
//
// CHINSTRAP_PLUGIN_API_VERSION_STRING is for human-readable logging.
// -------------------------------------------------------------------------
#define CHINSTRAP_PLUGIN_API_VERSION 1
#define CHINSTRAP_PLUGIN_API_VERSION_STRING "0.1.0"

// -------------------------------------------------------------------------
// Plugin type constants
// -------------------------------------------------------------------------
// TEACHING NOTE: The type field in ChinstrapPlugin tells the browser what
// kind of plugin this is. Different types get called at different points
// in the rendering pipeline.
//
//   CONTENT_PLUGIN  - Can modify page content (HTML/DOM)
//   FILTER_PLUGIN   - Can filter/modify network responses
//   RENDER_PLUGIN   - Hook into the rendering pipeline
//   UI_PLUGIN       - Add UI elements (toolbar buttons, menus)
// -------------------------------------------------------------------------
enum ChinstrapPluginType {
    CHINSTRAP_PLUGIN_CONTENT = 0,  // Modify page content during load
    CHINSTRAP_PLUGIN_FILTER  = 1,  // Filter network responses
    CHINSTRAP_PLUGIN_RENDER  = 2,  // Hook rendering pipeline
    CHINSTRAP_PLUGIN_UI      = 3,  // Add UI elements
};

// -------------------------------------------------------------------------
// Plugin capability flags
// -------------------------------------------------------------------------
// TEACHING NOTE: Capabilities are bitmask flags. A plugin can OR them
// together to declare what it wants to do. The browser checks these
// before calling certain hooks, so a plugin that does not declare
// CHINSTRAP_CAP_NETWORK will never receive network callbacks.
//
// This is a security measure: it limits what a plugin can do, similar
// to how Android apps declare permissions.
// -------------------------------------------------------------------------
enum ChinstrapPluginCaps {
    CHINSTRAP_CAP_NONE      = 0,       // No special capabilities
    CHINSTRAP_CAP_NETWORK   = 1 << 0,   // Can intercept network requests
    CHINSTRAP_CAP_DOM        = 1 << 1,   // Can read/modify the DOM
    CHINSTRAP_CAP_RENDER     = 1 << 2,   // Can modify rendering output
    CHINSTRAP_CAP_STORAGE   = 1 << 3,   // Can access local storage
    CHINSTRAP_CAP_UI         = 1 << 4,   // Can add UI elements
};

// -------------------------------------------------------------------------
// Plugin info struct
// -------------------------------------------------------------------------
// TEACHING NOTE: This struct is returned by the plugin init function.
// It contains metadata about the plugin (name, version, description) and
// function pointers for the plugin lifecycle callbacks.
//
// All strings are const char* (C-style, null-terminated). The plugin is
// responsible for keeping these strings alive for the lifetime of the
// plugin. In practice, they are usually string literals, which live
// forever in the data segment.
//
// The function pointers can be NULL if the plugin does not implement a
// particular callback. The browser must check for NULL before calling.
// -------------------------------------------------------------------------
struct ChinstrapPlugin {
    // --- Metadata ---
    const char* name;           // Human-readable plugin name
    const char* version;        // Plugin version string (e.g., "1.0.0")
    const char* description;    // Short description of what the plugin does
    const char* author;        // Author name(s)

    // --- Configuration ---
    int api_version;            // Must match CHINSTRAP_PLUGIN_API_VERSION
    int type;                   // One of ChinstrapPluginType
    uint32_t capabilities;      // Bitmask of ChinstrapPluginCaps

    // --- Lifecycle callbacks (all optional, can be NULL) ---

    // Called once when the plugin is first loaded into memory.
    // Return 0 on success, non-zero to abort loading.
    // The "ctx" parameter is an opaque pointer the browser uses to
    // pass plugin-specific context. The plugin should store it if
    // it needs to call back into the browser later.
    int  (*on_load)(void* ctx);

    // Called for each page that loads. The plugin can inspect or
    // modify the page URL. "url" is the page URL string.
    // Return 0 to allow the load, non-zero to block it.
    int  (*on_page_load)(void* ctx, const char* url);

    // Called before each render. The plugin can modify what gets drawn.
    // "width" and "height" are the viewport dimensions in pixels.
    void (*on_render)(void* ctx, int width, int height);

    // Called when a page is unloaded. Lets the plugin clean up
    // per-page state. "url" is the URL being unloaded.
    void (*on_page_unload)(void* ctx, const char* url);

    // Called once when the plugin is being unloaded from the browser.
    // The plugin should free any resources it allocated in on_load.
    void (*on_unload)(void* ctx);
};

// -------------------------------------------------------------------------
// Plugin init function signature
// -------------------------------------------------------------------------
// TEACHING NOTE: Every plugin must export a function with this exact
// name and signature. The browser calls dlsym(handle, "chinstrap_plugin_init")
// to find it. The function returns a pointer to a static ChinstrapPlugin
// struct (static so it stays alive after the function returns).
//
// The "api_version" parameter is the browser plugin API version. The
// plugin should check it against CHINSTRAP_PLUGIN_API_VERSION and
// return NULL if they do not match.
//
// Example implementation:
//
//   extern "C" const ChinstrapPlugin* chinstrap_plugin_init(int api_version) {
//       if (api_version != CHINSTRAP_PLUGIN_API_VERSION) return nullptr;
//       static ChinstrapPlugin plugin = { ... };
//       return &plugin;
//   }
//
// The "extern C" is essential: it tells the C++ compiler not to mangle
// the function name, so dlsym can find it by its plain C name.
// -------------------------------------------------------------------------
typedef const ChinstrapPlugin* (*ChinstrapPluginInitFn)(int api_version);

// -------------------------------------------------------------------------
// Convenience macro for declaring the plugin init function
// -------------------------------------------------------------------------
// TEACHING NOTE: This macro reduces boilerplate and ensures the function
// signature is always correct. Plugin authors use it like:
//
//   CHINSTRAP_PLUGIN_INIT(api_version) {
//       if (api_version != CHINSTRAP_PLUGIN_API_VERSION) return nullptr;
//       static ChinstrapPlugin plugin = { ... };
//       return &plugin;
//   }
//
// The "extern C" wrapper prevents name mangling so dlsym can find the
// function by its plain name "chinstrap_plugin_init".
// -------------------------------------------------------------------------
#define CHINSTRAP_PLUGIN_INIT(api_version) \
    extern "C" __attribute__((visibility("default"))) \
    const ChinstrapPlugin* chinstrap_plugin_init(int api_version)

#endif // CHINSTRAP_PLUGIN_TYPES_H