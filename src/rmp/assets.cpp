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
#include <rlgl.h> // rlGetShaderIdDefault(), rlFramebufferComplete(): what raylib does not say

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <rmp/assets.h>
#include <rmp/tilemap.h>

#include "animation_internal.h"
#include "audio_internal.h"
#include "tilemap_internal.h"
#include "internal.h"

namespace rmp::assets {

// For the CI boot gate. `failed` is the number of rmp::assets:: requests that
// gave nothing usable: nothing in the pack and nothing on disk, or a file that
// is there and does not decode.
namespace detail {
LoadCounts loads;

namespace {
// The default is what this library was compiled with, RMP_RESOURCES_PATH,
// read from the macro until something is set: a std::string made at static
// initialisation could throw where nothing can catch it. See internal.h for
// why it can be replaced at run time.
struct {
    std::optional<std::string> root;
} resources;
} // namespace

const char *resources_root() {
    return resources.root ? resources.root->c_str() : RMP_RESOURCES_PATH;
}

void set_resources_root(const char *root) {
    if (root == nullptr) root = "";
    resources.root = root;
}

// The backdrop's render target and shader (rmp/assets.h): what
// src/rmp/ui/backdrop.cpp draws overlay()'s blur with. Each is empty, and
// touches no GPU, when there is nothing to make -- which is everything a test
// with no window can ask for. With something to make there has to be a
// window: raylib's GL calls have nowhere to go before InitWindow(), and the
// backdrop asks only when there is one.
rmp::RenderTexture make_render_texture(const int width, const int height) {
    if (width <= 0 || height <= 0) return rmp::RenderTexture{};
    // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp makes one)
    const RenderTexture2D raw = LoadRenderTexture(width, height);
    // A framebuffer the driver would not complete is as good as none, and
    // raylib only says so in the log: its ids are there all the same, which
    // is all IsRenderTextureValid() looks at. Past the GPU's largest texture
    // is one way to get it.
    if (!IsRenderTextureValid(raw) || !rlFramebufferComplete(raw.id)) {
        UnloadRenderTexture(raw);
        return rmp::RenderTexture{};
    }
    rmp::RenderTexture made{ rmp::detail::adopt(rmp::detail::ResourceKind::RENDER_TEXTURE,
                                                raw) };
    if (!made.valid()) UnloadRenderTexture(raw);
    return made;
}
// RMP_COVERAGE_ON

rmp::Shader make_shader(const std::string_view vertex, const std::string_view fragment) {
    if (fragment.empty()) return rmp::Shader{};
    // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp makes one)
    // raylib reads C strings; an empty vertex source is its own default one.
    const std::string vs(vertex);
    const std::string fs(fragment);
    const ::Shader raw =
        LoadShaderFromMemory(vs.empty() ? nullptr : vs.c_str(), fs.c_str());
    // A shader that does not compile or link comes back as 0, or as raylib's
    // default program standing in for it -- which would draw, and draw the
    // wrong thing. Neither is what was asked for: empty, as a file that does
    // not decode is. raylib has logged why.
    if (raw.id == 0 || raw.id == rlGetShaderIdDefault()) {
        UnloadShader(raw); // frees nothing of raylib's own, and the locations of a 0
        return rmp::Shader{};
    }
    rmp::Shader made{ rmp::detail::adopt(rmp::detail::ResourceKind::SHADER, raw) };
    if (!made.valid()) UnloadShader(raw);
    return made;
}
// RMP_COVERAGE_ON
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
//
// Returns whether the file is there. A loader that then cannot DECODE it counts
// that too, with decode_failed(): a corrupt PNG left a blank texture and a
// green boot, because only a name with nothing behind it was counted.
//
// A folder is not a file: load_data("") asked for resources/ itself, which
// exists, so every loader called it "there" and then "could not be decoded"
// -- a report for a name that names nothing (tests/property/assets_prop.cpp).
bool fallback_path(const char *name, std::string &out) {
    out = std::string(detail::resources_root()) + name;
    if (FileExists(out.c_str()) && !DirectoryExists(out.c_str())) return true;
    ++detail::loads.failed;
    return false;
}

// A file that was there and gave nothing usable: as failed as a missing one.
// Said once per name: a failed load is not cached, so a game that asks for it
// every frame would otherwise say it every frame.
void decode_failed(const char *name) {
    ++detail::loads.failed;
    RMP_REPORT_ONCE_KEYED(name, "ASSETS: '%s' is in %s but could not be decoded", name,
                          detail::resources_root());
}

// A name with a NUL character in it names no file. Everything that opens one
// -- raylib, the pack, the C library -- reads a name as a C string and would
// open what comes before the NUL: load_sheet("hero.aseprite\0x") loaded
// hero.aseprite, and cached it under the name cut short. The game's mistake:
// nothing, one failed load, and said once per name, whichever loader it was
// handed to. Every public loader asks this first.
bool refused(std::string_view name) {
    if (name.find('\0') == std::string_view::npos) return false;
    ++detail::loads.requested;
    ++detail::loads.failed;
    const std::string shown(name); // what a C string of it says: up to the NUL
    RMP_REPORT_ONCE_KEYED(name,
                          "ASSETS: \"%s\" and a NUL character after it is no file "
                          "name; nothing was loaded",
                          shown.c_str());
    return true;
}

// raylib answers a font it cannot read with GetFontDefault() instead of an
// empty font. That is not the font that was asked for, and adopted under the
// file's name it would be cached as if it were -- so it is told apart here, by
// the glyphs it points at.
bool font_decoded(const ::Font &font) {
    return font.glyphs != nullptr && font.glyphCount > 0 &&
        font.glyphs != GetFontDefault().glyphs;
}

} // namespace

// Declared in internal.h. Here because this is where the loose path is built
// -- but NOT through fallback_path(), which counts a miss as a failed load.
// rmp::audio asks this about coin.wav, coin.ogg, coin.mp3 and coin.qoa in
// turn, and a game whose coin is an .ogg asked for one asset, not for three
// that failed: counted, the boot gate read assets_failed=3 for a correct game,
// and only on a machine where the device opens. A name that resolves to
// nothing at all is reported by rmp::audio itself.
bool detail::resource_exists(const char *name) {
    if (name == nullptr || name[0] == '\0') return false;
    if (detail::pack_has(name)) return true;
    const std::string path = std::string(detail::resources_root()) + name;
    return FileExists(path.c_str()) && !DirectoryExists(path.c_str());
}

void init() {
    if (detail::open_pack()) detail::install_loader_hook();
}

void shutdown() {
    // The resources were already released in rmp::app::detail::begin_stop(),
    // which runs BEFORE the window closes. Calling it again here is harmless —
    // the table is empty — but it must not be the only call, because by now
    // CloseWindow() has taken the GL context with it.
    rmp::detail::release_all();
    detail::remove_loader_hook();
    detail::close_pack();
}

bool using_pack() { return detail::pack_is_open(); }

int requested_loads() { return detail::loads.requested; }
int failed_loads() { return detail::loads.failed; }

// The raw loaders. Everything below returns a plain raylib struct with no
// ownership attached; the counted, cached versions that the header declares are
// further down and are the only ones anybody calls.
namespace {

::Image load_image_raw(const char *name) {
    ++detail::loads.requested;
    if (detail::pack_is_open()) {
        ::Image img = detail::pack_read_image(name);
        if (img.data != nullptr) return img;
    }

    std::string path;
    if (!fallback_path(name, path)) return ::Image{};
    ::Image img = ::LoadImage(path.c_str());
    if (img.data == nullptr) decode_failed(name);
    return img;
}

Texture2D load_texture_raw(const char *name) {
    // Counts the request, and a file that is missing or does not decode.
    const ::Image img = load_image_raw(name);
    if (img.data == nullptr) return Texture2D{};
    // A picture that decoded and then did not reach the GPU is not counted:
    // with no window there is no GPU, which is every headless test, and
    // whether there is a window is the scene's question to ask, not this
    // file's (tools/seam_check.sh).
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

::Sound load_sound_raw(const char *name) {
    ++detail::loads.requested;
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
                const Wave wave = LoadWaveFromMemory(ext, data, size);
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

    std::string path;
    if (!fallback_path(name, path)) return ::Sound{};
    ::Sound sound = ::LoadSound(path.c_str());
    if (sound.stream.buffer == nullptr || sound.frameCount == 0) {
        UnloadSound(
            sound); // a no-op on an empty one, and not empty when it has no frames
        decode_failed(name);
        return ::Sound{};
    }
    return sound;
}

::Font load_font_raw(const char *name, int font_size, std::span<const int> codepoints) {
    // The glyphs to bake. raylib's own default, with no list, is the 95 of ASCII
    // 32..126 (rtext.c, "default to 95"): a .ttf drew every n with a tilde, every
    // accented vowel and every inverted question mark as '?'. The list is ours,
    // always, so that never comes back.
    if (codepoints.empty()) codepoints = detail::default_codepoints();
    const int *list = codepoints.data();
    const int count = static_cast<int>(codepoints.size());
    ++detail::loads.requested;
    if (detail::pack_is_open()) {
        // The extension has to come from the name: rres stores the file verbatim
        // and LoadFontFromMemory needs to know what it is. An extensionless name
        // would reach TextToLower(NULL) inside raylib, so it never gets there.
        const char *ext = GetFileExtension(name);
        if (ext != nullptr) {
            int size = 0;
            unsigned char *data = detail::pack_read(name, &size);
            if (data != nullptr) {
                ::Font font = LoadFontFromMemory(ext, data, size, font_size, list, count);
                UnloadFileData(data);
                if (font_decoded(font)) return font;
                // UnloadFont() leaves the default font alone, so this is safe
                // whichever of the two raylib handed back.
                UnloadFont(font);
            }
        }
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    std::string path;
    const bool there = fallback_path(name, path);
    if (!there) return ::Font{};
    ::Font font = ::LoadFontEx(path.c_str(), font_size, list, count);
    if (!font_decoded(font)) {
        UnloadFont(font);
        decode_failed(name);
        return ::Font{};
    }
    return font;
}

} // namespace

// The largest font size load_font() bakes: a 1024-pixel face is already an
// atlas of 16384 by 16384.
constexpr int MAX_FONT_SIZE = 1024;

// The counted loaders. Each one is the same three steps: ask the table for a
// resource already loaded under this name, load it if there is not one, and
// hand the table the result so the NEXT caller gets this one.
//
// A failed load is deliberately NOT cached. Caching it would mean that fixing
// the missing file and loading again still gave you the hole, until the game
// restarted.

rmp::Image load_image(std::string_view name_view) {
    if (refused(name_view)) return rmp::Image{};
    const std::string key(name_view);
    const char *name = key.c_str();
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::IMAGE, name, 0))
        return rmp::Image{ hit };
    const ::Image raw = load_image_raw(name);
    if (raw.data == nullptr) return rmp::Image{};
    auto *slot = rmp::detail::adopt_named(ResourceKind::IMAGE, name, 0, raw);
    if (slot == nullptr) {
        UnloadImage(raw);
        return rmp::Image{};
    }
    return rmp::Image{ slot };
}

rmp::Texture load_texture(std::string_view name_view) {
    if (refused(name_view)) return rmp::Texture{};
    const std::string key(name_view);
    const char *name = key.c_str();
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::TEXTURE, name, 0))
        return rmp::Texture{ hit };
    const Texture2D raw = load_texture_raw(name);
    if (raw.id == 0) return rmp::Texture{};
    auto *slot = rmp::detail::adopt_named(ResourceKind::TEXTURE, name, 0, raw);
    if (slot == nullptr) {
        UnloadTexture(raw);
        return rmp::Texture{};
    }
    return rmp::Texture{ slot };
}

