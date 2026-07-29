// =============================================================================
// js_interpreter.cpp - Tree-Walking Interpreter Implementation
// =============================================================================
//
// TEACHING NOTE: Interpreter Implementation
// ===========================================
// This is the heart of the JavaScript engine. It walks the AST and executes
// each node. The main dispatch function is evaluate(), which switches on
// the node type and calls the appropriate handler.
//
// Key concepts implemented here:
//   1. Scope chain: linked list of scopes for variable lookup
//   2. Closures: functions capture their defining scope
//   3. Prototype chain: property lookup walks the prototype chain
//   4. this binding: set by how a function is called
//   5. Control flow: implemented via C++ exceptions
//   6. Error handling: try/catch catches JS exceptions
//
// How V8 differs:
//   1. V8 compiles AST to bytecode (Ignition) instead of tree-walking
//   2. V8 uses a register-based VM, not a stack-based one
//   3. V8 optimizes hot code with TurboFan (JIT to machine code)
//   4. V8 uses inline caches for property access and function calls
//   5. V8 does type feedback: it tracks types seen at each operation
//   6. V8 deoptimizes when assumptions fail
//
// =============================================================================

#include "js_interpreter.hpp"
#include "js_builtins.hpp"
#include <cmath>
#include <set>
#include <algorithm>

