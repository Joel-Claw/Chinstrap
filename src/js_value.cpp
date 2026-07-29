// =============================================================================
// js_value.cpp - JavaScript Value Implementation
// =============================================================================
//
// TEACHING NOTE: Value Operations
// ================================
// This file implements the core value operations: type coercion, equality,
// and string conversion. These are the most fundamental operations in JavaScript.
//
// JavaScript has both strict (===) and loose (==) equality. Strict equality
// compares values without type conversion. Loose equality performs type
// coercion before comparison. The coercion rules are specified in ECMAScript
// and are notoriously confusing - which is why most style guides recommend
// always using ===.
//
// The to_string and to_number conversions are used everywhere in JS. For
// example, "3" + 4 = "34" (string concatenation) but "3" - 4 = -1 (numeric
// subtraction). The operator determines the coercion, not the operands.
//
// How V8 does it: V8 generates specialized code for each operation based on
// the types it sees at runtime (type feedback). If it always sees two numbers,
// it generates a fast integer path. If it sees mixed types, it falls back to
// a slow path that does full coercion.
// =============================================================================

#include "js_value.hpp"
#include "js_gc.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>

namespace chinstrap {

// ---- Forward declarations for GC ----
// The GC manages all heap objects. We declare it here so JSValue can
// allocate strings through the GC. The actual GC is in js_gc.hpp/cpp.
// For now, we use a simple allocation scheme.

// Simple allocator for JSString (will be replaced by GC later)
static std::vector<std::unique_ptr<JSString>> g_string_pool;
static std::vector<std::unique_ptr<JSSymbol>> g_symbol_pool;

// Called by g_gc_reset() to clear the string/symbol pools
void clear_string_pools() {
    g_string_pool.clear();
    g_symbol_pool.clear();
}

JSString* alloc_string(const std::string& s) {
    auto ptr = std::make_unique<JSString>(s);
    JSString* raw = ptr.get();
    g_string_pool.push_back(std::move(ptr));
    return raw;
}

JSSymbol* alloc_symbol(const std::string& desc) {
    auto ptr = std::make_unique<JSSymbol>(desc);
    JSSymbol* raw = ptr.get();
    g_symbol_pool.push_back(std::move(ptr));
    return raw;
}

// ---- JSObject ----

JSObject::JSObject() = default;
JSObject::~JSObject() = default;

bool JSObject::has_property(const std::string& name) {
    auto it = property_index.find(name);
    if (it != property_index.end()) return true;
    if (prototype) {
        JSObject* proto_obj = GarbageCollector::get_object(prototype);
        if (proto_obj) {
            return proto_obj->has_property(name);
        }
    }
    return false;
}

JSValue JSObject::get_property(const std::string& name) {
    auto it = property_index.find(name);
    if (it != property_index.end()) {
        return properties[it->second].second.value;
    }
    // Walk prototype chain
    if (prototype) {
        JSObject* proto_obj = GarbageCollector::get_object(prototype);
        if (proto_obj) {
            return proto_obj->get_property(name);
        }
    }
    return JSValue::undefined();
}

void JSObject::set_property(const std::string& name, JSValue value) {
    auto it = property_index.find(name);
    if (it != property_index.end()) {
        properties[it->second].second.value = value;
    } else {
        property_index[name] = properties.size();
        PropertyDescriptor pd;
        pd.value = value;
        properties.push_back({name, pd});
    }
}

void JSObject::set_property(const std::string& name, JSValue value,
                            bool writable, bool enumerable, bool configurable) {
    auto it = property_index.find(name);
    if (it != property_index.end()) {
        size_t idx = it->second;
        properties[idx].second.value = value;
        properties[idx].second.writable = writable;
        properties[idx].second.enumerable = enumerable;
        properties[idx].second.configurable = configurable;
    } else {
        property_index[name] = properties.size();
        PropertyDescriptor pd;
        pd.value = value;
        pd.writable = writable;
        pd.enumerable = enumerable;
        pd.configurable = configurable;
        properties.push_back({name, pd});
    }
}

void JSObject::delete_property(const std::string& name) {
    auto it = property_index.find(name);
    if (it != property_index.end()) {
        size_t idx = it->second;
        properties.erase(properties.begin() + static_cast<long>(idx));
        // Rebuild index
        property_index.clear();
        for (size_t i = 0; i < properties.size(); i++) {
            property_index[properties[i].first] = i;
        }
    }
}

std::vector<std::string> JSObject::enumerable_keys() {
    std::vector<std::string> keys;
    for (auto& p : properties) {
        if (p.second.enumerable) {
            keys.push_back(p.first);
        }
    }
    return keys;
}

// ---- JSValue methods ----

bool JSValue::to_boolean() const {
    switch (type) {
        case ValueType::Undefined:
        case ValueType::Null:
            return false;
        case ValueType::Boolean:
            return boolean;
        case ValueType::Number:
            if (std::isnan(number)) return false;
            if (number == 0.0) return false;
            return true;
        case ValueType::String:
            return !str->value.empty();
        case ValueType::Object:
        case ValueType::Array:
        case ValueType::Function:
            return true;
        case ValueType::Symbol:
            return true;
    }
    return false;
}

double JSValue::to_number() const {
    switch (type) {
        case ValueType::Undefined:
            return std::numeric_limits<double>::quiet_NaN();
        case ValueType::Null:
            return 0.0;
        case ValueType::Boolean:
            return boolean ? 1.0 : 0.0;
        case ValueType::Number:
            return number;
        case ValueType::String: {
            // Trim whitespace
            std::string s = str->value;
            size_t start = s.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return 0.0;
            size_t end = s.find_last_not_of(" \t\n\r\f\v");
            s = s.substr(start, end - start + 1);
            if (s.empty()) return 0.0;
            // Handle special cases
            if (s == "Infinity" || s == "+Infinity") return std::numeric_limits<double>::infinity();
            if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
            try {
                size_t pos;
                double d = std::stod(s, &pos);
                if (pos != s.length()) return std::numeric_limits<double>::quiet_NaN();
                return d;
            } catch (...) {
                return std::numeric_limits<double>::quiet_NaN();
            }
        }
        case ValueType::Object:
        case ValueType::Array:
        case ValueType::Function:
            // Objects: NaN unless it has a valueOf that returns a number
            return std::numeric_limits<double>::quiet_NaN();
        case ValueType::Symbol:
            // Symbol to number throws in real JS - we return NaN
            return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::string JSValue::to_string() const {
    switch (type) {
        case ValueType::Undefined:
            return "undefined";
        case ValueType::Null:
            return "null";
        case ValueType::Boolean:
            return boolean ? "true" : "false";
        case ValueType::Number: {
            if (std::isnan(number)) return "NaN";
            if (std::isinf(number)) return number > 0 ? "Infinity" : "-Infinity";
            if (number == 0.0) return "0";
            // Integer check
            if (number == static_cast<double>(static_cast<long long>(number)) &&
                std::abs(number) < 1e21) {
                std::ostringstream oss;
                oss << static_cast<long long>(number);
                return oss.str();
            }
            // TEACHING NOTE: Number to String Conversion
            // =======================================
            // JavaScript uses the shortest representation that round-trips
            // back to the same double. For simplicity, we use a moderate
            // precision and strip trailing zeros. V8 uses a much more
            // sophisticated algorithm based on David Gaygrintf.
            std::ostringstream oss;
            oss << std::setprecision(15) << number;
            std::string s = oss.str();
            // Remove trailing zeros after decimal point
            if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
                size_t last = s.find_last_not_of('0');
                if (s[last] == '.') last--;
                s = s.substr(0, last + 1);
            }
            return s;
        }
        case ValueType::String:
            return str->value;
        case ValueType::Object:
        case ValueType::Array: {
            // Arrays: comma-join elements
            if (object && object->is_array) {
                // Get length
                auto len_prop = object->get_property("length");
                size_t len = static_cast<size_t>(len_prop.to_number());
                std::string result;
                for (size_t i = 0; i < len; i++) {
                    if (i > 0) result += ",";
                    auto elem = object->get_property(std::to_string(i));
                    if (!elem.is_nullish()) {
                        result += elem.to_string();
                    }
                }
                return result;
            }
            return "[object Object]";
        }
        case ValueType::Function:
            return "function";
        case ValueType::Symbol:
            return "Symbol(" + symbol->description + ")";
    }
    return "";
}

bool JSValue::strict_equals(const JSValue& other) const {
    if (type != other.type) return false;
    switch (type) {
        case ValueType::Undefined:
        case ValueType::Null:
            return true;
        case ValueType::Boolean:
            return boolean == other.boolean;
        case ValueType::Number:
            // NaN !== NaN in JavaScript
            if (std::isnan(number) || std::isnan(other.number)) return false;
            return number == other.number;
        case ValueType::String:
            return str->value == other.str->value;
        case ValueType::Object:
        case ValueType::Array:
        case ValueType::Function:
            return object == other.object; // reference equality
        case ValueType::Symbol:
            return symbol == other.symbol;
    }
    return false;
}

bool JSValue::loose_equals(const JSValue& other) const {
    // Same type: use strict equality
    if (type == other.type) return strict_equals(other);

    // null == undefined
    if (is_nullish() && other.is_nullish()) return true;

    // number == string: convert string to number
    if (is_number() && other.is_string()) {
        return number == other.to_number();
    }
    if (is_string() && other.is_number()) {
        return to_number() == other.number;
    }

    // boolean to number
    if (is_boolean()) {
        JSValue converted = JSValue::number_val(to_boolean() ? 1.0 : 0.0);
        return converted.loose_equals(other);
    }
    if (other.is_boolean()) {
        return loose_equals(JSValue::number_val(other.to_boolean() ? 1.0 : 0.0));
    }

    // object == primitive: convert object to primitive
    if (is_object() && (other.is_number() || other.is_string())) {
        JSValue prim = make_string(to_string());
        return prim.loose_equals(other);
    }
    if (other.is_object() && (is_number() || is_string())) {
        JSValue prim = make_string(other.to_string());
        return prim.loose_equals(*this);
    }

    return false;
}

std::string JSValue::type_of() const {
    switch (type) {
        case ValueType::Undefined: return "undefined";
        case ValueType::Null: return "object"; // typeof null === "object" (JS quirk)
        case ValueType::Boolean: return "boolean";
        case ValueType::Number: return "number";
        case ValueType::String: return "string";
        case ValueType::Object: return "object";
        case ValueType::Array: return "object";
        case ValueType::Function: return "function";
        case ValueType::Symbol: return "symbol";
    }
    return "undefined";
}

bool JSValue::is_nan() const {
    return type == ValueType::Number && std::isnan(number);
}

// ---- Factory functions ----

JSValue make_string(const std::string& s) {
    return JSValue::string_val(alloc_string(s));
}

JSValue make_number(double n) {
    return JSValue::number_val(n);
}

JSValue make_bool(bool b) {
    return JSValue::boolean_val(b);
}

JSValue make_undefined() {
    return JSValue::undefined();
}

JSValue make_null() {
    return JSValue::null_val();
}

} // namespace chinstrap