rmp::Sound load_sound(std::string_view name_view) {
    // A sound needs the device open, and this used to need the game to have
    // called InitAudioDevice() first -- one more line in every project, and a
    // silent empty sound when it was missing. The device opens here now, the
    // same lazy way rmp::audio opens it, and rmp::app closes it on the way out
    // in the right order. On a machine with no device this is empty and says
    // so once. A name that is no file name is refused first, device or not.
    if (refused(name_view)) return rmp::Sound{};
    if (!rmp::audio::detail::ensure_device()) return rmp::Sound{};
    const std::string key(name_view);
    const char *name = key.c_str();
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::SOUND, name, 0))
        return rmp::Sound{ hit };
    const ::Sound raw = load_sound_raw(name);
    if (raw.stream.buffer == nullptr) return rmp::Sound{};
    auto *slot = rmp::detail::adopt_named(ResourceKind::SOUND, name, 0, raw);
    if (slot == nullptr) {
        UnloadSound(raw);
        return rmp::Sound{};
    }
    return rmp::Sound{ slot };
}

namespace {
// What a font that decoded and has no GPU behind it still holds, let go: see
// load_font(). The default's are never touched.
void drop_unuploaded(const ::Font &raw) {
    if (font_decoded(raw)) {
        UnloadFontData(raw.glyphs, raw.glyphCount);
        MemFree(raw.recs);
    }
}

// The size check every font load makes. A size that is not one never reaches
// the font: stb_truetype asserts on a negative one, and the atlas of a huge
// one is past what an int can count.
bool font_size_refused(const char *name, int font_size) {
    if (font_size >= 1 && font_size <= MAX_FONT_SIZE) return false;
    ++detail::loads.requested;
    ++detail::loads.failed;
    RMP_REPORT_ONCE_KEYED(name, "ASSETS: load_font(\"%s\", %d): a font size is 1 to %d",
                          name, font_size, MAX_FONT_SIZE);
    return true;
}
} // namespace

