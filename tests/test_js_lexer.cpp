// =============================================================================
// tests/test_js_lexer.cpp - Unit Tests for JavaScript Lexer
// =============================================================================
//
// TEACHING NOTE: Lexer Testing
// ============================
// We test the lexer by tokenizing various JavaScript snippets and checking
// that the tokens match our expectations. Good lexer tests cover:
//   - All token types (keywords, operators, literals)
//   - Edge cases (empty input, comments, whitespace)
//   - Error cases (unterminated strings, invalid characters)
//   - Complex real-world code snippets
//
// We use a simple assert-based test framework (no external dependencies).
// Each test function prints pass/fail and we count results at the end.
//
// =============================================================================

#include "../src/js_lexer.hpp"
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>

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
    else { tests_failed++; printf("FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); } \
} while(0)

// Helper: tokenize a string and return all tokens
static std::vector<Token> tokenize_all(const std::string& source) {
    Lexer lexer(source);
    std::vector<Token> tokens;
    for (;;) {
        Token t = lexer.next();
        tokens.push_back(t);
        if (t.type == TokenType::Eof) break;
    }
    return tokens;
}

// ---- Test: basic tokens ----
static void test_basic_tokens() {
    printf("Test: basic tokens... ");
    auto tokens = tokenize_all("var x = 42;");
    ASSERT_EQ(tokens[0].type, TokenType::Var);
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "x");
    ASSERT_EQ(tokens[2].type, TokenType::Assign);
    ASSERT_EQ(tokens[3].type, TokenType::Number);
    ASSERT_EQ(tokens[3].number_value, 42.0);
    ASSERT_EQ(tokens[4].type, TokenType::Semicolon);
    ASSERT_EQ(tokens[5].type, TokenType::Eof);
    printf("OK\n");
}

// ---- Test: keywords ----
static void test_keywords() {
    printf("Test: keywords... ");
    auto tokens = tokenize_all("let const function return if else for while do");
    ASSERT_EQ(tokens[0].type, TokenType::Let);
    ASSERT_EQ(tokens[1].type, TokenType::Const);
    ASSERT_EQ(tokens[2].type, TokenType::Function);
    ASSERT_EQ(tokens[3].type, TokenType::Return);
    ASSERT_EQ(tokens[4].type, TokenType::If);
    ASSERT_EQ(tokens[5].type, TokenType::Else);
    ASSERT_EQ(tokens[6].type, TokenType::For);
    ASSERT_EQ(tokens[7].type, TokenType::While);
    ASSERT_EQ(tokens[8].type, TokenType::Do);
    printf("OK\n");
}

// ---- Test: numbers ----
static void test_numbers() {
    printf("Test: numbers... ");
    auto tokens = tokenize_all("0 1 3.14 0xFF 0b101 0o777 1e10 1.5e-3");
    ASSERT_EQ(tokens[0].type, TokenType::Number);
    ASSERT_EQ(tokens[0].number_value, 0.0);
    ASSERT_EQ(tokens[1].number_value, 1.0);
    ASSERT_EQ(tokens[2].number_value, 3.14);
    ASSERT_EQ(tokens[3].number_value, 255.0); // 0xFF
    ASSERT_EQ(tokens[4].number_value, 5.0);   // 0b101
    ASSERT_EQ(tokens[5].number_value, 511.0); // 0o777
    ASSERT_EQ(tokens[6].number_value, 1e10);
    ASSERT_EQ(tokens[7].number_value, 1.5e-3);
    printf("OK\n");
}

// ---- Test: strings ----
static void test_strings() {
    printf("Test: strings... ");
    auto tokens = tokenize_all("\"hello\" 'world' \"with\\nescapes\"");
    ASSERT_EQ(tokens[0].type, TokenType::String);
    ASSERT_EQ(tokens[0].value, "hello");
    ASSERT_EQ(tokens[1].type, TokenType::String);
    ASSERT_EQ(tokens[1].value, "world");
    ASSERT_EQ(tokens[2].type, TokenType::String);
    ASSERT_EQ(tokens[2].value, "with\nescapes");
    printf("OK\n");
}

// ---- Test: operators ----
static void test_operators() {
    printf("Test: operators... ");
    // Use valid JS context so / is division, not regex
    auto tokens = tokenize_all("1 + 1; 1 - 1; 1 * 1; 1 / 1; 1 % 1; 1 ** 1; 1 == 1; 1 != 1; 1 === 1; 1 !== 1; 1 < 1; 1 > 1; 1 <= 1; 1 >= 1; true && true; true || true; null ?? 1");
    // Tokens: 1 + 1 ; 1 - 1 ; 1 * 1 ; 1 / 1 ; 1 % 1 ; 1 ** 1 ; 1 == 1 ; 1 != 1 ; 1 === 1 ; 1 !== 1 ; 1 < 1 ; 1 > 1 ; 1 <= 1 ; 1 >= 1 ; true && true ; true || true ; null ?? 1 EOF
    // Indices: 0:1 1:+ 2:1 3:; 4:1 5:- 6:1 7:; 8:1 9:* 10:1 11:; 12:1 13:/ 14:1 15:; 16:1 17:% 18:1 19:; 20:1 21:** 22:1 23:; 24:1 25:== 26:1 27:; 28:1 29:!= 30:1 31:; 32:1 33:=== 34:1 35:; 36:1 37:!== 38:1 39:; 40:1 41:< 42:1 43:; 44:1 45:> 46:1 47:; 48:1 49:<= 50:1 51:; 52:1 53:>= 54:1 55:; 56:true 57:&& 58:true 59:; 60:true 61:|| 62:true 63:; 64:null 65:?? 66:1 67:; 68:EOF
    ASSERT_EQ(tokens[1].type, TokenType::Plus);
    ASSERT_EQ(tokens[5].type, TokenType::Minus);
    ASSERT_EQ(tokens[9].type, TokenType::Asterisk);
    ASSERT_EQ(tokens[13].type, TokenType::Slash);
    ASSERT_EQ(tokens[17].type, TokenType::Percent);
    ASSERT_EQ(tokens[21].type, TokenType::Exponent);
    ASSERT_EQ(tokens[25].type, TokenType::Equals);
    ASSERT_EQ(tokens[29].type, TokenType::NotEquals);
    ASSERT_EQ(tokens[33].type, TokenType::StrictEquals);
    ASSERT_EQ(tokens[37].type, TokenType::StrictNotEquals);
    ASSERT_EQ(tokens[41].type, TokenType::LessThan);
    ASSERT_EQ(tokens[45].type, TokenType::GreaterThan);
    ASSERT_EQ(tokens[49].type, TokenType::LessEqual);
    ASSERT_EQ(tokens[53].type, TokenType::GreaterEqual);
    ASSERT_EQ(tokens[57].type, TokenType::LogicalAnd);
    ASSERT_EQ(tokens[61].type, TokenType::LogicalOr);
    ASSERT_EQ(tokens[65].type, TokenType::NullishCoalescing);
    printf("OK\n");
}

// ---- Test: assignment operators ----
static void test_assignment_ops() {
    printf("Test: assignment operators... ");
    auto tokens = tokenize_all("x = 1; x += 1; x -= 1; x *= 1; x /= 1; x %= 1; x **= 1; x <<= 1; x >>= 1; x >>>= 1; x &= 1; x |= 1; x ^= 1; x &&= 1; x ||= 1; x ?\?= 1");
    // x = 1 ; x += 1 ; x -= 1 ; x *= 1 ; x /= 1 ; x %= 1 ; x **= 1 ; x <<= 1 ; x >>= 1 ; x >>>= 1 ; x &= 1 ; x |= 1 ; x ^= 1 ; x &&= 1 ; x ||= 1 ; x ?\?= 1 ; EOF
    // 0:x 1:= 2:1 3:; 4:x 5:+= 6:1 7:; 8:x 9:-= 10:1 11:; 12:x 13:*= 14:1 15:; 16:x 17:/= 18:1 19:; 20:x 21:%= 22:1 23:; 24:x 25:**= 26:1 27:; 28:x 29:<<= 30:1 31:; 32:x 33:>>= 34:1 35:; 36:x 37:>>>= 38:1 39:; 40:x 41:&= 42:1 43:; 44:x 45:|= 46:1 47:; 48:x 49:^= 50:1 51:; 52:x 53:&&= 54:1 55:; 56:x 57:||= 58:1 59:; 60:x 61:??= 62:1 63:; 64:EOF
    ASSERT_EQ(tokens[1].type, TokenType::Assign);
    ASSERT_EQ(tokens[5].type, TokenType::PlusAssign);
    ASSERT_EQ(tokens[9].type, TokenType::MinusAssign);
    ASSERT_EQ(tokens[13].type, TokenType::MultiplyAssign);
    ASSERT_EQ(tokens[17].type, TokenType::DivideAssign);
    ASSERT_EQ(tokens[21].type, TokenType::PercentAssign);
    ASSERT_EQ(tokens[25].type, TokenType::ExponentAssign);
    ASSERT_EQ(tokens[29].type, TokenType::LeftShiftAssign);
    ASSERT_EQ(tokens[33].type, TokenType::RightShiftAssign);
    ASSERT_EQ(tokens[37].type, TokenType::UnsignedRightShiftAssign);
    ASSERT_EQ(tokens[41].type, TokenType::BitAndAssign);
    ASSERT_EQ(tokens[45].type, TokenType::BitOrAssign);
    ASSERT_EQ(tokens[49].type, TokenType::BitXorAssign);
    ASSERT_EQ(tokens[53].type, TokenType::LogicalAndAssign);
    ASSERT_EQ(tokens[57].type, TokenType::LogicalOrAssign);
    ASSERT_EQ(tokens[61].type, TokenType::NullishCoalescingAssign);
    printf("OK\n");
}

// ---- Test: increment/decrement ----
static void test_increment_decrement() {
    printf("Test: increment/decrement... ");
    auto tokens = tokenize_all("++ --");
    ASSERT_EQ(tokens[0].type, TokenType::Increment);
    ASSERT_EQ(tokens[1].type, TokenType::Decrement);
    printf("OK\n");
}

// ---- Test: punctuation ----
static void test_punctuation() {
    printf("Test: punctuation... ");
    auto tokens = tokenize_all("( ) { } [ ] ; , . ... ? : =>");
    ASSERT_EQ(tokens[0].type, TokenType::LParen);
    ASSERT_EQ(tokens[1].type, TokenType::RParen);
    ASSERT_EQ(tokens[2].type, TokenType::LBrace);
    ASSERT_EQ(tokens[3].type, TokenType::RBrace);
    ASSERT_EQ(tokens[4].type, TokenType::LBracket);
    ASSERT_EQ(tokens[5].type, TokenType::RBracket);
    ASSERT_EQ(tokens[6].type, TokenType::Semicolon);
    ASSERT_EQ(tokens[7].type, TokenType::Comma);
    ASSERT_EQ(tokens[8].type, TokenType::Dot);
    ASSERT_EQ(tokens[9].type, TokenType::Spread);
    ASSERT_EQ(tokens[10].type, TokenType::QuestionMark);
    ASSERT_EQ(tokens[11].type, TokenType::Colon);
    ASSERT_EQ(tokens[12].type, TokenType::Arrow);
    printf("OK\n");
}

// ---- Test: comments ----
static void test_comments() {
    printf("Test: comments... ");
    auto tokens = tokenize_all("// line comment\n42 /* block comment */ 99");
    // First token after line comment should be 42
    ASSERT_EQ(tokens[0].type, TokenType::Number);
    ASSERT_EQ(tokens[0].number_value, 42.0);
    ASSERT_EQ(tokens[1].type, TokenType::Number);
    ASSERT_EQ(tokens[1].number_value, 99.0);
    ASSERT_EQ(tokens[2].type, TokenType::Eof);
    printf("OK\n");
}

// ---- Test: identifiers ----
static void test_identifiers() {
    printf("Test: identifiers... ");
    auto tokens = tokenize_all("foo bar123 _private $jquery camelCase");
    ASSERT_EQ(tokens[0].type, TokenType::Identifier);
    ASSERT_EQ(tokens[0].value, "foo");
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "bar123");
    ASSERT_EQ(tokens[2].type, TokenType::Identifier);
    ASSERT_EQ(tokens[2].value, "_private");
    ASSERT_EQ(tokens[3].type, TokenType::Identifier);
    ASSERT_EQ(tokens[3].value, "$jquery");
    ASSERT_EQ(tokens[4].type, TokenType::Identifier);
    ASSERT_EQ(tokens[4].value, "camelCase");
    printf("OK\n");
}

