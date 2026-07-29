// =============================================================================
// js_ast.hpp - Abstract Syntax Tree AstNode Definitions
// =============================================================================
//
// TEACHING NOTE: What is an AST?
// ==============================
//
// An Abstract Syntax Tree (AST) is a tree representation of source code.
// Each node in the tree represents a construct in the language. The AST
// abstracts away syntactic details like whitespace and comments - it
// captures the structure and meaning of the code.
//
// Example: "let x = 1 + 2;" becomes:
//   VariableDeclaration
//   - name: "x"
//   - init: BinaryExpression
//     - op: "+"
//     - left: Literal (1)
//     - right: Literal (2)
//
// How V8 builds ASTs:
// ==================
// V8 parser produces an AST that is then compiled to bytecode for the
// Ignition interpreter. The AST is also used for speculative optimizations
// in TurboFan (V8 optimizing JIT). V8 AST nodes include type information
// collected during parsing (e.g., "this literal is always a number") which
// helps the JIT generate faster code.
//
// Our AST uses class inheritance. Each node type is a class derived from
// the base AstNode class. We use a visitor-style approach where the interpreter
// dispatches based on node_type.
//
// AST AstNode Categories:
//   1. Expressions: produce values (literals, binary ops, calls, member access)
//   2. Statements: perform actions (if, while, return, declarations)
//   3. Declarations: bind names (let, const, function, class)
//
// =============================================================================

#ifndef CHINSTRAP_JS_AST_HPP
#define CHINSTRAP_JS_AST_HPP

#include "js_value.hpp"
#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace chinstrap {

// =============================================================================
// AstNode types enum
// =============================================================================

// TEACHING NOTE: Tagged AstNode Types
// =================================
// Each AST node has a type tag. The interpreter uses this tag to dispatch
// to the correct evaluation function. This is faster than dynamic_cast
// or virtual dispatch in some cases, and makes the code easier to read.
//
// V8 uses a similar approach but with a class hierarchy and virtual methods.
// =============================================================================

enum class AstNodeType {
    // Literals
    Literal,
    TemplateLiteral,
    TemplateElement,
    RegexLiteral,

    // Expressions
    Identifier,
    BinaryExpression,
    LogicalExpression,
    UnaryExpression,
    UpdateExpression,
    AssignmentExpression,
    ConditionalExpression,
    CallExpression,
    NewExpression,
    MemberExpression,
    OptionalMemberExpression,
    SequenceExpression,
    SpreadElement,
    ArrayExpression,
    ObjectExpression,
    Property,
    ArrowFunctionExpression,
    FunctionExpression,
    ClassExpression,
    YieldExpression,
    AwaitExpression,
    TaggedTemplateExpression,

    // Statements
    ExpressionStatement,
    BlockStatement,
    EmptyStatement,
    IfStatement,
    WhileStatement,
    DoWhileStatement,
    ForStatement,
    ForInStatement,
    ForOfStatement,
    ReturnStatement,
    BreakStatement,
    ContinueStatement,
    ThrowStatement,
    TryStatement,
    SwitchStatement,
    SwitchCase,
    LabeledStatement,

    // Declarations
    VariableDeclaration,
    VariableDeclarator,
    FunctionDeclaration,
    ClassDeclaration,
    ClassBody,
    MethodDefinition,

    // Patterns
    ObjectPattern,
    ArrayPattern,
    RestElement,
    AssignmentPattern,

    // Other
    Program,
    CatchClause,
    ImportDeclaration,
    ExportDeclaration,
};

// =============================================================================
// Base AST AstNode
// =============================================================================

// TEACHING NOTE: AST AstNode Design
// ==============================
// Each AST node stores:
//   - node_type: for dispatch
//   - line/col: for error messages and debugging
//   - type-specific fields in subclasses
//
// We use a class hierarchy with a base AstNode and derived classes. This is
// the most natural way to represent different node types in C++.
//
// V8 uses a zone-allocated AST (nodes are allocated in a Zone allocator
// that is bulk-freed after compilation). We use unique_ptr for automatic
// memory management. The performance difference matters for large programs
// but not for our educational engine.
// =============================================================================

struct AstNode {
    AstNodeType node_type;
    int line = 0;
    int col = 0;
    virtual ~AstNode() = default;
    AstNode(AstNodeType t) : node_type(t) {}
};

