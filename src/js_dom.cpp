// =============================================================================
// js_dom.cpp - DOM Bindings Implementation
// =============================================================================
//
// TEACHING NOTE: Implementing DOM Bindings
// =========================================
//
// This file implements the JavaScript <-> DOM bridge. The DOM is represented
// as a tree of nodes in C++. JavaScript code interacts with the DOM through
// wrapper objects that expose DOM operations as JavaScript methods.
//
// Key DOM APIs we expose:
//   - document.getElementById(id)
//   - document.querySelector(selector)
//   - document.querySelectorAll(selector)
//   - document.createElement(tagName)
//   - element.textContent
//   - element.innerHTML
//   - element.style
//   - element.className
//   - element.id
//   - element.appendChild(child)
//   - element.removeChild(child)
//   - element.addEventListener(type, callback)
//   - element.removeEventListener(type, callback)
//   - window.location
//   - window.onload
//   - window.addEventListener
//   - window.setTimeout / clearTimeout
//
// How Chrome/V8 bindings work:
// =============================
// Chrome uses a code generator (binding templates) to create V8 wrapper
// objects for each DOM C++ class. The wrapper maps property access to
// C++ getters/setters and method calls to C++ methods.
//
// Key concepts:
//   1. Wrapper lifetime: tied to both the DOM node and the JS object
//   2. Cross-compartment: DOM objects live in the main world, JS wrappers
//      can be in different worlds (extension sandboxes)
//   3. Event dispatch: DOM events trigger JS callbacks through the wrapper
//   4. GC integration: V8 GC talks to the DOM to keep wrappers alive
//
// We use a simplified approach: DOM nodes are stored as opaque pointers
// in JS objects, with native functions that call into the DOM engine.
//
// =============================================================================

#include "js_dom.hpp"
#include "js_interpreter.hpp"
#include <algorithm>
#include <unordered_map>
#include <ctime>
#include <vector>

namespace chinstrap {

// =============================================================================
// Simplified DOM AstNode structure
// =============================================================================

// TEACHING NOTE: DOM AstNode
// ========================
// In a real browser, DOM nodes are complex C++ objects with layout,
// style, rendering data, etc. We use a simplified structure that
// captures the essential data for JavaScript interaction.
//
// A real DOM node has:
//   - AstNode type (element, text, comment, document, etc.)
//   - Tag name (for elements)
//   - Attributes (id, class, style, custom)
//   - Parent and children (tree structure)
//   - Style computed from CSS
//   - Layout information (position, size)
//   - Event listeners

struct DOMNode {
    enum class Type { Element, Text, Comment, Document };

    Type type = Type::Element;
    std::string tag_name;
    std::string text_content;
    std::string inner_html;
    std::unordered_map<std::string, std::string> attributes;
    DOMNode* parent = nullptr;
    std::vector<DOMNode*> children;

    // Event listeners: type -> list of JS function objects
    std::unordered_map<std::string, std::vector<JSObject*>> listeners;

    // The JS wrapper object (for re-use)
    JSObject* js_wrapper = nullptr;

