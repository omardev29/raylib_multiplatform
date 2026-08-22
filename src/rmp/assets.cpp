// ===========================================================================
// rmp::assets:: — the public surface, declared in
// include/rmp/assets.h.
//
// Every loader here has the same shape: ask the pack, and fall back to the
// loose file if the pack does not have it. The fallback is what keeps a
// development build working while you drop new files into resources/ without
// repacking.
// ===========================================================================

#include <raylib.h>
#include <rmp/assets.h>

#include "internal.h"

#include <cstdio>

namespace rmp::assets {

// For the CI boot gate. failed_count is the number of rmp::assets:: requests that
// found nothing in the pack and nothing on disk either.
namespace detail {
int g_requested_count = 0;
int g_failed_count = 0;
} // namespace detail

namespace {

// Build the loose-file path for `name`, and notice when there is nothing
// behind it. This is the one place that can tell "you asked for a resource and
// there is nothing to give you" apart from "you never asked for anything" —
// which is what lets the CI boot gate stop demanding that every game ship an
// image. See tests/smoke_test.h.
//
// Only rmp::assets:: calls are counted, deliberately. The loader hook sees raylib's
// internal probing too — an .obj looking for a .mtl that legitimately is not
// there — and counting those would turn a working build red.
void fallback_path(const char *name, char *out, size_t n) {
    std::snprintf(out, n, "%s%s", RESOURCES_PATH, name);
    if (!FileExists(out)) detail::g_failed_count++;
}

} // namespace

void init() {
    if (detail::open_pack()) detail::install_loader_hook();
}

void shutdown() {
    detail::remove_loader_hook();
    detail::close_pack();
}

bool using_pack() { return detail::pack_is_open(); }

int requested_loads() { return detail::g_requested_count; }
int failed_loads() { return detail::g_failed_count; }

Image load_image(const char *name) {
    detail::g_requested_count++;
    if (detail::pack_is_open()) {
        Image img = detail::pack_read_image(name);
        if (img.data != nullptr) return img;
    }

    char path[2048];
    fallback_path(name, path, sizeof(path));
    return ::LoadImage(path);
}

Texture2D load_texture(const char *name) {
    Image img = load_image(name); // counts the request for us
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

Sound load_sound(const char *name) {
    detail::g_requested_count++;
    if (detail::pack_is_open()) {
        // The extension names the decoder, exactly as rres itself does for a
        // raw chunk (rres-raylib.h reads it back out of props and calls
        // LoadWaveFromMemory). It comes from the name here because that is
        // where our packer got it from in the first place.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = detail::pack_read(name, &size);
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
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    fallback_path(name, path, sizeof(path));
    return ::LoadSound(path);
}

Font load_font(const char *name, int font_size) {
    detail::g_requested_count++;
    if (detail::pack_is_open()) {
        // The extension has to come from the name: rres stores the file verbatim
        // and LoadFontFromMemory needs to know what it is. An extensionless name
        // would reach TextToLower(NULL) inside raylib, so it never gets there.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = detail::pack_read(name, &size);
            if (data != nullptr) {
                Font font = LoadFontFromMemory(ext, data, size, font_size, nullptr, 0);
                UnloadFileData(data);
                if (font.glyphCount > 0) return font;
            }
        }
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    fallback_path(name, path, sizeof(path));
    return ::LoadFontEx(path, font_size, nullptr, 0);
}

unsigned char *load_data(const char *name, int *size) {
    detail::g_requested_count++;
    if (detail::pack_is_open()) {
        unsigned char *data = detail::pack_read(name, size);
        if (data != nullptr) return data;
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    char path[2048];
    fallback_path(name, path, sizeof(path));
    return LoadFileData(path, size);
}

} // namespace rmp::assets
