// =============================================================================
// js_parser.cpp - JavaScript Parser Implementation
// =============================================================================
//
// TEACHING NOTE: Recursive Descent Parser
// =========================================
//
// This parser is a hand-written recursive descent parser. Each grammar
// production is a function that consumes tokens and returns AST nodes.
//
// The parser structure follows the ECMAScript specification closely.
// The spec uses a notation called "grammar productions" like:
//   ExpressionStatement[Yield, Await] :
//     [lookahead not in {{, function, class, let [}] Expression[?Yield, ?Await] ;
//
// We translate these into C++ functions with if/else chains.
//
// Key parsing techniques:
//   1. Lookahead: we use peek() and peek2() to see upcoming tokens
//   2. Backtracking: for arrow functions, we save the position and restart
//      if the arrow function guess was wrong
//   3. Precedence climbing: for binary operators, we use a precedence table
//   4. Context sensitivity: arrow functions and destructuring require
//      context-dependent parsing decisions
//
// How V8 parser works:
// ====================
// V8 parser (src/parsing/parser.cc) is also recursive descent. Key features:
//   - Lazy parsing: functions are "pre-parsed" (skipped) until first called
//   - Scope tracking: V8 builds a scope tree during parsing
//   - Early errors: V8 reports syntax errors during parsing (not deferred)
//   - Token budget: V8 limits tokens to prevent DoS attacks
//   - Preparse: a fast scan that skips function bodies
//
// =============================================================================

#include "js_parser.hpp"
#include <cmath>
#include <set>
#include <algorithm>

