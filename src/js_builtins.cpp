// =============================================================================
// js_builtins.cpp - Built-in Objects Implementation
// =============================================================================
//
// TEACHING NOTE: Implementing JavaScript Built-ins
// =================================================
//
// This file implements the built-in objects that every JavaScript runtime
// must provide. We create:
//   - Prototype objects (Object.prototype, Array.prototype, etc.)
//   - Constructor functions (Object, Array, String, etc.)
//   - Utility objects (Math, JSON, console)
//   - Global functions (parseInt, parseFloat, isNaN, etc.)
//
// Each built-in function is a "native function" - implemented in C++ but
// callable from JavaScript. We use JSObject with is_native=true and
// native_fn set to a C++ lambda.
//
// How V8 implements built-ins:
// V8 has a complex system for built-ins:
//   - Some are written in C++ (Torque/CodeStubAssembler)
//   - Some are written in JavaScript (builtins/*.js)
//   - Some are bytecoded handlers in the interpreter
// V8 compiles built-ins at build time and embeds them in the binary.
// This gives maximum performance but makes the build complex.
//
// =============================================================================

#include "js_builtins.hpp"
#include "js_interpreter.hpp"
#include "js_parser.hpp"
#include "js_lexer.hpp"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <limits>
#include <set>

namespace chinstrap {

// =============================================================================
// Helper: create a native function
// =============================================================================

// TEACHING NOTE: Native Function Setup
// =====================================
// We create native functions as JSObject with:
//   - is_function = true
//   - is_native = true
//   - native_fn = C++ function/lambda
//
// The native function signature takes the interpreter, this value, and
// arguments vector. It returns a JSValue. This mirrors how V8 handles
// built-in callbacks, though V8 uses a more sophisticated argument passing
// scheme with Argument objects.

static GCObject* make_native_gc(Interpreter& interp, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn);

static void set_method(JSObject* obj, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn,
    Interpreter& interp);

static void set_property(JSObject* obj, const std::string& name, JSValue value);

// Forward declaration of interpreter methods we need
// The Interpreter class is defined in js_interpreter.hpp

// =============================================================================
// Object builtins
// =============================================================================

void init_object_builtins(Interpreter& interp, JSObject* global, GCObject* object_proto) {
    JSObject* proto = GarbageCollector::get_object(object_proto);

    // Object.prototype methods
    set_method(proto, "hasOwnProperty",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            JSObject* obj = nullptr;
            if (this_val.is_object()) obj = this_val.as_object();
            else if (this_val.is_string()) {
                // String hasOwnProperty for primitive wrapper
                if (!args.empty() && args[0].is_string()) {
                    return JSValue::boolean_val(false);
                }
                return JSValue::boolean_val(false);
            }
            if (!obj) return JSValue::boolean_val(false);
            if (args.empty()) return JSValue::boolean_val(false);
            std::string key = args[0].to_string();
            return JSValue::boolean_val(obj->property_index.find(key) != obj->property_index.end());
        }, interp);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_undefined()) return make_string("[object Undefined]");
            if (this_val.is_null()) return make_string("[object Null]");
            if (this_val.is_object()) {
                JSObject* obj = this_val.as_object();
                if (obj->is_array) return make_string("[object Array]");
                if (obj->is_function) return make_string("[object Function]");
                return make_string("[object Object]");
            }
            return make_string("[object " + this_val.type_of() + "]");
        }, interp);

    set_method(proto, "valueOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            return this_val;
        }, interp);

    set_method(proto, "isPrototypeOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_object())
                return JSValue::boolean_val(false);
            JSObject* proto = this_val.as_object();
            JSObject* obj = args[0].as_object();
            while (obj && obj->prototype) {
                if (GarbageCollector::get_object(obj->prototype) == proto) {
                    return JSValue::boolean_val(true);
                }
                obj = GarbageCollector::get_object(obj->prototype);
            }
            return JSValue::boolean_val(false);
        }, interp);

    // Object constructor
    GCObject* object_ctor_gc = make_native_gc(interp, "Object",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || args[0].is_nullish()) {
                GCObject* gco = gc().allocate_object();
                return JSValue::object_val(GarbageCollector::get_object(gco));
            }
            return args[0];
        });

    JSObject* object_ctor = GarbageCollector::get_object(object_ctor_gc);
    object_ctor->prototype = object_proto;
    set_property(object_ctor, "prototype", JSValue::object_val(proto));
    set_property(global, "Object", JSValue::function_val(object_ctor));

    // Object.keys
    set_method(object_ctor, "keys",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_object()) return make_undefined();
            JSObject* obj = args[0].as_object();
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            auto keys = obj->enumerable_keys();
            for (size_t k = 0; k < keys.size(); k++) {
                arr->set_property(std::to_string(k), make_string(keys[k]));
            }
            arr->set_property("length", make_number(static_cast<double>(keys.size())));
            return JSValue::array_val(arr);
        }, interp);

    // Object.values
    set_method(object_ctor, "values",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_object()) return make_undefined();
            JSObject* obj = args[0].as_object();
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            auto keys = obj->enumerable_keys();
            for (size_t k = 0; k < keys.size(); k++) {
                arr->set_property(std::to_string(k), obj->get_property(keys[k]));
            }
            arr->set_property("length", make_number(static_cast<double>(keys.size())));
            return JSValue::array_val(arr);
        }, interp);

    // Object.entries
    set_method(object_ctor, "entries",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_object()) return make_undefined();
            JSObject* obj = args[0].as_object();
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            auto keys = obj->enumerable_keys();
            for (size_t k = 0; k < keys.size(); k++) {
                GCObject* entry_gco = gc().allocate_object();
                JSObject* entry = GarbageCollector::get_object(entry_gco);
                entry->is_array = true;
                entry->set_property("0", make_string(keys[k]));
                entry->set_property("1", obj->get_property(keys[k]));
                entry->set_property("length", make_number(2.0));
                arr->set_property(std::to_string(k), JSValue::array_val(entry));
            }
            arr->set_property("length", make_number(static_cast<double>(keys.size())));
            return JSValue::array_val(arr);
        }, interp);

    // Object.assign
    set_method(object_ctor, "assign",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            if (!args[0].is_object()) return make_undefined();
            JSObject* target = args[0].as_object();
            for (size_t a = 1; a < args.size(); a++) {
                if (!args[a].is_object()) continue;
                JSObject* src = args[a].as_object();
                for (auto& key : src->enumerable_keys()) {
                    target->set_property(key, src->get_property(key));
                }
            }
            return JSValue::object_val(target);
        }, interp);

    // Object.create
    set_method(object_ctor, "create",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* obj = GarbageCollector::get_object(gco);
            if (!args.empty() && args[0].is_object()) {
                // Find GC wrapper for the prototype
                JSObject* proto = args[0].as_object();
                // Find GCObject for this proto - search all objects
                for (auto* g : g_gc_objects_list()) {
                    if (g->type == GCObjectType::Object && g->payload.object == proto) {
                        obj->prototype = g;
                        break;
                    }
                }
            }
            // Optional property descriptors
            if (args.size() > 1 && args[1].is_object()) {
                JSObject* props = args[1].as_object();
                for (auto& key : props->enumerable_keys()) {
                    JSValue desc = props->get_property(key);
                    if (desc.is_object()) {
                        JSObject* d = desc.as_object();
                        JSValue val = d->get_property("value");
                        obj->set_property(key, val);
                    }
                }
            }
            return JSValue::object_val(obj);
        }, interp);

    // Object.freeze (simplified - just marks as non-extensible)
    set_method(object_ctor, "freeze",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (!args.empty() && args[0].is_object()) {
                args[0].as_object()->extensible = false;
            }
            return args.empty() ? make_undefined() : args[0];
        }, interp);

    set_method(object_ctor, "getPrototypeOf",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_object()) return make_null();
            JSObject* obj = args[0].as_object();
            if (obj->prototype) {
                return JSValue::object_val(GarbageCollector::get_object(obj->prototype));
            }
            return make_null();
        }, interp);
}

