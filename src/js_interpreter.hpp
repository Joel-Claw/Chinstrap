// =============================================================================
// js_interpreter.hpp - Tree-Walking JavaScript Interpreter
// =============================================================================
//
// TEACHING NOTE: Tree-Walking Interpreters
// =========================================
//
// An interpreter executes code directly without compiling it to bytecode or
// machine code. A tree-walking interpreter traverses the AST and evaluates
// each node. This is the simplest way to execute JavaScript.
//
// How it works:
//   1. Parse source code into an AST (done by the parser)
//   2. Walk the AST recursively, evaluating each node
//   3. Maintain a scope chain (environment) for variable lookup
//   4. Handle control flow with exceptions (return, break, continue, throw)
//
// How V8 does it differently:
// ===========================
// V8 does NOT use a tree-walking interpreter. Instead:
//   1. V8 compiles the AST to bytecode for the Ignition interpreter
//   2. Ignition is a register-based bytecode interpreter (much faster)
//   3. Hot code is optimized by TurboFan (JIT compiler) to machine code
//   4. If assumptions fail, V8 deoptimizes back to bytecode
//
// Why tree-walking is slow:
//   - Each node visit involves virtual dispatch or large switch statements
//   - No register allocation - values are stored in heap objects
//   - No inlining, no constant folding, no type feedback
//   - AST traversal has poor cache locality
//
// But tree-walking is simple and correct, which is what we need.
//
// =============================================================================

#ifndef CHINSTRAP_JS_INTERPRETER_HPP
#define CHINSTRAP_JS_INTERPRETER_HPP

#include "js_value.hpp"
#include "js_gc.hpp"
#include "js_ast.hpp"

namespace chinstrap {

// =============================================================================
// Scope (Environment)
// =============================================================================

// TEACHING NOTE: Lexical Scoping
// ===============================
// JavaScript uses lexical scoping: variables are visible in the scope where
// they are declared, and in all inner scopes. The scope chain is a linked
// list of scope objects, from innermost to outermost.
//
// Each scope has:
//   - A map of variable name to value
//   - A pointer to the parent scope (null for global scope)
//
// Variable lookup walks the scope chain until the name is found.
// Variable assignment walks the scope chain to find the variable, then sets it.
// Variable declaration creates a new binding in the current scope.
//
// Closures: a function captures its defining scope. When the function is
// called, a new scope is created with the captured scope as parent. This
// allows inner functions to access outer variables even after the outer
// function returns.
//
// V8 represents scopes in the scope chain as "Context" objects. V8 optimizes
// by storing local variables in registers (stack slots) when possible, and
// only creating Context objects for variables that escape (are captured by
// closures). This is called "scope analysis" and is done at compile time.

struct Scope {
    std::unordered_map<std::string, JSValue> variables;
    Scope* parent = nullptr;
    GCObject* this_obj = nullptr;
    bool is_function_scope = false;
    std::string function_name;

    Scope() = default;
    explicit Scope(Scope* p) : parent(p) {}

    JSValue lookup(const std::string& name);
    bool has(const std::string& name);
    void declare(const std::string& name, JSValue value);
    void assign(const std::string& name, JSValue value);
};

// =============================================================================
// Control Flow Exceptions
// =============================================================================

// TEACHING NOTE: Control Flow via Exceptions
// ==========================================
// In a tree-walking interpreter, control flow (return, break, continue, throw)
// is tricky because we are in a recursive call chain. We use C++ exceptions to
// implement these:
//
//   - ReturnException: carries the return value
//   - BreakException: carries an optional label
//   - ContinueException: carries an optional label
//   - JSException: carries a thrown JS value (from throw statements)
//
// This is simple but has performance overhead (exception handling is slow).
// In a bytecode interpreter, control flow is handled with jumps, which are
// much faster. V8 uses bytecode jumps for return/break/continue and only
// uses C++ exceptions for JS throw statements.

struct ReturnSignal {
    JSValue value;
    explicit ReturnSignal(JSValue v) : value(std::move(v)) {}
};

struct BreakSignal {
    std::string label;
    explicit BreakSignal(std::string l = "") : label(std::move(l)) {}
};

struct ContinueSignal {
    std::string label;
    explicit ContinueSignal(std::string l = "") : label(std::move(l)) {}
};

struct JSThrowSignal {
    JSValue value;
    explicit JSThrowSignal(JSValue v) : value(std::move(v)) {}
};

// =============================================================================
// Interpreter
// =============================================================================

class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    // Run a parsed program
    JSValue run(Program* program);