std::span<const int> detail::default_codepoints() {
    // Printable ASCII, Latin-1's letters and signs (n with a tilde, the accents,
    // the inverted marks, the pound and the degree), Latin Extended-A (Polish,
    // Czech, Hungarian, Turkish, the Baltic and Croatian letters), and the
    // typography a translated line uses: dashes, curly quotes, the bullet, the
    // ellipsis, the euro, the trademark.
    static const std::vector<int> LIST = [] {
        std::vector<int> v;
        for (int c = 32; c <= 126; ++c) v.push_back(c);
        for (int c = 160; c <= 383; ++c) v.push_back(c);
        for (const int c : { 0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D,
                             0x201E, 0x2022, 0x2026, 0x20AC, 0x2122 })
            v.push_back(c);
        return v;
    }();
    return LIST;
}

rmp::Font detail::load_font_glyphs(std::string_view name_view, int font_size,
                                   std::span<const int> codepoints) {
    if (refused(name_view)) return rmp::Font{};
    const std::string key(name_view);
    const char *name = key.c_str();
    if (font_size_refused(name, font_size)) return rmp::Font{};
    const ::Font raw = load_font_raw(name, font_size, codepoints);
    if (raw.texture.id == 0) {
        drop_unuploaded(raw);
        return rmp::Font{};
    }
    // Not under its name: the same file at the same size with another set of
    // glyphs is another atlas, and load_font()'s cache must not hand it out.
    auto *slot = rmp::detail::adopt(rmp::detail::ResourceKind::FONT, raw);
    if (slot == nullptr) {
        UnloadFont(raw);
        return rmp::Font{};
    }
    return rmp::Font{ slot };
}