// =============================================================================
// Array builtins
// =============================================================================

void init_array_builtins(Interpreter& interp, JSObject* global, GCObject* array_proto) {
    JSObject* proto = GarbageCollector::get_object(array_proto);
    proto->is_array = false; // prototype is not an array itself

    // Array.prototype.push
    set_method(proto, "push",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            for (size_t a = 0; a < args.size(); a++) {
                arr->set_property(std::to_string(static_cast<long long>(len)), args[a]);
                len++;
            }
            arr->set_property("length", make_number(len));
            return make_number(len);
        }, interp);

    // Array.prototype.pop
    set_method(proto, "pop",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            if (len <= 0) return make_undefined();
            std::string key = std::to_string(static_cast<long long>(len - 1));
            JSValue val = arr->get_property(key);
            arr->delete_property(key);
            arr->set_property("length", make_number(len - 1));
            return val;
        }, interp);

    // Array.prototype.shift
    set_method(proto, "shift",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            if (len <= 0) return make_undefined();
            JSValue first = arr->get_property("0");
            for (double d = 1; d < len; d++) {
                std::string old_key = std::to_string(static_cast<long long>(d));
                std::string new_key = std::to_string(static_cast<long long>(d - 1));
                arr->set_property(new_key, arr->get_property(old_key));
            }
            arr->delete_property(std::to_string(static_cast<long long>(len - 1)));
            arr->set_property("length", make_number(len - 1));
            return first;
        }, interp);

    // Array.prototype.unshift
    set_method(proto, "unshift",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            size_t nargs = args.size();
            for (double d = len - 1; d >= 0; d--) {
                std::string old_key = std::to_string(static_cast<long long>(d));
                std::string new_key = std::to_string(static_cast<long long>(d + nargs));
                arr->set_property(new_key, arr->get_property(old_key));
            }
            for (size_t a = 0; a < nargs; a++) {
                arr->set_property(std::to_string(a), args[a]);
            }
            arr->set_property("length", make_number(len + nargs));
            return make_number(len + nargs);
        }, interp);

    // Array.prototype.map
    set_method(proto, "map",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_undefined();
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            GCObject* result_gco = gc().allocate_object();
            JSObject* result = GarbageCollector::get_object(result_gco);
            result->is_array = true;
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {elem, make_number(d), this_val};
                JSValue mapped = i->call_function(fn, make_undefined(), call_args);
                result->set_property(key, mapped);
            }
            result->set_property("length", make_number(len));
            return JSValue::array_val(result);
        }, interp);

    // Array.prototype.filter
    set_method(proto, "filter",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_undefined();
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            GCObject* result_gco = gc().allocate_object();
            JSObject* result = GarbageCollector::get_object(result_gco);
            result->is_array = true;
            double out_idx = 0;
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {elem, make_number(d), this_val};
                JSValue keep = i->call_function(fn, make_undefined(), call_args);
                if (keep.to_boolean()) {
                    result->set_property(std::to_string(static_cast<long long>(out_idx)), elem);
                    out_idx++;
                }
            }
            result->set_property("length", make_number(out_idx));
            return JSValue::array_val(result);
        }, interp);

    // Array.prototype.reduce
    set_method(proto, "reduce",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_undefined();
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            JSValue acc;
            double start = 0;
            if (args.size() >= 2) {
                acc = args[1];
            } else {
                if (len == 0) {
                    return make_undefined();
                }
                acc = arr->get_property("0");
                start = 1;
            }
            for (double d = start; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {acc, elem, make_number(d), this_val};
                acc = i->call_function(fn, make_undefined(), call_args);
            }
            return acc;
        }, interp);

    // Array.prototype.forEach
    set_method(proto, "forEach",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_undefined();
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {elem, make_number(d), this_val};
                i->call_function(fn, make_undefined(), call_args);
            }
            return make_undefined();
        }, interp);

    // Array.prototype.find
    set_method(proto, "find",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_undefined();
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {elem, make_number(d), this_val};
                JSValue found = i->call_function(fn, make_undefined(), call_args);
                if (found.to_boolean()) return elem;
            }
            return make_undefined();
        }, interp);

    // Array.prototype.findIndex
    set_method(proto, "findIndex",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty() || !args[0].is_function())
                return make_number(-1.0);
            JSObject* arr = this_val.as_object();
            JSObject* fn = args[0].as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                std::vector<JSValue> call_args = {elem, make_number(d), this_val};
                JSValue found = i->call_function(fn, make_undefined(), call_args);
                if (found.to_boolean()) return make_number(d);
            }
            return make_number(-1.0);
        }, interp);

    // Array.prototype.includes
    set_method(proto, "includes",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty()) return make_bool(false);
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                if (args[0].strict_equals(elem)) return make_bool(true);
            }
            return make_bool(false);
        }, interp);

    // Array.prototype.indexOf
    set_method(proto, "indexOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object() || args.empty()) return make_number(-1.0);
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                if (args[0].strict_equals(elem)) return make_number(d);
            }
            return make_number(-1.0);
        }, interp);

    // Array.prototype.join
    set_method(proto, "join",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_string("");
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            std::string sep = ",";
            if (!args.empty()) sep = args[0].to_string();
            std::string result;
            for (double d = 0; d < len; d++) {
                if (d > 0) result += sep;
                std::string key = std::to_string(static_cast<long long>(d));
                JSValue elem = arr->get_property(key);
                if (!elem.is_undefined() && !elem.is_null()) {
                    result += elem.to_string();
                }
            }
            return make_string(result);
        }, interp);

    // Array.prototype.slice
    set_method(proto, "slice",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            double start = 0, end = len;
            if (!args.empty()) {
                start = args[0].to_number();
                if (start < 0) start = std::max(0.0, len + start);
            }
            if (args.size() >= 2) {
                end = args[1].to_number();
                if (end < 0) end = std::max(0.0, len + end);
            }
            GCObject* gco = gc().allocate_object();
            JSObject* result = GarbageCollector::get_object(gco);
            result->is_array = true;
            double out_idx = 0;
            for (double d = start; d < end && d < len; d++) {
                std::string key = std::to_string(static_cast<long long>(d));
                result->set_property(std::to_string(static_cast<long long>(out_idx)),
                    arr->get_property(key));
                out_idx++;
            }
            result->set_property("length", make_number(out_idx));
            return JSValue::array_val(result);
        }, interp);

    // Array.prototype.concat
    set_method(proto, "concat",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            GCObject* gco = gc().allocate_object();
            JSObject* result = GarbageCollector::get_object(gco);
            result->is_array = true;
            double out_idx = 0;
            for (double d = 0; d < len; d++) {
                result->set_property(std::to_string(static_cast<long long>(out_idx)),
                    arr->get_property(std::to_string(static_cast<long long>(d))));
                out_idx++;
            }
            for (auto& arg : args) {
                if (arg.is_array()) {
                    JSObject* src = arg.as_object();
                    double slen = src->get_property("length").to_number();
                    for (double d = 0; d < slen; d++) {
                        result->set_property(std::to_string(static_cast<long long>(out_idx)),
                            src->get_property(std::to_string(static_cast<long long>(d))));
                        out_idx++;
                    }
                } else {
                    result->set_property(std::to_string(static_cast<long long>(out_idx)), arg);
                    out_idx++;
                }
            }
            result->set_property("length", make_number(out_idx));
            return JSValue::array_val(result);
        }, interp);

    // Array.prototype.reverse
    set_method(proto, "reverse",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            for (double d = 0; d < len / 2; d++) {
                std::string k1 = std::to_string(static_cast<long long>(d));
                std::string k2 = std::to_string(static_cast<long long>(len - 1 - d));
                JSValue tmp = arr->get_property(k1);
                arr->set_property(k1, arr->get_property(k2));
                arr->set_property(k2, tmp);
            }
            return this_val;
        }, interp);

    // Array.prototype.sort (simplified)
    set_method(proto, "sort",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_object()) return make_undefined();
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            // Collect elements
            std::vector<JSValue> elements;
            for (double d = 0; d < len; d++) {
                elements.push_back(arr->get_property(std::to_string(static_cast<long long>(d))));
            }
            // Sort
            JSObject* compare_fn = nullptr;
            if (!args.empty() && args[0].is_function()) {
                compare_fn = args[0].as_object();
            }
            std::sort(elements.begin(), elements.end(),
                [&i, compare_fn](const JSValue& a, const JSValue& b) -> bool {
                    if (compare_fn) {
                        std::vector<JSValue> call_args = {a, b};
                        JSValue result = i->call_function(compare_fn, make_undefined(), call_args);
                        return result.to_number() < 0;
                    }
                    return a.to_string() < b.to_string();
                });
            // Put back
            for (size_t d = 0; d < elements.size(); d++) {
                arr->set_property(std::to_string(d), elements[d]);
            }
            return this_val;
        }, interp);

    // Array.prototype.toString
    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (!this_val.is_object()) return make_string("");
            JSObject* arr = this_val.as_object();
            double len = arr->get_property("length").to_number();
            std::string result;
            for (double d = 0; d < len; d++) {
                if (d > 0) result += ",";
                JSValue elem = arr->get_property(std::to_string(static_cast<long long>(d)));
                if (!elem.is_nullish()) result += elem.to_string();
            }
            return make_string(result);
        }, interp);

    // Array constructor
    GCObject* array_ctor_gc = make_native_gc(interp, "Array",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            if (args.size() == 1 && args[0].is_number()) {
                arr->set_property("length", args[0]);
            } else {
                for (size_t a = 0; a < args.size(); a++) {
                    arr->set_property(std::to_string(a), args[a]);
                }
                arr->set_property("length", make_number(static_cast<double>(args.size())));
            }
            return JSValue::array_val(arr);
        });

    JSObject* array_ctor = GarbageCollector::get_object(array_ctor_gc);
    array_ctor->prototype = array_proto;
    array_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(array_proto)));
    set_property(global, "Array", JSValue::function_val(array_ctor));

    // Array.isArray
    set_method(array_ctor, "isArray",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_object()) return make_bool(false);
            return make_bool(args[0].as_object()->is_array);
        }, interp);

    // Array.from (simplified)
    set_method(array_ctor, "from",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            if (args[0].is_string()) {
                std::string s = args[0].as_string()->value;
                for (size_t c = 0; c < s.length(); c++) {
                    arr->set_property(std::to_string(c), make_string(std::string(1, s[c])));
                }
                arr->set_property("length", make_number(static_cast<double>(s.length())));
            } else if (args[0].is_object()) {
                JSObject* src = args[0].as_object();
                double len = src->get_property("length").to_number();
                for (double d = 0; d < len; d++) {
                    arr->set_property(std::to_string(static_cast<long long>(d)),
                        src->get_property(std::to_string(static_cast<long long>(d))));
                }
                arr->set_property("length", make_number(len));
            }
            return JSValue::array_val(arr);
        }, interp);
}