// Backward-compatible alias. Test code uses NodeType::Xxx which maps to
// AstNodeType::Xxx via this alias. This avoids an ODR violation with
// the unrelated NodeType enum in html_parser.hpp.
using NodeType = AstNodeType;

// ---- Forward declarations of all node types ----

struct Literal;
struct TemplateLiteral;
struct RegexLiteral;
struct Identifier;
struct BinaryExpression;
struct LogicalExpression;
struct UnaryExpression;
struct UpdateExpression;
struct AssignmentExpression;
struct ConditionalExpression;
struct CallExpression;
struct NewExpression;
struct MemberExpression;
struct OptionalMemberExpression;
struct SequenceExpression;
struct SpreadElement;
struct ArrayExpression;
struct ObjectExpression;
struct Property;
struct ArrowFunctionExpression;
struct FunctionExpression;
struct ClassExpression;
struct YieldExpression;
struct AwaitExpression;
struct ExpressionStatement;
struct BlockStatement;
struct IfStatement;
struct WhileStatement;
struct DoWhileStatement;
struct ForStatement;
struct ForInStatement;
struct ForOfStatement;
struct ReturnStatement;
struct BreakStatement;
struct ContinueStatement;
struct ThrowStatement;
struct TryStatement;
struct SwitchStatement;
struct SwitchCase;
struct VariableDeclaration;
struct VariableDeclarator;
struct FunctionDeclaration;
struct ClassDeclaration;
struct ClassBody;
struct MethodDefinition;
struct ObjectPattern;
struct ArrayPattern;
struct RestElement;
struct AssignmentPattern;
struct Program;
struct CatchClause;

// =============================================================================
// Literal values
// =============================================================================

// TEACHING NOTE: Literals
// =======================
// Literals are the leaves of the AST. They represent constant values
// directly in source code: numbers, strings, booleans, null, undefined,
// regex patterns, and template literals.
//
// V8 stores literals as "literal nodes" that the interpreter turns into
// pre-allocated constant values. This avoids creating new objects for
// the same literal on every execution.
// =============================================================================

struct Literal : AstNode {
    // The literal value stored as a JSValue
    JSValue value;

    // For regex: the pattern and flags
    std::string regex_pattern;
    std::string regex_flags;
    bool is_regex = false;

    Literal() : AstNode(AstNodeType::Literal) {}
};

struct TemplateLiteral : AstNode {
    std::vector<std::unique_ptr<AstNode>> quasis;    // string parts
    std::vector<std::unique_ptr<AstNode>> expressions; // interpolated expressions

    TemplateLiteral() : AstNode(AstNodeType::TemplateLiteral) {}
};

struct TemplateElement : AstNode {
    std::string raw;
    std::string cooked;
    bool tail = false;

    TemplateElement() : AstNode(AstNodeType::TemplateElement) {}
};

struct RegexLiteral : AstNode {
    std::string pattern;
    std::string flags;

    RegexLiteral() : AstNode(AstNodeType::RegexLiteral) {}
};

// =============================================================================
// Identifiers
// =============================================================================

// TEACHING NOTE: Identifiers
// ==========================
// Identifiers are variable references. In the AST, an Identifier node
// represents a use of a variable name. The name is resolved to a value
// at runtime by looking it up in the scope chain.
//
// V8 resolves identifiers at parse time when possible. It distinguishes
// between "global variables" (resolved through the global object) and
// "local variables" (resolved through stack slots). This helps the JIT
// generate fast code for variable access.
// =============================================================================

struct Identifier : AstNode {
    std::string name;

    Identifier() : AstNode(AstNodeType::Identifier) {}
    explicit Identifier(std::string n) : AstNode(AstNodeType::Identifier), name(std::move(n)) {}
};

// =============================================================================
// Binary and Logical Expressions
// =============================================================================

// TEACHING NOTE: Binary Expressions
// ==================================
// Binary expressions have a left operand, an operator, and a right operand.
// Examples: a + b, x * y, str === "hello"
//
// Logical expressions are short-circuit: && and || do not evaluate the
// right side if the left side determines the result.
//
// V8 type feedback: V8 tracks the types seen at binary operators. If it
// always sees integers, it generates a fast integer path. If it sees
// mixed types, it falls back to generic code.
// =============================================================================

