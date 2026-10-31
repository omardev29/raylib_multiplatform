// ---------------------------------------------------------------------------
// Hot reload held to a model over generated edits (src/rmp/watch.cpp):
//
//   A file under resources/ is written, saved again unchanged, broken (not
//   JSON, as an editor half way through a save leaves it), removed, and read
//   again with reload(), in any order. The model keeps what the watcher last
//   saw and the generation it counts; the Value read with load_value() must
//   answer changed() exactly when that generation moved since it read, and
//   reload() must make it the file's contents when the file is there and is
//   JSON -- and leave it as it was otherwise. A copy of the Value answers as
//   it does; a Value taken out of it never changes.
//
// The clock of the files is the model's: each write sets the file's time one
// second later, as an editor's save a moment after the last would, so two
// writes inside the file system's timestamp granularity are still two.
// ---------------------------------------------------------------------------

// covers: rmp::assets::load_value(std::string_view)
// covers: rmp::Value::changed() const
// covers: rmp::Value::reload()

#include "check.h"
#include "gen.h"
#include "value_model.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/save_internal.h"

#include <rmp/assets.h>
#include <rmp/config.h>
#include <rmp/save.h>

#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#if !RMP_PRODUCTION_BUILD

namespace {

struct Write {
    rmp::Value value;
};
struct SaveSame {};
struct Break {};
struct Remove {};
struct Reload {};
using Step = std::variant<Write, SaveSame, Break, Remove, Reload>;

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Step &step, std::ostream &os) {
    switch (step.index()) {
        case 0:
            os << "write " << rmp::save::detail::to_json(std::get<Write>(step).value);
            break;
        case 1:
            os << "save the same bytes";
            break;
        case 2:
            os << "break it";
            break;
        case 3:
            os << "remove it";
            break;
        default:
            os << "reload";
            break;
    }
}

rc::Gen<rmp::Value> savable_value() {
    return rc::gen::suchThat(rmp::test::gen::value(), [](const rmp::Value &v) {
        return rmp::test::model::savable(v);
    });
}

rc::Gen<Step> step() {
    return rc::gen::weightedOneOf<Step>({
        { 4,
          rc::gen::map(savable_value(),
                       [](rmp::Value v) { return Step{ Write{ std::move(v) } }; }) },
        { 1, rc::gen::just(Step{ SaveSame{} }) },
        { 1, rc::gen::just(Step{ Break{} }) },
        { 1, rc::gen::just(Step{ Remove{} }) },
        { 3, rc::gen::just(Step{ Reload{} }) },
    });
}

// The watcher as the model sees it: the bytes it last hashed and the
// generation those moved.
struct Watcher {
    std::optional<std::string> file; // what is on disk, nothing when removed
    std::string watched; // the bytes the watcher last saw
    unsigned generation = 1;
    unsigned seen = 1; // the generation the Value holds
    rmp::Value held;

    // What a poll does: a file there whose bytes are not the last seen moves
    // the generation; a file gone is no change.
    void poll() {
        if (file && *file != watched) {
            watched = *file;
            ++generation;
        }
    }
};

class Folder {
public:
    Folder() {
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) / "rmp-watch-prop";
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
        _root = (_path / "").string();
        _previous = rmp::assets::detail::resources_root();
        rmp::assets::detail::set_resources_root(_root.c_str());
        rmp::assets::detail::forget_watched();
        _time = std::filesystem::file_time_type::clock::now();
    }
    ~Folder() {
        rmp::assets::detail::forget_watched();
        rmp::assets::detail::set_resources_root(_previous.c_str());
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }
    Folder(const Folder &) = delete;
    Folder &operator=(const Folder &) = delete;
    Folder(Folder &&) = delete;
    Folder &operator=(Folder &&) = delete;

    void put(const std::string &bytes) {
        const auto file = _path / "data.json";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << bytes;
        }
        _time += std::chrono::seconds(1);
        std::error_code error;
        std::filesystem::last_write_time(file, _time, error);
    }
    void remove() const {
        std::error_code error;
        std::filesystem::remove(_path / "data.json", error);
    }

private:
    std::filesystem::path _path;
    std::string _root;
    std::string _previous;
    std::filesystem::file_time_type _time;
};

std::optional<rmp::Value> parsed(const std::string &bytes) {
    rmp::Value out;
    if (!rmp::save::detail::from_json(bytes, &out)) return std::nullopt;
    return out;
}

} // namespace

TEST_SUITE("property: hot reload") {
    TEST_CASE("changed() and reload() against a model of the watcher") {
        rmp::test::check(
            "a Value read with load_value() changes and reloads exactly as the model "
            "says",
            [] {
                const rmp::Value first = *savable_value();
                const auto steps = *rc::gen::container<std::vector<Step>>(step());

                Folder folder;
                Watcher expected;
                expected.file = rmp::save::detail::to_json(first);
                expected.watched = *expected.file;
                folder.put(*expected.file);
                expected.held = *parsed(*expected.file);

                rmp::Value value = rmp::assets::load_value("data.json");
                RC_ASSERT(value == expected.held);
                const rmp::Value inner = value;
                const rmp::Value &read = value;
                const rmp::Value taken =
                    read.type() == rmp::Value::Type::LIST && read.size() > 0
                    ? read[0]
                    : rmp::Value{};

                for (const Step &s : steps) {
                    if (std::holds_alternative<Write>(s)) {
                        expected.file =
                            rmp::save::detail::to_json(std::get<Write>(s).value);
                        folder.put(*expected.file);
                    } else if (std::holds_alternative<SaveSame>(s)) {
                        if (expected.file) folder.put(*expected.file);
                    } else if (std::holds_alternative<Break>(s)) {
                        expected.file = std::string("{\"cut\": ");
                        folder.put(*expected.file);
                    } else if (std::holds_alternative<Remove>(s)) {
                        expected.file.reset();
                        folder.remove();
                    } else {
                        rmp::assets::detail::poll_watched_now();
                        expected.poll();
                        const auto now =
                            expected.file ? parsed(*expected.file) : std::nullopt;
                        RC_ASSERT(value.reload() == now.has_value());
                        if (now) {
                            expected.held = *now;
                            expected.seen = expected.generation;
                        }
                        RC_ASSERT(value == expected.held);
                    }
                    rmp::assets::detail::poll_watched_now();
                    expected.poll();
                    RC_ASSERT(value.changed() == (expected.generation != expected.seen));
                    RC_ASSERT(value == expected.held);
                    RC_ASSERT(inner.changed() == (expected.generation != 1));
                    RC_ASSERT(!taken.changed());
                }
            });
    }
}

#endif // !RMP_PRODUCTION_BUILD