namespace chinstrap {

// =============================================================================
// Scope implementation
// =============================================================================

// TEACHING NOTE: Scope Chain Lookup
// =================================
// When we look up a variable, we start at the current (innermost) scope and
// walk outward until we find it. If we reach the global scope without
// finding it, we try the global object. If it is not there either, we throw
// a ReferenceError.
//
// V8 optimizes this by resolving variables at parse time. It knows which
// scope a variable comes from and generates direct access to the right
// slot. No runtime chain walking needed.

JSValue Scope::lookup(const std::string& name) {
    Scope* s = this;
    while (s) {
        auto it = s->variables.find(name);
        if (it != s->variables.end()) return it->second;
        s = s->parent;
    }
    // Not found in scope chain - return undefined marker
    // The caller should check for this and handle the error
    return JSValue::undefined();
}

bool Scope::has(const std::string& name) {
    Scope* s = this;
    while (s) {
        if (s->variables.find(name) != s->variables.end()) return true;
        s = s->parent;
    }
    return false;
}

void Scope::declare(const std::string& name, JSValue value) {
    variables[name] = value;
}

void Scope::assign(const std::string& name, JSValue value) {
    Scope* s = this;
    while (s) {
        auto it = s->variables.find(name);
        if (it != s->variables.end()) {
            it->second = value;
            return;
        }
        s = s->parent;
    }
    // Not found - create in global scope (sloppy mode behavior)
    // In strict mode, this would throw a ReferenceError
    // Walk to the outermost scope
    s = this;
    while (s->parent) s = s->parent;
    s->variables[name] = value;
}

// =============================================================================
// Interpreter constructor/destructor
// =============================================================================

Interpreter::Interpreter() {
    // Reset GC objects only (NOT string pools, since the Parser may have
    // already created JSStrings in g_string_pool that are referenced by
    // the AST). The test helper calls g_gc_reset() (which clears both GC
    // objects and string pools) BEFORE creating the Parser. We only need
    // to reset GC objects here, not string pools.
    //
    // We use a private reset that does not clear string pools.
    gc().reset_gc_only();

    // Create global scope
    auto global_scope_uptr = std::make_unique<Scope>();
    global_scope = global_scope_uptr.get();
    scope_pool.push_back(std::move(global_scope_uptr));
    current_scope = global_scope;

    // Create global object
    GCObject* global_gco = gc().allocate_object();
    global_obj = GarbageCollector::get_object(global_gco);
    global_obj->class_name = "Object";
    gc().set_global(global_obj);

    // Disable GC during builtin initialization. Many objects are created
    // but not yet rooted. If GC triggers mid-init, it would free objects
    // that are still being set up, causing use-after-free.
    gc().gc_disabled = true;

    // Initialize built-ins
    init_builtins(*this, global_obj);

    // Re-enable GC and reset the allocation counter
    gc().gc_disabled = false;
    gc().reset_alloc_counter();

    // Store global object in global scope
    global_scope->declare("globalThis", JSValue::object_val(global_obj));
}

Interpreter::~Interpreter() = default;

// =============================================================================
// Scope management
// =============================================================================

Scope* Interpreter::push_scope() {
    auto scope_uptr = std::make_unique<Scope>(current_scope);
    Scope* s = scope_uptr.get();
    scope_pool.push_back(std::move(scope_uptr));
    current_scope = s;
    return s;
}

void Interpreter::pop_scope() {
    if (current_scope && current_scope->parent) {
        current_scope = current_scope->parent;
    }
}

// =============================================================================
// Main dispatch: evaluate
// =============================================================================

JSValue Interpreter::evaluate(AstNode* node) {
    // TEACHING NOTE: AstNode Dispatch
    // =============================
    // This is the main dispatch function. It switches on the node type and
    // calls the appropriate handler. In a bytecode interpreter, this dispatch
    // would be on opcode bytes instead of AST node types. In a JIT, the code
    // would be compiled to machine code and no dispatch is needed at all.

    if (!node) return make_undefined();

    switch (node->node_type) {
        case AstNodeType::Literal:
            return eval_literal(node);
        case AstNodeType::TemplateLiteral:
            return eval_template_literal(node);
        case AstNodeType::Identifier:
            return eval_identifier(node);
        case AstNodeType::BinaryExpression:
            return eval_binary(node);
        case AstNodeType::LogicalExpression:
            return eval_logical(node);
        case AstNodeType::UnaryExpression:
            return eval_unary(node);
        case AstNodeType::UpdateExpression:
            return eval_update(node);
        case AstNodeType::AssignmentExpression:
            return eval_assignment(node);
        case AstNodeType::ConditionalExpression:
            return eval_conditional(node);
        case AstNodeType::CallExpression:
            return eval_call(node);
        case AstNodeType::NewExpression:
            return eval_new(node);
        case AstNodeType::MemberExpression:
        case AstNodeType::OptionalMemberExpression:
            return eval_member(node);
        case AstNodeType::SequenceExpression:
            return eval_sequence(node);
        case AstNodeType::ArrayExpression:
            return eval_array(node);
        case AstNodeType::ObjectExpression:
            return eval_object(node);
        case AstNodeType::ArrowFunctionExpression:
            return eval_arrow_function(node);
        case AstNodeType::FunctionExpression:
            return eval_function_expression(node);
        case AstNodeType::ClassExpression:
            return eval_class_expression(node);
        case AstNodeType::SpreadElement: {
            // Spread in expression context: evaluate the argument
            auto* spread = as<SpreadElement>(node);
            return evaluate(spread->argument.get());
        }
        default:
            // Try executing as a statement
            exec_statement(node);
            return make_undefined();
    }
}

// =============================================================================
// Expression evaluators
// =============================================================================

JSValue Interpreter::eval_literal(AstNode* node) {
    auto* lit = as<Literal>(node);
    return lit->value;
}

JSValue Interpreter::eval_template_literal(AstNode* node) {
    auto* tmpl = as<TemplateLiteral>(node);
    std::string result;
    for (size_t i = 0; i < tmpl->quasis.size(); i++) {
        // Text part
        if (tmpl->quasis[i]) {
            auto* quasi = as<TemplateElement>(tmpl->quasis[i].get());
            result += quasi->cooked;
        }
        // Expression part (not after the last quasi)
        if (i < tmpl->expressions.size()) {
            JSValue expr_val = evaluate(tmpl->expressions[i].get());
            result += expr_val.to_string();
        }
    }
    return make_string(result);
}

JSValue Interpreter::eval_identifier(AstNode* node) {
    auto* id = as<Identifier>(node);

    // Special: this
    if (id->name == "this") {
        // Walk the scope chain to find this_obj
        // The function scope has this_obj set; block scopes inside it do not
        Scope* s = current_scope;
        while (s) {
            if (s->this_obj) {
                return JSValue::object_val(GarbageCollector::get_object(s->this_obj));
            }
            s = s->parent;
        }
        return JSValue::object_val(global_obj);
    }

    // Look up in scope chain
    JSValue val = current_scope->lookup(id->name);
    if (val.is_undefined() && !current_scope->has(id->name)) {
        // Try global object
        if (global_obj->has_property(id->name)) {
            return global_obj->get_property(id->name);
        }
        // Return undefined instead of throwing (lenient behavior)
        return make_undefined();
    }
    return val;
}

JSValue Interpreter::eval_binary(AstNode* node) {
    // TEACHING NOTE: Binary Operators
    // =================================
    // Binary operators evaluate both operands and combine them.
    // Key behaviors:
    //   - + can be string concatenation or numeric addition (based on operand types)
    //   - - * / % are always numeric
    //   - Comparison operators do type coercion (except === and !==)
    //   - Bitwise operators convert to 32-bit integers
    //
    // V8 collects type feedback at each binary operation. If it always sees
    // integers, it generates a fast integer path. If it sees mixed types,
    // it falls back to generic code with full coercion.

    auto* bin = as<BinaryExpression>(node);
    JSValue left = evaluate(bin->left.get());
    JSValue right = evaluate(bin->right.get());

    const std::string& op = bin->op;

    if (op == "+") {
        // + is special: string concatenation or numeric addition
        // If either operand is a string, do string concatenation
        if (left.is_string() || right.is_string()) {
            return make_string(left.to_string() + right.to_string());
        }
        if (left.is_object() || right.is_object()) {
            // Object to primitive conversion
            // Simplified: convert to string if either is string after conversion
            std::string ls = left.to_string();
            std::string rs = right.to_string();
            // Check if original was string - if not, try numeric
            if (left.is_string() || right.is_string()) {
                return make_string(ls + rs);
            }
            return make_number(left.to_number() + right.to_number());
        }
        return make_number(left.to_number() + right.to_number());
    }

    // Arithmetic operators (always numeric)
    if (op == "-") return make_number(left.to_number() - right.to_number());
    if (op == "*") return make_number(left.to_number() * right.to_number());
    if (op == "/") {
        double r = right.to_number();
        if (r == 0.0) {
            double l = left.to_number();
            if (std::isnan(l) || l == 0.0) return make_number(std::numeric_limits<double>::quiet_NaN());
            double sign = (l < 0) ? -1.0 : 1.0;
            return make_number(sign * std::numeric_limits<double>::infinity());
        }
        return make_number(left.to_number() / r);
    }
    if (op == "%") {
        double r = right.to_number();
        if (r == 0.0) return make_number(std::numeric_limits<double>::quiet_NaN());
        return make_number(std::fmod(left.to_number(), r));
    }
    if (op == "**") return make_number(std::pow(left.to_number(), right.to_number()));

    // Comparison operators
    if (op == "<") return JSValue::boolean_val(left.to_number() < right.to_number());
    if (op == ">") return JSValue::boolean_val(left.to_number() > right.to_number());
    if (op == "<=") return JSValue::boolean_val(left.to_number() <= right.to_number());
    if (op == ">=") return JSValue::boolean_val(left.to_number() >= right.to_number());
    if (op == "==") return JSValue::boolean_val(left.loose_equals(right));
    if (op == "!=") return JSValue::boolean_val(!left.loose_equals(right));
    if (op == "===") return JSValue::boolean_val(left.strict_equals(right));
    if (op == "!==") return JSValue::boolean_val(!left.strict_equals(right));

    // Bitwise operators (convert to 32-bit integers)
    auto to_int32 = [](const JSValue& v) -> int32_t {
        double d = v.to_number();
        if (std::isnan(d) || std::isinf(d)) return 0;
        // ToInt32: truncate toward zero, then take mod 2^32, then interpret as signed
        int64_t n = static_cast<int64_t>(d);
        n = n & 0xFFFFFFFF;
        if (n >= 0x80000000) n -= 0x100000000LL;
        return static_cast<int32_t>(n);
    };

    auto to_uint32 = [](const JSValue& v) -> uint32_t {
        double d = v.to_number();
        if (std::isnan(d) || std::isinf(d)) return 0;
        uint64_t n = static_cast<uint64_t>(static_cast<int64_t>(d));
        return static_cast<uint32_t>(n & 0xFFFFFFFF);
    };

    if (op == "&") return make_number(static_cast<double>(to_int32(left) & to_int32(right)));
    if (op == "|") return make_number(static_cast<double>(to_int32(left) | to_int32(right)));
    if (op == "^") return make_number(static_cast<double>(to_int32(left) ^ to_int32(right)));
    if (op == "<<") return make_number(static_cast<double>(to_int32(left) << (to_uint32(right) & 31)));
    if (op == ">>") return make_number(static_cast<double>(to_int32(left) >> (to_uint32(right) & 31)));
    if (op == ">>>") return make_number(static_cast<double>(to_uint32(left) >> (to_uint32(right) & 31)));

    // instanceof
    if (op == "instanceof") {
        // Simplified instanceof
        if (!right.is_function()) return JSValue::boolean_val(false);
        JSObject* ctor = right.as_function();
        if (!left.is_object()) return JSValue::boolean_val(false);
        JSObject* obj = left.as_object();
        // Walk prototype chain
        GCObject* proto = ctor->get_property("prototype").is_object() ?
            g_gc_find_for_object(ctor->get_property("prototype").as_object()) : nullptr;
        while (obj && obj->prototype) {
            if (obj->prototype == proto) return JSValue::boolean_val(true);
            obj = GarbageCollector::get_object(obj->prototype);
        }
        return JSValue::boolean_val(false);
    }

    // in operator
    if (op == "in") {
        if (!right.is_object()) return JSValue::boolean_val(false);
        std::string key = left.to_string();
        return JSValue::boolean_val(right.as_object()->has_property(key));
    }

    return make_undefined();
}

JSValue Interpreter::eval_logical(AstNode* node) {
    // TEACHING NOTE: Short-Circuit Evaluation
    // ========================================
    // Logical operators && and || short-circuit:
    //   - a && b: if a is falsy, return a (do not evaluate b)
    //   - a || b: if a is truthy, return a (do not evaluate b)
    //   - a ?? b: if a is not null/undefined, return a (do not evaluate b)
    //
    // This is important for correctness and performance. V8 generates code
    // that skips the right side based on the left side value.

    auto* log = as<LogicalExpression>(node);
    JSValue left = evaluate(log->left.get());

    if (log->op == "&&") {
        if (!left.to_boolean()) return left;
        return evaluate(log->right.get());
    }
    if (log->op == "||") {
        if (left.to_boolean()) return left;
        return evaluate(log->right.get());
    }
    if (log->op == "??") {
        if (!left.is_nullish()) return left;
        return evaluate(log->right.get());
    }
    return make_undefined();
}

JSValue Interpreter::eval_unary(AstNode* node) {
    auto* unary = as<UnaryExpression>(node);

    // typeof does not evaluate the operand if it is an identifier
    // that does not exist (returns "undefined" instead of throwing)
    if (unary->op == "typeof") {
        if (unary->argument->node_type == AstNodeType::Identifier) {
            auto* id = as<Identifier>(unary->argument.get());
            if (!current_scope->has(id->name) && !global_obj->has_property(id->name)) {
                return make_string("undefined");
            }
        }
        JSValue val = evaluate(unary->argument.get());
        return make_string(val.type_of());
    }

    // void evaluates the operand but returns undefined
    if (unary->op == "void") {
        evaluate(unary->argument.get());
        return make_undefined();
    }

    // delete removes a property
    if (unary->op == "delete") {
        if (unary->argument->node_type == AstNodeType::MemberExpression) {
            auto* member = as<MemberExpression>(unary->argument.get());
            JSValue obj = evaluate(member->object.get());
            if (!obj.is_object()) return JSValue::boolean_val(true);
            std::string key;
            if (member->computed) {
                JSValue key_val = evaluate(member->property.get());
                key = key_val.to_string();
            } else {
                auto* id = as<Identifier>(member->property.get());
                key = id->name;
            }
            obj.as_object()->delete_property(key);
            return JSValue::boolean_val(true);
        }
        evaluate(unary->argument.get());
        return JSValue::boolean_val(true);
    }

    JSValue val = evaluate(unary->argument.get());

    if (unary->op == "-") return make_number(-val.to_number());
    if (unary->op == "+") return make_number(val.to_number());
    if (unary->op == "!") return JSValue::boolean_val(!val.to_boolean());
    if (unary->op == "~") {
        double d = val.to_number();
        if (std::isnan(d) || std::isinf(d)) return make_number(-1);
        int32_t n = static_cast<int32_t>(static_cast<int64_t>(d) & 0xFFFFFFFF);
        return make_number(static_cast<double>(~n));
    }

    return make_undefined();
}

JSValue Interpreter::eval_update(AstNode* node) {
    auto* update = as<UpdateExpression>(node);

    // Get the current value
    JSValue old_val = evaluate(update->argument.get());
    double old_num = old_val.to_number();
    double new_num = (update->op == "++") ? old_num + 1 : old_num - 1;
    JSValue new_val = make_number(new_num);

    // Assign the new value
    if (update->argument->node_type == AstNodeType::Identifier) {
        auto* id = as<Identifier>(update->argument.get());
        current_scope->assign(id->name, new_val);
    } else if (update->argument->node_type == AstNodeType::MemberExpression) {
        auto* member = as<MemberExpression>(update->argument.get());
        JSValue obj = evaluate(member->object.get());
        std::string key;
        if (member->computed) {
            JSValue key_val = evaluate(member->property.get());
            key = key_val.to_string();
        } else {
            auto* id = as<Identifier>(member->property.get());
            key = id->name;
        }
        set_property(obj, key, new_val);
    }

    // Prefix returns the new value, postfix returns the old value
    return update->prefix ? new_val : make_number(old_num);
}

JSValue Interpreter::eval_assignment(AstNode* node) {
    // TEACHING NOTE: Assignment
    // ==========================
    // Assignment evaluates the right side, then assigns to the left side.
    // The left side can be:
    //   - Identifier: variable assignment
    //   - MemberExpression: property assignment
    //   - Pattern: destructuring assignment
    //
    // Compound assignments (+=, -=, etc.) read the current value, apply the
    // operation, then assign the result.

    auto* assign = as<AssignmentExpression>(node);
    JSValue right = evaluate(assign->right.get());

    // Compound assignments
    if (assign->op != "=") {
        JSValue left = evaluate(assign->left.get());
        // Extract the operator from compound assignment (e.g., "+=" -> "+")
        std::string base_op = assign->op.substr(0, assign->op.length() - 1);

        // Perform the operation
        if (base_op == "+") {
            if (left.is_string() || right.is_string()) {
                right = make_string(left.to_string() + right.to_string());
            } else {
                right = make_number(left.to_number() + right.to_number());
            }
        } else if (base_op == "-") right = make_number(left.to_number() - right.to_number());
        else if (base_op == "*") right = make_number(left.to_number() * right.to_number());
        else if (base_op == "/") right = make_number(left.to_number() / right.to_number());
        else if (base_op == "%") right = make_number(std::fmod(left.to_number(), right.to_number()));
        else if (base_op == "**") right = make_number(std::pow(left.to_number(), right.to_number()));
        else if (base_op == "&") {
            int32_t a = static_cast<int32_t>(left.to_number());
            int32_t b = static_cast<int32_t>(right.to_number());
            right = make_number(static_cast<double>(a & b));
        } else if (base_op == "|") {
            int32_t a = static_cast<int32_t>(left.to_number());
            int32_t b = static_cast<int32_t>(right.to_number());
            right = make_number(static_cast<double>(a | b));
        } else if (base_op == "^") {
            int32_t a = static_cast<int32_t>(left.to_number());
            int32_t b = static_cast<int32_t>(right.to_number());
            right = make_number(static_cast<double>(a ^ b));
        } else if (base_op == "<<") {
            int32_t a = static_cast<int32_t>(left.to_number());
            uint32_t b = static_cast<uint32_t>(right.to_number()) & 31;
            right = make_number(static_cast<double>(a << b));
        } else if (base_op == ">>") {
            int32_t a = static_cast<int32_t>(left.to_number());
            uint32_t b = static_cast<uint32_t>(right.to_number()) & 31;
            right = make_number(static_cast<double>(a >> b));
        } else if (base_op == ">>>") {
            uint32_t a = static_cast<uint32_t>(left.to_number());
            uint32_t b = static_cast<uint32_t>(right.to_number()) & 31;
            right = make_number(static_cast<double>(a >> b));
        } else if (base_op == "&&") {
            if (!left.to_boolean()) right = left;
        } else if (base_op == "||") {
            if (left.to_boolean()) right = left;
        } else if (base_op == "??") {
            if (!left.is_nullish()) right = left;
        }
    }

    // Assign to the left side
    if (assign->left->node_type == AstNodeType::Identifier) {
        auto* id = as<Identifier>(assign->left.get());
        current_scope->assign(id->name, right);
    } else if (assign->left->node_type == AstNodeType::MemberExpression) {
        auto* member = as<MemberExpression>(assign->left.get());
        JSValue obj = evaluate(member->object.get());
        std::string key;
        if (member->computed) {
            JSValue key_val = evaluate(member->property.get());
            key = key_val.to_string();
        } else {
            auto* id = as<Identifier>(member->property.get());
            key = id->name;
        }
        set_property(obj, key, right);
    } else if (assign->left->node_type == AstNodeType::ArrayPattern ||
               assign->left->node_type == AstNodeType::ObjectPattern) {
        bind_pattern(assign->left.get(), right, current_scope);
    }

    return right;
}

JSValue Interpreter::eval_conditional(AstNode* node) {
    auto* cond = as<ConditionalExpression>(node);
    JSValue test = evaluate(cond->test.get());
    if (test.to_boolean()) {
        return evaluate(cond->consequent.get());
    }
    return cond->alternate ? evaluate(cond->alternate.get()) : make_undefined();
}

JSValue Interpreter::eval_call(AstNode* node) {
    // TEACHING NOTE: Function Calls
    // ===============================
    // Function calls are the most complex operation in JavaScript. The key
    // is determining the "this" binding:
    //   - Direct call: f() - this is undefined or global
    //   - Method call: obj.f() - this is obj
    //   - Constructor: new F() - this is a new object
    //
    // We also handle:
    //   - Spread in arguments: f(...args)
    //   - Optional chaining: f?.()
    //   - eval (special handling needed)

    auto* call = as<CallExpression>(node);

    JSValue this_val = make_undefined();
    JSObject* fn = nullptr;

    if (call->callee->node_type == AstNodeType::MemberExpression) {
        // Method call: obj.method()
        auto* member = as<MemberExpression>(call->callee.get());
        JSValue obj = evaluate(member->object.get());

        if (member->optional && obj.is_nullish()) {
            return make_undefined();
        }

        std::string key;
        if (member->computed) {
            JSValue key_val = evaluate(member->property.get());
            key = key_val.to_string();
        } else {
            auto* id = as<Identifier>(member->property.get());
            key = id->name;
        }

        JSValue method = get_property(obj, key);
        if (!method.is_function()) {
            // Check if it is optional chaining
            if (member->optional && method.is_undefined()) {
                return make_undefined();
            }
            throw JSThrowSignal(make_string("TypeError: " + key + " is not a function"));
        }
        fn = method.as_function();
        this_val = obj;
    } else {
        // Direct call: f()
        JSValue callee = evaluate(call->callee.get());
        if (!callee.is_function()) {
            throw JSThrowSignal(make_string("TypeError: " + callee.to_string() + " is not a function"));
        }
        fn = callee.as_function();
    }

    // Evaluate arguments
    std::vector<JSValue> args;
    for (auto& arg_node : call->arguments) {
        if (arg_node->node_type == AstNodeType::SpreadElement) {
            auto* spread = as<SpreadElement>(arg_node.get());
            JSValue spread_val = evaluate(spread->argument.get());
            if (spread_val.is_array()) {
                JSObject* arr = spread_val.as_object();
                double len = arr->get_property("length").to_number();
                for (double d = 0; d < len; d++) {
                    args.push_back(arr->get_property(std::to_string(static_cast<long long>(d))));
                }
            } else if (spread_val.is_string()) {
                std::string s = spread_val.as_string()->value;
                for (char c : s) {
                    args.push_back(make_string(std::string(1, c)));
                }
            }
        } else {
            args.push_back(evaluate(arg_node.get()));
        }
    }

    return call_function(fn, this_val, args);
}

JSValue Interpreter::eval_new(AstNode* node) {
    // TEACHING NOTE: Constructor Calls
    // =================================
    // new F() does:
    //   1. Creates a new object
    //   2. Sets the object prototype to F.prototype
    //   3. Calls F with this = new object
    //   4. If F returns an object, use that; otherwise use the new object

    auto* new_expr = as<NewExpression>(node);
    JSValue callee = evaluate(new_expr->callee.get());
    if (!callee.is_function()) {
        throw JSThrowSignal(make_string("TypeError: " + callee.to_string() + " is not a constructor"));
    }
    JSObject* ctor = callee.as_function();

    // Create new object
    GCObject* new_obj_gc = gc().allocate_object();
    JSObject* new_obj = GarbageCollector::get_object(new_obj_gc);

    // Set prototype from constructor prototype property
    JSValue proto = get_property(callee, "prototype");
    if (proto.is_object()) {
        new_obj->prototype = g_gc_find_for_object(proto.as_object());
    }

    // Evaluate arguments
    std::vector<JSValue> args;
    for (auto& arg_node : new_expr->arguments) {
        if (arg_node->node_type == AstNodeType::SpreadElement) {
            auto* spread = as<SpreadElement>(arg_node.get());
            JSValue spread_val = evaluate(spread->argument.get());
            if (spread_val.is_array()) {
                JSObject* arr = spread_val.as_object();
                double len = arr->get_property("length").to_number();
                for (double d = 0; d < len; d++) {
                    args.push_back(arr->get_property(std::to_string(static_cast<long long>(d))));
                }
            }
        } else {
            args.push_back(evaluate(arg_node.get()));
        }
    }

    // Call the constructor
    JSValue result = call_function(ctor, JSValue::object_val(new_obj), args);

    // If constructor returns an object, use that
    if (result.is_object()) return result;

    // Otherwise return the new object
    return JSValue::object_val(new_obj);
}

JSValue Interpreter::eval_member(AstNode* node) {
    // TEACHING NOTE: Property Access
    // ===============================
    // Property access: obj.prop or obj[expr]
    // Optional chaining: obj?.prop (returns undefined if obj is null/undefined)
    //
    // Property lookup walks the prototype chain:
    //   1. Check the object own properties
    //   2. Check the prototype
    //   3. Check the prototype prototype, etc.
    //   4. Return undefined if not found

    auto* member = as<MemberExpression>(node);
    JSValue obj = evaluate(member->object.get());

    if (member->optional && obj.is_nullish()) {
        return make_undefined();
    }

    std::string key;
    if (member->computed) {
        JSValue key_val = evaluate(member->property.get());
        key = key_val.to_string();
    } else {
        auto* id = as<Identifier>(member->property.get());
        key = id->name;
    }

    return get_property(obj, key);
}

JSValue Interpreter::eval_sequence(AstNode* node) {
    auto* seq = as<SequenceExpression>(node);
    JSValue result;
    for (auto& expr : seq->expressions) {
        result = evaluate(expr.get());
    }
    return result;
}

JSValue Interpreter::eval_array(AstNode* node) {
    auto* arr_expr = as<ArrayExpression>(node);
    GCObject* gco = gc().allocate_object();
    JSObject* arr = GarbageCollector::get_object(gco);
    arr->is_array = true;
    // Set Array.prototype as the prototype so methods like push/pop/map work
    if (array_proto) {
        arr->prototype = gc().find_gc_for(array_proto);
    }

    size_t idx = 0;
    for (auto& elem : arr_expr->elements) {
        if (!elem) {
            // Hole
            idx++;
            continue;
        }
        if (elem->node_type == AstNodeType::SpreadElement) {
            auto* spread = as<SpreadElement>(elem.get());
            JSValue spread_val = evaluate(spread->argument.get());
            if (spread_val.is_array()) {
                JSObject* src = spread_val.as_object();
                double len = src->get_property("length").to_number();
                for (double d = 0; d < len; d++) {
                    arr->set_property(std::to_string(idx), src->get_property(std::to_string(static_cast<long long>(d))));
                    idx++;
                }
            }
        } else {
            arr->set_property(std::to_string(idx), evaluate(elem.get()));
            idx++;
        }
    }
    arr->set_property("length", make_number(static_cast<double>(idx)));
    return JSValue::array_val(arr);
}

JSValue Interpreter::eval_object(AstNode* node) {
    auto* obj_expr = as<ObjectExpression>(node);
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);