struct BinaryExpression : AstNode {
    std::string op; // + - * / % ** << >> >>> & | ^ < > <= >= == != === !==
    std::unique_ptr<AstNode> left;
    std::unique_ptr<AstNode> right;

    BinaryExpression() : AstNode(AstNodeType::BinaryExpression) {}
};

struct LogicalExpression : AstNode {
    std::string op; // && || ??
    std::unique_ptr<AstNode> left;
    std::unique_ptr<AstNode> right;

    LogicalExpression() : AstNode(AstNodeType::LogicalExpression) {}
};

// =============================================================================
// Unary Expressions
// =============================================================================

// TEACHING NOTE: Unary Operators
// ================================
// Unary operators take one operand. Prefix: ! - + ~ typeof void delete
// Postfix: ++ -- (also available as prefix)
//
// "typeof" returns a string describing the type.
// "void" returns undefined regardless of operand.
// "delete" removes a property from an object.
// =============================================================================

struct UnaryExpression : AstNode {
    std::string op; // ! - + ~ typeof void delete
    std::unique_ptr<AstNode> argument;
    bool prefix = true;

    UnaryExpression() : AstNode(AstNodeType::UnaryExpression) {}
};

struct UpdateExpression : AstNode {
    std::string op; // ++ --
    std::unique_ptr<AstNode> argument;
    bool prefix = false;

    UpdateExpression() : AstNode(AstNodeType::UpdateExpression) {}
};

// =============================================================================
// Assignment Expressions
// =============================================================================

// TEACHING NOTE: Assignment
// ==========================
// Assignment expressions assign a value to a target. The target can be:
//   - An identifier (simple assignment): x = 5
//   - A member expression (property assignment): obj.prop = 5, arr[0] = 5
//   - A destructuring pattern (destructuring assignment): [a, b] = arr
//
// Compound assignment operators: += -= *= /= %= **= <<= >>= >>>= &= |= ^=
// These are sugar for "x op= y" meaning "x = x op y".
//
// V8 optimizes assignments by tracking which properties are being modified
// and deoptimizing if the object shape changes (e.g., adding a new property
// that changes the hidden class).
// =============================================================================

struct AssignmentExpression : AstNode {
    std::string op; // = += -= *= /= %= **= <<= >>= >>>= &= |= ^=
    std::unique_ptr<AstNode> left;
    std::unique_ptr<AstNode> right;

    AssignmentExpression() : AstNode(AstNodeType::AssignmentExpression) {}
};

// =============================================================================
// Conditional (Ternary) Expression
// =============================================================================

struct ConditionalExpression : AstNode {
    std::unique_ptr<AstNode> test;
    std::unique_ptr<AstNode> consequent;
    std::unique_ptr<AstNode> alternate;

    ConditionalExpression() : AstNode(AstNodeType::ConditionalExpression) {}
};

// =============================================================================
// Call and New Expressions
// =============================================================================

// TEACHING NOTE: Function Calls
// =============================
// Call expressions invoke a function with arguments. The "this" binding
// depends on how the function is called:
//   - Direct call: f() - this is undefined (strict mode) or global (sloppy)
//   - Method call: obj.f() - this is obj
//   - Constructor call: new F() - this is a new object
//   - call/apply/bind: explicit this binding
//
// V8 uses inline caches (ICs) at call sites to optimize dispatch. If the
// same function is always called at a site, V8 patches the call to a
// direct jump. For method calls, V8 caches the lookup result.
// =============================================================================

struct CallExpression : AstNode {
    std::unique_ptr<AstNode> callee;
    std::vector<std::unique_ptr<AstNode>> arguments;
    bool optional = false; // for optional chaining: f?.()

    CallExpression() : AstNode(AstNodeType::CallExpression) {}
};

struct NewExpression : AstNode {
    std::unique_ptr<AstNode> callee;
    std::vector<std::unique_ptr<AstNode>> arguments;

    NewExpression() : AstNode(AstNodeType::NewExpression) {}
};

// =============================================================================
// Member Expressions (property access)
// =============================================================================