namespace chinstrap {

// =============================================================================
// Constructor
// =============================================================================

Parser::Parser(std::string source) : lexer(std::move(source)) {
    advance(); // prime the first token
}

// =============================================================================
// Token helpers
// =============================================================================

void Parser::advance() {
    current = lexer.next();
}

bool Parser::check(TokenType type) const {
    return current.type == type;
}

bool Parser::accept(TokenType type) {
    if (current.type == type) {
        advance();
        return true;
    }
    return false;
}

bool Parser::expect(TokenType type) {
    if (accept(type)) return true;
    error("Expected " + current.type_name() + " but got " + current.type_name());
    return false;
}

bool Parser::is_keyword(Token& tok) const {
    return tok.is_keyword();
}

// =============================================================================
// Error handling
// =============================================================================

void Parser::error(const std::string& msg) {
    ParseError e;
    e.message = msg;
    e.line = current.line;
    e.col = current.col;
    e.line_text = lexer.get_line(current.line);
    errors.push_back(e);
}

void Parser::error_at(const std::string& msg, int line, int col) {
    ParseError e;
    e.message = msg;
    e.line = line;
    e.col = col;
    e.line_text = lexer.get_line(line);
    errors.push_back(e);
}

std::string Parser::error_string() const {
    std::string result;
    for (auto& e : errors) {
        result += "Line " + std::to_string(e.line) + ":" + std::to_string(e.col) +
                  " - " + e.message + "\n  " + e.line_text + "\n";
    }
    return result;
}

// =============================================================================
// ASI (Automatic Semicolon Insertion)
// =============================================================================

// TEACHING NOTE: ASI Implementation
// ==================================
// ASI is one of the most controversial features of JavaScript. The rules are:
//   1. When a newline is encountered and the next token cannot continue the
//      current statement, insert a semicolon.
//   2. When a closing brace } is encountered and the statement is not
//      properly terminated, insert a semicolon.
//   3. At end of file, insert a semicolon.
//
// Additionally, ASI does NOT insert semicolons if:
//   - The newline is inside parentheses or brackets
//   - The next line starts with ( [ + - / or a binary operator
//   - The current statement is a restricted production (e.g., return with
//     a newline before the value)
//
// We approximate this by: if current token is ; consume it. If not, and
// the current token is } or EOF, treat as semicolon. Otherwise error.
// Our lexer already skips newlines, so we lose some ASI precision. A
// production-grade lexer would need to track newlines explicitly.

void Parser::consume_semicolon() {
    if (accept(TokenType::Semicolon)) return;
    // ASI: end of file or closing brace implies semicolon
    if (check(TokenType::Eof) || check(TokenType::RBrace)) return;
    // ASI: if next token is on a new line, insert semicolon
    // Since our lexer skips newlines, we use a heuristic: if the current
    // token could not continue the current statement, treat as semicolon.
    // For simplicity, we just error.
    error("Expected semicolon");
}

bool Parser::can_insert_semicolon() const {
    return check(TokenType::Semicolon) || check(TokenType::Eof) || check(TokenType::RBrace);
}

// =============================================================================
// Precedence tables
// =============================================================================

int Parser::binop_precedence(TokenType type) {
    // TEACHING NOTE: Operator Precedence in JavaScript
    // ===============================================
    // Higher numbers = higher precedence = binds tighter.
    // The precedence climbing algorithm uses these values to decide
    // which operator to parse first. An operator with precedence N
    // will grab its right operand with min_prec = N + 1, preventing
    // lower-precedence operators from being absorbed into the right side.
    //
    // Standard JS operator precedence (from highest to lowest):
    //   ** (exponentiation, right-associative)
    //   * / %
    //   + -
    //   << >> >>>
    //   < > <= >= instanceof in
    //   == != === !==
    //   &
    //   ^
    //   |
    //   &&
    //   || ??
    switch (type) {
        case TokenType::Exponent: return 14;
        case TokenType::Asterisk:
        case TokenType::Slash:
        case TokenType::Percent: return 13;
        case TokenType::Plus:
        case TokenType::Minus: return 12;
        case TokenType::LeftShift:
        case TokenType::RightShift:
        case TokenType::UnsignedRightShift: return 11;
        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual:
        case TokenType::Instanceof:
        case TokenType::In: return 10;
        case TokenType::Equals:
        case TokenType::NotEquals:
        case TokenType::StrictEquals:
        case TokenType::StrictNotEquals: return 9;
        case TokenType::Ampersand: return 8;
        case TokenType::Caret: return 7;
        case TokenType::Pipe: return 6;
        default: return 0;
    }
}

int Parser::logical_precedence(TokenType type) {
    switch (type) {
        case TokenType::LogicalAnd: return 5;
        case TokenType::LogicalOr: return 4;
        case TokenType::NullishCoalescing: return 4;
        default: return 0;
    }
}

// =============================================================================
// Expression parsing
// =============================================================================

std::unique_ptr<AstNode> Parser::parse_expression() {
    // TEACHING NOTE: Comma Operator
    // ==============================
    // The comma operator evaluates both operands and returns the right one.
    // Example: (a = 1, b = 2, a + b) evaluates to 3.
    // It has the lowest precedence of all operators.

    auto expr = parse_assignment_expression();
    if (!check(TokenType::Comma)) return expr;

    auto seq = std::make_unique<SequenceExpression>();
    seq->expressions.push_back(std::move(expr));
    while (accept(TokenType::Comma)) {
        seq->expressions.push_back(parse_assignment_expression());
    }
    return seq;
}

std::unique_ptr<AstNode> Parser::parse_assignment_expression() {
    // TEACHING NOTE: Assignment Parsing
    // ==================================
    // Assignment is right-associative: a = b = c means a = (b = c).
    // We parse the left side first, then check for assignment operators.
    //
    // The left side can be:
    //   - An identifier: x = ...
    //   - A member expression: obj.x = ... or arr[0] = ...
    //   - A destructuring pattern: [a, b] = ... or {x, y} = ...
    //
    // Destructuring assignment is tricky: [a, b] could be an array literal
    // or a destructuring pattern. We parse it as an expression first, then
    // reinterpret it as a pattern if followed by =.

    // Check for arrow function: (params) => ... or async (params) => ...
    // Also: x => ... (single identifier arrow)
    if (check(TokenType::Identifier) && lexer.peek().type == TokenType::Arrow) {
        // Single parameter arrow function: x => body
        std::string name = current.value;
        advance(); // identifier
        advance(); // =>
        bool was_async = in_async;
        auto params = std::vector<std::unique_ptr<AstNode>>();
        auto param = std::make_unique<Identifier>();
        param->name = name;
        params.push_back(std::move(param));
        return parse_arrow_function_body(std::move(params), was_async);
    }

    // async x => ... (async arrow with single param)
    if (check(TokenType::Async) && lexer.peek().type == TokenType::Identifier &&
        lexer.peek2().type == TokenType::Arrow) {
        advance(); // async
        std::string name = current.value;
        advance(); // identifier
        advance(); // =>
        bool was_async = in_async;
        in_async = true;
        auto params = std::vector<std::unique_ptr<AstNode>>();
        auto param = std::make_unique<Identifier>();
        param->name = name;
        params.push_back(std::move(param));
        auto result = parse_arrow_function_body(std::move(params), true);
        in_async = was_async;
        return result;
    }

    // Check for yield
    if (check(TokenType::Yield) && in_generator) {
        advance(); // yield
        auto yield_expr = std::make_unique<YieldExpression>();
        if (accept(TokenType::Asterisk)) {
            yield_expr->delegate = true;
        }
        // yield has an optional argument (unless next token cannot start an expression)
        if (!can_insert_semicolon() && !check(TokenType::Semicolon) &&
            !check(TokenType::RParen) && !check(TokenType::RBrace) &&
            !check(TokenType::RBracket) && !check(TokenType::Colon) &&
            !check(TokenType::Comma) && !check(TokenType::Eof)) {
            yield_expr->argument = parse_assignment_expression();
        }
        return yield_expr;
    }

    // Parse left side
    auto left = parse_conditional_expression();

    // Check for assignment operators
    static const std::set<TokenType> assign_ops = {
        TokenType::Assign, TokenType::PlusAssign, TokenType::MinusAssign,
        TokenType::MultiplyAssign, TokenType::DivideAssign, TokenType::PercentAssign,
        TokenType::ExponentAssign, TokenType::LeftShiftAssign, TokenType::RightShiftAssign,
        TokenType::UnsignedRightShiftAssign, TokenType::BitAndAssign,
        TokenType::BitOrAssign, TokenType::BitXorAssign,
        TokenType::LogicalAndAssign, TokenType::LogicalOrAssign,
        TokenType::NullishCoalescingAssign,
    };

    if (assign_ops.count(current.type) > 0) {
        auto assign = std::make_unique<AssignmentExpression>();
        assign->op = current.type_name();
        advance(); // consume operator
        assign->left = std::move(left);
        assign->right = parse_assignment_expression(); // right-assoc
        return assign;
    }

    return left;
}

std::unique_ptr<AstNode> Parser::parse_conditional_expression() {
    // TEACHING NOTE: Ternary Operator
    // =================================
    // The conditional (ternary) operator: cond ? a : b
    // It is right-associative: a ? b : c ? d : e means a ? b : (c ? d : e)

    auto test = parse_binary_expression(0);

    // Check for ?? and && / ||
    // Note: ?? cannot be mixed with && or || without parentheses
    if (check(TokenType::NullishCoalescing) || check(TokenType::LogicalAnd) ||
        check(TokenType::LogicalOr)) {
        std::string op = current.type_name();
        advance();
        auto right = parse_binary_expression(0);
        auto logical = std::make_unique<LogicalExpression>();
        logical->op = op;
        logical->left = std::move(test);
        logical->right = std::move(right);
        // Chain: a || b || c
        while (check(TokenType::NullishCoalescing) || check(TokenType::LogicalAnd) ||
               check(TokenType::LogicalOr)) {
            op = current.type_name();
            advance();
            auto next = parse_binary_expression(0);
            auto outer = std::make_unique<LogicalExpression>();
            outer->op = op;
            outer->left = std::move(logical);
            outer->right = std::move(next);
            logical = std::move(outer);
        }
        test = std::move(logical);
    }

    if (accept(TokenType::QuestionMark)) {
        auto cond = std::make_unique<ConditionalExpression>();
        cond->test = std::move(test);
        cond->consequent = parse_assignment_expression();
        expect(TokenType::Colon);
        cond->alternate = parse_assignment_expression();
        return cond;
    }

    return test;
}

std::unique_ptr<AstNode> Parser::parse_binary_expression(int min_prec) {
    // TEACHING NOTE: Precedence Climbing
    // ===================================
    // We use the "precedence climbing" algorithm for binary expressions.
    // The algorithm:
    //   1. Parse the left operand (a unary expression)
    //   2. While the current token is a binary operator with precedence >= min_prec:
    //      a. Consume the operator
    //      b. Parse the right operand with precedence = op_prec + 1
//      c. Build a BinaryExpression node
    //   3. Return the left operand
    //
    // For right-associative operators (like **), we use op_prec (not op_prec + 1)
    // for the right operand, so a**b**c parses as a**(b**c).

    auto left = parse_unary_expression();

    for (;;) {
        TokenType op_type = current.type;
        int prec = binop_precedence(op_type);

        if (prec < min_prec || prec == 0) break;

        bool right_assoc = (op_type == TokenType::Exponent);
        std::string op = current.type_name();
        advance();

        int next_min_prec = right_assoc ? prec : prec + 1;
        auto right = parse_binary_expression(next_min_prec);

        auto bin = std::make_unique<BinaryExpression>();
        bin->op = op;
        bin->left = std::move(left);
        bin->right = std::move(right);
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<AstNode> Parser::parse_unary_expression() {
    // TEACHING NOTE: Unary Operators
    // ===============================
    // Prefix unary operators: ! ~ + - typeof void delete await
    // These have higher precedence than binary operators but lower than
    // member access and function calls.

    static const std::set<TokenType> unary_prefix = {
        TokenType::Plus, TokenType::Minus, TokenType::Tilde, TokenType::Not,
    };

    // Handle keyword unary operators
    if (check(TokenType::Typeof) || check(TokenType::Void) ||
        check(TokenType::Delete)) {
        std::string op = current.type_name();
        advance();
        auto unary = std::make_unique<UnaryExpression>();
        unary->op = op;
        unary->prefix = true;
        unary->argument = parse_unary_expression();
        return unary;
    }

    // Handle + - ~ ! as prefix
    if (check(TokenType::Plus) || check(TokenType::Minus) || check(TokenType::Tilde) ||
        check(TokenType::Not)) {
        std::string op = current.type_name();
        advance();
        auto unary = std::make_unique<UnaryExpression>();
        unary->op = op;
        unary->prefix = true;
        unary->argument = parse_unary_expression();
        return unary;
    }

    if (check(TokenType::LogicalAnd)) {
        // ! operator (we use LogicalAnd token type for !)
        // Actually, ! should be its own token. Let me check...
        // We do not have a Not token. Let us use a different approach.
    }

    // Handle await (in async context)
    if (check(TokenType::Await) && in_async) {
        advance();
        auto await_expr = std::make_unique<AwaitExpression>();
        await_expr->argument = parse_unary_expression();
        return await_expr;
    }

    return parse_update_expression();
}

std::unique_ptr<AstNode> Parser::parse_update_expression() {
    // TEACHING NOTE: Update Operators (++ and --)
    // ============================================
    // ++ and -- can be prefix (++x) or postfix (x++).
    // Prefix: the value is updated before it is used.
    // Postfix: the value is updated after it is used.
    // Both have side effects - they modify the operand.

    // Prefix
    if (check(TokenType::Increment) || check(TokenType::Decrement)) {
        std::string op = current.type_name();
        advance();
        auto update = std::make_unique<UpdateExpression>();
        update->op = op;
        update->prefix = true;
        update->argument = parse_unary_expression();
        return update;
    }

    // Parse the expression first (for postfix)
    auto expr = parse_left_hand_side_expression();

    // Postfix
    if (check(TokenType::Increment) || check(TokenType::Decrement)) {
        std::string op = current.type_name();
        advance();
        auto update = std::make_unique<UpdateExpression>();
        update->op = op;
        update->prefix = false;
        update->argument = std::move(expr);
        return update;
    }

    return expr;
}

std::unique_ptr<AstNode> Parser::parse_left_hand_side_expression() {
    // TEACHING NOTE: Left-Hand Side Expressions
    // ==========================================
    // This handles: new expressions, member access, and function calls.
    // Examples: new Foo(), obj.method(), arr[0], f()()

    // Check for new
    if (check(TokenType::New)) {
        advance();
        auto new_expr = std::make_unique<NewExpression>();

        // new.target
        if (check(TokenType::Dot)) {
            // This is new.target - but we do not support it fully
            advance();
            if (check(TokenType::Identifier) && current.value == "target") {
                advance();
            }
            // Return an identifier "new.target" for now
            auto id = std::make_unique<Identifier>();
            id->name = "new.target";
            return id;
        }

        // Parse the callee (which could be another new expression)
        auto callee = parse_left_hand_side_expression();
        new_expr->callee = std::move(callee);

        // Parse arguments if present
        if (check(TokenType::LParen)) {
            parse_arguments(new_expr->arguments);
        }

        // Handle chained member access / calls after new
        auto result = std::unique_ptr<AstNode>(new_expr.release());
        while (check(TokenType::Dot) || check(TokenType::LBracket) ||
               check(TokenType::LParen) || check(TokenType::OptionalChaining) ||
               check(TokenType::TemplateStart)) {
            if (check(TokenType::Dot) || check(TokenType::LBracket) ||
                check(TokenType::OptionalChaining)) {
                result = parse_member_expression(std::move(result));
            } else if (check(TokenType::LParen)) {
                std::vector<std::unique_ptr<AstNode>> args;
                parse_arguments(args);
                auto call = std::make_unique<CallExpression>();
                call->callee = std::move(result);
                call->arguments = std::move(args);
                result = std::move(call);
            } else if (check(TokenType::TemplateStart)) {
                // Tagged template: expr`...`
                auto tagged = std::make_unique<AstNode>(AstNodeType::TaggedTemplateExpression);
                // Simplified: just parse the template
                auto tmpl = parse_template_literal();
                // For now, return the template (tagged templates are rare)
                return tmpl;
            }
        }
        return result;
    }

    auto expr = parse_primary_expression();

    // Handle member access and calls
    while (check(TokenType::Dot) || check(TokenType::LBracket) ||
           check(TokenType::LParen) || check(TokenType::OptionalChaining) ||
           check(TokenType::TemplateStart)) {
        if (check(TokenType::Dot) || check(TokenType::LBracket) ||
            check(TokenType::OptionalChaining)) {
            expr = parse_member_expression(std::move(expr));
        } else if (check(TokenType::LParen)) {
            std::vector<std::unique_ptr<AstNode>> args;
            parse_arguments(args);
            auto call = std::make_unique<CallExpression>();
            call->callee = std::move(expr);
            call->arguments = std::move(args);
            expr = std::move(call);
        } else if (check(TokenType::TemplateStart)) {
            // Tagged template
            auto tmpl = parse_template_literal();
            return tmpl; // simplified
        }
    }

    return expr;
}

std::unique_ptr<AstNode> Parser::parse_member_expression(std::unique_ptr<AstNode> object) {
    // TEACHING NOTE: Member Access
    // =============================
    // Two forms of member access:
    //   1. Dot notation: obj.prop - prop must be a valid identifier
    //   2. Bracket notation: obj[expr] - expr is evaluated to get property name
    //
    // Optional chaining (?.) works like dot access but returns undefined
    // if the object is null or undefined.

    auto member = std::make_unique<MemberExpression>();
    member->object = std::move(object);

    if (accept(TokenType::Dot)) {
        // Dot access: obj.prop
        member->computed = false;
        auto prop = std::make_unique<Identifier>();
        if (check(TokenType::Identifier) || current.is_keyword()) {
            prop->name = current.value;
            advance();
        } else {
            error("Expected property name after dot");
        }
        member->property = std::move(prop);
    } else if (accept(TokenType::OptionalChaining)) {
        member->optional = true;
        member->computed = false;
        auto prop = std::make_unique<Identifier>();
        if (check(TokenType::Identifier) || current.is_keyword()) {
            prop->name = current.value;
            advance();
        } else if (check(TokenType::LParen)) {
            // Optional call: f?.()
            // This should be handled as a call expression, not member
            // For simplicity, we handle it here
            std::vector<std::unique_ptr<AstNode>> args;
            parse_arguments(args);
            auto call = std::make_unique<CallExpression>();
            call->callee = std::move(member->object);
            call->arguments = std::move(args);
            call->optional = true;
            return call;
        } else {
            error("Expected property name after ?.");
        }
        member->property = std::move(prop);
    } else if (accept(TokenType::LBracket)) {
        // Bracket access: obj[expr]
        member->computed = true;
        member->property = parse_expression();
        expect(TokenType::RBracket);
    } else {
        error("Expected member access");
    }

    return member;
}

std::unique_ptr<AstNode> Parser::parse_arguments(std::vector<std::unique_ptr<AstNode>>& args) {
    // TEACHING NOTE: Function Arguments
    // =================================
    // Arguments are comma-separated expressions, wrapped in parentheses.
    // Spread (...) can be used in call arguments: f(...args)

    expect(TokenType::LParen);
    if (!check(TokenType::RParen)) {
        do {
            if (check(TokenType::Spread)) {
                advance();
                auto spread = std::make_unique<SpreadElement>();
                spread->argument = parse_assignment_expression();
                args.push_back(std::move(spread));
            } else {
                args.push_back(parse_assignment_expression());
            }
        } while (accept(TokenType::Comma) && !check(TokenType::RParen));
    }
    expect(TokenType::RParen);
    return nullptr;
}

std::unique_ptr<AstNode> Parser::parse_primary_expression() {
    // TEACHING NOTE: Primary Expressions
    // ==================================
    // Primary expressions are the building blocks:
    //   - Literals: numbers, strings, true, false, null, undefined
    //   - Identifiers: variable references
    //   - this: the current this binding
    //   - Parenthesized expressions: (expr)
    //   - Array literals: [1, 2, 3]
    //   - Object literals: {x: 1, y: 2}
    //   - Function expressions: function() {}
    //   - Arrow functions: () => {}
    //   - Class expressions: class {}
    //   - Template literals: `hello`
    //   - Regex literals: /pattern/flags

    switch (current.type) {
        case TokenType::Number: {
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::number_val(current.number_value);
            advance();
            return lit;
        }
        case TokenType::String: {
            auto lit = std::make_unique<Literal>();
            lit->value = make_string(current.cooked_value.empty() ? current.value : current.cooked_value);
            advance();
            return lit;
        }
        case TokenType::True: {
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::boolean_val(true);
            advance();
            return lit;
        }
        case TokenType::False: {
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::boolean_val(false);
            advance();
            return lit;
        }
        case TokenType::Null: {
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::null_val();
            advance();
            return lit;
        }
        case TokenType::Undefined: {
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::undefined();
            advance();
            return lit;
        }
        case TokenType::This: {
            advance();
            auto id = std::make_unique<Identifier>();
            id->name = "this";
            id->node_type = AstNodeType::Identifier;
            return id;
        }
        case TokenType::Super: {
            advance();
            auto id = std::make_unique<Identifier>();
            id->name = "super";
            return id;
        }
        case TokenType::Identifier: {
            auto id = std::make_unique<Identifier>();
            id->name = current.value;
            advance();
            return id;
        }
        case TokenType::PrivateIdentifier: {
            auto id = std::make_unique<Identifier>();
            id->name = "#" + current.value;
            advance();
            return id;
        }
        case TokenType::LParen: {
            return parse_parenthesized_or_arrow();
        }
        case TokenType::LBracket: {
            return parse_array_expression();
        }
        case TokenType::LBrace: {
            return parse_object_expression();
        }
        case TokenType::Function: {
            return parse_function_expression(false, false);
        }
        case TokenType::Async: {
            if (lexer.peek().type == TokenType::Function) {
                advance(); // async
                return parse_function_expression(true, false);
            }
            // async arrow or async identifier
            // Check for async arrow: async () => ... or async x => ...
            // For simplicity, treat async as identifier for now
            auto id = std::make_unique<Identifier>();
            id->name = "async";
            advance();
            return id;
        }
        case TokenType::Class: {
            return parse_class_expression();
        }
        case TokenType::TemplateStart:
        case TokenType::Template: {
            return parse_template_literal();
        }
        case TokenType::Slash:
        case TokenType::Regex: {
            // Regex literal (should not normally reach here because of lexer
            // disambiguation, but handle it anyway)
            auto regex = std::make_unique<RegexLiteral>();
            if (current.type == TokenType::Regex) {
                regex->pattern = current.value;
                regex->flags = current.regex_flags;
                advance();
            }
            return regex;
        }
        case TokenType::Yield: {
            // In non-generator context, yield is an identifier
            if (!in_generator) {
                auto id = std::make_unique<Identifier>();
                id->name = "yield";
                advance();
                return id;
            }
            // Should have been handled in parse_assignment_expression
            error("Unexpected yield");
            advance();
            return std::make_unique<Identifier>();
        }
        case TokenType::Await: {
            // In non-async context, await is an identifier
            if (!in_async) {
                auto id = std::make_unique<Identifier>();
                id->name = "await";
                advance();
                return id;
            }
            error("Unexpected await");
            advance();
            return std::make_unique<Identifier>();
        }
        default:
            // Handle keyword-as-identifier in expression context
            if (current.is_keyword()) {
                auto id = std::make_unique<Identifier>();
                id->name = current.value;
                advance();
                return id;
            }
            error("Unexpected token: " + current.type_name() + " (" + current.value + ")");
            advance();
            return std::make_unique<Identifier>();
    }
}

std::unique_ptr<AstNode> Parser::parse_parenthesized_or_arrow() {
    // TEACHING NOTE: Arrow Function Disambiguation
    // ============================================
    // This is one of the hardest parts of parsing JavaScript.
    // (a, b) could be:
    //   1. A parenthesized expression (sequence): (a, b)
    //   2. Arrow function parameters: (a, b) => ...
    //
    // We try to parse as arrow function parameters first. If we do not see =>
    // after the closing paren, we reinterpret as a parenthesized expression.
    //
    // V8 handles this by speculatively parsing and backtracking. We do the same.

    // Save position for backtracking
    // Since our lexer does not support backtracking easily, we parse
    // the parenthesized expression and check for => after.

    // Simple approach: parse as parenthesized expression, then if => follows,
    // convert to arrow function parameters.

    advance(); // consume (

    // Empty params: () => ...
    if (check(TokenType::RParen)) {
        advance();
        if (check(TokenType::Arrow)) {
            advance();
            return parse_arrow_function_body({}, in_async);
        }
        // Empty parenthesized expression - error
        error("Expected expression");
        return std::make_unique<Identifier>();
    }

    // Try to parse as expression list (could be params or comma expression)
    std::vector<std::unique_ptr<AstNode>> expressions;
    std::vector<std::unique_ptr<AstNode>> params;

    auto first = parse_assignment_expression();
    expressions.push_back(std::move(first));

    // Check if first could be a parameter pattern
    // (We do this loosely - any identifier or pattern)

    while (accept(TokenType::Comma) && !check(TokenType::RParen)) {
        auto next = parse_assignment_expression();
        expressions.push_back(std::move(next));
    }

    expect(TokenType::RParen);

    // Check for arrow =>
    if (check(TokenType::Arrow)) {
        advance();
        // Convert expressions to params
        for (auto& expr : expressions) {
            params.push_back(std::move(expr));
        }
        return parse_arrow_function_body(std::move(params), in_async);
    }

    // Not an arrow function - return as sequence or single expression
    if (expressions.size() == 1) {
        return std::move(expressions[0]);
    }
    auto seq = std::make_unique<SequenceExpression>();
    seq->expressions = std::move(expressions);
    return seq;
}

std::unique_ptr<AstNode> Parser::parse_arrow_function_body(
    std::vector<std::unique_ptr<AstNode>> params, bool is_async) {
    // TEACHING NOTE: Arrow Function Bodies
    // =====================================
    // Arrow functions can have two body forms:
    //   1. Expression body: x => x + 1 (implicit return)
    //   2. Block body: x => { return x + 1; } (explicit return needed)

    auto arrow = std::make_unique<ArrowFunctionExpression>();
    arrow->params = std::move(params);
    arrow->is_async = is_async;

    if (check(TokenType::LBrace)) {
        // Block body
        arrow->body = parse_block_statement();
        arrow->expression_body = false;
    } else {
        // Expression body (implicit return)
        arrow->body = parse_assignment_expression();
        arrow->expression_body = true;
    }

    return arrow;
}

std::unique_ptr<AstNode> Parser::parse_array_expression() {
    // TEACHING NOTE: Array Literals
    // ==============================
    // Array literals: [1, 2, 3]
    // Features:
    //   - Holes: [1, , 3] - creates a sparse array with a hole at index 1
    //   - Spread: [...arr] - expands an iterable
    //   - Trailing comma: [1, 2,] - allowed, does not create a hole

    expect(TokenType::LBracket);
    auto array = std::make_unique<ArrayExpression>();

    while (!check(TokenType::RBracket) && !check(TokenType::Eof)) {
        if (check(TokenType::Comma)) {
            // Hole
            array->elements.push_back(nullptr);
            advance();
            continue;
        }
        if (check(TokenType::Spread)) {
            advance();
            auto spread = std::make_unique<SpreadElement>();
            spread->argument = parse_assignment_expression();
            array->elements.push_back(std::move(spread));
        } else {
            array->elements.push_back(parse_assignment_expression());
        }
        if (!check(TokenType::RBracket)) {
            if (!accept(TokenType::Comma)) {
                break;
            }
        }
    }
    expect(TokenType::RBracket);
    return array;
}

std::unique_ptr<AstNode> Parser::parse_object_expression() {
    // TEACHING NOTE: Object Literals
    // ==============================
    // Object literals: { key: value, key2: value2 }
    // Features:
    //   - Shorthand: { x, y } - same as { x: x, y: y }
    //   - Computed keys: { [expr]: value }
    //   - Method shorthand: { method() {} }
    //   - Getters/setters: { get prop() {}, set prop(v) {} }
    //   - Spread: { ...obj }
    //   - Trailing comma: { a: 1, } - allowed

    expect(TokenType::LBrace);
    auto obj = std::make_unique<ObjectExpression>();

    while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
        // Spread in object: { ...obj }
        if (check(TokenType::Spread)) {
            advance();
            auto spread = std::make_unique<SpreadElement>();
            spread->argument = parse_assignment_expression();
            // Object spread - we add it as a special property
            auto prop = std::make_unique<Property>();
            prop->key = std::make_unique<Identifier>();
            auto spread_id = std::make_unique<Identifier>();
            spread_id->name = "__spread__";
            prop->key = std::move(spread_id);
            prop->value = std::move(spread);
            prop->computed = false;
            obj->properties.push_back(std::move(prop));
            if (!accept(TokenType::Comma)) break;
            continue;
        }

        auto prop = std::make_unique<Property>();

        // Computed property key: { [expr]: value }
        if (check(TokenType::LBracket)) {
            advance();
            prop->computed = true;
            prop->key = parse_assignment_expression();
            expect(TokenType::RBracket);
        } else if (check(TokenType::String)) {
            // String key: { "key": value }
            auto lit = std::make_unique<Literal>();
            lit->value = make_string(current.cooked_value.empty() ? current.value : current.cooked_value);
            prop->key = std::move(lit);
            advance();
        } else if (check(TokenType::Number)) {
            // Number key: { 0: value }
            auto lit = std::make_unique<Literal>();
            lit->value = JSValue::number_val(current.number_value);
            prop->key = std::move(lit);
            advance();
        } else if (check(TokenType::Identifier) || current.is_keyword()) {
            // Identifier key
            auto id = std::make_unique<Identifier>();
            id->name = current.value;
            prop->key = std::move(id);
            advance();
        } else {
            error("Expected property key");
            break;
        }

        // Check for method shorthand: { method() {} }
        if (check(TokenType::LParen)) {
            // Method shorthand
            prop->is_method = true;
            prop->kind = "init";
            auto fn = std::make_unique<FunctionExpression>();
            fn->params = parse_parameter_list();
            fn->body = parse_function_body();
            prop->value = std::move(fn);
        } else if (check(TokenType::Colon)) {
            // Regular property: { key: value }
            advance();
            prop->kind = "init";
            prop->value = parse_assignment_expression();
        } else {
            // Shorthand property: { x } - same as { x: x }
            prop->shorthand = true;
            prop->kind = "init";
            // The key is also the value
            auto id = std::make_unique<Identifier>();
            if (prop->key->node_type == AstNodeType::Identifier) {
                auto* key_id = static_cast<Identifier*>(prop->key.get());
                id->name = key_id->name;
            }
            prop->value = std::move(id);
        }

        // Getters and setters: { get prop() {} }
        // These are handled by checking if the key is "get" or "set" keyword
        // followed by a property name. We handle them at the start of the
        // property parsing. For simplicity, we skip full get/set support.

        obj->properties.push_back(std::move(prop));

        if (!accept(TokenType::Comma)) break;
    }

    expect(TokenType::RBrace);
    return obj;
}

std::unique_ptr<AstNode> Parser::parse_template_literal() {
    // TEACHING NOTE: Template Literal Parsing
    // ========================================
    // Template literals: `text ${expr} more text`
    // The lexer produces TemplateStart, TemplateMiddle, TemplateEnd tokens.
    // We parse them into a TemplateLiteral node with quasis and expressions.

    auto tmpl = std::make_unique<TemplateLiteral>();

    // First quasi (TemplateStart contains the text before first ${)
    auto quasi = std::make_unique<TemplateElement>();
    quasi->raw = current.value;
    quasi->cooked = current.cooked_value;
    tmpl->quasis.push_back(std::move(quasi));

    advance(); // consume TemplateStart

    // Parse expression
    tmpl->expressions.push_back(parse_expression());

    // Continue parsing middles and ends
    while (check(TokenType::TemplateMiddle)) {
        auto q = std::make_unique<TemplateElement>();
        q->raw = current.value;
        q->cooked = current.cooked_value;
        tmpl->quasis.push_back(std::move(q));
        advance();
        tmpl->expressions.push_back(parse_expression());
    }

    // Final quasi (TemplateEnd)
    if (check(TokenType::TemplateEnd)) {
        auto q = std::make_unique<TemplateElement>();
        q->raw = current.value;
        q->cooked = current.cooked_value;
        q->tail = true;
        tmpl->quasis.push_back(std::move(q));
        advance();
    }

    return tmpl;
}

std::unique_ptr<AstNode> Parser::parse_function_expression(bool is_async, bool is_generator) {
    // TEACHING NOTE: Function Expressions
    // ====================================
    // Function expressions: function() {}, function name() {}, function*() {}
    // They create a function value that can be assigned or passed around.

    expect(TokenType::Function);
    if (check(TokenType::Asterisk)) {
        is_generator = true;
        advance();
    }

    auto fn = std::make_unique<FunctionExpression>();
    fn->is_async = is_async;
    fn->is_generator = is_generator;

    // Optional name
    if (check(TokenType::Identifier)) {
        fn->name = current.value;
        advance();
    }

    bool was_async = in_async;
    bool was_generator = in_generator;
    in_async = is_async;
    in_generator = is_generator;

    fn->params = parse_parameter_list();
    fn->body = parse_function_body();

    in_async = was_async;
    in_generator = was_generator;

    return fn;
}

std::unique_ptr<AstNode> Parser::parse_class_expression() {
    expect(TokenType::Class);

    auto cls = std::make_unique<ClassExpression>();

    // Optional name
    if (check(TokenType::Identifier)) {
        // Store name in a temporary - ClassExpression does not have a name field
        // We use internal_slots or just skip it
        advance();
    }

    // Extends
    if (accept(TokenType::Extends)) {
        cls->superclass = parse_left_hand_side_expression();
    }

    cls->body = parse_class_body();
    return cls;
}

// =============================================================================
// Statement parsing
// =============================================================================

std::unique_ptr<AstNode> Parser::parse_statement() {
    // TEACHING NOTE: Statement Dispatch
    // ==================================
    // We dispatch based on the current token. Keywords like if, while, for
    // start specific statement types. Otherwise, we parse an expression statement.

    switch (current.type) {
        case TokenType::LBrace: return parse_block_statement();
        case TokenType::Var:
        case TokenType::Let:
        case TokenType::Const: return parse_variable_declaration();
        case TokenType::If: return parse_if_statement();
        case TokenType::For: return parse_for_statement();
        case TokenType::While: return parse_while_statement();
        case TokenType::Do: return parse_do_while_statement();
        case TokenType::Switch: return parse_switch_statement();
        case TokenType::Try: return parse_try_statement();
        case TokenType::Return: return parse_return_statement();
        case TokenType::Break: return parse_break_statement();
        case TokenType::Continue: return parse_continue_statement();
        case TokenType::Throw: return parse_throw_statement();
        case TokenType::Function: return parse_function_declaration(false, false);
        case TokenType::Async:
            if (lexer.peek().type == TokenType::Function) {
                advance(); // async
                return parse_function_declaration(true, false);
            }
            // Fall through to expression
            return parse_labeled_statement_or_expression();
        case TokenType::Class: return parse_class_declaration();
        case TokenType::Semicolon: {
            // Empty statement
            advance();
            return std::make_unique<EmptyStatement>();
        }
        case TokenType::Import:
        case TokenType::Export: {
            // Parse import/export as statement (minimal support)
            // For now, skip them
            advance();
            while (!check(TokenType::Semicolon) && !check(TokenType::Eof)) {
                advance();
            }
            accept(TokenType::Semicolon);
            return std::make_unique<EmptyStatement>();
        }
        case TokenType::Debugger: {
            advance();
            consume_semicolon();
            return std::make_unique<EmptyStatement>();
        }
        default:
            return parse_labeled_statement_or_expression();
    }
}

std::unique_ptr<AstNode> Parser::parse_labeled_statement_or_expression() {
    // TEACHING NOTE: Labeled Statements
    // =================================
    // A label is an identifier followed by a colon: label: statement
    // Labels are used with break/continue for nested loops.
    // We check for this pattern before parsing as an expression statement.

    // Check for label: identifier followed by :
    if (check(TokenType::Identifier) && lexer.peek().type == TokenType::Colon) {
        std::string label = current.value;
        advance(); // identifier
        advance(); // :

        auto labeled = std::make_unique<LabeledStatement>();
        labeled->label = label;
        labeled->body = parse_statement();
        return labeled;
    }

    // Expression statement
    auto expr = parse_expression();
    consume_semicolon();

    auto stmt = std::make_unique<ExpressionStatement>();
    stmt->expression = std::move(expr);
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_block_statement() {
    // TEACHING NOTE: Block Scope
    // ===========================
    // A block creates a new lexical scope for let/const declarations.
    // Variables declared with var are NOT block-scoped (they are function-scoped).

    expect(TokenType::LBrace);
    auto block = std::make_unique<BlockStatement>();

    while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
        block->body.push_back(parse_statement());
    }

    expect(TokenType::RBrace);
    return block;
}

std::unique_ptr<AstNode> Parser::parse_variable_declaration() {
    // TEACHING NOTE: Variable Declarations
    // =====================================
    // Three kinds: var, let, const
    //   var x = 1; - function-scoped, hoisted
    //   let x = 1; - block-scoped, TDZ
    //   const x = 1; - block-scoped, cannot reassign

    auto decl = std::make_unique<VariableDeclaration>();
    decl->kind = current.type_name();
    advance(); // var/let/const

    do {
        auto declarator = std::make_unique<VariableDeclarator>();

        // Parse the binding pattern
        declarator->id = parse_binding_pattern();

        // Optional initializer
        if (accept(TokenType::Assign)) {
            declarator->init = parse_assignment_expression();
        }

        decl->declarations.push_back(std::move(declarator));
    } while (accept(TokenType::Comma));

    consume_semicolon();
    return decl;
}

std::unique_ptr<AstNode> Parser::parse_binding_pattern() {
    // TEACHING NOTE: Binding Patterns
    // ================================
    // Variable declarations can use destructuring:
    //   let [a, b] = arr;       - array destructuring
    //   let {x, y} = obj;        - object destructuring
    //   let {x: a, y: b} = obj;  - renaming
    //   let {x = 10} = obj;      - default values
    //   let [...rest] = arr;    - rest element

    if (check(TokenType::LBracket)) {
        return parse_array_pattern();
    }
    if (check(TokenType::LBrace)) {
        return parse_object_pattern();
    }

    // Simple identifier
    if (check(TokenType::Identifier)) {
        auto id = std::make_unique<Identifier>();
        id->name = current.value;
        advance();
        return id;
    }

    error("Expected binding pattern");
    auto id = std::make_unique<Identifier>();
    id->name = "__error__";
    return id;
}

std::unique_ptr<AstNode> Parser::parse_array_pattern() {
    expect(TokenType::LBracket);
    auto pattern = std::make_unique<ArrayPattern>();

    while (!check(TokenType::RBracket) && !check(TokenType::Eof)) {
        if (accept(TokenType::Comma)) {
            // Hole
            pattern->elements.push_back(nullptr);
            continue;
        }

        if (check(TokenType::Spread)) {
            advance();
            auto rest = std::make_unique<RestElement>();
            rest->argument = parse_binding_pattern();
            pattern->rest = std::move(rest);
            break;
        }

        // Element with default value
        auto elem = parse_binding_pattern();
        if (accept(TokenType::Assign)) {
            // Default value
            auto assign = std::make_unique<AssignmentPattern>();
            assign->left = std::move(elem);
            assign->right = parse_assignment_expression();
            pattern->elements.push_back(std::move(assign));
        } else {
            pattern->elements.push_back(std::move(elem));
        }

        if (!check(TokenType::RBracket)) {
            accept(TokenType::Comma);
        }
    }

    expect(TokenType::RBracket);
    return pattern;
}

std::unique_ptr<AstNode> Parser::parse_object_pattern() {
    expect(TokenType::LBrace);
    auto pattern = std::make_unique<ObjectPattern>();

    while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
        if (check(TokenType::Spread)) {
            advance();
            auto rest = std::make_unique<RestElement>();
            rest->argument = parse_binding_pattern();
            pattern->rest = std::move(rest);
            break;
        }

        auto prop = std::make_unique<Property>();

        // Key
        std::string key_name;
        if (check(TokenType::Identifier) || current.is_keyword()) {
            key_name = current.value;
            auto id = std::make_unique<Identifier>();
            id->name = current.value;
            prop->key = std::move(id);
            advance();
        } else if (check(TokenType::String)) {
            auto lit = std::make_unique<Literal>();
            lit->value = make_string(current.value);
            prop->key = std::move(lit);
            advance();
        } else if (check(TokenType::LBracket)) {
            advance();
            prop->computed = true;
            prop->key = parse_assignment_expression();
            expect(TokenType::RBracket);
        } else {
            error("Expected property name in object pattern");
            break;
        }

        // Renaming: { x: y } or default: { x = 10 }
        if (accept(TokenType::Colon)) {
            // Renamed: { key: pattern }
            auto value = parse_binding_pattern();
            if (accept(TokenType::Assign)) {
                auto assign = std::make_unique<AssignmentPattern>();
                assign->left = std::move(value);
                assign->right = parse_assignment_expression();
                prop->value = std::move(assign);
            } else {
                prop->value = std::move(value);
            }
        } else if (accept(TokenType::Assign)) {
            // Default value: { key = default }
            auto assign = std::make_unique<AssignmentPattern>();
            auto id = std::make_unique<Identifier>();
            id->name = key_name;
            assign->left = std::move(id);
            assign->right = parse_assignment_expression();
            prop->value = std::move(assign);
            prop->shorthand = true;
        } else {
            // Shorthand: { key }
            auto id = std::make_unique<Identifier>();
            id->name = key_name;
            prop->value = std::move(id);
            prop->shorthand = true;
        }

        prop->kind = "init";
        pattern->properties.push_back(std::move(prop));

        if (!accept(TokenType::Comma)) break;
    }

    expect(TokenType::RBrace);
    return pattern;
}

std::unique_ptr<AstNode> Parser::parse_if_statement() {
    // TEACHING NOTE: If/Else Parsing
    // ==============================
    // if (cond) stmt1 else stmt2
    // The else binds to the nearest if (dangling else problem).
    // Both branches are statements (can be blocks or single statements).

    expect(TokenType::If);
    expect(TokenType::LParen);
    auto stmt = std::make_unique<IfStatement>();
    stmt->test = parse_expression();
    expect(TokenType::RParen);
    stmt->consequent = parse_statement();
    if (accept(TokenType::Else)) {
        stmt->alternate = parse_statement();
    }
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_for_statement() {
    // TEACHING NOTE: For Loop Variants
    // =================================
    // for (init; test; update) {} - classic
    // for (key in obj) {} - for-in (enumerates keys)
    // for (value of iterable) {} - for-of (iterates values)
    //
    // The init part can be a variable declaration or expression.
    // for-in and for-of have special parsing: after the variable/declaration,
    // we check for in/of instead of ;.

    expect(TokenType::For);
    expect(TokenType::LParen);

    // Check for await (for await...of)
    bool is_await = false;
    if (check(TokenType::Await) && in_async) {
        advance();
        is_await = true;
    }

    // Parse init part
    std::unique_ptr<AstNode> init;
    std::string decl_kind;

    if (check(TokenType::Semicolon)) {
        // No init
    } else if (check(TokenType::Var) || check(TokenType::Let) || check(TokenType::Const)) {
        decl_kind = current.type_name();
        advance();

        auto decl = std::make_unique<VariableDeclaration>();
        decl->kind = decl_kind;

        auto declarator = std::make_unique<VariableDeclarator>();
        declarator->id = parse_binding_pattern();

        // Check for for-in / for-of
        if (check(TokenType::In)) {
            // for (let key in obj)
            advance(); // in
            auto for_in = std::make_unique<ForInStatement>();

            // Wrap the declaration
            auto var_decl = std::make_unique<VariableDeclaration>();
            var_decl->kind = decl_kind;
            var_decl->declarations.push_back(std::move(declarator));
            for_in->left = std::move(var_decl);

            for_in->right = parse_expression();
            expect(TokenType::RParen);
            for_in->body = parse_statement();
            return for_in;
        } else if (check(TokenType::Of)) {
            // for (let value of iterable)
            advance(); // of
            auto for_of = std::make_unique<ForOfStatement>();
            for_of->is_await = is_await;

            auto var_decl = std::make_unique<VariableDeclaration>();
            var_decl->kind = decl_kind;
            var_decl->declarations.push_back(std::move(declarator));
            for_of->left = std::move(var_decl);

            for_of->right = parse_assignment_expression();
            expect(TokenType::RParen);
            for_of->body = parse_statement();
            return for_of;
        }

        // Regular for: parse initializer
        if (accept(TokenType::Assign)) {
            declarator->init = parse_assignment_expression();
        }
        decl->declarations.push_back(std::move(declarator));

        // Multiple declarators
        while (accept(TokenType::Comma)) {
            auto d = std::make_unique<VariableDeclarator>();
            d->id = parse_binding_pattern();
            if (accept(TokenType::Assign)) {
                d->init = parse_assignment_expression();
            }
            decl->declarations.push_back(std::move(d));
        }

        init = std::move(decl);
    } else {
        // Expression init
        init = parse_expression();
    }

    expect(TokenType::Semicolon);

    auto for_stmt = std::make_unique<ForStatement>();
    for_stmt->init = std::move(init);

    // Test
    if (!check(TokenType::Semicolon)) {
        for_stmt->test = parse_expression();
    }
    expect(TokenType::Semicolon);

    // Update
    if (!check(TokenType::RParen)) {
        for_stmt->update = parse_expression();
    }
    expect(TokenType::RParen);

    for_stmt->body = parse_statement();
    return for_stmt;
}

std::unique_ptr<AstNode> Parser::parse_while_statement() {
    expect(TokenType::While);
    expect(TokenType::LParen);
    auto stmt = std::make_unique<WhileStatement>();
    stmt->test = parse_expression();
    expect(TokenType::RParen);
    stmt->body = parse_statement();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_do_while_statement() {
    expect(TokenType::Do);
    auto stmt = std::make_unique<DoWhileStatement>();
    stmt->body = parse_statement();
    expect(TokenType::While);
    expect(TokenType::LParen);
    stmt->test = parse_expression();
    expect(TokenType::RParen);
    consume_semicolon();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_switch_statement() {
    expect(TokenType::Switch);
    expect(TokenType::LParen);
    auto stmt = std::make_unique<SwitchStatement>();
    stmt->discriminant = parse_expression();
    expect(TokenType::RParen);
    expect(TokenType::LBrace);

    while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
        auto case_node = std::make_unique<SwitchCase>();

        if (accept(TokenType::Case)) {
            case_node->test = parse_expression();
        } else if (accept(TokenType::Default)) {
            // Default case: test is nullptr
        } else {
            error("Expected case or default");
            break;
        }

        expect(TokenType::Colon);

        while (!check(TokenType::Case) && !check(TokenType::Default) &&
               !check(TokenType::RBrace) && !check(TokenType::Eof)) {
            case_node->consequent.push_back(parse_statement());
        }

        stmt->cases.push_back(std::move(case_node));
    }

    expect(TokenType::RBrace);
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_try_statement() {
    expect(TokenType::Try);
    auto stmt = std::make_unique<TryStatement>();

    stmt->block = parse_block_statement();

    if (accept(TokenType::Catch)) {
        auto handler = std::make_unique<CatchClause>();

        // Optional catch binding: catch (e) or just catch
        if (accept(TokenType::LParen)) {
            handler->param = parse_binding_pattern();
            expect(TokenType::RParen);
        }

        handler->body = parse_block_statement();
        stmt->handler = std::move(handler);
    }

    if (accept(TokenType::Finally)) {
        stmt->finalizer = parse_block_statement();
    }

    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_return_statement() {
    expect(TokenType::Return);
    auto stmt = std::make_unique<ReturnStatement>();

    // Return without value
    if (can_insert_semicolon()) {
        consume_semicolon();
        return stmt;
    }

    stmt->argument = parse_expression();
    consume_semicolon();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_break_statement() {
    expect(TokenType::Break);
    auto stmt = std::make_unique<BreakStatement>();

    // Optional label
    if (check(TokenType::Identifier) && !can_insert_semicolon()) {
        stmt->label = current.value;
        advance();
    }

    consume_semicolon();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_continue_statement() {
    expect(TokenType::Continue);
    auto stmt = std::make_unique<ContinueStatement>();

    if (check(TokenType::Identifier) && !can_insert_semicolon()) {
        stmt->label = current.value;
        advance();
    }

    consume_semicolon();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_throw_statement() {
    expect(TokenType::Throw);
    auto stmt = std::make_unique<ThrowStatement>();
    stmt->argument = parse_expression();
    consume_semicolon();
    return stmt;
}

std::unique_ptr<AstNode> Parser::parse_function_declaration(bool is_async, bool is_generator) {
    expect(TokenType::Function);
    if (check(TokenType::Asterisk)) {
        is_generator = true;
        advance();
    }

    auto fn = std::make_unique<FunctionDeclaration>();
    fn->is_async = is_async;
    fn->is_generator = is_generator;

    if (check(TokenType::Identifier)) {
        fn->name = current.value;
        advance();
    } else {
        error("Expected function name");
    }

    bool was_async = in_async;
    bool was_generator = in_generator;
    in_async = is_async;
    in_generator = is_generator;

    fn->params = parse_parameter_list();
    fn->body = parse_function_body();

    in_async = was_async;
    in_generator = was_generator;

    return fn;
}

std::unique_ptr<AstNode> Parser::parse_class_declaration() {
    expect(TokenType::Class);

    auto cls = std::make_unique<ClassDeclaration>();

    if (check(TokenType::Identifier)) {
        cls->name = current.value;
        advance();
    } else {
        error("Expected class name");
    }

    if (accept(TokenType::Extends)) {
        cls->superclass = parse_left_hand_side_expression();
    }

    cls->body = parse_class_body();
    return cls;
}

std::unique_ptr<ClassBody> Parser::parse_class_body() {
    expect(TokenType::LBrace);
    auto body = std::make_unique<ClassBody>();

    while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
        if (accept(TokenType::Semicolon)) continue;

        bool is_static = false;
        if (check(TokenType::Static)) {
            // Check if it is "static" keyword or "static" as property name
            // If next token is ( it is a method named "static"
            if (lexer.peek().type != TokenType::LParen &&
                lexer.peek().type != TokenType::Assign &&
                lexer.peek().type != TokenType::Semicolon) {
                advance();
                is_static = true;
            }
        }

        // Check for get/set
        if (check(TokenType::Get) || check(TokenType::Set)) {
            std::string kind = current.type_name();
            // Check if it is a getter/setter or a method named "get"/"set"
            if (lexer.peek().type != TokenType::LParen &&
                lexer.peek().type != TokenType::Assign &&
                lexer.peek().type != TokenType::Semicolon) {
                advance();
                auto method = parse_method_definition(is_static);
                method->kind = kind;
                body->body.push_back(std::move(method));
                continue;
            }
        }

        auto method = parse_method_definition(is_static);
        body->body.push_back(std::move(method));
    }

    expect(TokenType::RBrace);
    return body;
}

std::unique_ptr<MethodDefinition> Parser::parse_method_definition(bool is_static) {
    auto method = std::make_unique<MethodDefinition>();
    method->static_ = is_static;
    method->kind = "method";

    // Computed key
    if (check(TokenType::LBracket)) {
        advance();
        method->computed = true;
        method->key = parse_assignment_expression();
        expect(TokenType::RBracket);
    } else if (check(TokenType::Identifier) || current.is_keyword()) {
        std::string name = current.value;
        // Check for constructor
        if (name == "constructor") {
            method->kind = "constructor";
        }
        auto id = std::make_unique<Identifier>();
        id->name = name;
        method->key = std::move(id);
        advance();
    } else if (check(TokenType::String)) {
        auto lit = std::make_unique<Literal>();
        lit->value = make_string(current.value);
        method->key = std::move(lit);
        advance();
    } else if (check(TokenType::Number)) {
        auto lit = std::make_unique<Literal>();
        lit->value = JSValue::number_val(current.number_value);
        method->key = std::move(lit);
        advance();
    } else {
        error("Expected method name");
    }

    // Generator
    if (check(TokenType::Asterisk)) {
        advance();
    }

    // Parameters and body
    auto fn = std::make_unique<FunctionExpression>();

    bool was_async = in_async;
    bool was_generator = in_generator;
    in_generator = false;

    fn->params = parse_parameter_list();
    fn->body = parse_function_body();

    in_async = was_async;
    in_generator = was_generator;

    method->value = std::move(fn);
    return method;
}

std::vector<std::unique_ptr<AstNode>> Parser::parse_parameter_list() {
    // TEACHING NOTE: Function Parameters
    // ==================================
    // Function parameters can be:
    //   - Simple: function f(a, b) {}
    //   - Default: function f(a = 10) {}
    //   - Destructuring: function f({x, y}) {}
    //   - Rest: function f(...args) {}

    std::vector<std::unique_ptr<AstNode>> params;
    expect(TokenType::LParen);

    if (!check(TokenType::RParen)) {
        do {
            if (check(TokenType::Spread)) {
                advance();
                auto rest = std::make_unique<RestElement>();
                rest->argument = parse_binding_pattern();
                params.push_back(std::move(rest));
                break; // rest must be last
            }

            auto param = parse_binding_pattern();

            // Default value
            if (accept(TokenType::Assign)) {
                auto assign = std::make_unique<AssignmentPattern>();
                assign->left = std::move(param);
                assign->right = parse_assignment_expression();
                params.push_back(std::move(assign));
            } else {
                params.push_back(std::move(param));
            }
        } while (accept(TokenType::Comma) && !check(TokenType::RParen));
    }

    expect(TokenType::RParen);
    return params;
}

std::unique_ptr<AstNode> Parser::parse_function_body() {
    return parse_block_statement();
}

// =============================================================================
// Program parsing
// =============================================================================

std::unique_ptr<Program> Parser::parse_program() {
    auto program = std::make_unique<Program>();

    while (!check(TokenType::Eof)) {
        program->body.push_back(parse_statement());
    }

    return program;
}

std::unique_ptr<AstNode> Parser::parse_expression_only() {
    return parse_expression();
}

} // namespace chinstrap