// ---------------------------------------------------------------------------
// rmp::save: a Value to a file and back, and where the file goes.
//
// The format is documented in save_internal.h, the API in rmp/save.h. What is
// here, in the order a write goes through it: the Value becomes JSON through
// cJSON (the only file that includes it), optionally sealed with monocypher's
// XChaCha20-Poly1305, framed with a header and a CRC-32, written to a
// temporary file, flushed to the disk, and renamed over the old save -- so at
// every instant there is one complete save on disk, the old or the new.
//
// WHY MONOCYPHER AND NOT tiny-AES-c, since both come with rres. The roadmap
// said "reuse the AES rres brings" and asked whether that was acceptable. It
// is not the better of the two: tiny-AES-c gives CTR or CBC with no
// authentication, so a flipped byte in a sealed save decrypts to garbage that
// only the JSON parser might notice. XChaCha20-Poly1305 is authenticated --
// any change to a sealed file fails the tag and reads as MODIFIED, which is
// the distinction the whole checksum design exists to make -- and its 24-byte
// nonce is safe to pick at random. Still zero new crypto vendored.
// ---------------------------------------------------------------------------

#include <rmp/save.h>

#include "internal.h"
#include "save_internal.h"

#include <cJSON.h>
#include <external/monocypher.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h> // _commit, _fileno
#else
#include <unistd.h> // fsync, fileno
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#if defined(PLATFORM_ANDROID)
#include <android_native_app_glue.h> // struct android_app::activity
#include <raymob.h> // GetAndroidApp()
#endif

namespace fs = std::filesystem;

namespace rmp::save {

namespace {

constexpr int kFormat = 1;
constexpr const char *kMagic = "rmp-save";
constexpr const char *kExtension = ".save";
constexpr std::size_t kNonce = 24;
constexpr std::size_t kTag = 16;
// cJSON refuses to parse deeper than this, so nothing deeper is written: a
// save that could be written and never read back is the worst kind.
constexpr int kMaxDepth = CJSON_NESTING_LIMIT;

// ---- CRC-32 ----------------------------------------------------------------

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; i++) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
        table[i] = c;
    }
    return table;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

// ---- cJSON, owned ----------------------------------------------------------

struct JsonDelete {
    void operator()(cJSON *node) const { cJSON_Delete(node); }
};
using Json = std::unique_ptr<cJSON, JsonDelete>;

struct JsonTextDelete {
    void operator()(char *text) const { cJSON_free(text); }
};
using JsonText = std::unique_ptr<char, JsonTextDelete>;

// A number as the shortest text that reads back as exactly the same double.
// Not cJSON's printer: it tries 15 digits and keeps them when they read back
// within an epsilon, so 2^53 came back as 9007199254740990 -- a save that does
// not return what was written. And in "C" notation whatever the locale: a game
// that sets a German locale would otherwise write 0,1, which is not JSON.
std::string format_number(double n) {
    char text[40];
    for (int digits = 15; digits <= 17; digits++) {
        std::snprintf(text, sizeof text, "%.*g", digits, n);
        // Read back in the same locale it was printed in; the point is fixed
        // only afterwards.
        if (std::strtod(text, nullptr) == n) break;
    }
    const char *point = std::localeconv()->decimal_point;
    std::string out(text);
    if (point != nullptr && point[0] != '\0' && std::strcmp(point, ".") != 0) {
        const std::size_t at = out.find(point);
        if (at != std::string::npos) out.replace(at, std::strlen(point), ".");
    }
    return out;
}

