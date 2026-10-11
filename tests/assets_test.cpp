// ===========================================================================
// The asset layer, with no window in it:
//
//   WHICH PATHS THE LOADER HOOK OWNS. A release installs a callback on
//   raylib's own LoadFileData, and anything it claims is answered out of the
//   pack instead of off the disk. Claiming one path too many silently replaces
//   a file the game wrote, or shipped beside the pack, with the one packed.
//
//   WHAT THE PACK READS. src/rmp/pack.cpp reads resources.rres itself, every
//   field checked, because rres's own readers believed every field of it; a
//   name is looked up whole, the way the loose files are found.
//
//   WHICH NAME IS WHICH RESOURCE. The cache is keyed by name, and a key that
//   compares a prefix hands back the wrong texture.
//
//   WHAT A LOAD COUNTS. A name with nothing usable behind it is a failed load
//   -- missing, a folder, or a file that does not decode -- and only the last
//   is reported.
//
// Every pack here is written into a temporary folder by tests/pack_writer.h,
// the tests' own copy of what tools/rres_pack.c writes, except two that are
// committed: tests/fixtures/pack_zero/resources.rres, written by
// tools/make_zero_pack.py (tests/configure_test.py runs the script and
// compares), and tests/fixtures/pack_aes/resources.rres, written by
// tools/rres_pack.c itself:
//
//   cd tests/fixtures/pack_aes
//   ../../../build/debug/rres_pack resources.rres fixture-password hello.txt
//
// -- its salt is random, so it cannot be written twice the same, and it is
// the one test that pays for Argon2i (two seconds under ASan at -O0).
//
// The resources root and the pack are process-wide and shared with every other
// test in this binary: each test that moves the root puts it back, and each
// that opens a pack closes it.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/internal.h"
#include "../src/rmp/pack_internal.h"
#include "pack_writer.h"

#include <rmp/assets.h>
#include <rmp/config.h>

#include <raylib.h>
#include <rres.h> // rresSetCipherPassword

#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {

namespace pack = rmp::test::pack;
namespace rres = rmp::assets::detail::rres;

// The resources root is a global. Every test that moves it puts it back.
struct AtRoot {
    std::string previous;
    explicit AtRoot(const std::string &root)
        : previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(root.c_str());
    }
    ~AtRoot() { rmp::assets::detail::set_resources_root(previous.c_str()); }
    AtRoot(const AtRoot &) = delete;
    AtRoot &operator=(const AtRoot &) = delete;
    AtRoot(AtRoot &&) = delete;
    AtRoot &operator=(AtRoot &&) = delete;
};

// A folder of the test's own, gone with it.
class Scratch {
public:
    explicit Scratch(const std::string &name) {
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) / ("rmp-assets-test-" + name);
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
    }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;
    Scratch(Scratch &&) = delete;
    Scratch &operator=(Scratch &&) = delete;
    [[nodiscard]] const std::filesystem::path &path() const { return _path; }
    [[nodiscard]] std::string root() const { return (_path / "").string(); }
    void put(const std::string &name, std::string_view bytes) const {
        std::error_code error;
        std::filesystem::create_directories((_path / name).parent_path(), error);
        REQUIRE(pack::save(_path / name, pack::bytes_of(bytes)));
    }
    void put_pack(const pack::Bytes &bytes) const {
        REQUIRE(pack::save(_path / "resources.rres", bytes));
    }

private:
    std::filesystem::path _path;
};

// The pack under the root, open for as long as this lives.
struct OpenPack {
    bool opened = rmp::assets::detail::open_pack();
    OpenPack() = default;
    ~OpenPack() { rmp::assets::detail::close_pack(); }
    OpenPack(const OpenPack &) = delete;
    OpenPack &operator=(const OpenPack &) = delete;
    OpenPack(OpenPack &&) = delete;
    OpenPack &operator=(OpenPack &&) = delete;
};

// raylib's buffers, given back the way raylib says.
using FileBytes = std::unique_ptr<unsigned char, decltype(&UnloadFileData)>;
using FileText = std::unique_ptr<char, decltype(&UnloadFileText)>;

// What pack_read() hands back, as a string; nullopt for nothing.
std::optional<std::string> pack_bytes(const std::string &name) {
    int size = -1;
    const FileBytes data(rmp::assets::detail::pack_read(name.c_str(), &size),
                         &UnloadFileData);
    if (!data) {
        CHECK(size == 0);
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char *>(data.get()),
                       static_cast<std::size_t>(size));
}

std::string text_of(const std::vector<unsigned char> &bytes) {
    return { bytes.begin(), bytes.end() };
}

// What raylib's own LoadFileData() reads at `path` -- through the hook, when
// one is installed.
std::optional<std::string> raylib_read(const std::string &path) {
    int size = 0;
    const FileBytes data(LoadFileData(path.c_str(), &size), &UnloadFileData);
    if (!data) return std::nullopt;
    return std::string(reinterpret_cast<const char *>(data.get()),
                       static_cast<std::size_t>(size));
}

// And what its LoadFileText() reads there.
std::optional<std::string> raylib_text(const std::string &path) {
    const FileText text(LoadFileText(path.c_str()), &UnloadFileText);
    if (!text) return std::nullopt;
    return std::string(text.get());
}

// What a load counts, from here on.
struct Counts {
    int requested = rmp::assets::requested_loads();
    int failed = rmp::assets::failed_loads();
    [[nodiscard]] int more_requested() const {
        return rmp::assets::requested_loads() - requested;
    }
    [[nodiscard]] int more_failed() const { return rmp::assets::failed_loads() - failed; }
};

rres::Key cheap(std::string_view password, const rres::Salt &salt) {
    return pack::cheap(password, salt);
}

// The file `name` opens to from a pack in memory, or nullopt.
std::optional<std::string> memory_read(const pack::Bytes &bytes, std::string_view name,
                                       std::string_view password = RMP_RRES_PASSWORD) {
    rres::MemorySource memory(bytes);
    const std::optional<rres::Index> index = rres::read_index(memory);
    if (!index) return std::nullopt;
    const std::optional<rres::Chunk> chunk = rres::find(*index, name);
    if (!chunk) return std::nullopt;
    const auto contents = rres::open_chunk(memory, *chunk, password, cheap);
    if (!contents) return std::nullopt;
    const auto file = rres::file_of(*contents);
    if (!file) return std::nullopt;
    return std::string(file->begin(), file->end());
}

bool readable(const pack::Bytes &bytes) {
    rres::MemorySource memory(bytes);
    return rres::read_index(memory).has_value();
}