// TEACHING NOTE: Member Access
// =============================
// Member expressions access object properties. Two forms:
//   - Computed: obj[expr] - the property name is evaluated at runtime
//   - Non-computed: obj.name - the property name is a literal identifier
//
// Optional chaining (?.) is a modern feature: if the object is null/undefined,
// the expression short-circuits to undefined instead of throwing.
//
// V8 inline caches: at each property access site, V8 caches the "looked up
// property at offset N in hidden class H". If the same hidden class shows
// up again, V8 skips the lookup and goes directly to the offset.
// =============================================================================

struct MemberExpression : AstNode {
    std::unique_ptr<AstNode> object;
    std::unique_ptr<AstNode> property;
    bool computed = false;
    bool optional = false; // for ?. operator

    MemberExpression() : AstNode(AstNodeType::MemberExpression) {}
};

struct OptionalMemberExpression : AstNode {
    std::unique_ptr<AstNode> object;
    std::unique_ptr<AstNode> property;
    bool computed = false;

    OptionalMemberExpression() : AstNode(AstNodeType::OptionalMemberExpression) {}
};

// =============================================================================
// Sequence Expression (comma operator)
// =============================================================================

struct SequenceExpression : AstNode {
    std::vector<std::unique_ptr<AstNode>> expressions;

    SequenceExpression() : AstNode(AstNodeType::SequenceExpression) {}
};

// =============================================================================
// Spread/Rest Element
// =============================================================================

// TEACHING NOTE: Spread and Rest
// ===============================
// Spread (...x) expands an iterable into individual elements:
//   - In array literals: [1, ...arr, 2]
//   - In function calls: f(...args)
//   - In object literals: {...obj} (shallow copy)
//
// Rest parameters (...args) collect multiple arguments into an array:
//   - Function params: function f(...args) {}
//   - Destructuring: const [a, ...rest] = arr;
//
// Both use the same syntax (...) but opposite semantics.
// =============================================================================

struct SpreadElement : AstNode {
    std::unique_ptr<AstNode> argument;

    SpreadElement() : AstNode(AstNodeType::SpreadElement) {}
};

// =============================================================================
// Array and Object Expressions
// =============================================================================

struct ArrayExpression : AstNode {
    std::vector<std::unique_ptr<AstNode>> elements; // can contain nullptr for holes

    ArrayExpression() : AstNode(AstNodeType::ArrayExpression) {}
};

struct Property : AstNode {
    std::unique_ptr<AstNode> key;
    std::unique_ptr<AstNode> value;
    bool computed = false;
    bool shorthand = false;
    bool is_method = false;
    std::string kind; // "init", "get", "set"

    Property() : AstNode(AstNodeType::Property) {}
};

struct ObjectExpression : AstNode {
    std::vector<std::unique_ptr<Property>> properties;

    ObjectExpression() : AstNode(AstNodeType::ObjectExpression) {}
};

// =============================================================================
// Function Expressions
// =============================================================================

// TEACHING NOTE: Function vs Arrow Function
// ==========================================
// Regular functions have their own "this" binding, "arguments" object,
// "new.target", and can be used as constructors.
//
// Arrow functions do NOT have their own "this" - they capture "this" from
// the enclosing scope. They also lack "arguments" and "new.target".
// Arrow functions cannot be used with "new".
//
// This is the most important distinction in modern JavaScript. The "this"
// binding rules are a common source of bugs when mixing regular functions
// and arrow functions.
//
// V8 represents arrow functions differently in bytecode: they access "this"
// from the outer context rather than having it passed as a parameter.
// =============================================================================

struct FunctionParams {
    std::vector<std::unique_ptr<AstNode>> params; // can include RestElement, AssignmentPattern
    std::unique_ptr<AstNode> rest; // rest parameter, if any
};

struct ArrowFunctionExpression : AstNode {
    std::vector<std::unique_ptr<AstNode>> params;
    std::unique_ptr<AstNode> body; // BlockStatement or single expression
    bool expression_body = false; // true if body is a single expression
    bool is_async = false;

    ArrowFunctionExpression() : AstNode(AstNodeType::ArrowFunctionExpression) {}
};

struct FunctionExpression : AstNode {
    std::string name;
    std::vector<std::unique_ptr<AstNode>> params;
    std::unique_ptr<AstNode> body; // BlockStatement
    bool is_async = false;
    bool is_generator = false;

