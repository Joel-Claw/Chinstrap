// =========================================================================
// main.cpp - Chinstrap Browser Entry Point
// =========================================================================
// TEACHING NOTE: This is the main entry point for the Chinstrap browser.
// It ties together all the subsystems into the browser pipeline:
//
//   1. Parse command line arguments (URL to load, config path)
//   2. Load configuration from JSON file
//   3. Parse the URL using our URL parser
//   4. Resolve DNS and connect using our HTTP client
//   5. Send HTTP request and receive response
//   6. Parse the HTML response into a DOM tree
//   7. Parse any CSS (inline, in <style> tags, or linked stylesheets)
//   8. Apply CSS styles to the DOM (cascade)
//   9. Run layout to compute positions and sizes
//  10. Render to the Linux framebuffer (or stdout fallback)
//
// This pipeline is exactly what every browser does, though real browsers
// are much more sophisticated (multi-process, GPU acceleration, JavaScript
// execution, event loop, etc.).
//
// TEACHING NOTE: How Chrome starts up:
// Chrome is a multi-process browser. Each tab gets its own process
// (renderer process). The main process (browser process) manages the
// UI, bookmarks, and inter-process communication. The renderer process
// runs Blink (the rendering engine) and V8 (the JavaScript engine).
// Communication between processes uses Mojo (Chrome IPC system).
//
// Chinstrap is single-process. Everything happens in one process.
// This is fine for an educational browser but would not scale to
// modern web pages with JavaScript.
// =========================================================================

#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <poll.h>
#include <vector>
#include <cstring>
#include <sys/stat.h>

#include "url.hpp"
#include "http.hpp"
#include "html_parser.hpp"
#include "css_parser.hpp"
#include "layout.hpp"
#include "renderer.hpp"
#include "config.hpp"
#include "display.hpp"
#include "x11.hpp"
#include "wayland.hpp"

// =========================================================================
// print_usage - Show command line usage
// =========================================================================

static void print_usage(const char* program_name) {
    std::cout << "Chinstrap - A from-scratch web browser\n"
              << "\n"
              << "Usage: " << program_name << " [OPTIONS] [URL]\n"
              << "\n"
              << "Options:\n"
              << "  -c, --config FILE   Path to config file (default: chinstrap.json)\n"
              << "  -h, --help          Show this help message\n"
              << "  -v, --version       Show version information\n"
              << "\n"
              << "If no URL is given, the homepage from config is used.\n"
              << "\n"
              << "Example:\n"
              << "  " << program_name << " http://example.com\n"
              << "  " << program_name << " -c myconfig.json http://example.com\n";
}

// =========================================================================
// parse_args - Simple command line argument parser
// =========================================================================

struct CommandLineArgs {
    std::string url;
    std::string config_path = "chinstrap.json";
    std::string screenshot_path;  // --screenshot FILE
    bool show_help = false;
    bool show_version = false;
};

static CommandLineArgs parse_args(int argc, char* argv[]) {
    // TEACHING NOTE: Command line parsing is straightforward in C++.
    // We iterate over argv and match flags. Real browsers use more
    // sophisticated parsers (Chrome uses base::CommandLine) but the
    // concept is the same.
    //
    // We support:
    //   URL as first non-flag argument
    //   -c / --config FILE
    //   -h / --help
    //   -v / --version

    CommandLineArgs args;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            args.show_version = true;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                args.config_path = argv[++i];
            }
        } else if (arg.substr(0, 9) == "--config=") {
            args.config_path = arg.substr(9);
        } else if (arg == "--screenshot") {
            if (i + 1 < argc) {
                args.screenshot_path = argv[++i];
            }
        } else if (arg.substr(0, 13) == "--screenshot=") {
            args.screenshot_path = arg.substr(13);
        } else if (!arg.empty() && arg[0] != '-') {
            // URL argument
            if (args.url.empty()) {
                args.url = arg;
            }
        }
    }

    return args;
}

// =========================================================================
// extract_css - Extract CSS from <style> tags and inline styles
// =========================================================================