    for (auto& prop : obj_expr->properties) {
        // Compute key
        std::string key;
        if (prop->computed) {
            JSValue key_val = evaluate(prop->key.get());
            key = key_val.to_string();
        } else {
            if (prop->key->node_type == AstNodeType::Identifier) {
                auto* id = as<Identifier>(prop->key.get());
                key = id->name;
            } else if (prop->key->node_type == AstNodeType::Literal) {
                auto* lit = as<Literal>(prop->key.get());
                key = lit->value.to_string();
            }
        }

        // Check for spread
        if (key == "__spread__" && prop->value && prop->value->node_type == AstNodeType::SpreadElement) {
            auto* spread = as<SpreadElement>(prop->value.get());
            JSValue spread_val = evaluate(spread->argument.get());
            if (spread_val.is_object()) {
                JSObject* src = spread_val.as_object();
                for (auto& src_key : src->enumerable_keys()) {
                    obj->set_property(src_key, src->get_property(src_key));
                }
            }
            continue;
        }

        // Compute value
        JSValue val = evaluate(prop->value.get());
        obj->set_property(key, val);
    }

    return JSValue::object_val(obj);
}

JSValue Interpreter::eval_arrow_function(AstNode* node) {
    auto* arrow = as<ArrowFunctionExpression>(node);
    return JSValue::function_val(make_function_from_arrow(arrow, current_scope));
}

