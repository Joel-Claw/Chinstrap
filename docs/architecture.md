# Chinstrap Architecture

## Overview

Chinstrap is a web browser built from scratch with zero third-party libraries.
This document describes the architecture and data flow of the browser.

## The Browser Pipeline

Every web browser processes a URL through a pipeline of stages. Chinstrap
implements each stage from scratch, using only the C++17 standard library and
POSIX system calls.

```
                    Chinstrap Browser Pipeline

  +----------+    +-------+    +-----------+    +-----------+
  | URL      | -> | HTTP  | -> | HTML      | -> | CSS       |
  | Parser   |    | Client|    | Parser    |    | Parser    |
  +----------+    +-------+    +-----------+    +-----------+
       |              |              |              |
       v              v              v              v
  scheme://host   TCP socket    DOM tree      Stylesheet
  :port/path      send/recv     of nodes      of rules

  +-----------+    +----------+    +----------+
  | Layout    | -> | Renderer | -> | Screen   |
  | Engine    |    |          |    | /dev/fb0 |
  +-----------+    +----------+    +----------+
       |              |              |
       v              v              v
  Box tree        Pixel writes   Visible output
  (positions +    to framebuffer  on screen
   sizes)
```

## Stage 1: URL Parsing (`src/url.hpp`, `src/url.cpp`)

**Input:** A URL string (e.g., `http://example.com:80/path?q=1#frag`)
**Output:** A structured `Url` object with scheme, host, port, path, query, fragment

The URL parser implements RFC 3986. It breaks the URL into components that
the HTTP client needs to know where to connect and what to request.

```
  http://example.com:8080/path?query=1#fragment
  \___/   \_________/ \__/ \__/ \_______/ \______/
  scheme     host     port path   query    fragment
```

## Stage 2: HTTP Client (`src/http.hpp`, `src/http.cpp`)

**Input:** Host, port, and path from the URL parser
**Output:** HTTP response body (HTML text)

The HTTP client uses raw POSIX sockets (`socket`, `connect`, `send`, `recv`)
to communicate with the web server. It handles:

- DNS resolution via `getaddrinfo()` (POSIX, not a library)
- TCP connection establishment
- HTTP/1.1 request formatting and sending
- HTTP response reading and parsing
- Chunked transfer encoding decoding
- Redirect following (301, 302, 303, 307, 308)

```
  Browser                          Server
    |                                |
    |--- DNS lookup (getaddrinfo) -->|
    |<-- IP address -----------------|
    |                                |
    |--- TCP SYN ------------------->|
    |<-- TCP SYN-ACK ----------------|
    |--- TCP ACK ------------------->|
    |                                |
    |--- HTTP GET /path ------------>|
    |<-- HTTP 200 OK + body ---------|
    |                                |
    |--- TCP FIN ------------------->|
    |<-- TCP ACK --------------------|
```

## Stage 3: HTML Parsing (`src/html_parser.hpp`, `src/html_parser.cpp`)

**Input:** HTML text from the HTTP response
**Output:** A DOM (Document Object Model) tree

The HTML parser has two phases:

1. **Tokenizer:** Breaks the HTML text into tokens (start tags, end tags,
   text, comments, DOCTYPE)

2. **Tree builder:** Constructs a DOM tree from the tokens, handling
   implicit tag creation and auto-closing rules

```
  HTML text               Tokens              DOM Tree
  -----------             ------              --------
  <div>                   StartTag(div)       Document
    <p>Hello</p>    ->     StartTag(p)    ->     html
  </div>                  Text("Hello")           head
                          EndTag(p)               body
                          EndTag(div)               div
                                                      p
                                                   "Hello"
```

## Stage 4: CSS Parsing (`src/css_parser.hpp`, `src/css_parser.cpp`)

**Input:** CSS text from `<style>` tags and inline `style` attributes
**Output:** A stylesheet with rules, and computed styles applied to the DOM

The CSS parser:
1. Parses CSS text into rules (selector + declarations)
2. The style engine matches selectors against DOM elements
3. Rules are sorted by specificity (ID > class > type)
4. Declarations are applied in specificity order (cascade)
5. Inherited properties propagate from parent to child