    ~DOMNode() {
        for (auto* child : children) {
            delete child;
        }
    }
};

// Global DOM document (simplified)
static DOMNode* g_document = nullptr;
static DOMNode* g_window = nullptr;

// Map from JS object to DOM node (using internal slot)
static std::unordered_map<JSObject*, DOMNode*> g_dom_map;

// =============================================================================
// Get the global document
// =============================================================================

DOMNode* get_document() {
    if (!g_document) {
        g_document = new DOMNode();
        g_document->type = DOMNode::Type::Document;
        g_document->tag_name = "#document";
    }
    return g_document;
}

DOMNode* get_window() {
    if (!g_window) {
        g_window = new DOMNode();
        g_window->type = DOMNode::Type::Element;
        g_window->tag_name = "window";
    }
    return g_window;
}

// =============================================================================
// Helper: create a native function
// =============================================================================

static JSObject* make_native_fn(Interpreter& /*interp*/, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn) {
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    obj->is_function = true;
    obj->is_native = true;
    obj->class_name = name;
    obj->native_fn = std::move(fn);
    return obj;
}

static void set_method(JSObject* obj, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn,
    Interpreter& interp) {
    JSObject* method = make_native_fn(interp, name, std::move(fn));
    obj->set_property(name, JSValue::function_val(method));
}

static void set_prop(JSObject* obj, const std::string& name, JSValue value) {
    obj->set_property(name, value);
}

// =============================================================================
// Wrap/unwrap DOM nodes
// =============================================================================

JSValue wrap_dom_node(Interpreter& /*interp*/, DOMNode* node) {
    if (!node) return make_null();

    // Re-use existing wrapper
    if (node->js_wrapper) {
        return JSValue::object_val(node->js_wrapper);
    }

    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    node->js_wrapper = obj;
    g_dom_map[obj] = node;

    // Store DOM node pointer in internal slot (as a number for simplicity)
    // We use the address as a number - this is a hack but works for our purposes
    uintptr_t addr = reinterpret_cast<uintptr_t>(node);
    obj->internal_slots["__dom_node__"] = make_number(static_cast<double>(addr));

    // Set up common properties for elements
    if (node->type == DOMNode::Type::Element) {
        // tagName (read-only)
        set_prop(obj, "tagName", make_string(node->tag_name));
        set_prop(obj, "nodeName", make_string(node->tag_name));

        // id property
        set_prop(obj, "id", make_string(node->attributes.count("id") ?
            node->attributes["id"] : ""));

        // className property
        set_prop(obj, "className", make_string(node->attributes.count("class") ?
            node->attributes["class"] : ""));

        // textContent property
        set_prop(obj, "textContent", make_string(node->text_content));

        // innerHTML property
        set_prop(obj, "innerHTML", make_string(node->inner_html));
    }

    return JSValue::object_val(obj);
}

DOMNode* unwrap_dom_node(JSValue val) {
    if (!val.is_object()) return nullptr;
    JSObject* obj = val.as_object();

    // Check internal slot
    auto it = obj->internal_slots.find("__dom_node__");
    if (it == obj->internal_slots.end()) return nullptr;

    uintptr_t addr = static_cast<uintptr_t>(it->second.to_number());
    return reinterpret_cast<DOMNode*>(addr);
}

// =============================================================================
// DOM query helpers
// =============================================================================

// Simple querySelector: supports #id, .class, and tag selectors
static DOMNode* query_selector(DOMNode* root, const std::string& selector) {
    if (selector.empty()) return nullptr;

    // ID selector: #id
    if (selector[0] == '#') {
        std::string id = selector.substr(1);
        std::function<DOMNode*(DOMNode*)> find_by_id =
            [&](DOMNode* node) -> DOMNode* {
            if (node->type == DOMNode::Type::Element) {
                auto it = node->attributes.find("id");
                if (it != node->attributes.end() && it->second == id) return node;
            }
            for (auto* child : node->children) {
                DOMNode* found = find_by_id(child);
                if (found) return found;
            }
            return nullptr;
        };
        return find_by_id(root);
    }

    // Class selector: .class
    if (selector[0] == '.') {
        std::string cls = selector.substr(1);
        std::function<DOMNode*(DOMNode*)> find_by_class =
            [&](DOMNode* node) -> DOMNode* {
            if (node->type == DOMNode::Type::Element) {
                auto it = node->attributes.find("class");
                if (it != node->attributes.end()) {
                    // Check if class is in the class list
                    std::string classes = it->second;
                    if (classes.find(cls) != std::string::npos) return node;
                }
            }
            for (auto* child : node->children) {
                DOMNode* found = find_by_class(child);
                if (found) return found;
            }
            return nullptr;
        };
        return find_by_class(root);
    }

    // Tag selector
    std::function<DOMNode*(DOMNode*)> find_by_tag =
        [&](DOMNode* node) -> DOMNode* {
        if (node->type == DOMNode::Type::Element &&
            node->tag_name == selector) return node;
        for (auto* child : node->children) {
            DOMNode* found = find_by_tag(child);
            if (found) return found;
        }
        return nullptr;
    };
    return find_by_tag(root);
}

static std::vector<DOMNode*> query_selector_all(DOMNode* root, const std::string& selector) {
    std::vector<DOMNode*> results;
    if (selector.empty()) return results;

    std::function<void(DOMNode*)> collect = [&](DOMNode* node) {
        if (selector[0] == '#') {
            std::string id = selector.substr(1);
            if (node->type == DOMNode::Type::Element) {
                auto it = node->attributes.find("id");
                if (it != node->attributes.end() && it->second == id) {
                    results.push_back(node);
                }
            }
        } else if (selector[0] == '.') {
            std::string cls = selector.substr(1);
            if (node->type == DOMNode::Type::Element) {
                auto it = node->attributes.find("class");
                if (it != node->attributes.end() && it->second.find(cls) != std::string::npos) {
                    results.push_back(node);
                }
            }
        } else {
            if (node->type == DOMNode::Type::Element && node->tag_name == selector) {
                results.push_back(node);
            }
        }
        for (auto* child : node->children) {
            collect(child);
        }
    };
    collect(root);
    return results;
}

// =============================================================================
// Initialize DOM bindings
// =============================================================================

void init_dom_bindings(Interpreter& interp, JSObject* global) {
    // TEACHING NOTE: DOM API Setup
    // =============================
    // We create a "document" object and a "window" object on the global scope.
    // These objects have native methods that call into the C++ DOM implementation.
    // When JS code calls document.getElementById("foo"), the native function
    // looks up the DOM node by ID and returns a JS wrapper object.

    DOMNode* doc = get_document();
    DOMNode* win = get_window();

    // Create document object
    GCObject* doc_gco = gc().allocate_object();
    JSObject* document_obj = GarbageCollector::get_object(doc_gco);
    document_obj->class_name = "HTMLDocument";
    doc->js_wrapper = document_obj;
    g_dom_map[document_obj] = doc;
    uintptr_t doc_addr = reinterpret_cast<uintptr_t>(doc);
    document_obj->internal_slots["__dom_node__"] = make_number(static_cast<double>(doc_addr));

    // document.getElementById
    set_method(document_obj, "getElementById",
        [&interp](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_null();
            std::string id = args[0].to_string();
            DOMNode* doc_node = get_document();
            std::function<DOMNode*(DOMNode*)> find =
                [&](DOMNode* node) -> DOMNode* {
                if (node->type == DOMNode::Type::Element) {
                    auto it = node->attributes.find("id");
                    if (it != node->attributes.end() && it->second == id) return node;
                }
                for (auto* child : node->children) {
                    DOMNode* found = find(child);
                    if (found) return found;
                }
                return nullptr;
            };
            DOMNode* result = find(doc_node);
            return wrap_dom_node(interp, result);
        }, interp);

    // document.querySelector
    set_method(document_obj, "querySelector",
        [&interp](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_null();
            DOMNode* result = query_selector(get_document(), args[0].to_string());
            return wrap_dom_node(interp, result);
        }, interp);

    // document.querySelectorAll
    set_method(document_obj, "querySelectorAll",
        [&interp](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) {
                GCObject* gco = gc().allocate_object();
                JSObject* arr = GarbageCollector::get_object(gco);
                arr->is_array = true;
                arr->set_property("length", make_number(0));
                return JSValue::array_val(arr);
            }
            auto results = query_selector_all(get_document(), args[0].to_string());
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            for (size_t idx = 0; idx < results.size(); idx++) {
                arr->set_property(std::to_string(idx), wrap_dom_node(interp, results[idx]));
            }
            arr->set_property("length", make_number(static_cast<double>(results.size())));
            return JSValue::array_val(arr);
        }, interp);

