// ===========================================================================
// assets:: — the public surface, declared in
// include/raylib_multiplatform/assets.h.
//
// Every loader here has the same shape: ask the pack, and fall back to the
// loose file if the pack does not have it. The fallback is what keeps a
// development build working while you drop new files into resources/ without
// repacking.
// ===========================================================================

#include <raylib.h>
#include <raylib_multiplatform/assets.h>

#include "internal.h"

#include <cstdio>

namespace assets {

// For the CI boot gate. failedLoads is the number of assets:: requests that
// found nothing in the pack and nothing on disk either.
namespace detail {
int requestedLoads = 0;
int failedLoads = 0;
} // namespace detail

namespace {

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
    if (!FileExists(out)) detail::failedLoads++;
}

} // namespace

void Init() {
    if (detail::OpenPack()) detail::InstallLoaderHook();
}

void Shutdown() {
    detail::RemoveLoaderHook();
    detail::ClosePack();
}

bool UsingPack() { return detail::PackIsOpen(); }

int RequestedLoads() { return detail::requestedLoads; }
int FailedLoads() { return detail::failedLoads; }

Image LoadImage(const char *name) {
    detail::requestedLoads++;
    if (detail::PackIsOpen()) {
        Image img = detail::PackReadImage(name);
        if (img.data != nullptr) return img;
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

Sound LoadSound(const char *name) {
    detail::requestedLoads++;
    if (detail::PackIsOpen()) {
        // The extension names the decoder, exactly as rres itself does for a
        // raw chunk (rres-raylib.h reads it back out of props and calls
        // LoadWaveFromMemory). It comes from the name here because that is
        // where our packer got it from in the first place.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = detail::PackRead(name, &size);
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
    detail::requestedLoads++;
    if (detail::PackIsOpen()) {
        // The extension has to come from the name: rres stores the file verbatim
        // and LoadFontFromMemory needs to know what it is. An extensionless name
        // would reach TextToLower(NULL) inside raylib, so it never gets there.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = detail::PackRead(name, &size);
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
    detail::requestedLoads++;
    if (detail::PackIsOpen()) {
        unsigned char *data = detail::PackRead(name, size);
        if (data != nullptr) return data;
        TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    FallbackPath(name, path, sizeof(path));
    return LoadFileData(path, size);
}

} // namespace assets
