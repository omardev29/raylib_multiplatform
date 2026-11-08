// ===========================================================================
// The machine that runs a Program. Ported from VirtualMachine.cs,
// Dialogue.cs, Library.cs and Saliency/ of the official runtime: a stack of
// values, a stack of the nodes it detoured through, an instruction at a time
// until a line, a set of options or a command needs the game.
// ===========================================================================

#include "yarn.h"

#include <rmp/random.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace rmp::yarn {

namespace {

constexpr std::string_view VISITING = "$Yarn.Internal.Visiting.";
constexpr std::string_view VIEW_COUNT = "$Yarn.Internal.Content.ViewCount.";
constexpr std::int64_t MAX_STEPS = 100'000; // instructions between two events
constexpr std::size_t MAX_DEPTH = 256; // nodes on the detour stack
// A command's words: split on spaces, a "quoted part" one word.
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size()) break;
        std::string w;
        if (text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                w += text[i++];
            }
            ++i;
        } else {
            while (i < text.size() && text[i] != ' ') w += text[i++];
        }
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace

// --- the machine ---------------------------------------------------------------

Machine::Machine(Program program) : _program(std::move(program)) {}

void Machine::fault(std::string message) {
    if (std::ranges::find(_errors, message) == _errors.end())
        _errors.push_back(std::move(message));
}

bool Machine::start(std::string_view node) {
    stop();
    const int index = _program.node_index(node);
    if (index < 0) {
        fault("there is no node called '" + std::string(node) + "'");
        _event = Event{ Event::Is::DONE };
        return false;
    }
    _event = {};
    _stack.clear();
    _options.clear();
    _candidates.clear();
    enter(index);
    run();
    return true;
}

void Machine::stop() {
    leave_all();
    _waiting = false;
    _event = Event{ Event::Is::DONE };
}

void Machine::advance() {
    if (!_waiting || _event.is == Event::Is::OPTIONS) return;
    _waiting = false;
    run();
}

void Machine::choose(int index) {
    if (!_waiting || _event.is != Event::Is::OPTIONS) return;
    const auto &opts = _event.options;
    const bool valid = index >= 0 && static_cast<std::size_t>(index) < opts.size() &&
        opts[static_cast<std::size_t>(index)].available;
    if (!valid) {
        // Nothing chosen: on past the group, where a value waits to be popped.
        const bool any = std::ranges::any_of(opts, &OptionEvent::available);
        if (any) return; // a choice is still to be made
        _stack.push_back(Value::of(0.0f));
        _frames.back().pc = _fallthrough;
    } else {
        _stack.push_back(Value::of(
            static_cast<float>(opts[static_cast<std::size_t>(index)].destination)));
    }
    _waiting = false;
    run();
}

void Machine::on_command(std::string_view name, Handler handler, Signature signature) {
    _commands.insert_or_assign(std::string(name),
                               Command{ std::move(handler), std::move(signature) });
}

void Machine::on_function(std::string_view name, Function function, Signature signature) {
    _functions.insert_or_assign(std::string(name),
                                Registered{ std::move(function), std::move(signature) });
}

void Machine::bind(std::string_view name, Binding binding) {
    _bound.insert_or_assign(std::string(name), std::move(binding));
    _vars.erase(std::string(name)); // the game's from now on
}

bool Machine::bound(std::string_view name) const { return _bound.contains(name); }

bool Machine::has_function(std::string_view name) const {
    return _functions.contains(name);
}

std::optional<Signature> Machine::signature(std::string_view name) const {
    if (const auto f = _functions.find(name); f != _functions.end())
        return f->second.signature;
    if (const auto c = _commands.find(name); c != _commands.end())
        return c->second.signature;
    return std::nullopt;
}

namespace {

std::string_view kind_name(Kind k) {
    switch (k) {
        case Kind::NUMBER:
            return "a number";
        case Kind::STRING:
            return "a string";
        case Kind::BOOL:
            return "a bool";
    }
    return "a value";
}

} // namespace

