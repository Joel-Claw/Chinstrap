// =============================================================================
// js_builtins.hpp - Built-in Objects and Functions
// =============================================================================
//
// TEACHING NOTE: JavaScript Built-in Objects
// ============================================
//
// JavaScript comes with a rich set of built-in objects and functions. These
// are available in every JavaScript environment. The most important ones are:
//
// Global objects:
//   - Object: base object with prototype methods
//   - Array: array constructor and methods
//   - String: string constructor and methods
//   - Number: number constructor and methods
//   - Boolean: boolean constructor
//   - Function: function constructor
//   - Math: mathematical constants and functions
//   - JSON: JSON.parse and JSON.stringify
//   - Date: date and time
//   - Error: error constructor (and subclasses)
//   - RegExp: regular expression constructor
//   - Map, Set, WeakMap, WeakSet: collection types
//   - Promise: async/await support
//   - Symbol: unique identifiers
//
// Global functions:
//   - console.log/error/warn/info
//   - setTimeout/clearTimeout
//   - parseInt, parseFloat, isNaN, isFinite
//   - encodeURIComponent, decodeURIComponent
//
// How V8 implements builtins:
// ============================
// V8 implements builtins in three ways:
//   1. C++ builtins (src/builtins/) - for performance-critical code
//   2. JavaScript builtins (src/builtins/js/) - for complex logic
//   3. CodeStubAssembler builtins - for platform-specific optimization
//
// V8 builtins are registered in a builtin table and called by ID from
// bytecode. This allows fast dispatch and inlining.
//
// =============================================================================

#ifndef CHINSTRAP_JS_BUILTINS_HPP
#define CHINSTRAP_JS_BUILTINS_HPP

#include "js_value.hpp"
#include "js_gc.hpp"

namespace chinstrap {

class Interpreter;

// Initialize all built-in objects and functions
// Call this once after creating the Interpreter and global object
void init_builtins(Interpreter& interp, JSObject* global);

// Create the Object.prototype and Object constructor
void init_object_builtins(Interpreter& interp, JSObject* global, GCObject* object_proto);

// Create the Array.prototype and Array constructor
void init_array_builtins(Interpreter& interp, JSObject* global, GCObject* array_proto);

// Create the String.prototype and String constructor
void init_string_builtins(Interpreter& interp, JSObject* global, GCObject* string_proto);

// Create the Number.prototype and Number constructor
void init_number_builtins(Interpreter& interp, JSObject* global, GCObject* number_proto);

// Create the Boolean.prototype and Boolean constructor
void init_boolean_builtins(Interpreter& interp, JSObject* global, GCObject* boolean_proto);

// Create the Function.prototype and Function constructor
void init_function_builtins(Interpreter& interp, JSObject* global, GCObject* function_proto);

// Create Math object
void init_math_builtins(Interpreter& interp, JSObject* global);

// Create JSON object
void init_json_builtins(Interpreter& interp, JSObject* global);

// Create console object
void init_console_builtins(Interpreter& interp, JSObject* global);

// Create Error constructors
void init_error_builtins(Interpreter& interp, JSObject* global, GCObject* error_proto);

// Create Promise
void init_promise_builtins(Interpreter& interp, JSObject* global, GCObject* promise_proto);

// Create Date
void init_date_builtins(Interpreter& interp, JSObject* global, GCObject* date_proto);

// Create global functions (parseInt, parseFloat, etc.)
void init_global_functions(Interpreter& interp, JSObject* global);

// Create Symbol
void init_symbol_builtins(Interpreter& interp, JSObject* global, GCObject* symbol_proto);

} // namespace chinstrap

#endif // CHINSTRAP_JS_BUILTINS_HPP