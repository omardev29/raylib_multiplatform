// ---------------------------------------------------------------------------
// rmp::assets held to its promises over generated cases (package P4):
//
//   - the resource table against a model of who holds what: copies share,
//     moves steal, the last one out unloads, and release_all() leaves every
//     handle reading empty -- and none of them holding the slot of a resource
//     loaded after it;
//   - the pack against the loose files: whatever the files and whatever the
//     name asked for, a release that reads resources.rres hands back what a
//     development build reading resources/ does -- load_data(), and raylib's
//     own LoadFileData() through the loader hook;
//   - the pack reader round trip: every file packed, plain or encrypted,
//     reads back byte for byte with its password and not at all without it;
//     a pack cut short is refused whole; a byte changed anywhere is refused or
//     changes nothing;
//   - which paths the loader hook answers for: a file directly in the
//     resources folder, however the path is spelled, and nothing below it;
//   - the loaders under any name at all: a name that is nowhere is an empty
//     resource, counted as requested and failed, and not reported -- but for
//     a name with a NUL in it, which is no file's, and is said once.
//
// Every property here was seen red: the commit that added this file says how.
// ---------------------------------------------------------------------------

// covers: rmp::assets::load_data(std::string_view)
// covers: rmp::assets::load_image(std::string_view)
// covers: rmp::assets::load_texture(std::string_view)
// covers: rmp::assets::load_font(std::string_view, int)
// covers: rmp::assets::load_sound(std::string_view)
// covers: rmp::assets::requested_loads()
// covers: rmp::assets::failed_loads()
// covers: rmp::assets::init()
// covers: rmp::assets::shutdown()
// covers: rmp::assets::using_pack()

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/pack_internal.h"
#include "../pack_writer.h"

#include <rmp/assets.h>
#include <rmp/config.h>

#include <doctest.h>
#include <raylib.h>

#include <algorithm>
#include <climits>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace pack = rmp::test::pack;
namespace rres = rmp::assets::detail::rres;
using rmp::detail::ResourceKind;

constexpr std::string_view PASSWORD = RMP_RRES_PASSWORD;

rres::Key cheap(std::string_view password, const rres::Salt &salt) {
    return pack::cheap(password, salt);
}

// A folder of this process's own under the temporary directory, gone with it.
class Folder {
public:
    explicit Folder(std::string_view name) {
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) /
            (std::string("rmp-prop-assets-") + std::string(name) + "-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
    }
    Folder(const Folder &) = delete;
    Folder &operator=(const Folder &) = delete;
    Folder(Folder &&) = delete;
    Folder &operator=(Folder &&) = delete;
    ~Folder() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }
    [[nodiscard]] const std::filesystem::path &path() const { return _path; }
    // The folder as a resources root: ending in its separator.
    [[nodiscard]] std::string root() const { return (_path / "").string(); }

private:
    std::filesystem::path _path;
};

// The resources root, put back however the property ends.
class AtRoot {
public:
    explicit AtRoot(const std::string &root)
        : _previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(root.c_str());
    }
    AtRoot(const AtRoot &) = delete;
    AtRoot &operator=(const AtRoot &) = delete;
    AtRoot(AtRoot &&) = delete;
    AtRoot &operator=(AtRoot &&) = delete;
    ~AtRoot() { rmp::assets::detail::set_resources_root(_previous.c_str()); }

private:
    std::string _previous;
};

bool write_file(const std::filesystem::path &path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return pack::save(path, pack::bytes_of(bytes));
}

// --- the resource table ------------------------------------------------------

constexpr int HANDLES = 4;
constexpr std::array<std::string_view, 3> TABLE_NAMES = { "a.png", "b.png", "c.png" };

enum class Op {
    LOAD,
    COPY_ASSIGN,
    MOVE_ASSIGN,
    RESET,
    PUSH_COPY,
    PUSH_MOVE,
    POP,
    RELEASE_ALL
};

struct Step {
    Op op = Op::LOAD;
    int a = 0;
    int b = 0;
};

