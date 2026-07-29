// =============================================================================
// tests/test_js_interpreter.cpp - Unit Tests for JavaScript Interpreter
// =============================================================================
//
// TEACHING NOTE: Interpreter Testing
// ===================================
// We test the interpreter by running actual JavaScript code and checking
// the results. Good interpreter tests cover:
//   - Arithmetic and string operations
//   - Variable scoping (var, let, const, closures)
//   - Control flow (if, while, for, switch)
//   - Functions and arrow functions
//   - Objects and arrays
//   - Prototype chain
//   - Error handling (try/catch)
//   - Built-in functions (Math, Array methods, String methods)
//
// We capture console.log output to verify results.
//
// =============================================================================

#include "../src/js_interpreter.hpp"
#include "../src/js_parser.hpp"
#include "../src/js_lexer.hpp"
#include "../src/js_value.hpp"
#include "../src/js_gc.hpp"
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
#include <unistd.h>

using namespace chinstrap;

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s (line %d)\n", #cond, __LINE__); } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s == %s (line %d): got %s, expected %s\n", #a, #b, __LINE__, str_a.c_str(), str_b.c_str()); } \
} while(0)

// Helper: run JS code and return the last console.log output
static std::string last_output;

static std::string run_js(const std::string& code) {
    // Reset GC to avoid stale objects from previous interpreter
    g_gc_reset();

    // Redirect stdout to capture console.log output
    // We use a pipe for this
    int old_stdout = dup(fileno(stdout));
    
    // Create a temporary file to capture output
    FILE* tmp = tmpfile();
    if (!tmp) return "";
    
    fflush(stdout);
    dup2(fileno(tmp), fileno(stdout));
    
    // Run the code
    Parser parser(code);
    auto program = parser.parse_program();
    
    if (parser.has_errors()) {
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        return "PARSE_ERROR: " + parser.error_string();
    }
    
    Interpreter interp;
    interp.run(program.get());
    
    // Restore stdout
    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);
    
    // Read captured output
    rewind(tmp);
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), tmp)) {
        result += buffer;
    }
    fclose(tmp);
    
    // Remove trailing newline
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    last_output = result;
    return result;
}

// ---- Test: simple arithmetic ----
static void test_arithmetic() {
    printf("Test: simple arithmetic... ");
    std::string result = run_js("console.log(2 + 3);");
    ASSERT(result == "5");
    printf("OK\n");
}

// ---- Test: string concatenation ----
static void test_string_concat() {
    printf("Test: string concatenation... ");
    std::string result = run_js("console.log(\"hello\" + \" \" + \"world\");");
    ASSERT(result == "hello world");
    printf("OK\n");
}

// ---- Test: variable declaration ----
static void test_variables() {
    printf("Test: variable declaration... ");
    std::string result = run_js("let x = 10; console.log(x);");
    ASSERT(result == "10");
    printf("OK\n");
}

// ---- Test: const ----
static void test_const() {
    printf("Test: const... ");
    std::string result = run_js("const PI = 3.14; console.log(PI);");
    ASSERT(result == "3.14");
    printf("OK\n");
}

// ---- Test: function declaration ----
static void test_function() {
    printf("Test: function declaration... ");
    std::string result = run_js(
        "function add(a, b) { return a + b; } "
        "console.log(add(3, 4));"
    );
    ASSERT(result == "7");
    printf("OK\n");
}

// ---- Test: arrow function ----
static void test_arrow_function() {
    printf("Test: arrow function... ");
    std::string result = run_js(
        "const square = (x) => x * x; "
        "console.log(square(5));"
    );
    ASSERT(result == "25");
    printf("OK\n");
}