JSValue Interpreter::eval_function_expression(AstNode* node) {
    auto* fn = as<FunctionExpression>(node);
    return JSValue::function_val(make_function_from_expr(fn, current_scope));
}

JSValue Interpreter::eval_class_expression(AstNode* node) {
    // TEACHING NOTE: Class Evaluation
    // =================================
    // Classes are evaluated by:
    //   1. Creating a constructor function
    //   2. Creating a prototype object
    //   3. Adding methods to the prototype
    //   4. Setting up inheritance with extends

    auto* cls = as<ClassExpression>(node);

    // Create the constructor function
    GCObject* ctor_gco = gc().allocate_object();
    JSObject* ctor = GarbageCollector::get_object(ctor_gco);
    ctor->is_function = true;
    ctor->is_constructor = true;
    ctor->class_name = "Function";

    // Create prototype object
    GCObject* proto_gco = gc().allocate_object();
    JSObject* proto = GarbageCollector::get_object(proto_gco);
    ctor->set_property("prototype", JSValue::object_val(proto));
    proto->set_property("constructor", JSValue::function_val(ctor));

    // Set up inheritance
    if (cls->superclass) {
        JSValue super_val = evaluate(cls->superclass.get());
        if (super_val.is_function()) {
            JSObject* super_ctor = super_val.as_function();
            JSValue super_proto = get_property(super_val, "prototype");
            if (super_proto.is_object()) {
                proto->prototype = g_gc_find_for_object(super_proto.as_object());
            }
            ctor->prototype = g_gc_find_for_object(super_ctor);
        }
    } else {
        proto->prototype = g_gc_find_for_object(global_obj->get_property("Object").as_function()->get_property("prototype").as_object());
    }

    // Process class body
    if (cls->body && cls->body->node_type == AstNodeType::ClassBody) {
        auto* body = as<ClassBody>(cls->body.get());
        for (auto& method : body->body) {
            // Get method key
            std::string key;
            if (method->computed) {
                JSValue key_val = evaluate(method->key.get());
                key = key_val.to_string();
            } else {
                if (method->key->node_type == AstNodeType::Identifier) {
                    auto* id = as<Identifier>(method->key.get());
                    key = id->name;
                } else if (method->key->node_type == AstNodeType::Literal) {
                    auto* lit = as<Literal>(method->key.get());
                    key = lit->value.to_string();
                }
            }

            // Create function from method
            if (method->value && method->value->node_type == AstNodeType::FunctionExpression) {
                auto* fn = as<FunctionExpression>(method->value.get());
                JSValue fn_val = JSValue::function_val(make_function_from_expr(fn, current_scope));

                if (method->kind == "constructor") {
                    // Store constructor
                    ctor->native_fn = nullptr;
                    ctor->is_native = false;
                    ctor->ast_body = fn->body.get();
                    ctor->params = std::vector<std::string>();
                    for (auto& p : fn->params) {
                        if (p->node_type == AstNodeType::Identifier) {
                            ctor->params.push_back(as<Identifier>(p.get())->name);
                        }
                    }
                    ctor->closure_scope = g_gc_find_for_object(nullptr);
                    // Actually, store the AST info
                } else {
                    // Add method to prototype or constructor
                    if (method->static_) {
                        ctor->set_property(key, fn_val);
                    } else {
                        proto->set_property(key, fn_val);
                    }
                }
            }
        }
    }

    return JSValue::function_val(ctor);
}

