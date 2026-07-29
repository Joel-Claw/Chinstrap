# Chinstrap

```
   .___.
   / ___\
  / /   \ \   ___      _ __
 | |     | | / __| ___| '__|
 | |_____| | \__ \/ _ \ |
  \_______/  |___/\___/_|
     \  /
      \_/
  Chinstrap Browser
  From scratch. Zero libraries.
```

A web browser built from scratch with **zero third-party libraries**. Only the
C++17 standard library and POSIX system calls. No rendering engine, no JS
engine, no network library, no HTML parser library, no CSS parser library.
Everything written by hand to teach how browsers actually work.

## Why?

Most developers use browsers every day but have no idea how they work inside.
Chinstrap strips away every abstraction layer. When you read this code, you
see the actual socket calls that fetch a web page, the actual state machine
that tokenizes HTML, the actual cascade algorithm that resolves CSS
specificity. Nothing is hidden behind a library.

This is an educational project. It is not a production browser. It will not
replace Chrome or Firefox. But it will teach you what Chrome and Firefox are
actually doing under the hood.

## Subsystems

| Subsystem | Files | What It Does |
|---|---|---|
| URL Parser | `src/url.hpp` | Parses URLs per RFC 3986 (scheme, host, port, path, query, fragment) |
| HTTP Client | `src/http.hpp`, `src/http.cpp` | HTTP/1.1 over raw POSIX sockets. GET, POST, chunked transfer |
| HTML Parser | `src/html_parser.hpp`, `src/html_parser.cpp` | HTML5 tokenizer + DOM tree builder |
| CSS Parser | `src/css_parser.hpp`, `src/css_parser.cpp` | Stylesheet parser, selectors, specificity cascade |
| Layout Engine | `src/layout.hpp`, `src/layout.cpp` | Block and inline box layout, text wrapping |
| Renderer | `src/renderer.hpp`, `src/renderer.cpp` | Draws to Linux framebuffer (/dev/fb0) |
| JSON Parser | `src/json.hpp`, `src/json.cpp` | Minimal JSON parser for config files |
| Config | `src/config.hpp`, `src/config.cpp` | Browser config loaded from JSON |
| Main | `src/main.cpp` | Startup sequence, arg parsing, pipeline orchestration |

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
# Run the browser (renders to /dev/fb0, needs root or video group membership)
sudo ./chinstrap http://example.com

# Run tests
ctest
```

### Running Tests

```bash
cd build
ctest --output-on-failure
```

## The Chinstrap Penguin

The Chinstrap penguin (*Pygoscelis antarcticus*) is known for climbing steep
cliffs and diving into icy waters. Building a browser from scratch is the
software equivalent: steep, cold, and totally worth it.

## License

CC0 1.0 Universal (Public Domain Dedication). See [LICENSE](LICENSE).

## Project Goals

- **Zero dependencies.** Only C++17 stdlib and POSIX. No exceptions.
- **Heavy teaching comments.** Every file explains WHY, not just WHAT.
- **Compiles with -Wall -Wextra -Werror -Wpedantic.** Clean code or no code.
- **Actually works.** Not a mockup. Real sockets, real parsing, real rendering.

## Non-Goals

- TLS/HTTPS support (would need a crypto library, breaking the zero-dependency rule)
- JavaScript (a JS engine is a project unto itself)
- Production-level performance or security
- Replacing any real browser