// RapidCheck finds these by their name, in the type's namespace.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Step &s, std::ostream &os) {
    constexpr std::array<std::string_view, 8> NAMES = { "load",        "copy_assign",
                                                        "move_assign", "reset",
                                                        "push_copy",   "push_move",
                                                        "pop",         "release_all" };
    os << NAMES[static_cast<std::size_t>(s.op)] << "(" << s.a << ", " << s.b << ")";
}

rc::Gen<Step> step() {
    return rc::gen::build<Step>(
        rc::gen::set(&Step::op,
                     rc::gen::element(Op::LOAD, Op::COPY_ASSIGN, Op::MOVE_ASSIGN,
                                      Op::RESET, Op::PUSH_COPY, Op::PUSH_MOVE, Op::POP,
                                      Op::RELEASE_ALL)),
        rc::gen::set(&Step::a, rc::gen::inRange(0, HANDLES)),
        rc::gen::set(&Step::b, rc::gen::inRange(0, HANDLES)));
}

// What a handle holds, in the model: nothing, a live resource (its serial), or
// a slot release_all() emptied under it -- valid(), and reading empty.
struct Held {
    enum class State { EMPTY, LIVE, STALE } state = State::EMPTY;
    int serial = 0;
};

struct Model {
    std::vector<Held> handles = std::vector<Held>(HANDLES);
    std::vector<Held> pile;
    std::vector<int> name_of; // a resource's name, by serial

    template <class F> void each(F &&f) const {
        for (const Held &h : handles) f(h);
        for (const Held &h : pile) f(h);
    }
    [[nodiscard]] int refs(int serial) const {
        int n = 0;
        each([&](const Held &h) {
            n += h.state == Held::State::LIVE && h.serial == serial;
        });
        return n;
    }
    // The serial of the live resource called `name`, or -1.
    [[nodiscard]] int live_named(int name) const {
        int found = -1;
        each([&](const Held &h) {
            if (h.state == Held::State::LIVE &&
                name_of[static_cast<std::size_t>(h.serial)] == name)
                found = h.serial;
        });
        return found;
    }
};

// What the loaders do: the live resource by that name, counted once more, or a
// new one -- whose picture is as wide as its serial, so a handle says which
// resource it reads.
rmp::Image load(int name, int serial) {
    const std::string_view key = TABLE_NAMES[static_cast<std::size_t>(name)];
    rmp::Image shared{ rmp::detail::acquire_named(ResourceKind::IMAGE, key, 0) };
    if (shared.valid()) return shared;
    ::Image marked{};
    marked.width = serial;
    return rmp::Image{ rmp::detail::adopt_named(ResourceKind::IMAGE, key, 0, marked) };
}

void agree(const Model &model, const std::vector<rmp::Image> &handles,
           const std::vector<rmp::Image> &pile) {
    RC_ASSERT(pile.size() == model.pile.size());
    const auto same = [](const rmp::Image &real, const Held &held) {
        const bool valid = held.state != Held::State::EMPTY;
        const int width = held.state == Held::State::LIVE ? held.serial : 0;
        return real.valid() == valid && real.raw().width == width;
    };
    for (std::size_t i = 0; i < handles.size(); ++i)
        RC_ASSERT(same(handles[i], model.handles[i]));
    for (std::size_t i = 0; i < pile.size(); ++i) RC_ASSERT(same(pile[i], model.pile[i]));
    int live = 0;
    for (std::size_t serial = 1; serial < model.name_of.size(); ++serial)
        live += model.refs(static_cast<int>(serial)) > 0;
    RC_ASSERT(rmp::detail::live_count() == live);
    for (std::size_t name = 0; name < TABLE_NAMES.size(); ++name) {
        const int serial = model.live_named(static_cast<int>(name));
        RC_ASSERT(rmp::detail::ref_count(TABLE_NAMES[name]) ==
                  (serial < 0 ? 0 : model.refs(serial)));
    }
}