// =============================================================================
// call_function - the core function invocation
// =============================================================================

JSValue Interpreter::call_function(JSObject* fn, JSValue this_val, std::vector<JSValue>& args) {
    // TEACHING NOTE: Function Invocation
    // ====================================
    // Calling a function involves:
    //   1. Creating a new scope (function scope)
    //   2. Binding parameters to arguments
    //   3. Binding "this" to the provided this value
    //   4. Binding "arguments" (for regular functions, not arrows)
    //   5. Executing the function body
    //   6. Returning the return value (or undefined if no return)
    //
    // Arrow functions differ: they do not have their own "this" or "arguments".
    // They use the "this" from the scope where they were defined.
    //
    // V8 compiles functions to bytecode once, then executes the bytecode
    // each time the function is called. This is much faster than re-walking
    // the AST every time.

    if (!fn) return make_undefined();

    // Native function: call directly
    if (fn->is_native) {
        return fn->native_fn(this, this_val, args);
    }

    // TEACHING NOTE: Closure Scope Setup
    // ====================================
    // When calling a function, we create a new scope. The parent of this
    // scope should be the function defining scope (closure scope), not
    // the current calling scope. This is how closures work: the function
    // remembers the variables that were in scope when it was defined.
    //
    // V8 implements this via "context objects" that chain through the
    // outer context. We use a simpler Scope* pointer chain.
    Scope* defining_scope = nullptr;
    if (fn->defining_scope) {
        defining_scope = static_cast<Scope*>(fn->defining_scope);
    }

    // Create function scope with closure scope as parent
    auto scope_uptr = std::make_unique<Scope>(defining_scope);
    Scope* func_scope = scope_uptr.get();
    scope_pool.push_back(std::move(scope_uptr));
    current_scope = func_scope;
    current_scope->is_function_scope = true;

    // Bind "this"
    if (!fn->is_arrow && this_val.is_object()) {
        GCObject* this_gco = g_gc_find_for_object(this_val.as_object());
        if (this_gco) {
            current_scope->this_obj = this_gco;
        } else {
            // Object not found in GC - use global as fallback
            current_scope->this_obj = g_gc_find_for_object(global_obj);
        }
    } else if (!fn->is_arrow) {
        // this is primitive or undefined - use global in non-strict mode
        current_scope->this_obj = g_gc_find_for_object(global_obj);
    }

    // Bind parameters
    size_t num_params = fn->params.size();
    for (size_t i = 0; i < num_params; i++) {
        if (i < args.size()) {
            current_scope->declare(fn->params[i], args[i]);
        } else {
            current_scope->declare(fn->params[i], make_undefined());
        }
    }

    // Bind "arguments" object (for regular functions, not arrows)
    if (!fn->is_arrow) {
        GCObject* args_gco = gc().allocate_object();
        JSObject* args_obj = GarbageCollector::get_object(args_gco);
        args_obj->is_array = true;
        for (size_t i = 0; i < args.size(); i++) {
            args_obj->set_property(std::to_string(i), args[i]);
        }
        args_obj->set_property("length", make_number(static_cast<double>(args.size())));
        current_scope->declare("arguments", JSValue::array_val(args_obj));
    }

    // Execute function body
    JSValue result = make_undefined();
    try {
        if (fn->ast_body) {
            // TEACHING NOTE: Arrow Function Expression Bodies
            // =============================================
            // Arrow functions can have either a block body { return expr; }
            // or an expression body (expr). For block bodies, we execute
            // the block and catch ReturnSignal. For expression bodies, we
            // evaluate the expression and use it as the return value.
            // We store a flag in an internal slot to distinguish them.
            auto expr_body_slot = fn->internal_slots.find("__expression_body__");
            bool is_expr_body = false;
            if (expr_body_slot != fn->internal_slots.end()) {
                is_expr_body = expr_body_slot->second.as_boolean();
            }
            if (is_expr_body) {
                // Expression body: evaluate and return directly
                result = evaluate(static_cast<AstNode*>(fn->ast_body));
            } else {
                auto* body = static_cast<BlockStatement*>(fn->ast_body);
                exec_block(body);
            }
        }
    } catch (ReturnSignal& ret) {
        result = ret.value;
    }

    pop_scope();
    return result;
}

