// ===========================================================================
// rmp_yarnc -- the framework's Yarn compiler, on the build machine.
//
//     rmp_yarnc OUT.cpp RESOURCES_DIR a.yarn talk/b.yarn ...
//     rmp_yarnc --check [--function NAME=TYPES>RESULT ...] a.yarn b.yarn ...
//
// Compiles each .yarn of a game's resources/ with the compiler the game
// itself carries (src/rmp/dialogue/), and writes OUT.cpp: each program as
// bytes and the table rmp::assets::load_dialogue() looks in first. Built and
// run by CMake when [dialogue] build is "embedded" (or "auto" in a release):
// a script with an error fails the build, with file:line:column.
//
// --check is `rmp yarn check`'s: each file compiled, its errors said as
// file:line:column, and what it uses printed -- "function FILE NAME" and
// "command FILE NAME", a line each -- for tools/yarn_tool.py to hold against
// the game's C++. Each --function is one the game registers, typed for the
// checker ("has_item=string,number>bool"; "" or "any" is any type).
// ===========================================================================

#include "../src/rmp/dialogue/yarn.h"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

bool read(const std::string &path, std::string &out) {
    const std::unique_ptr<std::FILE, int (*)(std::FILE *)> f(std::fopen(path.c_str(), "rb"),
                                                             &std::fclose);
    if (!f) return false;
    std::array<char, 4096> buffer{};
    std::size_t n = 0;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), f.get())) > 0) out.append(buffer.data(), n);
    return true;
}

std::string escape(const std::string &s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::vector<std::string> split(std::string_view s, char by) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from <= s.size()) {
        const std::size_t at = s.find(by, from);
        const std::size_t end = at == std::string_view::npos ? s.size() : at;
        out.emplace_back(s.substr(from, end - from));
        if (at == std::string_view::npos) break;
        from = at + 1;
    }
    return out;
}

// "name=string,number>bool" as the checker's FunctionType.
std::optional<rmp::yarn::FunctionType> function_type(std::string_view spec) {
    const std::size_t eq = spec.find('=');
    const std::size_t gt = spec.rfind('>');
    if (eq == std::string_view::npos || gt == std::string_view::npos || gt < eq) return {};
    rmp::yarn::FunctionType f;
    f.name = std::string(spec.substr(0, eq));
    const std::string_view params = spec.substr(eq + 1, gt - eq - 1);
    if (!params.empty()) {
        for (std::string p : split(params, ',')) f.params.push_back(p == "any" ? "" : p);
    }
    f.result = std::string(spec.substr(gt + 1));
    if (f.result == "any") f.result.clear();
    return f;
}

int check(std::span<char *> args) {
    std::vector<rmp::yarn::FunctionType> functions;
    std::vector<std::string> files;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--function" && i + 1 < args.size()) {
            const std::optional<rmp::yarn::FunctionType> f = function_type(args[++i]);
            if (!f) {
                std::fprintf(stderr, "rmp_yarnc: --function %s: want NAME=TYPES>RESULT\n",
                             args[i]);
                return 2;
            }
            functions.push_back(*f);
        } else {
            files.emplace_back(a);
        }
    }
    int failed = 0;
    for (const std::string &name : files) {
        std::string text;
        if (!read(name, text)) {
            std::fprintf(stderr, "rmp_yarnc: cannot read %s\n", name.c_str());
            ++failed;
            continue;
        }
        const std::vector<rmp::yarn::Source> sources{ { name, text } };
        const rmp::yarn::Compiled c = rmp::yarn::compile(sources, functions);
        for (const rmp::yarn::Diagnostic &d : c.errors) {
            std::fprintf(stderr, "%s\n", d.str().c_str());
        }
        if (!c.ok()) {
            ++failed;
            continue;
        }
        for (const std::string &f : rmp::yarn::functions_called(c.program)) {
            if (!rmp::yarn::is_builtin_function(f))
                std::printf("function %s %s\n", name.c_str(), f.c_str());
        }
        for (const std::string &cmd : rmp::yarn::commands_run(c.program)) {
            std::printf("command %s %s\n", name.c_str(), cmd.c_str());
        }
    }
    if (failed > 0) {
        std::fprintf(stderr, "rmp_yarnc: %d script(s) did not compile\n", failed);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const std::span<char *> all(argv, static_cast<std::size_t>(argc));
    if (argc >= 2 && std::string_view(argv[1]) == "--check") return check(all.subspan(2));
    if (argc < 3) {
        std::fprintf(stderr, "usage: rmp_yarnc OUT.cpp RESOURCES_DIR [file.yarn ...]\n");
        return 2;
    }
    const std::string out_path = argv[1];
    const std::string root = argv[2];
    std::string code = "// Generated by rmp_yarnc from resources/**/*.yarn. Do not edit.\n"
                       "#include \"dialogue/dialogue_internal.h\"\n\n"
                       "#include <array>\n#include <cstdint>\n\nnamespace {\n";
    std::string table;
    int failed = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string name = argv[i];
        std::string text;
        if (!read(root + "/" + name, text)) {
            std::fprintf(stderr, "rmp_yarnc: cannot read %s/%s\n", root.c_str(), name.c_str());
            ++failed;
            continue;
        }
        const std::vector<rmp::yarn::Source> sources{ { name, text } };
        const rmp::yarn::Compiled c = rmp::yarn::compile(sources);
        for (const rmp::yarn::Diagnostic &d : c.errors) {
            std::fprintf(stderr, "%s/%s\n", root.c_str(), d.str().c_str());
        }
        if (!c.ok()) {
            ++failed;
            continue;
        }
        const std::vector<std::uint8_t> bytes = rmp::yarn::to_bytes(c.program);
        const std::string var = "PROGRAM_" + std::to_string(i);
        code += "constexpr std::array<std::uint8_t, " + std::to_string(bytes.size()) + "> " + var +
                "{";
        for (std::size_t k = 0; k < bytes.size(); ++k) {
            if (k % 24 == 0) code += "\n    ";
            code += std::to_string(bytes[k]) + ",";
        }
        code += "\n};\n";
        table += "    rmp::dialogue::detail::Embedded{ \"" + escape(name) + "\", " + var + " },\n";
    }
    if (failed > 0) {
        std::fprintf(stderr, "rmp_yarnc: %d script(s) did not compile\n", failed);
        return 1;
    }
    code += "\n} // namespace\n\nnamespace rmp::dialogue::detail {\n\n"
            "std::span<const Embedded> embedded_programs() {\n";
    if (table.empty()) {
        code += "    return {};\n";
    } else {
        code += "    static const std::array TABLE{\n" + table + "    };\n    return TABLE;\n";
    }
    code += "}\n\n} // namespace rmp::dialogue::detail\n";
    const std::unique_ptr<std::FILE, int (*)(std::FILE *)> f(std::fopen(out_path.c_str(), "wb"),
                                                             &std::fclose);
    if (!f || std::fwrite(code.data(), 1, code.size(), f.get()) != code.size()) {
        std::fprintf(stderr, "rmp_yarnc: cannot write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}
