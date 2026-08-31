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
    // Before the pack and before CloseWindow(): releasing a texture after the
    // GL context is gone is a write to memory that no longer exists.
    rmp::detail::release_all();
    detail::remove_loader_hook();
    detail::close_pack();
}

bool using_pack() { return detail::pack_is_open(); }

int requested_loads() { return detail::g_requested_count; }
int failed_loads() { return detail::g_failed_count; }

// The raw loaders. Everything below returns a plain raylib struct with no
// ownership attached; the counted, cached versions that the header declares are
// further down and are the only ones anybody calls.
namespace {

::Image load_image_raw(const char *name) {
    detail::g_requested_count++;
    if (detail::pack_is_open()) {
        ::Image img = detail::pack_read_image(name);
        if (img.data != nullptr) return img;
    }

    char path[2048];
    fallback_path(name, path, sizeof(path));
    return ::LoadImage(path);
}

Texture2D load_texture_raw(const char *name) {
    ::Image img = load_image_raw(name); // counts the request for us
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

::Sound load_sound_raw(const char *name) {
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
                    ::Sound snd = LoadSoundFromWave(wave);
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

::Font load_font_raw(const char *name, int font_size) {
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
                ::Font font = LoadFontFromMemory(ext, data, size, font_size, nullptr, 0);
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

} // namespace

// The counted loaders. Each one is the same three steps: ask the table for a
// resource already loaded under this name, load it if there is not one, and
// hand the table the result so the NEXT caller gets this one.
//
// A failed load is deliberately NOT cached. Caching it would mean that fixing
// the missing file and loading again still gave you the hole, until the game
// restarted.

rmp::Image load_image(const char *name) {
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::IMAGE, name, 0))
        return rmp::Image{ hit };
    ::Image raw = load_image_raw(name);
    if (raw.data == nullptr) return rmp::Image{};
    auto *slot =
        rmp::detail::adopt_named(ResourceKind::IMAGE, name, 0, &raw, sizeof(raw));
    if (slot == nullptr) {
        UnloadImage(raw);
        return rmp::Image{};
    }
    return rmp::Image{ slot };
}

rmp::Texture load_texture(const char *name) {
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::TEXTURE, name, 0))
        return rmp::Texture{ hit };
    Texture2D raw = load_texture_raw(name);
    if (raw.id == 0) return rmp::Texture{};
    auto *slot =
        rmp::detail::adopt_named(ResourceKind::TEXTURE, name, 0, &raw, sizeof(raw));
    if (slot == nullptr) {
        UnloadTexture(raw);
        return rmp::Texture{};
    }
    return rmp::Texture{ slot };
}

rmp::Sound load_sound(const char *name) {
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::SOUND, name, 0))
        return rmp::Sound{ hit };
    ::Sound raw = load_sound_raw(name);
    if (raw.stream.buffer == nullptr) return rmp::Sound{};
    auto *slot =
        rmp::detail::adopt_named(ResourceKind::SOUND, name, 0, &raw, sizeof(raw));
    if (slot == nullptr) {
        UnloadSound(raw);
        return rmp::Sound{};
    }
    return rmp::Sound{ slot };
}

rmp::Font load_font(const char *name, int font_size) {
    using rmp::detail::ResourceKind;
    // font_size is part of the key: the same face at 16 and at 32 is two
    // baked atlases, so it has to be two resources.
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::FONT, name, font_size))
        return rmp::Font{ hit };
    ::Font raw = load_font_raw(name, font_size);
    if (raw.texture.id == 0) return rmp::Font{};
    auto *slot =
        rmp::detail::adopt_named(ResourceKind::FONT, name, font_size, &raw, sizeof(raw));
    if (slot == nullptr) {
        UnloadFont(raw);
        return rmp::Font{};
    }
    return rmp::Font{ slot };
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