// A Value into a new cJSON tree. Null when it is nested past kMaxDepth or
// cJSON runs out of memory. Object members that are NONE are left out: a key
// holding nothing and a missing key read the same, and writing the first as
// `"key": null` would only grow the file.
Json to_cjson(const Value &v, int depth) {
    if (depth > kMaxDepth) return nullptr;
    switch (v.type()) {
        case Value::Type::NONE:
            return Json(cJSON_CreateNull());
        case Value::Type::BOOL:
            return Json(cJSON_CreateBool(v.as_bool() ? 1 : 0));
        case Value::Type::NUMBER: {
            // JSON has no NaN and no infinity; they are written as null and so
            // read back as the default, which is what they meant anyway.
            const double n = rmp::detail::ValueAccess::number(v);
            if (!std::isfinite(n)) return Json(cJSON_CreateNull());
            // Raw: printed exactly as format_number() wrote it. cJSON's parser
            // reads it back with strtod, which is exact.
            return Json(cJSON_CreateRaw(format_number(n).c_str()));
        }
        case Value::Type::STRING:
            return Json(cJSON_CreateString(std::string(v.as_string()).c_str()));
        case Value::Type::LIST: {
            Json list(cJSON_CreateArray());
            if (!list) return nullptr;
            for (int i = 0; i < v.size(); i++) {
                Json item = to_cjson(v[i], depth + 1);
                if (!item) return nullptr;
                cJSON_AddItemToArray(list.get(), item.release());
            }
            return list;
        }
        case Value::Type::OBJECT: {
            Json object(cJSON_CreateObject());
            if (!object) return nullptr;
            for (int i = 0; i < v.size(); i++) {
                const std::string_view key = v.key(i);
                const Value &item = v[key];
                if (item.type() == Value::Type::NONE) continue;
                Json node = to_cjson(item, depth + 1);
                if (!node) return nullptr;
                cJSON_AddItemToObject(object.get(), std::string(key).c_str(),
                                      node.release());
            }
            return object;
        }
    }
    return nullptr;
}

Value from_cjson(const cJSON *node) {
    if (cJSON_IsBool(node)) return { cJSON_IsTrue(node) != 0 };
    if (cJSON_IsNumber(node)) return { node->valuedouble };
    if (cJSON_IsString(node)) {
        return { node->valuestring != nullptr ? node->valuestring : "" };
    }
    if (cJSON_IsArray(node)) {
        Value list = Value::list();
        for (const cJSON *child = node->child; child != nullptr; child = child->next) {
            list.push(from_cjson(child));
        }
        return list;
    }
    if (cJSON_IsObject(node)) {
        Value object = Value::object();
        for (const cJSON *child = node->child; child != nullptr; child = child->next) {
            if (child->string == nullptr || cJSON_IsNull(child)) continue;
            object[child->string] = from_cjson(child);
        }
        return object;
    }
    return {}; // null, and anything cJSON did not recognise
}

// ---- sealing ---------------------------------------------------------------

// The key, from the game's name. It is in the binary, and that is said in
// rmp/save.h: this is tamper resistance. Derived rather than a literal so two
// games made with this framework cannot open each other's saves -- and so
// renaming [project] name makes old sealed saves unreadable, which the
// .toml's [save] comment says.
const std::array<std::uint8_t, 32> &seal_key() {
    static const std::array<std::uint8_t, 32> kKey = [] {
        std::array<std::uint8_t, 32> k{};
        const std::string material = std::string("rmp::save sealed v1") + '\0' + APP_NAME;
        crypto_blake2b(k.data(), k.size(),
                       reinterpret_cast<const std::uint8_t *>(material.data()),
                       material.size());
        return k;
    }();
    return kKey;
}

// 24 random bytes. A nonce must never repeat under one key, and at random
// with 192 bits it will not. std::random_device is the OS's entropy on every
// target we build; if it throws (it may, where there is none), the clock and
// a counter stand in -- weaker, and still never the same twice in a run.
std::array<std::uint8_t, kNonce> make_nonce() {
    std::array<std::uint8_t, kNonce> nonce{};
    static std::uint64_t counter = 0;
    counter++;
    try {
        std::random_device device;
        for (std::size_t i = 0; i < kNonce; i += 4) {
            const std::uint32_t r = device();
            std::memcpy(nonce.data() + i, &r, 4);
        }
    } catch (...) {
        const auto t = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::memcpy(nonce.data(), &t, sizeof t);
    }
    // The counter is mixed in either way, so two writes in the same tick with
    // a broken device still differ.
    for (std::size_t i = 0; i < sizeof counter; i++) {
        nonce[kNonce - 1 - i] ^= static_cast<std::uint8_t>(counter >> (8 * i));
    }
    return nonce;
}

// A field of the header as a number: digits only, all of them used, and in
// range for T. from_chars and not sscanf: sscanf takes "+1", " 1" and "1x",
// and says nothing when the number does not fit.
template <class T> bool whole_number(std::string_view text, int base, T *out) {
    if (text.empty()) return false;
    const std::from_chars_result r =
        std::from_chars(text.data(), text.data() + text.size(), *out, base);
    return r.ec == std::errc() && r.ptr == text.data() + text.size();
}