// The picture a single chunk of a pack in memory opens to.
::Image memory_image(const pack::Bytes &bytes, std::string_view name) {
    rres::MemorySource memory(bytes);
    const std::optional<rres::Index> index = rres::read_index(memory);
    CHECK(index.has_value());
    const std::optional<rres::Chunk> chunk =
        index ? rres::find(*index, name) : std::nullopt;
    CHECK(chunk.has_value());
    if (!chunk) return ::Image{};
    const auto contents = rres::open_chunk(memory, *chunk, RMP_RRES_PASSWORD, cheap);
    return contents ? rres::image_of(*contents) : ::Image{};
}

// Three files the way a pack holds them: text, bytes with a NUL inside, and
// nothing at all.
std::vector<pack::File> three() {
    return { { "a.txt", pack::bytes_of("the first file\n") },
             { "bytes.bin", pack::bytes_of(std::string_view("\0\x01\xFFz", 4)) },
             { "empty.dat", {} } };
}

} // namespace

// ---------------------------------------------------------------------------
// in_resources_dir: the rule that decides what the hook is responsible for
// ---------------------------------------------------------------------------

TEST_SUITE("assets: loader hook") {
    TEST_CASE("the loader hook only claims paths inside the resources directory") {
        const AtRoot at("resources/");
        using rmp::assets::detail::in_resources_dir;

        CHECK(in_resources_dir("resources/player.png"));
        CHECK(in_resources_dir("./resources/player.png")); // what an .obj builds
#if defined(_WIN32)
        CHECK(in_resources_dir("resources\\player.png")); // a Windows separator
#else
        // Elsewhere a backslash is a letter of a file's name, to the system and to
        // the hook: a file of that name, which a development build would not find
        // in resources/ either.
        CHECK_FALSE(in_resources_dir("resources\\player.png"));
#endif
        CHECK_FALSE(in_resources_dir("saves/player.png"));
        CHECK_FALSE(in_resources_dir("/home/someone/player.png"));
        CHECK_FALSE(in_resources_dir(""));
        CHECK_FALSE(in_resources_dir(nullptr));
    }

    TEST_CASE(
        "the hook claims a file directly in the folder, however the path spells it") {
        // An .mtl names its textures, and raylib joins them to the .obj's
        // folder: "resources/./skin.png" and "resources/sub/../skin.png" are
        // both resources/skin.png, which is packed.
        const AtRoot at("resources/");
        using rmp::assets::detail::in_resources_dir;

        CHECK(in_resources_dir("resources/./player.png"));
        CHECK(in_resources_dir("resources/sub/../player.png"));
        CHECK(in_resources_dir("resources//player.png"));
        CHECK(in_resources_dir("./resources/./player.png"));
    }

    TEST_CASE(
        "and nothing below the folder, nothing outside it, and not the folder itself") {
        // It used to claim every path that STARTED with the folder: the pack
        // is flat, so "resources/sub/player.png" -- shipped beside the pack --
        // was answered with the packed player.png, a different picture, and
        // "resources/../saves/level1.json" with the packed level1.json.
        const AtRoot at("resources/");
        using rmp::assets::detail::in_resources_dir;

        CHECK_FALSE(in_resources_dir("resources/sub/player.png"));
        CHECK_FALSE(in_resources_dir("resources/../saves/level1.json"));
        CHECK_FALSE(in_resources_dir("resources/"));
        CHECK_FALSE(in_resources_dir("resources"));
        CHECK_FALSE(in_resources_dir("resources/.."));
        CHECK_FALSE(in_resources_dir("resourcesX/player.png"));
        CHECK_FALSE(in_resources_dir("resources/player.png/")); // a folder by that name
        CHECK_FALSE(in_resources_dir("resources/player.png/."));
    }

    TEST_CASE("an absolute resources root claims only absolute paths into it") {
        const AtRoot at("/game/resources/");
        using rmp::assets::detail::in_resources_dir;

        CHECK(in_resources_dir("/game/resources/player.png"));
        CHECK(in_resources_dir("/game/./resources/player.png"));
        CHECK(in_resources_dir("/../game/resources/player.png")); // above / is /
        CHECK_FALSE(in_resources_dir("game/resources/player.png"));
        CHECK_FALSE(in_resources_dir("/game/player.png"));
    }

    TEST_CASE("an EMPTY resources root claims bare file names and nothing else") {
        // Android sets RMP_RESOURCES_PATH to "": every asset sits at the root of the
        // APK's assets/, which is also where the pack goes. An empty prefix used
        // to match every path in the filesystem -- the loop comparing it ran zero
        // times and the function returned true -- so LoadFileData on an absolute
        // path into internal storage was offered to the pack first, and a save
        // file named like a packed resource was answered with the shipped copy on
        // every load.
        const AtRoot at("");
        using rmp::assets::detail::in_resources_dir;

        CHECK(in_resources_dir("player.png"));
        CHECK(in_resources_dir("save.json"));
        // A directory separator of either kind means it is somebody else's file.
        CHECK_FALSE(in_resources_dir("/data/user/0/com.example.game/files/save.json"));
        CHECK_FALSE(in_resources_dir("files/save.json"));
#if defined(_WIN32)
        CHECK_FALSE(in_resources_dir("sub\\save.json"));
#else
        CHECK(in_resources_dir("sub\\save.json")); // one name, with a backslash in it
#endif
        // "./x" is still "x": raylib builds that form itself.
        CHECK(in_resources_dir("./save.json"));
        CHECK_FALSE(in_resources_dir(""));
        CHECK_FALSE(in_resources_dir("../save.json"));
    }

    TEST_CASE("through the hook, a file in a subfolder is the one beside the pack") {
        // The end to end of the rule above: raylib's own LoadFileData and
        // LoadFileText, in a release, for the packed file and for the loose
        // one of the same name below it (tests/property/assets_prop.cpp
        // found it, comparing a release with a development build).
        const Scratch release("hook");
        release.put_pack(pack::write({ { "same.txt", pack::bytes_of("packed") } }));
        release.put("sub/same.txt", "shipped beside the pack");
        const AtRoot at(release.root());
        rmp::assets::init();
        REQUIRE(rmp::assets::using_pack());

        CHECK(raylib_read(release.root() + "same.txt") == "packed");
        CHECK(raylib_read(release.root() + "./same.txt") == "packed");
        CHECK(raylib_read(release.root() + "sub/same.txt") == "shipped beside the pack");
        CHECK(raylib_text(release.root() + "sub/same.txt") == "shipped beside the pack");
        CHECK(raylib_text(release.root() + "same.txt") == "packed");
        rmp::assets::shutdown();
        CHECK_FALSE(rmp::assets::using_pack());
    }

    TEST_CASE(
        "through the hook, an empty packed file reads as raylib reads an empty file") {
        // raylib's LoadFileData and LoadFileText give an empty file back as no
        // data at all; the pack used to give back a buffer of size 0.
        const Scratch release("hook-empty");
        release.put_pack(pack::write({ { "empty.txt", {} } }));
        const AtRoot at(release.root());
        rmp::assets::init();
        REQUIRE(rmp::assets::using_pack());
        CHECK_FALSE(raylib_read(release.root() + "empty.txt").has_value());
        CHECK_FALSE(raylib_text(release.root() + "empty.txt").has_value());
        rmp::assets::shutdown();
    }
}

