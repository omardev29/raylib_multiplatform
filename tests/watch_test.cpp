// ===========================================================================
// Hot reload: a Value read with rmp::assets::load_value() and a texture read
// with load_texture() remember their file in resources/, and changed() says
// when it differs from what they hold (src/rmp/watch.cpp).
//
//   WHAT CHANGED() SEES. An edit that changes the bytes; not an editor saving
//   the same bytes again, not a file that is gone for a moment.
//
//   WHAT RELOAD() DOES. Reads the file again in place, and keeps remembering
//   it; a file that is not JSON now (half saved) leaves the Value as it was
//   and changed() true, to try again.
//
//   WHAT HAS NO FILE. A Value made in code, one taken out of another, an
//   empty handle, an adopted texture, anything read from the pack.
//
// The files are polled at most every 0.25 s; poll_watched_now() lets a test
// skip the wait. No window: a texture's slot is checked with a zeroed payload,
// and reload()'s GPU half is tests/ui_render_test.cpp's.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/internal.h"
#include "pack_writer.h"

#include <rmp/assets.h>
#include <rmp/config.h>
#include <rmp/save.h>

#include <raylib.h>

#include <filesystem>
#include <memory>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if !RMP_PRODUCTION_BUILD

namespace {

namespace pack = rmp::test::pack;

// A resources root of the test's own, gone with it, and the watch list
// emptied on both sides so no test sees another's files.
class Root {
public:
    explicit Root(const std::string &name)
        : _previous(rmp::assets::detail::resources_root()) {
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) / ("rmp-watch-test-" + name);
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
        _root = (_path / "").string();
        rmp::assets::detail::set_resources_root(_root.c_str());
        rmp::assets::detail::forget_watched();
    }
    ~Root() {
        rmp::assets::detail::forget_watched();
        rmp::assets::detail::set_resources_root(_previous.c_str());
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
    Root(Root &&) = delete;
    Root &operator=(Root &&) = delete;

    void put(const std::string &name, std::string_view text) const {
        std::ofstream out(_path / name, std::ios::binary | std::ios::trunc);
        out << text;
        REQUIRE(out.good());
    }
    void remove(const std::string &name) const {
        std::error_code error;
        std::filesystem::remove(_path / name, error);
    }
    [[nodiscard]] const std::filesystem::path &path() const { return _path; }

private:
    std::string _previous;
    std::filesystem::path _path;
    std::string _root;
};

// What changed() answers once the files have been looked at again.
bool changed_now(const rmp::Value &value) {
    rmp::assets::detail::poll_watched_now();
    return value.changed();
}
bool changed_now(const rmp::Texture &texture) {
    rmp::assets::detail::poll_watched_now();
    return texture.changed();
}

} // namespace

