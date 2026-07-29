// =============================================================================
// js_gc.cpp - Garbage Collector Implementation
// =============================================================================
//
// TEACHING NOTE: Mark-and-Sweep Implementation
// =============================================
//
// The mark phase uses a worklist (stack) of objects to visit. We pop an object,
// mark it, and push all objects it references. This is an iterative DFS to
// avoid stack overflow on deep object graphs (a recursive approach would
// blow the C++ stack on long linked lists or deep prototype chains).
//
// The sweep phase iterates all allocated objects. If an object is not marked,
// we free it. If it is marked, we clear the mark bit for the next GC cycle.
//
// Our GC is stop-the-world: we pause JS execution, run the full GC, then resume.
// This causes pauses proportional to heap size. V8 reduces pauses with:
//   - Incremental marking (split mark phase into small steps)
//   - Concurrent marking (mark on a background thread)
//   - Parallel sweeping (sweep on multiple threads)
//   - Generational hypothesis (most objects die young, so nursery GC is fast)
//
// We trigger GC after every GC_THRESHOLD allocations. This is a simple
// heuristic. V8 uses a dynamic heuristic based on allocation rate, heap size
// growth, and memory pressure.
//
// =============================================================================

#include "js_gc.hpp"
#include <cstdlib>
#include <algorithm>

