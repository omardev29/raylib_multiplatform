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
// rmp::assets::init() picks whichever exists. You never call it: the entry
// point in <rmp/app.h> calls it before your ready hook, and
// rmp::assets::shutdown() after your stop hook.
//
// Since init() also teaches raylib itself to read the pack, plain raylib calls
// work too — LoadTexture(RMP_RESOURCES_PATH "player.png"), LoadModel, LoadShader.
// The rmp::assets:: functions are the shorter spelling, not a requirement.
// See TECHNICAL.md, "Resources", for the two things that stay outside this:
// LoadMusicStream, and files loaded from outside resources/.
//
// Implementation: src/rmp/.
//
// Everything this framework adds lives under rmp::. What comes from raylib keeps
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

// The RAII half, written once for every handle type. Copy shares, move steals,
// and the last handle to a resource going away is what unloads it.
template <class T, ResourceKind K> class Resource {
public:
    // An empty handle: valid() is false until a loader's result is assigned to
    // it. The Slot constructor is how the loaders in rmp::assets make a full
    // one, from a reference the resource table has already counted.
    Resource() = default;
    explicit Resource(Slot *slot) : _slot(slot) {}

    // A copy is one more reference to the same resource; a move takes this
    // reference over and leaves `other` empty.
    Resource(const Resource &other) : _slot(other.slot()) { retain(_slot); }
    Resource(Resource &&other) noexcept : _slot(other.trade(nullptr)) {}

    // Copy-and-swap: one operator for copy AND move assignment, and
    // self-assignment cannot go wrong because the argument is already a copy.
    Resource &operator=(Resource other) noexcept {
        _slot = other.trade(_slot);
        return *this;
    }
    // Drops this reference. When it was the last one, the resource is unloaded.
    ~Resource() { release(_slot); }

    // True when this handle holds a resource. False for one that failed to
    // load, was never assigned, or was moved from; `if (texture)` asks the same.
    bool valid() const { return _slot != nullptr; }
    explicit operator bool() const { return valid(); }

    // An empty resource yields a zeroed raylib struct rather than a crash.
    // raylib draws nothing for one of those, which is the behaviour a missing
    // asset should have: a hole in the picture, not a dead process.
    const T &raw() const {
        static const T EMPTY{};
        const void *p = payload(_slot);
        return p ? *static_cast<const T *>(p) : EMPTY;
    }
    // Only an LVALUE converts. `DrawTexture(rabbit, ...)` compiles;
    // `Texture2D t = load_texture("x.png");` does NOT, and that is the point
    // -- it converted, the temporary handle died on the same line, and `t`
    // was a texture that had already been unloaded. The README and four
    // examples had it, and so did the UI's own font. The deleted `&&`
    // overload is what refuses the temporary: a `const &`-qualified function
    // on its own would still accept one, because a const reference binds to
    // an rvalue.
    operator const T &() const & { return raw(); }
    operator const T &() const && = delete;

private:
    // Another resource's slot is reached through these and never by name: an
    // underscore does not touch a dot. trade() hands this resource's slot over
    // and keeps `given` in its place -- the swap and the move are both one.
    Slot *slot() const { return _slot; }
    Slot *trade(Slot *given) noexcept {
        Slot *had = _slot;
        _slot = given;
        return had;
    }

    Slot *_slot = nullptr;
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

// The size of a SheetTag's name, terminator included. A tag whose name is
// longer keeps its first MAX_TAG_NAME - 1 characters.
constexpr int MAX_TAG_NAME = 32;

// One frame of a sheet: where it is in the packed texture, and how long it shows.
struct SheetFrame {
    Rectangle source{}; // where this frame is in the packed texture
    float seconds = 0; // from the file, so it plays at the speed you drew it
};

// One tag of the .aseprite: a named run of frames, and the direction Aseprite
// plays it in. Its name is what `sprite.play("walk")` looks for.
struct SheetTag {
    char name[MAX_TAG_NAME] = {}; // the tag's name in Aseprite, NUL-terminated
    int from = 0; // the first frame, an index into SheetData::frames
    int to = 0; // inclusive, the way Aseprite counts
    bool ping_pong = false; // Aseprite's "Ping-pong" direction: forwards, then back
    bool reverse = false; // Aseprite's "Reverse" direction: last frame first
};

// Everything read out of an .aseprite: the packed texture, the frames and the
// tags. The tables are vectors and the sheet owns them: a resource slot holds
// the whole SheetData behind a shared_ptr, so the language frees them with it
// and there is no size limit on frames or tags. The texture is the one thing
// here raylib owns, and the table unloads it before the sheet goes.
struct SheetData {
    // Every frame side by side in one row. Empty (id 0) when there was no GPU
    // to upload to; the frames and tags are there all the same.
    Texture2D texture{};
    int width = 0; // one frame's width, not the packed texture's
    int height = 0; // one frame's height, which is the packed texture's too
    std::vector<SheetFrame> frames; // in the file's order
    std::vector<SheetTag> tags; // in the file's order

    // How many frames and tags the file had.
    [[nodiscard]] int frame_count() const { return static_cast<int>(frames.size()); }
    [[nodiscard]] int tag_count() const { return static_cast<int>(tags.size()); }
    // The frame at `index`, or an empty one (no source, 0 seconds) out of range.
    [[nodiscard]] const SheetFrame &frame(int index) const {
        static const SheetFrame EMPTY{};
        if (index < 0 || index >= frame_count()) return EMPTY;
        return frames[static_cast<std::size_t>(index)];
    }
    // The tag at `index`, or an empty one (no name, frames 0 to 0) out of range.
    [[nodiscard]] const SheetTag &tag(int index) const {
        static const SheetTag EMPTY{};
        if (index < 0 || index >= tag_count()) return EMPTY;
        return tags[static_cast<std::size_t>(index)];
    }
};

// A loaded sprite sheet, from rmp::assets::load_sheet(). It shares and releases
// like the handles below, and raw() is its SheetData.
using SpriteSheet = detail::Resource<SheetData, detail::ResourceKind::SHEET>;

// The resource handles: each one is a detail::Resource around the raylib struct
// it is named after (Texture2D and RenderTexture2D for the two textures), and
// converts to it wherever raylib takes one. rmp::Texture, rmp::Image, rmp::Font
// and rmp::Sound come from the loaders in rmp::assets below. Nothing in
// rmp::assets returns an rmp::Music, rmp::Shader or rmp::RenderTexture; music
// is played by name through rmp::audio instead.
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
// file loading through it. Called for you by the entry point; calling it
// twice is harmless.
void init();

// Release the pack and unhook raylib's loaders. Called for you after your stop
// hook.
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

// The sound device opens on the first call, the same lazy way rmp::audio
// opens it; on a machine with no device this comes back empty.
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

// A level: an LDtk project (.ldtk) or a Tiled map (.json), told apart by the
// extension. See rmp/tilemap.h for what each editor's conventions mean here,
// and which of the two is maintained. The tileset images -- and an LDtk
// project's separate level files -- are loaded by FILE NAME through the same
// asset layer, so they come out of resources.rres in a release and out of
// resources/ while developing without the map knowing which.
//
// `level` picks an LDtk level by its identifier; without it, the first level.
// Tiled maps have one level and ignore it.
//
// Returns an empty map and says why if it cannot be read.
void load_map(std::string_view name, rmp::Tilemap *into);
// The same, by value: `map = rmp::assets::load_map("world.ldtk");`
rmp::Tilemap load_map(std::string_view name);
// An LDtk level by name: `map = rmp::assets::load_map("world.ldtk", "Level_2");`
rmp::Tilemap load_map(std::string_view name, std::string_view level);

// The bytes of a file -- a level, a shader's source, JSON, anything whose
// meaning only the caller knows -- as a vector that frees itself. Not cached:
// every call reads the file again. Empty when the name is in neither the pack
// nor resources/.
std::vector<unsigned char> load_data(std::string_view name); // empty when it is not there

// How many rmp::assets:: loads were asked for, and how many found nothing in the
// pack and nothing on disk either. The entry point reports these to the CI
// boot gate; you are unlikely to need them yourself.
int requested_loads();
int failed_loads();

} // namespace rmp::assets