std::string header_prefix(int version, bool sealed, std::size_t payload) {
    return std::string(kMagic) + " " + std::to_string(kFormat) + " " +
        std::to_string(version) + " " + (sealed ? "sealed" : "plain") + " " +
        std::to_string(payload);
}

std::uint32_t crc_of(const std::string &prefix, const unsigned char *payload,
                     std::size_t size) {
    // The CRC covers "prefix\n" and the payload, as one run of bytes.
    std::string head = prefix + "\n";
    std::uint32_t c = 0xFFFFFFFFU;
    for (const char ch : head)
        c = kCrcTable[(c ^ static_cast<unsigned char>(ch)) & 0xFFU] ^ (c >> 8U);
    for (std::size_t i = 0; i < size; i++)
        c = kCrcTable[(c ^ payload[i]) & 0xFFU] ^ (c >> 8U);
    return c ^ 0xFFFFFFFFU;
}

} // namespace

// ---------------------------------------------------------------------------
// The format, as bytes
// ---------------------------------------------------------------------------

namespace detail {

std::uint32_t crc32(const unsigned char *data, std::size_t size) {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; i++)
        c = kCrcTable[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
    return c ^ 0xFFFFFFFFU;
}

std::string to_json(const Value &value) {
    const Json tree = to_cjson(value, 0);
    if (!tree) return {};
    const JsonText text(cJSON_PrintUnformatted(tree.get()));
    return text ? std::string(text.get()) : std::string();
}

bool from_json(std::string_view json, Value *out) {
    // The whole text has to be one JSON value, with nothing after it but
    // whitespace. cJSON on its own stops after the first value and ignores
    // what follows, so `{"a":1}garbage` read as {"a":1}; asked to require the
    // terminating NUL, it refuses anything after the value -- a NUL in the
    // middle followed by more text included -- so the length handed over
    // counts the NUL std::string keeps at the end.
    const std::string text(json);
    const Json tree(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, 1));
    if (!tree) return false;
    *out = from_cjson(tree.get());
    return true;
}

bool encode(const Value &value, int version, bool sealed, Bytes *out) {
    const std::string json = to_json(value);
    if (json.empty()) return false;

    Bytes payload;
    if (sealed) {
        payload.resize(kNonce + kTag + json.size());
        const std::array<std::uint8_t, kNonce> nonce = make_nonce();
        std::memcpy(payload.data(), nonce.data(), kNonce);
        const std::string prefix = header_prefix(version, true, payload.size());
        crypto_aead_lock(
            payload.data() + kNonce + kTag, payload.data() + kNonce, seal_key().data(),
            nonce.data(), reinterpret_cast<const std::uint8_t *>(prefix.data()),
            prefix.size(), reinterpret_cast<const std::uint8_t *>(json.data()),
            json.size());
    } else {
        payload.assign(json.begin(), json.end());
    }

    const std::string prefix = header_prefix(version, sealed, payload.size());
    char crc[16];
    std::snprintf(crc, sizeof crc, " %08x\n",
                  static_cast<unsigned>(crc_of(prefix, payload.data(), payload.size())));
    out->assign(prefix.begin(), prefix.end());
    out->insert(out->end(), crc, crc + std::strlen(crc));
    out->insert(out->end(), payload.begin(), payload.end());
    return true;
}