// Whether a command's word is a number as a script writes one: a sign, digits
// with at most one point between or around them, and an exponent. By hand:
// a floating-point from_chars is not in every C++ library this builds with
// (emscripten's, Apple's), and it would bring its tables into every game.
namespace {

bool is_number_word(std::string_view w) {
    std::size_t i = 0;
    if (i < w.size() && (w[i] == '-' || w[i] == '+')) ++i;
    int digits = 0;
    bool point = false;
    for (; i < w.size(); ++i) {
        if (w[i] >= '0' && w[i] <= '9') {
            ++digits;
        } else if (w[i] == '.' && !point) {
            point = true;
        } else {
            break;
        }
    }
    if (digits == 0) return false;
    if (i < w.size() && (w[i] == 'e' || w[i] == 'E')) {
        ++i;
        if (i < w.size() && (w[i] == '-' || w[i] == '+')) ++i;
        const std::size_t from = i;
        while (i < w.size() && w[i] >= '0' && w[i] <= '9') ++i;
        if (i == from) return false;
    }
    return i == w.size();
}

} // namespace

// The same for a command, whose arguments are words: a number must read as
// one, a bool as true or false.
bool Machine::command_fits(const std::string &name, const Signature &s,
                           std::span<const std::string> words) {
    if (s.arity >= 0 && std::cmp_not_equal(words.size(), s.arity)) {
        fault("<<" + name + ">> takes " + std::to_string(s.arity) + " word" +
              (s.arity == 1 ? "" : "s") + " after its name, and the script gives it " +
              std::to_string(words.size()));
        return false;
    }
    for (std::size_t k = 0; k < words.size() && k < s.params.size(); ++k) {
        if (!s.params[k]) continue;
        const std::string &w = words[k];
        bool ok = true;
        if (*s.params[k] == Kind::NUMBER) {
            ok = is_number_word(w);
        } else if (*s.params[k] == Kind::BOOL) {
            ok = w == "true" || w == "false" || w == "True" || w == "False";
        }
        if (!ok) {
            fault("<<" + name + ">>'s word " + std::to_string(k + 1) + " is " +
                  std::string(kind_name(*s.params[k])) + ", and the script gives it '" +
                  w + "'");
            return false;
        }
    }
    return true;
}

// Whether `args` are what the game said `name` takes; a fault when not.
bool Machine::fits(const std::string &name, const Signature &s,
                   std::span<const Value> args) {
    if (s.arity >= 0 && std::cmp_not_equal(args.size(), s.arity)) {
        fault(name + " takes " + std::to_string(s.arity) + " argument" +
              (s.arity == 1 ? "" : "s") + ", and the script gives it " +
              std::to_string(args.size()));
        return false;
    }
    for (std::size_t k = 0; k < args.size() && k < s.params.size(); ++k) {
        if (s.params[k] && *s.params[k] != args[k].kind) {
            fault(name + "'s argument " + std::to_string(k + 1) + " is " +
                  std::string(kind_name(*s.params[k])) + ", and the script gives it " +
                  std::string(kind_name(args[k].kind)));
            return false;
        }
    }
    return true;
}

void Machine::enter(int node) { _frames.push_back({ node, 0 }); }

std::string Machine::node() const {
    if (_frames.empty()) return {};
    return _program.nodes[static_cast<std::size_t>(_frames.back().node)].name;
}

void Machine::leave_top() {
    if (_frames.empty()) return;
    const ProgramNode &n = _program.nodes[static_cast<std::size_t>(_frames.back().node)];
    if (n.tracked) {
        const std::string key = std::string(VISITING) + n.name;
        const Value count = _vars.contains(key) ? _vars[key] : Value::of(0.0f);
        _vars[key] = Value::of(count.as_number() + 1);
        if (!n.group.empty()) {
            // The group was visited too, through this member.
            const std::string group = std::string(VISITING) + n.group;
            (void)group;
        }
    }
    _frames.pop_back();
}

