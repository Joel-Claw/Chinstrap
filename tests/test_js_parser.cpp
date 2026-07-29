// =============================================================================
// tests/test_js_parser.cpp - Unit Tests for JavaScript Parser
// =============================================================================
//
// TEACHING NOTE: Parser Testing
// =============================
// We test the parser by parsing JavaScript snippets and checking the AST
// structure. Good parser tests cover:
//   - Each grammar production (expressions, statements, declarations)
//   - Operator precedence and associativity
//   - Edge cases (empty blocks, nested structures, arrow functions)
//   - Error cases (malformed code should produce errors)
//
// =============================================================================

#include "../src/js_parser.hpp"
#include "../src/js_ast.hpp"
#include "../src/js_lexer.hpp"
#include <cstdio>
#include <string>

using namespace chinstrap;

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s (line %d)\n", #cond, __LINE__); } \
} while(0)

// Helper: parse source code and return the program AST
static std::unique_ptr<Program> parse(const std::string& source) {
    Parser parser(source);
    return parser.parse_program();
}

// ---- Test: simple variable declaration ----
static void test_var_declaration() {
    printf("Test: variable declaration... ");
    auto prog = parse("let x = 42;");
    ASSERT(prog != nullptr);
    ASSERT(prog->body.size() == 1);
    ASSERT(prog->body[0]->node_type == NodeType::VariableDeclaration);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    ASSERT(decl->kind == "let");
    ASSERT(decl->declarations.size() == 1);
    auto* declarator = decl->declarations[0].get();
    ASSERT(declarator->id->node_type == NodeType::Identifier);
    auto* id = static_cast<Identifier*>(declarator->id.get());
    ASSERT(id->name == "x");
    ASSERT(declarator->init != nullptr);
    ASSERT(declarator->init->node_type == NodeType::Literal);
    auto* lit = static_cast<Literal*>(declarator->init.get());
    ASSERT(lit->value.is_number());
    ASSERT(lit->value.as_number() == 42.0);
    printf("OK\n");
}

// ---- Test: function declaration ----
static void test_function_declaration() {
    printf("Test: function declaration... ");
    auto prog = parse("function add(a, b) { return a + b; }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body.size() == 1);
    ASSERT(prog->body[0]->node_type == NodeType::FunctionDeclaration);
    auto* fn = static_cast<FunctionDeclaration*>(prog->body[0].get());
    ASSERT(fn->name == "add");
    ASSERT(fn->params.size() == 2);
    ASSERT(fn->body != nullptr);
    ASSERT(fn->body->node_type == NodeType::BlockStatement);
    printf("OK\n");
}

// ---- Test: if statement ----
static void test_if_statement() {
    printf("Test: if statement... ");
    auto prog = parse("if (x > 0) { console.log(x); } else { console.log(0); }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body.size() == 1);
    ASSERT(prog->body[0]->node_type == NodeType::IfStatement);
    auto* if_stmt = static_cast<IfStatement*>(prog->body[0].get());
    ASSERT(if_stmt->test != nullptr);
    ASSERT(if_stmt->consequent != nullptr);
    ASSERT(if_stmt->alternate != nullptr);
    printf("OK\n");
}

// ---- Test: while loop ----
static void test_while_loop() {
    printf("Test: while loop... ");
    auto prog = parse("while (x < 10) { x++; }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::WhileStatement);
    printf("OK\n");
}

// ---- Test: for loop ----
static void test_for_loop() {
    printf("Test: for loop... ");
    auto prog = parse("for (let i = 0; i < 10; i++) { console.log(i); }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::ForStatement);
    auto* for_stmt = static_cast<ForStatement*>(prog->body[0].get());
    ASSERT(for_stmt->init != nullptr);
    ASSERT(for_stmt->test != nullptr);
    ASSERT(for_stmt->update != nullptr);
    ASSERT(for_stmt->body != nullptr);
    printf("OK\n");
}

// ---- Test: arrow function ----
static void test_arrow_function() {
    printf("Test: arrow function... ");
    auto prog = parse("const add = (a, b) => a + b;");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::VariableDeclaration);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    ASSERT(decl->declarations.size() == 1);
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::ArrowFunctionExpression);
    auto* arrow = static_cast<ArrowFunctionExpression*>(init);
    ASSERT(arrow->params.size() == 2);
    ASSERT(arrow->expression_body == true);
    printf("OK\n");
}

// ---- Test: arrow function with block body ----
static void test_arrow_function_block() {
    printf("Test: arrow function with block body... ");
    auto prog = parse("const f = (x) => { return x * 2; }");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::ArrowFunctionExpression);
    auto* arrow = static_cast<ArrowFunctionExpression*>(init);
    ASSERT(arrow->expression_body == false);
    ASSERT(arrow->body->node_type == NodeType::BlockStatement);
    printf("OK\n");
}

// ---- Test: object literal ----
static void test_object_literal() {
    printf("Test: object literal... ");
    auto prog = parse("let obj = { x: 1, y: 2, method() { return this.x; } };");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::ObjectExpression);
    auto* obj = static_cast<ObjectExpression*>(init);
    ASSERT(obj->properties.size() >= 2);
    printf("OK\n");
}