Status decode(const Bytes &file, Value *out) {
    // The header line. A file that stops before its '\n' but starts like one
    // of ours was cut short; one that does not start like ours never was.
    const std::string magic = std::string(kMagic) + " ";
    const std::size_t probe = file.size() < magic.size() ? file.size() : magic.size();
    if (probe == 0 || std::memcmp(file.data(), magic.data(), probe) != 0) {
        return Status::UNREADABLE;
    }
    std::size_t newline = 0;
    while (newline < file.size() && newline < 128 && file[newline] != '\n') newline++;
    if (newline >= file.size()) return Status::TRUNCATED;
    if (file[newline] != '\n')
        return Status::UNREADABLE; // a header longer than any we write

    const std::string header(file.begin(),
                             file.begin() + static_cast<std::ptrdiff_t>(newline));
    // Six fields separated by single spaces, every number all digits: strict,
    // because nothing but our own writer produces this line, and anything it
    // would not write is not a file of ours.
    std::vector<std::string_view> field;
    for (std::size_t at = 0; at <= header.size();) {
        const std::size_t space = std::min(header.find(' ', at), header.size());
        field.emplace_back(header.data() + at, space - at);
        at = space + 1;
    }
    int format = 0;
    int version = 0;
    std::size_t declared = 0;
    std::uint32_t crc = 0;
    if (field.size() != 6 || field[0] != kMagic || !whole_number(field[1], 10, &format) ||
        !whole_number(field[2], 10, &version) || !whole_number(field[4], 10, &declared) ||
        field[5].size() != 8 || !whole_number(field[5], 16, &crc)) {
        return Status::UNREADABLE;
    }
    const std::string_view kind = field[3];
    if (format != kFormat) return Status::UNREADABLE; // a newer framework wrote it
    const bool sealed = kind == "sealed";
    if (!sealed && kind != "plain") return Status::UNREADABLE;

    const std::size_t have = file.size() - newline - 1;
    if (have < declared) return Status::TRUNCATED;
    if (have > declared) return Status::MODIFIED; // something was appended

    const unsigned char *payload = file.data() + newline + 1;
    const std::string prefix = header_prefix(version, sealed, have);
    if (crc_of(prefix, payload, have) != crc) return Status::MODIFIED;

    std::string json;
    if (sealed) {
        if (have < kNonce + kTag) return Status::MODIFIED;
        json.resize(have - kNonce - kTag);
        if (crypto_aead_unlock(reinterpret_cast<std::uint8_t *>(json.data()),
                               payload + kNonce, seal_key().data(), payload,
                               reinterpret_cast<const std::uint8_t *>(prefix.data()),
                               prefix.size(), payload + kNonce + kTag,
                               json.size()) != 0) {
            // The CRC matched and the seal did not: somebody recomputed the
            // CRC, or the game was renamed and the key moved with it.
            return Status::MODIFIED;
        }
    } else {
        json.assign(reinterpret_cast<const char *>(payload), have);
    }

    Value parsed;
    if (!from_json(json, &parsed)) return Status::UNREADABLE;
    rmp::detail::ValueAccess::set_version(parsed, version);
    *out = std::move(parsed);
    return Status::OK;
}

bool valid_slot(std::string_view slot) {
    if (slot.empty() || slot.size() > 64 || slot.front() == '.') return false;
    return std::ranges::all_of(slot, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

} // namespace detail

} // namespace rmp::save

// ---------------------------------------------------------------------------
// Where the files go
// ---------------------------------------------------------------------------

