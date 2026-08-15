// ===========================================================================
// src/raylib_multiplatform.cpp — the template's implementation, all of it.
//
// Two things live here:
//
//   1. rres. This translation unit is the one that defines
//      RRES_RAYLIB_IMPLEMENTATION, so it compiles the container format, AES,
//      Argon2i and QOI into the binary exactly once.
//   2. assets:: — the layer that reads resources/ whether it arrived as loose
//      files or as a packed resources.rres, and that teaches raylib's own
//      loaders to do the same.
//
// This file belongs to the template. src/main.cpp is yours.
// ===========================================================================

#include <raylib.h> // must precede rres-raylib.h; also provides ComputeMD5()

// rres pulls tiny-AES, monocypher and qoi in as C sources. They compile clean
// as C++ (both clang and gcc, checked), and rres.h/rres-raylib.h wrap their
// declarations in extern "C", so the symbols are the same either way — which
// is what lets this be one file instead of a .cpp plus a .c.
#define RRES_IMPLEMENTATION
#define RRES_RAYLIB_IMPLEMENTATION
// Encryption support. AES uses Argon2i from monocypher, which rres-raylib.h
// only includes when XCHACHA20 is enabled, so enable both to get a buildable
// AES path (also lets us read XChaCha20-packed files).
#define RRES_SUPPORT_ENCRYPTION_AES
#define RRES_SUPPORT_ENCRYPTION_XCHACHA20
#include "rres-raylib.h"

// Brings in raylib, generated/app_config.h and the assets:: declarations. It
// also brings in tests/smoke_test.h, whose counters are file-scope statics —
// this TU gets its own set and never touches them; main.cpp's are the live ones.
#include "raylib_multiplatform.h"

#include <cstdio>
#include <cstdlib> // RL_MALLOC/RL_FREE expand to malloc/free; raylib.h does not include this
#include <cstring>

#ifndef RRES_PACK_FILE
#define RRES_PACK_FILE "resources.rres"
#endif

// Password used to decrypt encrypted packs. Obfuscation, not real security:
// it ends up in the binary.
//
// It used to be hard-coded here AND in CMakeLists.txt. Only the desktop build
// passes -DRRES_PASSWORD, so Android and iOS silently used this copy — change
// one and those two platforms could no longer read their own asset pack, with
// no error that pointed at the cause. Both now come from the config.
#ifndef RRES_PASSWORD
#define RRES_PASSWORD APP_RRES_PASSWORD
#endif

