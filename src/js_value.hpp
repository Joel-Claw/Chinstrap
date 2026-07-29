// =============================================================================
// js_value.hpp - JavaScript Value Representation
// =============================================================================
//
// TEACHING NOTE: NaN-Boxing Explained
// ================================
//
// In JavaScript, all values can be represented in 64 bits. The IEEE 754 double
// precision floating point format has a special value: NaN (Not a Number).
// There are approximately 2^51 different NaN bit patterns, but only one is
// actually used for NaN in arithmetic. We can use all the other NaN bit
// patterns to encode other types of values.
//
// The IEEE 754 double format:
//   [sign 1 bit] [exponent 11 bits] [mantissa 52 bits]
//
// A value is NaN if the exponent is all 1s (0x7FF) and the mantissa is nonzero.
// The sign bit can be 0 or 1, so there are about 2^53 possible NaN bit
// patterns.
//
// Our NaN-boxing scheme (inspired by SpiderMonkey/JSVal):
//   - Doubles: stored directly as their IEEE 754 bit pattern
//   - Non-doubles: stored in the NaN space with a tag
//
// We use the "quiet NaN" space (sign=1, exponent=all 1s, mantissa top bit
// set). The upper bits encode the type tag, the lower 48 bits encode the
// payload (pointers or small integers).
//
// Why 48 bits for payload? On most 64-bit systems, user-space pointers only
// use 48 bits. This lets us store pointers directly in the NaN-boxed value.
//
// How V8 does it: V8 uses a different approach called "Tagged Pointers" with
// SMI (Small Integer) tags. Pointers have the low bit set to distinguish
// them from integers. V8 also has "HeapNumber" objects for doubles that do
// not fit in SMI range. V8 does not use NaN-boxing because it relies on its
// GC and object model instead. NaN-boxing is more common in SpiderMonkey
// (Firefox) and JavaScriptCore (Safari).
//
// Our value types:
//   - UNDEFINED: a special value
//   - NULL: a special value
//   - BOOLEAN: true/false
//   - NUMBER: IEEE 754 double
//   - STRING: pointer to a string object
//   - OBJECT: pointer to a heap object
//   - ARRAY: pointer to an array object (arrays ARE objects in JS)
//   - FUNCTION: pointer to a function object
//   - SYMBOL: pointer to a symbol object
//
// =============================================================================

#ifndef CHINSTRAP_JS_VALUE_HPP
#define CHINSTRAP_JS_VALUE_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <utility>

namespace chinstrap {

// Forward declarations
class JSObject;
class JSString;
class JSSymbol;
class Interpreter;
struct GCObject;

// =============================================================================
// Value type tags
// =============================================================================

// TEACHING NOTE: Type Tags
// ========================
// We use a tagged union (enum + union) instead of NaN-boxing for clarity.
// The tradeoff: our JSValue is larger than 8 bytes (typically 16-24 bytes
// depending on alignment), while NaN-boxing fits everything in 8 bytes.
// NaN-boxing saves memory but makes the code harder to read.
//
// In a production engine, the memory savings matter because JavaScript
// creates millions of values. V8 uses tagged pointers (1 tag bit) for
// pointers and small integers, and HeapNumber objects for doubles.
// SpiderMonkey uses NaN-boxing. Both approaches are valid.
// =============================================================================

enum class ValueType {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Object,
    Array,
    Function,
    Symbol,
};

// =============================================================================
// JSValue - the main value type (tagged union)
// =============================================================================
//
// TEACHING NOTE: Tagged Union vs NaN-Boxing
// =========================================
// We define JSValue before JSObject because JSObject contains JSValue members
// (property values, internal slots, bound this/args). This is a common
// pattern in C++ JS engine implementations: the value type is self-contained
// or uses forward declarations for pointer types in its union.
//
// Our union stores pointers to heap-allocated objects (JSObject, JSString,
// JSSymbol). These pointers are valid as long as the GC manages them.
// =============================================================================

class JSValue {
public:
    ValueType type = ValueType::Undefined;

    // Union of possible values. We store pointers for heap types.
    union {
        bool boolean;
        double number;
        JSObject* object;     // for Object, Array, Function
        JSString* str;        // for String
        JSSymbol* symbol;     // for Symbol
    };

    // Default constructor: undefined
    JSValue() : type(ValueType::Undefined), number(0) {}

    // Static factory methods
    static JSValue undefined() { JSValue v; v.type = ValueType::Undefined; return v; }
    static JSValue null_val() { JSValue v; v.type = ValueType::Null; v.number = 0; return v; }
    static JSValue boolean_val(bool b) { JSValue v; v.type = ValueType::Boolean; v.boolean = b; return v; }
    static JSValue number_val(double n) { JSValue v; v.type = ValueType::Number; v.number = n; return v; }
    static JSValue string_val(JSString* s) { JSValue v; v.type = ValueType::String; v.str = s; return v; }
    static JSValue object_val(JSObject* o) { JSValue v; v.type = ValueType::Object; v.object = o; return v; }
    static JSValue array_val(JSObject* o) { JSValue v; v.type = ValueType::Array; v.object = o; return v; }
    static JSValue function_val(JSObject* o) { JSValue v; v.type = ValueType::Function; v.object = o; return v; }
    static JSValue symbol_val(JSSymbol* s) { JSValue v; v.type = ValueType::Symbol; v.symbol = s; return v; }