    // document.createElement
    set_method(document_obj, "createElement",
        [&interp](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_null();
            DOMNode* node = new DOMNode();
            node->type = DOMNode::Type::Element;
            node->tag_name = args[0].to_string();
            // Convert to uppercase for HTML
            std::transform(node->tag_name.begin(), node->tag_name.end(),
                          node->tag_name.begin(), ::toupper);
            return wrap_dom_node(interp, node);
        }, interp);

    // document.createTextNode
    set_method(document_obj, "createTextNode",
        [&interp](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            DOMNode* node = new DOMNode();
            node->type = DOMNode::Type::Text;
            node->text_content = args.empty() ? "" : args[0].to_string();
            return wrap_dom_node(interp, node);
        }, interp);

    // document.body (create a body element)
    GCObject* body_gco = gc().allocate_object();
    JSObject* body_obj = GarbageCollector::get_object(body_gco);
    body_obj->class_name = "HTMLBodyElement";
    // Create a body DOM node
    static DOMNode body_node;
    body_node.type = DOMNode::Type::Element;
    body_node.tag_name = "BODY";
    body_node.js_wrapper = body_obj;
    g_dom_map[body_obj] = &body_node;
    body_obj->internal_slots["__dom_node__"] = make_number(static_cast<double>(reinterpret_cast<uintptr_t>(&body_node)));
    set_prop(document_obj, "body", JSValue::object_val(body_obj));
    set_prop(document_obj, "head", make_null());
    set_prop(document_obj, "documentElement", JSValue::object_val(body_obj));