namespace {

bool g_usingPack = false;
bool g_hooked = false;
rresCentralDir g_cdir = {0, nullptr};
char g_packPath[2048] = {0};

int g_requested = 0;
int g_failed = 0;

// Build the loose-file path for `name`, and notice when there is nothing
// behind it. This is the one place that can tell "you asked for a resource and
// there is nothing to give you" apart from "you never asked for anything" —
// which is what lets the CI boot gate stop demanding that every game ship an
// image. See tests/smoke_test.h.
//
// Only assets:: calls are counted, deliberately. The loader hook sees raylib's
// internal probing too — an .obj looking for a .mtl that legitimately is not
// there — and counting those would turn a working build red.
void FallbackPath(const char *name, char *out, size_t n) {
    std::snprintf(out, n, "%s%s", RESOURCES_PATH, name);
    if (!FileExists(out)) g_failed++;
}

// Pull one entry out of the pack by its name in the central directory.
// Returns bytes owned by the caller (malloc'd, so RL_FREE/UnloadFileData frees
// them) or nullptr if the name is not packed or does not decrypt.
unsigned char *PackRead(const char *name, int *size) {
    if (size != nullptr) *size = 0;
    if (!g_usingPack || name == nullptr) return nullptr;

    unsigned int id = rresGetResourceId(g_cdir, name);
    if (id == 0) return nullptr;

    rresResourceChunk chunk = rresLoadResourceChunk(g_packPath, id);
    if (UnpackResourceChunk(&chunk) != 0) {
        rresUnloadResourceChunk(chunk);
        return nullptr;
    }

    unsigned int dataSize = 0;
    // LoadDataFromResource, not ...FromResourceChunk: the latter is static
    // inside rres-raylib.h. The public one also transparently follows a LINK
    // chunk to an external file.
    void *data = LoadDataFromResource(chunk, &dataSize);
    rresUnloadResourceChunk(chunk);
    if (data == nullptr) return nullptr;

    if (size != nullptr) *size = static_cast<int>(dataSize);
    return static_cast<unsigned char *>(data);
}

// ---------------------------------------------------------------------------
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
// ---------------------------------------------------------------------------

// "./resources/x" and "resources/x" are the same path. raylib builds the
// second form out of GetDirectoryPath() when an .obj goes looking for its .mtl.
const char *SkipDotSlash(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    return p;
}

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
bool InResourcesDir(const char *path) {
    const char *root = SkipDotSlash(RESOURCES_PATH);
    const char *p = SkipDotSlash(path);
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

unsigned char *HookedLoadFileData(const char *fileName, int *dataSize) {
    if (fileName != nullptr && InResourcesDir(fileName)) {
        unsigned char *data = PackRead(GetFileName(fileName), dataSize);
        if (data != nullptr) return data;
    }

    SetLoadFileDataCallback(nullptr);
    unsigned char *data = ::LoadFileData(fileName, dataSize);
    SetLoadFileDataCallback(HookedLoadFileData);
    return data;
}

char *HookedLoadFileText(const char *fileName) {
    if (fileName != nullptr && InResourcesDir(fileName)) {
        int size = 0;
        unsigned char *data = PackRead(GetFileName(fileName), &size);
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
    char *text = ::LoadFileText(fileName);
    SetLoadFileTextCallback(HookedLoadFileText);
    return text;
}

} // namespace

namespace assets {

void Init() {
    if (g_usingPack) return; // idempotent: the lifecycle macro already called it

    std::snprintf(g_packPath, sizeof(g_packPath), "%s%s", RESOURCES_PATH, RRES_PACK_FILE);

    if (FileExists(g_packPath)) {
        rresSetCipherPassword(RRES_PASSWORD);
        g_cdir = rresLoadCentralDirectory(g_packPath);
        if (g_cdir.count > 0) {
            g_usingPack = true;
            TraceLog(LOG_INFO, "ASSETS: Using resource pack %s (%d entries)", g_packPath, g_cdir.count);
        } else {
            g_usingPack = false;
            TraceLog(LOG_WARNING, "ASSETS: %s has no central directory, using loose files", g_packPath);
        }
    } else {
        g_usingPack = false;
        TraceLog(LOG_INFO, "ASSETS: No resource pack found, using loose files from %s", RESOURCES_PATH);
    }

    if (g_usingPack && !g_hooked) {
        SetLoadFileDataCallback(HookedLoadFileData);
        SetLoadFileTextCallback(HookedLoadFileText);
        g_hooked = true;
        TraceLog(LOG_INFO, "ASSETS: raylib's own loaders will read the pack too");
    }
}

void Shutdown() {
    if (g_hooked) {
        SetLoadFileDataCallback(nullptr);
        SetLoadFileTextCallback(nullptr);
        g_hooked = false;
    }
    if (g_usingPack) {
        rresUnloadCentralDirectory(g_cdir);
        g_cdir.count = 0;
        g_cdir.entries = nullptr;
    }
    g_usingPack = false;
}

bool UsingPack() { return g_usingPack; }

// For the CI boot gate. FailedLoads() is the number of assets:: requests that
// found nothing in the pack and nothing on disk either.
int RequestedLoads() { return g_requested; }
int FailedLoads() { return g_failed; }

Image LoadImage(const char *name) {
    g_requested++;
    if (g_usingPack) {
        unsigned int id = rresGetResourceId(g_cdir, name);
        if (id != 0) {
            rresResourceMulti multi = rresLoadResourceMulti(g_packPath, id);
            if (multi.count > 0) {
                bool ok = true;
                for (int i = 0; i < multi.count; i++) {
                    int r = UnpackResourceChunk(&multi.chunks[i]);
                    if (r != 0) {
                        ok = false;
                        TraceLog(LOG_WARNING, "ASSETS: Failed to unpack '%s' (code %d, wrong password?)", name, r);
                    }
                }
                Image img = {0};
                if (ok) img = LoadImageFromResource(multi.chunks[0]);
                rresUnloadResourceMulti(multi);
                if (img.data != nullptr) return img;
            } else {
                rresUnloadResourceMulti(multi);
            }
            TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
        } else {
            TraceLog(LOG_WARNING, "ASSETS: '%s' not found in pack, falling back to loose file", name);
        }
    }

    char path[2048];
    FallbackPath(name, path, sizeof(path));
    return ::LoadImage(path);
}

Texture2D LoadTexture(const char *name) {
    Image img = LoadImage(name);   // counts the request for us
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

// Everything below follows the same shape as LoadImage: try the pack, fall back
// to a loose file. The fallback is what keeps a dev build working while you
// iterate without repacking.

Sound LoadSound(const char *name) {
    g_requested++;
    if (g_usingPack) {
        // The extension names the decoder, exactly as rres itself does for a
        // raw chunk (rres-raylib.h reads it back out of props and calls
        // LoadWaveFromMemory). It comes from the name here because that is
        // where our packer got it from in the first place.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = PackRead(name, &size);
            if (data != nullptr) {
                Wave wave = LoadWaveFromMemory(ext, data, size);
                UnloadFileData(data);
                if (wave.data != nullptr) {
                    Sound snd = LoadSoundFromWave(wave);
                    UnloadWave(wave);
                    return snd;
                }
            }
        }
        TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    FallbackPath(name, path, sizeof(path));
    return ::LoadSound(path);
}

Font LoadFont(const char *name, int fontSize) {
    g_requested++;
    if (g_usingPack) {
        // The extension has to come from the name: rres stores the file verbatim
        // and LoadFontFromMemory needs to know what it is. An extensionless name
        // would reach TextToLower(NULL) inside raylib, so it never gets there.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = PackRead(name, &size);
            if (data != nullptr) {
                Font font = LoadFontFromMemory(ext, data, size, fontSize, nullptr, 0);
                UnloadFileData(data);
                if (font.glyphCount > 0) return font;
            }
        }
        TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    FallbackPath(name, path, sizeof(path));
    return ::LoadFontEx(path, fontSize, nullptr, 0);
}

unsigned char *LoadData(const char *name, int *size) {
    g_requested++;
    if (g_usingPack) {
        unsigned char *data = PackRead(name, size);
        if (data != nullptr) return data;
        TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    FallbackPath(name, path, sizeof(path));
    return LoadFileData(path, size);
}

} // namespace assets
