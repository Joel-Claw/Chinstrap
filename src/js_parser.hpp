// =============================================================================
// js_parser.hpp - JavaScript Parser (Recursive Descent)
// =============================================================================
//
// TEACHING NOTE: Parsing JavaScript
// ================================
//
// The parser takes a token stream from the lexer and builds an Abstract
// Syntax Tree (AST). We use recursive descent parsing, which is the most
// common approach for hand-written parsers.
//
// Recursive descent parsing:
//   Each grammar rule is a function that calls other rule functions.
//   The parser uses the current token to decide which rule to apply.
//   If the current token does not match, we report a syntax error.
//
// JavaScript grammar is complex. Key challenges:
//   1. Automatic Semicolon Insertion (ASI)
//      We must insert semicolons at certain newlines. The parser tracks
//      line numbers to detect where ASI should apply.
//   2. Expression vs Statement ambiguity
//      "let" at the start of a statement could be a variable declaration
//      (let x = 1) or an expression where "let" is an identifier
//      (let.x = 1). We disambiguate based on context.
//   3. Arrow functions vs comma expressions
//      (a, b) => a + b looks like a parenthesized expression until we see =>
//   4. Object literals vs blocks
//      { x: 1 } could be an object literal or a block with a labeled statement.
//      We disambiguate based on context (expression context vs statement context).
//
// How V8 parses:
// ==============
// V8 uses a hand-written recursive descent parser (src/parsing/parser.cc).
// Features:
//   - Lazy parsing: functions are pre-parsed (skipped) until needed
//   - Error recovery: V8 can recover from syntax errors for IDE features
//   - Scope analysis: V8 tracks scopes during parsing for early errors
//   - Token budget: V8 limits token count to prevent DoS
//   - Preparse: V8 pre-scans functions to skip them until called
//
// =============================================================================

#ifndef CHINSTRAP_JS_PARSER_HPP
#define CHINSTRAP_JS_PARSER_HPP

#include "js_lexer.hpp"
#include "js_ast.hpp"
#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace chinstrap {

// =============================================================================
// Parse Error
// =============================================================================

struct ParseError {
    std::string message;
    int line;
    int col;
    std::string line_text;
};

// =============================================================================
// Parser
// =============================================================================

class Parser {
public:
    explicit Parser(std::string source);

    // Parse a full program
    std::unique_ptr<Program> parse_program();

    // Parse a single expression (for eval-like usage)
    std::unique_ptr<AstNode> parse_expression_only();

    // Check for errors
    bool has_errors() const { return !errors.empty(); }
    const std::vector<ParseError>& get_errors() const { return errors; }
    std::string error_string() const;

private:
    Lexer lexer;
    std::vector<ParseError> errors;

    // Current token (already consumed by next())
    Token current;

    // Track whether we are in async/generator context
    bool in_async = false;
    bool in_generator = false;

    // Track whether we are in strict mode
    bool strict_mode = true; // default to strict for modern JS

    // ---- Token helpers ----
    void advance();
    bool check(TokenType type) const;
    bool accept(TokenType type);
    bool expect(TokenType type);
    bool expect_semicolon();
    bool is_keyword(Token& tok) const;

    // ---- Error helpers ----
    void error(const std::string& msg);
    void error_at(const std::string& msg, int line, int col);

    // ---- ASI (Automatic Semicolon Insertion) ----
    bool can_insert_semicolon() const;
    void consume_semicolon();