    // Set document on global
    set_prop(global, "document", JSValue::object_val(document_obj));

    // Create window object
    GCObject* win_gco = gc().allocate_object();
    JSObject* window_obj = GarbageCollector::get_object(win_gco);
    window_obj->class_name = "Window";
    win->js_wrapper = window_obj;
    g_dom_map[window_obj] = win;
    uintptr_t win_addr = reinterpret_cast<uintptr_t>(win);
    window_obj->internal_slots["__dom_node__"] = make_number(static_cast<double>(win_addr));

    // window.location (simplified)
    GCObject* loc_gco = gc().allocate_object();
    JSObject* location_obj = GarbageCollector::get_object(loc_gco);
    location_obj->class_name = "Location";
    set_prop(location_obj, "href", make_string("about:blank"));
    set_prop(location_obj, "protocol", make_string("about:"));
    set_prop(location_obj, "host", make_string(""));
    set_prop(location_obj, "hostname", make_string(""));
    set_prop(location_obj, "pathname", make_string("blank"));
    set_prop(location_obj, "hash", make_string(""));
    set_prop(location_obj, "search", make_string(""));
    set_prop(window_obj, "location", JSValue::object_val(location_obj));

    // window.navigator (simplified)
    GCObject* nav_gco = gc().allocate_object();
    JSObject* navigator_obj = GarbageCollector::get_object(nav_gco);
    navigator_obj->class_name = "Navigator";
    set_prop(navigator_obj, "userAgent", make_string("Chinstrap/1.0"));
    set_prop(navigator_obj, "platform", make_string("Linux"));
    set_prop(navigator_obj, "language", make_string("en-US"));
    set_prop(navigator_obj, "languages", [&interp]() -> JSValue {
        GCObject* gco = gc().allocate_object();
        JSObject* arr = GarbageCollector::get_object(gco);
        arr->is_array = true;
        arr->set_property("0", make_string("en-US"));
        arr->set_property("1", make_string("en"));
        arr->set_property("length", make_number(2));
        return JSValue::array_val(arr);
    }());
    set_prop(window_obj, "navigator", JSValue::object_val(navigator_obj));

    // window.document (same as document)
    set_prop(window_obj, "document", JSValue::object_val(document_obj));

    // window.console (same as console)
    JSValue console_val = global->get_property("console");
    set_prop(window_obj, "console", console_val);