// ---------------------------------------------------------------------------
// The pack
// ---------------------------------------------------------------------------

TEST_SUITE("assets: pack") {
    TEST_CASE("a pack with no entries is not opened, and the game falls back") {
        // A truncated, corrupt or empty resources.rres. It has to come back as
        // "no pack", not as an open pack with nothing in it.
        const AtRoot at(RMP_TEST_FIXTURES "pack_zero/");
        CHECK_FALSE(rmp::assets::detail::open_pack());
        CHECK_FALSE(rmp::assets::detail::pack_is_open());

        int size = -1;
        CHECK(rmp::assets::detail::pack_read("anything.png", &size) == nullptr);
        CHECK(size == 0);
        rmp::assets::detail::close_pack(); // safe with nothing open
        CHECK_FALSE(rmp::assets::detail::pack_is_open());
    }

    TEST_CASE("a pack written as tools/rres_pack.c writes one reads back every file") {
        const Scratch release("three");
        release.put_pack(pack::write(three()));
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        CHECK(rmp::assets::using_pack());
        for (const pack::File &file : three()) {
            CAPTURE(file.name);
            CHECK(rmp::assets::detail::pack_has(file.name.c_str()));
            const std::string bytes(file.bytes.begin(), file.bytes.end());
            CHECK(pack_bytes(file.name) == bytes);
            const int requested = rmp::assets::requested_loads();
            const int failed = rmp::assets::failed_loads();
            CHECK(text_of(rmp::assets::load_data(file.name)) == bytes);
            CHECK(rmp::assets::requested_loads() == requested + 1);
            // An empty file found is not a file missing.
            CHECK(rmp::assets::failed_loads() == failed);
        }
    }

    TEST_CASE("a name is looked up whole: no prefix, no extension, no empty name") {
        // rres's lookup compared the name asked for against the start of each
        // entry: "music" found music.ogg, "level1" found level1.json, and ""
        // found whatever the pack held first -- in a release, while the same
        // call in development found nothing (tests/fuzz/regressions/rres/
        // empty_name_is_an_entry).
        const pack::Bytes bytes =
            pack::write({ { "music.ogg", pack::bytes_of("ogg") },
                          { "level10.json", pack::bytes_of("ten") } });
        CHECK(memory_read(bytes, "music.ogg") == "ogg");
        CHECK(memory_read(bytes, "level10.json") == "ten");
        CHECK_FALSE(memory_read(bytes, "music").has_value());
        CHECK_FALSE(memory_read(bytes, "level1").has_value());
        CHECK_FALSE(memory_read(bytes, "level1.json").has_value());
        CHECK_FALSE(memory_read(bytes, "music.ogg.bak").has_value());
        CHECK_FALSE(memory_read(bytes, "").has_value());

        const Scratch release("whole");
        release.put_pack(bytes);
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        CHECK_FALSE(rmp::assets::detail::pack_has("music"));
        CHECK_FALSE(rmp::assets::detail::pack_has(""));
        CHECK_FALSE(pack_bytes("").has_value());
        CHECK(rmp::assets::detail::pack_has("music.ogg"));
    }

    TEST_CASE("a loader's name finds the packed file the way the loose files are found") {
        // load_data("./level.json") finds resources/level.json in
        // development, so it finds the packed level.json in a release; and
        // "sub/level.json" is a file below the folder, never the packed one.
        const Scratch release("spelled");
        release.put_pack(pack::write({ { "level.json", pack::bytes_of("top") } }));
        release.put("sub/level.json", "below");
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        CHECK(text_of(rmp::assets::load_data("./level.json")) == "top");
        CHECK(text_of(rmp::assets::load_data("sub/../level.json")) == "top");
        CHECK(text_of(rmp::assets::load_data("sub/level.json")) == "below");
        CHECK(rmp::assets::detail::pack_has("./level.json"));
        CHECK_FALSE(rmp::assets::detail::pack_has("sub/level.json"));
#if defined(_WIN32)
        CHECK(rmp::assets::detail::pack_has("sub\\..\\level.json"));
#else
        // A file named \level.json to the system, which development does not
        // have: the pack found level.json, until tests/property/assets_prop.cpp
        // compared the two with a name of a backslash and a letter.
        CHECK_FALSE(rmp::assets::detail::pack_has("\\level.json"));
        CHECK(rmp::assets::load_data("\\level.json").empty());
#endif
        // Out of the folder and back in is the folder, in both.
        CHECK(text_of(rmp::assets::load_data("../rmp-assets-test-spelled/level.json")) ==
              "top");
        CHECK(rres::packed_name("res/", "res/a/../b.png") == "b.png");
        CHECK(rres::packed_name("", "a/../b.png") == "b.png");
        CHECK_FALSE(rres::packed_name("", "../b.png").has_value());
        CHECK_FALSE(rres::packed_name("", "/b.png").has_value());
        CHECK_FALSE(rres::packed_name("res/", "res/b.png/").has_value());
        CHECK_FALSE(rres::packed_name("res/", "res/b.png/.").has_value());
        CHECK_FALSE(rres::packed_name("", ".").has_value());
    }

    TEST_CASE("an encrypted chunk opens with its password, and is refused with another") {
        // Refused, and nothing kept: rres's own unpacker leaked the buffer it
        // decrypted into whenever the MD5 did not match -- a pack made with
        // another password, on every load (tests/fuzz/regressions/rres/
        // encrypted_chunk_that_does_not_decrypt; LeakSanitizer is the check).
        const pack::Bytes bytes =
            pack::write({ { "aes.txt", pack::bytes_of("secret"), pack::CIPHER_AES },
                          { "xchacha.txt", pack::bytes_of("also secret"),
                            pack::CIPHER_XCHACHA20_POLY1305 } });
        CHECK(memory_read(bytes, "aes.txt") == "secret");
        CHECK(memory_read(bytes, "xchacha.txt") == "also secret");
        CHECK_FALSE(memory_read(bytes, "aes.txt", "another").has_value());
        CHECK_FALSE(memory_read(bytes, "xchacha.txt", "another").has_value());
        CHECK_FALSE(memory_read(bytes, "aes.txt", "").has_value());
    }

    TEST_CASE("an encrypted chunk with fewer bytes than its trailer is refused") {
        // rres subtracted the salt and the MD5 from the size without looking:
        // ten bytes asked for four gigabytes (tests/fuzz/regressions/rres/
        // encrypted_chunk_shorter_than_its_trailer).
        for (const std::uint8_t cipher :
             { pack::CIPHER_AES, pack::CIPHER_XCHACHA20_POLY1305 }) {
            CAPTURE(static_cast<int>(cipher));
            const pack::Bytes bytes = pack::write_chunks(
                { { "short.txt", "RAWD", 0, cipher, pack::bytes_of("0123456789"), 10 } });
            CHECK(readable(bytes));
            CHECK_FALSE(memory_read(bytes, "short.txt").has_value());
        }
    }

    TEST_CASE("a chunk whose bytes are not what its CRC says is not opened") {
        // rres handed the unpacker a chunk with no data at all, and for an
        // encrypted one that was a read through null (tests/fuzz/regressions/
        // rres/encrypted_chunk_with_a_bad_crc).
        for (const std::uint8_t cipher : { pack::CIPHER_NONE, pack::CIPHER_AES }) {
            CAPTURE(static_cast<int>(cipher));
            pack::Bytes bytes =
                pack::write({ { "a.txt", pack::bytes_of("plain"), cipher } });
            const pack::Located chunk = pack::chunks(bytes).front();
            bytes[chunk.data + chunk.size - 1] ^= 0x40U;
            CHECK(readable(bytes));
            CHECK_FALSE(memory_read(bytes, "a.txt").has_value());
        }
    }

    TEST_CASE("a chunk whose properties say more than it holds is not opened") {
        // A property count past the chunk was a heap read past it, and a RAWD
        // chunk with no properties at all a read through null (tests/fuzz/
        // regressions/rres/property_count_past_the_chunk and
        // raw_chunk_without_properties).
        const auto chunk = [](const std::vector<std::uint32_t> &props,
                              std::string_view data) {
            pack::Bytes text = pack::plain(props, pack::bytes_of(data));
            const auto size = static_cast<std::uint32_t>(text.size());
            return pack::write_chunks(
                { { "a.txt", "RAWD", 0, 0, std::move(text), size } });
        };
        CHECK(memory_read(chunk({ 3, 0, 0, 0 }, "abc"), "a.txt") == "abc");
        CHECK_FALSE(memory_read(chunk({ 4, 0, 0, 0 }, "abc"), "a.txt").has_value());
        CHECK_FALSE(memory_read(chunk({}, "abc"), "a.txt").has_value());

        pack::Bytes count_past = chunk({ 3, 0, 0, 0 }, "abc");
        pack::set_u32(count_past, pack::chunks(count_past).front().data, 1000);
        pack::fix_crcs(count_past);
        CHECK_FALSE(memory_read(count_past, "a.txt").has_value());

        // And a base size that is not the size of what the chunk holds.
        pack::Bytes base = chunk({ 3, 0, 0, 0 }, "abc");
        pack::set_u32(base, pack::chunks(base).front().info + 16, 4096);
        CHECK_FALSE(memory_read(base, "a.txt").has_value());
    }

    TEST_CASE(
        "a pack cut short anywhere is refused whole, and the game reads loose files") {
        const pack::Bytes bytes = pack::write(three());
        REQUIRE(readable(bytes));
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            CAPTURE(cut);
            CHECK_FALSE(readable(pack::Bytes(
                bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut))));
        }
        const Scratch release("cut");
        release.put_pack(pack::Bytes(bytes.begin(), bytes.end() - 1));
        release.put("a.txt", "loose");
        const AtRoot at(release.root());
        const OpenPack open;
        CHECK_FALSE(open.opened);
        CHECK_FALSE(rmp::assets::using_pack());
        CHECK(text_of(rmp::assets::load_data("a.txt")) == "loose");
    }

    TEST_CASE("a directory that is not one is refused whole") {
        const auto with_directory = [](const pack::Bytes &entries, std::uint32_t count,
                                       std::uint32_t props) {
            pack::Bytes bytes = pack::write({ { "a.txt", pack::bytes_of("a") } });
            const pack::Located directory = pack::chunks(bytes).back();
            bytes.resize(directory.data);
            pack::Bytes data;
            pack::put_u32(data, props);
            if (props > 0) pack::put_u32(data, count);
            data.insert(data.end(), entries.begin(), entries.end());
            pack::set_u32(bytes, directory.info + 12,
                          static_cast<std::uint32_t>(data.size()));
            pack::set_u32(bytes, directory.info + 16,
                          static_cast<std::uint32_t>(data.size()));
            bytes.insert(bytes.end(), data.begin(), data.end());
            pack::fix_crcs(bytes);
            return bytes;
        };
        const auto entry = [](std::string_view name, std::uint32_t name_size) {
            pack::Bytes out;
            pack::put_u32(out, pack::crc32(name));
            pack::put_u32(out, 16);
            pack::put_u32(out, 0);
            pack::put_u32(out, name_size);
            out.insert(out.end(), name.begin(), name.end());
            out.resize(16 + name_size, 0);
            return out;
        };
        // The control: the directory the writer writes, written again.
        CHECK(memory_read(with_directory(entry("a.txt", 8), 1, 1), "a.txt") == "a");
        // No properties: rres read the count through null
        // (tests/fuzz/regressions/rres/directory_without_properties).
        CHECK_FALSE(readable(with_directory({}, 0, 0)));
        // A name longer than rres's 1024-byte entry: a heap overflow WRITE
        // (tests/fuzz/regressions/rres/directory_name_longer_than_an_entry).
        // Read whole here, it is a name like any other.
        const std::string long_name(1100, 'n');
        CHECK(readable(with_directory(entry(long_name, 1104), 1, 1)));
        // More entries than the directory holds, a name past its end, a name
        // with no end, and a name with nothing in it.
        CHECK_FALSE(readable(with_directory(entry("a.txt", 8), 2, 1)));
        pack::Bytes past_its_end = entry("a.txt", 8);
        pack::set_u32(past_its_end, 12, 64);
        CHECK_FALSE(readable(with_directory(past_its_end, 1, 1)));
        CHECK_FALSE(readable(with_directory(entry("abcd", 4), 1, 1)));
        CHECK_FALSE(readable(with_directory(entry("", 4), 1, 1)));
    }

    TEST_CASE("a chunk whose info is not where the directory says, or another id, is not "
              "read") {
        // The CRC covers a chunk's bytes and not its info: rres found a chunk
        // by walking for its id, and a chunk whose id was corrupt into
        // another's answered for that other name.
        pack::Bytes bytes = pack::write(
            { { "a.txt", pack::bytes_of("A") }, { "b.txt", pack::bytes_of("B") } });
        const std::vector<pack::Located> chunks = pack::chunks(bytes);
        pack::set_u32(bytes, chunks[0].info + 4, pack::crc32("b.txt"));
        CHECK_FALSE(memory_read(bytes, "a.txt").has_value());
        CHECK(memory_read(bytes, "b.txt") == "B");
    }

    TEST_CASE("a compressed or linked chunk is refused, and the loader falls back") {
        // raylib's inflater has undefined behaviour on a corrupt stream
        // (tests/fuzz/regressions/rres/deflate_*), and a LINK chunk names a
        // file outside the pack by a path inside it.
        const pack::Bytes text = pack::plain({ 1, 0, 0, 0 }, pack::bytes_of("x"));
        const auto size = static_cast<std::uint32_t>(text.size());
        for (const std::uint8_t compression : { 10, 20, 40 }) {
            CAPTURE(static_cast<int>(compression));
            CHECK_FALSE(memory_read(pack::write_chunks({ { "a.txt", "RAWD", compression,
                                                           0, text, size } }),
                                    "a.txt")
                            .has_value());
        }
        CHECK_FALSE(
            memory_read(pack::write_chunks({ { "a.txt", "LINK", 0, 0, text, size } }),
                        "a.txt")
                .has_value());

        const Scratch release("deflate");
        release.put_pack(pack::write_chunks({ { "a.txt", "RAWD", 10, 0, text, size } }));
        release.put("a.txt", "loose");
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        CHECK(text_of(rmp::assets::load_data("a.txt")) == "loose");
    }

    TEST_CASE("an IMGE chunk is a picture with the pixels its size says, or nothing") {
        // rres's IMGE is width, height, format, mipmaps and the pixels.
        // R8G8B8A8 is rres's 7; one level only, because rres checked the data
        // against the first level's size alone.
        const auto picture = [](const std::vector<std::uint32_t> &props,
                                std::size_t bytes) {
            pack::Bytes text = pack::plain(props, pack::Bytes(bytes, 0x7F));
            const auto size = static_cast<std::uint32_t>(text.size());
            return pack::write_chunks(
                { { "p.png", "IMGE", 0, 0, std::move(text), size } });
        };
        const ::Image image = memory_image(picture({ 2, 3, 7, 1 }, 24), "p.png");
        REQUIRE(image.data != nullptr);
        CHECK(image.width == 2);
        CHECK(image.height == 3);
        CHECK(image.mipmaps == 1);
        CHECK(image.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        UnloadImage(image);

        CHECK(memory_image(picture({ 2, 3, 7, 2 }, 24), "p.png").data == nullptr);
        CHECK(memory_image(picture({ 2, 3, 7, 1 }, 23), "p.png").data == nullptr);
        CHECK(memory_image(picture({ 2, 3, 99, 1 }, 24), "p.png").data == nullptr);
        CHECK(memory_image(picture({ 0, 3, 7, 1 }, 0), "p.png").data == nullptr);
        CHECK(memory_image(picture({ 2, 3, 7 }, 24), "p.png").data == nullptr);
        CHECK(memory_image(picture({ 65536, 65536, 7, 1 }, 16), "p.png").data == nullptr);
    }

    TEST_CASE("every pixel format rres names is raylib's of that name") {
        // By name, not by number: raylib 6 put formats in the middle of its enum.
        const std::vector<std::pair<std::uint32_t, int>> formats = {
            { 1, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE },
            { 2, PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA },
            { 3, PIXELFORMAT_UNCOMPRESSED_R5G6B5 },
            { 4, PIXELFORMAT_UNCOMPRESSED_R8G8B8 },
            { 5, PIXELFORMAT_UNCOMPRESSED_R5G5B5A1 },
            { 6, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4 },
            { 7, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 },
            { 8, PIXELFORMAT_UNCOMPRESSED_R32 },
            { 9, PIXELFORMAT_UNCOMPRESSED_R32G32B32 },
            { 10, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32 },
            { 11, PIXELFORMAT_COMPRESSED_DXT1_RGB },
            { 12, PIXELFORMAT_COMPRESSED_DXT1_RGBA },
            { 13, PIXELFORMAT_COMPRESSED_DXT3_RGBA },
            { 14, PIXELFORMAT_COMPRESSED_DXT5_RGBA },
            { 15, PIXELFORMAT_COMPRESSED_ETC1_RGB },
            { 16, PIXELFORMAT_COMPRESSED_ETC2_RGB },
            { 17, PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA },
            { 18, PIXELFORMAT_COMPRESSED_PVRT_RGB },
            { 19, PIXELFORMAT_COMPRESSED_PVRT_RGBA },
            { 20, PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA },
            { 21, PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA },
        };
        for (const auto &[rres_format, raylib_format] : formats) {
            CAPTURE(rres_format);
            const auto bytes =
                static_cast<std::size_t>(GetPixelDataSize(8, 8, raylib_format));
            pack::Bytes text =
                pack::plain({ 8, 8, rres_format, 1 }, pack::Bytes(bytes, 1));
            const auto size = static_cast<std::uint32_t>(text.size());
            const ::Image image = memory_image(
                pack::write_chunks({ { "p", "IMGE", 0, 0, std::move(text), size } }),
                "p");
            CHECK(image.format == raylib_format);
            CHECK(image.data != nullptr);
            UnloadImage(image);
        }
    }

    TEST_CASE("a pack that is not one, or keeps its directory some way rres cannot, is "
              "refused") {
        const pack::Bytes good = pack::write({ { "a.txt", pack::bytes_of("a") } });
        REQUIRE(readable(good));
        pack::Bytes magic = good;
        magic[0] = 'R';
        CHECK_FALSE(readable(magic));
        pack::Bytes version = good;
        version[4] = 101;
        CHECK_FALSE(readable(version));
        pack::Bytes no_directory = good;
        pack::set_u32(no_directory, 8, 0);
        CHECK_FALSE(readable(no_directory));
        // A directory that is encrypted, or compressed.
        for (const std::size_t field : { std::size_t{ 8 }, std::size_t{ 9 } }) {
            pack::Bytes directory = good;
            directory[pack::chunks(directory).back().info + field] = 30;
            CHECK_FALSE(readable(directory));
        }
        // A cipher this does not read: rres's XOR.
        pack::Bytes xored = good;
        xored[pack::chunks(xored).front().info + 9] = 1;
        CHECK(readable(xored));
        CHECK_FALSE(memory_read(xored, "a.txt").has_value());
        // And a chunk past 2 GiB, which a CRC and an MD5 cannot be taken of.
        rres::MemorySource memory(good);
        rres::Chunk huge;
        huge.info.packed_size = 0x80000000U;
        CHECK_FALSE(rres::open_chunk(memory, huge, RMP_RRES_PASSWORD, cheap).has_value());
    }

    TEST_CASE("a RAWD picture is decoded by the extension packed with it") {
        const ::Image red = GenImageColor(2, 2, RED);
        int size = 0;
        const std::unique_ptr<unsigned char, decltype(&MemFree)> png(
            ExportImageToMemory(red, ".png", &size), &MemFree);
        UnloadImage(red);
        REQUIRE(png != nullptr);
        const pack::Bytes bytes(png.get(), png.get() + size);

        const ::Image image =
            memory_image(pack::write({ { "dot.png", bytes } }), "dot.png");
        CHECK(image.width == 2);
        CHECK(image.height == 2);
        UnloadImage(image);
        // The same bytes under another extension are not a PNG to raylib.
        CHECK(memory_image(pack::write({ { "dot.txt", bytes } }), "dot.txt").data ==
              nullptr);
    }
}

