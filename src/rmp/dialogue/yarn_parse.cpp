// ===========================================================================
// Yarn source -> the tree (yarn.h). Ported from YarnSpinnerLexer.g4,
// YarnSpinnerParser.g4 and IndentAwareLexer.cs of the official compiler: the
// same modes -- header, body, text, command, expression, hashtag -- read a
// line at a time, because every statement of Yarn is one line, and the same
// rule for indentation: it means something only under an option (`->`) or a
// line group item (`=>`), whose body is the lines indented further.
// ===========================================================================

#include "yarn.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::yarn {

std::string Diagnostic::str() const {
    return file + ":" + std::to_string(at.line) + ":" + std::to_string(at.column) + ": " +
           message;
}

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

bool id_head(unsigned char c) { return std::isalpha(c) != 0 || c == '_' || c >= 0x80; }

bool id_char(unsigned char c) { return id_head(c) || std::isdigit(c) != 0; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (is_space(s.front()) || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (is_space(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string_view trim_right(std::string_view s) {
    while (!s.empty() && (is_space(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// ---------------------------------------------------------------------------
// Expressions: the tokens of ExpressionMode, and a Pratt parser over them.
// ---------------------------------------------------------------------------

struct Token {
    enum class Is : std::uint8_t {
        END, NUMBER, STRING, TRUE, FALSE, NULL_, VARIABLE, ID, OP, LPAREN, RPAREN, COMMA, DOT,
        AS, ERROR,
    };
    Is is = Is::END;
    std::string text;
    float number = 0;
    int column = 0; // 0-based, in the line
};

class Lexer {
public:
    Lexer(std::string_view text, int base_column) : _text(text), _base(base_column) {}

    Token next() {
        while (_at < _text.size() && is_space(_text[_at])) ++_at;
        Token t;
        t.column = _base + static_cast<int>(_at);
        if (_at >= _text.size()) return t;
        const char c = _text[_at];
        const auto rest = _text.substr(_at);
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t end = _at;
            while (end < _text.size() && std::isdigit(static_cast<unsigned char>(_text[end])) != 0) {
                ++end;
            }
            if (end + 1 < _text.size() && _text[end] == '.' &&
                std::isdigit(static_cast<unsigned char>(_text[end + 1])) != 0) {
                ++end;
                while (end < _text.size() &&
                       std::isdigit(static_cast<unsigned char>(_text[end])) != 0) {
                    ++end;
                }
            }
            t.is = Token::Is::NUMBER;
            t.text = std::string(_text.substr(_at, end - _at));
            t.number = std::strtof(t.text.c_str(), nullptr);
            _at = end;
            return t;
        }
        if (c == '"') {
            std::string out;
            std::size_t i = _at + 1;
            bool closed = false;
            while (i < _text.size()) {
                const char d = _text[i];
                if (d == '\\' && i + 1 < _text.size() && (_text[i + 1] == '"' || _text[i + 1] == '\\')) {
                    out += _text[i + 1];
                    i += 2;
                    continue;
                }
                if (d == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                out += d;
                ++i;
            }
            _at = i;
            t.is = closed ? Token::Is::STRING : Token::Is::ERROR;
            t.text = closed ? out : "a string with no closing quote";
            return t;
        }
        if (c == '$') {
            std::size_t end = _at + 1;
            while (end < _text.size() && id_char(static_cast<unsigned char>(_text[end]))) ++end;
            t.is = end > _at + 1 ? Token::Is::VARIABLE : Token::Is::ERROR;
            t.text = std::string(_text.substr(_at, end - _at));
            if (t.is == Token::Is::ERROR) t.text = "a $ with no variable name";
            _at = end;
            return t;
        }
        if (id_head(static_cast<unsigned char>(c))) {
            std::size_t end = _at;
            while (end < _text.size() && id_char(static_cast<unsigned char>(_text[end]))) ++end;
            const std::string word(_text.substr(_at, end - _at));
            _at = end;
            t.text = word;
            if (word == "true") {
                t.is = Token::Is::TRUE;
            } else if (word == "false") {
                t.is = Token::Is::FALSE;
            } else if (word == "null") {
                t.is = Token::Is::NULL_;
            } else if (word == "as") {
                t.is = Token::Is::AS;
            } else if (word == "is" || word == "eq") {
                t = op(t, "==");
            } else if (word == "neq") {
                t = op(t, "!=");
            } else if (word == "lt") {
                t = op(t, "<");
            } else if (word == "gt") {
                t = op(t, ">");
            } else if (word == "lte") {
                t = op(t, "<=");
            } else if (word == "gte") {
                t = op(t, ">=");
            } else if (word == "and") {
                t = op(t, "&&");
            } else if (word == "or") {
                t = op(t, "||");
            } else if (word == "xor") {
                t = op(t, "^");
            } else if (word == "not") {
                t = op(t, "!");
            } else if (word == "to") {
                t = op(t, "=");
            } else {
                t.is = Token::Is::ID;
            }
            return t;
        }
        for (const std::string_view two : { "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=",
                                            "/=", "%=" }) {
            if (rest.starts_with(two)) {
                _at += 2;
                return op(t, two);
            }
        }
        ++_at;
        switch (c) {
            case '(': t.is = Token::Is::LPAREN; return t;
            case ')': t.is = Token::Is::RPAREN; return t;
            case ',': t.is = Token::Is::COMMA; return t;
            case '.': t.is = Token::Is::DOT; return t;
            case '<': case '>': case '+': case '-': case '*': case '/': case '%': case '!':
            case '^': case '=':
                return op(t, std::string_view(&c, 1));
            default: break;
        }
        t.is = Token::Is::ERROR;
        t.text = std::string("an unexpected '") + c + "'";
        return t;
    }

private:
    static Token op(Token t, std::string_view text) {
        t.is = Token::Is::OP;
        t.text = std::string(text);
        return t;
    }

    std::string_view _text;
    std::size_t _at = 0;
    int _base = 0;
};

class ExprParser {
public:
    ExprParser(std::string_view text, int line, int base_column, std::vector<std::string> &problems)
        : _lexer(text, base_column), _line(line), _problems(problems) {
        _tok = _lexer.next();
    }

    std::optional<Expr> parse() {
        auto e = expression(0);
        return e;
    }

    // What is left, so a caller can parse `x as type` or say "nothing more".
    Token &peek() { return _tok; }
    Token take() {
        Token t = std::move(_tok);
        _tok = _lexer.next();
        return t;
    }
    [[nodiscard]] bool failed() const { return _failed; }

    void fail(const Token &at, const std::string &what) {
        if (_failed) return;
        _failed = true;
        _problems.push_back(std::to_string(at.column + 1) + "\x1f" + what);
    }

private:
    static int binary_precedence(const Token &t) {
        if (t.is != Token::Is::OP) return -1;
        const std::string &o = t.text;
        if (o == "&&" || o == "||" || o == "^") return 1;
        if (o == "==" || o == "!=") return 2;
        if (o == "<" || o == ">" || o == "<=" || o == ">=") return 3;
        if (o == "+" || o == "-") return 4;
        if (o == "*" || o == "/" || o == "%") return 5;
        return -1;
    }

    Position here(const Token &t) const { return { _line, t.column + 1 }; }

    std::optional<Expr> expression(int min) {
        auto left = unary();
        if (!left) return std::nullopt;
        while (true) {
            const int p = binary_precedence(_tok);
            if (p < 0 || p < min) break;
            const Token o = take();
            auto right = expression(p + 1);
            if (!right) return std::nullopt;
            Expr e;
            e.is = Expr::Is::BINARY;
            e.text = o.text;
            e.at = left->at;
            e.args.push_back(std::move(*left));
            e.args.push_back(std::move(*right));
            left = std::move(e);
        }
        return left;
    }

    std::optional<Expr> unary() {
        if (_tok.is == Token::Is::OP && (_tok.text == "-" || _tok.text == "!")) {
            const Token o = take();
            auto operand = unary();
            if (!operand) return std::nullopt;
            Expr e;
            e.is = Expr::Is::UNARY;
            e.text = o.text;
            e.at = here(o);
            e.args.push_back(std::move(*operand));
            return e;
        }
        return primary();
    }

    std::optional<Expr> primary() {
        Expr e;
        e.at = here(_tok);
        switch (_tok.is) {
            case Token::Is::LPAREN: {
                take();
                auto inner = expression(0);
                if (!inner) return std::nullopt;
                if (_tok.is != Token::Is::RPAREN) {
                    fail(_tok, "a '(' with no ')'");
                    return std::nullopt;
                }
                take();
                return inner;
            }
            case Token::Is::NUMBER:
                e.is = Expr::Is::NUMBER;
                e.number = _tok.number;
                take();
                return e;
            case Token::Is::STRING:
                e.is = Expr::Is::STRING;
                e.text = _tok.text;
                take();
                return e;
            case Token::Is::TRUE:
            case Token::Is::FALSE:
                e.is = Expr::Is::BOOL;
                e.boolean = _tok.is == Token::Is::TRUE;
                take();
                return e;
            case Token::Is::VARIABLE:
                e.is = Expr::Is::VARIABLE;
                e.text = _tok.text;
                take();
                return e;
            case Token::Is::DOT: {
                take();
                if (_tok.is != Token::Is::ID) {
                    fail(_tok, "a '.' with no member after it");
                    return std::nullopt;
                }
                e.is = Expr::Is::MEMBER;
                e.member = take().text;
                return e;
            }
            case Token::Is::ID: {
                const Token name = take();
                if (_tok.is == Token::Is::DOT) {
                    take();
                    if (_tok.is != Token::Is::ID) {
                        fail(_tok, "a '.' with no member after it");
                        return std::nullopt;
                    }
                    e.is = Expr::Is::MEMBER;
                    e.text = name.text;
                    e.member = take().text;
                    return e;
                }
                if (_tok.is != Token::Is::LPAREN) {
                    fail(name, "'" + name.text + "' is not a value: a variable starts with $, "
                                                 "and a function is called with ()");
                    return std::nullopt;
                }
                take();
                e.is = Expr::Is::CALL;
                e.text = name.text;
                if (_tok.is != Token::Is::RPAREN) {
                    while (true) {
                        auto arg = expression(0);
                        if (!arg) return std::nullopt;
                        e.args.push_back(std::move(*arg));
                        if (_tok.is == Token::Is::COMMA) {
                            take();
                            continue;
                        }
                        break;
                    }
                }
                if (_tok.is != Token::Is::RPAREN) {
                    fail(_tok, "the call to " + e.text + "() has no ')'");
                    return std::nullopt;
                }
                take();
                return e;
            }
            case Token::Is::NULL_: fail(_tok, "null is not a value in Yarn"); return std::nullopt;
            case Token::Is::END: fail(_tok, "an expression was expected"); return std::nullopt;
            case Token::Is::ERROR: fail(_tok, _tok.text); return std::nullopt;
            default: break;
        }
        fail(_tok, "an unexpected '" + _tok.text + "'");
        return std::nullopt;
    }

    Lexer _lexer;
    Token _tok;
    int _line;
    std::vector<std::string> &_problems;
    bool _failed = false;
};

// ---------------------------------------------------------------------------
// The file, a line at a time.
// ---------------------------------------------------------------------------

struct RawLine {
    std::string_view text; // without its new line
    int number = 0; // 1-based
};

int indent_of(std::string_view s) {
    int n = 0;
    for (const char c : s) {
        if (c == ' ') {
            n += 1;
        } else if (c == '\t') {
            n += 4;
        } else {
            break;
        }
    }
    return n;
}

std::size_t indent_chars(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return i;
}

// The keyword a command starts with, when it is one: followed by a space or
// the end, so <<iffy>> and <<settings>> are commands of the game's.
std::string_view keyword_of(std::string_view content) {
    content = trim(content);
    for (const std::string_view k : { "if", "elseif", "else", "endif", "set", "call", "declare",
                                      "jump", "detour", "return", "enum", "case", "endenum",
                                      "once", "endonce", "local" }) {
        if (content.starts_with(k) && (content.size() == k.size() || is_space(content[k.size()]))) {
            return k;
        }
    }
    return {};
}

class Parser {
public:
    Parser(std::string_view source, std::string_view file, std::vector<Diagnostic> &errors)
        : _file(file), _errors(errors) {
        std::size_t start = 0;
        int number = 1;
        if (source.starts_with("\xEF\xBB\xBF")) start = 3;
        while (start <= source.size()) {
            std::size_t end = source.find('\n', start);
            if (end == std::string_view::npos) end = source.size();
            std::string_view text = source.substr(start, end - start);
            if (text.ends_with('\r')) text.remove_suffix(1);
            _lines.push_back({ text, number++ });
            if (end == source.size()) break;
            start = end + 1;
        }
    }

    Script run() {
        Script script;
        script.file = std::string(_file);
        // File hashtags, then nodes.
        while (_at < _lines.size()) {
            const std::string_view t = trim(strip_comment(_lines[_at].text));
            if (t.empty()) {
                ++_at;
                continue;
            }
            if (t.front() == '#') {
                for (auto &tag : hashtags(t)) script.tags.push_back(std::move(tag));
                ++_at;
                continue;
            }
            break;
        }
        while (_at < _lines.size()) {
            const std::string_view t = trim(strip_comment(_lines[_at].text));
            if (t.empty()) {
                ++_at;
                continue;
            }
            script.nodes.push_back(node());
        }
        return script;
    }

private:
    void error(int line, int column, std::string message) {
        _errors.push_back({ std::string(_file), { line, column }, std::move(message) });
    }

    static std::string_view strip_comment(std::string_view s) {
        const std::size_t c = s.find("//");
        return c == std::string_view::npos ? s : s.substr(0, c);
    }

    static std::vector<std::string> hashtags(std::string_view s) {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < s.size()) {
            if (s[i] != '#') {
                ++i;
                continue;
            }
            std::size_t end = i + 1;
            while (end < s.size() && !is_space(s[end]) && s[end] != '#' && s[end] != '$' &&
                   s[end] != '<') {
                ++end;
            }
            if (end > i + 1) out.emplace_back(s.substr(i + 1, end - i - 1));
            i = end;
        }
        return out;
    }

    // --- a node: its headers, ---, its body, === ----------------------------

    NodeSpec node() {
        NodeSpec n;
        n.at = { _lines[_at].number, 1 };
        bool started = false;
        while (_at < _lines.size()) {
            const RawLine &raw = _lines[_at];
            const std::string_view t = trim(raw.text);
            if (t == "---") {
                ++_at;
                started = true;
                break;
            }
            if (t == "===") {
                error(raw.number, 1, "a node has to have a body: '---', its lines, then '==='");
                ++_at;
                return n;
            }
            if (t.empty() || t.starts_with("//")) {
                ++_at;
                continue;
            }
            header(n, raw);
            ++_at;
        }
        if (!started) {
            error(n.at.line, 1, "a node has to have a body: '---', its lines, then '==='");
            return n;
        }
        if (n.title.empty()) error(n.at.line, 1, "a node has to have a title: header");
        const std::size_t body_start = _at;
        std::size_t body_end = body_start;
        while (body_end < _lines.size() && trim(_lines[body_end].text) != "===") ++body_end;
        if (body_end >= _lines.size()) {
            error(_lines.back().number, 1, "the node '" + n.title + "' has no '===' to end it");
        }
        _end = body_end;
        n.body = block(-1, {});
        if (_at < _end) {
            const RawLine &raw = _lines[_at];
            error(raw.number, static_cast<int>(indent_chars(raw.text)) + 1,
                  "'" + std::string(trim(raw.text)) + "' has nothing to close");
        }
        _at = body_end + 1;
        return n;
    }

    void header(NodeSpec &n, const RawLine &raw) {
        const std::string_view t = trim(raw.text);
        const std::size_t colon = t.find(':');
        if (colon == std::string_view::npos) {
            error(raw.number, 1, "'" + std::string(t) + "' is not a header: key: value");
            return;
        }
        const std::string key(trim(t.substr(0, colon)));
        std::string_view value = trim(t.substr(colon + 1));
        if (key == "when") {
            n.whens.push_back(when(value, raw.number,
                                   static_cast<int>(raw.text.find(':')) + 2));
            n.headers.emplace_back(key, std::string(value));
            return;
        }
        value = trim(strip_comment(value));
        if (key == "title") {
            n.title = std::string(value);
            bool ok = !value.empty() && id_head(static_cast<unsigned char>(value.front()));
            for (const char c : value) ok = ok && (id_char(static_cast<unsigned char>(c)) || c == '.');
            if (!ok) error(raw.number, 1, "'" + n.title + "' is not a node title: letters, "
                                                           "digits and _, starting with a letter");
        } else if (key == "subtitle") {
            n.subtitle = std::string(value);
        } else if (key == "tags") {
            std::size_t i = 0;
            while (i < value.size()) {
                while (i < value.size() && is_space(value[i])) ++i;
                std::size_t end = i;
                while (end < value.size() && !is_space(value[end])) ++end;
                if (end > i) n.tags.emplace_back(value.substr(i, end - i));
                i = end;
            }
        } else if (key == "tracking") {
            n.tracked = value != "never";
        }
        n.headers.emplace_back(key, std::string(value));
    }

    When when(std::string_view value, int line, int column) {
        When w;
        w.at = { line, column };
        value = trim(value);
        // A comment is allowed after the expression.
        if (const std::size_t c = value.find("//"); c != std::string_view::npos) {
            value = trim(value.substr(0, c));
        }
        if (value.empty()) {
            error(line, column, "a 'when' header has to have a condition: an expression, "
                                "'always' or 'once'");
            return w;
        }
        if (value == "always") {
            w.always = true;
            return w;
        }
        if (value == "once") {
            w.once = true;
            return w;
        }
        if (value.starts_with("once") && value.size() > 4 && is_space(value[4])) {
            std::string_view rest = trim(value.substr(4));
            if (rest.starts_with("if") && (rest.size() == 2 || is_space(rest[2]))) {
                w.once = true;
                w.condition = expression(trim(rest.substr(2)), line, column);
                return w;
            }
        }
        w.condition = expression(value, line, column);
        return w;
    }

    std::optional<Expr> expression(std::string_view text, int line, int column) {
        std::vector<std::string> problems;
        ExprParser p(text, line, column - 1, problems);
        auto e = p.parse();
        if (e && !p.failed() && p.peek().is != Token::Is::END) {
            p.fail(p.peek(), "an unexpected '" + p.peek().text + "' after the expression");
        }
        report(problems, line);
        if (p.failed()) return std::nullopt;
        return e;
    }

    void report(const std::vector<std::string> &problems, int line) {
        for (const std::string &p : problems) {
            const std::size_t sep = p.find('\x1f');
            error(line, std::atoi(p.substr(0, sep).c_str()), p.substr(sep + 1));
        }
    }

    // --- the body -------------------------------------------------------------

    [[nodiscard]] bool blank(std::size_t i) const {
        return trim(strip_comment(_lines[i].text)).empty();
    }

    // The command a line is, when it is one: what is between << and >>, and
    // what follows it.
    struct CommandLine {
        std::string_view content;
        std::string_view after;
        std::size_t content_column = 0; // 0-based
        bool closed = false;
    };

    static CommandLine command_line(std::string_view text) {
        CommandLine c;
        const std::size_t open = text.find("<<");
        if (open == std::string_view::npos) return c;
        const std::string_view key = keyword_of(text.substr(open + 2));
        const bool expression_kind = !key.empty();
        std::size_t i = open + 2;
        while (i + 1 < text.size()) {
            if (text[i] == '{') {
                i = skip_braces(text, i);
                continue;
            }
            if (expression_kind && text[i] == '"') {
                ++i;
                while (i < text.size() && text[i] != '"') i += text[i] == '\\' ? 2 : 1;
                ++i;
                continue;
            }
            if (text[i] == '>' && text[i + 1] == '>') {
                c.content = text.substr(open + 2, i - open - 2);
                c.after = text.substr(i + 2);
                c.content_column = open + 2;
                c.closed = true;
                return c;
            }
            ++i;
        }
        c.content = text.substr(open + 2);
        c.content_column = open + 2;
        return c;
    }

    // Past the } that closes the { at `i`, strings inside skipped.
    static std::size_t skip_braces(std::string_view text, std::size_t i) {
        ++i;
        while (i < text.size() && text[i] != '}') {
            if (text[i] == '"') {
                ++i;
                while (i < text.size() && text[i] != '"') i += text[i] == '\\' ? 2 : 1;
            }
            ++i;
        }
        return i < text.size() ? i + 1 : i;
    }

    // Statements, until the end of the node, a line at `parent` indentation or
    // less (when parent >= 0: an option's body), or a command in `stops`.
    Block block(int parent, std::initializer_list<std::string_view> stops) {
        Block out;
        while (_at < _end) {
            if (blank(_at)) {
                ++_at;
                continue;
            }
            const RawLine &raw = _lines[_at];
            if (parent >= 0 && indent_of(raw.text) <= parent) break;
            const std::string_view t = trim(raw.text);
            if (t.starts_with("<<")) {
                const std::string_view key = keyword_of(command_line(t).content);
                bool stop = false;
                for (const std::string_view s : stops) stop = stop || key == s;
                if (stop) break;
            }
            statement(out);
        }
        return out;
    }

    void statement(Block &out) {
        const RawLine &raw = _lines[_at];
        const std::size_t indent = indent_chars(raw.text);
        const std::string_view t = raw.text.substr(indent);
        const int column = static_cast<int>(indent) + 1;
        if (t.starts_with("->") || t.starts_with("=>")) {
            out.push_back(items(t.starts_with("->") ? Stmt::Is::OPTIONS : Stmt::Is::LINE_GROUP));
            return;
        }
        if (t.starts_with("<<")) {
            command(out, raw, indent);
            return;
        }
        Stmt s;
        s.is = Stmt::Is::LINE;
        s.at = { raw.number, column };
        s.line = line_spec(t, raw.number, column);
        out.push_back(std::move(s));
        ++_at;
    }

    // A group of options, or of line group items: one after another, a blank
    // line ending the group.
    Stmt items(Stmt::Is is) {
        Stmt s;
        s.is = is;
        const std::string_view arrow = is == Stmt::Is::OPTIONS ? "->" : "=>";
        s.at = { _lines[_at].number, static_cast<int>(indent_chars(_lines[_at].text)) + 1 };
        const int group_indent = indent_of(_lines[_at].text);
        while (_at < _end) {
            const RawLine &raw = _lines[_at];
            const std::size_t indent = indent_chars(raw.text);
            const std::string_view t = raw.text.substr(indent);
            if (!t.starts_with(arrow)) break;
            // Less indented than the group: an option of the group around it.
            if (indent_of(raw.text) < group_indent) break;
            Item item;
            std::size_t start = 2;
            while (start < t.size() && is_space(t[start])) ++start;
            item.line = line_spec(t.substr(start), raw.number,
                                  static_cast<int>(indent + start) + 1);
            ++_at;
            // An option followed by a line of nothing but deeper indentation,
            // and then the end, opens a body with nothing in it.
            if (_at < _end && trim(_lines[_at].text).empty() &&
                indent_of(_lines[_at].text) > indent_of(raw.text) && _at + 1 >= _end) {
                error(_lines[_at].number, 1, "the lines indented under an option have to say "
                                             "something");
            }
            item.body = block(indent_of(raw.text), {});
            s.items.push_back(std::move(item));
            // A blank line after an option ends its group: one among the
            // comments and blank lines before the next item.
            bool ended = false;
            for (std::size_t k = _at; k > 0 && blank(k - 1) && k - 1 > 0; --k) {
                if (trim(_lines[k - 1].text).empty()) ended = true;
            }
            if (ended) break;
        }
        return s;
    }

    void command(Block &out, const RawLine &raw, std::size_t indent) {
        const std::string_view text = raw.text;
        const CommandLine c = command_line(text);
        const int line = raw.number;
        const int column = static_cast<int>(c.content_column) + 1;
        if (!c.closed) {
            error(line, static_cast<int>(indent) + 1,
                  "a command has to end with '>>' on the same line");
            ++_at;
            return;
        }
        const std::string_view key = keyword_of(c.content);
        std::string_view args = trim(c.content);
        if (!key.empty()) args = trim(args.substr(key.size()));
        const int args_column = column + static_cast<int>(c.content.find(args.empty() ? std::string_view(" ") : args));
        const std::string_view after = trim(strip_comment(c.after));
        Stmt s;
        s.at = { line, static_cast<int>(indent) + 1 };
        if (key.empty()) {
            s.is = Stmt::Is::COMMAND;
            s.line = pieces_of_command(c.content, line, column);
            for (auto &tag : hashtags(after)) s.line.tags.push_back(std::move(tag));
            out.push_back(std::move(s));
            ++_at;
            return;
        }
        if (!after.empty() && after.front() != '#') {
            error(line, static_cast<int>(text.size() - c.after.size()) + 1,
                  "nothing may follow <<" + std::string(key) + ">> on its line");
        }
        ++_at;
        if (key == "if") {
            ifs(out, s, args, line, args_column);
        } else if (key == "once") {
            once(out, s, args, line, args_column);
        } else if (key == "set") {
            set(out, s, args, line, args_column);
        } else if (key == "declare") {
            declare(out, s, args, line, args_column);
        } else if (key == "call") {
            s.is = Stmt::Is::CALL;
            s.expr = expression(args, line, args_column);
            if (s.expr && s.expr->is != Expr::Is::CALL) {
                error(line, args_column, "<<call>> takes a function call: <<call f(1)>>");
            }
            out.push_back(std::move(s));
        } else if (key == "jump" || key == "detour") {
            s.is = key == "jump" ? Stmt::Is::JUMP : Stmt::Is::DETOUR;
            if (args.starts_with("{") && args.ends_with("}")) {
                s.expr = expression(args.substr(1, args.size() - 2), line, args_column + 1);
            } else if (args.empty()) {
                error(line, args_column, "<<" + std::string(key) + ">> needs a node");
            } else {
                s.name = std::string(args);
            }
            out.push_back(std::move(s));
        } else if (key == "return") {
            s.is = Stmt::Is::RETURN;
            out.push_back(std::move(s));
        } else if (key == "enum") {
            enums(out, s, args, line, args_column);
        } else if (key == "local") {
            error(line, column, "<<local>> is not supported yet: use <<declare>>");
        } else {
            error(line, column, "<<" + std::string(key) + ">> with nothing open to close");
        }
    }

    // <<if>>, <<elseif>>..., <<else>>, <<endif>>.
    void ifs(Block &out, Stmt &s, std::string_view condition, int line, int column) {
        s.is = Stmt::Is::IF;
        Clause first;
        first.condition = expression(condition, line, column);
        first.body = block(-1, { "elseif", "else", "endif" });
        s.clauses.push_back(std::move(first));
        while (true) {
            if (_at >= _end) {
                error(line, column, "an <<if>> with no <<endif>>");
                break;
            }
            const RawLine &raw = _lines[_at];
            const CommandLine c = command_line(trim(raw.text));
            const std::string_view key = keyword_of(c.content);
            ++_at;
            if (key == "endif") break;
            Clause next;
            if (key == "elseif") {
                const std::string_view args = trim(trim(c.content).substr(6));
                next.condition = expression(args, raw.number,
                                            static_cast<int>(raw.text.find(args)) + 1);
            }
            next.body = block(-1, { "elseif", "else", "endif" });
            s.clauses.push_back(std::move(next));
        }
        out.push_back(std::move(s));
    }

    // <<once>> or <<once if ...>>, <<else>>, <<endonce>>.
    void once(Block &out, Stmt &s, std::string_view args, int line, int column) {
        s.is = Stmt::Is::ONCE;
        Clause first;
        if (!args.empty()) {
            if (!(args.starts_with("if") && (args.size() == 2 || is_space(args[2])))) {
                error(line, column, "<<once>> takes nothing, or 'if' and a condition");
            } else {
                first.condition = expression(trim(args.substr(2)), line, column + 3);
            }
        }
        first.body = block(-1, { "else", "endonce" });
        s.clauses.push_back(std::move(first));
        if (_at < _end) {
            const CommandLine c = command_line(trim(_lines[_at].text));
            if (keyword_of(c.content) == "else") {
                ++_at;
                Clause other;
                other.body = block(-1, { "endonce" });
                s.clauses.push_back(std::move(other));
            }
        }
        if (_at < _end && keyword_of(command_line(trim(_lines[_at].text)).content) == "endonce") {
            ++_at;
        } else {
            error(line, column, "a <<once>> with no <<endonce>>");
        }
        out.push_back(std::move(s));
    }

    void set(Block &out, Stmt &s, std::string_view args, int line, int column) {
        s.is = Stmt::Is::SET;
        std::vector<std::string> problems;
        ExprParser p(args, line, column - 1, problems);
        const Token var = p.take();
        if (var.is != Token::Is::VARIABLE) {
            error(line, column, "<<set>> takes a variable: <<set $x = 1>>");
            return;
        }
        s.name = var.text;
        const Token op = p.take();
        if (op.is != Token::Is::OP ||
            (op.text != "=" && op.text != "+=" && op.text != "-=" && op.text != "*=" &&
             op.text != "/=" && op.text != "%=")) {
            error(line, op.column + 1, "<<set " + s.name + ">> needs = and a value");
            return;
        }
        s.op = op.text;
        s.expr = p.parse();
        if (s.expr && !p.failed() && p.peek().is != Token::Is::END) {
            p.fail(p.peek(), "an unexpected '" + p.peek().text + "' after the value");
        }
        report(problems, line);
        if (!p.failed() && s.expr) out.push_back(std::move(s));
    }

    void declare(Block &out, Stmt &s, std::string_view args, int line, int column) {
        s.is = Stmt::Is::DECLARE;
        std::vector<std::string> problems;
        ExprParser p(args, line, column - 1, problems);
        const Token var = p.take();
        if (var.is != Token::Is::VARIABLE) {
            error(line, column, "<<declare>> takes a variable: <<declare $x = 1>>");
            return;
        }
        s.name = var.text;
        const Token op = p.take();
        if (op.is != Token::Is::OP || op.text != "=") {
            error(line, op.column + 1, "<<declare " + s.name + ">> needs = and a value");
            return;
        }
        s.expr = p.parse();
        if (!p.failed() && p.peek().is == Token::Is::AS) {
            p.take();
            const Token type = p.take();
            if (type.is != Token::Is::ID) {
                p.fail(type, "'as' needs a type: number, string, bool, or an enum");
            } else {
                s.op = type.text;
            }
        }
        if (s.expr && !p.failed() && p.peek().is != Token::Is::END) {
            p.fail(p.peek(), "an unexpected '" + p.peek().text + "' after the value");
        }
        report(problems, line);
        if (!p.failed() && s.expr) out.push_back(std::move(s));
    }

    void enums(Block &out, Stmt &s, std::string_view args, int line, int column) {
        s.is = Stmt::Is::ENUM;
        s.name = std::string(args);
        if (s.name.empty()) error(line, column, "<<enum>> needs a name");
        while (_at < _end) {
            if (blank(_at)) {
                ++_at;
                continue;
            }
            const RawLine &raw = _lines[_at];
            const CommandLine c = command_line(trim(raw.text));
            const std::string_view key = keyword_of(c.content);
            if (key == "endenum") {
                ++_at;
                if (s.cases.empty()) error(line, column, "the enum " + s.name + " has no cases");
                out.push_back(std::move(s));
                return;
            }
            if (key != "case") {
                error(raw.number, 1, "an enum holds only <<case>>s, up to its <<endenum>>");
                ++_at;
                continue;
            }
            EnumCase ec;
            const std::string_view body = trim(trim(c.content).substr(4));
            const std::size_t eq = body.find('=');
            ec.name = std::string(trim(body.substr(0, eq)));
            ec.at = { raw.number, static_cast<int>(raw.text.find(body)) + 1 };
            if (eq != std::string_view::npos) {
                ec.raw = expression(trim(body.substr(eq + 1)), raw.number, ec.at.column);
            }
            s.cases.push_back(std::move(ec));
            ++_at;
        }
        error(line, column, "an <<enum>> with no <<endenum>>");
    }

    // A command of the game's: text, with {expressions} in it.
    LineSpec pieces_of_command(std::string_view content, int line, int column) {
        LineSpec spec;
        spec.at = { line, column };
        content = trim(content);
        std::string text;
        std::size_t i = 0;
        while (i < content.size()) {
            if (content[i] == '{') {
                const std::size_t end = skip_braces(content, i);
                if (!text.empty()) spec.pieces.push_back({ std::exchange(text, {}), {} });
                Piece p;
                p.expr = expression(content.substr(i + 1, end - i - 2), line,
                                    column + static_cast<int>(i) + 1);
                spec.pieces.push_back(std::move(p));
                i = end;
                continue;
            }
            text += content[i++];
        }
        if (!text.empty()) spec.pieces.push_back({ text, {} });
        return spec;
    }

    // A line of text: its pieces, then optionally a condition and hashtags.
    LineSpec line_spec(std::string_view t, int line, int column) {
        LineSpec spec;
        spec.at = { line, column };
        std::string text;
        std::size_t i = 0;
        bool tail = false; // past the text: hashtags, a condition, a comment
        const auto flush = [&] {
            if (!text.empty()) spec.pieces.push_back({ std::exchange(text, {}), {} });
        };
        while (i < t.size()) {
            const char c = t[i];
            if (!tail && c == '\\' && i + 1 < t.size()) {
                const char next = t[i + 1];
                if (next == '[' || next == ']' || next == ':') {
                    text += '\\';
                    text += next; // for the markup parser
                } else {
                    text += next;
                }
                i += 2;
                continue;
            }
            if (t.substr(i).starts_with("//")) break;
            if (!tail && c == '{') {
                const std::size_t end = skip_braces(t, i);
                flush();
                Piece p;
                p.expr = expression(t.substr(i + 1, end - i - 2), line,
                                    column + static_cast<int>(i) + 1);
                spec.pieces.push_back(std::move(p));
                i = end;
                continue;
            }
            if (c == '#') {
                tail = true;
                std::size_t end = i + 1;
                while (end < t.size() && !is_space(t[end]) && t[end] != '#' && t[end] != '$' &&
                       t[end] != '<') {
                    ++end;
                }
                const std::string tag(t.substr(i + 1, end - i - 1));
                if (tag.starts_with("line:")) {
                    spec.id = tag;
                } else if (tag.starts_with("shadow:")) {
                    spec.shadow = "line:" + tag.substr(7);
                }
                if (!tag.empty()) spec.tags.push_back(tag);
                i = end;
                continue;
            }
            if (t.substr(i).starts_with("<<")) {
                tail = true;
                const CommandLine cl = command_line(t.substr(i));
                if (!cl.closed) {
                    error(line, column + static_cast<int>(i), "a condition has to end with '>>'");
                    break;
                }
                condition(spec, cl.content, line, column + static_cast<int>(i) + 2);
                i += cl.content.size() + 4;
                continue;
            }
            if (tail) {
                if (!is_space(c)) {
                    error(line, column + static_cast<int>(i),
                          "only hashtags and a condition may follow a line's text");
                    break;
                }
                ++i;
                continue;
            }
            text += c;
            ++i;
        }
        // The text ends before the whitespace that leads to its tags.
        if (!text.empty()) {
            const std::string_view kept = trim_right(text);
            text = std::string(kept);
        } else if (!spec.pieces.empty() && !spec.pieces.back().expr) {
            spec.pieces.back().text = std::string(trim_right(spec.pieces.back().text));
        }
        flush();
        if (!spec.pieces.empty() && !spec.pieces.back().expr) {
            spec.pieces.back().text = std::string(trim_right(spec.pieces.back().text));
        }
        return spec;
    }

    void condition(LineSpec &spec, std::string_view content, int line, int column) {
        const std::string_view key = keyword_of(content);
        const std::string_view args = trim(trim(content).substr(key.size()));
        if (key == "if") {
            if (args.empty()) {
                error(line, column, "<<if>> needs a condition");
                return;
            }
            spec.condition = expression(args, line, column + 3);
        } else if (key == "once") {
            spec.once = true;
            if (!args.empty()) {
                if (!(args.starts_with("if") && (args.size() == 2 || is_space(args[2])))) {
                    error(line, column, "<<once>> takes nothing, or 'if' and a condition");
                    return;
                }
                const std::string_view cond = trim(args.substr(2));
                if (cond.empty()) {
                    error(line, column, "<<once if>> needs a condition");
                    return;
                }
                spec.condition = expression(cond, line, column + 8);
            }
        } else {
            error(line, column, "a line may end with <<if ...>> or <<once>>, not <<" +
                                    std::string(trim(content)) + ">>");
        }
    }

    std::string_view _file;
    std::vector<Diagnostic> &_errors;
    std::vector<RawLine> _lines;
    std::size_t _at = 0;
    std::size_t _end = 0; // the === of the node being read
};

} // namespace

Script parse(std::string_view source, std::string_view file, std::vector<Diagnostic> &errors) {
    Parser p(source, file, errors);
    return p.run();
}

} // namespace rmp::yarn