// =============================================================================
// String builtins
// =============================================================================

void init_string_builtins(Interpreter& interp, JSObject* global, GCObject* string_proto) {
    JSObject* proto = GarbageCollector::get_object(string_proto);

    // String.prototype.charAt
    set_method(proto, "charAt",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            double idx = args.empty() ? 0 : args[0].to_number();
            if (idx < 0 || idx >= static_cast<double>(s.length())) return make_string("");
            return make_string(std::string(1, s[static_cast<size_t>(idx)]));
        }, interp);

    // String.prototype.charCodeAt
    set_method(proto, "charCodeAt",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            double idx = args.empty() ? 0 : args[0].to_number();
            if (idx < 0 || idx >= static_cast<double>(s.length())) return make_number(std::numeric_limits<double>::quiet_NaN());
            return make_number(static_cast<double>(static_cast<unsigned char>(s[static_cast<size_t>(idx)])));
        }, interp);

    // String.prototype.concat
    set_method(proto, "concat",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            for (auto& arg : args) s += arg.to_string();
            return make_string(s);
        }, interp);

    // String.prototype.includes
    set_method(proto, "includes",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_bool(false);
            std::string s = this_val.to_string();
            std::string search = args[0].to_string();
            size_t start = 0;
            if (args.size() >= 2) start = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            return make_bool(s.find(search, start) != std::string::npos);
        }, interp);

    // String.prototype.indexOf
    set_method(proto, "indexOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(-1.0);
            std::string s = this_val.to_string();
            std::string search = args[0].to_string();
            size_t start = 0;
            if (args.size() >= 2) start = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            size_t pos = s.find(search, start);
            if (pos == std::string::npos) return make_number(-1.0);
            return make_number(static_cast<double>(pos));
        }, interp);

    // String.prototype.lastIndexOf
    set_method(proto, "lastIndexOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(-1.0);
            std::string s = this_val.to_string();
            std::string search = args[0].to_string();
            size_t pos = s.rfind(search);
            if (pos == std::string::npos) return make_number(-1.0);
            return make_number(static_cast<double>(pos));
        }, interp);

    // String.prototype.slice
    set_method(proto, "slice",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            long long len = static_cast<long long>(s.length());
            long long start = 0, end = len;
            if (!args.empty()) {
                start = static_cast<long long>(args[0].to_number());
                if (start < 0) start = std::max(0LL, len + start);
            }
            if (args.size() >= 2) {
                end = static_cast<long long>(args[1].to_number());
                if (end < 0) end = std::max(0LL, len + end);
            }
            if (start >= end) return make_string("");
            return make_string(s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
        }, interp);

    // String.prototype.substring
    set_method(proto, "substring",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            long long len = static_cast<long long>(s.length());
            long long start = 0, end = len;
            if (!args.empty()) {
                start = static_cast<long long>(args[0].to_number());
                if (start < 0) start = 0;
                if (start > len) start = len;
            }
            if (args.size() >= 2) {
                end = static_cast<long long>(args[1].to_number());
                if (end < 0) end = 0;
                if (end > len) end = len;
            }
            if (start > end) std::swap(start, end);
            return make_string(s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
        }, interp);

    // String.prototype.substr
    set_method(proto, "substr",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            long long len = static_cast<long long>(s.length());
            long long start = 0, length = len;
            if (!args.empty()) {
                start = static_cast<long long>(args[0].to_number());
                if (start < 0) start = std::max(0LL, len + start);
            }
            if (args.size() >= 2) {
                length = static_cast<long long>(args[1].to_number());
                if (length < 0) length = 0;
            }
            if (start >= len) return make_string("");
            if (start + length > len) length = len - start;
            return make_string(s.substr(static_cast<size_t>(start), static_cast<size_t>(length)));
        }, interp);

    // String.prototype.split
    set_method(proto, "split",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            GCObject* gco = gc().allocate_object();
            JSObject* arr = GarbageCollector::get_object(gco);
            arr->is_array = true;
            if (args.empty() || args[0].is_undefined()) {
                arr->set_property("0", make_string(s));
                arr->set_property("length", make_number(1));
                return JSValue::array_val(arr);
            }
            std::string sep = args[0].to_string();
            if (sep.empty()) {
                for (size_t c = 0; c < s.length(); c++) {
                    arr->set_property(std::to_string(c), make_string(std::string(1, s[c])));
                }
                arr->set_property("length", make_number(static_cast<double>(s.length())));
                return JSValue::array_val(arr);
            }
            size_t start = 0;
            size_t idx = 0;
            size_t pos;
            while ((pos = s.find(sep, start)) != std::string::npos) {
                arr->set_property(std::to_string(idx), make_string(s.substr(start, pos - start)));
                idx++;
                start = pos + sep.length();
            }
            arr->set_property(std::to_string(idx), make_string(s.substr(start)));
            idx++;
            arr->set_property("length", make_number(static_cast<double>(idx)));
            return JSValue::array_val(arr);
        }, interp);

    // String.prototype.toUpperCase
    set_method(proto, "toUpperCase",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            std::string s = this_val.to_string();
            std::transform(s.begin(), s.end(), s.begin(), ::toupper);
            return make_string(s);
        }, interp);

    // String.prototype.toLowerCase
    set_method(proto, "toLowerCase",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            std::string s = this_val.to_string();
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return make_string(s);
        }, interp);

    // String.prototype.trim
    set_method(proto, "trim",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            std::string s = this_val.to_string();
            size_t start = s.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return make_string("");
            size_t end = s.find_last_not_of(" \t\n\r\f\v");
            return make_string(s.substr(start, end - start + 1));
        }, interp);

    // String.prototype.replace (simplified - first match only)
    set_method(proto, "replace",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            if (args.size() < 2) return make_string(s);
            std::string search = args[0].to_string();
            std::string replacement = args[1].to_string();
            size_t pos = s.find(search);
            if (pos != std::string::npos) {
                s.replace(pos, search.length(), replacement);
            }
            return make_string(s);
        }, interp);

    // String.prototype.replaceAll
    set_method(proto, "replaceAll",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            if (args.size() < 2) return make_string(s);
            std::string search = args[0].to_string();
            std::string replacement = args[1].to_string();
            if (search.empty()) return make_string(s);
            size_t pos = 0;
            while ((pos = s.find(search, pos)) != std::string::npos) {
                s.replace(pos, search.length(), replacement);
                pos += replacement.length();
            }
            return make_string(s);
        }, interp);

    // String.prototype.startsWith
    set_method(proto, "startsWith",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_bool(false);
            std::string s = this_val.to_string();
            std::string prefix = args[0].to_string();
            return make_bool(s.substr(0, prefix.length()) == prefix);
        }, interp);

    // String.prototype.endsWith
    set_method(proto, "endsWith",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_bool(false);
            std::string s = this_val.to_string();
            std::string suffix = args[0].to_string();
            if (suffix.length() > s.length()) return make_bool(false);
            return make_bool(s.substr(s.length() - suffix.length()) == suffix);
        }, interp);

    // String.prototype.repeat
    set_method(proto, "repeat",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            if (args.empty()) return make_string(s);
            long long count = static_cast<long long>(args[0].to_number());
            if (count < 0) return make_string(s);
            std::string result;
            for (long long c = 0; c < count; c++) result += s;
            return make_string(result);
        }, interp);

    // String.prototype.padStart
    set_method(proto, "padStart",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            std::string s = this_val.to_string();
            if (args.empty()) return make_string(s);
            long long target = static_cast<long long>(args[0].to_number());
            std::string pad = " ";
            if (args.size() >= 2) pad = args[1].to_string();
            while (static_cast<long long>(s.length()) < target) {
                s = pad + s;
                if (static_cast<long long>(s.length()) > target) {
                    s = s.substr(s.length() - target);
                }
            }
            return make_string(s);
        }, interp);

    // String.prototype.toString
    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_string(this_val.to_string());
        }, interp);

    // String constructor
    GCObject* string_ctor_gc = make_native_gc(interp, "String",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_string("");
            return make_string(args[0].to_string());
        });

    JSObject* string_ctor = GarbageCollector::get_object(string_ctor_gc);
    string_ctor->prototype = string_proto;
    // Set String.prototype property on the constructor so that
    // get_property("String").as_function()->get_property("prototype") works
    string_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(string_proto)));
    set_property(global, "String", JSValue::function_val(string_ctor));

    // String.fromCharCode
    set_method(string_ctor, "fromCharCode",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            std::string s;
            for (auto& arg : args) {
                int code = static_cast<int>(arg.to_number());
                if (code < 128) {
                    s += static_cast<char>(code);
                } else if (code < 0x800) {
                    s += static_cast<char>(0xC0 | (code >> 6));
                    s += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    s += static_cast<char>(0xE0 | (code >> 12));
                    s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    s += static_cast<char>(0x80 | (code & 0x3F));
                }
            }
            return make_string(s);
        }, interp);
}