rmp::Font load_font(std::string_view name_view, int font_size) {
    if (refused(name_view)) return rmp::Font{};
    const std::string key(name_view);
    const char *name = key.c_str();
    using rmp::detail::ResourceKind;
    // A size that is not one never reaches the font. stb_truetype asserts on
    // a negative one -- load_font("ui.ttf", -1) aborted the game, and read out
    // of bounds where asserts are off -- and the atlas of a huge one is past
    // what an int can count. The game's mistake: an empty font, counted as a
    // failed load, and said once.
    if (font_size_refused(name, font_size)) return rmp::Font{};
    // font_size is part of the key: the same face at 16 and at 32 is two
    // baked atlases, so it has to be two resources.
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::FONT, name, font_size))
        return rmp::Font{ hit };
    const ::Font raw = load_font_raw(name, font_size, {});
    if (raw.texture.id == 0) {
        // Decoded, and no GPU to go to -- before the window, or headless.
        // raylib's UnloadFont() tells its default font apart by the texture
        // id, which with no window is 0 for both, so it frees nothing: every
        // such load kept its glyph tables (97 allocations for 95 glyphs,
        // LeakSanitizer said). They are let go here, the default's never.
        drop_unuploaded(raw);
        return rmp::Font{};
    }
    auto *slot = rmp::detail::adopt_named(ResourceKind::FONT, name, font_size, raw);
    if (slot == nullptr) {
        UnloadFont(raw);
        return rmp::Font{};
    }
    return rmp::Font{ slot };
}

