// ===========================================================================
// rmp::Dialogue -- see include/rmp/dialogue.h. The machine of yarn_vm.cpp
// behind the public API: what it waits for turned into a DialogueLine or a
// set of DialogueOptions in the current language, its variables into an
// rmp::Value, and a script loaded from resources/ -- or, built with
// [dialogue] build = "embedded", from the bytes rmp_yarnc wrote into the game.
// ===========================================================================

#include "../internal.h"
#include "../text_internal.h"
#include "dialogue_internal.h"
#include "yarn.h"

#include <rmp/assets.h>
#include <rmp/dialogue.h>
#include <rmp/save.h>
#include <rmp/text.h>

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace rmp {

namespace detail {

struct DialogueState {
    yarn::Machine machine;
    std::vector<std::string> errors;
    bool loaded = false;
    // What the machine waits for, in the game's words, made when it stops.
    DialogueLine line;
    std::vector<DialogueOption> options;
    std::string command;
};

} // namespace detail

namespace {

struct {
    std::vector<rmp::dialogue::detail::Embedded> table;
} embedded;

yarn::Value to_yarn(const DialogueValue &v) {
    switch (v.kind()) {
        case DialogueValue::Kind::NUMBER: return yarn::Value::of(v.as_number()); // NOLINT(misc-no-recursion): as_number() of a NUMBER reads the field
        case DialogueValue::Kind::BOOL: return yarn::Value::of(v.as_bool());
        case DialogueValue::Kind::STRING: return yarn::Value::of(v.as_string());
    }
    return {};
}

DialogueValue from_yarn(const yarn::Value &v) {
    switch (v.kind) {
        case yarn::Kind::NUMBER: return DialogueValue(v.number);
        case yarn::Kind::BOOL: return DialogueValue(v.boolean);
        case yarn::Kind::STRING: return DialogueValue(v.text);
    }
    return {};
}

std::vector<DialogueMarkup> markup_of(const std::vector<yarn::Attribute> &attributes, int shift) {
    std::vector<DialogueMarkup> out;
    for (const yarn::Attribute &a : attributes) {
        if (a.name == "character") continue;
        out.push_back({ a.name, std::max(0, a.position - shift), a.length, a.properties });
    }
    return out;
}

// A line's text as the player reads it: translated, filled, its markup
// parsed, its speaker taken off the front.
struct Shown {
    std::string speaker;
    std::string text;
    std::vector<DialogueMarkup> markup;
};

Shown show(const yarn::Program &program, const std::string &id,
           const std::vector<std::string> &substitutions) {
    std::string source;
    const int index = program.line_index(id);
    if (index >= 0) {
        const yarn::LineInfo *info = &program.lines[static_cast<std::size_t>(index)];
        if (!info->shadow.empty()) {
            const int src = program.line_index(info->shadow);
            if (src >= 0) info = &program.lines[static_cast<std::size_t>(src)];
        }
        source = info->text;
    }
    const std::optional<std::string> translated = rmp::lang::detail::find(id);
    const std::string language = rmp::lang::current();
    const yarn::Marked marked =
        yarn::parse_markup(yarn::expand(translated ? *translated : source, substitutions), language);
    Shown out;
    out.text = marked.text;
    int shift = 0;
    for (const yarn::Attribute &a : marked.attributes) {
        if (a.name != "character") continue;
        for (const auto &[k, v] : a.properties) {
            if (k == "name") out.speaker = v;
        }
        // The speaker's characters, from the front of the text.
        std::size_t bytes = 0;
        int seen = 0;
        while (bytes < out.text.size() && seen < a.length) {
            ++bytes;
            while (bytes < out.text.size() &&
                   (static_cast<unsigned char>(out.text[bytes]) & 0xC0) == 0x80) {
                ++bytes;
            }
            ++seen;
        }
        out.text.erase(0, bytes);
        shift = a.length;
        break;
    }
    out.markup = markup_of(marked.attributes, shift);
    return out;
}

std::vector<std::string> tags_of(const yarn::Program &program, const std::string &id) {
    const int index = program.line_index(id);
    if (index < 0) return {};
    return program.lines[static_cast<std::size_t>(index)].tags;
}

// The machine's event, turned into the game's words: once per event, so a
// presenter asking every frame does not translate every frame.
void refresh(detail::DialogueState &s) {
    const yarn::Event &e = s.machine.event();
    s.line = {};
    s.options.clear();
    s.command.clear();
    const yarn::Program &p = s.machine.program();
    switch (e.is) {
        case yarn::Event::Is::LINE: {
            Shown shown = show(p, e.id, e.substitutions);
            s.line.id = e.id;
            s.line.speaker = std::move(shown.speaker);
            s.line.text = std::move(shown.text);
            s.line.markup = std::move(shown.markup);
            s.line.tags = tags_of(p, e.id);
            break;
        }
        case yarn::Event::Is::OPTIONS:
            for (const yarn::OptionEvent &o : e.options) {
                Shown shown = show(p, o.id, o.substitutions);
                DialogueOption opt;
                opt.id = o.id;
                // An option's speaker is part of what it says.
                opt.text = shown.speaker.empty() ? shown.text : shown.speaker + ": " + shown.text;
                opt.available = o.available;
                opt.markup = std::move(shown.markup);
                opt.tags = tags_of(p, o.id);
                s.options.push_back(std::move(opt));
            }
            break;
        case yarn::Event::Is::COMMAND: s.command = e.command; break;
        default: break;
    }
}

void report_errors(const std::vector<std::string> &errors) {
    for (const std::string &e : errors) {
        // A mistake in the game's script: said once each, and drawn in a
        // debug build like every mistake of the game's.
        RMP_REPORT_ONCE_KEYED(std::string_view(e), "DIALOGUE: %s", e.c_str());
    }
}

Dialogue from_sources(const std::vector<yarn::Source> &sources) {
    auto state = std::make_unique<detail::DialogueState>();
    yarn::Compiled compiled = yarn::compile(sources);
    for (const yarn::Diagnostic &d : compiled.errors) state->errors.push_back(d.str());
    report_errors(state->errors);
    state->loaded = compiled.ok();
    if (compiled.ok()) state->machine = yarn::Machine(std::move(compiled.program));
    return Dialogue(std::move(state));
}

} // namespace

// --- DialogueValue -------------------------------------------------------------

float DialogueValue::as_number() const {
    switch (_kind) {
        case Kind::NUMBER: return _number;
        case Kind::BOOL: return _bool ? 1.0f : 0.0f;
        case Kind::STRING: return yarn::Value::of(_text).as_number();
    }
    return 0;
}

bool DialogueValue::as_bool() const {
    switch (_kind) {
        case Kind::NUMBER: return _number != 0;
        case Kind::BOOL: return _bool;
        case Kind::STRING: return yarn::Value::of(_text).as_bool();
    }
    return false;
}

std::string DialogueValue::as_string() const {
    switch (_kind) {
        case Kind::NUMBER: return yarn::number_text(_number);
        case Kind::BOOL: return _bool ? "True" : "False";
        case Kind::STRING: return _text;
    }
    return {};
}

std::string DialogueMarkup::property(std::string_view key) const {
    for (const auto &[k, v] : properties) {
        if (k == key) return v;
    }
    return {};
}

// --- Dialogue -----------------------------------------------------------------

Dialogue::Dialogue() : _state(std::make_unique<detail::DialogueState>()) {}
Dialogue::Dialogue(std::unique_ptr<detail::DialogueState> state) : _state(std::move(state)) {}
Dialogue::~Dialogue() = default;
Dialogue::Dialogue(Dialogue &&other) noexcept = default;
Dialogue &Dialogue::operator=(Dialogue &&other) noexcept = default;

bool Dialogue::loaded() const { return _state && _state->loaded; }

std::span<const std::string> Dialogue::errors() const {
    if (!_state) return {};
    return _state->errors;
}

bool Dialogue::has_node(std::string_view node) const {
    return _state && _state->machine.program().node_index(node) >= 0;
}

bool Dialogue::start(std::string_view node) {
    if (!loaded()) return false;
    const bool ok = _state->machine.start(node);
    if (!ok) {
        RMP_REPORT_ONCE_KEYED(node, "DIALOGUE: start(\"%.*s\"): the script has no such node",
                              static_cast<int>(node.size()), node.data());
    }
    refresh(*_state);
    return ok;
}

void Dialogue::stop() {
    if (!_state) return;
    _state->machine.stop();
    refresh(*_state);
}

bool Dialogue::running() const { return _state && _state->machine.running(); }

std::string Dialogue::node() const {
    if (!running()) return {};
    return _state->machine.node();
}

bool Dialogue::has_line() const {
    return running() && _state->machine.event().is == yarn::Event::Is::LINE;
}

const DialogueLine &Dialogue::line() const {
    static const DialogueLine none;
    return has_line() ? _state->line : none;
}

bool Dialogue::has_options() const {
    return running() && _state->machine.event().is == yarn::Event::Is::OPTIONS;
}

std::span<const DialogueOption> Dialogue::options() const {
    if (!has_options()) return {};
    return _state->options;
}

bool Dialogue::has_command() const {
    return running() && _state->machine.event().is == yarn::Event::Is::COMMAND;
}

std::string_view Dialogue::command() const {
    if (!has_command()) return {};
    return _state->command;
}

void Dialogue::advance() {
    if (!running()) return;
    _state->machine.advance();
    refresh(*_state);
}

void Dialogue::choose(int index) {
    if (!running()) return;
    _state->machine.choose(index);
    refresh(*_state);
}

void Dialogue::on_command(std::string_view name,
                          rmp::Callback<std::span<const std::string>> handler) {
    if (_state) _state->machine.on_command(name, std::move(handler));
}

void Dialogue::on_function(std::string_view name,
                           rmp::Callback<DialogueValue &, std::span<const DialogueValue>> function) {
    if (!_state) return;
    // The game's function in the game's types, around the machine's.
    auto shared = std::make_shared<rmp::Callback<DialogueValue &, std::span<const DialogueValue>>>(
        std::move(function));
    _state->machine.on_function(name, [shared](yarn::Value &out, std::span<const yarn::Value> args) {
        std::vector<DialogueValue> in;
        in.reserve(args.size());
        for (const yarn::Value &a : args) in.push_back(from_yarn(a));
        DialogueValue result;
        (*shared)(result, in);
        out = to_yarn(result);
    });
}

DialogueValue Dialogue::variable(std::string_view name) const {
    if (!_state) return {};
    return from_yarn(_state->machine.variable(name));
}

void Dialogue::set_variable(std::string_view name, DialogueValue value) {
    if (_state) _state->machine.set_variable(name, to_yarn(value));
}

int Dialogue::visits(std::string_view node) const {
    return _state ? _state->machine.visits(node) : 0;
}

void Dialogue::save(rmp::Value &out) const {
    out = rmp::Value::object();
    if (!_state) return;
    for (const auto &[name, v] : _state->machine.variables()) {
        switch (v.kind) {
            case yarn::Kind::NUMBER: out[name] = v.number; break;
            case yarn::Kind::BOOL: out[name] = v.boolean; break;
            case yarn::Kind::STRING: out[name] = v.text; break;
        }
    }
}

void Dialogue::load(const rmp::Value &in) {
    if (!_state) return;
    std::map<std::string, yarn::Value> vars;
    for (int i = 0; i < in.size(); ++i) {
        const std::string name(in.key(i));
        if (name.empty()) continue;
        const rmp::Value &v = in[name];
        switch (v.type()) {
            case rmp::Value::Type::BOOL: vars[name] = yarn::Value::of(v.as_bool()); break;
            case rmp::Value::Type::NUMBER: vars[name] = yarn::Value::of(v.as_float()); break;
            case rmp::Value::Type::STRING: vars[name] = yarn::Value::of(std::string(v.as_string())); break;
            default: break;
        }
    }
    _state->machine.set_variables(std::move(vars));
}

void Dialogue::set_saliency(DialogueSaliency saliency) {
    if (!_state) return;
    switch (saliency) {
        case DialogueSaliency::FIRST: _state->machine.set_saliency(yarn::Saliency::FIRST); break;
        case DialogueSaliency::BEST: _state->machine.set_saliency(yarn::Saliency::BEST); break;
        case DialogueSaliency::LEAST_SEEN:
            _state->machine.set_saliency(yarn::Saliency::BEST_LEAST_RECENTLY_VIEWED);
            break;
        case DialogueSaliency::RANDOM_LEAST_SEEN:
            _state->machine.set_saliency(yarn::Saliency::RANDOM_BEST_LEAST_RECENTLY_VIEWED);
            break;
    }
}

namespace dialogue::detail {

void set_embedded(std::span<const Embedded> table) {
    embedded.table.assign(table.begin(), table.end());
}

Dialogue compile_text(std::string_view source, std::string_view file) {
    return from_sources({ { std::string(file), std::string(source) } });
}

Dialogue from_program(std::span<const std::uint8_t> bytes) {
    auto state = std::make_unique<rmp::detail::DialogueState>();
    if (std::optional<yarn::Program> p = yarn::from_bytes(bytes)) {
        state->machine = yarn::Machine(std::move(*p));
        state->loaded = true;
    } else {
        state->errors.emplace_back("the embedded dialogue is damaged: rebuild the game");
    }
    return Dialogue(std::move(state));
}

} // namespace dialogue::detail

namespace assets {

Dialogue load_dialogue(std::string_view file) {
    const std::array<std::string_view, 1> one{ file };
    return load_dialogue(std::span<const std::string_view>(one));
}

Dialogue load_dialogue(std::span<const std::string_view> files) {
    // Built in, when the game was built with [dialogue] build = "embedded".
    if (files.size() == 1) {
        for (const auto &e : embedded.table) {
            if (e.name == files[0]) return rmp::dialogue::detail::from_program(e.bytes);
        }
    }
    std::vector<yarn::Source> sources;
    for (const std::string_view f : files) {
        const std::vector<unsigned char> bytes = load_data(f);
        if (bytes.empty()) {
            RMP_REPORT_ONCE_KEYED(f, "DIALOGUE: load_dialogue(\"%.*s\"): no such file in resources/",
                                  static_cast<int>(f.size()), f.data());
            auto state = std::make_unique<rmp::detail::DialogueState>();
            state->errors.push_back(std::string(f) + ": no such file in resources/");
            return Dialogue(std::move(state));
        }
        sources.push_back({ std::string(f), std::string(bytes.begin(), bytes.end()) });
    }
    return from_sources(sources);
}

} // namespace assets

} // namespace rmp