// =============================================================================
// Number builtins
// =============================================================================

void init_number_builtins(Interpreter& interp, JSObject* global, GCObject* number_proto) {
    JSObject* proto = GarbageCollector::get_object(number_proto);

    // Number.prototype.toString
    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_number()) {
                return make_string(this_val.to_string());
            }
            return make_string("0");
        }, interp);

    // Number.prototype.toFixed
    set_method(proto, "toFixed",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            double n = this_val.to_number();
            int digits = args.empty() ? 0 : static_cast<int>(args[0].to_number());
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(digits) << n;
            return make_string(oss.str());
        }, interp);

    // Number.prototype.valueOf
    set_method(proto, "valueOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_number()) return this_val;
            return make_number(0.0);
        }, interp);

    // Number constructor
    GCObject* number_ctor_gc = make_native_gc(interp, "Number",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(0.0);
            return make_number(args[0].to_number());
        });

    JSObject* number_ctor = GarbageCollector::get_object(number_ctor_gc);
    number_ctor->prototype = number_proto;
    number_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(number_proto)));
    set_property(global, "Number", JSValue::function_val(number_ctor));

    // Number constants
    set_property(number_ctor, "MAX_SAFE_INTEGER", make_number(9007199254740991.0));
    set_property(number_ctor, "MIN_SAFE_INTEGER", make_number(-9007199254740991.0));
    set_property(number_ctor, "MAX_VALUE", make_number(std::numeric_limits<double>::max()));
    set_property(number_ctor, "MIN_VALUE", make_number(std::numeric_limits<double>::min()));
    set_property(number_ctor, "POSITIVE_INFINITY", make_number(std::numeric_limits<double>::infinity()));
    set_property(number_ctor, "NEGATIVE_INFINITY", make_number(-std::numeric_limits<double>::infinity()));
    set_property(number_ctor, "NaN", make_number(std::numeric_limits<double>::quiet_NaN()));
    set_property(number_ctor, "EPSILON", make_number(std::numeric_limits<double>::epsilon()));
    // Number.isInteger
    set_method(number_ctor, "isInteger",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_number()) return make_bool(false);
            double n = args[0].as_number();
            return make_bool(!std::isnan(n) && !std::isinf(n) && n == static_cast<double>(static_cast<long long>(n)));
        }, interp);
    set_method(number_ctor, "isNaN",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_number()) return make_bool(false);
            return make_bool(std::isnan(args[0].as_number()));
        }, interp);
    set_method(number_ctor, "isFinite",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty() || !args[0].is_number()) return make_bool(false);
            double n = args[0].as_number();
            return make_bool(!std::isnan(n) && !std::isinf(n));
        }, interp);
}