    // Evaluate a single node (recursive dispatch)
    JSValue evaluate(AstNode* node);

    // Call a function with given this and arguments
    JSValue call_function(JSObject* fn, JSValue this_val, std::vector<JSValue>& args);

    // Get the global object
    JSObject* global() { return global_obj; }

    // Get the GC
    GarbageCollector& gc() { return chinstrap::gc(); }

    // Prototype accessors (set by init_builtins)
    JSObject* array_proto = nullptr;
    JSObject* string_proto = nullptr;
    JSObject* object_proto = nullptr;
    JSObject* function_proto = nullptr;
    JSObject* number_proto = nullptr;
    JSObject* boolean_proto = nullptr;

private:
    JSObject* global_obj;
    Scope* global_scope;

    // Scope management
    std::vector<std::unique_ptr<Scope>> scope_pool;
    Scope* current_scope;

    Scope* push_scope();
    void pop_scope();

    // Evaluation helpers for specific node types
    JSValue eval_literal(AstNode* node);
    JSValue eval_template_literal(AstNode* node);
    JSValue eval_identifier(AstNode* node);
    JSValue eval_binary(AstNode* node);
    JSValue eval_logical(AstNode* node);
    JSValue eval_unary(AstNode* node);
    JSValue eval_update(AstNode* node);
    JSValue eval_assignment(AstNode* node);
    JSValue eval_conditional(AstNode* node);
    JSValue eval_call(AstNode* node);
    JSValue eval_new(AstNode* node);
    JSValue eval_member(AstNode* node);
    JSValue eval_sequence(AstNode* node);
    JSValue eval_array(AstNode* node);
    JSValue eval_object(AstNode* node);
    JSValue eval_arrow_function(AstNode* node);
    JSValue eval_function_expression(AstNode* node);
    JSValue eval_class_expression(AstNode* node);

    // Statement evaluation
    void exec_statement(AstNode* node);
    void exec_block(AstNode* node);
    void exec_if(AstNode* node);
    void exec_while(AstNode* node);
    void exec_do_while(AstNode* node);
    void exec_for(AstNode* node);
    void exec_for_in(AstNode* node);
    void exec_for_of(AstNode* node);
    void exec_return(AstNode* node);
    void exec_break(AstNode* node);
    void exec_continue(AstNode* node);
    void exec_throw(AstNode* node);
    void exec_try(AstNode* node);
    void exec_switch(AstNode* node);
    void exec_variable_declaration(AstNode* node);
    void exec_function_declaration(AstNode* node);
    void exec_class_declaration(AstNode* node);
    void exec_labeled(AstNode* node);

    // Helpers
    JSValue get_property(JSValue obj, const std::string& name);
    JSValue get_property(JSValue obj, JSValue key);
    void set_property(JSValue obj, const std::string& name, JSValue value);
    void set_property(JSValue obj, JSValue key, JSValue value);
    JSValue call_member(AstNode* callee, JSValue this_val, std::vector<JSValue>& args);

    // Destructuring
    void bind_pattern(AstNode* pattern, JSValue value, Scope* scope);

    // Convert AST to function object
    JSObject* make_function_from_ast(FunctionDeclaration* fn, Scope* scope);
    JSObject* make_function_from_expr(FunctionExpression* fn, Scope* scope);
    JSObject* make_function_from_arrow(ArrowFunctionExpression* fn, Scope* scope);

    // Helper to cast nodes
    template<typename T>
    T* as(AstNode* node) { return static_cast<T*>(node); }
};

} // namespace chinstrap

#endif // CHINSTRAP_JS_INTERPRETER_HPP