// TEACHING NOTE: CSS comes from three sources in HTML:
//   1. External stylesheets (<link rel="stylesheet" href="style.css">)
//      - These require additional HTTP requests. We could fetch them
//        but for simplicity we skip them for now.
//   2. Embedded styles (<style> ... </style>)
//      - These are in the HTML itself, inside <style> tags in <head>.
//        We extract the text content of these tags.
//   3. Inline styles (style="color: red" on elements)
//      - These are handled by the CSS engine during cascade.

static std::string extract_embedded_css(const chinstrap::Node& root) {
    std::string css;

    // Find all <style> elements and concatenate their text content
    auto style_elements = root.get_elements_by_tag("style");
    for (const auto* style_node : style_elements) {
        // Get text content of the <style> tag
        for (const auto& child : style_node->children) {
            if (child->type == chinstrap::NodeType::Text) {
                css += child->text_content;
                css += "\n";
            }
        }
    }

    return css;
}

// =========================================================================
// get_user_agent_stylesheet - Basic browser default styles
// =========================================================================
// TEACHING NOTE: Every browser has a user agent (UA) stylesheet that
// provides default styling for HTML elements. Without it, elements
// like h1 and p would have no margins, no font size differences, and
// no visual distinction. We define a minimal UA stylesheet here.

static std::string get_user_agent_stylesheet() {
    return std::string(
        // Body defaults
        "body { display: block; margin: 8px; color: black; "
        "background-color: white; font-size: 16px; line-height: 1.2; }\n"
        // Headings
        "h1 { display: block; font-size: 32px; margin-top: 21px; "
        "margin-bottom: 21px; font-weight: bold; }\n"
        "h2 { display: block; font-size: 24px; margin-top: 19px; "
        "margin-bottom: 19px; font-weight: bold; }\n"
        "h3 { display: block; font-size: 19px; margin-top: 18px; "
        "margin-bottom: 18px; font-weight: bold; }\n"
        "h4 { display: block; font-size: 16px; margin-top: 21px; "
        "margin-bottom: 21px; font-weight: bold; }\n"
        "h5 { display: block; font-size: 13px; margin-top: 22px; "
        "margin-bottom: 22px; font-weight: bold; }\n"
        "h6 { display: block; font-size: 11px; margin-top: 25px; "
        "margin-bottom: 25px; font-weight: bold; }\n"
        // Paragraphs
        "p { display: block; margin-top: 16px; margin-bottom: 16px; }\n"
        // Divs and sections
        "div { display: block; }\n"
        "section { display: block; }\n"
        "article { display: block; }\n"
        "header { display: block; }\n"
        "footer { display: block; }\n"
        "nav { display: block; }\n"
        "main { display: block; }\n"
        // Lists
        "ul { display: block; margin-top: 16px; margin-bottom: 16px; "
        "padding-left: 40px; }\n"
        "ol { display: block; margin-top: 16px; margin-bottom: 16px; "
        "padding-left: 40px; }\n"
        "li { display: block; }\n"
        // Inline elements
        "a { display: inline; color: blue; text-decoration: underline; }\n"
        "span { display: inline; }\n"
        "em { display: inline; font-style: italic; }\n"
        "strong { display: inline; font-weight: bold; }\n"
        "b { display: inline; font-weight: bold; }\n"
        "i { display: inline; font-style: italic; }\n"
        "code { display: inline; font-family: monospace; }\n"
        "pre { display: block; font-family: monospace; "
        "margin-top: 16px; margin-bottom: 16px; }\n"
        // Other block elements
        "blockquote { display: block; margin-top: 16px; margin-bottom: 16px; "
        "padding-left: 20px; }\n"
        "hr { display: block; border: 1px solid black; height: 0; }\n"
        // Images
        "img { display: inline; }\n"
        // Table elements
        "table { display: block; }\n"
        "tr { display: block; }\n"
        "td { display: block; padding: 1px; }\n"
        "th { display: block; padding: 1px; font-weight: bold; }\n"
        // Forms
        "form { display: block; }\n"
        "input { display: inline; }\n"
        "button { display: inline; }\n"
        // Meta elements (hidden)
        "script { display: none; }\n"
        "style { display: none; }\n"
        "head { display: none; }\n"
        "title { display: none; }\n"
        "meta { display: none; }\n"
        "link { display: none; }\n"
        "noscript { display: none; }\n"
    );
}