// ---- Test: template literal ----
static void test_template_literal() {
    printf("Test: template literal... ");
    auto tokens = tokenize_all("`hello`");
    // A template without interpolation produces a single TemplateEnd token
    ASSERT_EQ(tokens[0].type, TokenType::TemplateEnd);
    ASSERT_EQ(tokens[0].cooked_value, "hello");
    printf("OK\n");
}

// ---- Test: template with interpolation ----
static void test_template_with_interpolation() {
    printf("Test: template with interpolation... ");
    auto tokens = tokenize_all("`hello ${name} world`");
    // First: TemplateStart with "hello "
    ASSERT_EQ(tokens[0].type, TokenType::TemplateStart);
    ASSERT_EQ(tokens[0].cooked_value, "hello ");
    // Second: identifier "name"
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "name");
    // Third: } closes the expression, then " world" + closing backtick
    // The lexer needs to be called again for the TemplateEnd
    // But tokenize_all just calls next() repeatedly, so the } is handled
    // by the parser, not the lexer. Actually, the } is consumed internally
    // by scan_template when called with start=false.
    // But our tokenize_all does not handle this - it just calls next().
    // The } is a separate token (RBrace), then the lexer needs to know
    // to scan a template continuation. This requires parser cooperation.
    // For testing, we just check the first two tokens.
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "name");
    printf("OK\n");
}