TEST_SUITE("assets: hot reload, values") {
    TEST_CASE("an edit is seen, reload() reads it, and the Value keeps watching") {
        const Root root("edit");
        root.put("tuning.json", R"({"speed": 200})");
        rmp::Value tuning = rmp::assets::load_value("tuning.json");
        CHECK(tuning["speed"].as_int() == 200);
        CHECK_FALSE(changed_now(tuning));

        root.put("tuning.json", R"({"speed": 350, "jump": 9})");
        CHECK(changed_now(tuning));
        CHECK(tuning.changed()); // asked again, nothing read: still true
        CHECK(tuning["speed"].as_int() == 200); // changed() only says

        CHECK(tuning.reload());
        CHECK(tuning["speed"].as_int() == 350);
        CHECK(tuning["jump"].as_int() == 9);
        CHECK_FALSE(changed_now(tuning));

        root.put("tuning.json", R"({"speed": 1})");
        CHECK(changed_now(tuning));
        CHECK(tuning.reload());
        CHECK(tuning["speed"].as_int() == 1);
    }

    TEST_CASE("the same bytes saved again are no change") {
        const Root root("same");
        root.put("a.json", R"([1, 2, 3])");
        const rmp::Value value = rmp::assets::load_value("a.json");
        root.put("a.json", R"([1, 2, 3])");
        CHECK_FALSE(changed_now(value));
    }

    TEST_CASE("a file that is not JSON now leaves the Value as it was, and changed()") {
        const Root root("half");
        root.put("t.json", R"({"lives": 3})");
        rmp::Value value = rmp::assets::load_value("t.json");
        root.put("t.json", R"({"lives": 4, "spe)");
        CHECK(changed_now(value));
        CHECK_FALSE(value.reload());
        CHECK(value["lives"].as_int() == 3);
        CHECK(value.changed()); // to be tried again once it is saved whole

        root.put("t.json", R"({"lives": 4, "speed": 2})");
        CHECK(changed_now(value));
        CHECK(value.reload());
        CHECK(value["lives"].as_int() == 4);
        CHECK_FALSE(changed_now(value));
    }

    TEST_CASE("a file gone for a moment is no change, and its return is one") {
        const Root root("gone");
        root.put("g.json", "1");
        rmp::Value value = rmp::assets::load_value("g.json");
        CHECK(value.as_int() == 1);
        root.remove("g.json");
        CHECK_FALSE(changed_now(value));
        CHECK_FALSE(value.reload());
        CHECK(value.as_int() == 1);
        root.put("g.json", "22");
        CHECK(changed_now(value));
        CHECK(value.reload());
        CHECK(value.as_int() == 22);
    }

    TEST_CASE("a file that is not JSON is an empty Value, a failed load, and no watch") {
        const Root root("bad");
        root.put("bad.json", "{ not json");
        const int failed = rmp::assets::failed_loads();
        rmp::Value value = rmp::assets::load_value("bad.json");
        CHECK(value.type() == rmp::Value::Type::NONE);
        CHECK(rmp::assets::failed_loads() == failed + 1);
        root.put("bad.json", "[1]");
        CHECK_FALSE(changed_now(value));
        CHECK_FALSE(value.reload());
    }

    TEST_CASE("a missing file and a refused name are empty and watch nothing") {
        const Root root("missing");
        rmp::Value missing = rmp::assets::load_value("nope.json");
        CHECK(missing.type() == rmp::Value::Type::NONE);
        CHECK_FALSE(changed_now(missing));
        CHECK_FALSE(missing.reload());
        const rmp::Value empty = rmp::assets::load_value("");
        CHECK_FALSE(changed_now(empty));
        root.put("nope.json", "5");
        CHECK_FALSE(changed_now(missing)); // it did not exist when it was read
    }

    TEST_CASE(
        "a Value made in code, read from a save or taken out of another has no file") {
        const Root root("code");
        rmp::Value made = rmp::Value::object();
        made["x"] = 1;
        CHECK_FALSE(changed_now(made));
        CHECK_FALSE(made.reload());
        CHECK(made["x"].as_int() == 1);

        root.put("w.json", R"({"enemy": {"hp": 3}})");
        const rmp::Value whole = rmp::assets::load_value("w.json");
        rmp::Value enemy = whole["enemy"];
        // A copy of the whole is the same file.
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): the copy is tested
        const rmp::Value copy = whole;
        root.put("w.json", R"({"enemy": {"hp": 30}})");
        CHECK(changed_now(whole));
        CHECK(copy.changed());
        CHECK_FALSE(enemy.changed());
        CHECK_FALSE(enemy.reload());
        CHECK(enemy["hp"].as_int() == 3);
    }

    TEST_CASE("two Values of one file are one watch, and each reloads on its own") {
        const Root root("two");
        root.put("s.json", "1");
        rmp::Value a = rmp::assets::load_value("s.json");
        rmp::Value b = rmp::assets::load_value("s.json");
        root.put("s.json", "12");
        CHECK(changed_now(a));
        CHECK(a.reload());
        CHECK_FALSE(a.changed());
        CHECK(b.changed()); // b still holds the old contents
        CHECK(b.as_int() == 1);
        CHECK(b.reload());
        CHECK(b.as_int() == 12);
    }

    TEST_CASE("the files are looked at four times a second, not on every question") {
        const Root root("poll");
        root.put("p.json", "1");
        const rmp::Value value = rmp::assets::load_value("p.json");
        CHECK_FALSE(changed_now(value)); // looked at just now
        root.put("p.json", "123");
        CHECK_FALSE(value.changed()); // not looked at again yet
        rmp::assets::detail::poll_watched_now();
        CHECK(value.changed());
    }

    TEST_CASE("a watch list cleared under a Value is no change") {
        const Root root("forget");
        root.put("f.json", "1");
        rmp::Value value = rmp::assets::load_value("f.json");
        rmp::assets::detail::forget_watched();
        root.put("f.json", "12");
        CHECK_FALSE(changed_now(value));
        CHECK_FALSE(value.reload());
    }

    TEST_CASE("a Value read from the pack watches nothing: the pack does not change") {
        const Root root("pack");
        REQUIRE(
            pack::save(root.path() / "resources.rres",
                       pack::write({ pack::File{ "v.json", pack::bytes_of("[7]") } })));
        root.put("v.json", "[8]");
        REQUIRE(rmp::assets::detail::open_pack());
        rmp::Value value = rmp::assets::load_value("v.json");
        CHECK(value[0].as_int() == 7);
        root.put("v.json", "[9, 9]");
        CHECK_FALSE(changed_now(value));
        CHECK_FALSE(value.reload());
        rmp::assets::detail::close_pack();
    }
}