void play(const std::vector<Step> &steps) {
    rmp::detail::release_all();
    Model model;
    model.name_of.push_back(-1); // serial 0 is no resource
    std::vector<rmp::Image> handles(HANDLES);
    std::vector<rmp::Image> pile;
    const auto release = [](Held &held) { held = Held{}; };
    for (const Step &s : steps) {
        auto &a = handles[static_cast<std::size_t>(s.a)];
        auto &b = handles[static_cast<std::size_t>(s.b)];
        Held &ma = model.handles[static_cast<std::size_t>(s.a)];
        Held &mb = model.handles[static_cast<std::size_t>(s.b)];
        switch (s.op) {
            case Op::LOAD: {
                const int name = s.b % static_cast<int>(TABLE_NAMES.size());
                int serial = model.live_named(name);
                if (serial < 0) {
                    serial = static_cast<int>(model.name_of.size());
                    model.name_of.push_back(name);
                }
                a = load(name, serial);
                ma = { Held::State::LIVE, serial };
                break;
            }
            case Op::COPY_ASSIGN:
                b = a;
                mb = ma;
                break;
            case Op::MOVE_ASSIGN:
                b = std::move(a);
                if (s.a != s.b) {
                    mb = ma;
                    release(ma);
                }
                break;
            case Op::RESET:
                a = rmp::Image{};
                release(ma);
                break;
            case Op::PUSH_COPY:
                pile.push_back(a);
                model.pile.push_back(ma);
                break;
            case Op::PUSH_MOVE:
                pile.push_back(std::move(a));
                model.pile.push_back(ma);
                release(ma);
                break;
            case Op::POP:
                if (!pile.empty()) {
                    pile.pop_back();
                    model.pile.pop_back();
                }
                break;
            case Op::RELEASE_ALL:
                rmp::detail::release_all();
                for (Held &h : model.handles)
                    if (h.state == Held::State::LIVE) h.state = Held::State::STALE;
                for (Held &h : model.pile)
                    if (h.state == Held::State::LIVE) h.state = Held::State::STALE;
                break;
        }
        agree(model, handles, pile);
    }
    handles.clear();
    pile.clear();
    RC_ASSERT(rmp::detail::live_count() == 0);
}

// --- the pack and the loose files --------------------------------------------

// Names that are each other's prefixes, extensions and near misses: the ones a
// lookup that is not the whole name gets wrong.
constexpr std::array<std::string_view, 11> POOL = {
    "a",     "a.txt",     "a.txt.bak", "b.png",   "level1.json", "level10.json",
    "music", "music.ogg", ".hidden",   "x y.dat", "UPPER.TXT"
};

struct Files {
    std::vector<std::pair<std::string, std::string>> top; // packed in a release
    std::vector<std::pair<std::string, std::string>>
        sub; // resources/sub/, shipped beside it
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Files &files, std::ostream &os) {
    const auto list = [&](const auto &named, std::string_view where) {
        for (const auto &[name, bytes] : named)
            os << where << name << " (" << bytes.size() << " bytes) ";
    };
    list(files.top, "");
    list(files.sub, "sub/");
}

rc::Gen<std::string> content() {
    return rc::gen::container<std::string>(
        rc::gen::element<char>('a', 'b', '\0', '\n', '\xFF'));
}

rc::Gen<Files> files() {
    return rc::gen::exec([] {
        Files out;
        for (const std::string_view name : POOL) {
            if (*rc::gen::inRange(0, 3) == 0) out.top.emplace_back(name, *content());
            if (*rc::gen::inRange(0, 5) == 0) out.sub.emplace_back(name, *content());
        }
        return out;
    });
}

// What a game might ask for: a name of the pool spelled some way -- as it
// is, in the subfolder, with "./", in and out of the subfolder, out of the
// folder, as a folder -- nothing, or anything at all.
rc::Gen<std::string> pool_name() {
    return rc::gen::map(rc::gen::elementOf(POOL),
                        [](std::string_view name) { return std::string(name); });
}

rc::Gen<std::string> query() {
    return rc::gen::oneOf(
        pool_name(),
        rc::gen::map(rc::gen::pair(rc::gen::element<std::string>("sub/", "./", "sub/../",
                                                                 "../", "sub\\", "/", ""),
                                   pool_name()),
                     [](const std::pair<std::string, std::string> &p) {
                         return p.first + p.second;
                     }),
        rc::gen::map(pool_name(), [](const std::string &n) { return n + "/"; }),
        rc::gen::just(std::string()), rmp::test::gen::hostile_string());
}