TEST_SUITE("assets: pack, loaders") {
    TEST_CASE("a picture in the pack is loaded from it, and one that is not falls back") {
        const ::Image red = GenImageColor(2, 2, RED);
        int size = 0;
        const std::unique_ptr<unsigned char, decltype(&MemFree)> png(
            ExportImageToMemory(red, ".png", &size), &MemFree);
        UnloadImage(red);
        REQUIRE(png != nullptr);
        const pack::Bytes dot(png.get(), png.get() + size);

        const Scratch release("pictures");
        release.put_pack(
            pack::write({ { "dot.png", dot }, { "bad.png", pack::bytes_of("no") } }));
        release.put(
            "loose.png",
            std::string_view(reinterpret_cast<const char *>(dot.data()), dot.size()));
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        const Counts before;
        const rmp::Image packed = rmp::assets::load_image("dot.png");
        REQUIRE(packed.valid());
        CHECK(packed.raw().width == 2);
        CHECK(before.more_failed() == 0);
        // Not in the pack: the loose file, as development reads it.
        CHECK(rmp::assets::load_image("loose.png").valid());
        CHECK(before.more_failed() == 0);
        // In the pack and not a picture, and no loose file behind it.
        CHECK_FALSE(rmp::assets::load_image("bad.png").valid());
        CHECK(before.more_requested() == 3);
        CHECK(before.more_failed() == 1);
        CHECK(::Image(rmp::assets::detail::pack_read_image(nullptr)).data == nullptr);
    }

    TEST_CASE("a font in the pack is read from it, and one that cannot be drawn leaks "
              "nothing") {
        // With no window there is no GPU, and the font comes back empty; it
        // decoded, so it is no failed load -- and its glyph tables used to be
        // kept, every load (LeakSanitizer is the check).
        const std::string fonts =
            RMP_TEST_FIXTURES "../../examples/assets/01_loading/resources/";
        std::ifstream file(fonts + "ui.ttf", std::ios::binary);
        const pack::Bytes ttf{ std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>() };
        REQUIRE_FALSE(ttf.empty());
        {
            const AtRoot at(fonts);
            const Counts before;
            static_cast<void>(rmp::assets::load_font("ui.ttf", 20));
            CHECK(before.more_failed() == 0);
        }
        const Scratch release("fonts");
        release.put_pack(pack::write({ { "ui.ttf", ttf } }));
        const AtRoot at(release.root());
        const OpenPack open;
        REQUIRE(open.opened);
        const Counts before;
        static_cast<void>(rmp::assets::load_font("ui.ttf", 20));
        CHECK(before.more_failed() == 0);
        CHECK(before.more_requested() == 1);
    }
}

