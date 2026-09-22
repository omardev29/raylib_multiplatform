// ===========================================================================
// The asset layer's two rules that have no window in them:
//
//   WHICH PATHS THE LOADER HOOK OWNS. A release installs a callback on
//   raylib's own LoadFileData, and anything it claims is answered out of the
//   pack instead of off the disk. Claiming one path too many silently replaces
//   a file the game wrote with the one it shipped.
//
//   WHICH NAME IS WHICH RESOURCE. The cache is keyed by name, and a key that
//   compares a prefix hands back the wrong texture.
//
// No pack is opened here except the deliberately empty one, and the resources
// root is put back after every test: it is process-wide and shared with every
// other test in this binary.
//
// THE EMPTY PACK IS GENERATED. tests/fixtures/pack_zero/resources.rres is
// written by tools/make_zero_pack.py, which is the only source of those bytes
// and explains every field of them; tests/configure_test.py runs the script
// into a temporary directory and compares it against the committed file, so
// the two cannot drift. Do not edit the fixture by hand -- change the script
// and run it.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/internal.h"

#include <rmp/assets.h>

#include <string>
#include <type_traits>
#include <vector>

namespace {

// The resources root is a global. Every test that moves it puts it back.
struct AtRoot {
    std::string previous;
    explicit AtRoot(const char *root) : previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(root);
    }
    ~AtRoot() { rmp::assets::detail::set_resources_root(previous.c_str()); }
    AtRoot(const AtRoot &) = delete;
    AtRoot &operator=(const AtRoot &) = delete;
};

} // namespace

// ---------------------------------------------------------------------------
// in_resources_dir: the rule that decides what the hook is responsible for
// ---------------------------------------------------------------------------

TEST_CASE("the loader hook only claims paths inside the resources directory") {
    const AtRoot at("resources/");
    using rmp::assets::detail::in_resources_dir;

    CHECK(in_resources_dir("resources/player.png"));
    CHECK(in_resources_dir("./resources/player.png")); // what an .obj builds
    CHECK(in_resources_dir("resources\\player.png")); // a Windows separator
    CHECK_FALSE(in_resources_dir("saves/player.png"));
    CHECK_FALSE(in_resources_dir("/home/someone/player.png"));
    CHECK_FALSE(in_resources_dir(""));
}

TEST_CASE("an EMPTY resources root claims bare file names and nothing else") {
    // Android sets RESOURCES_PATH to "": every asset sits at the root of the
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
    CHECK_FALSE(in_resources_dir("sub\\save.json"));
    // "./x" is still "x": raylib builds that form itself.
    CHECK(in_resources_dir("./save.json"));
}

// ---------------------------------------------------------------------------
// The pack
// ---------------------------------------------------------------------------

TEST_CASE("a pack with no entries is not opened, and the game falls back") {
    // A truncated, corrupt or empty resources.rres. It has to come back as
    // "no pack", not as an open pack with nothing in it -- and the central
    // directory rres allocated has to be given back, which is the half of
    // this only a leak checker can see.
    const AtRoot at(RMP_TEST_FIXTURES "pack_zero/");
    CHECK_FALSE(rmp::assets::detail::open_pack());
    CHECK_FALSE(rmp::assets::detail::pack_is_open());

    int size = -1;
    CHECK(rmp::assets::detail::pack_read("anything.png", &size) == nullptr);
    CHECK(size == 0);
    rmp::assets::detail::close_pack(); // safe with nothing open
    CHECK_FALSE(rmp::assets::detail::pack_is_open());
}

// ---------------------------------------------------------------------------
// The name cache
// ---------------------------------------------------------------------------

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

    ::Image zeroed{};
    auto *a = rmp::detail::adopt_named(ResourceKind::IMAGE, first.c_str(), 0, zeroed);
    REQUIRE(a != nullptr);
    rmp::Image held_a{ a };

    // The whole point: the second name must NOT find the first slot.
    CHECK(rmp::detail::acquire_named(ResourceKind::IMAGE, second.c_str(), 0) == nullptr);

    auto *b = rmp::detail::adopt_named(ResourceKind::IMAGE, second.c_str(), 0, zeroed);
    REQUIRE(b != nullptr);
    rmp::Image held_b{ b };

    CHECK(rmp::detail::live_count() == 2);
    CHECK(rmp::detail::ref_count(first.c_str()) == 1);
    CHECK(rmp::detail::ref_count(second.c_str()) == 1);

    SUBCASE("and the full name still finds its own slot") {
        auto *again = rmp::detail::acquire_named(ResourceKind::IMAGE, first.c_str(), 0);
        CHECK(again == a);
        rmp::detail::release(again);
    }
    rmp::detail::release_all();
}

// ---------------------------------------------------------------------------
// The table after the migration: no cap, handles that outlive shutdown, and
// the conversion that no longer compiles from a temporary.
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

TEST_CASE("the resource table has no cap: three hundred names are three hundred slots") {
    // It used to be a fixed array of 256, and the 257th load was silently not
    // cached -- a warning in the log and a second GPU copy every time.
    using rmp::detail::ResourceKind;
    rmp::detail::release_all();
    const ::Image zeroed{};
    std::vector<rmp::Image> held;
    for (int i = 0; i < 300; i++) {
        const std::string name = "many_" + std::to_string(i) + ".png";
        auto *slot =
            rmp::detail::adopt_named(ResourceKind::IMAGE, name.c_str(), 0, zeroed);
        REQUIRE(slot != nullptr);
        held.emplace_back(slot);
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
    auto *slot = rmp::detail::adopt_named(ResourceKind::IMAGE, "late.png", 0, marked);
    REQUIRE(slot != nullptr);
    rmp::Image survivor{ slot };
    CHECK(survivor.raw().width == 42);

    rmp::detail::release_all();
    CHECK(rmp::detail::live_count() == 0);
    CHECK(rmp::detail::ref_count("late.png") == 0);
    // Still "valid" in the sense of pointing at a slot, but the slot is empty:
    // drawing it draws nothing, the same as a missing asset.
    CHECK(survivor.raw().width == 0);
    survivor = rmp::Image{}; // the release that used to be the crash
    CHECK(rmp::detail::live_count() == 0);

    // And the slot is reused afterwards, refs starting from one.
    auto *again = rmp::detail::adopt_named(ResourceKind::IMAGE, "after.png", 0, marked);
    REQUIRE(again != nullptr);
    CHECK(rmp::detail::ref_count("after.png") == 1);
    rmp::detail::release_all();
}