// =============================================================================
// Statement executors
// =============================================================================

void Interpreter::exec_statement(AstNode* node) {
    if (!node) return;

    switch (node->node_type) {
        case AstNodeType::ExpressionStatement: {
            auto* stmt = as<ExpressionStatement>(node);
            evaluate(stmt->expression.get());
            break;
        }
        case AstNodeType::BlockStatement:
            exec_block(node);
            break;
        case AstNodeType::EmptyStatement:
            break;
        case AstNodeType::IfStatement:
            exec_if(node);
            break;
        case AstNodeType::WhileStatement:
            exec_while(node);
            break;
        case AstNodeType::DoWhileStatement:
            exec_do_while(node);
            break;
        case AstNodeType::ForStatement:
            exec_for(node);
            break;
        case AstNodeType::ForInStatement:
            exec_for_in(node);
            break;
        case AstNodeType::ForOfStatement:
            exec_for_of(node);
            break;
        case AstNodeType::ReturnStatement:
            exec_return(node);
            break;
        case AstNodeType::BreakStatement:
            exec_break(node);
            break;
        case AstNodeType::ContinueStatement:
            exec_continue(node);
            break;
        case AstNodeType::ThrowStatement:
            exec_throw(node);
            break;
        case AstNodeType::TryStatement:
            exec_try(node);
            break;
        case AstNodeType::SwitchStatement:
            exec_switch(node);
            break;
        case AstNodeType::VariableDeclaration:
            exec_variable_declaration(node);
            break;
        case AstNodeType::FunctionDeclaration:
            exec_function_declaration(node);
            break;
        case AstNodeType::ClassDeclaration:
            exec_class_declaration(node);
            break;
        case AstNodeType::LabeledStatement:
            exec_labeled(node);
            break;
        default:
            // Try as expression
            evaluate(node);
            break;
    }
}

void Interpreter::exec_block(AstNode* node) {
    auto* block = as<BlockStatement>(node);
    push_scope();
    for (auto& stmt : block->body) {
        exec_statement(stmt.get());
    }
    pop_scope();
}

void Interpreter::exec_if(AstNode* node) {
    auto* if_stmt = as<IfStatement>(node);
    JSValue test = evaluate(if_stmt->test.get());
    if (test.to_boolean()) {
        exec_statement(if_stmt->consequent.get());
    } else if (if_stmt->alternate) {
        exec_statement(if_stmt->alternate.get());
    }
}

void Interpreter::exec_while(AstNode* node) {
    auto* while_stmt = as<WhileStatement>(node);
    while (evaluate(while_stmt->test.get()).to_boolean()) {
        try {
            exec_statement(while_stmt->body.get());
        } catch (BreakSignal&) {
            break;
        } catch (ContinueSignal&) {
            continue;
        }
    }
}

void Interpreter::exec_do_while(AstNode* node) {
    auto* do_while = as<DoWhileStatement>(node);
    do {
        try {
            exec_statement(do_while->body.get());
        } catch (BreakSignal&) {
            break;
        } catch (ContinueSignal&) {
            // Continue to condition check
        }
    } while (evaluate(do_while->test.get()).to_boolean());
}

void Interpreter::exec_for(AstNode* node) {
    auto* for_stmt = as<ForStatement>(node);
    push_scope();

    if (for_stmt->init) {
        exec_statement(for_stmt->init.get());
    }

    while (!for_stmt->test || evaluate(for_stmt->test.get()).to_boolean()) {
        try {
            exec_statement(for_stmt->body.get());
        } catch (BreakSignal&) {
            goto for_end;
        } catch (ContinueSignal&) {
            // Fall through to update
        }
        if (for_stmt->update) {
            evaluate(for_stmt->update.get());
        }
    }

    for_end:
    pop_scope();
}

void Interpreter::exec_for_in(AstNode* node) {
    auto* for_in = as<ForInStatement>(node);
    JSValue obj = evaluate(for_in->right.get());

    if (!obj.is_object() && !obj.is_string()) return;

    std::vector<std::string> keys;
    if (obj.is_object()) {
        keys = obj.as_object()->enumerable_keys();
    } else if (obj.is_string()) {
        std::string s = obj.as_string()->value;
        for (size_t i = 0; i < s.length(); i++) {
            keys.push_back(std::to_string(i));
        }
    }

    for (auto& key : keys) {
        // Bind key to left side
        if (for_in->left->node_type == AstNodeType::VariableDeclaration) {
            auto* var_decl = as<VariableDeclaration>(for_in->left.get());
            if (!var_decl->declarations.empty()) {
                auto& declarator = var_decl->declarations[0];
                if (declarator->id->node_type == AstNodeType::Identifier) {
                    auto* id = as<Identifier>(declarator->id.get());
                    current_scope->declare(id->name, make_string(key));
                }
            }
        } else if (for_in->left->node_type == AstNodeType::Identifier) {
            auto* id = as<Identifier>(for_in->left.get());
            current_scope->assign(id->name, make_string(key));
        }

        try {
            exec_statement(for_in->body.get());
        } catch (BreakSignal&) {
            break;
        } catch (ContinueSignal&) {
            continue;
        }
    }
}

void Interpreter::exec_for_of(AstNode* node) {
    auto* for_of = as<ForOfStatement>(node);
    JSValue iterable = evaluate(for_of->right.get());

    if (iterable.is_array()) {
        JSObject* arr = iterable.as_object();
        double len = arr->get_property("length").to_number();
        for (double d = 0; d < len; d++) {
            JSValue val = arr->get_property(std::to_string(static_cast<long long>(d)));

            if (for_of->left->node_type == AstNodeType::VariableDeclaration) {
                auto* var_decl = as<VariableDeclaration>(for_of->left.get());
                if (!var_decl->declarations.empty()) {
                    auto& declarator = var_decl->declarations[0];
                    if (declarator->id->node_type == AstNodeType::Identifier) {
                        auto* id = as<Identifier>(declarator->id.get());
                        current_scope->declare(id->name, val);
                    }
                }
            } else if (for_of->left->node_type == AstNodeType::Identifier) {
                auto* id = as<Identifier>(for_of->left.get());
                current_scope->assign(id->name, val);
            }

            try {
                exec_statement(for_of->body.get());
            } catch (BreakSignal&) {
                break;
            } catch (ContinueSignal&) {
                continue;
            }
        }
    } else if (iterable.is_string()) {
        std::string s = iterable.as_string()->value;
        for (char c : s) {
            JSValue val = make_string(std::string(1, c));

            if (for_of->left->node_type == AstNodeType::VariableDeclaration) {
                auto* var_decl = as<VariableDeclaration>(for_of->left.get());
                if (!var_decl->declarations.empty()) {
                    auto& declarator = var_decl->declarations[0];
                    if (declarator->id->node_type == AstNodeType::Identifier) {
                        auto* id = as<Identifier>(declarator->id.get());
                        current_scope->declare(id->name, val);
                    }
                }
            }

            try {
                exec_statement(for_of->body.get());
            } catch (BreakSignal&) {
                break;
            } catch (ContinueSignal&) {
                continue;
            }
        }
    }
}