// =============================================================================
// Boolean builtins
// =============================================================================

void init_boolean_builtins(Interpreter& interp, JSObject* global, GCObject* boolean_proto) {
    JSObject* proto = GarbageCollector::get_object(boolean_proto);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_string(this_val.to_boolean() ? "true" : "false");
        }, interp);

    set_method(proto, "valueOf",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            return JSValue::boolean_val(this_val.to_boolean());
        }, interp);

    GCObject* bool_ctor_gc = make_native_gc(interp, "Boolean",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            return JSValue::boolean_val(args.empty() ? false : args[0].to_boolean());
        });

    JSObject* bool_ctor = GarbageCollector::get_object(bool_ctor_gc);
    bool_ctor->prototype = boolean_proto;
    bool_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(boolean_proto)));
    set_property(global, "Boolean", JSValue::function_val(bool_ctor));
}

// =============================================================================
// Function builtins
// =============================================================================

void init_function_builtins(Interpreter& interp, JSObject* /*global*/, GCObject* function_proto) {
    JSObject* proto = GarbageCollector::get_object(function_proto);

    set_method(proto, "call",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_function()) return make_undefined();
            JSObject* fn = this_val.as_object();
            JSValue this_arg = args.empty() ? make_undefined() : args[0];
            std::vector<JSValue> call_args;
            if (args.size() > 1) {
                call_args.assign(args.begin() + 1, args.end());
            }
            return i->call_function(fn, this_arg, call_args);
        }, interp);

    set_method(proto, "apply",
        [&interp](Interpreter* i, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_function()) return make_undefined();
            JSObject* fn = this_val.as_object();
            JSValue this_arg = args.empty() ? make_undefined() : args[0];
            std::vector<JSValue> call_args;
            if (args.size() > 1 && args[1].is_object()) {
                JSObject* arr = args[1].as_object();
                double len = arr->get_property("length").to_number();
                for (double d = 0; d < len; d++) {
                    call_args.push_back(arr->get_property(std::to_string(static_cast<long long>(d))));
                }
            }
            return i->call_function(fn, this_arg, call_args);
        }, interp);

    set_method(proto, "bind",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& args) -> JSValue {
            if (!this_val.is_function()) return make_undefined();
            // Create a bound function
            GCObject* gco = gc().allocate_object();
            JSObject* bound = GarbageCollector::get_object(gco);
            bound->is_function = true;
            bound->is_native = true;
            bound->is_constructor = false;
            JSObject* original = this_val.as_object();
            bound->bound_this = args.empty() ? make_undefined() : args[0];
            if (args.size() > 1) {
                bound->bound_args.assign(args.begin() + 1, args.end());
            }
            bound->native_fn = [original](Interpreter* interp, JSValue, std::vector<JSValue>& call_args) -> JSValue {
                // Prepend bound args
                std::vector<JSValue> all_args = original->bound_args;
                all_args.insert(all_args.end(), call_args.begin(), call_args.end());
                return interp->call_function(original, original->bound_this, all_args);
            };
            return JSValue::function_val(bound);
        }, interp);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_string("function () { [native code] }");
        }, interp);
}

// =============================================================================
// Math builtins
// =============================================================================

void init_math_builtins(Interpreter& interp, JSObject* global) {
    GCObject* math_gco = gc().allocate_object();
    JSObject* math = GarbageCollector::get_object(math_gco);
    math->class_name = "Math";

    set_property(math, "PI", make_number(3.14159265358979323846));
    set_property(math, "E", make_number(2.71828182845904523536));
    set_property(math, "LN2", make_number(0.6931471805599453));
    set_property(math, "LN10", make_number(2.302585092994046));
    set_property(math, "LOG2E", make_number(1.4426950408889634));
    set_property(math, "LOG10E", make_number(0.4342944819032518));
    set_property(math, "SQRT2", make_number(1.4142135623730951));
    set_property(math, "SQRT1_2", make_number(0.7071067811865476));

    auto make_math_fn = [](double (*fn)(double)) {
        return [fn](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(std::numeric_limits<double>::quiet_NaN());
            return make_number(fn(args[0].to_number()));
        };
    };

    set_method(math, "abs", make_math_fn(std::fabs), interp);
    set_method(math, "floor", make_math_fn(std::floor), interp);
    set_method(math, "ceil", make_math_fn(std::ceil), interp);
    set_method(math, "round", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& a) -> JSValue {
        if (a.empty()) return make_number(std::numeric_limits<double>::quiet_NaN());
        double n = a[0].to_number();
        return make_number(std::floor(n + 0.5));
    }, interp);
    set_method(math, "trunc", make_math_fn(std::trunc), interp);
    set_method(math, "sqrt", make_math_fn(std::sqrt), interp);
    set_method(math, "cbrt", make_math_fn(std::cbrt), interp);
    set_method(math, "log", make_math_fn(std::log), interp);
    set_method(math, "log2", make_math_fn(std::log2), interp);
    set_method(math, "log10", make_math_fn(std::log10), interp);
    set_method(math, "exp", make_math_fn(std::exp), interp);
    set_method(math, "sin", make_math_fn(std::sin), interp);
    set_method(math, "cos", make_math_fn(std::cos), interp);
    set_method(math, "tan", make_math_fn(std::tan), interp);
    set_method(math, "asin", make_math_fn(std::asin), interp);
    set_method(math, "acos", make_math_fn(std::acos), interp);
    set_method(math, "atan", make_math_fn(std::atan), interp);
    set_method(math, "sinh", make_math_fn(std::sinh), interp);
    set_method(math, "cosh", make_math_fn(std::cosh), interp);
    set_method(math, "tanh", make_math_fn(std::tanh), interp);
    set_method(math, "sign", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& a) -> JSValue {
        if (a.empty()) return make_number(std::numeric_limits<double>::quiet_NaN());
        double n = a[0].to_number();
        if (std::isnan(n)) return make_number(std::numeric_limits<double>::quiet_NaN());
        if (n > 0) return make_number(1.0);
        if (n < 0) return make_number(-1.0);
        return make_number(0.0);
    }, interp);

    set_method(math, "max", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& a) -> JSValue {
        if (a.empty()) return make_number(-std::numeric_limits<double>::infinity());
        double result = -std::numeric_limits<double>::infinity();
        for (auto& v : a) {
            double n = v.to_number();
            if (std::isnan(n)) return make_number(std::numeric_limits<double>::quiet_NaN());
            if (n > result) result = n;
        }
        return make_number(result);
    }, interp);

    set_method(math, "min", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& a) -> JSValue {
        if (a.empty()) return make_number(std::numeric_limits<double>::infinity());
        double result = std::numeric_limits<double>::infinity();
        for (auto& v : a) {
            double n = v.to_number();
            if (std::isnan(n)) return make_number(std::numeric_limits<double>::quiet_NaN());
            if (n < result) result = n;
        }
        return make_number(result);
    }, interp);

    set_method(math, "pow", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& a) -> JSValue {
        if (a.size() < 2) return make_number(std::numeric_limits<double>::quiet_NaN());
        return make_number(std::pow(a[0].to_number(), a[1].to_number()));
    }, interp);

    set_method(math, "random", [](Interpreter* /*i*/, JSValue /*tv*/, std::vector<JSValue>& /*a*/) -> JSValue {
        // Simple PRNG using rand() - not cryptographically secure
        return make_number(static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX));
    }, interp);

    set_property(global, "Math", JSValue::object_val(math));
}

