// ===========================================================================
// Teaching raylib's own loaders to read the pack.
//
// Without this, LoadTexture(RMP_RESOURCES_PATH "player.png") works in development
// and comes back 0x0 in a release, because a release ships resources.rres and
// not the loose files. That is the worst shape a bug can have. With it, every
// raylib call that reads a file — LoadTexture, LoadModel, LoadShader,
// LoadFont, and the .mtl and textures an .obj pulls in behind your back —
// finds the packed copy.
//
// Two details make this safe rather than clever:
//
//   * raylib returns the callback's result verbatim. rcore.c reads
//     `if (loadFileData) return loadFileData(fileName, dataSize);` — there is
//     no fallback to the filesystem behind it. So a miss has to delegate by
//     hand, and the exact way to delegate is to unhook, call raylib, and hook
//     back. That runs raylib's own reader, including the Android one, where
//     fopen is redirected into the APK's asset manager and stdio of our own
//     would find nothing.
//   * The hook is only installed when a pack is actually open. A development
//     build behaves precisely as it did before.
// ===========================================================================

#include <raylib.h>

#include "internal.h"
#include "pack_internal.h"

#include <algorithm>
#include <cstdlib> // RL_MALLOC/RL_FREE expand to malloc/free; raylib.h does not include this
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rmp::assets::detail {

namespace {

struct {
    bool installed = false;
} hook;

// The name inside the pack that `path` means, if the hook answers for it:
// rres::packed_name() against the resources folder.
std::optional<std::string> packed_name(std::string_view path) {
    return rres::packed_name(rmp::assets::detail::resources_root(), path);
}

unsigned char *hooked_load_file_data(const char *file_name, int *data_size) {
    if (file_name != nullptr) {
        if (const std::optional<std::string> name = packed_name(file_name)) {
            int size = 0;
            unsigned char *data = pack_read(name->c_str(), &size);
            // raylib reads an empty file as no data at all, and so does this.
            if (data != nullptr && size == 0) {
                UnloadFileData(data);
                data = nullptr;
            }
            if (data != nullptr) {
                if (data_size != nullptr) *data_size = size;
                return data;
            }
        }
    }

    SetLoadFileDataCallback(nullptr);
    unsigned char *data = ::LoadFileData(file_name, data_size);
    SetLoadFileDataCallback(hooked_load_file_data);
    return data;
}

char *hooked_load_file_text(const char *file_name) {
    if (file_name != nullptr) {
        if (const std::optional<std::string> name = packed_name(file_name)) {
            int size = 0;
            unsigned char *data = pack_read(name->c_str(), &size);
            if (data != nullptr && size == 0) { // raylib's empty file: no text at all
                UnloadFileData(data);
                data = nullptr;
            }
            if (data != nullptr) {
                // LoadFileText's contract is a NUL-terminated string. The pack
                // stores the file byte for byte, without one.
                // raylib's contract, not ours: LoadFileText's caller frees this
                // with UnloadFileText, i.e. RL_FREE, so it has to come from
                // RL_MALLOC. The one place in the framework that owns by hand.
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
                char *text =
                    static_cast<char *>(RL_MALLOC(static_cast<size_t>(size) + 1));
                if (text != nullptr) {
                    std::memcpy(text, data, static_cast<size_t>(size));
                    text[size] = '\0';
                }
                RL_FREE( // NOLINT(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
                    data); // NOLINT(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
                if (text != nullptr) return text;
            }
        }
    }

    SetLoadFileTextCallback(nullptr);
    char *text = ::LoadFileText(file_name);
    SetLoadFileTextCallback(hooked_load_file_text);
    return text;
}

} // namespace

// True when `path` is a file directly inside RMP_RESOURCES_PATH.
//
// The pack is keyed by bare file name, so matching on the name alone would let
// a save file called level1.json anywhere on disk be answered with the packed
// level1.json instead. Requiring the directory to match keeps the hook to the
// files it is responsible for; anything else falls through to the real reader.
// And only the folder itself: it used to claim every path that STARTED with
// it, so "resources/sub/x.png" -- shipped beside the pack, which has no
// subfolders -- was answered with the packed "x.png", a different file, and
// "resources/../saves/level1.json" with the packed level1.json.
//
// '\' and '/' compare equal on Windows. CMake writes RMP_RESOURCES_PATH with
// forward slashes on every platform, but nothing stops a Windows user writing
// RMP_RESOURCES_PATH "art\\x.png" — and a miss here sends them to the loose file,
// which is exactly what a packaged release does not ship. Elsewhere a backslash
// is a letter of a file's name, as it is to the system.
//
// Android sets RMP_RESOURCES_PATH to "" -- every asset is at the root of the
// APK's assets/. An empty prefix used to match EVERY path, including an
// absolute one into the app's internal storage, and the pack IS packaged into
// assets/ there: a save file named like a packed resource would have been
// answered with the shipped copy on every load. With no folder to compare,
// "inside resources/" is "a bare file name", which is the same rule.
bool in_resources_dir(const char *path) {
    return path != nullptr && packed_name(path).has_value();
}

void install_loader_hook() {
    if (hook.installed) return;
    SetLoadFileDataCallback(hooked_load_file_data);
    SetLoadFileTextCallback(hooked_load_file_text);
    hook.installed = true;
    TraceLog(LOG_INFO, "ASSETS: raylib's own loaders will read the pack too");
}

void remove_loader_hook() {
    if (!hook.installed) return;
    SetLoadFileDataCallback(nullptr);
    SetLoadFileTextCallback(nullptr);
    hook.installed = false;
}

} // namespace rmp::assets::detail