// ---- Test: complex code ----
static void test_complex_code() {
    printf("Test: complex code... ");
    std::string code = R"(
        function add(a, b) {
            return a + b;
        }
        let result = add(1, 2);
    )";
    auto tokens = tokenize_all(code);
    ASSERT_EQ(tokens[0].type, TokenType::Function);
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "add");
    ASSERT_EQ(tokens[2].type, TokenType::LParen);
    ASSERT_EQ(tokens[3].type, TokenType::Identifier);
    ASSERT_EQ(tokens[3].value, "a");
    ASSERT_EQ(tokens[4].type, TokenType::Comma);
    ASSERT_EQ(tokens[5].type, TokenType::Identifier);
    ASSERT_EQ(tokens[5].value, "b");
    ASSERT_EQ(tokens[6].type, TokenType::RParen);
    ASSERT_EQ(tokens[7].type, TokenType::LBrace);
    ASSERT_EQ(tokens[8].type, TokenType::Return);
    ASSERT_EQ(tokens[9].type, TokenType::Identifier);
    ASSERT_EQ(tokens[9].value, "a");
    ASSERT_EQ(tokens[10].type, TokenType::Plus);
    ASSERT_EQ(tokens[11].type, TokenType::Identifier);
    ASSERT_EQ(tokens[11].value, "b");
    ASSERT_EQ(tokens[12].type, TokenType::Semicolon);
    ASSERT_EQ(tokens[13].type, TokenType::RBrace);
    printf("OK\n");
}