void Machine::leave_all() {
    while (!_frames.empty()) leave_top();
}

bool Machine::jump_to(std::string_view node, bool detour) {
    const int index = _program.node_index(node);
    if (index < 0) {
        fault("there is no node called '" + std::string(node) + "' to jump to");
        stop();
        return false;
    }
    if (detour) {
        ++_frames.back().pc;
    } else {
        leave_all();
    }
    enter(index);
    return true;
}

int Machine::visits(std::string_view node) const {
    const auto it = _vars.find(std::string(VISITING) + std::string(node));
    return it == _vars.end() ? 0 : static_cast<int>(it->second.as_number());
}

Value Machine::read(std::string_view name) const {
    if (const auto b = _bound.find(name); b != _bound.end()) {
        Value v = Value::of(0.0f);
        b->second.get(v);
        return v;
    }
    if (const auto it = _vars.find(std::string(name)); it != _vars.end())
        return it->second;
    for (const SmartVariable &s : _program.smart) {
        if (s.name == name) return evaluate(s.code);
    }
    if (const auto it = _program.initial.find(std::string(name));
        it != _program.initial.end()) {
        return it->second;
    }
    return Value::of(0.0f);
}

Value Machine::variable(std::string_view name) const { return read(name); }

void Machine::set_variable(std::string_view name, Value value) {
    if (const auto b = _bound.find(name); b != _bound.end()) {
        b->second.set(value);
        return;
    }
    _vars.insert_or_assign(std::string(name), std::move(value));
}

Value Machine::evaluate(const std::vector<Instruction> &code) const {
    // Smart variables and node conditions: pure code, run on a stack of its own.
    std::vector<Value> stack;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): call() records faults
    auto &self = const_cast<Machine &>(*this);
    for (const Instruction &i : code) {
        switch (i.op) {
            case Op::PUSH_FLOAT:
                stack.push_back(Value::of(i.f));
                break;
            case Op::PUSH_STRING:
                stack.push_back(Value::of(i.s));
                break;
            case Op::PUSH_BOOL:
                stack.push_back(Value::of(i.a != 0));
                break;
            case Op::PUSH_VARIABLE:
                stack.push_back(read(i.s));
                break;
            case Op::CALL_FUNC: {
                const auto n = static_cast<std::size_t>(std::max(0, i.a));
                if (stack.size() < n) return Value::of(false);
                std::vector<Value> args(stack.end() - static_cast<std::ptrdiff_t>(n),
                                        stack.end());
                stack.resize(stack.size() - n);
                stack.push_back(self.call(i.s, args));
                break;
            }
            default:
                break;
        }
    }
    return stack.empty() ? Value::of(false) : stack.back();
}