TEST_SUITE("assets: hot reload, textures") {
    TEST_CASE("a texture's slot sees its file change; an adopted one has no file") {
        using rmp::detail::ResourceKind;
        const Root root("texture");
        rmp::detail::release_all();
        root.put("hero.png", "first");
        // NOLINTNEXTLINE(custom-rmp-raw-pointer): the table's handle, as watch_slot() takes it
        auto *slot =
            rmp::detail::adopt_named(ResourceKind::TEXTURE, "hero.png", 0, Texture2D{});
        REQUIRE(slot != nullptr);
        rmp::detail::watch_slot(*slot, rmp::assets::detail::watch_file("hero.png"));
        rmp::Texture hero{ slot };
        rmp::Texture other = hero; // another handle to the same slot
        CHECK_FALSE(changed_now(hero));
        root.put("hero.png", "second, longer");
        CHECK(changed_now(hero));
        CHECK(other.changed()); // one slot: every handle sees it
        // No window, so the picture does not reach a GPU: the reload fails and
        // the texture stays what it was, still changed.
        CHECK_FALSE(hero.reload());
        CHECK(hero.changed());

        const rmp::Texture adopted{ rmp::detail::adopt(ResourceKind::TEXTURE,
                                                       Texture2D{}) };
        CHECK_FALSE(changed_now(adopted));
        rmp::Texture copy = adopted;
        CHECK_FALSE(copy.reload());

        const rmp::Texture empty;
        CHECK_FALSE(changed_now(empty));
        const rmp::Texture moved = std::move(other);
        CHECK(moved.changed());
        rmp::detail::release_all();
        CHECK_FALSE(changed_now(hero)); // released under the handle
        CHECK_FALSE(hero.reload());
    }

    TEST_CASE(
        "replace_payload swaps what every handle reads, and refuses an empty slot") {
        using rmp::detail::ResourceKind;
        rmp::detail::release_all();
        ::Image first{};
        first.width = 1;
        ::Image second{};
        second.width = 2;
        // NOLINTNEXTLINE(custom-rmp-raw-pointer): the table's handle, as replace_payload() takes it
        auto *slot = rmp::detail::adopt_named(ResourceKind::IMAGE, "i.png", 0, first);
        REQUIRE(slot != nullptr);
        const rmp::Image a{ slot };
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): a second handle
        const rmp::Image b = a;
        CHECK_FALSE(rmp::detail::replace_payload(*slot, nullptr));
        CHECK(a.raw().width == 1);
        CHECK(rmp::detail::replace_payload(*slot, std::make_shared<::Image>(second)));
        CHECK(a.raw().width == 2);
        CHECK(b.raw().width == 2);
        CHECK(a.name() == "i.png");
        CHECK(rmp::detail::ref_count("i.png") == 2);
        CHECK_FALSE(rmp::detail::slot_watched(*slot)); // never watched
        rmp::detail::release_all();
        CHECK_FALSE(
            rmp::detail::replace_payload(*slot, std::make_shared<::Image>(first)));
        CHECK(a.raw().width == 0);
    }
}

#endif // !RMP_PRODUCTION_BUILD
