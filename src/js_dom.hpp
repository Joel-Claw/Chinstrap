// =============================================================================
// js_dom.hpp - DOM Bindings for JavaScript
// =============================================================================
//
// TEACHING NOTE: DOM-JS Bridge
// =============================
//
// The DOM (Document Object Model) is the browser representation of an HTML
// page. JavaScript interacts with the DOM through a set of APIs:
//   - document.getElementById, querySelector, createElement
//   - element.textContent, innerHTML, style, classList
//   - element.addEventListener, appendChild, removeChild
//   - window.location, window.onload
//
// In a real browser, the DOM is implemented in C++ and exposed to JavaScript
// through "bindings". V8 uses "V8 bindings" (src/third_party/blink/renderer/bindings/)
// that wrap C++ DOM objects and expose them as JavaScript objects.
//
// How Chrome does it:
//   1. Each DOM C++ class has a "wrapper" V8 object
//   2. Properties on the wrapper call into C++ methods
//   3. DOM events trigger JS callbacks through the wrapper
//   4. The GC integrates with V8 to handle DOM object lifetime
//
// Our approach:
//   We expose DOM operations as native JS functions. The DOM tree itself
//   is stored in C++ (in the browser rendering engine). We provide JS
//   functions that call into the DOM C++ code.
//
// =============================================================================

#ifndef CHINSTRAP_JS_DOM_HPP
#define CHINSTRAP_JS_DOM_HPP

#include "js_value.hpp"
#include "js_gc.hpp"

namespace chinstrap {

class Interpreter;

// Forward declaration: DOM node structure from the browser engine
// In a real implementation, this would be the actual DOM node from the
// HTML parser and rendering engine.
struct DOMNode;

// DOM bindings setup
void init_dom_bindings(Interpreter& interp, JSObject* global);

// Create a JS object that wraps a DOM node
JSValue wrap_dom_node(Interpreter& interp, DOMNode* node);

// Get the DOM node from a JS wrapper object
DOMNode* unwrap_dom_node(JSValue val);

} // namespace chinstrap

#endif // CHINSTRAP_JS_DOM_HPP