Value Machine::call(const std::string &name, std::span<const Value> args) {
    const auto num = [&](std::size_t k) {
        return k < args.size() ? args[k].as_number() : 0.0f;
    };
    const auto boo = [&](std::size_t k) { return k < args.size() && args[k].as_bool(); };
    if (args.size() == 2) {
        const Value &a = args[0];
        const Value &b = args[1];
        if (name == "+") {
            if (a.kind == Kind::STRING || b.kind == Kind::STRING)
                return Value::of(a.str() + b.str());
            return Value::of(a.as_number() + b.as_number());
        }
        if (name == "-") return Value::of(num(0) - num(1));
        if (name == "*") return Value::of(num(0) * num(1));
        if (name == "/") return Value::of(num(0) / num(1));
        if (name == "%") return Value::of(std::fmod(num(0), num(1)));
        if (name == "==") return Value::of(a == b);
        if (name == "!=") return Value::of(!(a == b));
        if (name == "<") return Value::of(num(0) < num(1));
        if (name == ">") return Value::of(num(0) > num(1));
        if (name == "<=") return Value::of(num(0) <= num(1));
        if (name == ">=") return Value::of(num(0) >= num(1));
        if (name == "&&") return Value::of(boo(0) && boo(1));
        if (name == "||") return Value::of(boo(0) || boo(1));
        if (name == "^") return Value::of(boo(0) != boo(1));
    }
    if (name == "!") return Value::of(!boo(0));
    if (name == "neg") return Value::of(-num(0));
    if (const auto f = _functions.find(name); f != _functions.end()) {
        Value out;
        if (!fits(name + "()", f->second.signature, args)) return Value::of(false);
        f->second.function(out, args);
        return out;
    }
    const auto truncate = [](float v) { return std::trunc(v); };
    if (name == "visited")
        return Value::of(visits(args.empty() ? "" : args[0].str()) > 0);
    if (name == "visited_count") {
        return Value::of(static_cast<float>(visits(args.empty() ? "" : args[0].str())));
    }
    if (name == "has_any_content")
        return Value::of(has_content(args.empty() ? "" : args[0].str()));
    if (name == "string") return Value::of(args.empty() ? std::string() : args[0].str());
    if (name == "number") return Value::of(args.empty() ? 0.0f : args[0].as_number());
    if (name == "bool") return Value::of(boo(0));
    if (name == "format_invariant") return Value::of(number_text(num(0)));
    if (name == "format") {
        return Value::of(
            expand(args.empty() ? "" : args[0].str(),
                   std::vector<std::string>{ args.size() > 1 ? args[1].str() : "" }));
    }
    if (name == "random") return Value::of(rmp::random::value());
    if (name == "random_range" || name == "random_range_float") {
        const auto lo = static_cast<int>(num(0));
        const auto hi = static_cast<int>(num(1));
        return Value::of(static_cast<float>(rmp::random::range(0, std::max(0, hi - lo))) +
                         num(0));
    }
    if (name == "dice") {
        return Value::of(static_cast<float>(
            rmp::random::range(1, std::max(1, static_cast<int>(num(0))))));
    }
    if (name == "min") return Value::of(std::min(num(0), num(1)));
    if (name == "max") return Value::of(std::max(num(0), num(1)));
    if (name == "round") return Value::of(std::nearbyint(num(0)));
    if (name == "round_places") {
        const float scale = std::pow(10.0f, std::trunc(num(1)));
        return Value::of(std::nearbyint(num(0) * scale) / scale);
    }
    if (name == "floor") return Value::of(std::floor(num(0)));
    if (name == "ceil") return Value::of(std::ceil(num(0)));
    if (name == "decimal") return Value::of(num(0) - truncate(num(0)));
    if (name == "int") return Value::of(truncate(num(0)));
    if (name == "inc") {
        const float v = num(0);
        return Value::of(v - truncate(v) == 0 ? truncate(v + 1) : std::ceil(v));
    }
    if (name == "dec") {
        const float v = num(0);
        return Value::of(v - truncate(v) == 0 ? truncate(v) - 1 : std::floor(v));
    }
    fault("the script calls " + name + "(), and nothing registered it: on_function(\"" +
          name + "\", ...)");
    return Value::of(false);
}

std::optional<Machine::Candidate>
Machine::select(const std::vector<Candidate> &candidates) {
    // By their places in `candidates`: what passes, then the one chosen.
    std::vector<std::size_t> passing;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].passing) passing.push_back(i);
    }
    if (passing.empty()) return std::nullopt;
    const auto views = [&](std::size_t i) {
        return static_cast<int>(
            read(std::string(VIEW_COUNT) + candidates[i].id).as_number());
    };
    std::size_t chosen = passing.front();
    switch (_saliency) {
        case Saliency::FIRST:
            break;
        case Saliency::BEST:
            for (const std::size_t c : passing) {
                if (candidates[c].complexity > candidates[chosen].complexity) chosen = c;
            }
            break;
        case Saliency::BEST_LEAST_RECENTLY_VIEWED:
        case Saliency::RANDOM_BEST_LEAST_RECENTLY_VIEWED: {
            int best_views = views(chosen);
            for (const std::size_t c : passing) {
                const int v = views(c);
                if (v < best_views ||
                    (v == best_views &&
                     candidates[c].complexity > candidates[chosen].complexity)) {
                    chosen = c;
                    best_views = v;
                }
            }
            if (_saliency == Saliency::RANDOM_BEST_LEAST_RECENTLY_VIEWED) {
                std::vector<std::size_t> equal;
                for (const std::size_t c : passing) {
                    if (views(c) == best_views &&
                        candidates[c].complexity == candidates[chosen].complexity) {
                        equal.push_back(c);
                    }
                }
                chosen = equal[static_cast<std::size_t>(
                    rmp::random::index(static_cast<int>(equal.size())))];
            }
            const std::string key = std::string(VIEW_COUNT) + candidates[chosen].id;
            _vars[key] = Value::of(static_cast<float>(best_views + 1));
            break;
        }
    }
    return candidates[chosen];
}

