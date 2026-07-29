// =============================================================================
// js_lexer.hpp - JavaScript Lexer / Tokenizer
// =============================================================================
//
// TEACHING NOTE: Lexical Analysis
// ===============================
//
// The lexer (also called tokenizer or scanner) is the first phase of a
// compiler/interpreter. It reads the raw source code character by character
// and groups characters into meaningful units called "tokens".
//
// For example, the source code "let x = 42;" is tokenized as:
//   [LET] [IDENTIFIER("x")] [EQUALS] [NUMBER(42)] [SEMICOLON]
//
// The lexer handles:
//   1. Whitespace: spaces, tabs, newlines are mostly ignored (except for ASI)
//   2. Comments: // line comments and /* block comments */
//   3. Identifiers: variable names, following [a-zA-Z_$][a-zA-Z0-9_$]*
//   4. Keywords: reserved words like let, const, function, if, etc.
//   5. Numbers: integer, float, hex (0x), binary (0b), octal (0o), BigInt
//   6. Strings: single, double, and template literals
//   7. Regex literals: /pattern/flags
//   8. Punctuation: operators and delimiters
//   9. Automatic Semicolon Insertion (ASI)
//
// How V8 tokenizes:
// =================
// V8 lexer is in src/parsing/scanner.cc. It uses a hand-written state machine
// for performance. V8 scanner features:
//   - Streaming: scans ahead as needed, can peek and push back tokens
//   - Preparse: V8 can pre-parse (lazy parse) functions to save time
//   - Template literals: V8 tracks template literal state for nesting
//   - Regex detection: V8 uses a heuristic to decide if / is division or regex
//
// The regex/division disambiguation is the hardest part of lexing JavaScript.
// The lexer must track context: after an expression, / is division; after
// an operator or at statement start, / begins a regex literal.
//
// =============================================================================

#ifndef CHINSTRAP_JS_LEXER_HPP
#define CHINSTRAP_JS_LEXER_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace chinstrap {

// =============================================================================
// Token Types
// =============================================================================

// TEACHING NOTE: Token Categories
// ================================
// Tokens fall into categories:
//   - Keywords: reserved words with special meaning
//   - Identifiers: user-defined names
//   - Literals: numbers, strings, templates, regex
//   - Punctuation: operators and delimiters
//   - Special: EOF, ASI (automatic semicolon insertion)
//
// We use a single enum for all token types. Some parsers separate these
// into different token kinds, but a flat enum is simpler and equally effective.
// =============================================================================

enum class TokenType {
    // EOF
    Eof,

    // Literals
    Number,
    String,
    Template,
    TemplateStart,   // `...${  - start of template with interpolation
    TemplateMiddle,  // }...${  - middle part of template
    TemplateEnd,     // }...`   - end of template
    Regex,
    Identifier,
    PrivateIdentifier, // #name (class private fields)

    // Keywords
    Var,
    Let,
    Const,
    Function,
    Return,
    If,
    Else,
    For,
    While,
    Do,
    Break,
    Continue,
    Switch,
    Case,
    Default,
    Class,
    Extends,
    Super,
    This,
    New,
    Delete,
    Typeof,
    Instanceof,
    In,
    Of,
    Try,
    Catch,
    Finally,
    Throw,
    Import,
    Export,
    From,
    As,
    Async,
    Await,
    Yield,
    Static,
    Get,
    Set,
    Undefined,
    Null,
    True,
    False,
    Void,
    Debugger,

    // Punctuation
    // Arithmetic
    Plus,           // +
    Minus,          // -
    Asterisk,       // *
    Slash,          // /
    Percent,        // %
    Exponent,       // **

    // Bitwise
    Ampersand,      // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    LeftShift,      // <<
    RightShift,     // >>
    UnsignedRightShift, // >>>

    // Logical
    LogicalAnd,     // &&
    LogicalOr,      // ||
    NullishCoalescing, // ??