TEST_SUITE("assets: pack, Argon2i") {
    TEST_CASE(
        "a pack made by tools/rres_pack.c is read with the password it was made with") {
        // The one test of the real key: Argon2i, 16 MiB, three passes -- what
        // the packer encrypts with and what a release reads.
        const std::string folder = RMP_TEST_FIXTURES "pack_aes/";
        std::ifstream file(folder + "hello.txt", std::ios::binary);
        const std::string loose{ std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>() };
        REQUIRE_FALSE(loose.empty());
        const AtRoot at(folder);
        const OpenPack open;
        REQUIRE(open.opened);
        rresSetCipherPassword("fixture-password"); // open_pack() set the game's
        CHECK(text_of(rmp::assets::load_data("hello.txt")) == loose);
        rresSetCipherPassword(RMP_RRES_PASSWORD);
    }
}

// ---------------------------------------------------------------------------
// The name cache
// ---------------------------------------------------------------------------

TEST_SUITE("resources: table") {
    TEST_CASE("two long names that differ only at the end are two resources") {
        // Assets in deep folders: characters/enemies/tier3/..._diffuse.png and
        // ..._normal.png differ past character 95. Comparing a prefix made the
        // second load_texture hand back the first texture, so the normal map drew
        // as the diffuse and nothing was logged.
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();

        const std::string shared(115, 'a');
        const std::string first = shared + "_one";
        const std::string second = shared + "_two";
        REQUIRE(first.size() == 119);
        REQUIRE(first != second);

        const ::Image zeroed{};
        auto *a = rmp::detail::adopt_named(ResourceKind::IMAGE, first, 0, zeroed);
        REQUIRE(a != nullptr);
        const rmp::Image held_a{ a };

        // The whole point: the second name must NOT find the first slot.
        CHECK(rmp::detail::acquire_named(ResourceKind::IMAGE, second, 0) == nullptr);

        auto *b = rmp::detail::adopt_named(ResourceKind::IMAGE, second, 0, zeroed);
        REQUIRE(b != nullptr);
        const rmp::Image held_b{ b };

        CHECK(rmp::detail::live_count() == 2);
        CHECK(rmp::detail::ref_count(first) == 1);
        CHECK(rmp::detail::ref_count(second) == 1);

        SUBCASE("and the full name still finds its own slot") {
            auto *again = rmp::detail::acquire_named(ResourceKind::IMAGE, first, 0);
            CHECK(again == a);
            rmp::detail::release(again);
        }
        rmp::detail::release_all();
    }

    TEST_CASE(
        "the resource table has no cap: three hundred names are three hundred slots") {
        // It used to be a fixed array of 256, and the 257th load was silently not
        // cached -- a warning in the log and a second GPU copy every time.
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();
        const ::Image zeroed{};
        std::vector<rmp::Image> held;
        for (int i = 0; i < 300; ++i) {
            const std::string name = "many_" + std::to_string(i) + ".png";
            held.emplace_back(
                rmp::detail::adopt_named(ResourceKind::IMAGE, name, 0, zeroed));
            REQUIRE(held.back().valid());
        }
        CHECK(rmp::detail::live_count() == 300);
        CHECK(rmp::detail::ref_count("many_299.png") == 1);
        held.clear();
        CHECK(rmp::detail::live_count() == 0);
        rmp::detail::release_all();
    }

    TEST_CASE("a handle that outlives release_all() is harmless, and reads as empty") {
        // rmp::assets::shutdown() runs before the window closes; a global holding
        // an rmp::Texture is destroyed after. Its slot must still exist -- emptied,
        // never freed -- so that destructor is a no-op and not a use after free.
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();
        ::Image marked{};
        marked.width = 42;
        rmp::Image survivor{ rmp::detail::adopt_named(ResourceKind::IMAGE, "late.png", 0,
                                                      marked) };
        REQUIRE(survivor.valid());
        CHECK(survivor.raw().width == 42);

        rmp::detail::release_all();
        CHECK(rmp::detail::live_count() == 0);
        CHECK(rmp::detail::ref_count("late.png") == 0);
        // Still "valid" in the sense of pointing at a slot, but the slot is empty:
        // drawing it draws nothing, the same as a missing asset.
        CHECK(survivor.raw().width == 0);
        survivor = rmp::Image{}; // the release that used to be the crash
        CHECK(rmp::detail::live_count() == 0);

        // And a resource loaded afterwards counts from one.
        const rmp::Image again{ rmp::detail::adopt_named(ResourceKind::IMAGE, "after.png",
                                                         0, marked) };
        REQUIRE(again.valid());
        CHECK(rmp::detail::ref_count("after.png") == 1);
        rmp::detail::release_all();
    }

    TEST_CASE("every kind the table holds is let go with its last handle, empty or not") {
        // The kinds no loader of rmp::assets hands out yet -- music, shaders,
        // render textures -- and a sheet with no texture: a zeroed payload of
        // each is what a test without a window can make, and raylib's Unload*
        // of each takes one.
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();
        {
            const rmp::Texture texture{ rmp::detail::adopt(ResourceKind::TEXTURE,
                                                           Texture2D{}) };
            const rmp::Music music{ rmp::detail::adopt(ResourceKind::MUSIC, ::Music{}) };
            const rmp::Shader shader{ rmp::detail::adopt(ResourceKind::SHADER,
                                                         ::Shader{}) };
            const rmp::RenderTexture target{ rmp::detail::adopt(
                ResourceKind::RENDER_TEXTURE, RenderTexture2D{}) };
            const rmp::SpriteSheet sheet{ rmp::detail::adopt(ResourceKind::SHEET,
                                                             rmp::SheetData{}) };
            CHECK(rmp::detail::live_count() == 5);
            CHECK(sheet.raw().frame_count() == 0);
        }
        CHECK(rmp::detail::live_count() == 0);
    }

    TEST_CASE(
        "a handle that outlived release_all() never holds what is loaded after it") {
        // The next load used to reuse the slot release_all() emptied under a
        // handle: the old handle read the new resource, and letting it go
        // unloaded the new one under the handle that loaded it. Loaded again
        // into the same handle, the move-assignment released the slot it had
        // just been given, and the handle held an empty texture --
        // tests/property/assets_prop.cpp, shrunk to [load, release_all, load].
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();
        ::Image first{};
        first.width = 1;
        ::Image second{};
        second.width = 2;

        rmp::Image handle{ rmp::detail::adopt_named(ResourceKind::IMAGE, "x.png", 0,
                                                    first) };
        rmp::detail::release_all();
        handle = rmp::Image{ rmp::detail::adopt_named(ResourceKind::IMAGE, "x.png", 0,
                                                      second) };
        CHECK(handle.raw().width == 2);
        CHECK(rmp::detail::live_count() == 1);

        rmp::Image stale{ rmp::detail::adopt_named(ResourceKind::IMAGE, "y.png", 0,
                                                   first) };
        rmp::detail::release_all();
        const rmp::Image fresh{ rmp::detail::adopt_named(ResourceKind::IMAGE, "z.png", 0,
                                                         second) };
        CHECK(stale.raw().width == 0);
        CHECK(fresh.raw().width == 2);
        rmp::Image copy = stale; // a copy of nothing holds nothing more
        stale = rmp::Image{};
        copy = rmp::Image{};
        CHECK(fresh.raw().width == 2);
        CHECK(rmp::detail::ref_count("z.png") == 1);
        CHECK(rmp::detail::live_count() == 1);
        rmp::detail::release_all();
    }
}