bool Machine::has_content(std::string_view group) {
    // A node that is not a group always has its content.
    if (const int i = _program.node_index(group);
        i >= 0 && !_program.nodes[static_cast<std::size_t>(i)].hub) {
        return true;
    }
    return std::ranges::any_of(_program.nodes, [&](const ProgramNode &n) {
        return n.group == group && evaluate(n.condition).as_bool();
    });
}

void Machine::run() {
    // A script that loops with nothing to show -- <<jump>> back to itself, a
    // node that detours into itself before any line -- would hang the game or
    // grow without end: a budget of instructions between two events, and of
    // nodes deep, stops it, said once.
    std::int64_t budget = MAX_STEPS;
    while (!_waiting && !_frames.empty()) {
        if (--budget < 0 || _frames.size() > MAX_DEPTH) {
            fault(budget < 0 ? "the script ran " + std::to_string(MAX_STEPS) +
                          " instructions without a line or a choice: a loop with "
                          "nothing in it?"
                             : "the script detoured " + std::to_string(MAX_DEPTH) +
                          " nodes deep: a detour that never returns?");
            stop();
            return;
        }
        const Frame &f = _frames.back();
        const ProgramNode &node = _program.nodes[static_cast<std::size_t>(f.node)];
        if (f.pc < 0 || static_cast<std::size_t>(f.pc) >= node.code.size()) {
            leave_top();
            continue;
        }
        step(node.code[static_cast<std::size_t>(f.pc)]);
    }
    if (_frames.empty() && !_waiting) _event = Event{ Event::Is::DONE };
}

