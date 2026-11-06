#pragma once

// ===========================================================================
// Yarn Spinner 3, in C++ -- the insides of rmp/dialogue.h.
//
// A port of the official compiler and virtual machine (YarnSpinnerTool/
// YarnSpinner, MIT; THIRD_PARTY_LICENSES.md): the same statements, the same
// instructions (YarnSpinner/yarn_spinner.proto's OpCode, one for one where it
// has one), the same saliency strategies, the same visit tracking. The parser
// is not ANTLR's: Yarn is a language of lines, and yarn_parse.cpp reads it a
// line at a time, by recursive descent, with a Pratt parser for expressions
// -- one function for each rule of YarnSpinnerParser.g4.
//
//   yarn_parse.cpp     source -> Script (the tree), with file:line:column errors
//   yarn_compile.cpp   Script -> Program: types checked, instructions emitted
//   yarn_vm.cpp        Program -> lines, options, commands: the machine
//   yarn_markup.cpp    a line's [markup] -> plain text and attributes
//   yarn_bytes.cpp     Program <-> bytes, for the build that embeds them
// ===========================================================================

#include <rmp/object.h> // rmp::Callback

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::yarn {

// --- values ----------------------------------------------------------------

enum class Kind : std::uint8_t { NUMBER, STRING, BOOL };

// A value of the language: Yarn's numbers are single-precision floats, and
// they print as C# prints them (2, 2.5, True).
struct Value {
    Kind kind = Kind::NUMBER;
    float number = 0;
    bool boolean = false;
    std::string text;

    static Value of(float n) { return { Kind::NUMBER, n, false, {} }; }
    static Value of(bool b) { return { Kind::BOOL, 0, b, {} }; }
    static Value of(std::string s) { return { Kind::STRING, 0, false, std::move(s) }; }

    [[nodiscard]] std::string str() const; // as a line shows it
    [[nodiscard]] float as_number() const;
    [[nodiscard]] bool as_bool() const;
    friend bool operator==(const Value &a, const Value &b);
};

// A float as C# writes it: the shortest text that reads back, no exponent
// for the numbers a game uses.
std::string number_text(float n);

// --- the tree ----------------------------------------------------------------

struct Position {
    int line = 0; // 1-based
    int column = 0; // 1-based
};

struct Expr {
    enum class Is : std::uint8_t { NUMBER, STRING, BOOL, VARIABLE, CALL, UNARY, BINARY, MEMBER };
    Is is = Is::NUMBER;
    float number = 0;
    bool boolean = false;
    std::string text; // a string's value, a variable's or a function's name, an operator
    std::string member; // MEMBER: Type.member (text is the type, empty for .member)
    std::vector<Expr> args; // operands, arguments
    Position at;
};

// A piece of a line: text as written, or an expression whose value goes in.
struct Piece {
    std::string text;
    std::optional<Expr> expr;
};

struct LineSpec {
    std::vector<Piece> pieces;
    std::optional<Expr> condition; // <<if ...>>
    bool once = false; // <<once>> / <<once if ...>>
    std::vector<std::string> tags; // every #hashtag, line: and shadow: included
    std::string id; // from #line:, else made up by the compiler
    std::string shadow; // from #shadow:
    Position at;
};

struct Stmt;
using Block = std::vector<Stmt>;

struct Item {
    LineSpec line;
    Block body;
};

struct Clause {
    std::optional<Expr> condition; // empty: else
    Block body;
};

struct EnumCase {
    std::string name;
    std::optional<Expr> raw;
    Position at;
};

struct Stmt {
    enum class Is : std::uint8_t {
        LINE, COMMAND, SET, DECLARE, ENUM, JUMP, DETOUR, RETURN, IF, ONCE, OPTIONS, LINE_GROUP,
        CALL,
    };
    Is is = Is::LINE;
    LineSpec line; // LINE; COMMAND (its pieces and tags)
    std::string name; // SET/DECLARE: the variable; ENUM: its name; JUMP/DETOUR: the node
    std::string op; // SET: = += -= *= /= %=; DECLARE: the type after `as`
    std::optional<Expr> expr; // SET/DECLARE: the value; JUMP/DETOUR: the node's name; CALL
    std::vector<Clause> clauses; // IF; ONCE (its condition, body, and else)
    std::vector<Item> items; // OPTIONS, LINE_GROUP
    std::vector<EnumCase> cases; // ENUM
    Position at;
};

struct When {
    bool always = false;
    bool once = false;
    std::optional<Expr> condition;
    Position at;
};

struct NodeSpec {
    std::string title;
    std::string subtitle;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<When> whens;
    std::vector<std::string> tags;
    bool tracked = true; // tracking: never says false
    Block body;
    Position at;
};

struct Script {
    std::string file;
    std::vector<std::string> tags; // the file's own hashtags, above every node
    std::vector<NodeSpec> nodes;
};

struct Diagnostic {
    std::string file;
    Position at;
    std::string message;
    [[nodiscard]] std::string str() const; // file:line:column: message
};

// Read one file. What could not be read is in `errors`, and the tree holds
// what could.
Script parse(std::string_view source, std::string_view file, std::vector<Diagnostic> &errors);

// --- the program -------------------------------------------------------------

enum class Op : std::uint8_t {
    JUMP_TO, // a: instruction
    PEEK_AND_JUMP, // the instruction on the stack
    RUN_LINE, // s: line id, a: substitutions on the stack
    RUN_COMMAND, // s: text with {0}.., a: substitutions
    ADD_OPTION, // s: line id, a: destination, b: substitutions, c: has a condition
    SHOW_OPTIONS,
    PUSH_STRING, // s
    PUSH_FLOAT, // f
    PUSH_BOOL, // a
    JUMP_IF_FALSE, // a: instruction (the value stays)
    POP,
    CALL_FUNC, // s: function, a: arguments
    PUSH_VARIABLE, // s
    STORE_VARIABLE, // s (the value stays)
    STOP,
    RUN_NODE, // s: node (a jump: the whole call stack unwinds)
    PEEK_AND_RUN_NODE, // the node's name on the stack
    DETOUR_TO_NODE, // s
    PEEK_AND_DETOUR_TO_NODE,
    RETURN,
    ADD_SALIENCY_CANDIDATE, // s: content id, a: complexity, b: destination (condition on stack)
    ADD_SALIENCY_CANDIDATE_FROM_NODE, // s: node, b: destination
    SELECT_SALIENCY_CANDIDATE, // pushes the destination and true, or false
    REPLACE_NODE, // the node on the stack runs in this node's place (a node group's choice)
};

struct Instruction {
    Op op = Op::STOP;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    float f = 0;
    std::string s;
};

struct ProgramNode {
    std::string name;
    std::string group; // the node group it belongs to, or empty
    std::vector<Instruction> code;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::string> tags;
    bool tracked = true;
    bool hub = false; // a node group's chooser
    // A member of a node group: its 'when' conditions as code that leaves a
    // bool, and their complexity.
    std::vector<Instruction> condition;
    int complexity = -1;
    int source_line = 0;
};

struct LineInfo {
    std::string id;
    std::string text; // as written, {0} where each substitution goes
    std::string node;
    std::vector<std::string> tags;
    std::string shadow; // the id of the line this one shadows
    int source_line = 0;
};

struct SmartVariable {
    std::string name;
    std::vector<Instruction> code; // leaves the value on the stack
};

struct Program {
    std::vector<ProgramNode> nodes;
    std::vector<LineInfo> lines;
    std::map<std::string, Value> initial; // every declared variable's start
    std::vector<SmartVariable> smart;

    [[nodiscard]] int node_index(std::string_view name) const;
    [[nodiscard]] int line_index(std::string_view id) const;
};

struct Compiled {
    Program program;
    std::vector<Diagnostic> errors;
    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// One or more files, compiled together (a jump may cross files).
struct Source {
    std::string file;
    std::string text;
};
// A function the game registers, told to the type checker: "number",
// "string", "bool" for each parameter and the result.
struct FunctionType {
    std::string name;
    std::vector<std::string> params;
    std::string result;
};
Compiled compile(std::span<const Source> sources, std::span<const FunctionType> functions = {});

// The program as bytes and back: what the embedded build writes and reads.
std::vector<std::uint8_t> to_bytes(const Program &program);
std::optional<Program> from_bytes(std::span<const std::uint8_t> bytes);

// --- markup --------------------------------------------------------------------

struct Attribute {
    std::string name;
    int position = 0; // in code points of the plain text
    int length = 0;
    std::vector<std::pair<std::string, std::string>> properties;
};

struct Marked {
    std::string text; // the plain text: markup gone, escapes resolved
    std::vector<Attribute> attributes;
};

// [b]..[/b], [name prop=value/], [/], [nomarkup], \[ \] \:, the character
// ("Name: "), and the replacement markers select, plural and ordinal, whose
// plural rules are those of `language`.
Marked parse_markup(std::string_view text, std::string_view language);

// {0}, {1}... replaced by the substitutions, once, left to right.
std::string expand(std::string_view text, std::span<const std::string> substitutions);

// --- the machine -------------------------------------------------------------

enum class Saliency : std::uint8_t {
    FIRST, // the first whose conditions pass
    BEST, // the one with the most conditions that pass
    BEST_LEAST_RECENTLY_VIEWED, // the least seen, the most specific of those (the default)
    RANDOM_BEST_LEAST_RECENTLY_VIEWED, // as above, a random one among equals
};

struct OptionEvent {
    std::string id;
    std::vector<std::string> substitutions;
    bool available = true;
    int destination = 0;
};

struct Event {
    enum class Is : std::uint8_t { NONE, LINE, OPTIONS, COMMAND, DONE };
    Is is = Is::NONE;
    std::string id; // LINE: the line's id
    std::vector<std::string> substitutions; // LINE
    std::string command; // COMMAND: its text, expanded
    std::vector<OptionEvent> options; // OPTIONS
};

// A function a script calls: it reads its arguments and writes its result.
using Function = rmp::Callback<Value &, std::span<const Value>>;

class Machine {
public:
    Machine() = default;
    explicit Machine(Program program);

    [[nodiscard]] const Program &program() const { return _program; }

    // Run from `node`'s first line; false (and nothing runs) when there is no
    // such node.
    bool start(std::string_view node);
    void advance(); // past a line or a command
    void choose(int index); // an option, by its place; -1 when none can be chosen
    void stop();
    [[nodiscard]] const Event &event() const { return _event; }
    [[nodiscard]] bool running() const { return !_frames.empty(); }
    [[nodiscard]] std::string node() const; // the node running now, or empty

    // A command's handler: called with its words, and the dialogue carries on.
    void on_command(std::string_view name, rmp::Callback<std::span<const std::string>> handler);
    void on_function(std::string_view name, Function function);

    [[nodiscard]] Value variable(std::string_view name) const; // smart ones computed
    void set_variable(std::string_view name, Value value);
    [[nodiscard]] const std::map<std::string, Value> &variables() const { return _vars; }
    void set_variables(std::map<std::string, Value> vars) { _vars = std::move(vars); }

    void set_saliency(Saliency s) { _saliency = s; }
    [[nodiscard]] int visits(std::string_view node) const;
    [[nodiscard]] bool has_content(std::string_view group);

    // What went wrong while it ran: a function nobody registered, a node not
    // there. Each said once.
    [[nodiscard]] const std::vector<std::string> &errors() const { return _errors; }

private:
    struct Frame {
        int node = 0;
        int pc = 0;
    };
    struct Candidate {
        std::string id;
        int complexity = 0;
        int destination = 0;
        bool passing = true;
    };

    void run();
    void step(const Instruction &i);
    void enter(int node);
    void leave_top();
    void leave_all();
    bool jump_to(std::string_view node, bool detour);
    Value call(const std::string &name, std::span<const Value> args);
    Value evaluate(const std::vector<Instruction> &code) const;
    Value read(std::string_view name) const;
    std::optional<Candidate> select(std::vector<Candidate> &candidates);
    void fault(std::string message);

    Program _program;
    std::vector<Frame> _frames;
    std::vector<Value> _stack;
    std::vector<OptionEvent> _options;
    std::vector<Candidate> _candidates;
    std::map<std::string, Value> _vars;
    std::map<std::string, rmp::Callback<std::span<const std::string>>, std::less<>> _commands;
    std::map<std::string, Function, std::less<>> _functions;
    Event _event;
    int _fallthrough = 0;
    bool _waiting = false;
    Saliency _saliency = Saliency::BEST_LEAST_RECENTLY_VIEWED;
    std::vector<std::string> _errors;
};

} // namespace rmp::yarn
