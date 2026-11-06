// ===========================================================================
// The tree -> a Program. Ported from Compiler.cs, NodeGroupCompiler.cs,
// CodeGenerationVisitor.cs and TypeSolver/ of the official compiler: the
// declarations and enums gathered first, the types of what is not declared
// inferred from where it is used, then each node's instructions -- options,
// line groups and node groups chosen by saliency, <<once>> by a variable of
// its own, a node's visits counted when it is left.
// ===========================================================================

#include "yarn.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rmp::yarn {

int Program::node_index(std::string_view name) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Program::line_index(std::string_view id) const {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

namespace {

constexpr std::string_view NUMBER = "number";
constexpr std::string_view STRING = "string";
constexpr std::string_view BOOL = "bool";

struct Signature {
    std::vector<std::string> params; // "" = any
    std::string result;
};

const std::map<std::string, Signature, std::less<>> &builtins() {
    static const std::map<std::string, Signature, std::less<>> table{
        { "visited", { { "string" }, "bool" } },
        { "visited_count", { { "string" }, "number" } },
        { "has_any_content", { { "string" }, "bool" } },
        { "random", { {}, "number" } },
        { "random_range", { { "number", "number" }, "number" } },
        { "random_range_float", { { "number", "number" }, "number" } },
        { "dice", { { "number" }, "number" } },
        { "min", { { "number", "number" }, "number" } },
        { "max", { { "number", "number" }, "number" } },
        { "round", { { "number" }, "number" } },
        { "round_places", { { "number", "number" }, "number" } },
        { "floor", { { "number" }, "number" } },
        { "ceil", { { "number" }, "number" } },
        { "inc", { { "number" }, "number" } },
        { "dec", { { "number" }, "number" } },
        { "decimal", { { "number" }, "number" } },
        { "int", { { "number" }, "number" } },
        { "string", { { "" }, "string" } },
        { "number", { { "" }, "number" } },
        { "bool", { { "" }, "bool" } },
        { "format_invariant", { { "number" }, "string" } },
        { "format", { { "string", "" }, "string" } },
        // The test suite's own, which the official runner registers.
        { "assert", { { "bool" }, "bool" } },
        { "dummy_bool", { {}, "bool" } },
        { "dummy_number", { {}, "number" } },
        { "dummy_string", { {}, "string" } },
    };
    return table;
}

struct EnumDef {
    std::string name;
    std::vector<std::pair<std::string, Value>> cases;
    Kind raw = Kind::NUMBER;
};

struct VarInfo {
    std::string type;
    bool declared = false;
    bool smart = false;
    std::optional<Expr> init;
    Position at;
    std::string file;
};

int operator_count(const Expr &e) {
    int n = e.is == Expr::Is::BINARY && (e.text == "&&" || e.text == "||" || e.text == "^") ? 1 : 0;
    for (const Expr &a : e.args) n += operator_count(a);
    return n;
}

std::string stem_of(std::string_view file) {
    std::string_view s = file;
    if (const std::size_t slash = s.find_last_of("/\\"); slash != std::string_view::npos) {
        s = s.substr(slash + 1);
    }
    if (const std::size_t dot = s.rfind('.'); dot != std::string_view::npos) s = s.substr(0, dot);
    std::string out;
    for (const char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    return out;
}

class Compiler {
public:
    Compiler(std::span<const Source> sources, std::span<const FunctionType> functions)
        : _sources(sources) {
        for (const FunctionType &f : functions) _functions[f.name] = Signature{ f.params, f.result };
    }

    Compiled run() {
        for (const Source &s : _sources) {
            _scripts.push_back(parse(s.text, s.file, _out.errors));
        }
        gather();
        if (_out.errors.empty()) infer();
        if (_out.errors.empty()) emit_all();
        return std::move(_out);
    }

private:
    void error(const std::string &file, Position at, std::string message) {
        _out.errors.push_back({ file, at, std::move(message) });
    }

    // --- pass 1: nodes, groups, enums, declarations ---------------------------

    void gather() {
        std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> by_title;
        for (std::size_t f = 0; f < _scripts.size(); ++f) {
            for (std::size_t n = 0; n < _scripts[f].nodes.size(); ++n) {
                by_title[_scripts[f].nodes[n].title].emplace_back(f, n);
            }
        }
        for (auto &[title, members] : by_title) {
            if (title.empty()) continue;
            bool any_when = false;
            bool all_when = true;
            for (auto [f, n] : members) {
                const bool w = !_scripts[f].nodes[n].whens.empty();
                any_when = any_when || w;
                all_when = all_when && w;
            }
            if (members.size() > 1 && !any_when) {
                const auto [f, n] = members[1];
                error(_scripts[f].file, _scripts[f].nodes[n].at,
                      "more than one node is called '" + title + "'");
                continue;
            }
            if (any_when && !all_when) {
                for (auto [f, n] : members) {
                    if (_scripts[f].nodes[n].whens.empty()) {
                        error(_scripts[f].file, _scripts[f].nodes[n].at,
                              "the node '" + title + "' shares its title with a node group: "
                              "every node of a group has a 'when' header");
                    }
                }
                continue;
            }
            if (any_when) {
                std::set<std::string> subtitles;
                for (auto [f, n] : members) {
                    const std::string &sub = _scripts[f].nodes[n].subtitle;
                    if (!sub.empty() && !subtitles.insert(sub).second) {
                        error(_scripts[f].file, _scripts[f].nodes[n].at,
                              "two nodes of the group '" + title + "' have the subtitle '" + sub +
                                  "'");
                    }
                }
                _groups[title] = members;
            }
        }
        for (const Script &s : _scripts) {
            for (const NodeSpec &n : s.nodes) gather_block(s.file, n.body);
        }
        // Enum raw values.
        for (auto &[name, def] : _enums) (void)name, (void)def;
    }

    void gather_block(const std::string &file, const Block &block) {
        for (const Stmt &st : block) {
            switch (st.is) {
                case Stmt::Is::ENUM: enum_def(file, st); break;
                case Stmt::Is::DECLARE: declaration(file, st); break;
                default: break;
            }
            for (const Clause &c : st.clauses) gather_block(file, c.body);
            for (const Item &i : st.items) gather_block(file, i.body);
        }
    }

    void enum_def(const std::string &file, const Stmt &st) {
        if (_enums.contains(st.name)) {
            error(file, st.at, "the enum " + st.name + " is declared twice");
            return;
        }
        EnumDef def;
        def.name = st.name;
        std::set<std::string> names;
        bool any_raw = false;
        bool all_raw = true;
        std::optional<Kind> kind;
        for (const EnumCase &c : st.cases) {
            if (!names.insert(c.name).second) {
                error(file, c.at, "the enum " + st.name + " has the case " + c.name + " twice");
            }
            any_raw = any_raw || c.raw.has_value();
            all_raw = all_raw && c.raw.has_value();
            if (c.raw) {
                if (c.raw->is == Expr::Is::NUMBER) {
                    if (kind && *kind != Kind::NUMBER) {
                        error(file, c.at, "the raw values of " + st.name + " are not all one type");
                    }
                    kind = Kind::NUMBER;
                } else if (c.raw->is == Expr::Is::STRING) {
                    if (kind && *kind != Kind::STRING) {
                        error(file, c.at, "the raw values of " + st.name + " are not all one type");
                    }
                    kind = Kind::STRING;
                } else if (c.raw->is == Expr::Is::MEMBER) {
                    error(file, c.at, "a raw value cannot be an enum's case");
                } else {
                    error(file, c.at, "a raw value has to be a number or a string written out");
                }
            }
        }
        def.raw = kind.value_or(Kind::NUMBER);
        if (any_raw && !all_raw && def.raw == Kind::STRING) {
            error(file, st.at, "every case of " + st.name + " needs its string raw value");
        }
        float next = 0;
        std::set<std::string> raws;
        for (const EnumCase &c : st.cases) {
            Value v;
            if (c.raw && c.raw->is == Expr::Is::STRING) {
                v = Value::of(c.raw->text);
            } else if (c.raw && c.raw->is == Expr::Is::NUMBER) {
                v = Value::of(c.raw->number);
                next = c.raw->number + 1;
            } else if (def.raw == Kind::STRING) {
                v = Value::of(c.name);
            } else {
                v = Value::of(next);
                next += 1;
            }
            if (!raws.insert(v.str()).second) {
                error(file, c.at, "two cases of " + st.name + " have the raw value " + v.str());
            }
            def.cases.emplace_back(c.name, v);
        }
        _enums[st.name] = std::move(def);
    }

    void declaration(const std::string &file, const Stmt &st) {
        if (_vars.contains(st.name) && _vars[st.name].declared) {
            error(file, st.at, st.name + " is declared twice");
            return;
        }
        VarInfo &v = _vars[st.name];
        v.declared = true;
        v.init = st.expr;
        v.at = st.at;
        v.file = file;
        v.smart = st.expr && !constant(*st.expr);
        if (!st.op.empty()) {
            const std::string &t = st.op;
            if (t == "number" || t == "string" || t == "bool" || _enums.contains(t)) {
                v.type = t;
            } else {
                error(file, st.at, "'" + t + "' is not a type: number, string, bool or an enum");
            }
        }
    }

    static bool constant(const Expr &e) {
        if (e.is == Expr::Is::VARIABLE || e.is == Expr::Is::CALL) return false;
        for (const Expr &a : e.args) {
            if (!constant(a)) return false;
        }
        return true;
    }

    // --- pass 2: types ---------------------------------------------------------

    struct Ctx {
        std::string file;
        bool report = false;
    };

    // The type of `e`, or "" when nothing says yet; `want` is what the place it
    // is in expects, which an untyped variable or member takes.
    std::string type_of(const Expr &e, const std::string &want, const Ctx &ctx) {
        switch (e.is) {
            case Expr::Is::NUMBER: return std::string(NUMBER);
            case Expr::Is::STRING: return std::string(STRING);
            case Expr::Is::BOOL: return std::string(BOOL);
            case Expr::Is::VARIABLE: {
                VarInfo &v = _vars[e.text];
                if (v.file.empty()) {
                    v.file = ctx.file;
                    v.at = e.at;
                }
                if (v.type.empty() && !want.empty()) {
                    v.type = want;
                    _changed = true;
                }
                return v.type;
            }
            case Expr::Is::MEMBER: return member_type(e, want, ctx);
            case Expr::Is::UNARY: {
                const std::string t =
                    type_of(e.args[0], e.text == "!" ? std::string(BOOL) : std::string(NUMBER), ctx);
                const std::string need = e.text == "!" ? std::string(BOOL) : std::string(NUMBER);
                if (ctx.report && !t.empty() && t != need) {
                    error(ctx.file, e.at, "'" + e.text + "' takes a " + need + ", not a " + t);
                }
                return need;
            }
            case Expr::Is::BINARY: return binary_type(e, want, ctx);
            case Expr::Is::CALL: return call_type(e, want, ctx);
        }
        return {};
    }

    std::string member_type(const Expr &e, const std::string &want, const Ctx &ctx) {
        if (!e.text.empty()) {
            const auto it = _enums.find(e.text);
            if (it == _enums.end()) {
                if (ctx.report) error(ctx.file, e.at, "there is no enum called " + e.text);
                return {};
            }
            if (!has_case(it->second, e.member)) {
                if (ctx.report) error(ctx.file, e.at, e.text + " has no case " + e.member);
                return {};
            }
            return e.text;
        }
        if (!want.empty() && _enums.contains(want)) {
            if (has_case(_enums[want], e.member)) return want;
        }
        std::vector<std::string> owners;
        for (const auto &[name, def] : _enums) {
            if (has_case(def, e.member)) owners.push_back(name);
        }
        if (owners.size() == 1) return owners[0];
        if (ctx.report) {
            error(ctx.file, e.at,
                  owners.empty() ? "no enum has the case ." + e.member
                                 : "." + e.member + " could be a case of more than one enum: "
                                                    "write which, like " +
                                       owners[0] + "." + e.member);
        }
        return {};
    }

    static bool has_case(const EnumDef &def, const std::string &name) {
        return std::ranges::any_of(def.cases, [&](const auto &c) { return c.first == name; });
    }

    [[nodiscard]] bool is_enum(const std::string &t) const { return _enums.contains(t); }

    std::string binary_type(const Expr &e, const std::string &want, const Ctx &ctx) {
        const std::string &o = e.text;
        if (o == "&&" || o == "||" || o == "^") {
            for (const Expr &a : e.args) {
                const std::string t = type_of(a, std::string(BOOL), ctx);
                if (ctx.report && !t.empty() && t != BOOL) {
                    error(ctx.file, a.at, "'" + o + "' takes bools, not a " + t);
                }
            }
            return std::string(BOOL);
        }
        // Each side takes the type the other says.
        std::string left = type_of(e.args[0], {}, ctx);
        std::string right = type_of(e.args[1], left, ctx);
        if (left.empty() && !right.empty()) left = type_of(e.args[0], right, ctx);
        if (o == "==" || o == "!=") {
            if (ctx.report && !left.empty() && !right.empty() && left != right) {
                error(ctx.file, e.at, "a " + left + " cannot be compared with a " + right);
            }
            return std::string(BOOL);
        }
        if (o == "<" || o == ">" || o == "<=" || o == ">=") {
            const std::string t = left.empty() ? right : left;
            if (left.empty()) (void)type_of(e.args[0], std::string(NUMBER), ctx);
            if (right.empty()) (void)type_of(e.args[1], std::string(NUMBER), ctx);
            if (ctx.report && ((!left.empty() && left != NUMBER) || (!right.empty() && right != NUMBER))) {
                error(ctx.file, e.at, "'" + o + "' compares numbers");
            }
            (void)t;
            return std::string(BOOL);
        }
        // + - * / %
        std::string t = left.empty() ? right : left;
        if (t.empty() && !want.empty()) t = want;
        if (t.empty() && o != "+") t = std::string(NUMBER);
        if (!t.empty()) {
            if (left.empty()) left = type_of(e.args[0], t, ctx);
            if (right.empty()) right = type_of(e.args[1], t, ctx);
        }
        if (ctx.report) {
            if (!left.empty() && !right.empty() && left != right) {
                error(ctx.file, e.at, "'" + o + "' needs two of one type, not a " + left +
                                          " and a " + right);
            } else if (o == "+" && !t.empty() && t != NUMBER && t != STRING) {
                error(ctx.file, e.at, "'+' adds numbers or joins strings, not " + t + "s");
            } else if (o != "+" && !t.empty() && t != NUMBER) {
                error(ctx.file, e.at, "'" + o + "' takes numbers, not " + t + "s");
            }
        }
        return t;
    }

    std::string call_type(const Expr &e, const std::string &want, const Ctx &ctx) {
        const auto b = builtins().find(e.text);
        Signature *known = nullptr;
        Signature found;
        if (b != builtins().end()) {
            found = b->second;
            known = &found;
        } else if (auto f = _functions.find(e.text); f != _functions.end()) {
            known = &f->second;
        }
        if (known == nullptr) {
            Signature s;
            for (const Expr &a : e.args) s.params.push_back(type_of(a, {}, ctx));
            s.result = want;
            _functions[e.text] = s;
            _changed = true;
            return want;
        }
        if (known->params.size() != e.args.size()) {
            if (ctx.report) {
                error(ctx.file, e.at, e.text + "() takes " + std::to_string(known->params.size()) +
                                          " argument(s), not " + std::to_string(e.args.size()));
            }
            return known->result;
        }
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            const std::string need = known->params[i];
            const std::string got = type_of(e.args[i], need, ctx);
            if (need.empty() && !got.empty() && b == builtins().end()) {
                known->params[i] = got;
                _changed = true;
            } else if (ctx.report && !need.empty() && !got.empty() && got != need &&
                       !(need == STRING && is_enum(got) && _enums[got].raw == Kind::STRING)) {
                error(ctx.file, e.args[i].at, e.text + "() takes a " + need + " here, not a " + got);
            }
        }
        if (known->result.empty() && !want.empty()) {
            known->result = want;
            _changed = true;
        } else if (ctx.report && b == builtins().end() && !known->result.empty() && !want.empty() &&
                   known->result != want) {
            error(ctx.file, e.at, e.text + "() is used as a " + known->result + " and as a " + want);
        }
        return known->result;
    }

    void infer_block(const std::string &file, const Block &block, bool report) {
        const Ctx ctx{ file, report };
        const auto line = [&](const LineSpec &l) {
            for (const Piece &p : l.pieces) {
                if (p.expr) (void)type_of(*p.expr, {}, ctx);
            }
            if (l.condition) condition(*l.condition, ctx);
        };
        for (const Stmt &st : block) {
            switch (st.is) {
                case Stmt::Is::LINE:
                case Stmt::Is::COMMAND: line(st.line); break;
                case Stmt::Is::SET: {
                    VarInfo &v = _vars[st.name];
                    if (v.file.empty()) {
                        v.file = file;
                        v.at = st.at;
                    }
                    if (v.smart && report) {
                        error(file, st.at, st.name + " is a smart variable: it cannot be set");
                    }
                    const std::string want = st.op == "=" ? v.type : std::string(NUMBER);
                    const std::string got = type_of(*st.expr, v.type.empty() ? want : v.type, ctx);
                    if (v.type.empty() && !got.empty()) {
                        v.type = st.op == "+=" ? got : (st.op == "=" ? got : std::string(NUMBER));
                        _changed = true;
                    }
                    if (report && !v.type.empty() && !got.empty() && got != v.type) {
                        error(file, st.at, st.name + " is a " + v.type + ", and this sets it to a " +
                                               got);
                    }
                    if (report && st.op != "=" && st.op != "+=" && !v.type.empty() &&
                        v.type != NUMBER) {
                        error(file, st.at, "'" + st.op + "' takes a number");
                    }
                    break;
                }
                case Stmt::Is::DECLARE: break;
                case Stmt::Is::JUMP:
                case Stmt::Is::DETOUR:
                    if (st.expr) {
                        const std::string t = type_of(*st.expr, std::string(STRING), ctx);
                        if (report && !t.empty() && t != STRING) {
                            error(file, st.at, "a jump goes to a node by its name, a string, "
                                               "not a " + t);
                        }
                    }
                    break;
                case Stmt::Is::CALL: (void)type_of(*st.expr, {}, ctx); break;
                case Stmt::Is::IF:
                case Stmt::Is::ONCE:
                    for (const Clause &c : st.clauses) {
                        if (c.condition) condition(*c.condition, ctx);
                        infer_block(file, c.body, report);
                    }
                    break;
                case Stmt::Is::OPTIONS:
                case Stmt::Is::LINE_GROUP:
                    for (const Item &i : st.items) {
                        line(i.line);
                        infer_block(file, i.body, report);
                    }
                    break;
                default: break;
            }
        }
    }

    void condition(const Expr &e, const Ctx &ctx) {
        const std::string t = type_of(e, std::string(BOOL), ctx);
        if (ctx.report && !t.empty() && t != BOOL) {
            error(ctx.file, e.at, "a condition has to be a bool, not a " + t);
        }
    }

    void infer() {
        // Declarations first: their values' types.
        for (int round = 0; round < 8; ++round) {
            _changed = false;
            for (auto &[name, v] : _vars) {
                if (!v.declared || !v.init) continue;
                const std::string t = type_of(*v.init, v.type, Ctx{ v.file, false });
                if (v.type.empty() && !t.empty()) {
                    v.type = t;
                    _changed = true;
                }
            }
            for (const Script &s : _scripts) {
                for (const NodeSpec &n : s.nodes) {
                    for (const When &w : n.whens) {
                        if (w.condition) condition(*w.condition, Ctx{ s.file, false });
                    }
                    infer_block(s.file, n.body, false);
                }
            }
            if (!_changed) break;
        }
        // Then once more, saying what is wrong.
        for (auto &[name, v] : _vars) {
            if (v.declared && v.init) {
                const std::string t = type_of(*v.init, v.type, Ctx{ v.file, true });
                if (!v.type.empty() && !t.empty() && t != v.type) {
                    error(v.file, v.at, name + " is declared a " + v.type + " and given a " + t);
                }
            }
        }
        for (const Script &s : _scripts) {
            for (const NodeSpec &n : s.nodes) {
                for (const When &w : n.whens) {
                    if (w.condition) condition(*w.condition, Ctx{ s.file, true });
                }
                infer_block(s.file, n.body, true);
            }
        }
        for (const auto &[name, v] : _vars) {
            if (name.starts_with("$Yarn.Internal")) continue;
            if (v.type.empty()) {
                error(v.file, v.at, "nothing says what type " + name + " is: <<declare " + name +
                                        " = ...>> it, or use it where its type is known");
            }
        }
        // A function of the game's whose result nothing types is fine where
        // any value goes -- a line's {f()}, a command -- since it is registered
        // after the script is compiled; a variable it sets must still be typed.
        smart_loops();
        // Initial values: declared, or the default of the type.
        for (const auto &[name, v] : _vars) {
            if (v.smart) continue;
            Value init;
            if (v.init) {
                init = constant_value(*v.init, v.type);
            } else if (v.type == STRING) {
                init = Value::of(std::string());
            } else if (v.type == BOOL) {
                init = Value::of(false);
            } else if (is_enum(v.type)) {
                init = _enums[v.type].cases.front().second;
            } else {
                init = Value::of(0.0f);
            }
            _out.program.initial[name] = init;
        }
    }

    void smart_loops() {
        for (const auto &[name, v] : _vars) {
            if (!v.smart) continue;
            std::set<std::string> path;
            if (loops(name, path)) {
                error(v.file, v.at, "the smart variable " + name + " depends on itself");
            }
        }
    }

    bool loops(const std::string &name, std::set<std::string> &path) {
        if (path.contains(name)) return true;
        const auto it = _vars.find(name);
        if (it == _vars.end() || !it->second.smart || !it->second.init) return false;
        path.insert(name);
        std::set<std::string> refs;
        collect_vars(*it->second.init, refs);
        for (const std::string &r : refs) {
            if (loops(r, path)) return true;
        }
        path.erase(name);
        return false;
    }

    static void collect_vars(const Expr &e, std::set<std::string> &out) {
        if (e.is == Expr::Is::VARIABLE) out.insert(e.text);
        for (const Expr &a : e.args) collect_vars(a, out);
    }

    Value constant_value(const Expr &e, const std::string &type) {
        switch (e.is) {
            case Expr::Is::NUMBER: return Value::of(e.number);
            case Expr::Is::STRING: return Value::of(e.text);
            case Expr::Is::BOOL: return Value::of(e.boolean);
            case Expr::Is::MEMBER: return member_value(e, type);
            case Expr::Is::UNARY: {
                const Value v = constant_value(e.args[0], type);
                return e.text == "-" ? Value::of(-v.as_number()) : Value::of(!v.as_bool());
            }
            case Expr::Is::BINARY: {
                const Value a = constant_value(e.args[0], type);
                const Value b = constant_value(e.args[1], type);
                const std::string &o = e.text;
                if (o == "+") {
                    return a.kind == Kind::STRING ? Value::of(a.text + b.text)
                                                  : Value::of(a.number + b.number);
                }
                if (o == "-") return Value::of(a.number - b.number);
                if (o == "*") return Value::of(a.number * b.number);
                if (o == "/") return Value::of(a.number / b.number);
                if (o == "%") return Value::of(std::fmod(a.number, b.number));
                if (o == "==") return Value::of(a == b);
                if (o == "!=") return Value::of(!(a == b));
                if (o == "<") return Value::of(a.number < b.number);
                if (o == ">") return Value::of(a.number > b.number);
                if (o == "<=") return Value::of(a.number <= b.number);
                if (o == ">=") return Value::of(a.number >= b.number);
                if (o == "&&") return Value::of(a.boolean && b.boolean);
                if (o == "||") return Value::of(a.boolean || b.boolean);
                if (o == "^") return Value::of(a.boolean != b.boolean);
                return a;
            }
            default: return Value::of(0.0f);
        }
    }

    Value member_value(const Expr &e, const std::string &want) {
        std::string type = e.text;
        if (type.empty()) type = member_type(e, want, Ctx{ {}, false });
        const auto it = _enums.find(type);
        if (it == _enums.end()) return Value::of(0.0f);
        for (const auto &[name, v] : it->second.cases) {
            if (name == e.member) return v;
        }
        return Value::of(0.0f);
    }

    // --- pass 3: instructions ------------------------------------------------

    struct Emitter {
        std::vector<Instruction> *code = nullptr;
        int here() const { return static_cast<int>(code->size()); }
        Instruction &add(Op op) {
            code->push_back(Instruction{ op });
            return code->back();
        }
    };

    void emit_all() {
        std::set<std::string> taken;
        for (const Script &s : _scripts) {
            for (const NodeSpec &n : s.nodes) {
                const bool member = _groups.contains(n.title);
                std::string name = n.title;
                if (member) {
                    name = n.title + "." + (n.subtitle.empty() ? stem_of(s.file) + "_" +
                                                                     std::to_string(n.at.line)
                                                               : n.subtitle);
                }
                if (!taken.insert(name).second) {
                    error(s.file, n.at, "more than one node is called '" + name + "'");
                    continue;
                }
                ProgramNode out;
                out.name = name;
                out.group = member ? n.title : std::string();
                out.headers = n.headers;
                out.tags = n.tags;
                out.tracked = n.tracked;
                out.source_line = n.at.line;
                _node = name;
                _file = s.file;
                _line_counter = 0;
                Emitter em{ &out.code };
                if (member) member_conditions(out, n);
                // A node 'when: once' marks itself seen as it starts.
                for (const When &w : n.whens) {
                    if (w.once) {
                        em.add(Op::PUSH_BOOL).a = 1;
                        em.add(Op::STORE_VARIABLE).s = once_node_var(name);
                        em.add(Op::POP);
                    }
                }
                block(em, n.body);
                // The end of a node: back to the node that detoured here, or
                // the end of the dialogue.
                em.add(Op::RETURN);
                _out.program.nodes.push_back(std::move(out));
            }
        }
        // A node group's chooser, under the group's own title.
        for (const auto &[title, members] : _groups) {
            if (!taken.insert(title).second) {
                const auto [f, n] = members[0];
                error(_scripts[f].file, _scripts[f].nodes[n].at,
                      "a node and a node group are both called '" + title + "'");
                continue;
            }
            ProgramNode hub;
            hub.name = title;
            hub.hub = true;
            // The group is counted as visited only when no member says never.
            hub.tracked = std::ranges::all_of(members, [&](const auto &m) {
                return _scripts[m.first].nodes[m.second].tracked;
            });
            Emitter em{ &hub.code };
            for (const ProgramNode &n : _out.program.nodes) {
                if (n.group != title) continue;
                em.add(Op::ADD_SALIENCY_CANDIDATE_FROM_NODE).s = n.name;
            }
            em.add(Op::SELECT_SALIENCY_CANDIDATE);
            Instruction &none = em.add(Op::JUMP_IF_FALSE);
            const std::size_t none_at = hub.code.size() - 1;
            em.add(Op::POP);
            em.add(Op::REPLACE_NODE);
            hub.code[none_at].a = em.here();
            (void)none;
            em.add(Op::POP);
            em.add(Op::RETURN);
            _out.program.nodes.push_back(std::move(hub));
        }
        // Smart variables: their expressions, to be run when read.
        for (const auto &[name, v] : _vars) {
            if (!v.smart || !v.init) continue;
            SmartVariable sv;
            sv.name = name;
            Emitter em{ &sv.code };
            _want = v.type;
            expr(em, *v.init);
            _out.program.smart.push_back(std::move(sv));
        }
        shadows();
    }

    static std::string once_node_var(const std::string &node) { return "$Yarn.Internal.Once." + node; }

    void member_conditions(ProgramNode &out, const NodeSpec &n) {
        Emitter em{ &out.condition };
        int complexity = 0;
        bool first = true;
        for (const When &w : n.whens) {
            int count = 0;
            if (w.always) {
                em.add(Op::PUSH_BOOL).a = 1;
            } else {
                if (w.once) {
                    em.add(Op::PUSH_VARIABLE).s = once_node_var(out.name);
                    em.add(Op::CALL_FUNC).s = "!";
                    em.code->back().a = 1;
                    count += 1;
                }
                if (w.condition) {
                    _want = std::string(BOOL);
                    expr(em, *w.condition);
                    count += operator_count(*w.condition) + 1;
                    if (w.once) {
                        Instruction &i = em.add(Op::CALL_FUNC);
                        i.s = "&&";
                        i.a = 2;
                    }
                }
            }
            complexity += count;
            if (!first) {
                Instruction &i = em.add(Op::CALL_FUNC);
                i.s = "&&";
                i.a = 2;
            }
            first = false;
        }
        out.complexity = complexity;
        _out.program.initial[once_node_var(out.name)] = Value::of(false);
    }

    void block(Emitter &em, const Block &b) {
        for (const Stmt &st : b) statement(em, st);
    }

    std::string line_id(const LineSpec &l) {
        ++_line_counter;
        if (!l.id.empty()) return l.id;
        return "line:" + stem_of(_file) + "-" + _node + "-" + std::to_string(_line_counter);
    }

    // The line's text into the string table; its substitutions onto the
    // stack. Gives back the id, and how many substitutions there are.
    std::pair<std::string, int> line_text(Emitter &em, const LineSpec &l) {
        std::string text;
        int n = 0;
        for (const Piece &p : l.pieces) {
            if (p.expr) {
                _want.clear();
                expr(em, *p.expr);
                text += "{" + std::to_string(n++) + "}";
            } else {
                text += p.text;
            }
        }
        const std::string id = line_id(l);
        if (_out.program.line_index(id) >= 0) {
            error(_file, l.at, "two lines have the id " + id);
        }
        LineInfo info;
        info.id = id;
        info.text = text;
        info.node = _node;
        info.tags = l.tags;
        info.shadow = l.shadow;
        info.source_line = l.at.line;
        if (!l.shadow.empty() && n > 0) {
            error(_file, l.at, "a shadow line cannot have expressions: it shows its source's");
        }
        _out.program.lines.push_back(std::move(info));
        return { id, n };
    }

    void shadows() {
        for (LineInfo &l : _out.program.lines) {
            if (l.shadow.empty()) continue;
            const int src = _out.program.line_index(l.shadow);
            if (src < 0) {
                error(_file, { l.source_line, 1 }, "a shadow line has no line " + l.shadow + " to shadow");
                continue;
            }
            const LineInfo &source = _out.program.lines[static_cast<std::size_t>(src)];
            if (source.text != l.text) {
                error(_file, { l.source_line, 1 },
                      "a shadow line has to say what its source, " + l.shadow + ", says");
            }
        }
    }

    static std::string once_var(const std::string &id) { return "$Yarn.Internal.Once." + id; }

    // A line's condition onto the stack (once, if, both). False when it has none.
    bool line_condition(Emitter &em, const LineSpec &l, const std::string &id, int *complexity) {
        int count = 0;
        if (l.once) {
            em.add(Op::PUSH_VARIABLE).s = once_var(id);
            Instruction &n = em.add(Op::CALL_FUNC);
            n.s = "!";
            n.a = 1;
            _out.program.initial[once_var(id)] = Value::of(false);
            count += 1;
        }
        if (l.condition) {
            _want = std::string(BOOL);
            expr(em, *l.condition);
            count += operator_count(*l.condition) + 1;
            if (l.once) {
                Instruction &a = em.add(Op::CALL_FUNC);
                a.s = "&&";
                a.a = 2;
            }
        }
        if (complexity != nullptr) *complexity = count;
        return l.once || l.condition.has_value();
    }

    void mark_once(Emitter &em, const LineSpec &l, const std::string &id) {
        if (!l.once) return;
        em.add(Op::PUSH_BOOL).a = 1;
        em.add(Op::STORE_VARIABLE).s = once_var(id);
        em.add(Op::POP);
    }

    // The id a line WILL get, without taking it: for a condition emitted first.
    std::string peek_id(const LineSpec &l) const {
        if (!l.id.empty()) return l.id;
        return "line:" + stem_of(_file) + "-" + _node + "-" + std::to_string(_line_counter + 1);
    }

    void statement(Emitter &em, const Stmt &st) {
        switch (st.is) {
            case Stmt::Is::LINE: {
                const std::string id = peek_id(st.line);
                if (line_condition(em, st.line, id, nullptr)) {
                    const int skip = em.here();
                    em.add(Op::JUMP_IF_FALSE);
                    em.add(Op::POP);
                    mark_once(em, st.line, id);
                    const auto [lid, n] = line_text(em, st.line);
                    Instruction &run = em.add(Op::RUN_LINE);
                    run.s = lid;
                    run.a = n;
                    const int to_end = em.here();
                    em.add(Op::JUMP_TO);
                    (*em.code)[static_cast<std::size_t>(skip)].a = em.here();
                    em.add(Op::POP);
                    (*em.code)[static_cast<std::size_t>(to_end)].a = em.here();
                } else {
                    const auto [lid, n] = line_text(em, st.line);
                    Instruction &run = em.add(Op::RUN_LINE);
                    run.s = lid;
                    run.a = n;
                }
                break;
            }
            case Stmt::Is::COMMAND: {
                std::string text;
                int n = 0;
                for (const Piece &p : st.line.pieces) {
                    if (p.expr) {
                        _want.clear();
                        expr(em, *p.expr);
                        text += "{" + std::to_string(n++) + "}";
                    } else {
                        text += p.text;
                    }
                }
                if (text == "stop") {
                    em.add(Op::STOP);
                    break;
                }
                Instruction &c = em.add(Op::RUN_COMMAND);
                c.s = text;
                c.a = n;
                break;
            }
            case Stmt::Is::SET: {
                const VarInfo &v = _vars[st.name];
                _want = v.type;
                if (st.op != "=") {
                    em.add(Op::PUSH_VARIABLE).s = st.name;
                    expr(em, *st.expr);
                    Instruction &f = em.add(Op::CALL_FUNC);
                    f.s = st.op.substr(0, 1);
                    f.a = 2;
                } else {
                    expr(em, *st.expr);
                }
                em.add(Op::STORE_VARIABLE).s = st.name;
                em.add(Op::POP);
                break;
            }
            case Stmt::Is::DECLARE:
            case Stmt::Is::ENUM: break;
            case Stmt::Is::JUMP:
            case Stmt::Is::DETOUR: {
                const bool jump = st.is == Stmt::Is::JUMP;
                if (st.expr) {
                    _want = std::string(STRING);
                    expr(em, *st.expr);
                    em.add(jump ? Op::PEEK_AND_RUN_NODE : Op::PEEK_AND_DETOUR_TO_NODE);
                } else {
                    em.add(jump ? Op::RUN_NODE : Op::DETOUR_TO_NODE).s = st.name;
                    _targets.emplace_back(st.name, std::make_pair(_file, st.at));
                }
                break;
            }
            case Stmt::Is::RETURN: em.add(Op::RETURN); break;
            case Stmt::Is::CALL:
                _want.clear();
                expr(em, *st.expr);
                em.add(Op::POP);
                break;
            case Stmt::Is::IF: ifs(em, st); break;
            case Stmt::Is::ONCE: once(em, st); break;
            case Stmt::Is::OPTIONS: options(em, st); break;
            case Stmt::Is::LINE_GROUP: line_group(em, st); break;
        }
    }

    void ifs(Emitter &em, const Stmt &st) {
        std::vector<int> to_end;
        for (const Clause &c : st.clauses) {
            if (c.condition) {
                _want = std::string(BOOL);
                expr(em, *c.condition);
                const int skip = em.here();
                em.add(Op::JUMP_IF_FALSE);
                em.add(Op::POP);
                block(em, c.body);
                to_end.push_back(em.here());
                em.add(Op::JUMP_TO);
                (*em.code)[static_cast<std::size_t>(skip)].a = em.here();
                em.add(Op::POP);
            } else {
                block(em, c.body);
            }
        }
        for (const int j : to_end) (*em.code)[static_cast<std::size_t>(j)].a = em.here();
    }

    void once(Emitter &em, const Stmt &st) {
        const std::string var = "$Yarn.Internal.Once." + _node + "-" + std::to_string(st.at.line);
        _out.program.initial[var] = Value::of(false);
        em.add(Op::PUSH_VARIABLE).s = var;
        Instruction &n = em.add(Op::CALL_FUNC);
        n.s = "!";
        n.a = 1;
        const Clause &primary = st.clauses[0];
        if (primary.condition) {
            _want = std::string(BOOL);
            expr(em, *primary.condition);
            Instruction &a = em.add(Op::CALL_FUNC);
            a.s = "&&";
            a.a = 2;
        }
        const int skip = em.here();
        em.add(Op::JUMP_IF_FALSE);
        em.add(Op::POP);
        em.add(Op::PUSH_BOOL).a = 1;
        em.add(Op::STORE_VARIABLE).s = var;
        em.add(Op::POP);
        block(em, primary.body);
        const int to_end = em.here();
        em.add(Op::JUMP_TO);
        (*em.code)[static_cast<std::size_t>(skip)].a = em.here();
        em.add(Op::POP);
        if (st.clauses.size() > 1) block(em, st.clauses[1].body);
        (*em.code)[static_cast<std::size_t>(to_end)].a = em.here();
    }

    void options(Emitter &em, const Stmt &st) {
        std::vector<int> adds;
        std::vector<std::string> ids;
        for (const Item &item : st.items) {
            const std::string id = peek_id(item.line);
            const auto [lid, n] = line_text(em, item.line);
            const bool has = line_condition(em, item.line, id, nullptr);
            adds.push_back(em.here());
            Instruction &add = em.add(Op::ADD_OPTION);
            add.s = lid;
            add.b = n;
            add.c = has ? 1 : 0;
            ids.push_back(lid);
        }
        const int show = em.here();
        em.add(Op::SHOW_OPTIONS);
        em.add(Op::PEEK_AND_JUMP);
        std::vector<int> to_end;
        for (std::size_t i = 0; i < st.items.size(); ++i) {
            (*em.code)[static_cast<std::size_t>(adds[i])].a = em.here();
            em.add(Op::POP);
            mark_once(em, st.items[i].line, ids[i]);
            block(em, st.items[i].body);
            to_end.push_back(em.here());
            em.add(Op::JUMP_TO);
        }
        // Nothing available, nothing chosen: on past the group.
        (*em.code)[static_cast<std::size_t>(show)].a = em.here();
        em.add(Op::POP);
        for (const int j : to_end) (*em.code)[static_cast<std::size_t>(j)].a = em.here();
    }

    void line_group(Emitter &em, const Stmt &st) {
        std::vector<int> adds;
        std::vector<std::string> ids;
        // Each item's id is taken here, in order, so the lines below reuse them.
        std::vector<LineSpec> specs;
        for (const Item &item : st.items) {
            LineSpec spec = item.line;
            spec.id = line_id(item.line);
            int complexity = 0;
            if (!line_condition(em, spec, spec.id, &complexity)) em.add(Op::PUSH_BOOL).a = 1;
            adds.push_back(em.here());
            Instruction &add = em.add(Op::ADD_SALIENCY_CANDIDATE);
            add.s = spec.id;
            add.a = complexity;
            ids.push_back(spec.id);
            specs.push_back(std::move(spec));
        }
        em.add(Op::SELECT_SALIENCY_CANDIDATE);
        const int none = em.here();
        em.add(Op::JUMP_IF_FALSE);
        em.add(Op::POP);
        em.add(Op::PEEK_AND_JUMP);
        std::vector<int> to_end;
        const int saved = _line_counter;
        for (std::size_t i = 0; i < st.items.size(); ++i) {
            (*em.code)[static_cast<std::size_t>(adds[i])].b = em.here();
            em.add(Op::POP);
            mark_once(em, specs[i], ids[i]);
            const auto [lid, n] = line_text(em, specs[i]);
            Instruction &run = em.add(Op::RUN_LINE);
            run.s = lid;
            run.a = n;
            block(em, st.items[i].body);
            to_end.push_back(em.here());
            em.add(Op::JUMP_TO);
        }
        (void)saved;
        (*em.code)[static_cast<std::size_t>(none)].a = em.here();
        em.add(Op::POP);
        for (const int j : to_end) (*em.code)[static_cast<std::size_t>(j)].a = em.here();
    }

    void expr(Emitter &em, const Expr &e) {
        switch (e.is) {
            case Expr::Is::NUMBER: em.add(Op::PUSH_FLOAT).f = e.number; return;
            case Expr::Is::STRING: em.add(Op::PUSH_STRING).s = e.text; return;
            case Expr::Is::BOOL: em.add(Op::PUSH_BOOL).a = e.boolean ? 1 : 0; return;
            case Expr::Is::VARIABLE: em.add(Op::PUSH_VARIABLE).s = e.text; return;
            case Expr::Is::MEMBER: {
                const Value v = member_value(e, _want);
                if (v.kind == Kind::STRING) {
                    em.add(Op::PUSH_STRING).s = v.text;
                } else {
                    em.add(Op::PUSH_FLOAT).f = v.number;
                }
                return;
            }
            case Expr::Is::UNARY: {
                expr(em, e.args[0]);
                Instruction &f = em.add(Op::CALL_FUNC);
                f.s = e.text == "-" ? "neg" : "!";
                f.a = 1;
                return;
            }
            case Expr::Is::BINARY: {
                // An enum member on one side takes the other side's enum.
                const std::string saved = _want;
                _want = side_type(e.args[1]);
                expr(em, e.args[0]);
                _want = side_type(e.args[0]);
                expr(em, e.args[1]);
                _want = saved;
                Instruction &f = em.add(Op::CALL_FUNC);
                f.s = e.text;
                f.a = 2;
                return;
            }
            case Expr::Is::CALL: {
                for (const Expr &a : e.args) {
                    _want.clear();
                    expr(em, a);
                }
                Instruction &f = em.add(Op::CALL_FUNC);
                f.s = e.text;
                f.a = static_cast<int>(e.args.size());
                return;
            }
        }
    }

    std::string side_type(const Expr &e) {
        if (e.is == Expr::Is::VARIABLE) return _vars[e.text].type;
        if (e.is == Expr::Is::MEMBER) return member_type(e, {}, Ctx{ {}, false });
        return {};
    }

    std::span<const Source> _sources;
    std::vector<Script> _scripts;
    Compiled _out;
    std::map<std::string, EnumDef> _enums;
    std::map<std::string, VarInfo> _vars;
    std::map<std::string, Signature> _functions;
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> _groups;
    std::vector<std::pair<std::string, std::pair<std::string, Position>>> _targets;
    bool _changed = false;
    std::string _want;
    std::string _node;
    std::string _file;
    int _line_counter = 0;
};

} // namespace

Compiled compile(std::span<const Source> sources, std::span<const FunctionType> functions) {
    Compiler c(sources, functions);
    Compiled out = c.run();
    // A jump to a node that is not there is said when the program is built.
    return out;
}

} // namespace rmp::yarn