    // window.setTimeout
    set_method(window_obj, "setTimeout",
        [&interp](Interpreter* i, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_function()) return make_number(0);
            // Simplified: just call the callback immediately
            JSObject* fn = args[0].as_function();
            std::vector<JSValue> call_args;
            if (args.size() > 2) {
                call_args.assign(args.begin() + 2, args.end());
            }
            i->call_function(fn, make_undefined(), call_args);
            return make_number(1); // timer id
        }, interp);

    // window.clearTimeout
    set_method(window_obj, "clearTimeout",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_undefined();
        }, interp);

    // window.setInterval
    set_method(window_obj, "setInterval",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_number(1);
        }, interp);

    // window.clearInterval
    set_method(window_obj, "clearInterval",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_undefined();
        }, interp);

    // window.addEventListener
    set_method(window_obj, "addEventListener",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.size() < 2) return make_undefined();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_undefined();
            std::string type = args[0].to_string();
            if (args[1].is_function()) {
                node->listeners[type].push_back(args[1].as_function());
            }
            return make_undefined();
        }, interp);

    // window.removeEventListener
    set_method(window_obj, "removeEventListener",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_undefined();
        }, interp);

    // Set window on global
    set_prop(global, "window", JSValue::object_val(window_obj));
    set_prop(global, "self", JSValue::object_val(window_obj));

    // Now set up element methods on a prototype
    // We create an "Element.prototype" object and set it as prototype
    // for all element wrapper objects.

    // For simplicity, we add methods to Object.prototype so all objects
    // can use DOM methods. In a real browser, each element type has its
    // own prototype chain.

    // Actually, we should set up element methods differently. Let us add
    // them to the document.body and make all wrapped elements inherit from
    // a common element prototype.

    // Create Element prototype
    GCObject* element_proto_gco = gc().allocate_object();
    JSObject* element_proto = GarbageCollector::get_object(element_proto_gco);
    element_proto->class_name = "Element";

    // element.appendChild
    set_method(element_proto, "appendChild",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            DOMNode* parent = unwrap_dom_node(this_val);
            DOMNode* child = unwrap_dom_node(args[0]);
            if (!parent || !child) return make_undefined();
            parent->children.push_back(child);
            child->parent = parent;
            return args[0];
        }, interp);

    // element.removeChild
    set_method(element_proto, "removeChild",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            DOMNode* parent = unwrap_dom_node(this_val);
            DOMNode* child = unwrap_dom_node(args[0]);
            if (!parent || !child) return make_undefined();
            auto it = std::find(parent->children.begin(), parent->children.end(), child);
            if (it != parent->children.end()) {
                parent->children.erase(it);
                child->parent = nullptr;
            }
            return args[0];
        }, interp);

    // element.addEventListener
    set_method(element_proto, "addEventListener",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.size() < 2) return make_undefined();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_undefined();
            std::string type = args[0].to_string();
            if (args[1].is_function()) {
                node->listeners[type].push_back(args[1].as_function());
            }
            return make_undefined();
        }, interp);

    // element.removeEventListener
    set_method(element_proto, "removeEventListener",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_undefined();
        }, interp);

    // element.getAttribute
    set_method(element_proto, "getAttribute",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_null();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_null();
            auto it = node->attributes.find(args[0].to_string());
            if (it != node->attributes.end()) return make_string(it->second);
            return make_null();
        }, interp);

    // element.setAttribute
    set_method(element_proto, "setAttribute",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.size() < 2) return make_undefined();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_undefined();
            node->attributes[args[0].to_string()] = args[1].to_string();
            return make_undefined();
        }, interp);

    // element.removeAttribute
    set_method(element_proto, "removeAttribute",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_undefined();
            node->attributes.erase(args[0].to_string());
            return make_undefined();
        }, interp);

    // element.querySelector (on element)
    set_method(element_proto, "querySelector",
        [&interp](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_null();
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_null();
            DOMNode* result = query_selector(node, args[0].to_string());
            return wrap_dom_node(interp, result);
        }, interp);

    // element.querySelectorAll (on element)
    set_method(element_proto, "querySelectorAll",
        [&interp](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) {
                GCObject* gco = gc().allocate_object();
                JSObject* arr = GarbageCollector::get_object(gco);
                arr->is_array = true;
                arr->set_property("length", make_number(0));
                return JSValue::array_val(arr);
            }
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) {
                GCObject* gco = gc().allocate_object();
                JSObject* arr = GarbageCollector::get_object(gco);
                arr->is_array = true;
                arr->set_property("length", make_number(0));
                return JSValue::array_val(arr);
            }
            auto results = query_selector_all(node, args[0].to_string());
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            for (size_t idx = 0; idx < results.size(); idx++) {
                arr->set_property(std::to_string(idx), wrap_dom_node(interp, results[idx]));
            }
            arr->set_property("length", make_number(static_cast<double>(results.size())));
            return JSValue::array_val(arr);
        }, interp);

    // element.classList (simplified - returns object with add/remove/toggle)
    set_method(element_proto, "classList",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_undefined();
            GCObject* gco = gc().allocate_object();
            (void)gco; // placeholder for future classList object
            // Implement add, remove, toggle, contains as native methods
            // For simplicity, just return a string-based class list
            std::string classes = node->attributes.count("class") ? node->attributes["class"] : "";
            return make_string(classes);
        }, interp);

    // element.toString
    set_method(element_proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            DOMNode* node = unwrap_dom_node(this_val);
            if (!node) return make_string("[object HTMLElement]");
            return make_string("[object HTML" + node->tag_name + "Element]");
        }, interp);

    // Set element prototype on body object
    body_obj->prototype = element_proto_gco;
}

} // namespace chinstrap