namespace rmp::save {

namespace {

struct Folders {
    bool overridden = false; // a test pointed them somewhere
    std::string portable; // empty = [save] portable is off, or this OS has no such place
    std::string user;
    bool resolved = false;
    std::string chosen;
    int fallbacks = 0;
};

Folders &folders() {
    static Folders f;
    return f;
}

std::string env(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string with_separator(std::string path) {
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    return path;
}

} // namespace

// The user's data folder, per family. The name is [project] name, which
// configure.py only accepts in a form every file system takes.
std::string detail::user_folder() {
#if defined(__EMSCRIPTEN__)
    // Mounted on IndexedDB before main() by cmake/web/rmp_web.js.
    return "/rmp_save/";
#elif defined(PLATFORM_ANDROID)
    // The app's private internal storage: no permission needed, removed with
    // the app, invisible to other apps.
    struct android_app *app = GetAndroidApp();
    if (app != nullptr && app->activity != nullptr &&
        app->activity->internalDataPath != nullptr) {
        return with_separator(app->activity->internalDataPath) + "saves/";
    }
    return "saves/";
#elif defined(PLATFORM_IOS)
    // Application Support and NOT Documents: the iOS Data Storage Guidelines
    // reject apps that put data the user did not create into Documents,
    // which is what the Files app shows. The container is the app's own, so
    // no name under it.
    return with_separator(env("HOME")) + "Library/Application Support/";
#elif defined(__APPLE__)
    return with_separator(env("HOME")) + "Library/Application Support/" APP_NAME "/";
#elif defined(_WIN32)
    // %APPDATA% is Roaming, which is where a save that should follow the
    // player to another machine on a domain belongs.
    const std::string appdata = env("APPDATA");
    if (!appdata.empty()) return with_separator(appdata) + APP_NAME "/";
    return with_separator(env("USERPROFILE")) + "AppData/Roaming/" APP_NAME "/";
#else
    // XDG: $XDG_DATA_HOME when it is set and absolute, as the spec requires,
    // else ~/.local/share.
    const std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty() && xdg.front() == '/') return with_separator(xdg) + APP_NAME "/";
    return with_separator(env("HOME")) + ".local/share/" APP_NAME "/";
#endif
}

namespace {

// Next to the executable, where [save] portable asks for it and the OS has
// such a place. Empty otherwise.
std::string portable_folder() {
#if APP_SAVE_PORTABLE &&                                              \
    (defined(_WIN32) || defined(__linux__) || defined(__FreeBSD__) || \
     defined(__NetBSD__) || defined(__OpenBSD__)) &&                  \
    !defined(PLATFORM_ANDROID) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    const char *dir = GetApplicationDirectory();
    // raylib answers "" or "./" where it cannot tell (NetBSD, OpenBSD): a
    // folder relative to wherever the game was launched from is not "next to
    // the executable", so that is no portable folder at all.
    if (dir == nullptr || dir[0] == '\0' || std::strcmp(dir, "./") == 0) {
        TraceLog(LOG_WARNING,
                 "SAVE: [save] portable: this system does not say where the "
                 "executable is; saving in the user's folder instead");
        return {};
    }
    return with_separator(dir) + "saves/";
#else
    return {};
#endif
}

fs::path to_path(const std::string &utf8) {
    // u8path is deprecated in C++20, and it is the one spelling that means
    // "these bytes are UTF-8" on Windows too, where %APPDATA% can hold a
    // user name that is not in the ANSI code page.
    return std::u8string(utf8.begin(), utf8.end());
}

struct FileClose {
    // The deleter IS the owner: this is where an owning FILE* ends.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    void operator()(std::FILE *f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileClose>;

File open_file(const fs::path &path, bool for_write) {
#if defined(_WIN32)
    return File(_wfopen(path.c_str(), for_write ? L"wb" : L"rb"));
#else
    return File(std::fopen(path.c_str(), for_write ? "wb" : "rb"));
#endif
}

// Whether we can create files in `folder`, found out the only reliable way:
// by making it and writing into it. Permissions lie -- a portable build under
// Program Files is "writable" to a check and redirected or refused on write.
bool writable(const std::string &folder) {
    std::error_code ec;
    fs::create_directories(to_path(folder), ec);
    if (ec && !fs::is_directory(to_path(folder), ec)) return false;
    const fs::path probe = to_path(folder + ".rmp-write-test");
    bool wrote = false;
    {
        const File f = open_file(probe, true);
        if (!f) return false;
        wrote = std::fputc('x', f.get()) != EOF && std::fflush(f.get()) == 0;
    }
    fs::remove(probe, ec);
    return wrote;
}

void give_up_on_portable(Folders &f) {
    f.fallbacks++;
    // A plain log and not RMP_REPORT_ONCE: a read-only install folder is the
    // machine, not a mistake in the game, and [dev] strict must not abort it.
    // Once, because `chosen` is the user's folder from here on.
    TraceLog(
        LOG_WARNING,
        "SAVE: cannot write in %s (a portable build in a read-only folder, like Program "
        "Files?); saving in %s instead",
        f.portable.c_str(), f.user.c_str());
    f.chosen = f.user;
    f.portable.clear();
}

const std::string &resolve() {
    Folders &f = folders();
    if (f.resolved) return f.chosen;
    if (!f.overridden) {
        f.portable = portable_folder();
        f.user = detail::user_folder();
    }
    f.resolved = true;
    f.chosen = f.user;
    if (!f.portable.empty()) {
        if (writable(f.portable)) {
            f.chosen = f.portable;
        } else {
            give_up_on_portable(f);
        }
    }
    return f.chosen;
}

std::string file_for(std::string_view slot) {
    return resolve() + std::string(slot) + kExtension;
}

void persist() {
#if defined(__EMSCRIPTEN__)
    // Without this the save lives in memory until the tab is closed and is
    // then gone: IDBFS only reaches IndexedDB on a sync. It is asynchronous,
    // and that is fine -- the file is already complete in memory.
    EM_ASM({
        FS.syncfs(
            false, function(err) {
                if (err)
                    console.warn('rmp::save: could not persist to IndexedDB: ' + err);
            });
    });
#endif
}

// Everything in the file, or false when it cannot be opened or read whole.
// Sized first and read in one call. And capped: a save is kilobytes, and a
// 4 GB file that happens to be called slot1.save must be UNREADABLE, not a
// bad_alloc out of a function that promises not to throw.
constexpr std::uintmax_t kMaxFile = std::uintmax_t{ 64 } * 1024 * 1024;

bool read_file(const fs::path &path, detail::Bytes *out) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxFile) return false;
    const File f = open_file(path, false);
    if (!f) return false;
    out->assign(static_cast<std::size_t>(size), 0);
    if (out->empty()) return true;
    return std::fread(out->data(), 1, out->size(), f.get()) == out->size();
}

// The bytes into `path`, all or nothing: a temporary next to it, flushed to
// the disk, then renamed over it. rename() replaces atomically on POSIX, and
// std::filesystem::rename does on Windows too (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING), which plain std::rename does not.
bool write_atomically(const fs::path &path, const detail::Bytes &bytes) {
    fs::path temp = path;
    temp += ".tmp";
    {
        const File f = open_file(temp, true);
        if (!f) return false;
        const bool wrote =
            std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size();
        bool flushed = std::fflush(f.get()) == 0;
        // fflush only empties the C library's buffer into the OS; this is what
        // gets it onto the disk before the rename can make it the save.
#if defined(_WIN32)
        flushed = flushed && _commit(_fileno(f.get())) == 0;
#elif !defined(__EMSCRIPTEN__)
        flushed = flushed && fsync(fileno(f.get())) == 0;
#endif
        if (!wrote || !flushed) {
            std::error_code ec;
            fs::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool check_slot(std::string_view slot) {
    if (detail::valid_slot(slot)) return true;
    RMP_REPORT_ONCE_KEYED(
        std::string(slot).c_str(),
        "SAVE: \"%s\" is not a slot name: letters, digits, '_', '-' and '.', "
        "not starting with '.', at most 64. A slot is a file name, not a path",
        std::string(slot).c_str());
    return false;
}

} // namespace

namespace detail {

void set_folders_for_tests(const std::string &portable, const std::string &user) {
    Folders &f = folders();
    f = Folders{};
    f.overridden = true;
    f.portable = portable.empty() ? std::string() : with_separator(portable);
    f.user = with_separator(user);
}

void reset_for_tests() { folders() = Folders{}; }

int fallbacks() { return folders().fallbacks; }

} // namespace detail

// ---------------------------------------------------------------------------
// The public half
// ---------------------------------------------------------------------------

bool write(std::string_view slot, const Value &value, const WriteOptions &options) {
    if (!check_slot(slot)) return false;
    detail::Bytes bytes;
    if (!detail::encode(value, APP_SAVE_VERSION, options.encrypted, &bytes)) {
        RMP_REPORT_ONCE(
            "SAVE: a Value nested deeper than %d levels cannot be saved: it could "
            "not be read back",
            kMaxDepth);
        return false;
    }
    Folders &f = folders();
    (void)resolve();
    std::error_code ec;
    fs::create_directories(to_path(f.chosen), ec);
    if (write_atomically(to_path(file_for(slot)), bytes)) {
        persist();
        return true;
    }
    // The portable folder passed the probe and failed now (the disk filled,
    // a permission changed under us): the user's folder, once, rather than
    // losing the save.
    if (f.chosen != f.user) {
        give_up_on_portable(f);
        fs::create_directories(to_path(f.chosen), ec);
        if (write_atomically(to_path(file_for(slot)), bytes)) {
            persist();
            return true;
        }
    }
    TraceLog(LOG_WARNING, "SAVE: could not write %s", file_for(slot).c_str());
    return false;
}

Result read(std::string_view slot, Value *out) {
    Result result;
    if (out == nullptr || !check_slot(slot)) {
        result.status = Status::UNREADABLE;
        return result;
    }
    const fs::path path = to_path(file_for(slot));
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        result.status = Status::MISSING;
        return result;
    }
    detail::Bytes bytes;
    if (!read_file(path, &bytes)) {
        result.status = Status::UNREADABLE;
        return result;
    }
    result.status = detail::decode(bytes, out);
    return result;
}

bool exists(std::string_view slot) {
    if (!detail::valid_slot(slot)) return false;
    std::error_code ec;
    return fs::exists(to_path(file_for(slot)), ec);
}

bool remove(std::string_view slot) {
    if (!check_slot(slot)) return false;
    std::error_code ec;
    const bool removed = fs::remove(to_path(file_for(slot)), ec);
    if (removed) persist();
    return removed;
}

std::string directory() { return resolve(); }

} // namespace rmp::save