// =========================================================================
// detect_display_backend - Auto-detect the best available display backend
// =========================================================================
// TEACHING NOTE: Display backend auto-detection
// On a modern Linux system, multiple display systems may be available.
// We try them in order of preference:
//   1. Wayland - if WAYLAND_DISPLAY is set and the socket is reachable
//   2. X11 - if DISPLAY is set and the X server is reachable
//   3. Framebuffer - if /dev/fb0 exists (kiosk/embedded/headless mode)
// This allows Chinstrap to work in any environment without manual config.

static chinstrap::DisplayBackend detect_display_backend() {
    // Check for Wayland: WAYLAND_DISPLAY env var must be set
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (wayland_display && xdg_runtime) {
        // Try to see if the Wayland socket exists
        std::string path;
        if (wayland_display[0] == '/') {
            path = wayland_display;
        } else {
            path = std::string(xdg_runtime) + "/" + wayland_display;
        }
        // Check if the socket file exists
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFSOCK)) {
            return chinstrap::DisplayBackend::WAYLAND;
        }
    }

    // Check for X11: DISPLAY env var must be set
    const char* display = std::getenv("DISPLAY");
    if (display && display[0] != '\0') {
        return chinstrap::DisplayBackend::X11;
    }

    // Fall back to framebuffer
    return chinstrap::DisplayBackend::FRAMEBUFFER;
}

// =========================================================================
// main - Entry point
// =========================================================================

