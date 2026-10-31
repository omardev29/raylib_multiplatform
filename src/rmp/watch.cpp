// ===========================================================================
// Watching resources/ for files that change while the game runs.
//
// A Value read with rmp::assets::load_value() and a texture read with
// load_texture() remember their file here, and changed() asks whether it is
// different now from what they hold: a number tuned in tuning.json, a sprite
// redrawn, seen without restarting the game.
//
// Debug builds on the desktop only. A release reads its resources from the
// pack, which does not change, and a phone or a browser has no file anyone
// edits: there watch_file() is -1 and nothing is ever looked at.
//
// changed() is meant for every frame, so it must cost nothing: it compares two
// numbers. The files themselves are looked at here at most every POLL_SECONDS,
// the first time someone asks after that: their size and modification time,
// and, only when one of those moved, their bytes -- an editor that saves the
// same contents is no change.
// ===========================================================================

#include "internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace rmp::assets::detail {

namespace {

constexpr double POLL_SECONDS = 0.25;

struct Watched {
    std::string path; // the loose file, under resources_root()
    std::filesystem::file_time_type time{};
    std::uintmax_t size = 0;
    std::uint64_t hash = 0;
    unsigned generation = 1; // what changed() compares; moves with each change
};

struct {
    std::vector<Watched> files;
    // The steady clock's count at the last look; a plain number, so that the
    // state is built without anything that could throw.
    std::chrono::steady_clock::rep last = 0;
    bool polled = false;
} watch;

// C's stdio, closed by its handle: the way src/rmp reads a file without
// iostreams (tools/style_check.sh S8).
struct Close {
    // The deleter IS the owner: this is where an owning FILE * ends.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,custom-rmp-raw-pointer)
    void operator()(std::FILE *file) const { std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, Close>;

File open_read(const std::string &path) { return File(std::fopen(path.c_str(), "rb")); }

// A file this big is not a tuning table or a sprite: it is not read.
constexpr std::uintmax_t MOST_BYTES = std::uintmax_t{ 256 } * 1024 * 1024;

// Every byte of `path`, sized first and read in one call; false when it is
// not there, not readable whole, or past MOST_BYTES.
bool read_bytes(const std::string &path, std::string &out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > MOST_BYTES) return false;
    const File file = open_read(path);
    if (!file) return false;
    out.assign(static_cast<std::size_t>(size), '\0');
    if (out.empty()) return true;
    return std::fread(out.data(), 1, out.size(), file.get()) == out.size();
}

// FNV-1a over the file's bytes: 0 for a file that cannot be read.
std::uint64_t hash_of(const std::string &path) {
    std::string bytes;
    if (!read_bytes(path, bytes)) return 0;
    std::uint64_t h = 1469598103934665603ULL;
    for (const char byte : bytes) {
        h ^= static_cast<unsigned char>(byte);
        h *= 1099511628211ULL;
    }
    return h;
}

// Size and time of `w`'s file, false when it is not there now (being saved:
// an editor may write a new file and rename it over the old one).
bool stat_of(const Watched &w, std::filesystem::file_time_type &time,
             std::uintmax_t &size) {
    std::error_code ec;
    time = std::filesystem::last_write_time(w.path, ec);
    if (ec) return false;
    size = std::filesystem::file_size(w.path, ec);
    return !ec;
}

void poll() {
    using std::chrono::steady_clock;
    const steady_clock::rep now = steady_clock::now().time_since_epoch().count();
    const double since =
        std::chrono::duration<double>(steady_clock::duration(now - watch.last)).count();
    if (watch.polled && since < POLL_SECONDS) return;
    watch.polled = true;
    watch.last = now;
    for (Watched &w : watch.files) {
        std::filesystem::file_time_type time{};
        std::uintmax_t size = 0;
        if (!stat_of(w, time, size) || (time == w.time && size == w.size)) continue;
        w.time = time;
        w.size = size;
        const std::uint64_t hash = hash_of(w.path);
        if (hash == w.hash) continue; // saved again, the same bytes
        w.hash = hash;
        ++w.generation;
    }
}

} // namespace

int watch_file(std::string_view name) {
#if RMP_PRODUCTION_BUILD || defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS) || \
    defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
    (void)name;
    return -1;
#else
    // With the pack open the game reads the pack, not the loose file: a change
    // to resources/ would not be what it loads again (`rmp unpack`).
    if (name.empty() || pack_is_open()) return -1;
    std::string path = std::string(resources_root()) + std::string(name);
    for (std::size_t i = 0; i < watch.files.size(); ++i) {
        if (watch.files[i].path == path) return static_cast<int>(i);
    }
    Watched w;
    w.path = std::move(path);
    if (!stat_of(w, w.time, w.size)) return -1;
    w.hash = hash_of(w.path);
    watch.files.push_back(std::move(w));
    return static_cast<int>(watch.files.size() - 1);
#endif
}

unsigned watch_generation(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= watch.files.size()) return 0;
    poll();
    return watch.files[static_cast<std::size_t>(index)].generation;
}

std::optional<std::string> read_watched(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= watch.files.size())
        return std::nullopt;
    std::string text;
    if (!read_bytes(watch.files[static_cast<std::size_t>(index)].path, text))
        return std::nullopt;
    return text;
}

void poll_watched_now() { watch.polled = false; }

void forget_watched() {
    watch.files.clear();
    watch.polled = false;
}

} // namespace rmp::assets::detail
