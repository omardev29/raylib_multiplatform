#pragma once
// ---------------------------------------------------------------------------
// rmp::assets:: — loading from resources/
//
// Put your files in resources/ and load them by name. Which of the two ways
// they arrive is a build detail you do not have to think about:
//
//   - a packed resources.rres next to the executable (what a release ships),
//     optionally AES-encrypted;
//   - the loose files in resources/ (what you get while developing).
//
// rmp::assets::init() picks whichever exists. You never call it: the lifecycle
// macro in <rmp/app.h> does, before on_ready(), and
// Shutdown() after on_exit().
//
// Since Init() also teaches raylib itself to read the pack, plain raylib calls
// work too — LoadTexture(RESOURCES_PATH "player.png"), LoadModel, LoadShader.
// The rmp::assets:: functions are the shorter spelling, not a requirement.
// See TECHNICAL.md, "Resources", for the two things that stay outside this:
// LoadMusicStream, and files loaded from outside resources/.
//
// Implementation: src/rmp/.
//
// Everything this template adds lives under rmp::. What comes from raylib keeps
// its own name, so you can always tell at a glance which is which.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/config.h>

#include <memory> // std::shared_ptr: what owns a resource slot's payload
#include <string_view> // names on the way in
#include <vector> // the sheet tables, and load_data()'s bytes

namespace rmp {
class Tilemap;
} // namespace rmp

namespace rmp {

// ---------------------------------------------------------------------------
// The resource types.
//
//     rmp::Texture rabbit = rmp::assets::load_texture("rabbit.png");
//     DrawTexture(rabbit, 100, 100, WHITE);
//
// That is the whole API. There is no Unload* to remember, no pairing to get
// wrong, and no order to respect — the last copy to go out of scope releases
// it, and rmp::assets::shutdown() releases whatever is still held when the game
// ends, before the window closes and takes the GL context with it.
//
// THEY SHARE. Loading the same name twice gives you the same GPU texture with
// the count at two. That is not an optimisation bolted on afterwards; it is
// what makes a hundred enemies with one sprite work without anyone having to
// decide who owns it. The name IS the cache key.
//
// THEY CONVERT. Every one of these turns into the raylib type it wraps, so
// every raylib function that takes a Texture2D takes an rmp::Texture. Nothing
// here is a wall you have to climb over to reach raylib.
//
// AND THEY DO NOT THROW. An asset that fails to load gives you an empty
// resource: valid() is false, and drawing it draws nothing, the same as raylib
// does with a zeroed struct. That is deliberate — see rmp/app.h for why this
// framework has no exceptions anywhere.
// ---------------------------------------------------------------------------

namespace detail {

enum class ResourceKind {
    TEXTURE,
    IMAGE,
    FONT,
    SOUND,
    MUSIC,
    SHADER,
    RENDER_TEXTURE,
    SHEET
};

// One slot per loaded resource. The count is on the slot, not on the handle,
// which is what lets two handles to the same name share one GPU object.
struct Slot;

Slot *acquire_named(ResourceKind kind, const char *name, int font_size);

// The slot OWNS the payload: a std::shared_ptr<void> because it carries T's
// destructor with it, so a sheet's tables are freed by the language and the
// table never learns what a T is. The raylib Unload* for the kind is called
// first, by the table, which is the one place in the framework it happens.
Slot *adopt_owned(ResourceKind kind, std::shared_ptr<void> payload);
Slot *adopt_named_owned(ResourceKind kind, const char *name, int font_size,
                        std::shared_ptr<void> payload);
template <class T> Slot *adopt(ResourceKind kind, T payload) {
    return adopt_owned(kind, std::make_shared<T>(std::move(payload)));
}
template <class T>
Slot *adopt_named(ResourceKind kind, const char *name, int font_size, T payload) {
    return adopt_named_owned(kind, name, font_size,
                             std::make_shared<T>(std::move(payload)));
}
void release_all();
void retain(Slot *slot);
void release(Slot *slot);
const void *payload(const Slot *slot);

// For tests, and for a debug overlay. How many distinct resources are loaded,
// and how many references exist to a given name.
int live_count();
int ref_count(const char *name);

// The RAII half, once, for all seven types. Copy shares, move steals, and the
// destructor is the only place an Unload* is ever called.
template <class T, ResourceKind K> class Resource {
public:
    Resource() = default;
    explicit Resource(Slot *slot) : slot_(slot) {}

    Resource(const Resource &other) : slot_(other.slot_) { retain(slot_); }
    Resource(Resource &&other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }

    // Copy-and-swap: one operator for copy AND move assignment, and
    // self-assignment cannot go wrong because the argument is already a copy.
    Resource &operator=(Resource other) noexcept {
        Slot *tmp = slot_;
        slot_ = other.slot_;
        other.slot_ = tmp;
        return *this;
    }
    ~Resource() { release(slot_); }

    bool valid() const { return slot_ != nullptr; }
    explicit operator bool() const { return valid(); }

    // An empty resource yields a zeroed raylib struct rather than a crash.
    // raylib draws nothing for one of those, which is the behaviour a missing
    // asset should have: a hole in the picture, not a dead process.
    const T &raw() const {
        static const T kEmpty{};
        const void *p = payload(slot_);
        return p ? *static_cast<const T *>(p) : kEmpty;
    }
    // Ref-qualified `&`: only an LVALUE converts. `DrawTexture(rabbit, ...)`
    // compiles; `Texture2D t = load_texture("x.png");` does NOT, and that is
    // the point -- it converted, the temporary handle died on the same line,
    // and `t` was a texture that had already been unloaded. The README and
    // four examples had it, and so did the UI's own font.
    operator const T &() const & { return raw(); }

private:
    Slot *slot_ = nullptr;
};

} // namespace detail

// ---------------------------------------------------------------------------
// A sprite sheet, read straight out of an Aseprite file.
//
// THE ANIMATION NAMES ARE YOURS. We do not know what your animations are
// called, so we do not invent `walk` or `idle` -- they are read from the TAGS
// in your .aseprite, and `sprite.play("walk")` names one of yours.
//
// Reading the binary rather than an exported PNG + JSON is what removes a whole
// step from the workflow: you save in Aseprite and the game has the new
// animation. And the duration comes from the file per frame, so it plays at the
// speed you drew it at.
//
// Vectors, and a char buffer for the tag name: <vector> is paid by this header
// (measured, see tools/header_cost.py), and the name stays a fixed buffer
// because it is compared per frame and never grows.
// ---------------------------------------------------------------------------

constexpr int kMaxTagName = 32;

struct SheetFrame {
    Rectangle source{}; // where this frame is in the packed texture
    float seconds = 0; // from the file, so it plays at the speed you drew it
};

struct SheetTag {
    char name[kMaxTagName] = {};
    int from = 0;
    int to = 0; // inclusive, the way Aseprite counts
    bool ping_pong = false;
    bool reverse = false;
};

// The tables are vectors and the sheet owns them: a resource slot holds the
// whole SheetData behind a shared_ptr, so the language frees them with it and
// there is no size limit on frames or tags. The texture is the one thing here
// raylib owns, and the table unloads it before the sheet goes.
struct SheetData {
    Texture2D texture{};
    int width = 0; // one frame's width, not the packed texture's
    int height = 0;
    std::vector<SheetFrame> frames;
    std::vector<SheetTag> tags;