struct Answer {
    std::vector<unsigned char> data; // load_data()
    std::optional<std::string> raw; // LoadFileData() of root + name, through the hook
    int requested = 0;
    int failed = 0;

    bool operator==(const Answer &) const = default;
};

std::optional<std::string> raylib_read(const std::string &path) {
    int size = 0;
    const std::unique_ptr<unsigned char, decltype(&UnloadFileData)> data(
        LoadFileData(path.c_str(), &size), &UnloadFileData);
    if (!data) return std::nullopt;
    return std::string(reinterpret_cast<const char *>(data.get()),
                       static_cast<std::size_t>(size));
}

Answer ask(const std::string &root, const std::string &name) {
    Answer out;
    const int requested = rmp::assets::requested_loads();
    const int failed = rmp::assets::failed_loads();
    out.data = rmp::assets::load_data(name);
    out.requested = rmp::assets::requested_loads() - requested;
    out.failed = rmp::assets::failed_loads() - failed;
    // What a C string of the name is: raylib reads up to the first NUL.
    out.raw = raylib_read(root + name.substr(0, name.find('\0')));
    return out;
}

// --- the pack reader ------------------------------------------------------------

struct Packed {
    std::vector<pack::File> files;
};

// A name the packer could have written: not empty, no NUL.
rc::Gen<std::string> file_name() {
    return rc::gen::nonEmpty(rc::gen::container<std::string>(
        rc::gen::suchThat(rc::gen::arbitrary<char>(), [](char c) { return c != '\0'; })));
}

// Up to eight files with names of their own -- a pack is read once per file,
// so a hundred of them made the reader's properties a twenty-second run --
// each with any bytes at all, plain or encrypted either way rres reads.
rc::Gen<std::vector<pack::File>> packed_files() {
    return rc::gen::exec([] {
        const auto names =
            *rc::gen::resize(8, rc::gen::unique<std::vector<std::string>>(file_name()));
        std::vector<pack::File> out;
        out.reserve(names.size());
        for (const std::string &name : names) {
            out.push_back({ name, *rc::gen::arbitrary<pack::Bytes>(),
                            *rc::gen::element(pack::CIPHER_NONE, pack::CIPHER_AES,
                                              pack::CIPHER_XCHACHA20_POLY1305) });
        }
        return out;
    });
}

// The bytes `name` reads to from memory with `password`, or nullopt.
std::optional<rres::Bytes> read_back(std::span<const std::uint8_t> bytes,
                                     const std::string &name, std::string_view password) {
    rres::MemorySource memory(bytes);
    const std::optional<rres::Index> index = rres::read_index(memory);
    if (!index) return std::nullopt;
    const std::optional<rres::Chunk> chunk = rres::find(*index, name);
    if (!chunk) return std::nullopt;
    const std::optional<rres::Contents> contents =
        rres::open_chunk(memory, *chunk, password, cheap);
    if (!contents) return std::nullopt;
    return rres::file_of(*contents);
}

// Any text made a plain file name: no separator, no NUL, not "." or "..",
// not empty -- what the hook answers for, directly in the folder.
std::string plain_name(std::string text) {
    std::erase_if(text, [](char c) { return c == '/' || c == '\\' || c == '\0'; });
    if (text.empty() || text == "." || text == "..") text = "x" + text;
    return text;
}

// Resources folders the way a game's may be spelled: relative, with "./",
// absolute, Android's empty one, a drive -- and on Windows, where the system
// reads a backslash as a separator, with backslashes.
const std::vector<std::string> &roots() {
    static const std::vector<std::string> SPELLED = {
        "resources/", "./res/", "/abs/game/resources/", "", "C:/g/r/",
#if defined(_WIN32)
        R"(C:\g\r\)",
#endif
    };
    return SPELLED;
}