// ---- Test: closure ----
static void test_closure() {
    printf("Test: closure... ");
    std::string result = run_js(
        "function makeCounter() { let count = 0; return function() { count++; return count; }; } "
        "let c = makeCounter(); "
        "c(); c(); "
        "console.log(c());"
    );
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: if statement ----
static void test_if() {
    printf("Test: if statement... ");
    std::string result = run_js(
        "let x = 5; "
        "if (x > 3) { console.log(\"big\"); } else { console.log(\"small\"); }"
    );
    ASSERT(result == "big");
    printf("OK\n");
}

// ---- Test: while loop ----
static void test_while() {
    printf("Test: while loop... ");
    std::string result = run_js(
        "let i = 0; let sum = 0; "
        "while (i < 5) { sum += i; i++; } "
        "console.log(sum);"
    );
    ASSERT(result == "10");
    printf("OK\n");
}

// ---- Test: for loop ----
static void test_for() {
    printf("Test: for loop... ");
    std::string result = run_js(
        "let sum = 0; "
        "for (let i = 1; i <= 10; i++) { sum += i; } "
        "console.log(sum);"
    );
    ASSERT(result == "55");
    printf("OK\n");
}

// ---- Test: array creation ----
static void test_array() {
    printf("Test: array creation... ");
    std::string result = run_js("let arr = [1, 2, 3]; console.log(arr.length);");
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: array push ----
static void test_array_push() {
    printf("Test: array push... ");
    std::string result = run_js(
        "let arr = [1, 2]; arr.push(3); console.log(arr.length);"
    );
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: array map ----
static void test_array_map() {
    printf("Test: array map... ");
    std::string result = run_js(
        "let arr = [1, 2, 3]; "
        "let doubled = arr.map(function(x) { return x * 2; }); "
        "console.log(doubled[0] + \" \" + doubled[1] + \" \" + doubled[2]);"
    );
    ASSERT(result == "2 4 6");
    printf("OK\n");
}

// ---- Test: array filter ----
static void test_array_filter() {
    printf("Test: array filter... ");
    std::string result = run_js(
        "let arr = [1, 2, 3, 4, 5]; "
        "let evens = arr.filter(function(x) { return x % 2 === 0; }); "
        "console.log(evens.length);"
    );
    ASSERT(result == "2");
    printf("OK\n");
}

// ---- Test: array reduce ----
static void test_array_reduce() {
    printf("Test: array reduce... ");
    std::string result = run_js(
        "let arr = [1, 2, 3, 4, 5]; "
        "let sum = arr.reduce(function(a, b) { return a + b; }, 0); "
        "console.log(sum);"
    );
    ASSERT(result == "15");
    printf("OK\n");
}

// ---- Test: array forEach ----
static void test_array_foreach() {
    printf("Test: array forEach... ");
    std::string result = run_js(
        "let sum = 0; "
        "[1, 2, 3].forEach(function(x) { sum += x; }); "
        "console.log(sum);"
    );
    ASSERT(result == "6");
    printf("OK\n");
}

// ---- Test: array includes ----
static void test_array_includes() {
    printf("Test: array includes... ");
    std::string result = run_js(
        "let arr = [1, 2, 3]; console.log(arr.includes(2));"
    );
    ASSERT(result == "true");
    printf("OK\n");
}

// ---- Test: string methods ----
static void test_string_methods() {
    printf("Test: string methods... ");
    std::string result = run_js(
        "let s = \"Hello World\"; "
        "console.log(s.toUpperCase());"
    );
    ASSERT(result == "HELLO WORLD");
    printf("OK\n");
}

// ---- Test: string split ----
static void test_string_split() {
    printf("Test: string split... ");
    std::string result = run_js(
        "let parts = \"a,b,c\".split(\",\"); "
        "console.log(parts.length);"
    );
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: object property access ----
static void test_object_access() {
    printf("Test: object property access... ");
    std::string result = run_js(
        "let obj = { x: 1, y: 2 }; "
        "console.log(obj.x + obj.y);"
    );
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: object method ----
static void test_object_method() {
    printf("Test: object method... ");
    std::string result = run_js(
        "let obj = { val: 42, getVal() { return this.val; } }; "
        "console.log(obj.getVal());"
    );
    ASSERT(result == "42");
    printf("OK\n");
}

// ---- Test: Math functions ----
static void test_math() {
    printf("Test: Math functions... ");
    std::string result = run_js(
        "console.log(Math.max(1, 5, 3));"
    );
    ASSERT(result == "5");
    printf("OK\n");
}

// ---- Test: Math.floor ----
static void test_math_floor() {
    printf("Test: Math.floor... ");
    std::string result = run_js("console.log(Math.floor(3.7));");
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: JSON.stringify ----
static void test_json_stringify() {
    printf("Test: JSON.stringify... ");
    std::string result = run_js("console.log(JSON.stringify({x: 1}));");
    ASSERT(result == "{\"x\":1}");
    printf("OK\n");
}

// ---- Test: try/catch ----
static void test_try_catch() {
    printf("Test: try/catch... ");
    std::string result = run_js(
        "try { throw \"error\"; } catch (e) { console.log(e); }"
    );
    ASSERT(result == "error");
    printf("OK\n");
}

// ---- Test: typeof ----
static void test_typeof() {
    printf("Test: typeof... ");
    std::string result = run_js(
        "console.log(typeof 42 + \" \" + typeof \"hello\" + \" \" + typeof true);"
    );
    ASSERT(result == "number string boolean");
    printf("OK\n");
}

// ---- Test: comparison ----
static void test_comparison() {
    printf("Test: comparison... ");
    std::string result = run_js("console.log(1 === 1);");
    ASSERT(result == "true");
    printf("OK\n");
}

// ---- Test: ternary ----
static void test_ternary() {
    printf("Test: ternary... ");
    std::string result = run_js("console.log(5 > 3 ? \"yes\" : \"no\");");
    ASSERT(result == "yes");
    printf("OK\n");
}

// ---- Test: for-of loop ----
static void test_for_of() {
    printf("Test: for-of loop... ");
    std::string result = run_js(
        "let sum = 0; "
        "for (let x of [1, 2, 3]) { sum += x; } "
        "console.log(sum);"
    );
    ASSERT(result == "6");
    printf("OK\n");
}

// ---- Test: spread in array ----
static void test_spread() {
    printf("Test: spread in array... ");
    std::string result = run_js(
        "let a = [1, 2]; let b = [...a, 3]; console.log(b.length);"
    );
    ASSERT(result == "3");
    printf("OK\n");
}

// ---- Test: nullish coalescing ----
static void test_nullish() {
    printf("Test: nullish coalescing... ");
    std::string result = run_js("console.log(null ?? \"default\");");
    ASSERT(result == "default");
    printf("OK\n");
}

// ---- Test: logical operators ----
static void test_logical() {
    printf("Test: logical operators... ");
    std::string result = run_js("console.log(true && false);");
    ASSERT(result == "false");
    printf("OK\n");
}

// ---- Test: template literal ----
static void test_template_literal() {
    printf("Test: template literal... ");
    std::string result = run_js(
        "let name = \"world\"; console.log(`hello ${name}`);"
    );
    ASSERT(result == "hello world");
    printf("OK\n");
}

// ---- Test: parseInt ----
static void test_parseInt() {
    printf("Test: parseInt... ");
    std::string result = run_js("console.log(parseInt(\"42\"));");
    ASSERT(result == "42");
    printf("OK\n");
}

// ---- Test: nested functions ----
static void test_nested_functions() {
    printf("Test: nested functions... ");
    std::string result = run_js(
        "function outer() { let x = 10; function inner() { return x * 2; } return inner(); } "
        "console.log(outer());"
    );
    ASSERT(result == "20");
    printf("OK\n");
}

// ---- Test: array join ----
static void test_array_join() {
    printf("Test: array join... ");
    std::string result = run_js(
        "console.log([1, 2, 3].join(\",\"));"
    );
    ASSERT(result == "1,2,3");
    printf("OK\n");
}

// ---- Main ----
int main() {
    printf("\n=== JavaScript Interpreter Tests ===\n\n");

    test_arithmetic();
    test_string_concat();
    test_variables();
    test_const();
    test_function();
    test_arrow_function();
    test_closure();
    test_if();
    test_while();
    test_for();
    test_array();
    test_array_push();
    test_array_map();
    test_array_filter();
    test_array_reduce();
    test_array_foreach();
    test_array_includes();
    test_string_methods();
    test_string_split();
    test_object_access();
    test_object_method();
    test_math();
    test_math_floor();
    test_json_stringify();
    test_try_catch();
    test_typeof();
    test_comparison();
    test_ternary();
    test_for_of();
    test_spread();
    test_nullish();
    test_logical();
    test_template_literal();
    test_parseInt();
    test_nested_functions();
    test_array_join();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}