    // Type checking
    bool is_undefined() const { return type == ValueType::Undefined; }
    bool is_null() const { return type == ValueType::Null; }
    bool is_boolean() const { return type == ValueType::Boolean; }
    bool is_number() const { return type == ValueType::Number; }
    bool is_string() const { return type == ValueType::String; }
    bool is_object() const { return type == ValueType::Object || type == ValueType::Array || type == ValueType::Function; }
    bool is_array() const { return type == ValueType::Array; }
    bool is_function() const { return type == ValueType::Function; }
    bool is_symbol() const { return type == ValueType::Symbol; }
    bool is_nullish() const { return type == ValueType::Null || type == ValueType::Undefined; }

    // Value extraction
    bool as_boolean() const { return boolean; }
    double as_number() const { return number; }
    JSString* as_string() const { return str; }
    JSObject* as_object() const { return object; }
    JSObject* as_function() const { return object; }
    JSSymbol* as_symbol() const { return symbol; }

    // Coercion (loose)
    bool to_boolean() const;
    double to_number() const;
    std::string to_string() const;

    // Strict equality comparison (===)
    bool strict_equals(const JSValue& other) const;

    // Abstract equality comparison (==)
    bool loose_equals(const JSValue& other) const;

    // Type string for error messages
    std::string type_of() const;

    // Check if number is NaN
    bool is_nan() const;
};

// =============================================================================
// Property Descriptor (for object properties)
// =============================================================================

// TEACHING NOTE: Property Descriptors
// ===================================
// In JavaScript, object properties have attributes: writable, enumerable,
// configurable. The property descriptor captures these. V8 stores properties
// in "transition trees" and uses hidden classes (Maps) to optimize property
// access. We use a simpler hash map approach.
// =============================================================================

struct PropertyDescriptor {
    JSValue value;
    bool writable = true;
    bool enumerable = true;
    bool configurable = true;
    bool is_accessor = false;
    JSValue getter; // for accessor properties
    JSValue setter; // for accessor properties
};

// =============================================================================
// JSObject - the base heap object
// =============================================================================

// TEACHING NOTE: JSObject Structure
// ==================================
// Every non-primitive value in JavaScript is an Object. Arrays, Functions,
// Errors, etc. are all objects with different internal slots.
//
// V8 represents objects with a hidden class (Map) that describes the shape
// (property names and layout). This allows V8 to use inline caches for
// property access. We use a simple hash map instead.
//
// The prototype chain is fundamental to JavaScripts inheritance model.
// Every object has a [[Prototype]] internal slot that points to another
// object (or null). Property lookups walk this chain.
// =============================================================================

class JSObject {
public:
    JSObject();
    ~JSObject();

    // Properties (ordered map for predictable iteration)
    std::vector<std::pair<std::string, PropertyDescriptor>> properties;
    std::unordered_map<std::string, size_t> property_index;

    // Internal prototype (for prototype chain lookup)
    GCObject* prototype = nullptr; // raw pointer, managed by GC

    // Internal slots
    std::unordered_map<std::string, JSValue> internal_slots;

    // Is this an array object?
    bool is_array = false;

    // Is this an error object?
    bool is_error = false;

    // Class name (for Object.prototype.toString)
    std::string class_name = "Object";

    // For functions: the function body
    bool is_function = false;
    bool is_native = false;
    bool is_arrow = false;
    bool is_generator = false;
    bool is_async = false;
    bool is_constructor = false;

    // Native function pointer
    std::function<JSValue(Interpreter*, JSValue, std::vector<JSValue>&)> native_fn;

    // For closures: the scope/environment
    GCObject* closure_scope = nullptr;

    // For functions: parameter names
    std::vector<std::string> params;

    // For functions: the AST body (if not native)
    void* ast_body = nullptr; // points to AST BlockStatement

    // For closures: pointer to the defining scope (Scope*)
    // Stored as void* to avoid circular dependency between js_value.hpp and js_interpreter.hpp
    void* defining_scope = nullptr;

    // For bound functions
    JSValue bound_this;
    std::vector<JSValue> bound_args;

    // extensible flag
    bool extensible = true;

    // Property helpers
    bool has_property(const std::string& name);
    JSValue get_property(const std::string& name);
    void set_property(const std::string& name, JSValue value);
    void set_property(const std::string& name, JSValue value, bool writable, bool enumerable, bool configurable);
    void delete_property(const std::string& name);
    std::vector<std::string> enumerable_keys();

    // GC bookkeeping
    bool gc_marked = false;
    GCObject* gc_next = nullptr; // for GC linked list
};

// =============================================================================
// JSString - string value
// =============================================================================

// TEACHING NOTE: String Representation
// ====================================
// JavaScript strings are immutable sequences of UTF-16 code units. V8 uses
// different internal representations depending on content: Latin-1 (1 byte
// per char) for ASCII, UTF-16 (2 bytes) for others, and "sliced strings"
// and "cons strings" for efficient concatenation. We use std::string for
// simplicity, treating strings as UTF-8.
// =============================================================================

class JSString {
public:
    std::string value;
    explicit JSString(std::string v) : value(std::move(v)) {}
    bool gc_marked = false;
    GCObject* gc_next = nullptr;
};

// =============================================================================
// JSSymbol - symbol value
// =============================================================================

class JSSymbol {
public:
    std::string description;
    explicit JSSymbol(std::string d) : description(std::move(d)) {}
    bool gc_marked = false;
    GCObject* gc_next = nullptr;
};

// =============================================================================
// Helper: create JS values from C++ types
// =============================================================================

JSValue make_string(const std::string& s);
JSValue make_number(double n);
JSValue make_bool(bool b);
JSValue make_undefined();
JSValue make_null();

// Clear string/symbol pools (called by g_gc_reset)
void clear_string_pools();

} // namespace chinstrap

#endif // CHINSTRAP_JS_VALUE_HPP