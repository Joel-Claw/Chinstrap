// =========================================================================
// Example Chinstrap Plugin
// =========================================================================
// TEACHING NOTE: This file demonstrates how to write a plugin for the
// Chinstrap browser using the C plugin API defined in
// include/chinstrap/plugin_types.h.
//
// The plugin is compiled into a shared library (.so file) that the browser
// loads at runtime with dlopen(). The only requirement is that the plugin
// exports a function called "chinstrap_plugin_init" that returns a pointer
// to a ChinstrapPlugin struct.
//
// Build (standalone):
//   g++ -std=c++17 -shared -fPIC -I include -o example.so plugins/example/example_plugin.cpp
//
// Build (via CMake):
//   The CMakeLists.txt includes a target for this plugin. It builds as
//   part of the normal "make" command.
//
// This example plugin is a "content" plugin that:
//   1. Logs when it is loaded and unloaded (to stderr)
//   2. Logs each page load with the URL
//   3. Logs each page unload
//   4. Does NOT modify any content - it is a passive observer for teaching
//
// Real plugins would modify the DOM, filter network requests, add UI
// elements, etc. This one just demonstrates the lifecycle.
// =========================================================================

#include "chinstrap/plugin_types.h"

#include <cstdio>
#include <cstring>

// -------------------------------------------------------------------------
// Plugin state
// -------------------------------------------------------------------------
// TEACHING NOTE: In a real plugin, you would store mutable state here.
// For example, a content filter plugin might keep a list of blocked
// domains. The "ctx" pointer passed to callbacks can point to this struct.
//
// Here we keep a simple counter of how many pages have been loaded.
// -------------------------------------------------------------------------
struct ExamplePluginState {
    int pages_loaded;
    int pages_unloaded;
};

// Static state instance. Since there is only one instance of the plugin
// loaded at a time, a static variable is fine. For a more robust design
// you would allocate this in on_load and free it in on_unload.
static ExamplePluginState g_state = {0, 0};

// -------------------------------------------------------------------------
// Lifecycle callbacks
// -------------------------------------------------------------------------

// on_load - Called once when the plugin is loaded.
// TEACHING NOTE: This is where you initialize plugin state, open files,
// connect to databases, etc. The "ctx" parameter is an opaque pointer
// from the browser. Store it if you need to call back into the browser.
//
// We return 0 (success) to tell the browser the plugin is ready.
static int example_on_load(void* ctx) {
    (void)ctx;  // We do not use the browser context in this example
    fprintf(stderr, "[example_plugin] Loaded. Initializing state.\n");
    g_state.pages_loaded = 0;
    g_state.pages_unloaded = 0;
    return 0;  // 0 = success
}

// on_page_load - Called for each page that loads.
// TEACHING NOTE: The "url" parameter is the page URL string. In a real
// plugin, you could:
//   - Inspect the URL and block certain sites (return non-zero)
//   - Modify the page DOM before it renders
//   - Log browsing history (with user consent!)
//
// We just log the URL and increment the counter.
static int example_on_page_load(void* ctx, const char* url) {
    (void)ctx;
    g_state.pages_loaded++;
    fprintf(stderr, "[example_plugin] Page loaded: %s (total: %d)\n",
            url ? url : "(null)", g_state.pages_loaded);
    return 0;  // 0 = allow the page to load
}

// on_render - Called before each render.
// TEACHING NOTE: The "width" and "height" parameters are the viewport
// dimensions in pixels. A render plugin could:
//   - Draw overlays on top of the page
//   - Modify the rendering output
//   - Capture screenshots
//
// We do nothing here - this is a passive observer plugin.
static void example_on_render(void* ctx, int width, int height) {
    (void)ctx;
    (void)width;
    (void)height;
    // Intentionally empty: this plugin does not modify rendering.
}

// on_page_unload - Called when a page is unloaded.
// TEACHING NOTE: This is where you clean up per-page state. For example,
// if you allocated per-page memory in on_page_load, free it here.
static void example_on_page_unload(void* ctx, const char* url) {
    (void)ctx;
    g_state.pages_unloaded++;
    fprintf(stderr, "[example_plugin] Page unloaded: %s (total: %d)\n",
            url ? url : "(null)", g_state.pages_unloaded);
}

// on_unload - Called once when the plugin is unloaded.
// TEACHING NOTE: This is the last callback the plugin receives. Free any
// resources allocated in on_load. After this returns, the shared library
// may be dlclose()d, so do not use any memory that was allocated by the
// plugin after this point.
static void example_on_unload(void* ctx) {
    (void)ctx;
    fprintf(stderr, "[example_plugin] Unloaded. Stats: %d loaded, %d unloaded.\n",
            g_state.pages_loaded, g_state.pages_unloaded);
}

// -------------------------------------------------------------------------
// Plugin init function - The entry point
// -------------------------------------------------------------------------
// TEACHING NOTE: This is the function the browser calls via dlsym() after
// dlopen()ing the shared library. The CHINSTRAP_PLUGIN_INIT macro
// expands to:
//
//   extern "C" __attribute__((visibility("default")))
//   const ChinstrapPlugin* chinstrap_plugin_init(int api_version)
//
// Key points:
//   - extern "C" prevents C++ name mangling so dlsym can find the function
//   - __attribute__((visibility("default"))) ensures the symbol is
//     exported from the shared library (on Linux, symbols can be hidden
//     by default with -fvisibility=hidden)
//   - We return a pointer to a static struct so it persists after the
//     function returns
//
// The api_version check is a safety measure. If the browser was compiled
// with a different plugin API version, we refuse to initialize.
// -------------------------------------------------------------------------
CHINSTRAP_PLUGIN_INIT(api_version) {
    // Check that the browser plugin API version matches ours.
    // If they do not match, return NULL to tell the browser to skip us.
    if (api_version != CHINSTRAP_PLUGIN_API_VERSION) {
        fprintf(stderr, "[example_plugin] API version mismatch: browser=%d, plugin=%d\n",
                api_version, CHINSTRAP_PLUGIN_API_VERSION);
        return nullptr;
    }

    // Build the plugin struct. This is static so it stays alive after
    // the function returns. The browser holds onto this pointer for
    // the lifetime of the plugin.
    //
    // TEACHING NOTE: We use positional initialization (not designated
    // initializers like .name = ...) because designated initializers
    // are a C++20 feature and Chinstrap targets C++17. The fields must
    // be in the exact same order as declared in the ChinstrapPlugin
    // struct in plugin_types.h.
    static ChinstrapPlugin plugin = {
        // --- Metadata ---
        "Example Plugin",
        "1.0.0",
        "A passive observer plugin that logs page loads and unloads",
        "Chinstrap Project",

        // --- Configuration ---
        CHINSTRAP_PLUGIN_API_VERSION,
        CHINSTRAP_PLUGIN_CONTENT,
        CHINSTRAP_CAP_NONE,  // We only observe, no special access needed

        // --- Lifecycle callbacks ---
        example_on_load,
        example_on_page_load,
        example_on_render,
        example_on_page_unload,
        example_on_unload,
    };

    fprintf(stderr, "[example_plugin] Initialized successfully (API v%d).\n",
            CHINSTRAP_PLUGIN_API_VERSION);

    return &plugin;
}