    // ---- Precedence ----
    // TEACHING NOTE: Operator Precedence
    // ===================================
    // JavaScript operator precedence (highest to lowest):
    //   1. Grouping: ()
    //   2. Member access: . [], new (with args)
    //   3. new (without args), function call: ()
    //   4. Postfix: ++ --
    //   5. Unary: ! ~ + - ++ -- typeof void delete await
    //   6. Exponent: ** (right-assoc)
    //   7. Multiplicative: * / %
    //   8. Additive: + -
    //   9. Bitwise shift: << >> >>>
    //   10. Relational: < > <= >= instanceof in
    //   11. Equality: == != === !==
    //   12. Bitwise AND: &
    //   13. Bitwise XOR: ^
    //   14. Bitwise OR: |
    //   15. Logical AND: &&
    //   16. Logical OR: ||
    //   17. Nullish coalescing: ??
    //   18. Conditional: ?:
    //   19. Assignment: = += -= etc. (right-assoc)
    //   20. Comma: ,

    int binop_precedence(TokenType type);
    int logical_precedence(TokenType type);

    // ---- Expression parsing ----
    std::unique_ptr<AstNode> parse_expression();
    std::unique_ptr<AstNode> parse_assignment_expression();
    std::unique_ptr<AstNode> parse_conditional_expression();
    std::unique_ptr<AstNode> parse_binary_expression(int min_prec);
    std::unique_ptr<AstNode> parse_unary_expression();
    std::unique_ptr<AstNode> parse_update_expression();
    std::unique_ptr<AstNode> parse_left_hand_side_expression();
    std::unique_ptr<AstNode> parse_call_expression(std::unique_ptr<AstNode> callee);
    std::unique_ptr<AstNode> parse_member_expression(std::unique_ptr<AstNode> object);
    std::unique_ptr<AstNode> parse_primary_expression();
    std::unique_ptr<AstNode> parse_arguments(std::vector<std::unique_ptr<AstNode>>& args);
    std::unique_ptr<AstNode> parse_array_expression();
    std::unique_ptr<AstNode> parse_object_expression();
    std::unique_ptr<AstNode> parse_template_literal();
    std::unique_ptr<AstNode> parse_function_expression(bool is_async, bool is_generator);
    std::unique_ptr<AstNode> parse_arrow_function_body(std::vector<std::unique_ptr<AstNode>> params, bool is_async);
    std::unique_ptr<AstNode> parse_class_expression();
    std::unique_ptr<AstNode> parse_parenthesized_or_arrow();

    // ---- Statement parsing ----
    std::unique_ptr<AstNode> parse_statement();
    std::unique_ptr<AstNode> parse_block_statement();
    std::unique_ptr<AstNode> parse_variable_declaration();
    std::unique_ptr<AstNode> parse_if_statement();
    std::unique_ptr<AstNode> parse_for_statement();
    std::unique_ptr<AstNode> parse_while_statement();
    std::unique_ptr<AstNode> parse_do_while_statement();
    std::unique_ptr<AstNode> parse_switch_statement();
    std::unique_ptr<AstNode> parse_try_statement();
    std::unique_ptr<AstNode> parse_return_statement();
    std::unique_ptr<AstNode> parse_break_statement();
    std::unique_ptr<AstNode> parse_continue_statement();
    std::unique_ptr<AstNode> parse_throw_statement();
    std::unique_ptr<AstNode> parse_labeled_statement_or_expression();
    std::unique_ptr<AstNode> parse_class_declaration();

    // ---- Declaration parsing ----
    std::unique_ptr<AstNode> parse_function_declaration(bool is_async, bool is_generator);

    // ---- Pattern parsing (destructuring) ----
    std::unique_ptr<AstNode> parse_binding_pattern();
    std::unique_ptr<AstNode> parse_array_pattern();
    std::unique_ptr<AstNode> parse_object_pattern();

    // ---- Class parsing ----
    std::unique_ptr<ClassBody> parse_class_body();
    std::unique_ptr<MethodDefinition> parse_method_definition(bool is_static);

    // ---- Helpers ----
    std::vector<std::unique_ptr<AstNode>> parse_parameter_list();
    std::unique_ptr<AstNode> parse_function_body();

    // Check if current tokens form an arrow function
    bool is_arrow_function();
    bool is_async_arrow_function();
};

} // namespace chinstrap

#endif // CHINSTRAP_JS_PARSER_HPP