```
  CSS Rule                    Specificity
  -------------------         -------------------
  #id { color: red; }         (1, 0, 0) - highest
  .class { color: blue; }     (0, 1, 0)
  div { color: green; }       (0, 0, 1) - lowest
  * { color: black; }         (0, 0, 0) - universal
```

## Stage 5: Layout (`src/layout.hpp`, `src/layout.cpp`)

**Input:** DOM tree with computed styles + viewport dimensions
**Output:** Box tree with positions and sizes

The layout engine computes the geometry of every element:

1. Walk the DOM tree
2. For each element, determine if it is block or inline
3. Block elements stack vertically
4. Inline elements flow horizontally with text wrapping
5. Apply the box model (content, padding, border, margin)
6. Compute widths from available space and CSS width property
7. Compute heights from content or CSS height property

```
  +------------------------------+
  |  div (block)                 |
  |  +------------------------+  |
  |  |  p (block)             |  |
  |  |  +------------------+  |  |
  |  |  | "text" (inline)  |  |  |
  |  |  +------------------+  |  |
  |  +------------------------+  |
  +------------------------------+
```

## Stage 6: Rendering (`src/renderer.hpp`, `src/renderer.cpp`)

**Input:** Box tree with positions and sizes
**Output:** Pixels on the Linux framebuffer (`/dev/fb0`)

The renderer:
1. Opens `/dev/fb0` and memory-maps it with `mmap()`
2. Queries the framebuffer format (resolution, pixel format)
3. For each box, draws:
   - Background color (filled rectangle)
   - Border (rectangle outline)
   - Text content (bitmap font rendering)
4. Falls back to stdout output if framebuffer is unavailable

```
  Box tree               Framebuffer
  --------               -----------
  Box(x=0, y=0,       ->  +----------------+
    w=800, h=600,           |  ##########  |
    bg=white)               |  #          #  |
                            |  #  text    #  |
  Box(x=10, y=10,           |  #          #  |
    text="Hello")           |  ##########  |
                            +----------------+
```

## Configuration (`src/config.hpp`, `src/config.cpp`)

Configuration is loaded from a JSON file using our own JSON parser. Settings
include homepage URL, viewport dimensions, font settings, and colors.

## JSON Parser (`src/json.hpp`, `src/json.cpp`)

A recursive descent JSON parser used for configuration files. Supports all
JSON types: null, boolean, number, string, array, object.

## File Structure

```
chinstrap/
  CMakeLists.txt          Build configuration
  LICENSE                 CC0 Public Domain
  README.md               Project overview
  src/
    url.hpp / url.cpp       URL parser (RFC 3986)
    http.hpp / http.cpp     HTTP/1.1 client (POSIX sockets)
    html_parser.hpp / .cpp  HTML tokenizer + DOM tree builder
    css_parser.hpp / .cpp   CSS parser + style cascade engine
    layout.hpp / layout.cpp Layout engine (block + inline)
    renderer.hpp / .cpp      Framebuffer renderer
    json.hpp / json.cpp      JSON parser (for config)
    config.hpp / config.cpp Browser configuration
    main.cpp                 Entry point + pipeline orchestration
  tests/
    test_framework.hpp      Simple test macros
    test_url.cpp             URL parser tests
    test_http.cpp            HTTP client tests
    test_html_parser.cpp     HTML parser tests
    test_css_parser.cpp      CSS parser tests
    test_json.cpp            JSON parser tests
  docs/
    architecture.md          This file
    how-browsers-work.md     Comprehensive browser internals guide
```

## Design Principles

1. **Zero dependencies.** Only C++17 stdlib and POSIX. No exceptions.
2. **Heavy teaching comments.** Every file explains WHY, not just WHAT.
3. **Clean compilation.** -Wall -Wextra -Werror -Wpedantic. No warnings.
4. **Simplicity over completeness.** We implement enough to render simple
   pages. Complex features (TLS, JavaScript, flexbox) are documented as
   non-goals with explanations of how real browsers handle them.
5. **Readable code.** The code is the textbook. Comments teach concepts.