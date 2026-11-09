// ---------------------------------------------------------------------------
// The rule that decides which name in the pack a path means, fuzzed:
// rres::packed_name() (src/rmp/assets/pack.cpp), which the loader hook asks of every
// path raylib reads and the loaders of root + the name they were given.
//
// An input is a resources root and a path, split at the first newline (no
// newline: the root is empty, as on Android). The oracle:
//
//   - it returns, whatever the bytes;
//   - it agrees with a second reading written with nothing in common:
//     std::filesystem's lexically_normal() -- the path is a file in the
//     root's folder or below it, whose last part is not "" "." or "..", and
//     the name is its path below the root, joined by '/';
//   - '\' and '/' are one separator where the system says so (Windows);
//   - in_resources_dir() is the same answer, under that root;
//   - a name it gives is a relative path with no "" "." or ".." in it, and
//     the root and that name find it again.
//
//   bash tools/fuzz.sh replay resource_path
//   bash tools/fuzz.sh run resource_path 60
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/assets/pack_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rmp::test::fuzz::require;
namespace rres = rmp::assets::detail::rres;

// What separates folders: '/', and '\\' where the system reads it as one.
#if defined(_WIN32)
constexpr std::string_view SEPARATORS = "/\\";
#else
constexpr std::string_view SEPARATORS = "/";
#endif

// The path with every separator written '/'.
std::string forward(std::string text) {
    for (char &c : text) {
        if (SEPARATORS.find(c) != std::string_view::npos) c = '/';
    }
    return text;
}

// The same question, asked of std::filesystem. A root or a path that is "."
// once normal is the folder itself, which lexically_normal spells "." and
// parent_path() spells "".
std::optional<std::string> model(const std::string &root, const std::string &path) {
    const std::string p = forward(path);
    const std::size_t last = p.find_last_of('/');
    const std::string tail = last == std::string::npos ? p : p.substr(last + 1);
    if (tail.empty() || tail == "." || tail == "..") return std::nullopt;
    // The parts of each, once normal, with "." and the empty name a trailing
    // separator leaves dropped: what is left is folders and the file.
    const auto parts = [](const std::string &text) {
        std::vector<std::string> out;
        for (const std::filesystem::path &part :
             std::filesystem::path(text).lexically_normal()) {
            const std::string s = part.generic_string();
            if (!s.empty() && s != ".") out.push_back(s);
        }
        return out;
    };
    const std::vector<std::string> folder = parts(forward(root));
    const std::vector<std::string> file = parts(p);
    if (file.size() <= folder.size() ||
        !std::equal(folder.begin(), folder.end(), file.begin()))
        return std::nullopt;
    std::string name;
    for (std::size_t i = folder.size(); i < file.size(); ++i) {
        if (file[i] == "..") return std::nullopt;
        if (!name.empty()) name += '/';
        name += file[i];
    }
    return name;
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    std::string text(input.begin(), input.end());
    // What a C string of it is: the root and the path reach the rule as one.
    text = text.substr(0, text.find('\0'));
    const std::size_t split = text.find('\n');
    const std::string root =
        split == std::string::npos ? std::string() : text.substr(0, split);
    const std::string path = split == std::string::npos ? text : text.substr(split + 1);

    const std::optional<std::string> name = rres::packed_name(root, path);
    require(name == model(root, path),
            "the rule is std::filesystem's lexically_normal()");
    require(rres::packed_name(forward(root), forward(path)) == name,
            "every separator the system has is '/'");

    rmp::assets::detail::set_resources_root(root.c_str());
    require(rmp::assets::detail::in_resources_dir(path.c_str()) == name.has_value(),
            "the loader hook answers for exactly those paths");
    if (name) {
        bool plain = !name->empty() && name->front() != '/' && name->back() != '/';
        for (std::size_t at = 0; plain && at <= name->size();) {
            const std::size_t end = std::min(name->find('/', at), name->size());
            const std::string_view part = std::string_view(*name).substr(at, end - at);
            plain = !part.empty() && part != "." && part != "..";
            at = end + 1;
        }
        require(plain, "a name in the pack is a relative path, its parts joined by '/'");
        const std::string again = root +
            (root.empty() || SEPARATORS.find(root.back()) != std::string_view::npos
                 ? ""
                 : "/") +
            *name;
        require(rres::packed_name(root, again) == name,
                "the root and the name find it again");
    }
    const std::string empty = (rmp::test::fuzz::detail::empty.folder / "").string();
    rmp::assets::detail::set_resources_root(empty.c_str());
    return 0;
}