    FunctionExpression() : AstNode(AstNodeType::FunctionExpression) {}
};

struct ClassExpression : AstNode {
    std::unique_ptr<AstNode> superclass; // can be nullptr
    std::unique_ptr<AstNode> body; // ClassBody

    ClassExpression() : AstNode(AstNodeType::ClassExpression) {}
};

// =============================================================================
// Yield and Await
// =============================================================================

struct YieldExpression : AstNode {
    std::unique_ptr<AstNode> argument;
    bool delegate = false; // yield*

    YieldExpression() : AstNode(AstNodeType::YieldExpression) {}
};

struct AwaitExpression : AstNode {
    std::unique_ptr<AstNode> argument;

    AwaitExpression() : AstNode(AstNodeType::AwaitExpression) {}
};

// =============================================================================
// Statements
// =============================================================================

// TEACHING NOTE: Statements vs Expressions
// =========================================
// In JavaScript, some constructs are expressions (produce values) and some
// are statements (perform actions). Some are both:
//   - "x = 5" is both an expression (value 5) and a statement
//   - "if (x) {}" is only a statement
//
// Expression statements wrap an expression in statement position. The
// expression is evaluated and its value is discarded.
// =============================================================================

struct ExpressionStatement : AstNode {
    std::unique_ptr<AstNode> expression;

    ExpressionStatement() : AstNode(AstNodeType::ExpressionStatement) {}
};

// TEACHING NOTE: Block Statement and Scoping
// ===========================================
// A block statement creates a new lexical scope. Variables declared with
// let/const are block-scoped. Variables declared with var are function-scoped
// (hoisted to the function body).
//
// V8 represents scopes in a "scope chain" data structure. Each scope has
// a variable map. Block scopes are pushed/popped during execution.
// =============================================================================

struct BlockStatement : AstNode {
    std::vector<std::unique_ptr<AstNode>> body;

    BlockStatement() : AstNode(AstNodeType::BlockStatement) {}
};

struct EmptyStatement : AstNode {
    EmptyStatement() : AstNode(AstNodeType::EmptyStatement) {}
};

struct IfStatement : AstNode {
    std::unique_ptr<AstNode> test;
    std::unique_ptr<AstNode> consequent;
    std::unique_ptr<AstNode> alternate; // can be nullptr

    IfStatement() : AstNode(AstNodeType::IfStatement) {}
};

struct WhileStatement : AstNode {
    std::unique_ptr<AstNode> test;
    std::unique_ptr<AstNode> body;

    WhileStatement() : AstNode(AstNodeType::WhileStatement) {}
};

struct DoWhileStatement : AstNode {
    std::unique_ptr<AstNode> body;
    std::unique_ptr<AstNode> test;

    DoWhileStatement() : AstNode(AstNodeType::DoWhileStatement) {}
};

// TEACHING NOTE: For Statement Variants
// =======================================
// JavaScript has three for loop variants:
//   1. Classic for: for (init; test; update) {}
//   2. for-in: for (key in object) {} - iterates enumerable keys
//   3. for-of: for (value of iterable) {} - iterates iterable values
//
// The init part of classic for can be a variable declaration or expression.
// =============================================================================

struct ForStatement : AstNode {
    std::unique_ptr<AstNode> init;    // VariableDeclaration or Expression (can be nullptr)
    std::unique_ptr<AstNode> test;    // Expression (can be nullptr)
    std::unique_ptr<AstNode> update;  // Expression (can be nullptr)
    std::unique_ptr<AstNode> body;

    ForStatement() : AstNode(AstNodeType::ForStatement) {}
};

struct ForInStatement : AstNode {
    std::unique_ptr<AstNode> left;     // VariableDeclaration or Identifier
    std::unique_ptr<AstNode> right;   // Expression (object to iterate)
    std::unique_ptr<AstNode> body;

    ForInStatement() : AstNode(AstNodeType::ForInStatement) {}
};

struct ForOfStatement : AstNode {
    std::unique_ptr<AstNode> left;     // VariableDeclaration or Identifier
    std::unique_ptr<AstNode> right;   // Expression (iterable)
    std::unique_ptr<AstNode> body;
    bool is_await = false; // for await...of