// =============================================================================
// JSON builtins
// =============================================================================

void init_json_builtins(Interpreter& interp, JSObject* global) {
    GCObject* json_gco = gc().allocate_object();
    JSObject* json = GarbageCollector::get_object(json_gco);
    json->class_name = "JSON";

    // JSON.parse (simplified)
    set_method(json, "parse",
        [&interp](Interpreter* i, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            std::string s = args[0].to_string();
            // Very simplified JSON parser
            // We parse using the JS parser instead by wrapping in (...)
            Parser parser(s);
            auto ast = parser.parse_expression_only();
            if (ast) {
                return i->evaluate(ast.get());
            }
            return make_undefined();
        }, interp);

    // JSON.stringify (simplified)
    set_method(json, "stringify",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_undefined();
            std::function<std::string(const JSValue&, int)> stringify_value;
            stringify_value = [&stringify_value](const JSValue& v, int indent) -> std::string {
                std::string pad(indent * 2, ' ');
                std::string pad2((indent + 1) * 2, ' ');
                if (v.is_undefined()) return "undefined";
                if (v.is_null()) return "null";
                if (v.is_boolean()) return v.as_boolean() ? "true" : "false";
                if (v.is_number()) {
                    if (std::isnan(v.as_number()) || std::isinf(v.as_number())) return "null";
                    return v.to_string();
                }
                if (v.is_string()) {
                    std::string s = v.as_string()->value;
                    std::string result = "\"";
                    for (char c : s) {
                        switch (c) {
                            case '"': result += "\\\""; break;
                            case '\\': result += "\\\\"; break;
                            case '\n': result += "\\n"; break;
                            case '\r': result += "\\r"; break;
                            case '\t': result += "\\t"; break;
                            default: result += c; break;
                        }
                    }
                    result += "\"";
                    return result;
                }
                if (v.is_function()) return "undefined";
                if (v.is_object()) {
                    JSObject* obj = v.as_object();
                    if (obj->is_array) {
                        double len = obj->get_property("length").to_number();
                        std::string result = "[";
                        for (double d = 0; d < len; d++) {
                            if (d > 0) result += ",";
                            JSValue elem = obj->get_property(std::to_string(static_cast<long long>(d)));
                            result += stringify_value(elem, indent);
                        }
                        result += "]";
                        return result;
                    }
                    auto keys = obj->enumerable_keys();
                    std::string result = "{";
                    bool first = true;
                    for (auto& key : keys) {
                        JSValue val = obj->get_property(key);
                        std::string val_str = stringify_value(val, indent + 1);
                        if (val_str == "undefined") continue;
                        if (!first) result += ",";
                        first = false;
                        result += "\"" + key + "\":" + val_str;
                    }
                    result += "}";
                    return result;
                }
                return "null";
            };
            return make_string(stringify_value(args[0], 0));
        }, interp);

    set_property(global, "JSON", JSValue::object_val(json));
}

// =============================================================================
// Console builtins
// =============================================================================

void init_console_builtins(Interpreter& interp, JSObject* global) {
    GCObject* console_gco = gc().allocate_object();
    JSObject* console_obj = GarbageCollector::get_object(console_gco);
    console_obj->class_name = "Console";

    auto log_fn = [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
        for (size_t a = 0; a < args.size(); a++) {
            if (a > 0) std::printf(" ");
            std::printf("%s", args[a].to_string().c_str());
        }
        std::printf("\n");
        std::fflush(stdout);
        return make_undefined();
    };

    set_method(console_obj, "log", log_fn, interp);
    set_method(console_obj, "info", log_fn, interp);
    set_method(console_obj, "debug", log_fn, interp);

    auto err_fn = [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
        for (size_t a = 0; a < args.size(); a++) {
            if (a > 0) std::fprintf(stderr, " ");
            std::fprintf(stderr, "%s", args[a].to_string().c_str());
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        return make_undefined();
    };

    set_method(console_obj, "error", err_fn, interp);
    set_method(console_obj, "warn", err_fn, interp);

    set_method(console_obj, "dir",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (!args.empty()) {
                std::printf("%s\n", args[0].to_string().c_str());
                std::fflush(stdout);
            }
            return make_undefined();
        }, interp);

    set_property(global, "console", JSValue::object_val(console_obj));
}

// =============================================================================
// Error builtins
// =============================================================================

void init_error_builtins(Interpreter& interp, JSObject* global, GCObject* error_proto) {
    JSObject* proto = GarbageCollector::get_object(error_proto);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (!this_val.is_object()) return make_string("Error");
            JSObject* obj = this_val.as_object();
            std::string name = obj->get_property("name").to_string();
            std::string msg = obj->get_property("message").to_string();
            if (msg.empty()) return make_string(name);
            return make_string(name + ": " + msg);
        }, interp);

    // Error constructor
    GCObject* error_ctor_gc = make_native_gc(interp, "Error",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* err = GarbageCollector::get_object(gco);
            err->is_error = true;
            err->class_name = "Error";
            err->set_property("name", make_string("Error"));
            err->set_property("message", args.empty() ? make_string("") : make_string(args[0].to_string()));
            return JSValue::object_val(err);
        });

    JSObject* error_ctor = GarbageCollector::get_object(error_ctor_gc);
    error_ctor->prototype = error_proto;
    error_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(error_proto)));
    set_property(global, "Error", JSValue::function_val(error_ctor));

    // Error subclasses
    const char* error_types[] = {
        "TypeError", "RangeError", "ReferenceError",
        "SyntaxError", "EvalError", "URIError"
    };

    for (auto& type_name : error_types) {
        GCObject* sub_proto_gc = gc().allocate_object();
        JSObject* sub_proto = GarbageCollector::get_object(sub_proto_gc);
        sub_proto->is_error = true;
        sub_proto->prototype = error_proto;
        sub_proto->set_property("name", make_string(type_name));

        GCObject* sub_ctor_gc = make_native_gc(interp, type_name,
            [type_name](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
                GCObject* gco = gc().allocate_object();
                JSObject* err = GarbageCollector::get_object(gco);
                err->is_error = true;
                err->class_name = type_name;
                err->set_property("name", make_string(type_name));
                err->set_property("message", args.empty() ? make_string("") : make_string(args[0].to_string()));
                return JSValue::object_val(err);
            });

        JSObject* sub_ctor = GarbageCollector::get_object(sub_ctor_gc);
        sub_ctor->prototype = sub_proto_gc;
        sub_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(sub_proto_gc)));
        set_property(global, type_name, JSValue::function_val(sub_ctor));
    }
}