void Machine::step(const Instruction &i) {
    Frame &f = _frames.back();
    const auto pop = [this]() {
        if (_stack.empty()) return Value::of(0.0f);
        Value v = std::move(_stack.back());
        _stack.pop_back();
        return v;
    };
    const auto pop_strings = [&](int n) {
        std::vector<std::string> out(static_cast<std::size_t>(std::max(0, n)));
        for (int k = n - 1; k >= 0; --k) out[static_cast<std::size_t>(k)] = pop().str();
        return out;
    };
    switch (i.op) {
        case Op::JUMP_TO:
            f.pc = i.a;
            return;
        case Op::PEEK_AND_JUMP:
            f.pc =
                _stack.empty() ? f.pc + 1 : static_cast<int>(_stack.back().as_number());
            return;
        case Op::RUN_LINE:
            _event = Event{ Event::Is::LINE };
            _event.id = i.s;
            _event.substitutions = pop_strings(i.a);
            ++f.pc;
            _waiting = true;
            return;
        case Op::RUN_COMMAND: {
            const std::vector<std::string> subs = pop_strings(i.a);
            const std::string text = expand(i.s, subs);
            ++f.pc;
            const std::vector<std::string> w = words(text);
            if (!w.empty()) {
                if (const auto h = _commands.find(w.front()); h != _commands.end()) {
                    const std::span<const std::string> rest =
                        std::span<const std::string>(w).subspan(1);
                    if (!command_fits(w.front(), h->second.signature, rest)) return;
                    h->second.handler(rest);
                    if (_hold) {
                        _hold = false;
                        _event = Event{ Event::Is::HOLD };
                        _waiting = true;
                    }
                    return;
                }
                if (w.front() == "wait") {
                    // Yarn's own <<wait 2>>, when the game has no "wait" of its own.
                    _event = Event{ Event::Is::HOLD };
                    _event.seconds =
                        w.size() > 1 ? std::max(0.0f, Value::of(w[1]).as_number()) : 0.0f;
                    _waiting = true;
                    return;
                }
            }
            _event = Event{ Event::Is::COMMAND };
            _event.command = text;
            _waiting = true;
            return;
        }
        case Op::ADD_OPTION: {
            OptionEvent o;
            o.available = i.c == 0 || pop().as_bool();
            o.substitutions = pop_strings(i.b);
            o.id = i.s;
            o.destination = i.a;
            _options.push_back(std::move(o));
            ++f.pc;
            return;
        }
        case Op::SHOW_OPTIONS:
            _event = Event{ Event::Is::OPTIONS };
            _event.options = std::move(_options);
            _options.clear();
            _fallthrough = i.a;
            ++f.pc;
            _waiting = true;
            return;
        case Op::PUSH_STRING:
            _stack.push_back(Value::of(i.s));
            break;
        case Op::PUSH_FLOAT:
            _stack.push_back(Value::of(i.f));
            break;
        case Op::PUSH_BOOL:
            _stack.push_back(Value::of(i.a != 0));
            break;
        case Op::JUMP_IF_FALSE:
            if (_stack.empty() || !_stack.back().as_bool()) {
                f.pc = i.a;
                return;
            }
            break;
        case Op::POP:
            (void)pop();
            break;
        case Op::CALL_FUNC: {
            const auto n = static_cast<std::size_t>(std::max(0, i.a));
            std::vector<Value> args(n);
            for (std::size_t k = n; k > 0; --k) args[k - 1] = pop();
            Value result = call(i.s, args);
            _stack.push_back(std::move(result));
            break;
        }
        case Op::PUSH_VARIABLE:
            _stack.push_back(read(i.s));
            break;
        case Op::STORE_VARIABLE:
            if (!_stack.empty()) set_variable(i.s, _stack.back());
            break;
        case Op::STOP:
            stop();
            return;
        case Op::RUN_NODE:
            jump_to(i.s, false);
            return;
        case Op::PEEK_AND_RUN_NODE:
            jump_to(pop().str(), false);
            return;
        case Op::DETOUR_TO_NODE:
            jump_to(i.s, true);
            return;
        case Op::PEEK_AND_DETOUR_TO_NODE:
            jump_to(pop().str(), true);
            return;
        case Op::RETURN:
            leave_top();
            return;
        case Op::ADD_SALIENCY_CANDIDATE:
            _candidates.push_back({ i.s, i.a, i.b, pop().as_bool() });
            break;
        case Op::ADD_SALIENCY_CANDIDATE_FROM_NODE: {
            const int index = _program.node_index(i.s);
            if (index >= 0) {
                const ProgramNode &n = _program.nodes[static_cast<std::size_t>(index)];
                _candidates.push_back(
                    { n.name, n.complexity, index, evaluate(n.condition).as_bool() });
            }
            break;
        }
        case Op::SELECT_SALIENCY_CANDIDATE: {
            const std::optional<Candidate> chosen = select(_candidates);
            _candidates.clear();
            if (chosen) {
                _stack.push_back(Value::of(static_cast<float>(chosen->destination)));
                _stack.push_back(Value::of(true));
            } else {
                _stack.push_back(Value::of(false));
            }
            break;
        }
        case Op::REPLACE_NODE: {
            const int target = static_cast<int>(pop().as_number());
            if (target < 0 || static_cast<std::size_t>(target) >= _program.nodes.size()) {
                fault("a node group chose a node that is not there");
                stop();
                return;
            }
            leave_top(); // the group's chooser: the group was visited
            enter(target);
            return;
        }
    }
    ++_frames.back().pc;
}

} // namespace rmp::yarn
