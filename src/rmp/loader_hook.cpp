// ===========================================================================
// Teaching raylib's own loaders to read the pack.
//
// Without this, LoadTexture(RESOURCES_PATH "player.png") works in development
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

#include <cstdlib> // RL_MALLOC/RL_FREE expand to malloc/free; raylib.h does not include this
#include <cstring>

namespace rmp::assets::detail {

namespace {

bool g_hooked = false;

// "./resources/x" and "resources/x" are the same path. raylib builds the
// second form out of GetDirectoryPath() when an .obj goes looking for its .mtl.
const char *skip_dot_slash(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    return p;
}

unsigned char *hooked_load_file_data(const char *file_name, int *data_size) {
    if (file_name != nullptr && in_resources_dir(file_name)) {
        unsigned char *data = pack_read(GetFileName(file_name), data_size);
        if (data != nullptr) return data;
    }

    SetLoadFileDataCallback(nullptr);
    unsigned char *data = ::LoadFileData(file_name, data_size);
    SetLoadFileDataCallback(hooked_load_file_data);
    return data;
}

char *hooked_load_file_text(const char *file_name) {
    if (file_name != nullptr && in_resources_dir(file_name)) {
        int size = 0;
        unsigned char *data = pack_read(GetFileName(file_name), &size);
        if (data != nullptr) {
            // LoadFileText's contract is a NUL-terminated string. The pack
            // stores the file byte for byte, without one.
            char *text = static_cast<char *>(RL_MALLOC(static_cast<size_t>(size) + 1));
            if (text != nullptr) {
                std::memcpy(text, data, static_cast<size_t>(size));
                text[size] = '\0';
            }
            RL_FREE(data);
            if (text != nullptr) return text;
        }
    }

    SetLoadFileTextCallback(nullptr);
    char *text = ::LoadFileText(file_name);
    SetLoadFileTextCallback(hooked_load_file_text);
    return text;
}

} // namespace

// True when `path` points inside RESOURCES_PATH.
//
// The pack is keyed by bare file name, so matching on the name alone would let
// a save file called level1.json anywhere on disk be answered with the packed
// level1.json instead. Requiring the directory to match keeps the hook to the
// files it is responsible for; anything else falls through to the real reader.
//
// '\' and '/' compare equal. CMake writes RESOURCES_PATH with forward slashes
// on every platform, but nothing stops a Windows user writing
// RESOURCES_PATH "art\\x.png" — and a miss here sends them to the loose file,
// which is exactly what a packaged release does not ship.
bool in_resources_dir(const char *path) {
    const char *root = skip_dot_slash(RESOURCES_PATH);
    const char *p = skip_dot_slash(path);
    // Android sets RESOURCES_PATH to "" — every asset is at the root of the
    // APK's assets/. No pack is ever open there, so this branch is moot, but
    // an empty prefix matching everything is the right reading of it anyway.
    for (size_t i = 0; root[i] != '\0'; i++) {
        char a = p[i];
        char b = root[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return false;
    }
    return true;
}

void install_loader_hook() {
    if (g_hooked) return;
    SetLoadFileDataCallback(hooked_load_file_data);
    SetLoadFileTextCallback(hooked_load_file_text);
    g_hooked = true;
    TraceLog(LOG_INFO, "ASSETS: raylib's own loaders will read the pack too");
}

void remove_loader_hook() {
    if (!g_hooked) return;
    SetLoadFileDataCallback(nullptr);
    SetLoadFileTextCallback(nullptr);
    g_hooked = false;
}

} // namespace rmp::assets::detail