// ---------------------------------------------------------------------------
// The table after the migration: the conversion that no longer compiles from
// a temporary.
// ---------------------------------------------------------------------------

// Compile-time, which is the only place this can be tested: an LVALUE handle
// converts to the raylib type, a TEMPORARY does not. `Texture2D t =
// rmp::assets::load_texture("x")` used to compile and hand back a texture the
// dying temporary had already released.
static_assert(
    std::is_convertible_v<rmp::Texture &, const Texture2D &>,
    "a named rmp::Texture must still convert where a raylib function wants one");
static_assert(
    !std::is_convertible_v<rmp::Texture, const Texture2D &>,
    "a temporary rmp::Texture must NOT convert: it would release on the same line");
static_assert(!std::is_convertible_v<rmp::Font, const ::Font &>);
static_assert(!std::is_convertible_v<rmp::Sound, const ::Sound &>);
// A handle's moves cannot fail, and say so: a std::vector of them moves its
// handles when it grows, instead of copying each one and counting it twice.
static_assert(std::is_nothrow_move_constructible_v<rmp::Texture>);
static_assert(std::is_nothrow_move_assignable_v<rmp::Texture>);

// ---------------------------------------------------------------------------
// What a load counts
// ---------------------------------------------------------------------------
//
// tests/fixtures/corrupt/ holds one of each: every file starts the way its
// format does, so the decoder is chosen and runs, and is then cut short --
// what a truncated download or a bad export looks like. The CI boot gate reads
// assets_failed, and it counted only names with NOTHING behind them: a corrupt
// PNG left a blank texture and a green boot. The sound and the music need a
// device, so they are in tests/audio_device_test.cpp.