void Interpreter::exec_return(AstNode* node) {
    auto* ret = as<ReturnStatement>(node);
    JSValue val = ret->argument ? evaluate(ret->argument.get()) : make_undefined();
    throw ReturnSignal(val);
}

void Interpreter::exec_break(AstNode* node) {
    auto* brk = as<BreakStatement>(node);
    throw BreakSignal(brk->label);
}

void Interpreter::exec_continue(AstNode* node) {
    auto* cnt = as<ContinueStatement>(node);
    throw ContinueSignal(cnt->label);
}

void Interpreter::exec_throw(AstNode* node) {
    auto* thr = as<ThrowStatement>(node);
    JSValue val = evaluate(thr->argument.get());
    throw JSThrowSignal(val);
}

void Interpreter::exec_try(AstNode* node) {
    auto* try_stmt = as<TryStatement>(node);

    try {
        exec_statement(try_stmt->block.get());
    } catch (JSThrowSignal& js_throw) {
        if (try_stmt->handler) {
            auto* handler = as<CatchClause>(try_stmt->handler.get());
            push_scope();
            if (handler->param) {
                if (handler->param->node_type == AstNodeType::Identifier) {
                    auto* id = as<Identifier>(handler->param.get());
                    current_scope->declare(id->name, js_throw.value);
                }
            }
            exec_statement(handler->body.get());
            pop_scope();
        } else {
            // Re-throw if no handler
            throw;
        }
    }

    // Finally block always executes
    if (try_stmt->finalizer) {
        exec_statement(try_stmt->finalizer.get());
    }
}

void Interpreter::exec_switch(AstNode* node) {
    auto* sw = as<SwitchStatement>(node);
    JSValue discriminant = evaluate(sw->discriminant.get());

    bool matched = false;
    bool fallthrough = false;

    for (auto& case_node : sw->cases) {
        auto* c = as<SwitchCase>(case_node.get());

        if (!matched && !fallthrough) {
            if (!c->test) {
                // Default case - skip for now, handle later
                continue;
            }
            JSValue test_val = evaluate(c->test.get());
            if (discriminant.strict_equals(test_val)) {
                matched = true;
            }
        }

        if (matched || fallthrough) {
            for (auto& stmt : c->consequent) {
                try {
                    exec_statement(stmt.get());
                } catch (BreakSignal&) {
                    return;
                }
            }
            fallthrough = true;
        }
    }

    // Handle default case if no match
    if (!matched) {
        bool in_default = false;
        for (auto& case_node : sw->cases) {
            auto* c = as<SwitchCase>(case_node.get());
            if (!c->test) {
                in_default = true;
            }
            if (in_default) {
                for (auto& stmt : c->consequent) {
                    try {
                        exec_statement(stmt.get());
                    } catch (BreakSignal&) {
                        return;
                    }
                }
            }
        }
    }
}

void Interpreter::exec_variable_declaration(AstNode* node) {
    auto* decl = as<VariableDeclaration>(node);
    for (auto& declarator : decl->declarations) {
        if (declarator->init) {
            JSValue val = evaluate(declarator->init.get());
            if (declarator->id->node_type == AstNodeType::Identifier) {
                auto* id = as<Identifier>(declarator->id.get());
                current_scope->declare(id->name, val);
            } else {
                // Destructuring
                bind_pattern(declarator->id.get(), val, current_scope);
            }
        } else if (declarator->id->node_type == AstNodeType::Identifier) {
            auto* id = as<Identifier>(declarator->id.get());
            current_scope->declare(id->name, make_undefined());
        }
    }
}

void Interpreter::exec_function_declaration(AstNode* node) {
    auto* fn_decl = as<FunctionDeclaration>(node);
    JSObject* fn_obj = make_function_from_ast(fn_decl, current_scope);
    current_scope->declare(fn_decl->name, JSValue::function_val(fn_obj));
}

void Interpreter::exec_class_declaration(AstNode* node) {
    auto* cls_decl = as<ClassDeclaration>(node);
    // Reuse class expression logic
    auto cls_expr = std::make_unique<ClassExpression>();
    cls_expr->superclass = std::move(cls_decl->superclass);
    cls_expr->body = std::move(cls_decl->body);
    JSValue cls_val = eval_class_expression(cls_expr.get());
    if (cls_val.is_function()) {
        current_scope->declare(cls_decl->name, cls_val);
    }
}

void Interpreter::exec_labeled(AstNode* node) {
    auto* labeled = as<LabeledStatement>(node);
    try {
        exec_statement(labeled->body.get());
    } catch (BreakSignal& bs) {
        if (bs.label == labeled->label) return;
        throw;
    }
}

// =============================================================================
// Property access helpers
// =============================================================================

JSValue Interpreter::get_property(JSValue obj, const std::string& name) {
    // TEACHING NOTE: Property Lookup with Prototype Chain
    // ===================================================
    // Property lookup walks the prototype chain:
    //   1. Check the object own properties
    //   2. Check the prototype (and its prototype, etc.)
    //   3. Return undefined if not found
    //
    // This is how JavaScript inheritance works. Methods are defined on
    // the prototype, and all instances share the same prototype methods.
    //
    // V8 optimizes this with inline caches (ICs). At each property access
    // site, V8 caches the "shape" (hidden class) and offset of the property.
    // If the same shape shows up again, V8 skips the lookup.

    // String property access
    if (obj.is_string()) {
        if (name == "length") {
            return make_number(static_cast<double>(obj.as_string()->value.length()));
        }
        // String prototype methods
        JSValue str_proto = global_obj->get_property("String");
        if (str_proto.is_function()) {
            JSValue proto = str_proto.as_function()->get_property("prototype");
            if (proto.is_object()) {
                JSValue method = proto.as_object()->get_property(name);
                if (method.is_function()) return method;
            }
        }
    }

    // Number property access
    if (obj.is_number()) {
        JSValue num_proto = global_obj->get_property("Number");
        if (num_proto.is_function()) {
            JSValue proto = num_proto.as_function()->get_property("prototype");
            if (proto.is_object()) {
                JSValue method = proto.as_object()->get_property(name);
                if (method.is_function()) return method;
            }
        }
    }

    if (!obj.is_object()) return make_undefined();

    JSObject* o = obj.as_object();

    // Check own properties
    auto it = o->property_index.find(name);
    if (it != o->property_index.end()) {
        return o->properties[it->second].second.value;
    }

    // Walk prototype chain
    GCObject* proto = o->prototype;
    while (proto) {
        JSObject* po = GarbageCollector::get_object(proto);
        auto pit = po->property_index.find(name);
        if (pit != po->property_index.end()) {
            return po->properties[pit->second].second.value;
        }
        proto = po->prototype;
    }

    // Try global object properties (for things like console, Math)
    // This is not correct JS semantics but helps with common patterns
    return make_undefined();
}