    // Comparison
    LessThan,       // <
    GreaterThan,    // >
    LessEqual,      // <=
    GreaterEqual,   // >=
    Equals,         // ==
    NotEquals,      // !=
    StrictEquals,   // ===
    StrictNotEquals,// !==

    // Assignment
    Assign,         // =
    PlusAssign,     // +=
    MinusAssign,    // -=
    MultiplyAssign, // *=
    DivideAssign,   // /=
    PercentAssign,  // %=
    ExponentAssign, // **=
    LeftShiftAssign, // <<=
    RightShiftAssign, // >>=
    UnsignedRightShiftAssign, // >>>=
    BitAndAssign,   // &=
    BitOrAssign,    // |=
    BitXorAssign,   // ^=
    LogicalAndAssign, // &&=
    LogicalOrAssign,  // ||=
    NullishCoalescingAssign, // ??=

    // Update
    Increment,      // ++
    Decrement,      // --

    // Punctuation
    LParen,         // (
    RParen,         // )
    LBrace,         // {
    RBrace,         // }
    LBracket,       // [
    RBracket,       // ]
    Semicolon,       // ;
    Comma,          // ,
    Dot,            // .
    Spread,         // ...
    OptionalChaining, // ?.
    Arrow,          // =>
    Colon,          // :
    QuestionMark,    // ?
    Hash,           // # (for private identifiers)
    Not,            // !

    // Special
    Error,
};

// =============================================================================
// Token
// =============================================================================

struct Token {
    TokenType type;
    std::string value;       // raw text of the token
    double number_value = 0; // for Number tokens
    int line = 0;
    int col = 0;

    // For template literals
    std::string cooked_value;
    std::string raw_value;

    // For regex
    std::string regex_flags;

    Token() : type(TokenType::Eof) {}
    Token(TokenType t) : type(t) {}
    Token(TokenType t, std::string v) : type(t), value(std::move(v)) {}

    bool is_keyword() const;
    std::string type_name() const;
};

// =============================================================================
// Lexer
// =============================================================================

class Lexer {
public:
    explicit Lexer(std::string source);

    // Get the next token
    Token next();

    // Peek at the next token without consuming it
    Token peek();

    // Peek at the token after the next one
    Token peek2();

    // Get the current token (already consumed)
    const Token& current() const { return current_token; }

    // Get source line for error messages
    std::string get_line(int line_num) const;

    // Error handling
    struct LexError {
        std::string message;
        int line;
        int col;
    };
    std::vector<LexError> errors;
    bool has_errors() const { return !errors.empty(); }

private:
    std::string source;
    size_t pos = 0;
    int line = 1;
    int col = 1;

    Token current_token;
    bool has_peek = false;
    Token peeked_token;
    bool has_peek2 = false;
    Token peeked2_token;

    // Regex/division disambiguation: track if last significant token
    // was a value (so / means division) or an operator (so / means regex)
    bool prev_token_was_value = false;

    // Template literal tracking
    int template_depth = 0; // nesting depth of template literals
    int template_brace_nesting = 0; // brace depth inside template expressions

    // Character helpers
    char peek_char(size_t offset = 0) const;
    char advance_char();
    bool match_char(char c);
    bool at_end() const { return pos >= source.length(); }

    // Token scanners
    Token scan_token();
    Token scan_number();
    Token scan_string(char quote);
    Token scan_template(bool start, bool consume_brace = true);
    Token scan_regex();
    Token scan_identifier_or_keyword();
    Token scan_punctuation();

    // Skip whitespace and comments
    void skip_whitespace_and_comments();

    // Check if a character starts an identifier
    static bool is_identifier_start(char c);
    static bool is_identifier_part(char c);
    static bool is_digit(char c);
    static bool is_hex_digit(char c);
    static bool is_octal_digit(char c);

    // Keyword lookup
    static TokenType keyword_lookup(const std::string& word);

    // ASI handling
    bool check_asi();

    // Line/col tracking helpers
    void newline() { line++; col = 1; }
};

} // namespace chinstrap

#endif // CHINSTRAP_JS_LEXER_HPP