rmp::SpriteSheet load_sheet(std::string_view name_view) {
    if (refused(name_view)) return rmp::SpriteSheet{};
    const std::string key(name_view);
    const char *name = key.c_str();
    using rmp::detail::ResourceKind;
    if (auto *hit = rmp::detail::acquire_named(ResourceKind::SHEET, name, 0))
        return rmp::SpriteSheet{ hit };

    // Through load_data, so a sheet comes out of the rres pack exactly like
    // every other asset -- the animation layer never learns where files live.
    const std::vector<unsigned char> bytes = load_data(name_view);
    if (bytes.empty()) return rmp::SpriteSheet{};
    const int size = static_cast<int>(bytes.size());

    SheetData sheet{};
    const bool parsed = rmp::animation::detail::parse_sheet(bytes.data(), size, &sheet);
    if (!parsed) {
        TraceLog(LOG_WARNING, "SHEET: [%s] is not an .aseprite file this can read", name);
        ++detail::loads.failed;
        return rmp::SpriteSheet{};
    }

    // Every frame into ONE image, side by side, then one texture. A hundred
    // enemies from the same sheet is then one bind rather than a hundred.
    sheet.texture = rmp::animation::detail::upload_sheet(bytes.data(), size, sheet);

    auto *slot = rmp::detail::adopt_named(ResourceKind::SHEET, name, 0, sheet);
    if (slot == nullptr) {
        if (sheet.texture.id != 0) UnloadTexture(sheet.texture);
        return rmp::SpriteSheet{};
    }
    return rmp::SpriteSheet{ slot };
}

rmp::Tilemap load_map(std::string_view name) { return load_map(name, ""); }

rmp::Tilemap load_map(std::string_view name, std::string_view level_view) {
    rmp::Tilemap map;
    if (refused(name)) return map;
    const std::string key(name);
    const std::string level(level_view);
    const std::vector<unsigned char> bytes = load_data(name);
    if (bytes.empty()) return map;
    auto parsed = rmp::tilemap::detail::parse_map(
        bytes.data(), static_cast<int>(bytes.size()), key.c_str(), level);
    if (parsed == nullptr) ++detail::loads.failed;
    // Owned by the map from here. A map is not a cached resource the way a
    // texture is: one scene owns one map, the factories on it are that
    // scene's, and sharing it would share those too.
    rmp::tilemap::detail::Access::adopt(map, std::move(parsed));
    return map;
}

std::vector<unsigned char> load_data(std::string_view name_view) {
    if (refused(name_view)) return {};
    const std::string key(name_view);
    const char *name = key.c_str();
    ++detail::loads.requested;
    // Copied out of raylib's buffer and into a vector that frees itself: the
    // one place the framework touches raylib's C loader contract on the way
    // in, so nobody downstream has to remember UnloadFileData.
    const auto owned = [](unsigned char *data, int size) {
        std::vector<unsigned char> out;
        if (data != nullptr && size > 0) out.assign(data, data + size);
        if (data != nullptr) UnloadFileData(data);
        return out;
    };
    if (detail::pack_is_open()) {
        int size = 0;
        unsigned char *data = detail::pack_read(name, &size);
        if (data != nullptr) return owned(data, size);
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }

    // Bytes have no format to fail to decode: the file is there or it is not.
    std::string path;
    if (!fallback_path(name, path)) return {};
    int size = 0;
    unsigned char *data = LoadFileData(path.c_str(), &size);
    return owned(data, size);
}

} // namespace rmp::assets