// The log off for the length of a test case: a property asks for thousands of
// files that are not there, and raylib says so for each.
class Quiet {
public:
    Quiet() { SetTraceLogLevel(LOG_NONE); }
    Quiet(const Quiet &) = delete;
    Quiet &operator=(const Quiet &) = delete;
    Quiet(Quiet &&) = delete;
    Quiet &operator=(Quiet &&) = delete;
    ~Quiet() { SetTraceLogLevel(LOG_INFO); }
};

} // namespace

TEST_SUITE("property: assets") {
    TEST_CASE("the resource table: handles against a model of who holds what") {
        rmp::test::check(
            "handles share, steal, unload and outlive release_all() as the model says",
            [] { play(*rc::gen::container<std::vector<Step>>(step())); });
    }

    TEST_CASE("the pack against the loose files") {
        const Quiet quiet;
        rmp::test::check(
            "a release reading the pack answers every name as a development build does",
            [] {
                const Files generated = *files();
                const auto queries =
                    *rc::gen::container<std::vector<std::string>>(query());
                const Folder loose("loose");
                const Folder release("release");
                std::vector<pack::File> packed;
                for (const auto &[name, bytes] : generated.top) {
                    RC_ASSERT(write_file(loose.path() / name, bytes));
                    packed.push_back({ name, pack::bytes_of(bytes) });
                }
                // The subfolder is there in both, empty or not: "sub/../x" is
                // taken back lexically, which is what the system does when the
                // folders on the way exist.
                std::error_code error;
                std::filesystem::create_directories(loose.path() / "sub", error);
                std::filesystem::create_directories(release.path() / "sub", error);
                for (const auto &[name, bytes] : generated.sub) {
                    RC_ASSERT(write_file(loose.path() / "sub" / name, bytes));
                    RC_ASSERT(write_file(release.path() / "sub" / name, bytes));
                }
                RC_ASSERT(
                    pack::save(release.path() / "resources.rres", pack::write(packed)));

                for (const std::string &name : queries) {
                    if (name == "resources.rres")
                        continue; // the pack itself, which dev has not
                    Answer development;
                    {
                        const AtRoot at(loose.root());
                        development = ask(loose.root(), name);
                    }
                    Answer shipped;
                    {
                        const AtRoot at(release.root());
                        rmp::assets::init();
                        RC_ASSERT(rmp::assets::using_pack() == !packed.empty());
                        shipped = ask(release.root(), name);
                        rmp::assets::shutdown();
                        RC_ASSERT(!rmp::assets::using_pack());
                    }
                    RC_ASSERT(shipped.data == development.data);
                    RC_ASSERT(shipped.raw == development.raw);
                    RC_ASSERT(shipped.requested == development.requested);
                    RC_ASSERT(shipped.failed == development.failed);
                }
            });
    }

    TEST_CASE("the pack reader: round trip, truncation, one byte changed") {
        rmp::test::check(
            "every file packed reads back byte for byte with its password, and an "
            "encrypted one not at all with another",
            [] {
                const auto all = *packed_files();
                const pack::Bytes bytes = pack::write(all, { .password = PASSWORD });
                for (const pack::File &file : all) {
                    RC_ASSERT(read_back(bytes, file.name, PASSWORD) == file.bytes);
                    const auto wrong = read_back(bytes, file.name, "not the password");
                    if (file.cipher == pack::CIPHER_NONE) {
                        RC_ASSERT(wrong == file.bytes);
                    } else {
                        RC_ASSERT(!wrong.has_value());
                    }
                }
            });

        rmp::test::check("a pack cut short anywhere is refused whole", [] {
            const pack::Bytes bytes = pack::write(*packed_files());
            const auto cut = *rc::gen::inRange<std::size_t>(0, bytes.size());
            rres::MemorySource memory(std::span(bytes).first(cut));
            RC_ASSERT(!rres::read_index(memory).has_value());
        });

        rmp::test::check(
            "a byte changed anywhere in a pack is refused, or changes nothing it reads",
            [] {
                const auto all = *rc::gen::nonEmpty(packed_files());
                pack::Bytes bytes = pack::write(all, { .password = PASSWORD });
                const auto at = *rc::gen::inRange<std::size_t>(0, bytes.size());
                const std::uint8_t was = bytes[at];
                bytes[at] =
                    *rc::gen::suchThat(rc::gen::arbitrary<std::uint8_t>(),
                                       [was](std::uint8_t b) { return b != was; });
                for (const pack::File &file : all) {
                    const auto read = read_back(bytes, file.name, PASSWORD);
                    // One bool: RC_ASSERT captures both sides of an ||.
                    const bool refused_or_same = !read.has_value() || *read == file.bytes;
                    RC_ASSERT(refused_or_same);
                }
            });
    }

    TEST_CASE("which paths the loader hook answers for") {
        rmp::test::check(
            "a file directly in the resources folder, however it is spelled, and nothing "
            "below it",
            [] {
                const std::string name = plain_name(*rmp::test::gen::hostile_string());
                for (const std::string &root : roots()) {
                    const AtRoot at(root);
                    using rmp::assets::detail::in_resources_dir;
                    const auto in = [&](std::string_view between,
                                        std::string_view after) {
                        std::string path = root;
                        path += between;
                        path += name;
                        path += after;
                        return in_resources_dir(path.c_str());
                    };
                    RC_ASSERT(in("", ""));
                    RC_ASSERT(in("./", ""));
                    RC_ASSERT(in("sub/../", ""));
                    RC_ASSERT(in(R"(sub\..\)", ""));
                    RC_ASSERT(!in("sub/", ""));
                    RC_ASSERT(!in("../", ""));
                    RC_ASSERT(!in("", "/" + name));
                    RC_ASSERT(!in("", "/"));
                }
            });
    }

    TEST_CASE("the loaders under any name at all") {
        const Quiet quiet;
        rmp::test::check(
            "a name that is nowhere is an empty resource, counted, and reported only "
            "when it has a NUL in it",
            [] {
                const std::string name = *rmp::test::gen::hostile_string();
                const int font_size = *rc::gen::inRange(1, 257);
                const Folder nothing("nothing");
                const AtRoot at(nothing.root());
                rmp::detail::reset_reports_for_tests();
                const int requested = rmp::assets::requested_loads();
                const int failed = rmp::assets::failed_loads();
                RC_ASSERT(!rmp::assets::load_image(name).valid());
                RC_ASSERT(!rmp::assets::load_texture(name).valid());
                RC_ASSERT(!rmp::assets::load_font(name, font_size).valid());
                RC_ASSERT(rmp::assets::load_data(name).empty());
                RC_ASSERT(rmp::assets::requested_loads() - requested == 4);
                RC_ASSERT(rmp::assets::failed_loads() - failed == 4);
                RC_ASSERT(!rmp::assets::load_sound(name).valid());
                // A NUL makes it no file name at all -- everything that opens
                // a file would open what comes before it -- and that is said,
                // once, whichever loaders it went to; any other name that is
                // nowhere is just not there.
                const bool nul = name.find('\0') != std::string::npos;
                RC_ASSERT(rmp::detail::report_count() == (nul ? 1 : 0));
            });
    }

    TEST_CASE("a font size that is not one") {
        const Quiet quiet;
        rmp::test::check("a font size outside 1 to 1024 is an empty font, counted and "
                         "said once a name",
                         [] {
                             const std::string name = *rmp::test::gen::hostile_string();
                             const int size =
                                 *rc::gen::oneOf(rc::gen::inRange(INT_MIN, 1),
                                                 rc::gen::inRange(1025, INT_MAX));
                             rmp::detail::reset_reports_for_tests();
                             const int requested = rmp::assets::requested_loads();
                             const int failed = rmp::assets::failed_loads();
                             RC_ASSERT(!rmp::assets::load_font(name, size).valid());
                             RC_ASSERT(!rmp::assets::load_font(name, size).valid());
                             RC_ASSERT(rmp::assets::requested_loads() - requested == 2);
                             RC_ASSERT(rmp::assets::failed_loads() - failed == 2);
                             RC_ASSERT(rmp::detail::report_count() == 1);
                         });
    }

} // TEST_SUITE
