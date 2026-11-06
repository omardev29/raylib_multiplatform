// ---------------------------------------------------------------------------
// Yarn Spinner's own test suite, against our compiler and machine.
//
// tests/dialogue/yarn/TestCases is the official repository's Tests/TestCases
// (MIT; the commit in tests/dialogue/yarn/COMMIT): each .yarn with a
// .testplan beside it compiles and runs exactly as its plan says -- every
// line's text after its substitutions and markup, every option and whether
// it can be chosen, every command, the stop -- and each one without a plan,
// and every file of ParseFailures/, fails to compile. A file that does not
// yet is in tests/dialogue/yarn_unsupported.txt with the reason, a list that
// only shrinks: one listed that passes fails here until its line goes.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/dialogue/yarn.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace y = rmp::yarn;

fs::path data_root() { return fs::path(__FILE__).parent_path() / "dialogue"; }

std::string read_file(const fs::path &p) {
    const std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::map<std::string, std::string> unsupported() {
    std::map<std::string, std::string> out;
    std::istringstream in(read_file(data_root() / "yarn_unsupported.txt"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t sp = line.find(' ');
        out[line.substr(0, sp)] = sp == std::string::npos ? "" : line.substr(sp + 1);
    }
    return out;
}

struct Step {
    std::string kind; // line option command stop select set node saliency start
    std::string text;
    bool any = false;
    bool disabled = false;
    std::vector<std::string> tags;
    int number = 0;
    std::string name;
    std::string value;
};

std::vector<std::vector<Step>> read_plan(const std::string &source) {
    std::vector<std::vector<Step>> runs(1);
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line.starts_with("//")) continue;
        if (line.starts_with("---")) {
            runs.emplace_back();
            continue;
        }
        Step s;
        const std::size_t colon = line.find(':');
        s.kind = line.substr(0, colon == std::string::npos ? line.size() : colon);
        while (!s.kind.empty() && (s.kind.back() == ' ' || s.kind.back() == '\t')) s.kind.pop_back();
        if (const std::size_t c = s.kind.find("//"); c != std::string::npos) s.kind.resize(c);
        while (!s.kind.empty() && (s.kind.back() == ' ' || s.kind.back() == '\t')) s.kind.pop_back();
        std::string rest = colon == std::string::npos ? "" : line.substr(colon + 1);
        const std::size_t tick = rest.find('`');
        if (tick != std::string::npos) {
            const std::size_t end = rest.find('`', tick + 1);
            s.text = rest.substr(tick + 1, end - tick - 1);
            rest = rest.substr(end + 1);
        } else if (rest.find('*') != std::string::npos && (s.kind == "line" || s.kind == "option")) {
            s.any = true;
        }
        std::istringstream words(rest);
        std::string w;
        while (words >> w) {
            if (w.starts_with("//")) break;
            if (w == "[disabled]") {
                s.disabled = true;
            } else if (w.starts_with("#")) {
                s.tags.push_back(w.substr(1));
            } else if (s.kind == "select") {
                s.number = std::stoi(w);
            } else if (s.kind == "set") {
                if (s.name.empty()) {
                    s.name = w;
                } else if (w != "=") {
                    s.value = w;
                }
            } else if (s.kind == "node" || s.kind == "start" || s.kind == "saliency" ||
                       s.kind == "environment") {
                s.name = w;
            }
        }
        runs.back().push_back(std::move(s));
    }
    return runs;
}

std::string composed(const y::Program &p, const std::string &id,
                     const std::vector<std::string> &subs) {
    const int index = p.line_index(id);
    if (index < 0) return "<no line " + id + ">";
    int shown = index;
    const std::string &shadow = p.lines[static_cast<std::size_t>(index)].shadow;
    if (!shadow.empty() && p.line_index(shadow) >= 0) shown = p.line_index(shadow);
    return y::parse_markup(y::expand(p.lines[static_cast<std::size_t>(shown)].text, subs), "en").text;
}