    [[nodiscard]] int frame_count() const { return static_cast<int>(frames.size()); }
    [[nodiscard]] int tag_count() const { return static_cast<int>(tags.size()); }
    [[nodiscard]] const SheetFrame &frame(int index) const {
        static const SheetFrame kEmpty{};
        if (index < 0 || index >= frame_count()) return kEmpty;
        return frames[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] const SheetTag &tag(int index) const {
        static const SheetTag kEmpty{};
        if (index < 0 || index >= tag_count()) return kEmpty;
        return tags[static_cast<std::size_t>(index)];
    }
};

using SpriteSheet = detail::Resource<SheetData, detail::ResourceKind::SHEET>;

using Texture = detail::Resource<Texture2D, detail::ResourceKind::TEXTURE>;
using Image = detail::Resource<::Image, detail::ResourceKind::IMAGE>;
using Font = detail::Resource<::Font, detail::ResourceKind::FONT>;
using Sound = detail::Resource<::Sound, detail::ResourceKind::SOUND>;
using Music = detail::Resource<::Music, detail::ResourceKind::MUSIC>;
using Shader = detail::Resource<::Shader, detail::ResourceKind::SHADER>;
using RenderTexture =
    detail::Resource<RenderTexture2D, detail::ResourceKind::RENDER_TEXTURE>;

} // namespace rmp

namespace rmp::assets {

// Detect and open the resource pack, if there is one, and route raylib's own
// file loading through it. Called for you by the lifecycle macro; calling it
// twice is harmless.
void init();

// Release the pack and unhook raylib's loaders. Called for you after on_exit().
void shutdown();

// True when assets are being served from a .rres pack.
bool using_pack();

// Load by resource name (e.g. "rabbit.png"). Nothing to unload: the returned
// value releases itself when the last copy goes, and asking twice for the same
// name gives you the SAME resource with the reference count at two.
//
// A name that is in neither the pack nor resources/ gives an empty resource
// rather than a crash. valid() tells them apart, and drawing an empty one draws
// nothing — a hole in the picture, not a dead process.
rmp::Image load_image(std::string_view name);
rmp::Texture load_texture(std::string_view name);

// InitAudioDevice() must have been called first.
rmp::Sound load_sound(std::string_view name);

// font_size is the baked glyph size, and it is part of the cache key: the same
// font at 16 and at 32 is two resources, because it is two textures.
rmp::Font load_font(std::string_view name, int font_size);

// An .aseprite or .ase from resources/. Every frame is packed into one texture
// in a single row, so drawing a hundred enemies from the same sheet is one
// texture bind. Tags, frame ranges and per-frame durations come with it.
//
// On a machine with no GPU the texture is left empty and the metadata is still
// there, which is what lets tests/animation_test.cpp exist.
rmp::SpriteSheet load_sheet(std::string_view name);

// A Tiled map, as JSON with the tile layers saved as CSV -- which is what a new
// map in Tiled does by default. The tileset images are loaded by file name
// through load_texture above, so they come out of resources.rres in a release
// and out of resources/ while developing without the map knowing which.
//
// Returns an empty map and says why if it cannot be read, and for the one
// failure people actually hit -- layers saved compressed or as base64 -- the
// message names the setting in the editor rather than the byte it choked on.
void load_map(std::string_view name, rmp::Tilemap *into);
// The same, by value: `map = rmp::assets::load_map("level1.json");`
rmp::Tilemap load_map(std::string_view name);

// Raw bytes for anything else — a level file, a shader, JSON. `size` receives
// the byte count. This one is NOT counted or cached: free it with
// The bytes of a file, as a vector that frees itself. Empty when the name is
// in neither the pack nor resources/.
// whose meaning only the caller knows.
std::vector<unsigned char> load_data(std::string_view name); // empty when it is not there

// How many rmp::assets:: loads were asked for, and how many found nothing in the
// pack and nothing on disk either. The entry point reports these to the CI
// boot gate; you are unlikely to need them yourself.
int requested_loads();
int failed_loads();

} // namespace rmp::assets