    ForOfStatement() : AstNode(AstNodeType::ForOfStatement) {}
};

// TEACHING NOTE: Control Flow
// ============================
// Return, break, and continue are "abrupt completions" in the spec.
// They do not just jump - they carry a value (for return) or a label
// (for break/continue with labeled statements).
//
// We implement these as exceptions in the interpreter. This is simple
// but has performance overhead. V8 uses a different approach: it marks
// these in the AST and the bytecode handles them with jumps.
// =============================================================================

struct ReturnStatement : AstNode {
    std::unique_ptr<AstNode> argument; // can be nullptr (undefined return)

    ReturnStatement() : AstNode(AstNodeType::ReturnStatement) {}
};

struct BreakStatement : AstNode {
    std::string label; // empty if unlabeled

    BreakStatement() : AstNode(AstNodeType::BreakStatement) {}
};

struct ContinueStatement : AstNode {
    std::string label; // empty if unlabeled

    ContinueStatement() : AstNode(AstNodeType::ContinueStatement) {}
};

struct ThrowStatement : AstNode {
    std::unique_ptr<AstNode> argument;

    ThrowStatement() : AstNode(AstNodeType::ThrowStatement) {}
};

// TEACHING NOTE: Try/Catch/Finally
// =================================
// The try/catch/finally statement handles exceptions. The catch clause
// binds the thrown value to a variable. The finally clause always runs,
// even if try or catch returns or throws.
//
// We implement try/catch using C++ exceptions. The interpreter throws
// a JSValue when JS code throws, and catches it in the try block handler.
// =============================================================================

struct TryStatement : AstNode {
    std::unique_ptr<AstNode> block;     // try block
    std::unique_ptr<CatchClause> handler; // catch clause (can be nullptr)
    std::unique_ptr<AstNode> finalizer; // finally block (can be nullptr)

    TryStatement() : AstNode(AstNodeType::TryStatement) {}
};

struct CatchClause : AstNode {
    std::unique_ptr<AstNode> param; // Identifier or Pattern (can be nullptr for optional catch binding)
    std::unique_ptr<AstNode> body;  // BlockStatement

    CatchClause() : AstNode(AstNodeType::CatchClause) {}
};

struct SwitchStatement : AstNode {
    std::unique_ptr<AstNode> discriminant;
    std::vector<std::unique_ptr<SwitchCase>> cases;

    SwitchStatement() : AstNode(AstNodeType::SwitchStatement) {}
};

struct SwitchCase : AstNode {
    std::unique_ptr<AstNode> test;  // null for default case
    std::vector<std::unique_ptr<AstNode>> consequent;

    SwitchCase() : AstNode(AstNodeType::SwitchCase) {}
};

struct LabeledStatement : AstNode {
    std::string label;
    std::unique_ptr<AstNode> body;

    LabeledStatement() : AstNode(AstNodeType::LabeledStatement) {}
};

// =============================================================================
// Declarations
// =============================================================================

// TEACHING NOTE: Variable Declarations
// =====================================
// JavaScript has three variable declaration kinds:
//   - var: function-scoped, hoisted, can be redeclared
//   - let: block-scoped, temporal dead zone (TDZ), cannot be redeclared
//   - const: like let, but cannot be reassigned
//
// "var" hoisting means the declaration is moved to the top of the function,
// but the assignment stays in place. Accessing a var before its assignment
// gives undefined, not a ReferenceError.
//
// "let/const" are in the temporal dead zone before their declaration.
// Accessing them before declaration throws a ReferenceError.
// =============================================================================

struct VariableDeclarator : AstNode {
    std::unique_ptr<AstNode> id;    // Identifier or Pattern
    std::unique_ptr<AstNode> init;  // Expression (can be nullptr)

    VariableDeclarator() : AstNode(AstNodeType::VariableDeclarator) {}
};

struct VariableDeclaration : AstNode {
    std::string kind; // "var", "let", "const"
    std::vector<std::unique_ptr<VariableDeclarator>> declarations;

    VariableDeclaration() : AstNode(AstNodeType::VariableDeclaration) {}
};

struct FunctionDeclaration : AstNode {
    std::string name;
    std::vector<std::unique_ptr<AstNode>> params;
    std::unique_ptr<AstNode> body; // BlockStatement
    bool is_async = false;
    bool is_generator = false;

