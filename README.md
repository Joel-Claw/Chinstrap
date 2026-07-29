# Chinstrap

<img src="assets/logo.svg" alt="Chinstrap logo" width="200">

A web browser built from scratch with **zero third-party libraries**. Only the
C++17 standard library and POSIX system calls. No rendering engine, no JS
engine, no network library, no HTML parser library, no CSS parser library, no
crypto library, no image decoder library. Everything written by hand to teach
how browsers actually work.

## Why?

Most developers use browsers every day but have no idea how they work inside.
Chinstrap strips away every abstraction layer. When you read this code, you
see the actual socket calls that fetch a web page, the actual state machine
that tokenizes HTML, the actual cascade algorithm that resolves CSS
specificity, the actual AES-NI instructions that encrypt TLS, the actual
mark-and-sweep garbage collector that manages JavaScript heap memory. Nothing
is hidden behind a library.

This is an educational project. It is not a production browser. It will not
replace Chrome or Firefox. But it will teach you what Chrome and Firefox are
actually doing under the hood.

## Subsystems

| Subsystem | Files | What It Does |
|---|---|---|
| URL Parser | `src/url.hpp` | Parses URLs per RFC 3986 (scheme, host, port, path, query, fragment) |
| DNS Resolver | `src/dns.hpp`, `src/dns.cpp` | Raw UDP DNS queries, no libc resolver |
| HTTP Client | `src/http.hpp`, `src/http.cpp` | HTTP/1.1 over raw POSIX sockets. GET, POST, chunked transfer |
| HTTP/2 | `src/http2.hpp`, `src/http2.cpp` | HTTP/2 with HPACK header compression |
| TLS 1.2 | `src/tls.hpp`, `src/tls.cpp` | TLS handshake, ECDHE_RSA_AES256_GCM_SHA384 |
| AES-256 | `src/aes.hpp`, `src/aes.cpp` | AES-256-GCM using CPU AES-NI instructions |
| SHA-256 | `src/sha256.hpp`, `src/sha256.cpp` | SHA-256 hash from scratch |
| Big Integer | `src/bigint.hpp`, `src/bigint.cpp` | Arbitrary-precision math for RSA |
| X.509 | `src/x509.hpp`, `src/x509.cpp` | Certificate parsing and verification |
| HTML Parser | `src/html_parser.hpp`, `src/html_parser.cpp` | HTML5 tokenizer + DOM tree builder |
| CSS Parser | `src/css_parser.hpp`, `src/css_parser.cpp` | Stylesheet parser, selectors, specificity cascade |
| Layout Engine | `src/layout.hpp`, `src/layout.cpp` | Block and inline box layout, text wrapping |
| Renderer | `src/renderer.hpp`, `src/renderer.cpp` | Draws to Linux framebuffer or X11 window |
| Display | `src/display.hpp`, `src/display.cpp` | Framebuffer (/dev/fb0) and X11 protocol over raw sockets |
| X11 | `src/x11.hpp`, `src/x11.cpp` | Raw X11 protocol, no Xlib |
| Font | `src/font.hpp`, `src/font.cpp` | TrueType/OpenType parser, glyph rasterizer from scratch |
| Image Decoder | `src/image.hpp`, `src/image.cpp` | PNG, JPEG, GIF decoders from scratch |
| zlib/DEFLATE | `src/zlib.hpp`, `src/zlib.cpp` | DEFLATE decompressor for PNG and HTTP gzip |
| JS Lexer | `src/js_lexer.hpp`, `src/js_lexer.cpp` | JavaScript tokenizer |
| JS Parser | `src/js_parser.hpp`, `src/js_parser.cpp` | JavaScript parser, AST construction |
| JS Interpreter | `src/js_interpreter.hpp`, `src/js_interpreter.cpp` | Tree-walking JS interpreter |
| JS GC | `src/js_gc.hpp`, `src/js_gc.cpp` | Mark-and-sweep garbage collector |
| JS Builtins | `src/js_builtins.hpp`, `src/js_builtins.cpp` | Array, String, Math, JSON, Object builtins |
| JS DOM | `src/js_dom.hpp`, `src/js_dom.cpp` | DOM bindings for JavaScript |
| Cookies | `src/cookies.hpp`, `src/cookies.cpp` | Cookie jar, SameSite, HttpOnly, Secure |
| Cache | `src/cache.hpp`, `src/cache.cpp` | HTTP cache with TTL and validation |
| Storage | `src/storage.hpp`, `src/storage.cpp` | Key-value storage (Bitcask-style) |
| History | `src/history.hpp`, `src/history.cpp` | Browsing history with timestamps |
| Downloads | `src/download.hpp`, `src/download.cpp` | Download manager |
| JSON Parser | `src/json.hpp`, `src/json.cpp` | Minimal JSON parser for config files |
| Config | `src/config.hpp`, `src/config.cpp` | Browser config loaded from JSON |
| GUI | `src/gui.hpp`, `src/gui.cpp` | Widgets, address bar, tabs |
| Input | `src/input.hpp`, `src/input.cpp` | Keyboard and mouse input handling |
| Main | `src/main.cpp` | Startup sequence, arg parsing, pipeline orchestration |
| Plugins | `include/chinstrap/plugin_types.h`, `plugins/` | Shared library (.so) plugin system with dlopen |

## Documentation

- [Architecture Overview](docs/architecture.md) - Pipeline diagrams and subsystem relationships
- [How Browsers Work](docs/how-browsers-work.md) - Comprehensive guide linking real browser internals to our implementation

## Build Instructions

### Prerequisites

You need a C++17 compiler and CMake. That is it. No libraries to install.

### Debian / Ubuntu / Raspberry Pi OS

```bash
sudo apt install build-essential cmake
cd chinstrap
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Arch Linux

```bash
sudo pacman -S base-devel cmake
cd chinstrap
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Running

```bash
# Run the browser (framebuffer mode, needs root or video group membership)
sudo ./chinstrap http://example.com

# Run the browser (X11 mode, if DISPLAY is set)
./chinstrap http://example.com

# Run tests
ctest --output on-failure
```

### Test Results

All 16 test suites pass:

```
 1/16 test_aes .........................   Passed
 2/16 test_cache .......................   Passed
 3/16 test_cookies .....................   Passed
 4/16 test_css_parser ..................   Passed
 5/16 test_dns .........................   Passed
 6/16 test_font ........................   Passed
 7/16 test_html_parser .................   Passed
 8/16 test_http ........................   Passed
 9/16 test_image .......................   Passed
10/16 test_js_interpreter ..............   Passed
11/16 test_js_lexer ....................   Passed
12/16 test_js_parser ...................   Passed
13/16 test_json .......................   Passed
14/16 test_sha256 ......................   Passed
15/16 test_url .........................   Passed
16/16 test_zlib ........................   Passed

100% tests passed, 0 tests failed out of 16
```

## The Chinstrap Penguin

The Chinstrap penguin (*Pygoscelis antarcticus*) is known for climbing steep
cliffs and diving into icy waters. Building a browser from scratch is the
software equivalent: steep, cold, and totally worth it.

## License

CC0 1.0 Universal (Public Domain Dedication). See [LICENSE](LICENSE).