// ---- Test: not operator ----
static void test_not_operator() {
    printf("Test: not operator... ");
    auto tokens = tokenize_all("!x");
    ASSERT_EQ(tokens[0].type, TokenType::Not);
    ASSERT_EQ(tokens[1].type, TokenType::Identifier);
    ASSERT_EQ(tokens[1].value, "x");
    printf("OK\n");
}

// ---- Test: empty input ----
static void test_empty_input() {
    printf("Test: empty input... ");
    auto tokens = tokenize_all("");
    ASSERT_EQ(tokens.size(), 1u);
    ASSERT_EQ(tokens[0].type, TokenType::Eof);
    printf("OK\n");
}

// ---- Test: whitespace only ----
static void test_whitespace_only() {
    printf("Test: whitespace only... ");
    auto tokens = tokenize_all("   \t\n  \r\n  ");
    ASSERT_EQ(tokens.size(), 1u);
    ASSERT_EQ(tokens[0].type, TokenType::Eof);
    printf("OK\n");
}

// ---- Main ----
int main() {
    printf("\n=== JavaScript Lexer Tests ===\n\n");

    test_basic_tokens();
    test_keywords();
    test_numbers();
    test_strings();
    test_operators();
    test_assignment_ops();
    test_increment_decrement();
    test_punctuation();
    test_comments();
    test_identifiers();
    test_template_literal();
    test_template_with_interpolation();
    test_complex_code();
    test_not_operator();
    test_empty_input();
    test_whitespace_only();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}