std::vector<std::string> tags_of(const y::Program &p, const std::string &id) {
    const int index = p.line_index(id);
    return index < 0 ? std::vector<std::string>{} : p.lines[static_cast<std::size_t>(index)].tags;
}

// Runs one plan; an empty string when it went as planned.
std::string play(const y::Program &program, const std::vector<std::vector<Step>> &runs) {
    y::Machine m(program);
    std::string assert_failed;
    m.on_function("assert", [&](y::Value &out, std::span<const y::Value> args) {
        if (args.empty() || !args[0].as_bool()) assert_failed = "assert() was false";
        out = y::Value::of(true);
    });
    // What LanguageTests.cs registers.
    m.on_function("add_three_operands", [](y::Value &out, std::span<const y::Value> a) {
        out = y::Value::of(a[0].as_number() + a[1].as_number() + a[2].as_number());
    });
    m.on_function("set_objective_complete",
                  [](y::Value &out, std::span<const y::Value>) { out = y::Value::of(true); });
    m.on_function("is_objective_active",
                  [](y::Value &out, std::span<const y::Value>) { out = y::Value::of(true); });
    m.on_function("get_quest_status", [](y::Value &out, std::span<const y::Value>) {
        out = y::Value::of(std::string("InProgress"));
    });
    m.on_function("dummy_bool", [](y::Value &out, std::span<const y::Value>) { out = y::Value::of(true); });
    m.on_function("dummy_number", [](y::Value &out, std::span<const y::Value>) { out = y::Value::of(1.0f); });
    m.on_function("dummy_string", [](y::Value &out, std::span<const y::Value>) {
        out = y::Value::of(std::string("string"));
    });
    const std::map<std::string, y::Value> storage;
    std::vector<Step> expected_options;
    for (const auto &run : runs) {
        if (run.empty()) continue;
        std::string node = "Start";
        bool started = false;
        const auto ensure = [&]() {
            if (started) return;
            m.start(node);
            started = true;
        };
        for (const Step &s : run) {
            const y::Event &e = m.event();
            if (s.kind == "environment") continue;
            if (s.kind == "start" || s.kind == "node") {
                node = s.name;
                started = false;
                if (s.kind == "node") {
                    ensure();
                }
                continue;
            }
            if (s.kind == "saliency") {
                if (s.name == "first") {
                    m.set_saliency(y::Saliency::FIRST);
                } else if (s.name == "best") {
                    m.set_saliency(y::Saliency::BEST);
                } else {
                    m.set_saliency(y::Saliency::BEST_LEAST_RECENTLY_VIEWED);
                }
                continue;
            }
            if (s.kind == "set") {
                if (s.value == "true" || s.value == "false") {
                    m.set_variable(s.name, y::Value::of(s.value == "true"));
                } else {
                    m.set_variable(s.name, y::Value::of(std::stof(s.value)));
                }
                continue;
            }
            if (s.kind == "option") {
                expected_options.push_back(s);
                continue;
            }
            ensure();
            if (!assert_failed.empty()) return assert_failed;
            const y::Event &now = m.event();
            (void)e;
            if (s.kind == "line") {
                if (now.is != y::Event::Is::LINE) return "expected line `" + s.text + "`, got another event";
                const std::string got = composed(program, now.id, now.substitutions);
                if (!s.any && got != s.text) return "expected line `" + s.text + "`, got `" + got + "`";
                const auto tags = tags_of(program, now.id);
                for (const std::string &t : s.tags) {
                    // NOLINTNEXTLINE(performance-inefficient-string-concatenation): a failure's message
                    if (std::ranges::find(tags, t) == tags.end()) return "line `" + got + "` lacks #" + t;
                }
                m.advance();
            } else if (s.kind == "select") {
                if (now.is != y::Event::Is::OPTIONS) {
                    const std::string what = now.is == y::Event::Is::LINE
                        ? "line `" + composed(program, now.id, now.substitutions) + "`"
                        : now.is == y::Event::Is::DONE ? "stop"
                                                       : "command `" + now.command + "`";
                    return "expected options, got " + what;
                }
                if (now.options.size() != expected_options.size()) {
                    return "expected " + std::to_string(expected_options.size()) + " options, got " +
                           std::to_string(now.options.size());
                }
                for (std::size_t k = 0; k < now.options.size(); ++k) {
                    const auto &o = now.options[k];
                    const std::string got = composed(program, o.id, o.substitutions);
                    if (!expected_options[k].any && got != expected_options[k].text) {
                        return "expected option `" + expected_options[k].text + "`, got `" + got + "`";
                    }
                    if (o.available == expected_options[k].disabled) {
                        return "option `" + got + "` availability is wrong";
                    }
                }
                expected_options.clear();
                m.choose(s.number - 1);
            } else if (s.kind == "command") {
                if (now.is != y::Event::Is::COMMAND) return "expected command `" + s.text + "`";
                if (now.command != s.text) return "expected command `" + s.text + "`, got `" + now.command + "`";
                m.advance();
            } else if (s.kind == "stop") {
                if (now.is != y::Event::Is::DONE) {
                    const std::string what = now.is == y::Event::Is::LINE
                        ? "line `" + composed(program, now.id, now.substitutions) + "`"
                        : "another event";
                    return "expected stop, got " + what;
                }
                break;
            } else {
                return "unknown step " + s.kind;
            }
        }
    }
    return assert_failed;
}

