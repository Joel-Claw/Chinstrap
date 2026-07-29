# How Browsers Work: A Guide for Developers

This document explains how real web browsers work, with references to
the corresponding Chinstrap implementation. Each section covers a major
browser subsystem.

## Table of Contents

1. [URL Parsing](#1-url-parsing)
2. [DNS Resolution](#2-dns-resolution)
3. [HTTP](#3-http)
4. [HTML Parsing](#4-html-parsing)
5. [CSS Cascade](#5-css-cascade)
6. [Layout](#6-layout)
7. [Painting](#7-painting)
8. [Compositing](#8-compositing)
9. [Event Handling](#9-event-handling)

---

## 1. URL Parsing

### How real browsers do it

When you type "example.com" in the address bar, the browser first parses
this into a structured URL. Modern browsers follow the [WHATWG URL
Living Standard](https://url.spec.whatwg.org/), which has some differences
from RFC 3986 (the IETF standard):

- WHATWG treats some schemes as "special" (http, https, ftp, ws, wss, file)
  with special parsing rules
- WHATWG handles international domain names via IDNA (punycode)
- WHATWG is more lenient with malformed input (it tries to fix it)
- WHATWG separates the URL parser into "basic" and "non-basic" paths

Chrome uses the `GURL` class (in `src/url/gurl.h`) which wraps the WHATWG
parser. Firefox uses `nsStandardURL` which also follows WHATWG.

### How Chinstrap does it

See `src/url.hpp` and `src/url.cpp`. We implement RFC 3986 with practical
extensions for HTTP. The parser is a manual character-by-character parser
(no regex library needed).

Key concepts:
- **Scheme:** The protocol (http, https). Determines the default port.
- **Authority:** The server identity (userinfo@host:port)
- **Path:** The resource location on the server
- **Query:** Parameters for the server (after ?)
- **Fragment:** A location within the page (after #). NOT sent to the server.

One important feature is **relative URL resolution**. When a page at
`http://example.com/foo/bar` has a link to `baz`, the browser resolves it
to `http://example.com/foo/baz`. This is defined in RFC 3986 Section 5.3.
Our `Url::resolve()` method implements this.

### What we skip
- IDNA/punycode (international domain names)
- WHATWG special scheme handling
- URL canonicalization beyond basic scheme/port normalization

---

## 2. DNS Resolution

### How real browsers do it

DNS (Domain Name System) translates human-readable hostnames like
"example.com" to IP addresses like 93.184.216.34. The process is:

1. Check the browser DNS cache (Chrome caches DNS results for ~60 seconds)
2. Check the OS resolver cache
3. Check `/etc/hosts` (on Linux)
4. Query the configured DNS server (UDP port 53)
5. The DNS server may recursively query other servers

Chrome also does **DNS prefetching**: it proactively resolves hostnames
for links on the current page, so navigation is faster when the user
clicks a link.

### How Chinstrap does it

We use the POSIX `getaddrinfo()` function, which handles all the DNS
complexity for us. This is part of POSIX, not a third-party library.
It handles:
- Querying the system resolver
- IPv4 and IPv6
- `/etc/hosts` file
- DNS caching (via the OS resolver)

See `src/http.cpp`, the `HttpClient::connect()` method.

### What we skip
- DNS caching (we rely on the OS)
- DNS prefetching
- DNS-over-HTTPS (DoH)
- Implementing the DNS protocol from scratch

---

## 3. HTTP

### How real browsers do it

HTTP (HyperText Transfer Protocol) is how browsers fetch web resources.
The protocol has evolved:

- **HTTP/1.0:** One request per TCP connection. Simple but slow.
- **HTTP/1.1:** Keep-alive connections. Multiple requests on one connection.
  Chrome uses up to 6 connections per origin for parallelism.
- **HTTP/2:** Multiplexed streams over a single connection. Binary protocol.
  All requests on one connection run in parallel.
- **HTTP/3:** Over QUIC (UDP-based). No head-of-line blocking. Fast connection
  setup with 0-RTT.

Chrome uses a network stack that supports all HTTP versions, connection
pooling, proxy support, cookie management, authentication, and caching.

### How Chinstrap does it

See `src/http.hpp` and `src/http.cpp`. We implement HTTP/1.1 over raw
POSIX sockets. The steps are:

1. **DNS lookup:** `getaddrinfo()` resolves the hostname
2. **TCP connect:** `socket()` + `connect()` establish the connection
3. **Send request:** `send()` the raw HTTP request bytes
4. **Read response:** `recv()` the response bytes
5. **Parse response:** Split status line, headers, and body
6. **Handle chunked encoding:** Decode if Transfer-Encoding: chunked
7. **Follow redirects:** Up to 5 redirects (301, 302, 303, 307, 308)

We send `Connection: close` to keep things simple (no keep-alive). Real
browsers use keep-alive to reuse connections across requests.

### What we skip
- TLS/HTTPS (requires a crypto library)
- HTTP/2 and HTTP/3
- Connection pooling
- Cookie management
- Compression (gzip, brotli)
- Caching
- Content negotiation
- Authentication

---

## 4. HTML Parsing

### How real browsers do it

HTML parsing is one of the most complex parts of a browser. The HTML5
specification defines a **tokenizer** (which breaks input into tokens)
and a **tree construction** stage (which builds the DOM tree).

The tokenizer is a state machine with ~30 states:
- Data state: reading text
- Tag open state: just saw `<`
- Tag name state: reading a tag name
- Attribute name state: reading an attribute name
- Attribute value (double-quoted) state: reading "value"
- Comment state: reading a comment
- And many more

The tree builder maintains an **open element stack**. When a start tag
is seen, the element is pushed. When an end tag is seen, the matching
element is popped. The stack also handles implicit closes (a new `<li>`
closes the previous `<li>`).

HTML is famously **error-tolerant**. The spec defines exact recovery
rules for malformed input. This is why browsers can render billions of
malformed pages.

Chrome uses the Blink HTML parser (in `third_party/blink/renderer/core/html/parser/`).
It runs on a separate thread for streaming parsing (starts parsing before
the full response is received).

### How Chinstrap does it

See `src/html_parser.hpp` and `src/html_parser.cpp`. We implement a
simplified version:

1. **Tokenizer:** State machine that produces tokens (StartTag, EndTag,
   Text, Comment, Doctype)
2. **Tree builder:** Maintains an open element stack, handles auto-closing
   rules for `<li>`, `<td>`, `<p>`, etc.
3. **Error recovery:** Missing `<html>`, `<head>`, `<body>` are auto-inserted.
   Unmatched end tags are ignored.

### What we skip
- Script execution (we parse `<script>` content as text)
- Foreign content (SVG, MathML)
- Adoption agency algorithm (complex misnested tag handling)
- Streaming parsing (we parse the full document at once)
- Preload scanner (scanning for resources while parsing)

---

## 5. CSS Cascade

### How real browsers do it

CSS (Cascading Style Sheets) determines how HTML elements are presented.
The **cascade** is the algorithm for resolving conflicts when multiple
rules apply to the same element.

The cascade order (lowest to highest priority):
1. User agent stylesheet (browser defaults)
2. User stylesheet (user preferences)
3. Author stylesheet (the page CSS)
4. Author inline styles (`style="..."`)
5. Author `!important` declarations
6. User `!important` declarations
7. User agent `!important` declarations

Within each level, **specificity** determines which rule wins:
- ID selectors: `(1, 0, 0)` - e.g., `#header`
- Class selectors: `(0, 1, 0)` - e.g., `.menu`
- Type selectors: `(0, 0, 1)` - e.g., `div`

Comparison is lexicographic: a single ID beats any number of classes.

After the cascade, some properties are **inherited** from parent to child
(e.g., color, font-size), while others are not (e.g., border, margin).

Chrome uses a **rule set** with indexes by tag, class, and ID for fast
selector matching. The style resolver produces a `ComputedStyle` for each
element.

### How Chinstrap does it

See `src/css_parser.hpp` and `src/css_parser.cpp`. We implement:
- Stylesheet parsing (rules, selectors, declarations)
- Selectors: type, class, ID, descendant, child, universal
- Specificity calculation and sorting
- Cascade: apply rules in specificity order
- Inheritance for inherited properties
- Inline styles (highest non-important specificity)
- `!important` flag support

### What we skip
- Pseudo-classes (`:hover`, `:focus`, `:nth-child`)
- Pseudo-elements (`::before`, `::after`)
- Attribute selectors (`[type="text"]`)
- Media queries (`@media`)
- CSS variables (`var()`)
- Animations and transitions
- User agent stylesheet
- Rule indexing optimizations

---

## 6. Layout

### How real browsers do it

Layout (also called **reflow**) computes the position and size of every
element. This is one of the most complex parts of a browser engine.

Key concepts:
- **Formatting context:** Block (vertical stacking) or inline (horizontal flow)
- **Box model:** Content area, padding, border, margin
- **Containing block:** The area an element is laid out within
- **Floats:** Elements that flow to the left or right
- **Positioning:** Static, relative, absolute, fixed, sticky
- **Flexbox:** Flexible box layout (1D)
- **Grid:** 2D grid layout

Chrome uses **LayoutNG** (Next Generation layout engine, introduced 2019).
It represents layout as a tree of `LayoutBox` objects with positions and
sizes. LayoutNG processes the tree in a single pass.

Layout is triggered by:
- Initial page load
- Window resize
- DOM changes (via JavaScript)
- Style changes
- Font loading completion

### How Chinstrap does it

See `src/layout.hpp` and `src/layout.cpp`. We implement:
- Block layout (vertical stacking of block elements)
- Inline layout (horizontal flow with word wrapping)
- Box model (content, padding, border, margin)
- Width/height from CSS values (px and %)
- Default block/inline display based on tag name

### What we skip
- Flexbox
- CSS Grid
- Floats
- Absolute/fixed/sticky positioning
- Transforms
- Overflow / scrolling
- Table layout
- Text shaping (we use a fixed-width approximation)

---

## 7. Painting

### How real browsers do it

Painting converts the layout tree into **drawing commands** (a display
list). These commands describe operations like:
- Fill a rectangle with a color
- Draw text at a position
- Draw a border
- Draw an image

Chrome separates paint into:
1. **Paint:** Generate a display list of drawing commands
2. **Rasterization:** Convert commands into pixels (using Skia for 2D)
3. **GPU acceleration:** Many operations are GPU-accelerated

Text rendering in Chrome uses:
- **FreeType:** For loading and rasterizing font outlines (TrueType/OpenType)
- **HarfBuzz:** For text shaping (ligatures, diacritics, bidirectional)
- **Skia:** For the actual drawing

### How Chinstrap does it

See `src/renderer.hpp` and `src/renderer.cpp`. We draw directly to the
Linux framebuffer (`/dev/fb0`):
- `mmap()` the framebuffer memory
- Write pixels by setting memory values
- Draw rectangles for backgrounds and borders
- Draw text using a simple 8x16 bitmap font

No GPU, no Skia, no FreeType, no HarfBuzz. Just raw pixel writing.

### What we skip
- GPU acceleration
- Anti-aliasing
- Sub-pixel rendering
- Font shaping (ligatures, etc.)
- Vector font rendering
- Image decoding (JPEG, PNG, GIF)
- SVG rendering

---

## 8. Compositing

### How real browsers do it

**Compositing** combines multiple layers into the final image you see on
screen. Chrome uses a compositor that runs on the GPU:

1. The page is divided into **layers** (e.g., a video, a scrolling section)
2. Each layer is rasterized independently
3. The compositor combines layers with the right position, opacity, and
   transforms
4. This allows smooth scrolling and animations without re-running layout

Chrome uses **viz** (the visual services component) for compositing. It
communicates with the GPU process via Mojo IPC.

### How Chinstrap does it

We do not have a compositor. We paint directly to a single framebuffer.
There is no layer separation, no GPU acceleration, no compositing step.
Every paint goes directly to the screen.

For an educational browser, this is fine. Compositing is an optimization
that improves performance, not correctness.

---

## 9. Event Handling

### How real browsers do it

Browsers handle user input (keyboard, mouse, touch) through an **event
loop**. The event loop:

1. Polls for events from the OS (via X11, Wayland, or platform APIs)
2. Dispatches events to the appropriate target (hit testing)
3. Runs JavaScript event handlers
4. Triggers layout and paint if needed

Chrome has a **scheduler** that prioritizes work:
- User input (highest priority)
- Compositor work
- JavaScript timers
- Idle cleanup (lowest priority)

The event loop is also responsible for:
- RequestAnimationFrame callbacks (for animations)
- setTimeout/setInterval callbacks
- Microtasks (Promises)
- Layout and paint scheduling

### How Chinstrap does it

Chinstrap does not have an event loop. We render once and then wait for
Ctrl+C to exit. Adding an event loop would require:
- Reading keyboard/mouse input from `/dev/input/*`
- Hit testing (mapping screen coordinates to DOM elements)
- A main loop that polls for events and triggers re-render

This is a natural next step for the project.

---

## Further Reading

- [Web Browser Engineering](https://browser.engineering/) - Pavel Panchekha & Chris Harrelson
- [How Browsers Work](https://www.html5rocks.com/en/tutorials/internals/howbrowserswork/) - Tali Garsiel
- [Chromium Source Code](https://source.chromium.org/)
- [Servo Browser Engine](https://servo.org/)
- [Web Platform Standards](https://platform.html5.org/)

---

## Chinstrap Subsystem Reference

| Subsystem | Source Files | Key Classes |
|---|---|---|
| URL Parsing | `src/url.hpp`, `src/url.cpp` | `Url` |
| HTTP | `src/http.hpp`, `src/http.cpp` | `HttpClient`, `HttpRequest`, `HttpResponse` |
| HTML Parsing | `src/html_parser.hpp`, `src/html_parser.cpp` | `HtmlParser`, `Node` |
| CSS | `src/css_parser.hpp`, `src/css_parser.cpp` | `CssParser`, `StyleEngine`, `Selector` |
| Layout | `src/layout.hpp`, `src/layout.cpp` | `LayoutEngine`, `Box` |
| Rendering | `src/renderer.hpp`, `src/renderer.cpp` | `Renderer`, `Color`, `FramebufferInfo` |
| JSON | `src/json.hpp`, `src/json.cpp` | `JsonValue`, `JsonParser` |
| Config | `src/config.hpp`, `src/config.cpp` | `Config` |
| Main | `src/main.cpp` | (entry point) |