TEST_SUITE("assets: loads") {
    TEST_CASE("an image that is there and does not decode is a failed load") {
        const AtRoot at(RMP_TEST_FIXTURES "corrupt/");
        const Counts before;
        CHECK_FALSE(rmp::assets::load_image("broken.png").valid());
        CHECK(before.more_requested() == 1);
        CHECK(before.more_failed() == 1);
    }

    TEST_CASE("a texture whose image does not decode is a failed load, once") {
        const AtRoot at(RMP_TEST_FIXTURES "corrupt/");
        const Counts before;
        CHECK_FALSE(rmp::assets::load_texture("broken.png").valid());
        CHECK(before.more_requested() == 1);
        CHECK(before.more_failed() == 1);
    }

    TEST_CASE("a font that does not decode is a failed load, and not the default font") {
        // raylib answers a font it cannot read with GetFontDefault(). Adopted
        // under the file's name, that would be cached as if it were the font --
        // and a failed load is deliberately never cached.
        const AtRoot at(RMP_TEST_FIXTURES "corrupt/");
        const Counts before;
        CHECK_FALSE(rmp::assets::load_font("broken.ttf", 20).valid());
        CHECK(before.more_requested() == 1);
        CHECK(before.more_failed() == 1);
    }

    TEST_CASE("a font size that is not one is refused before the font is read") {
        // stb_truetype asserts on a negative size: load_font("ui.ttf", -1)
        // aborted the program (a debug build; out of bounds in a release).
        // A real face, the one examples/assets/01_loading ships.
        const AtRoot at(RMP_TEST_FIXTURES "../../examples/assets/01_loading/resources/");
        rmp::detail::reset_reports_for_tests();
        for (const int size : { 0, -1, -1000, 1025, 100000, INT_MAX, INT_MIN }) {
            CAPTURE(size);
            const Counts before;
            CHECK_FALSE(rmp::assets::load_font("ui.ttf", size).valid());
            CHECK(before.more_requested() == 1);
            CHECK(before.more_failed() == 1);
        }
        CHECK(rmp::detail::report_count() == 1); // one name, said once
        CHECK_FALSE(rmp::assets::load_font("other.ttf", -1).valid());
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE("an image that decodes is not a failed load") {
        // The control: the counter moves for the corrupt file, not for every file.
        const Counts before;
        const rmp::Image image = rmp::assets::load_image("rabbit.png");
        CHECK(image.valid());
        CHECK(before.more_requested() == 1);
        CHECK(before.more_failed() == 0);
    }

    TEST_CASE("a folder is not a file: no load finds it, and none reports it") {
        // load_data("") asked for resources/ itself, which exists: every
        // loader called it there, raylib was handed a folder to read, and the
        // pictures and fonts reported "could not be decoded" for a name that
        // names nothing (tests/property/assets_prop.cpp).
        const Scratch empty("folder");
        empty.put("sub/x.txt", "x");
        const AtRoot at(empty.root());
        rmp::detail::reset_reports_for_tests();
        for (const std::string name : { "", ".", "sub", "sub/" }) {
            CAPTURE(name);
            const Counts before;
            CHECK(rmp::assets::load_data(name).empty());
            CHECK_FALSE(rmp::assets::load_image(name).valid());
            CHECK_FALSE(rmp::assets::load_font(name, 16).valid());
            CHECK(before.more_requested() == 3);
            CHECK(before.more_failed() == 3);
            CHECK_FALSE(rmp::assets::detail::resource_exists(name.c_str()));
        }
        CHECK(rmp::detail::report_count() == 0);
    }

    TEST_CASE(
        "load_data hands back a file's bytes, and nothing for a name that is nowhere") {
        const Scratch loose("load-data");
        loose.put("level.json", std::string_view("{\"a\":\0 1}", 9));
        loose.put("empty.json", "");
        const AtRoot at(loose.root());
        const Counts before;
        CHECK(text_of(rmp::assets::load_data("level.json")) ==
              std::string_view("{\"a\":\0 1}", 9));
        CHECK(rmp::assets::load_data("empty.json").empty());
        CHECK(before.more_failed() == 0);
        CHECK(rmp::assets::load_data("missing.json").empty());
        CHECK(before.more_requested() == 3);
        CHECK(before.more_failed() == 1);
    }

    TEST_CASE("init() reads the pack when there is one, and shutdown() lets it go") {
        CHECK_FALSE(rmp::assets::using_pack());
        {
            const Scratch loose("no-pack");
            const AtRoot at(loose.root());
            rmp::assets::init();
            CHECK_FALSE(rmp::assets::using_pack());
            rmp::assets::shutdown();
        }
        const Scratch release("init");
        release.put_pack(pack::write({ { "a.txt", pack::bytes_of("packed") } }));
        const AtRoot at(release.root());
        rmp::assets::init();
        rmp::assets::init(); // twice is harmless
        CHECK(rmp::assets::using_pack());
        CHECK(text_of(rmp::assets::load_data("a.txt")) == "packed");
        rmp::assets::shutdown();
        CHECK_FALSE(rmp::assets::using_pack());
        CHECK(rmp::assets::load_data("a.txt").empty()); // the pack is gone with it
    }

    TEST_CASE(
        "load_sheet reads an .aseprite once, shares it, and counts what it cannot") {
        rmp::detail::release_all();
        const AtRoot at(RMP_TEST_FIXTURES);
        const Counts before;
        const rmp::SpriteSheet sheet = rmp::assets::load_sheet("anim.aseprite");
        REQUIRE(sheet.valid());
        CHECK(sheet.raw().frame_count() == 4);
        CHECK(sheet.raw().tag_count() == 3);
        CHECK(sheet.raw().texture.id ==
              0); // no window, no GPU, and the frames regardless
        const rmp::SpriteSheet again = rmp::assets::load_sheet("anim.aseprite");
        CHECK(rmp::detail::ref_count("anim.aseprite") == 2);
        CHECK(&again.raw() == &sheet.raw());
        CHECK(before.more_failed() == 0);

        CHECK_FALSE(rmp::assets::load_sheet("missing.aseprite").valid());
        CHECK(before.more_failed() == 1);
        CHECK_FALSE(
            rmp::assets::load_sheet("map_minimal.json").valid()); // there, not a sheet
        CHECK(before.more_failed() == 2);
        rmp::detail::release_all();
    }
}