    FunctionDeclaration() : AstNode(AstNodeType::FunctionDeclaration) {}
};

// =============================================================================
// Class Declarations
// =============================================================================

// TEACHING NOTE: Classes
// =======================
// ES6 classes are syntactic sugar over prototype-based inheritance. A class
// declaration creates a constructor function and puts methods on the
// prototype. "extends" sets up the prototype chain.
//
//   class A extends B { constructor() { super(); } method() {} }
//
// Is equivalent to:
//   function A() { B.call(this); }
//   A.prototype = Object.create(B.prototype);
//   A.prototype.constructor = A;
//   A.prototype.method = function() {};
//
// V8 optimizes classes with hidden classes. If many objects are created
// from the same class, they share the same hidden class and property layout.
// =============================================================================

struct MethodDefinition : AstNode {
    std::unique_ptr<AstNode> key;    // Identifier or computed expression
    std::unique_ptr<AstNode> value;  // FunctionExpression
    bool computed = false;
    bool static_ = false;
    std::string kind; // "constructor", "method", "get", "set"

    MethodDefinition() : AstNode(AstNodeType::MethodDefinition) {}
};

struct ClassBody : AstNode {
    std::vector<std::unique_ptr<MethodDefinition>> body;

    ClassBody() : AstNode(AstNodeType::ClassBody) {}
};

struct ClassDeclaration : AstNode {
    std::string name;
    std::unique_ptr<AstNode> superclass; // can be nullptr
    std::unique_ptr<AstNode> body;       // ClassBody

    ClassDeclaration() : AstNode(AstNodeType::ClassDeclaration) {}
};

// =============================================================================
// Destructuring Patterns
// =============================================================================

// TEACHING NOTE: Destructuring
// ============================
// Destructuring is a pattern that extracts values from arrays or objects.
//
// Array destructuring: const [a, b] = [1, 2];
// Object destructuring: const {x, y} = {x: 1, y: 2};
// Default values: const {x = 10} = {};
// Renaming: const {x: y} = {x: 1}; // y = 1
// Nested: const {a: {b}} = {a: {b: 1}};
// Rest: const [a, ...rest] = [1, 2, 3];
//
// Destructuring works in variable declarations, function parameters, and
// assignment expressions. It is one of the most useful ES6 features.
// =============================================================================

struct ObjectPattern : AstNode {
    std::vector<std::unique_ptr<Property>> properties;
    std::unique_ptr<AstNode> rest; // RestElement for trailing properties

    ObjectPattern() : AstNode(AstNodeType::ObjectPattern) {}
};

struct ArrayPattern : AstNode {
    std::vector<std::unique_ptr<AstNode>> elements; // can contain nullptr for holes
    std::unique_ptr<AstNode> rest; // RestElement for trailing elements

    ArrayPattern() : AstNode(AstNodeType::ArrayPattern) {}
};

struct RestElement : AstNode {
    std::unique_ptr<AstNode> argument; // Identifier or Pattern

    RestElement() : AstNode(AstNodeType::RestElement) {}
};

struct AssignmentPattern : AstNode {
    std::unique_ptr<AstNode> left;     // Identifier or Pattern
    std::unique_ptr<AstNode> right;    // default value expression

    AssignmentPattern() : AstNode(AstNodeType::AssignmentPattern) {}
};

// =============================================================================
// Program (root node)
// =============================================================================

struct Program : AstNode {
    std::vector<std::unique_ptr<AstNode>> body;

    Program() : AstNode(AstNodeType::Program) {}
};

// =============================================================================
// Import/Export (minimal - for parsing only, not full module support)
// =============================================================================

struct ImportDeclaration : AstNode {
    std::vector<std::pair<std::string, std::string>> specifiers; // (imported, local)
    std::string source;

    ImportDeclaration() : AstNode(AstNodeType::ImportDeclaration) {}
};

struct ExportDeclaration : AstNode {
    std::unique_ptr<AstNode> declaration;
    std::vector<std::pair<std::string, std::string>> specifiers;
    std::string source;
    bool is_default = false;

    ExportDeclaration() : AstNode(AstNodeType::ExportDeclaration) {}
};

} // namespace chinstrap

#endif // CHINSTRAP_JS_AST_HPP