// ---- Test: array literal ----
static void test_array_literal() {
    printf("Test: array literal... ");
    auto prog = parse("let arr = [1, 2, 3];");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::ArrayExpression);
    auto* arr = static_cast<ArrayExpression*>(init);
    ASSERT(arr->elements.size() == 3);
    printf("OK\n");
}

// ---- Test: class declaration ----
static void test_class_declaration() {
    printf("Test: class declaration... ");
    auto prog = parse("class Point { constructor(x, y) { this.x = x; this.y = y; } }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::ClassDeclaration);
    auto* cls = static_cast<ClassDeclaration*>(prog->body[0].get());
    ASSERT(cls->name == "Point");
    ASSERT(cls->body != nullptr);
    printf("OK\n");
}

// ---- Test: try/catch ----
static void test_try_catch() {
    printf("Test: try/catch... ");
    auto prog = parse("try { throw 42; } catch (e) { console.log(e); } finally { cleanup(); }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::TryStatement);
    auto* try_stmt = static_cast<TryStatement*>(prog->body[0].get());
    ASSERT(try_stmt->block != nullptr);
    ASSERT(try_stmt->handler != nullptr);
    ASSERT(try_stmt->finalizer != nullptr);
    printf("OK\n");
}

// ---- Test: binary operator precedence ----
static void test_operator_precedence() {
    printf("Test: operator precedence... ");
    auto prog = parse("let x = 1 + 2 * 3;");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::BinaryExpression);
    auto* bin = static_cast<BinaryExpression*>(init);
    ASSERT(bin->op == "+");
    // The right side should be 2 * 3
    ASSERT(bin->right->node_type == NodeType::BinaryExpression);
    auto* right = static_cast<BinaryExpression*>(bin->right.get());
    ASSERT(right->op == "*");
    printf("OK\n");
}

// ---- Test: template literal ----
static void test_template_literal() {
    printf("Test: template literal... ");
    auto prog = parse("let x = `hello ${name} world`;");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::TemplateLiteral);
    printf("OK\n");
}

// ---- Test: destructuring ----
static void test_destructuring() {
    printf("Test: destructuring... ");
    auto prog = parse("let [a, b] = [1, 2];");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    ASSERT(decl->declarations[0]->id->node_type == NodeType::ArrayPattern);
    printf("OK\n");
}

// ---- Test: spread ----
static void test_spread() {
    printf("Test: spread... ");
    auto prog = parse("let arr = [...other, 1, 2];");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::ArrayExpression);
    auto* arr = static_cast<ArrayExpression*>(init);
    ASSERT(arr->elements[0]->node_type == NodeType::SpreadElement);
    printf("OK\n");
}

// ---- Test: optional chaining ----
static void test_optional_chaining() {
    printf("Test: optional chaining... ");
    auto prog = parse("let x = obj?.prop;");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::MemberExpression);
    auto* member = static_cast<MemberExpression*>(init);
    ASSERT(member->optional == true);
    printf("OK\n");
}

// ---- Test: nullish coalescing ----
static void test_nullish_coalescing() {
    printf("Test: nullish coalescing... ");
    auto prog = parse("let x = a ?? b;");
    ASSERT(prog != nullptr);
    auto* decl = static_cast<VariableDeclaration*>(prog->body[0].get());
    auto* init = decl->declarations[0]->init.get();
    ASSERT(init->node_type == NodeType::LogicalExpression);
    auto* log = static_cast<LogicalExpression*>(init);
    ASSERT(log->op == "??");
    printf("OK\n");
}

// ---- Test: for-of ----
static void test_for_of() {
    printf("Test: for-of... ");
    auto prog = parse("for (let x of arr) { console.log(x); }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::ForOfStatement);
    printf("OK\n");
}

// ---- Test: for-in ----
static void test_for_in() {
    printf("Test: for-in... ");
    auto prog = parse("for (let key in obj) { console.log(key); }");
    ASSERT(prog != nullptr);
    ASSERT(prog->body[0]->node_type == NodeType::ForInStatement);
    printf("OK\n");
}

// ---- Test: multiple statements ----
static void test_multiple_statements() {
    printf("Test: multiple statements... ");
    auto prog = parse("let a = 1; let b = 2; let c = a + b;");
    ASSERT(prog != nullptr);
    ASSERT(prog->body.size() == 3);
    printf("OK\n");
}

// ---- Test: no errors on valid code ----
static void test_no_errors() {
    printf("Test: no parse errors on valid code... ");
    auto prog = parse("let x = 1; function f() { return x; }");
    ASSERT(prog != nullptr);
    Parser parser("let x = 1; function f() { return x; }");
    parser.parse_program();
    ASSERT(!parser.has_errors());
    printf("OK\n");
}

// ---- Main ----
int main() {
    printf("\n=== JavaScript Parser Tests ===\n\n");

    test_var_declaration();
    test_function_declaration();
    test_if_statement();
    test_while_loop();
    test_for_loop();
    test_arrow_function();
    test_arrow_function_block();
    test_object_literal();
    test_array_literal();
    test_class_declaration();
    test_try_catch();
    test_operator_precedence();
    test_template_literal();
    test_destructuring();
    test_spread();
    test_optional_chaining();
    test_nullish_coalescing();
    test_for_of();
    test_for_in();
    test_multiple_statements();
    test_no_errors();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}