JSValue Interpreter::get_property(JSValue obj, JSValue key) {
    return get_property(obj, key.to_string());
}

void Interpreter::set_property(JSValue obj, const std::string& name, JSValue value) {
    if (!obj.is_object()) return;
    obj.as_object()->set_property(name, value);
}

void Interpreter::set_property(JSValue obj, JSValue key, JSValue value) {
    if (!obj.is_object()) return;
    obj.as_object()->set_property(key.to_string(), value);
}

// =============================================================================
// Destructuring
// =============================================================================

void Interpreter::bind_pattern(AstNode* pattern, JSValue value, Scope* scope) {
    // TEACHING NOTE: Destructuring Binding
    // ====================================
    // Destructuring extracts values from arrays or objects and binds them
    // to variables. We handle both array and object patterns.

    if (pattern->node_type == AstNodeType::Identifier) {
        auto* id = as<Identifier>(pattern);
        scope->declare(id->name, value);
        return;
    }

    if (pattern->node_type == AstNodeType::ArrayPattern) {
        auto* arr_pat = as<ArrayPattern>(pattern);
        if (!value.is_object()) return;

        JSObject* arr = value.as_object();
        double len = arr->get_property("length").to_number();

        for (size_t i = 0; i < arr_pat->elements.size(); i++) {
            if (!arr_pat->elements[i]) continue; // hole
            JSValue elem = arr->get_property(std::to_string(i));
            bind_pattern(arr_pat->elements[i].get(), elem, scope);
        }

        if (arr_pat->rest) {
            auto* rest = as<RestElement>(arr_pat->rest.get());
            GCObject* rest_gco = gc().allocate_object();
            JSObject* rest_arr = GarbageCollector::get_object(rest_gco);
            rest_arr->is_array = true;
            for (double d = static_cast<double>(arr_pat->elements.size()); d < len; d++) {
                rest_arr->set_property(std::to_string(d - arr_pat->elements.size()),
                    arr->get_property(std::to_string(static_cast<long long>(d))));
            }
            rest_arr->set_property("length", make_number(len - arr_pat->elements.size()));
            bind_pattern(rest->argument.get(), JSValue::array_val(rest_arr), scope);
        }
        return;
    }

    if (pattern->node_type == AstNodeType::ObjectPattern) {
        auto* obj_pat = as<ObjectPattern>(pattern);
        if (!value.is_object()) return;

        JSObject* obj = value.as_object();

        for (auto& prop : obj_pat->properties) {
            std::string key;
            if (prop->key->node_type == AstNodeType::Identifier) {
                auto* id = as<Identifier>(prop->key.get());
                key = id->name;
            } else if (prop->key->node_type == AstNodeType::Literal) {
                auto* lit = as<Literal>(prop->key.get());
                key = lit->value.to_string();
            }

            JSValue val = obj->get_property(key);
            if (val.is_undefined() && prop->value && prop->value->node_type == AstNodeType::AssignmentPattern) {
                auto* assign = as<AssignmentPattern>(prop->value.get());
                val = evaluate(assign->right.get());
                bind_pattern(assign->left.get(), val, scope);
            } else {
                bind_pattern(prop->value.get(), val, scope);
            }
        }

        if (obj_pat->rest) {
            auto* rest = as<RestElement>(obj_pat->rest.get());
            GCObject* rest_gco = gc().allocate_object();
            JSObject* rest_obj = GarbageCollector::get_object(rest_gco);
            // Copy remaining properties
            std::set<std::string> bound_keys;
            for (auto& p : obj_pat->properties) {
                if (p->key->node_type == AstNodeType::Identifier) {
                    bound_keys.insert(as<Identifier>(p->key.get())->name);
                }
            }
            for (auto& key : obj->enumerable_keys()) {
                if (bound_keys.find(key) == bound_keys.end()) {
                    rest_obj->set_property(key, obj->get_property(key));
                }
            }
            bind_pattern(rest->argument.get(), JSValue::object_val(rest_obj), scope);
        }
        return;
    }

    if (pattern->node_type == AstNodeType::AssignmentPattern) {
        auto* assign = as<AssignmentPattern>(pattern);
        if (value.is_undefined()) {
            value = evaluate(assign->right.get());
        }
        bind_pattern(assign->left.get(), value, scope);
        return;
    }
}

// =============================================================================
// Function object creation from AST
// =============================================================================

JSObject* Interpreter::make_function_from_ast(FunctionDeclaration* fn, Scope* scope) {
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    obj->is_function = true;
    obj->is_constructor = true;
    obj->class_name = "Function";
    obj->ast_body = fn->body.get();
    obj->is_async = fn->is_async;
    obj->is_generator = fn->is_generator;
    obj->is_arrow = false;

    for (auto& p : fn->params) {
        if (p->node_type == AstNodeType::Identifier) {
            obj->params.push_back(as<Identifier>(p.get())->name);
        }
    }

    // Capture closure scope
    obj->defining_scope = scope;

    return obj;
}

JSObject* Interpreter::make_function_from_expr(FunctionExpression* fn, Scope* scope) {
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    obj->is_function = true;
    obj->is_constructor = true;
    obj->class_name = "Function";
    obj->ast_body = fn->body.get();
    obj->is_async = fn->is_async;
    obj->is_generator = fn->is_generator;
    obj->is_arrow = false;

    for (auto& p : fn->params) {
        if (p->node_type == AstNodeType::Identifier) {
            obj->params.push_back(as<Identifier>(p.get())->name);
        }
    }

    obj->defining_scope = scope;

    return obj;
}

JSObject* Interpreter::make_function_from_arrow(ArrowFunctionExpression* fn, Scope* scope) {
    GCObject* gco = gc().allocate_object();
    JSObject* obj = GarbageCollector::get_object(gco);
    obj->is_function = true;
    obj->is_arrow = true;
    obj->is_async = fn->is_async;
    obj->class_name = "Function";

    for (auto& p : fn->params) {
        if (p->node_type == AstNodeType::Identifier) {
            obj->params.push_back(as<Identifier>(p.get())->name);
        }
    }

    if (fn->expression_body) {
        // Expression body: wrap in a block with return
        // We store the expression and return it directly
        obj->ast_body = fn->body.get();
        // Mark as expression body by checking node type in call_function
        // Actually, we need to handle this differently. For now, store the
        // expression body and check in call_function.
    } else {
        obj->ast_body = fn->body.get();
    }

    obj->defining_scope = scope;

    // Store whether it is expression body
    // We use an internal slot
    obj->internal_slots["__expression_body__"] = JSValue::boolean_val(fn->expression_body);
    if (fn->expression_body && fn->body) {
        obj->internal_slots["__body_expr__"] = JSValue::object_val(nullptr); // We cannot easily store this
        // Actually we need to store the AST node pointer
        // For simplicity, we will check the body node type in call_function
    }

    return obj;
}

// =============================================================================
// Run program
// =============================================================================

JSValue Interpreter::run(Program* program) {
    JSValue result = make_undefined();
    for (auto& stmt : program->body) {
        exec_statement(stmt.get());
    }
    return result;
}

} // namespace chinstrap