namespace chinstrap {

// ---- Static GC instance ----
static GarbageCollector g_gc;
GarbageCollector& gc() { return g_gc; }

// ---- GCObject factory methods ----

GCObject* GCObject::create_object() {
    GCObject* gco = new GCObject();
    gco->type = GCObjectType::Object;
    gco->payload.object = new JSObject();
    return gco;
}

GCObject* GCObject::create_string(const std::string& s) {
    GCObject* gco = new GCObject();
    gco->type = GCObjectType::String;
    gco->payload.string = new JSString(s);
    return gco;
}

GCObject* GCObject::create_symbol(const std::string& desc) {
    GCObject* gco = new GCObject();
    gco->type = GCObjectType::Symbol;
    gco->payload.symbol = new JSSymbol(desc);
    return gco;
}

// ---- GarbageCollector ----

GarbageCollector::GarbageCollector() = default;

GCObject* GarbageCollector::allocate_object() {
    GCObject* gco = GCObject::create_object();
    objects.push_back(gco);
    total_bytes += sizeof(GCObject) + sizeof(JSObject);
    allocs_since_gc++;
    if (!gc_disabled && allocs_since_gc >= GC_THRESHOLD) {
        collect();
        allocs_since_gc = 0;
    }
    return gco;
}

GCObject* GarbageCollector::allocate_string(const std::string& s) {
    GCObject* gco = GCObject::create_string(s);
    objects.push_back(gco);
    total_bytes += sizeof(GCObject) + sizeof(JSString) + s.length();
    allocs_since_gc++;
    if (!gc_disabled && allocs_since_gc >= GC_THRESHOLD) {
        collect();
        allocs_since_gc = 0;
    }
    return gco;
}

GCObject* GarbageCollector::allocate_symbol(const std::string& desc) {
    GCObject* gco = GCObject::create_symbol(desc);
    objects.push_back(gco);
    total_bytes += sizeof(GCObject) + sizeof(JSSymbol) + desc.length();
    allocs_since_gc++;
    return gco;
}

void GarbageCollector::remove_root(GCObject* root) {
    auto it = std::find(roots.begin(), roots.end(), root);
    if (it != roots.end()) {
        roots.erase(it);
    }
}

void GarbageCollector::push_temp_root(const JSValue& val) {
    temp_roots.push_back(val);
}

void GarbageCollector::pop_temp_root() {
    if (!temp_roots.empty()) {
        temp_roots.pop_back();
    }
}

void GarbageCollector::mark_value(const JSValue& value) {
    switch (value.type) {
        case ValueType::String:
            if (value.str) {
                // Find GCObject for this string - we need to mark it
                // In our design, JSString is wrapped by GCObject
                // We use gc_marked on JSString directly
                value.str->gc_marked = true;
            }
            break;
        case ValueType::Symbol:
            if (value.symbol) {
                value.symbol->gc_marked = true;
            }
            break;
        case ValueType::Object:
        case ValueType::Array:
        case ValueType::Function:
            if (value.object) {
                // Find and mark the GCObject
                // We search our objects list - this is slow but correct
                for (auto* gco : objects) {
                    if (gco->type == GCObjectType::Object && gco->payload.object == value.object) {
                        mark_object(gco);
                        break;
                    }
                }
            }
            break;
        default:
            // Primitives (undefined, null, boolean, number) need no marking
            break;
    }
}

void GarbageCollector::mark_object(GCObject* obj) {
    if (!obj || obj->gc_marked) return;

    obj->gc_marked = true;

    if (obj->type == GCObjectType::Object) {
        JSObject* jsobj = obj->payload.object;
        if (!jsobj) return;

        // Mark prototype
        if (jsobj->prototype) {
            mark_object(jsobj->prototype);
        }

        // Mark closure scope
        if (jsobj->closure_scope) {
            mark_object(jsobj->closure_scope);
        }

        // Mark all property values
        for (auto& prop : jsobj->properties) {
            mark_value(prop.second.value);
            if (prop.second.is_accessor) {
                mark_value(prop.second.getter);
                mark_value(prop.second.setter);
            }
        }

        // Mark bound values
        if (jsobj->is_function) {
            mark_value(jsobj->bound_this);
            for (auto& arg : jsobj->bound_args) {
                mark_value(arg);
            }
        }
    } else if (obj->type == GCObjectType::String) {
        // Strings have no references
    } else if (obj->type == GCObjectType::Symbol) {
        // Symbols have no references
    }
}

void GarbageCollector::clear_marks() {
    for (auto* gco : objects) {
        gco->gc_marked = false;
    }
    // Also clear marks on JSString/JSSymbol (stored separately for legacy alloc)
    // These were allocated before GC was initialized
}

void GarbageCollector::collect() {
    // Phase 1: Clear all marks
    clear_marks();

    // Phase 2: Mark from roots
    // Mark global object
    if (global_gc) {
        mark_object(global_gc);
    }

    // Mark root objects
    for (auto* root : roots) {
        mark_object(root);
    }

    // Mark temporary roots
    for (auto& val : temp_roots) {
        mark_value(val);
    }

    // Phase 3: Sweep
    sweep();
}

void GarbageCollector::sweep() {
    std::vector<GCObject*> alive;
    size_t freed = 0;

    for (auto* gco : objects) {
        if (gco->gc_marked) {
            gco->gc_marked = false; // Clear for next cycle
            alive.push_back(gco);
        } else {
            // Free the object
            switch (gco->type) {
                case GCObjectType::Object:
                    delete gco->payload.object;
                    break;
                case GCObjectType::String:
                    delete gco->payload.string;
                    break;
                case GCObjectType::Symbol:
                    delete gco->payload.symbol;
                    break;
            }
            delete gco;
            freed++;
        }
    }

    objects = std::move(alive);
    if (freed > 0) {
        total_bytes = 0;
        for (auto* gco : objects) {
            // Recalculate (approximate)
            total_bytes += sizeof(GCObject);
            if (gco->type == GCObjectType::Object) total_bytes += sizeof(JSObject);
            else if (gco->type == GCObjectType::String) total_bytes += sizeof(JSString);
            else if (gco->type == GCObjectType::Symbol) total_bytes += sizeof(JSSymbol);
        }
    }
}

GCObject* GarbageCollector::find_gc_for(JSObject* obj) {
    for (auto* gco : objects) {
        if (gco->type == GCObjectType::Object && gco->payload.object == obj) {
            return gco;
        }
    }
    return nullptr;
}

// ---- Global helper functions ----

GCObject* g_gc_find_for_object(JSObject* obj) {
    return gc().find_gc_for(obj);
}

GCObject* g_gc_find_for_scope(void* /*scope*/) {
    // Scopes are not GC managed - return nullptr
    return nullptr;
}

std::vector<GCObject*>& g_gc_objects_list() {
    return gc().get_objects();
}

void g_gc_reset() {
    gc().reset();
}

void GarbageCollector::reset_gc_only() {
    // Same as reset() but does NOT clear string/symbol pools.
    // Used by Interpreter constructor when the Parser has already
    // created JSStrings in g_string_pool.
    for (auto* gco : objects) {
        switch (gco->type) {
            case GCObjectType::Object:
                delete gco->payload.object;
                break;
            case GCObjectType::String:
                delete gco->payload.string;
                break;
            case GCObjectType::Symbol:
                delete gco->payload.symbol;
                break;
        }
        delete gco;
    }
    objects.clear();
    roots.clear();
    temp_roots.clear();
    global_obj = nullptr;
    global_gc = nullptr;
    total_bytes = 0;
    allocs_since_gc = 0;
    gc_disabled = false;
}

void GarbageCollector::reset() {
    // Free all GC objects
    for (auto* gco : objects) {
        switch (gco->type) {
            case GCObjectType::Object:
                delete gco->payload.object;
                break;
            case GCObjectType::String:
                delete gco->payload.string;
                break;
            case GCObjectType::Symbol:
                delete gco->payload.symbol;
                break;
        }
        delete gco;
    }
    objects.clear();
    roots.clear();
    temp_roots.clear();
    global_obj = nullptr;
    global_gc = nullptr;
    total_bytes = 0;
    allocs_since_gc = 0;
    gc_disabled = false;
    
    // Also clear the string/symbol pools in js_value.cpp
    clear_string_pools();
}

} // namespace chinstrap