int main(int argc, char* argv[]) {
    // Parse command line arguments
    CommandLineArgs args = parse_args(argc, argv);

    if (args.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    if (args.show_version) {
        std::cout << "Chinstrap 0.1.0\n"
                  << "A from-scratch web browser\n"
                  << "Zero third-party libraries. C++17 and POSIX only.\n";
        return 0;
    }

    // Load configuration
    // TEACHING NOTE: We load the config file first because it contains
    // the homepage URL (used if no URL is given on the command line)
    // and viewport dimensions (used for layout).
    std::cout << "Loading configuration..." << std::endl;
    chinstrap::Config config = chinstrap::Config::load(args.config_path);

    // Determine URL to load
    std::string url_string = args.url.empty() ? config.homepage : args.url;

    // TEACHING NOTE: If the URL does not have a scheme, prepend "http://".
    // Real browsers do this too - if you type "example.com", they add
    // "http://" (or "https://" for browsers with HTTPS-first mode).
    if (url_string.find("://") == std::string::npos) {
        url_string = "http://" + url_string;
    }

    std::cout << "Chinstrap starting..." << std::endl;
    std::cout << "  URL: " << url_string << std::endl;
    std::cout << "  Viewport: " << config.viewport_width << "x" << config.viewport_height << std::endl;

    // Step 1: Parse the URL
    // TEACHING NOTE: This is the first step of the browser pipeline.
    // We parse the URL to extract the scheme, host, port, and path.
    // These components tell us where to connect and what to request.
    std::cout << "[1/6] Parsing URL..." << std::endl;
    chinstrap::Url url(url_string);
    if (!url.is_valid()) {
        std::cerr << "Error: Invalid URL: " << url_string << std::endl;
        return 1;
    }

    std::cout << "  Scheme: " << url.scheme() << std::endl;
    std::cout << "  Host: " << url.host() << std::endl;
    std::cout << "  Port: " << url.port() << std::endl;
    std::cout << "  Path: " << url.path() << std::endl;

    if (url.scheme() != "http") {
        std::cerr << "Error: Only HTTP is supported (no HTTPS/TLS)." << std::endl;
        std::cerr << "HTTPS requires a crypto library, which breaks the zero-dependency rule." << std::endl;
        return 1;
    }

    // Step 2: Fetch the URL via HTTP
    // TEACHING NOTE: Now we use our HTTP client to fetch the page.
    // This opens a TCP connection to the server, sends an HTTP GET
    // request, and reads the response. The response body is the HTML
    // that we will parse next.
    std::cout << "[2/6] Fetching " << url.host() << ":" << url.port() << url.path() << "..." << std::endl;

    chinstrap::HttpClient client;
    chinstrap::HttpResponse response;

    try {
        // Use send() with redirect following (get() does not follow redirects)
        chinstrap::HttpRequest req; req.host = url.host(); req.port = url.port(); req.path = url.path();
        response = client.send(req, 5);
    } catch (const std::exception& e) {
        std::cerr << "Error: HTTP request failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "  Status: " << response.status_code << " " << response.status_text << std::endl;
    std::cout << "  Content-Type: " << response.content_type() << std::endl;
    std::cout << "  Body size: " << response.body.size() << " bytes" << std::endl;

    if (!response.is_success()) {
        std::cerr << "Error: HTTP " << response.status_code << std::endl;
        if (!response.body.empty()) {
            std::cout << "--- Response body ---" << std::endl;
            std::cout << response.body << std::endl;
        }
        return 1;
    }

    // Step 3: Parse the HTML
    // TEACHING NOTE: Now we parse the HTML response body into a DOM tree.
    // The DOM tree is our internal representation of the page. CSS will
    // be applied to it, and layout will compute positions for each node.
    std::cout << "[3/6] Parsing HTML..." << std::endl;
    chinstrap::HtmlParser html_parser(response.body);
    auto document = html_parser.parse();

    if (!document) {
        std::cerr << "Error: Failed to parse HTML" << std::endl;
        return 1;
    }

    // Count elements for debugging
    auto all_elements = document->get_elements_by_tag("*");
    std::cout << "  Parsed " << all_elements.size() << " elements" << std::endl;

    // Step 4: Parse CSS
    // TEACHING NOTE: We extract CSS from <style> tags in the HTML and
    // parse it into a Stylesheet. Then we apply the styles to the DOM
    // tree using the cascade algorithm.
    std::cout << "[4/6] Parsing CSS..." << std::endl;

    // Parse user agent stylesheet (browser defaults)
    // TEACHING NOTE: The UA stylesheet provides default styles for HTML
    // elements. It is merged with the author stylesheet so that author
    // rules override UA defaults through the cascade.
    std::string ua_css = get_user_agent_stylesheet();
    chinstrap::CssParser ua_parser(ua_css);
    chinstrap::Stylesheet ua_stylesheet = ua_parser.parse();

    // Parse author stylesheet (page CSS)
    std::string css_text = extract_embedded_css(*document);
    chinstrap::CssParser css_parser(css_text);
    chinstrap::Stylesheet stylesheet = css_parser.parse();

    // Merge UA and author stylesheets (UA first = lower priority)
    chinstrap::Stylesheet combined;
    for (const auto& rule : ua_stylesheet.rules)
        combined.rules.push_back(rule);
    for (const auto& rule : stylesheet.rules)
        combined.rules.push_back(rule);

    std::cout << "  Parsed " << stylesheet.rules.size() << " CSS rules ("
              << ua_stylesheet.rules.size() << " UA)" << std::endl;

    // Apply combined styles to the DOM
    chinstrap::StyleEngine::apply_styles(combined, *document);

    // Step 5: Layout
    // TEACHING NOTE: Layout computes the position and size of every
    // element. We pass the DOM tree (with computed styles) and the
    // viewport dimensions to the layout engine.
    std::cout << "[5/6] Computing layout..." << std::endl;
    chinstrap::LayoutEngine layout_engine;
    auto root_box = layout_engine.layout(*document,
                                          static_cast<float>(config.viewport_width),
                                          static_cast<float>(config.viewport_height));

    std::cout << "  Root box: " << root_box->width << "x" << root_box->height << std::endl;

    // Step 6: Render
    // TEACHING NOTE: The final step is rendering. We detect the best
    // available display backend and use it to show the page.
    // TEACHING NOTE: We try Wayland first, then X11, then framebuffer.
    // This lets Chinstrap work in any desktop environment automatically.
    chinstrap::Renderer renderer;
    chinstrap::DisplayBackend backend = detect_display_backend();

    std::cout << "[6/6] Rendering..." << std::endl;

    // Screenshot mode: render to PPM file and exit (no display needed)
    // TEACHING NOTE: This is useful for headless screenshots and CI.
    if (!args.screenshot_path.empty()) {
        std::cout << "  Saving screenshot to " << args.screenshot_path
                  << "..." << std::endl;
        renderer.set_screenshot_url(url_string);
        renderer.render_to_ppm(*root_box, args.screenshot_path,
                                config.viewport_width, config.viewport_height);
        return 0;
    }

    // Try backends in order: detected backend first, then fallbacks.
    // TEACHING NOTE: On a Wayland desktop, XWayland is usually available,
    // so X11 often works even when Wayland is detected. We try the
    // detected backend first, then fall back to the others.
    chinstrap::DisplayBackend try_order[3];
    try_order[0] = backend;
    if (backend == chinstrap::DisplayBackend::WAYLAND) {
        try_order[1] = chinstrap::DisplayBackend::X11;
        try_order[2] = chinstrap::DisplayBackend::FRAMEBUFFER;
    } else if (backend == chinstrap::DisplayBackend::X11) {
        try_order[1] = chinstrap::DisplayBackend::WAYLAND;
        try_order[2] = chinstrap::DisplayBackend::FRAMEBUFFER;
    } else {
        try_order[1] = chinstrap::DisplayBackend::X11;
        try_order[2] = chinstrap::DisplayBackend::WAYLAND;
    }

    chinstrap::Display display;
    bool display_ok = false;
    std::string used_backend_name;

    for (int i = 0; i < 3 && !display_ok; i++) {
        const char* name = "unknown";
        switch (try_order[i]) {
            case chinstrap::DisplayBackend::WAYLAND: name = "Wayland"; break;
            case chinstrap::DisplayBackend::X11: name = "X11"; break;
            case chinstrap::DisplayBackend::FRAMEBUFFER: name = "Framebuffer"; break;
        }
        std::cout << "  Trying backend: " << name << "..." << std::endl;
        if (display.init(try_order[i], config.viewport_width, config.viewport_height)) {
            display_ok = true;
            used_backend_name = name;
            std::cout << "  Using backend: " << name << std::endl;
        } else {
            std::cout << "  Backend " << name << " failed" << std::endl;
        }
    }

    if (!display_ok) {
        // All backends failed - fall back to stdout rendering
        std::cout << "  No display backend available, rendering to stdout..." << std::endl;
        renderer.render(*root_box);
        return 0;
    }

    std::cout << "  Display: " << display.get_width() << "x"
              << display.get_height() << std::endl;

    // Render the page to an off-screen RGB buffer, then copy to the display.
    // TEACHING NOTE: We use render_to_buffer() which renders the full page
    // (including browser UI chrome) into a raw RGB pixel buffer. Then we
    // convert RGB to BGRA (the format X11 and Wayland expect) and write it
    // to the display back buffer, and call flip() to present it.
    std::cout << "  Rendering page..." << std::endl;
    renderer.set_screenshot_url(url_string);
    std::vector<uint8_t> rgb = renderer.render_to_buffer(
        *root_box, display.get_width(), display.get_height());

    // Convert RGB to BGRA for the display back buffer
    int dw = display.get_width();
    int dh = display.get_height();
    int dstride = display.get_stride();
    uint8_t* dbuf = display.get_buffer();
    if (dbuf && dstride >= dw * 4) {
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                size_t src_idx = static_cast<size_t>((y * dw + x) * 3);
                size_t dst_idx = static_cast<size_t>(y * dstride + x * 4);
                dbuf[dst_idx + 0] = rgb[src_idx + 2];  // B
                dbuf[dst_idx + 1] = rgb[src_idx + 1];  // G
                dbuf[dst_idx + 2] = rgb[src_idx + 0];  // R
                dbuf[dst_idx + 3] = 255;               // A (opaque)
            }
        }
    }

    display.set_title("Chinstrap - " + url_string);
    display.flip();
    std::cout << "  Rendering complete. Press Ctrl+C to exit." << std::endl;

    // Event loop: poll for events, redraw on expose, exit on close/Ctrl+C.
    // TEACHING NOTE: In a real browser, the event loop handles keyboard,
    // mouse, timer, network, and IPC events. Here we just poll the display
    // for events and exit when the window is closed or Ctrl+C is pressed.
    ::signal(SIGINT, [](int) { ::exit(0); });

    bool running = true;
    while (running) {
        // Sleep briefly to avoid consuming 100% CPU
        ::usleep(100000);  // 100ms

        // Process display events
        if (!display.process_events()) {
            // Window was closed
            running = false;
            std::cout << "  Window closed. Exiting." << std::endl;
        }
    }

    display.shutdown();
    return 0;
}