// =============================================================================
// Promise builtins (simplified)
// =============================================================================

void init_promise_builtins(Interpreter& interp, JSObject* global, GCObject* promise_proto) {
    JSObject* proto = GarbageCollector::get_object(promise_proto);

    // Promise.prototype.then
    set_method(proto, "then",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            // Simplified: just return a new resolved promise
            // Real implementation would chain properly
            return this_val;
        }, interp);

    // Promise.prototype.catch
    set_method(proto, "catch",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            return this_val;
        }, interp);

    // Promise constructor
    GCObject* promise_ctor_gc = make_native_gc(interp, "Promise",
        [&interp](Interpreter* i, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* promise = GarbageCollector::get_object(gco);
            promise->class_name = "Promise";
            promise->set_property("state", make_string("pending"));
            promise->set_property("result", make_undefined());

            if (!args.empty() && args[0].is_function()) {
                JSObject* executor = args[0].as_object();
                // Create resolve and reject functions
                GCObject* resolve_gc = gc().allocate_object();
                JSObject* resolve_fn = GarbageCollector::get_object(resolve_gc);
                resolve_fn->is_function = true;
                resolve_fn->is_native = true;
                resolve_fn->native_fn = [promise](Interpreter*, JSValue, std::vector<JSValue>& a) -> JSValue {
                    promise->set_property("state", make_string("fulfilled"));
                    promise->set_property("result", a.empty() ? make_undefined() : a[0]);
                    return make_undefined();
                };

                GCObject* reject_gc = gc().allocate_object();
                JSObject* reject_fn = GarbageCollector::get_object(reject_gc);
                reject_fn->is_function = true;
                reject_fn->is_native = true;
                reject_fn->native_fn = [promise](Interpreter*, JSValue, std::vector<JSValue>& a) -> JSValue {
                    promise->set_property("state", make_string("rejected"));
                    promise->set_property("result", a.empty() ? make_undefined() : a[0]);
                    return make_undefined();
                };

                std::vector<JSValue> exec_args = {JSValue::function_val(resolve_fn), JSValue::function_val(reject_fn)};
                i->call_function(executor, make_undefined(), exec_args);
            }

            return JSValue::object_val(promise);
        });

    JSObject* promise_ctor = GarbageCollector::get_object(promise_ctor_gc);
    promise_ctor->prototype = promise_proto;
    promise_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(promise_proto)));
    set_property(global, "Promise", JSValue::function_val(promise_ctor));

    // Promise.resolve
    set_method(promise_ctor, "resolve",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* promise = GarbageCollector::get_object(gco);
            promise->class_name = "Promise";
            promise->set_property("state", make_string("fulfilled"));
            promise->set_property("result", args.empty() ? make_undefined() : args[0]);
            return JSValue::object_val(promise);
        }, interp);

    // Promise.reject
    set_method(promise_ctor, "reject",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* promise = GarbageCollector::get_object(gco);
            promise->class_name = "Promise";
            promise->set_property("state", make_string("rejected"));
            promise->set_property("result", args.empty() ? make_undefined() : args[0]);
            return JSValue::object_val(promise);
        }, interp);
}

// =============================================================================
// Date builtins (simplified)
// =============================================================================

void init_date_builtins(Interpreter& interp, JSObject* global, GCObject* date_proto) {
    JSObject* proto = GarbageCollector::get_object(date_proto);

    set_method(proto, "getTime",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_object()) {
                JSValue t = this_val.as_object()->get_property("__time__");
                if (t.is_number()) return t;
            }
            return make_number(0.0);
        }, interp);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_object()) {
                JSValue t = this_val.as_object()->get_property("__time__");
                if (t.is_number()) {
                    time_t time = static_cast<time_t>(t.as_number() / 1000);
                    char buf[256];
                    std::strftime(buf, sizeof(buf), "%a %b %d %Y %H:%M:%S", std::gmtime(&time));
                    return make_string(buf);
                }
            }
            return make_string("Invalid Date");
        }, interp);

    GCObject* date_ctor_gc = make_native_gc(interp, "Date",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            GCObject* gco = gc().allocate_object();
            JSObject* date = GarbageCollector::get_object(gco);
            date->class_name = "Date";
            double time;
            if (args.empty()) {
                time = static_cast<double>(std::time(nullptr)) * 1000.0;
            } else if (args.size() == 1 && args[0].is_number()) {
                time = args[0].as_number();
            } else {
                time = static_cast<double>(std::time(nullptr)) * 1000.0;
            }
            date->set_property("__time__", make_number(time));
            return JSValue::object_val(date);
        });

    JSObject* date_ctor = GarbageCollector::get_object(date_ctor_gc);
    date_ctor->prototype = date_proto;
    date_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(date_proto)));
    set_property(global, "Date", JSValue::function_val(date_ctor));

    set_method(date_ctor, "now",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& /*args*/) -> JSValue {
            return make_number(static_cast<double>(std::time(nullptr)) * 1000.0);
        }, interp);
}

// =============================================================================
// Symbol builtins (simplified)
// =============================================================================

void init_symbol_builtins(Interpreter& interp, JSObject* global, GCObject* symbol_proto) {
    JSObject* proto = GarbageCollector::get_object(symbol_proto);

    set_method(proto, "toString",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_symbol()) {
                return make_string("Symbol(" + this_val.as_symbol()->description + ")");
            }
            return make_string("Symbol()");
        }, interp);

    set_method(proto, "description",
        [](Interpreter* /*i*/, JSValue this_val, std::vector<JSValue>& /*args*/) -> JSValue {
            if (this_val.is_symbol()) {
                return make_string(this_val.as_symbol()->description);
            }
            return make_undefined();
        }, interp);

    GCObject* symbol_ctor_gc = make_native_gc(interp, "Symbol",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            std::string desc = args.empty() ? "" : args[0].to_string();
            GCObject* gco = gc().allocate_symbol(desc);
            return JSValue::symbol_val(GarbageCollector::get_symbol(gco));
        });

    JSObject* symbol_ctor = GarbageCollector::get_object(symbol_ctor_gc);
    symbol_ctor->prototype = symbol_proto;
    symbol_ctor->set_property("prototype", JSValue::object_val(GarbageCollector::get_object(symbol_proto)));
    set_property(global, "Symbol", JSValue::function_val(symbol_ctor));

    // Symbol.iterator
    set_property(symbol_ctor, "iterator", JSValue::symbol_val(GarbageCollector::get_symbol(gc().allocate_symbol("Symbol.iterator"))));
}

// =============================================================================
// Global functions
// =============================================================================