std::string compile_errors(const y::Compiled &c) {
    std::string out;
    for (const auto &e : c.errors) out += e.str() + "\n";
    return out;
}

} // namespace

TEST_SUITE("dialogue: Yarn Spinner's own tests") {
    TEST_CASE("every test case runs as its plan says, or fails as it should") {
        const auto skip = unsupported();
        const fs::path cases = data_root() / "yarn" / "TestCases";
        std::vector<fs::path> files;
        for (const auto &e : fs::directory_iterator(cases)) {
            if (e.path().extension() == ".yarn") files.push_back(e.path());
        }
        for (const auto &e : fs::directory_iterator(cases / "ParseFailures")) {
            if (e.path().extension() == ".yarn") files.push_back(e.path());
        }
        std::ranges::sort(files);
        REQUIRE(files.size() > 60);
        int passed = 0;
        std::set<std::string> seen;
        for (const fs::path &f : files) {
            const std::string key =
                (f.parent_path().filename() == "ParseFailures" ? "ParseFailures/" : "") +
                f.filename().string();
            seen.insert(key);
            const std::vector<y::Source> sources{ { f.filename().string(), read_file(f) } };
            const std::vector<y::FunctionType> functions{
                { "add_three_operands", { "number", "number", "number" }, "number" },
                { "set_objective_complete", { "string" }, "bool" },
                { "is_objective_active", { "string" }, "bool" },
                { "get_quest_status", { "string" }, "string" },
            };
            const y::Compiled c = y::compile(sources, functions);
            fs::path plan = f;
            plan.replace_extension(".testplan");
            std::string problem;
            if (!fs::exists(plan)) {
                if (c.ok()) problem = "compiled, and it should not";
            } else if (!c.ok()) {
                problem = "did not compile:\n" + compile_errors(c);
            } else {
                problem = play(c.program, read_plan(read_file(plan)));
            }
            const bool listed = skip.contains(key);
            if (listed) {
                CHECK_MESSAGE(!problem.empty(), key << " passes now: take its line out of "
                                                       "tests/dialogue/yarn_unsupported.txt");
            } else {
                CHECK_MESSAGE(problem.empty(), key << ": " << problem);
                if (problem.empty()) ++passed;
            }
        }
        for (const auto &[key, why] : skip) {
            CHECK_MESSAGE(seen.contains(key), key << " is listed and is not a test case");
        }
        MESSAGE("Yarn test cases passing: " << passed << " of " << files.size());
    }
}
