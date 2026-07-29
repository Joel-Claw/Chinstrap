// =============================================================================
// js_gc.hpp - Garbage Collector for JavaScript Heap
// =============================================================================
//
// TEACHING NOTE: Garbage Collection in JavaScript Engines
// ========================================================
//
// JavaScript is a garbage-collected language. Programs allocate objects freely,
// and the runtime reclaims memory when objects are no longer reachable.
//
// GC Algorithms:
// ---------------
//
// 1. Reference Counting
//    Each object has a count of references to it. When count reaches 0, free it.
//    Pros: Simple, incremental. Cons: Cannot handle cycles (A->B, B->A).
//    JavaScript has many cycles (e.g., closures), so reference counting alone
//    is not sufficient.
//
// 2. Mark-and-Sweep (what we implement)
//    Phase 1 (Mark): Start from roots (stack, global object, registers) and
//    walk all reachable objects, marking them.
//    Phase 2 (Sweep): Walk all allocated objects, free unmarked ones, clear marks.
//
//    Pros: Handles cycles correctly. Cons: Pause the world (stop-the-world GC).
//
// 3. Generational GC (what V8 uses)
//    V8 divides the heap into "young generation" (nursery) and "old generation".
//    New objects go to the nursery. Most objects die young, so nursery GC is
//    frequent but fast (uses copying/survivor spaces). Objects that survive
//    multiple nursery collections get promoted to the old generation.
//    Old generation GC is less frequent and uses mark-sweep + compaction.
//
//    V8 also uses concurrent and parallel GC to reduce pause times.
//
// 4. Incremental GC
//    Mark phase is split into small increments interleaved with mutator
//    (program) execution. Reduces pause times at the cost of complexity
//    (write barriers needed).
//
// Our GC: Simple mark-and-sweep with a linked list of all allocated objects.
// The GC runs periodically (after N allocations) and on explicit request.
// Roots are: the value stack, the global object, and the scope chain.
//
// =============================================================================

#ifndef CHINSTRAP_JS_GC_HPP
#define CHINSTRAP_JS_GC_HPP

#include "js_value.hpp"
#include <vector>
#include <unordered_set>

namespace chinstrap {

// =============================================================================
// GCObject - wrapper for any GC-managed heap object
// =============================================================================

// TEACHING NOTE: GC Object Header
// ================================
// Every GC-managed object starts with a GC header. This header contains:
//   - The mark bit (for mark-and-sweep)
//   - A pointer to the next GC object (for the linked list of all objects)
//   - The type of the wrapped object
//
// In a production GC, the header would also contain: object size, forwarding
// pointer (for copying GC), age (for generational GC), and remembered-set bits.
// =============================================================================

enum class GCObjectType {
    Object,
    String,
    Symbol,
};

struct GCObject {
    GCObjectType type;
    bool gc_marked = false;
    GCObject* gc_next = nullptr;

    // Union of possible payloads
    union {
        JSObject* object;
        JSString* string;
        JSSymbol* symbol;
    } payload;

    // Constructor helpers
    static GCObject* create_object();
    static GCObject* create_string(const std::string& s);
    static GCObject* create_symbol(const std::string& desc);
};

// =============================================================================
// GarbageCollector - mark-and-sweep GC
// =============================================================================

// TEACHING NOTE: GC Roots
// ========================
// GC roots are the starting points for the mark phase. Anything reachable
// from roots is alive. Our roots are:
//   1. The global object
//   2. The interpreter value stack (local variables in current execution)
//   3. The scope chain (closures that are currently active)
//   4. Temporarily registered values (avoid premature collection)
//
// In V8, roots include: built-in objects, the global proxy, stack values,
// registers, handles, and persistent handles from C++ API.
// =============================================================================

class GarbageCollector {
public:
    GarbageCollector();

    // Allocation - creates a new GC object and registers it
    GCObject* allocate_object();
    GCObject* allocate_string(const std::string& s);
    GCObject* allocate_symbol(const std::string& desc);

    // Get JSObject from GCObject
    static JSObject* get_object(GCObject* gc) {
        if (!gc) return nullptr;
        return gc->payload.object;
    }

    // Get JSString from GCObject
    static JSString* get_string(GCObject* gc) {
        if (!gc) return nullptr;
        return gc->payload.string;
    }

    // Get JSSymbol from GCObject
    static JSSymbol* get_symbol(GCObject* gc) {
        if (!gc) return nullptr;
        return gc->payload.symbol;
    }

    // Find the GCObject wrapper for a given JSObject
    GCObject* find_gc_for(JSObject* obj);

    // Mark a value as reachable
    void mark_value(const JSValue& value);

    // Mark an object and everything it references
    void mark_object(GCObject* obj);

    // Add/remove a root
    void add_root(GCObject* root) { roots.push_back(root); }
    void remove_root(GCObject* root);

    // Register a temporary root (for values on the C++ stack)
    void push_temp_root(const JSValue& val);
    void pop_temp_root();

    // Set the global object as a root
    void set_global(JSObject* global) { global_obj = global; global_gc = find_gc_for(global); }

    // Run a full GC cycle
    void collect();

    // Reset the GC completely (for testing)
    void reset();

    // Reset GC objects only (does not clear string pools)
    // Used by Interpreter constructor to avoid destroying Parser strings
    void reset_gc_only();

    // Stats
    size_t object_count() const { return objects.size(); }
    size_t bytes_allocated() const { return total_bytes; }

    // Allocation threshold for triggering GC
    static constexpr size_t GC_THRESHOLD = 1024;

    // Disable automatic GC (used during init_builtins to prevent
    // collection of objects that are not yet rooted)
    bool gc_disabled = false;

    // Public access to objects list (for builtins)
    std::vector<GCObject*>& get_objects() { return objects; }

    // Reset allocation counter (call after disabling GC and doing init)
    void reset_alloc_counter() { allocs_since_gc = 0; }

private:
    // All allocated objects (linked list via gc_next)
    std::vector<GCObject*> objects;

    // GC roots
    std::vector<GCObject*> roots;
    std::vector<JSValue> temp_roots;
    JSObject* global_obj = nullptr;
    GCObject* global_gc = nullptr;

    // Stats
    size_t total_bytes = 0;
    size_t allocs_since_gc = 0;

    // Clear all mark bits
    void clear_marks();

    // Sweep unmarked objects
    void sweep();
};

// Global GC instance
GarbageCollector& gc();

// Helper functions used by interpreter and builtins

// Find the GCObject wrapper for a given JSObject
GCObject* g_gc_find_for_object(JSObject* obj);

// Find the GCObject wrapper for a given Scope (stored as internal)
// Scopes are not GC managed - we just return nullptr for now
GCObject* g_gc_find_for_scope(void* scope);

// Access to the list of all GC objects (used by builtins)
std::vector<GCObject*>& g_gc_objects_list();
void g_gc_reset();

} // namespace chinstrap

#endif // CHINSTRAP_JS_GC_HPP