void init_global_functions(Interpreter& interp, JSObject* global) {
    // parseInt
    set_method(global, "parseInt",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(std::numeric_limits<double>::quiet_NaN());
            std::string s = args[0].to_string();
            int radix = 10;
            if (args.size() >= 2 && args[1].is_number()) {
                radix = static_cast<int>(args[1].to_number());
            }
            // Trim whitespace
            size_t start = s.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return make_number(std::numeric_limits<double>::quiet_NaN());
            s = s.substr(start);
            // Handle 0x prefix
            if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                radix = 16;
                s = s.substr(2);
            }
            try {
                size_t pos;
                long long val = std::stoll(s, &pos, radix);
                return make_number(static_cast<double>(val));
            } catch (...) {
                return make_number(std::numeric_limits<double>::quiet_NaN());
            }
        }, interp);

    // parseFloat
    set_method(global, "parseFloat",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_number(std::numeric_limits<double>::quiet_NaN());
            try {
                return make_number(std::stod(args[0].to_string()));
            } catch (...) {
                return make_number(std::numeric_limits<double>::quiet_NaN());
            }
        }, interp);

    // isNaN
    set_method(global, "isNaN",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_bool(true);
            return make_bool(std::isnan(args[0].to_number()));
        }, interp);

    // isFinite
    set_method(global, "isFinite",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_bool(false);
            double n = args[0].to_number();
            return make_bool(!std::isnan(n) && !std::isinf(n));
        }, interp);

    // encodeURIComponent
    set_method(global, "encodeURIComponent",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_string("");
            std::string s = args[0].to_string();
            std::string result;
            static const char* hex = "0123456789ABCDEF";
            for (unsigned char c : s) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '!' ||
                    c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
                    result += c;
                } else {
                    result += '%';
                    result += hex[c >> 4];
                    result += hex[c & 0xF];
                }
            }
            return make_string(result);
        }, interp);

    // decodeURIComponent
    set_method(global, "decodeURIComponent",
        [](Interpreter* /*i*/, JSValue /*this_val*/, std::vector<JSValue>& args) -> JSValue {
            if (args.empty()) return make_string("");
            std::string s = args[0].to_string();
            std::string result;
            for (size_t c = 0; c < s.length(); c++) {
                if (s[c] == '%' && c + 2 < s.length()) {
                    int val = 0;
                    for (int i = 0; i < 2; i++) {
                        char h = s[c + 1 + i];
                        val <<= 4;
                        if (h >= '0' && h <= '9') val |= h - '0';
                        else if (h >= 'A' && h <= 'F') val |= h - 'A' + 10;
                        else if (h >= 'a' && h <= 'f') val |= h - 'a' + 10;
                    }
                    result += static_cast<char>(val);
                    c += 2;
                } else {
                    result += s[c];
                }
            }
            return make_string(result);
        }, interp);
}

// =============================================================================
// Main initialization
// =============================================================================

// Static storage for GC objects list access - provided by js_gc.cpp

void init_builtins(Interpreter& interp, JSObject* global) {
    // Create prototype objects
    GCObject* object_proto_gc = gc().allocate_object();
    GCObject* array_proto_gc = gc().allocate_object();
    GCObject* string_proto_gc = gc().allocate_object();
    GCObject* number_proto_gc = gc().allocate_object();
    GCObject* boolean_proto_gc = gc().allocate_object();
    GCObject* function_proto_gc = gc().allocate_object();
    GCObject* error_proto_gc = gc().allocate_object();
    GCObject* promise_proto_gc = gc().allocate_object();
    GCObject* date_proto_gc = gc().allocate_object();
    GCObject* symbol_proto_gc = gc().allocate_object();

    // Set up prototype chain: all prototypes inherit from Object.prototype
    JSObject* array_proto = GarbageCollector::get_object(array_proto_gc);
    array_proto->prototype = object_proto_gc;
    JSObject* string_proto = GarbageCollector::get_object(string_proto_gc);
    string_proto->prototype = object_proto_gc;
    JSObject* number_proto = GarbageCollector::get_object(number_proto_gc);
    number_proto->prototype = object_proto_gc;
    JSObject* boolean_proto = GarbageCollector::get_object(boolean_proto_gc);
    boolean_proto->prototype = object_proto_gc;
    JSObject* function_proto = GarbageCollector::get_object(function_proto_gc);
    function_proto->prototype = object_proto_gc;
    function_proto->is_function = true;
    function_proto->is_native = true;
    function_proto->native_fn = [](Interpreter*, JSValue, std::vector<JSValue>&) -> JSValue {
        return make_undefined();
    };
    JSObject* error_proto = GarbageCollector::get_object(error_proto_gc);
    error_proto->prototype = object_proto_gc;
    error_proto->is_error = true;
    JSObject* promise_proto = GarbageCollector::get_object(promise_proto_gc);
    promise_proto->prototype = object_proto_gc;
    JSObject* date_proto = GarbageCollector::get_object(date_proto_gc);
    date_proto->prototype = object_proto_gc;
    JSObject* symbol_proto = GarbageCollector::get_object(symbol_proto_gc);
    symbol_proto->prototype = object_proto_gc;

    // Initialize each set of builtins
    init_object_builtins(interp, global, object_proto_gc);
    init_array_builtins(interp, global, array_proto_gc);
    init_string_builtins(interp, global, string_proto_gc);
    init_number_builtins(interp, global, number_proto_gc);
    init_boolean_builtins(interp, global, boolean_proto_gc);
    init_function_builtins(interp, global, function_proto_gc);
    init_math_builtins(interp, global);
    init_json_builtins(interp, global);
    init_console_builtins(interp, global);
    init_error_builtins(interp, global, error_proto_gc);
    init_promise_builtins(interp, global, promise_proto_gc);
    init_date_builtins(interp, global, date_proto_gc);
    init_symbol_builtins(interp, global, symbol_proto_gc);
    init_global_functions(interp, global);

    // Store prototype pointers on the interpreter for use by eval
    interp.array_proto = array_proto;
    interp.string_proto = string_proto;
    interp.object_proto = GarbageCollector::get_object(object_proto_gc);
    interp.function_proto = function_proto;
    interp.number_proto = number_proto;
    interp.boolean_proto = boolean_proto;

    // Set global properties
    set_property(global, "undefined", make_undefined());
    set_property(global, "NaN", make_number(std::numeric_limits<double>::quiet_NaN()));
    set_property(global, "Infinity", make_number(std::numeric_limits<double>::infinity()));
    set_property(global, "globalThis", JSValue::object_val(global));

    // Set prototype on global object
    global->prototype = object_proto_gc;
}

// =============================================================================
// Helper implementations
// =============================================================================

GCObject* make_native_gc(Interpreter& /*interp*/, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn) {
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    obj->is_function = true;
    obj->is_native = true;
    obj->class_name = name;
    obj->native_fn = std::move(fn);
    return gco;
}

void set_method(JSObject* obj, const std::string& name,
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> fn,
    Interpreter& /*interp*/) {
    GCObject* gco = gc().allocate_object();
    JSObject* method = GarbageCollector::get_object(gco);
    method->is_function = true;
    method->is_native = true;
    method->native_fn = std::move(fn);
    method->class_name = name;
    obj->set_property(name, JSValue::function_val(method));
}

void set_property(JSObject* obj, const std::string& name, JSValue value) {
    obj->set_property(name, value);
}

} // namespace chinstrap