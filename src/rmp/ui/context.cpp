// ===========================================================================
// The UI context: starting Clay, the scale, the font, the frame arena and
// element identity.
//
// Nothing here is public. What the user sees is begin()/end()/button()/text(),
// and the reason they need no configuration is that everything they would have
// had to configure is decided in this file.
// ===========================================================================

#include "internal.h"
#include "../internal.h"
#include "../scene_internal.h" // running(): whose frame this is, RMP_GAME's or the runner's

#include <rmp/app.h> // stretch(), for the insets
#include <rmp/assets.h>
#include <rmp/config.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

// Defaults, so this still compiles against a generated header from before the
// [ui] section existed. tools/configure.py normally provides all four.
#ifndef RMP_UI_FONT
#define RMP_UI_FONT ""
#endif
#ifndef RMP_UI_FONT_SIZE
#define RMP_UI_FONT_SIZE 20
#endif
#ifndef RMP_UI_SCALE
#define RMP_UI_SCALE 0.0f
#endif
#ifndef RMP_UI_MAX_ELEMENTS
#define RMP_UI_MAX_ELEMENTS 512
#endif

namespace rmp::ui {

// current_theme() / set_theme() live in theme.cpp.

namespace detail {

namespace {

// The file's state, one struct per concern. The dot at every use says "this is
// file state", and a group cannot collide with a function of the same name
// (started(), scale(), focus()), a C library name, or a parameter.

struct {
    bool started = false;
    bool frame_open = false;
    // begin()s said while the frame was open, each waiting for the end() that
    // pairs with it: see note_nested_begin().
    int nested = 0;
    std::vector<unsigned char> arena; // Clay's memory, ours to own
} context;

// Which part of the frame the scene stack is running (set_phase()). FREE is
// everything outside the stack, which is all there is without scenes.
struct {
    Phase current = Phase::FREE;
} phases;

// A frame the runner marked for RMP_ENTRY_POINT, so that it is the one to
// close it -- and nothing else.
struct {
    bool marked = false;
} runner;

struct {
    float scale = 1.0f;
    float scale_override = RMP_UI_SCALE; // 0 = automatic
} sizing;

// The font. When [ui] font is empty we use raylib's built-in one, which needs
// no asset, no licence and no loading, and is drawn at whatever size the text
// asks for.
//
// A .ttf is only sharp at the size it was baked at, so it is baked once per
// PIXEL size the interface draws: SMALL, MEDIUM, LARGE and every number, each
// times the scale. It used to be baked once, at the theme's font_size, and
// every other size was that bake stretched -- LARGE came out blurry. A face
// is kept while it is used and let go of with the UI; past FONT_FACES sizes
// the one used longest ago makes room, but never one drawn THIS frame: its
// glyphs may still be waiting in raylib's batch, and unloading the texture
// under them would draw garbage.
constexpr int FONT_FACES = 8;
struct FontFace {
    int size = 0; // the pixel size it was baked at; 0 = a free slot
    rmp::Font handle; // counted, like every other resource: emptying it unloads it
    unsigned frame = 0; // the last frame it was asked for
};
struct {
    std::array<FontFace, FONT_FACES> faces;
    unsigned frame = 0; // counted at every frame boundary, for FontFace::frame
    // A configured font that cannot be loaded is a one-time problem, not a
    // per-draw one. Without this we would go back to the filesystem and log the
    // same warning for every piece of text, every frame.
    bool failed = false;
} font;

// Everything a frame leaves behind, gone: the UI is shutting down. Defined at
// the bottom, where every piece of it has been declared.
void forget_frame();

// Every face goes: the UI is shutting down, or the font is another one.
void release_fonts() {
    for (FontFace &face : font.faces) face = FontFace{};
}

// Text arena. Clay keeps pointers into whatever we hand it and reads them at
// Clay_EndLayout, so the memory has to survive the frame. 8 KB is a lot of
// menu; if a UI ever needs more, the truncation below says so out loud.
constexpr int ARENA_SIZE = 8 * 1024;
struct {
    std::array<char, ARENA_SIZE> bytes;
    int used = 0;
} text_arena;

// Occurrence counters, so two buttons with the same label are two elements.
// 256 of them is 2 KB and it is sized against the ceiling, not against taste:
// every labelled widget costs at least one Clay element, and the cheapest of
// them costs two, so a pass that could fill this table cannot fit under [ui]
// max_elements in the first place. It used to be 128, which an inventory with
// unique item names reaches while the frame is still perfectly legal -- and the
// 129th label onwards silently shared occurrence 0 with every other one.
constexpr int MAX_LABELS = 256;
struct LabelCount {
    uint32_t hash;
    uint16_t count;
};
struct {
    std::array<LabelCount, MAX_LABELS> table;
    int count = 0;
    // Labels that did not fit. They take indices from the TOP of the pass block,
    // counting down, so two elements sharing an unrecorded label are still two
    // elements. See element_id().
    int overflow = 0;
} labels;

// The frame, and the passes inside it. A frame is one turn of the game loop; a
// pass is one begin()/end(), and there is one per scene that draws UI.
struct {
    bool marked = false;
    bool self_marked = false;
    // Marking a frame and preparing it are two things, because the app marks
    // every frame from the very first one and the UI does not exist until
    // something asks for it. Without the second flag, frame one would be
    // marked-but-unprepared, and the first begin() would open a second boundary
    // on top of the app's.
    bool prepared = false;
} frame;

// Each pass gets its own block of element indices, so that a scene's ids depend
// only on that scene. 4096 is far more widgets than a pass will ever have and
// leaves room for 2^20 passes, which is not a number anyone will reach.
constexpr uint32_t INDICES_PER_PASS = 4096;

// Last frame's geometry, per pass. Two buffers: one being filled by the frame
// being described, one being read by it. Clay cannot answer this itself —
// Clay_BeginLayout resets its element map, so after two passes only the second
// one's boxes exist, and the first scene's grid would size itself from the
// second scene's layout.
constexpr int MAX_BOUNDS = 512;
struct BoundsEntry {
    uint32_t id;
    // The innermost container this element was declared inside that owns what
    // the pointer does there: a scroll area, or an open dropdown list. 0 for
    // most elements. Hit testing needs it twice over -- a row scrolled out of a
    // list still has a box, it is just outside the list and the pointer would
    // find it there; and an element is only exempt from what is in front of it
    // when it belongs to it.
    uint32_t clip;
    Clay_BoundingBox box;
};
struct {
    std::array<std::array<BoundsEntry, MAX_BOUNDS>, 2> entries;
    std::array<int, 2> count{};
    int front = 0; // the one this frame writes; the other is last frame's
} bounds;

// Elements that are IN FRONT and take the pointer — an open dropdown list, and
// so far nothing else. Hit testing is a box test against a snapshot, which
// knows no z-order and no parentage, so the one case that matters is recorded
// explicitly: while the pointer is inside one of these, only what is inside it
// can be over. Per pass, and double-buffered with the geometry because a
// blocker declared this frame can only be known to the next one.
constexpr int MAX_BLOCKERS = 4;
struct Blocker {
    int pass;
    uint32_t id;
};
struct {
    std::array<std::array<Blocker, MAX_BLOCKERS>, 2> entries;
    std::array<int, 2> count{};
} blockers;

// Where the interface takes the pointer, as a frame drew it: every surface a
// pass input can reach painted, and every control it declared, each cut to
// the scroll areas around it. Double-buffered with the geometry. It is what
// wants_pointer() answers from until this frame's UI has run -- for where the
// pointer is NOW. Answering with last frame's own answer, which is where the
// pointer WAS, let a finger that lands on a button without hovering first
// reach the game underneath as well: ui/05_inventory's e2e script, clicking
// its list, logged a click in the world.
constexpr int MAX_CLAIMS = 512;
// Every field initialised, so all of it is constant-initialised: the e2e
// driver's set_pointer_provider() reaches forget_claims() before main(), and
// a member left to a constructor is initialised in an order nobody chose.
struct {
    std::array<std::array<Clay_BoundingBox, MAX_CLAIMS>, 2> rects{};
    std::array<int, 2> count{};
    std::array<bool, 2> full{}; // more than fit: the frame's answer stands
} claims;

void forget_claims() {
    claims.count = {};
    claims.full = {};
}

struct PassId {
    uint32_t id;
    uint32_t clip;
};
// The pass being described: which one, whether it takes input, the ids handed
// out in it -- so that capture_pass_bounds() knows what to ask Clay about when
// the pass closes -- and its next layer. Cleared per pass.
struct {
    int index = -1;
    bool input = true;
    std::array<PassId, MAX_BOUNDS> ids;
    int id_count = 0;
    int16_t layer_z = 0;
    uint32_t last_control = 0; // see note_control()
    // Behind a pass that was in front of it last frame (set_pass_modal()):
    // no input, whatever `input` says. Apart from `input`, which the app sets
    // once per scene, because a scene may describe two passes and only the
    // first is behind the second.
    bool behind = false;
} this_pass;

// The frontmost pass that said it is in front of the rest (set_pass_modal()):
// this frame's, being found, and last frame's, which is what this frame's
// passes obey. -1 for none.
struct {
    int this_frame = -1;
    int last_frame = -1;
} modality;

// What frame_time() answers in test mode. 0, unless a test lets time go by.
struct {
    float seconds = 0;
} test_clock;

// The clipping containers open right now, innermost last. Pushed by
// open_scroll() and popped by close_scroll(); reset per pass so an imbalance
// cannot leak into the next one.
constexpr int MAX_CLIP_DEPTH = 8;
struct {
    std::array<uint32_t, MAX_CLIP_DEPTH> stack;
    int depth = 0;
    int overflow = 0; // pushed past the limit, so the pops still pair up
} clips;

// Where measuring, the pointer and navigation come from: raylib, unless a test
// has swapped one out.
rmp::Font load_ui_font(const char *name, int pixel_size) {
    return rmp::assets::load_font(name, pixel_size);
}

struct {
    MeasureFn measure = measure_with_raylib;
    PointerFn pointer = pointer_from_raylib;
    NavFn nav = nav_from_raylib;
    TextFn text = typed_from_raylib;
    FontFn font = load_ui_font;
    const char *font_name = nullptr; // nullptr = [ui] font
} providers;

const char *configured_font() {
    return providers.font_name != nullptr ? providers.font_name : RMP_UI_FONT;
}

// Test viewport. 0 means "ask raylib", which is every real run.
struct {
    float width = 0.0f;
    float height = 0.0f;
} test_viewport;

// Clay reports through a handler rather than a return value, so a test that
// wants to know whether a frame produced a duplicate id or ran out of elements
// has to be told from here.
struct {
    int errors = 0;
    // The FIRST since the last reset, not the last: the ones that follow a
    // capacity failure are its consequences, and the first one is the one worth
    // asserting on.
    Clay_ErrorType first_error = CLAY_ERROR_TYPE_INTERNAL_ERROR;
    // Set by a capacity failure and cleared at the frame boundary. What Clay
    // reports after one of those is its consequence -- an unbalanced tree,
    // because its own CloseElement stops doing anything once the ceiling
    // latches -- and printing that too only sends the reader looking for a bug
    // in their layout.
    bool over_capacity = false;
} clay;

// One name per error, so RMP_REPORT_ONCE_KEYED gives each KIND of failure its
// own line instead of one line for the first one that happens.
const char *clay_error_name(Clay_ErrorType t) {
    switch (t) {
        case CLAY_ERROR_TYPE_TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED:
            return "measure";
        case CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED:
            return "arena";
        case CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED:
            return "elements";
        case CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED:
            return "text-cache";
        case CLAY_ERROR_TYPE_DUPLICATE_ID:
            return "duplicate-id";
        case CLAY_ERROR_TYPE_FLOATING_CONTAINER_PARENT_NOT_FOUND:
            return "floating";
        case CLAY_ERROR_TYPE_PERCENTAGE_OVER_1:
            return "percentage";
        case CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE:
            return "unbalanced";
        case CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED:
            return "hashmap";
        case CLAY_ERROR_TYPE_INTERNAL_ERROR:
            break;
    }
    return "internal";
}

void on_clay_error(Clay_ErrorData e) {
    ++clay.errors;
    if (clay.errors == 1) clay.first_error = e.errorType;

    // The ceiling, translated. Clay's own text says to call
    // Clay_SetMaxElementCount() with a higher value — a function the user
    // cannot reach, naming neither the number nor the file it lives in. And it
    // is worse than one bad frame: Clay's element hashmap never shrinks again
    // once it has filled, so from here the interface lays out nothing at all
    // until the game is restarted. That is worth saying in full, once.
    if (e.errorType == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED ||
        e.errorType == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED) {
        clay.over_capacity = true;
        RMP_REPORT_ONCE(
            "UI: more than max_elements (%d) elements in one frame. The interface "
            "will not lay out again until the game is restarted. Raise max_elements "
            "in the [ui] section of raylib_multiplatform.toml, or draw less at once.",
            RMP_UI_MAX_ELEMENTS);
        return;
    }

    // Everything else in Clay's own words, but once per kind rather than sixty
    // times a second.
    if (clay.over_capacity) return;
    RMP_REPORT_ONCE_KEYED(clay_error_name(e.errorType), "UI: clay: %.*s",
                          e.errorText.length, e.errorText.chars);
}

uint32_t fnv1a(std::string_view s) {
    uint32_t h = 2166136261u;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

bool started() { return context.started; }

bool ensure_started() {
    if (context.started) return true;

    // Order matters: the element count is what sizes the arena, so it has to be
    // set before asking how much memory Clay needs. Clay's own default is 8192
    // elements, which reserves megabytes for a three-button menu — noticeable
    // on a phone, and pure waste everywhere.
    Clay_SetMaxElementCount(RMP_UI_MAX_ELEMENTS);

    const uint32_t size = Clay_MinMemorySize();
    // new[] of char is aligned for anything Clay puts in it (the default new
    // alignment is 16 on every toolchain here); Clay itself only needs 8.
    context.arena.assign(size, 0);

    const Clay_Arena arena =
        Clay_CreateArenaWithCapacityAndMemory(size, context.arena.data());
    Clay_Initialize(arena, viewport(), Clay_ErrorHandler{ on_clay_error, nullptr });
    Clay_SetMeasureTextFunction(providers.measure, nullptr);

    context.started = true;
    TraceLog(LOG_INFO, "UI: ready (%u bytes, up to %d elements)", size,
             RMP_UI_MAX_ELEMENTS);
    return true;
}

void shutdown_context() {
    // First, and whether or not the UI started: the backdrop's texture is on
    // the GPU, and the window closes after this.
    release_backdrop();
    // And whether or not it started, everything a frame leaves behind goes:
    // the next begin() starts the UI again -- there is no init() -- and it has
    // to be the UI the first begin() started, not one inside a frame the app
    // marked before it shut down, with a font it gave up on and every fade
    // where it stopped.
    forget_frame();
    font.failed = false;
    forget_transitions();
    begin_pass_containers();
    forget_focus();
    if (!context.started) return;
    release_fonts();
    context.arena = std::vector<unsigned char>{};
    // Clay's current context lived in that arena. Left pointing at it, the next
    // begin() -- which starts the UI again, there being no init() -- wrote the
    // element ceiling into freed memory before anything else. With none, Clay
    // is exactly where it was before the first start.
    Clay_SetCurrentContext(nullptr);
    context.started = false;
}

bool frame_open() { return context.frame_open; }

// Opening or closing a frame, either way, starts the count of begin()s
// written inside it again: they belong to the frame they were written in.
void set_frame_open(bool open) {
    context.frame_open = open;
    context.nested = 0;
}

void note_nested_begin() { ++context.nested; }

bool close_nested_begin() {
    if (context.nested == 0) return false;
    --context.nested;
    return true;
}

namespace {

// A function's name, terminated, for a report's %s.
std::array<char, 24> name_of(std::string_view what) {
    std::array<char, 24> name{};
    std::snprintf(name.data(), name.size(), "%.*s", static_cast<int>(what.size()),
                  what.data());
    return name;
}

} // namespace

// While the stack updates, or draws the world under the camera, a frame
// cannot be opened -- so one line for the whole phase, naming the call that
// tried first, and silence for the rest of what it describes.
bool phase_refuses(std::string_view what) {
    switch (phases.current) {
        case Phase::UPDATE:
            RMP_REPORT_ONCE(
                "UI: rmp::ui::%s() in _update(): nothing is drawn while the "
                "world moves, so it did nothing. The interface is described in "
                "the scene's _draw().",
                name_of(what).data());
            return true;
        case Phase::WORLD:
            RMP_REPORT_ONCE(
                "UI: rmp::ui::%s() in an object's _draw(), which runs under "
                "the camera, so it did nothing. The interface is described in "
                "the scene's own _draw(), in screen space -- and a box that "
                "follows a point of the world is rmp::ui::at().",
                name_of(what).data());
            return true;
        case Phase::FREE:
        case Phase::DRAW:
            break;
    }
    return false;
}

bool frame_ready(std::string_view what, std::string_view label) {
    if (context.frame_open) return true;
    if (phase_refuses(what)) return false;
    // The call as it was written: button("Play"), or spacer() for one with no
    // label -- cut short, a label being any length.
    std::array<char, 96> call{};
    if (label.empty()) {
        std::snprintf(call.data(), call.size(), "%.*s()", static_cast<int>(what.size()),
                      what.data());
    } else {
        std::snprintf(call.data(), call.size(), "%.*s(\"%.*s\")",
                      static_cast<int>(what.size()), what.data(),
                      static_cast<int>(label.size()), label.data());
    }
    // Keyed by the call, so two buttons in the wrong place are two lines --
    // but by the function alone for one whose argument is what it SAYS and
    // not a name: text("120") would be a line for every score the game
    // reaches.
    const bool says = what == "text" || what == "tooltip";
    const std::array<char, 24> name = name_of(what);
    // Where the pair goes, for the game this is: a scene's _draw() with the
    // stack running, and the frame hook's drawing otherwise -- a game with no
    // scenes has no _draw() to look for.
    const std::string_view where = rmp::scenes::detail::running()
        ? "in a scene's _draw()"
        : "the two of them between BeginDrawing() and EndDrawing()";
    RMP_REPORT_ONCE_KEYED(says ? name.data() : call.data(),
                          "UI: rmp::ui::%s with no frame open, so it did nothing. The "
                          "interface is described between rmp::ui::begin() and "
                          "rmp::ui::end(), %.*s.",
                          call.data(), static_cast<int>(where.size()), where.data());
    return false;
}

// ---------------------------------------------------------------------------
// Scale
// ---------------------------------------------------------------------------

void update_scale() {
    if (sizing.scale_override > 0.0f) {
        sizing.scale = sizing.scale_override;
    } else {
        // Against the design resolution declared in [window]. min(), not max():
        // a UI that does not fit is worse than one with room to spare, so the
        // tighter axis wins and everything stays on screen.
        const Clay_Dimensions v = viewport();
        const float sx = v.width / static_cast<float>(RMP_WINDOW_WIDTH);
        const float sy = v.height / static_cast<float>(RMP_WINDOW_HEIGHT);
        float s = sx < sy ? sx : sy;
        if (s < 0.5f) s = 0.5f;
        if (s > 4.0f) s = 4.0f;
        sizing.scale = s;
    }
}

float ui_scale() { return sizing.scale; }

void set_scale_override(float s) {
    sizing.scale_override = (s > 0.0f) ? s : 0.0f;
    if (context.started) update_scale();
}

::Font ui_font(float pixel_size) {
    const bool builtin = (configured_font()[0] == '\0');
    if (builtin || font.failed) return GetFontDefault();

    int wanted = static_cast<int>(std::lround(pixel_size));
    if (wanted < 1) wanted = 1;

    // Already baked at this size? If not: a free slot, else the face used
    // longest ago -- but not one used this frame.
    FontFace *free_slot = nullptr;
    FontFace *stale = nullptr;
    const FontFace *nearest = nullptr;
    for (FontFace &face : font.faces) {
        if (face.size == wanted) {
            face.frame = font.frame;
            return face.handle.raw();
        }
        if (face.size == 0) {
            if (free_slot == nullptr) free_slot = &face;
            continue;
        }
        if (face.frame != font.frame && (stale == nullptr || face.frame < stale->frame))
            stale = &face;
        if (nearest == nullptr ||
            std::abs(face.size - wanted) < std::abs(nearest->size - wanted))
            nearest = &face;
    }
    FontFace *room = free_slot != nullptr ? free_slot : stale;
    // Every face was drawn this frame and none is this size: more sizes in one
    // frame than there are faces. The closest one, stretched, is a soft letter
    // where the alternative is a texture pulled out from under the batch. (A
    // full table with no nearest one would be a table of no faces at all.)
    if (room == nullptr)
        return nearest != nullptr ? nearest->handle.raw() : GetFontDefault();

    const rmp::Font baked = providers.font(configured_font(), wanted);
    if (baked.raw().glyphCount <= 0) {
        RMP_REPORT_ONCE("UI: [ui] font '%s' could not be loaded; using the built-in font",
                        configured_font());
        font.failed = true; // say it once, then stop asking
        release_fonts();
        return GetFontDefault(); // a missing font must not switch the UI off
    }
    // Assigning over an old face releases it; the resource table unloads it.
    room->size = wanted;
    room->handle = baked;
    room->frame = font.frame;
    return room->handle.raw();
}

// ---------------------------------------------------------------------------
// Frame arena
// ---------------------------------------------------------------------------

void reset_frame_arena() { text_arena.used = 0; }

void *frame_alloc(size_t bytes) {
    // Everything stored here is at most pointer-aligned, so rounding the
    // cursor up to 8 is enough and costs a few bytes a frame.
    const int aligned = (text_arena.used + 7) & ~7;
    if (aligned + static_cast<int>(bytes) > ARENA_SIZE) return nullptr;
    void *p = text_arena.bytes.data() + aligned;
    text_arena.used = aligned + static_cast<int>(bytes);
    return p;
}

Clay_String intern(std::string_view s) {
    int len = static_cast<int>(s.size());
    if (len > ARENA_SIZE - text_arena.used) {
        len = ARENA_SIZE - text_arena.used;
        RMP_REPORT_ONCE("UI: text arena full (%d bytes); labels are being truncated",
                        ARENA_SIZE);
    }
    if (len <= 0) return Clay_String{ false, 0, text_arena.bytes.data() };

    char *dst = text_arena.bytes.data() + text_arena.used;
    std::memcpy(dst, s.data(), static_cast<size_t>(len));
    text_arena.used += len;
    // isStaticallyAllocated stays false: this lives exactly one frame, which is
    // the contract Clay asks for.
    return Clay_String{ false, len, dst };
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

namespace {
// Every id handed out this pass, so that capture_pass_bounds() can ask Clay for
// its box when the pass closes. Silently dropping the overflow is right: the
// only consequence is that one element forgets how big it was last frame, and
// capture_pass_bounds() is where that gets said out loud, once.
void remember_id(uint32_t id) {
    if (this_pass.id_count >= MAX_BOUNDS) return;
    const uint32_t clip = clips.depth > 0 ? clips.stack[clips.depth - 1] : 0u;
    this_pass.ids[this_pass.id_count++] = PassId{ id, clip };
}
} // namespace

void reset_id_counters() {
    labels.count = 0;
    labels.overflow = 0;
    this_pass.layer_z = 0;
    this_pass.last_control = 0;
    this_pass.id_count = 0;
    clips.depth = 0;
    clips.overflow = 0;
}

bool frame_marked() { return frame.marked; }
bool ui_ran_this_frame() { return frame.marked && this_pass.index >= 0; }
bool frame_self_marked() { return frame.self_marked; }
void set_frame_self_marked(bool self) { frame.self_marked = self; }

void begin_pass() {
    ++this_pass.index;
    // Behind a pass that was in front of it last frame: an overlay's veil is
    // over it, so the pointer, the keys and the focus are the overlay's. On
    // top of what the app said (set_pass_input()), never instead of it.
    this_pass.behind = this_pass.index < modality.last_frame;
    reset_id_counters();
    begin_pass_containers();
    begin_pass_focus();
    // The first pass of the frame is where wants_pointer() and wants_keyboard()
    // start again from nothing. See begin_capture_frame().
    if (this_pass.index == 0) begin_capture_frame();
}

int current_pass() { return this_pass.index < 0 ? 0 : this_pass.index; }
bool pass_input() { return this_pass.input && !this_pass.behind; }

void set_pass_modal() {
    if (current_pass() > modality.this_frame) modality.this_frame = current_pass();
}

void note_control(const Clay_ElementId id) { this_pass.last_control = id.id; }

uint32_t take_last_control() {
    const uint32_t id = this_pass.last_control;
    this_pass.last_control = 0;
    return id;
}

// Ask Clay for the box of every id this pass used, now that the layout is
// finished, and write them where the next frame's matching pass will look.
void capture_pass_bounds() {
    int &count = bounds.count[bounds.front];
    for (int i = 0; i < this_pass.id_count; ++i) {
        if (count >= MAX_BOUNDS) {
            RMP_REPORT_ONCE("UI: more than %d elements in one frame; the extra ones lose "
                            "their remembered geometry, so a grid or slider among them "
                            "may size itself oddly",
                            MAX_BOUNDS);
            return;
        }
        Clay_ElementId key{};
        key.id = this_pass.ids[i].id;
        const Clay_ElementData d = Clay_GetElementData(key);
        if (!d.found) continue;
        bounds.entries[bounds.front][count++] =
            BoundsEntry{ this_pass.ids[i].id, this_pass.ids[i].clip, d.boundingBox };
    }
}

// --- clipping and occlusion, declared by the containers -------------------

void push_clip(Clay_ElementId id) {
    // Paired even when it overflows, the way the grid stack learned to be: a
    // push that does not happen must not be followed by a pop that does.
    if (clips.depth < MAX_CLIP_DEPTH) {
        clips.stack[clips.depth++] = id.id;
    } else {
        ++clips.overflow;
        RMP_REPORT_ONCE("UI: scroll areas nested more than %d deep; the ones past that "
                        "do not clip what can be clicked inside them",
                        MAX_CLIP_DEPTH);
    }
}

void pop_clip() {
    if (clips.overflow > 0) {
        --clips.overflow;
    } else if (clips.depth > 0) {
        --clips.depth;
    }
}

void block_pointer(Clay_ElementId id) {
    int &count = blockers.count[bounds.front];
    if (count >= MAX_BLOCKERS) return; // four open dropdowns is already absurd
    blockers.entries[bounds.front][count++] = Blocker{ current_pass(), id.id };
}

} // namespace detail

// ---------------------------------------------------------------------------
// The frame boundary — the public half, in rmp::ui::detail because rmp::app is
// the only caller. See the long comment in include/rmp/ui.h for what belongs
// here rather than in begin(), and why the answer changed the day two scenes
// could draw in one frame.
// ---------------------------------------------------------------------------

namespace detail {

namespace {

// The boundary, without the half that needs Clay.
void mark_frame() {
    frame.marked = true;
    this_pass.index = -1;
    this_pass.input = true;
    this_pass.behind = false;
}

} // namespace

// Marking is unconditional: from here to end_frame() there is one frame, and
// nobody else gets to open another. Preparing is not, because the UI may not
// exist yet — see prepare_frame().
void begin_frame() {
    mark_frame();
    prepare_frame();
}

// The half that needs Clay to be up. started(), not ensure_started(): the app
// calls begin_frame() every frame whether or not anything draws UI, and forcing
// the UI up here would quietly cost every game an arena and a font it never
// asked for. Called again from the first begin() of the frame, which is where
// the UI does come up — and does nothing the second time.
void prepare_frame() {
    if (frame.prepared || !started()) return;
    frame.prepared = true;
    clay.over_capacity = false;

    // Swap the geometry buffers: what this frame writes, the next one reads.
    bounds.front = 1 - bounds.front;
    bounds.count[bounds.front] = 0;
    blockers.count[bounds.front] = 0;
    claims.count[bounds.front] = 0;
    claims.full[bounds.front] = false;

    reset_frame_arena();
    update_scale();
    anim_begin_frame();
    ++font.frame; // what "drawn this frame" means to the font faces

    Clay_SetLayoutDimensions(viewport());

    // Sampled once. Every pass in this frame is handed the same answer, which
    // is the whole reason this is not in begin(): with two scenes, the mouse
    // must not appear to move between them.
    update_pointer();

    // Before Clay_BeginLayout — Clay is explicit that after it the offset
    // arrives a frame late. Once per frame and not once per pass, or one wheel
    // click would scroll as many notches as there are scenes on the stack.
    // Drag scrolling is on because on a phone it is the only way to scroll
    // anything; the wheel is the desktop half of the same gesture.
    const Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, Clay_Vector2{ wheel.x * 30.0f, wheel.y * 30.0f },
                                frame_time());

    // Focus spans the whole frame on purpose: arrow-key navigation has to see
    // the focusable widgets of every layer before it can decide where to go.
    begin_focus_frame();
}

// --- the focus, kept in view ------------------------------------------------
//
// A scroll area moves for the wheel and a finger and for nothing else, so the
// keyboard or the gamepad walking down a list took the focus onto rows the
// area had clipped away, and the ring went on being drawn where nobody could
// see it: ui/05_inventory's list, found by its e2e script. Now, when the focus
// lands somewhere new and shows, every scroll area it is inside moves just far
// enough to show it -- once, so the wheel or a finger can take the list
// somewhere else afterwards without being pulled back.

namespace {

struct {
    uint32_t id = 0; // the focus last brought into view
} revealed;

// THIS frame's box: at the frame boundary the front buffer is the frame that
// has just been described, all of its passes. drawn_box() is the same, outside.
std::optional<BoundsEntry> drawn_this_frame(uint32_t id) {
    const int front = bounds.front;
    for (int i = 0; i < bounds.count[front]; ++i) {
        if (bounds.entries[front][i].id == id) return bounds.entries[front][i];
    }
    return std::nullopt;
}

// How far to move something at `start`, `size` long, for it to lie inside
// `outer_start`, `outer_size`: 0 when it does, and when it cannot, its start
// against the outer one's.
float into_view(float start, float size, float outer_start, float outer_size) {
    if (start < outer_start || size > outer_size) return outer_start - start;
    const float over = (start + size) - (outer_start + outer_size);
    return over > 0.0f ? -over : 0.0f;
}

void reveal_focus() {
    const uint32_t id = focused_id();
    if (id == 0 || id == revealed.id || !focus_visible()) return;
    const std::optional<BoundsEntry> at = drawn_this_frame(id);
    if (!at) return; // not drawn this frame: when it is
    revealed.id = id;
    Clay_BoundingBox box = at->box;
    uint32_t clip = at->clip;
    // Innermost first, and each move carries the box with it into the next
    // area out: a list inside a scrolling page shows its row, then the page
    // shows the list. Both axes: one an area does not scroll along has
    // nothing to show, and Clay clamps it back to 0 at the next frame.
    for (int depth = 0; clip != 0 && depth <= MAX_CLIP_DEPTH; ++depth) {
        const std::optional<BoundsEntry> area = drawn_this_frame(clip);
        if (!area) return;
        Clay_ElementId area_id{};
        area_id.id = clip;
        const Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(area_id);
        if (scroll.found && scroll.scrollPosition != nullptr) {
            const float dx = into_view(box.x, box.width, area->box.x, area->box.width);
            const float dy = into_view(box.y, box.height, area->box.y, area->box.height);
            scroll.scrollPosition->x += dx;
            scroll.scrollPosition->y += dy;
            box.x += dx;
            box.y += dy;
        }
        clip = area->clip;
    }
}

} // namespace

std::optional<DrawnBox> drawn_box(uint32_t id) {
    const std::optional<BoundsEntry> at = drawn_this_frame(id);
    if (!at) return std::nullopt;
    return DrawnBox{ at->box, at->clip };
}

void end_frame() {
    if (!frame.marked) return;
    if (frame.prepared) {
        end_focus_frame();
        reveal_focus();
    }
    // The UI drew nothing this frame, so it wants neither the pointer nor the
    // keyboard. It is said here because otherwise there is nothing to say it:
    // the flags are cleared by the FIRST begin() of a frame, and a pause menu
    // popping while the pointer sits over one of its buttons means there is no
    // next begin() at all — and the game's mouse would stay dead.
    if (this_pass.index < 0) begin_capture_frame();
    frame.marked = false;
    frame.self_marked = false;
    frame.prepared = false;
    this_pass.input = true;
    this_pass.behind = false;
    // What was in front this frame is what the next one's passes obey.
    modality.last_frame = modality.this_frame;
    modality.this_frame = -1;
}

void set_pass_input(bool reachable) { this_pass.input = reachable; }

float frame_time() {
    return test_mode() ? test_clock.seconds : rmp::app::detail::frame_delta();
}

void set_test_frame_time(const float seconds) {
    test_clock.seconds = seconds > 0 ? seconds : 0.0f;
}

// --- the runner's frame, and where the interface is described ---------------
//
// Hooks the app and the scene stack call (../ui_internal.h).

// RMP_ENTRY_POINT's frame hook is one UI frame however many begin()/end()
// pairs it draws, so the runner marks it around the hook. It used to be
// begin() that marked it, and each pair was a frame of its own: a screen and
// the overlay over it sampled the pointer twice, and the one in front never
// saw a press begin. Marking only: the half that needs Clay still waits for
// the first begin(), after the step the game was handed, exactly as before.
// With the scene stack running this is RMP_GAME, whose frame() marks its own
// frame after the step, and this leaves it alone.
void runner_begin_frame() {
    if (rmp::scenes::detail::running() || frame.marked) return;
    mark_frame();
    runner.marked = true;
}

// After the game's EndDrawing(). A frame it left open is said once and closed
// for it -- laid out and not drawn, the screen being finished -- so that the
// next frame starts clean instead of nesting inside this one for good.
void runner_end_frame() {
    if (!runner.marked) return;
    runner.marked = false;
    if (context.frame_open) {
        RMP_REPORT_ONCE("UI: rmp::ui::begin() without rmp::ui::end() in the frame hook. "
                        "The frame was closed for it after EndDrawing(), too late to be "
                        "drawn: every begin() needs its end(), before EndDrawing().");
        close_frame(false);
    }
    end_frame();
}

void set_phase(const Phase phase) { phases.current = phase; }

// A scene's _draw() has returned, and its frame has to be closed by now: the
// next scene's begin() would otherwise go inside it, and the frame would end
// with the scene it was not. Said once for each place on the stack -- there
// is no RTTI to name the scene by -- and closed as end() closes one, drawn:
// this is still between BeginDrawing() and EndDrawing().
void after_scene_draw(const int index, const int top) {
    if (!context.frame_open) return;
    std::array<char, 16> where{};
    std::snprintf(where.data(), where.size(), "%d", index);
    RMP_REPORT_ONCE_KEYED(
        where.data(),
        "UI: rmp::ui::begin() without rmp::ui::end() in the _draw() of the "
        "scene %d of %d on the stack, counting from the bottom%s. The frame "
        "was closed for it: every begin() needs its end().",
        index + 1, top + 1, index == top ? " (the one on top)" : "");
    close_frame(true);
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

int16_t next_layer_z() { return ++this_pass.layer_z; }

// The hash, and nothing else: no occurrence bump, no interning, nothing
// remembered. Both element_id() and everything that wants to FIND an element
// go through here, so there is one place that knows how an id is built.
Clay_ElementId peek_element_id(std::string_view label, unsigned occurrence, int pass) {
    // WithIndex(…, 0) rather than Clay_GetElementId, so there is exactly one id
    // scheme in the whole layer. Clay's two hashes disagree even at offset 0 —
    // the offset is mixed in before the final avalanche — so using both would
    // mean an element created one way could never be found the other way. That
    // is precisely how the headless test failed to see a panel that was on
    // screen.
    const Clay_String s{ false, static_cast<int32_t>(label.size()), label.data() };
    return Clay_GetElementIdWithIndex(
        s, static_cast<uint32_t>(pass) * INDICES_PER_PASS + occurrence);
}

Clay_ElementId peek_element_id(std::string_view label) {
    return peek_element_id(label, 0, current_pass());
}

// The id is hashed from the label the CALLER gave; the arena copy is only what
// Clay may read back later when it draws. Those were the same string until the
// arena filled up, and then they were not: a label interned short hashed
// differently, so the element silently became a different element and its
// hover, its focus and its remembered box all detached at the same time. Near
// full it changed every frame as a list scrolled; completely full every id
// became the hash of the empty string and Clay reported duplicates.
Clay_ElementId finish_id(std::string_view label, unsigned occurrence) {
    Clay_ElementId id = peek_element_id(label, occurrence, current_pass());
    id.stringId = intern(label);
    remember_id(id.id);
    return id;
}

Clay_ElementId element_id(std::string_view label, std::string_view explicit_id) {
    // An explicit id is the escape hatch, and it says "this element, whatever
    // else is on screen" — so it never counts as an occurrence of anything.
    if (!explicit_id.empty()) return finish_id(explicit_id, 0);

    const uint32_t h = fnv1a(label);
    uint16_t occurrence = 0;
    int slot = -1;
    for (int i = 0; i < labels.count; ++i) {
        if (labels.table[i].hash == h) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        occurrence = ++labels.table[slot].count;
    } else if (labels.count < MAX_LABELS) {
        labels.table[labels.count++] = LabelCount{ h, 0 };
    } else {
        // The table is full. Leaving these at occurrence 0 is what made two
        // "Use" buttons late in a long list ONE element — hover one, both
        // light up; click either, the wrong one fires — which is exactly the
        // collision the counter exists to prevent. They take indices from the
        // top of the pass block instead, counting down: deterministic, so an
        // id is still the same id next frame, and it can only meet the
        // occurrences coming up from 0 in a pass of four thousand widgets,
        // which cannot fit under [ui] max_elements.
        RMP_REPORT_ONCE("UI: more than %d distinct labels in one pass; the ones past "
                        "that keep working but are numbered from the other end. Give "
                        "the repeated ones an explicit id if anything looks swapped.",
                        MAX_LABELS);
        if (labels.overflow < static_cast<int>(INDICES_PER_PASS) / 2) ++labels.overflow;
        return finish_id(label,
                         static_cast<unsigned>(INDICES_PER_PASS) -
                             static_cast<unsigned>(labels.overflow));
    }
    // Same label twice in one pass => different index => different element, so
    // hovering one does not light up the other. The pass offset is what keeps
    // that true ACROSS scenes: a menu's "Back" and a pause overlay's "Back" are
    // in different blocks, so one of them appearing or not cannot renumber the
    // other.
    return finish_id(label, occurrence);
}

// ---------------------------------------------------------------------------
// Providers (swapped out by the headless layout tests)
// ---------------------------------------------------------------------------

// What render.cpp's DrawTextEx() will draw, measured the way it draws it, a
// piece at a time -- which is how Clay asks. Clay measures a text word by word
// and a space on its own, once, and adds them up (Clay__MeasureTextCached);
// DrawTextEx() draws a whole line and puts letter_spacing() between every two
// of its letters, the spaces' included. raylib's MeasureTextEx() counts the
// spacing only inside what it is given, so every space lost the two around
// it: a two-word line was laid out four pixels narrower than it was drawn at
// 20 pixels, and a menu's longer lines ran past the buttons under them.
//
// So each letter brings the spacing BEFORE it, but the first letter of its
// line: a word does when there is a letter before it on its line (Clay hands
// the whole string over with the word, so that is a look back), and a space,
// which Clay measures once without saying where it is, always does. Added
// up, a line is exactly as wide as it is drawn -- unless it starts with a
// space, which brought a spacing nothing is before; then it is one spacing
// wider, never narrower. A line that Clay wraps is one spacing wider too,
// past the first, for the same reason: its first word was measured with the
// space before it. Wider is a box with a little room in it; narrower is text
// running out of its box.
//
// Letter for letter as DrawTextEx() goes: the bytes cstr() copies -- up to a
// NUL, as it reads them --, raylib's own UTF-8 decoding and glyph lookup, a
// glyph with no advance taking its rectangle's width, and raylib's font when
// this one has no texture. tests/property/ui_text_prop.cpp holds it to
// DrawTextEx() over random fonts, sizes and lines.
Clay_Dimensions measure_text(const ::Font &face, Clay_StringSlice text, float font_size) {
    const ::Font f = face.texture.id != 0 ? face : GetFontDefault();
    // A font with nothing in it -- raylib's, before there is a window -- is
    // nothing DrawTextEx() could draw either.
    if (!IsFontValid(f) || text.length <= 0) return Clay_Dimensions{ 0, 0 };
    const float scale = font_size / static_cast<float>(f.baseSize);
    const float spacing = letter_spacing(font_size);

    const char *const s = cstr(text);
    if (s[0] == '\0') return Clay_Dimensions{ 0, 0 }; // what MeasureTextEx() said
    float width = 0;
    int letters = 0;
    for (int i = 0; s[i] != '\0';) {
        int bytes = 0;
        const int codepoint = GetCodepointNext(&s[i], &bytes);
        i += bytes;
        if (codepoint == '\n') continue; // a line ends there: not Clay's to ask
        const int index = GetGlyphIndex(f, codepoint);
        const int advance = f.glyphs[index].advanceX;
        width +=
            (advance == 0 ? f.recs[index].width : static_cast<float>(advance)) * scale;
        ++letters;
    }
    if (letters == 0) return Clay_Dimensions{ 0, font_size };
    width += static_cast<float>(letters - 1) * spacing;

    // The spacing before it. A space: always. A word: when a letter is before
    // it on its line, spaces between them.
    const bool space = text.length == 1 && text.chars[0] == ' ';
    const char *before = text.chars;
    while (before > text.baseChars && before[-1] == ' ') --before;
    if (space || (before > text.baseChars && before[-1] != '\n')) width += spacing;
    return Clay_Dimensions{ width, font_size };
}

Clay_Dimensions measure_with_raylib(Clay_StringSlice text, Clay_TextElementConfig *config,
                                    void * /*unused*/) {
    auto size = static_cast<float>(config->fontSize);
    return measure_text(ui_font(size), text, size);
}

void pointer_from_raylib(Clay_Vector2 &position, bool &down) {
    const Vector2 p = GetMousePosition();
    position = Clay_Vector2{ p.x, p.y };
    down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
}

// Down/Up on the keyboard, the d-pad, or the left stick pushed far enough to
// be deliberate; Left/Right the same way; the press that means "do it", and
// the one that means "never mind" (Escape, the B button).
// This function is the whole of what the UI reads from the keyboard and the
// gamepad -- everything else works from the NavState it fills in. What the
// keys and the pad mean is nav_from_keys() and nav_from_pad(), in internal.h.
void nav_from_raylib(NavState &out) {
    const auto down = [](int key) { return IsKeyDown(key); };
    const auto pressed = [](int key) { return IsKeyPressed(key); };
    const auto pad = [](int &x, int &y, bool &submit, bool &cancel) {
        if (!IsGamepadAvailable(0)) return;
        nav_from_pad([](int button) { return IsGamepadButtonDown(0, button); },
                     [](int button) { return IsGamepadButtonPressed(0, button); },
                     [](int axis) { return GetGamepadAxisMovement(0, axis); }, x, y,
                     submit, cancel);
    };
    out = nav_from_keys(down, pressed, pad);
}

void read_nav(NavState &out) { providers.nav(out); }

// raylib's half of the text field: the character queue, as far as it goes,
// and Backspace pressed or repeating. Read only by a field that is typing.
void typed_from_raylib(Typed &out) {
    out = Typed{};
    int c = 0;
    while ((c = GetCharPressed()) != 0) {
        if (out.count < MAX_TYPED) out.codepoints[static_cast<size_t>(out.count++)] = c;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
        out.backspaces = 1;
}

void read_typed(Typed &out) { providers.text(out); }

void set_text_provider(TextFn fn) {
    providers.text = (fn != nullptr) ? fn : typed_from_raylib;
}

void set_nav_provider(NavFn fn) {
    providers.nav = (fn != nullptr) ? fn : nav_from_raylib;
}

// Both of these forget what Clay measured: it keeps a text's measurement for as
// long as the text is on screen, and one taken with another font, or another
// measurement, would lay the next frame out at the old width.
void set_font_provider(FontFn fn, const char *name) {
    providers.font = (fn != nullptr) ? fn : load_ui_font;
    providers.font_name = name;
    release_fonts();
    font.failed = false;
    if (context.started) Clay_ResetMeasureTextCache();
}

void set_measure_provider(MeasureFn fn) {
    providers.measure = (fn != nullptr) ? fn : measure_with_raylib;
    if (!context.started) return;
    Clay_SetMeasureTextFunction(providers.measure, nullptr);
    Clay_ResetMeasureTextCache();
}

// Both of these throw away what the UI thinks about the pointer, and they have
// to: wants_pointer() answers from a position and a geometry that have just
// been replaced by somebody else's. Without it a test that leaves the pointer
// over a button hands the NEXT test a UI that still believes the pointer is
// its -- which is how a click on an rmp::Object in a completely unrelated suite
// came back consumed.
void set_pointer_provider(PointerFn fn) {
    providers.pointer = (fn != nullptr) ? fn : pointer_from_raylib;
    begin_capture_frame();
    forget_claims();
}

void set_test_viewport(float width, float height) {
    test_viewport.width = width;
    test_viewport.height = height;
    begin_capture_frame();
    forget_claims();
}

bool test_mode() { return test_viewport.width > 0.0f && test_viewport.height > 0.0f; }

Clay_Dimensions viewport() {
    if (test_mode()) return Clay_Dimensions{ test_viewport.width, test_viewport.height };
    return Clay_Dimensions{ static_cast<float>(GetScreenWidth()),
                            static_cast<float>(GetScreenHeight()) };
}

bool bounds_of(std::string_view label, unsigned occurrence, int pass,
               Clay_BoundingBox *out) {
    const Clay_ElementData data =
        Clay_GetElementData(peek_element_id(label, occurrence, pass));
    if (!data.found) return false;
    if (out != nullptr) *out = data.boundingBox;
    return true;
}

int clay_error_count() { return clay.errors; }
Clay_ErrorType first_clay_error() { return clay.first_error; }

void reset_clay_errors_for_tests() {
    clay.errors = 0;
    clay.first_error = CLAY_ERROR_TYPE_INTERNAL_ERROR;
}

void read_pointer(Clay_Vector2 &position, bool &down) {
    providers.pointer(position, down);
}

namespace {
struct {
    Clay_Vector2 position{};
    bool down = false;
    bool was_down = false;
    bool present = false;
} pointer;
} // namespace

void update_pointer() {
    pointer.was_down = pointer.down;
    read_pointer(pointer.position, pointer.down);
    // A touch screen has no pointer when no finger is on it: the coordinates
    // stay wherever the last tap ended, and a button under them would sit lit
    // up forever.
    pointer.present = pointer.down || !touch_only();
}

// All five are gated on pass_input(). A pass that input cannot reach sees the
// same thing a pass sees when the window is not focused: nothing is down,
// nothing is present, nothing was just pressed. Doing it here rather than in
// each widget is what makes it impossible to miss one — the alternative is
// twelve widgets that each have to remember.
Clay_Vector2 pointer_position() { return pointer.position; }
bool pointer_down() { return pass_input() && pointer.down; }
bool pointer_present() { return pass_input() && pointer.present; }
bool pointer_just_pressed() { return pass_input() && pointer.down && !pointer.was_down; }
bool pointer_released() { return pass_input() && !pointer.down && pointer.was_down; }

namespace {
// The BACK buffer: what the last frame measured. The front one is being filled
// by the frame we are inside, and half of it does not exist yet.
const BoundsEntry *entry_of(uint32_t id) {
    const int back = 1 - bounds.front;
    for (int i = 0; i < bounds.count[back]; ++i) {
        if (bounds.entries[back][i].id == id) return &bounds.entries[back][i];
    }
    return nullptr;
}

bool inside_box(Clay_Vector2 p, const Clay_BoundingBox &b) {
    return p.x >= b.x && p.x <= b.x + b.width && p.y >= b.y && p.y <= b.y + b.height;
}

// Is `ancestor` one of the containers this element was declared inside? The
// chain of them is what the snapshot keeps instead of the tree, and it answers
// exactly where a box test cannot: a dropdown's own items stick two pixels out
// of the list they live in, so "inside its box" would say they are not its.
bool descends_from(const BoundsEntry *e, uint32_t ancestor) {
    uint32_t clip = e->clip;
    for (int depth = 0; clip != 0 && depth <= MAX_CLIP_DEPTH; ++depth) {
        if (clip == ancestor) return true;
        const BoundsEntry *c = entry_of(clip);
        if (c == nullptr) return false;
        clip = c->clip;
    }
    return false;
}
} // namespace

bool bounds_of_id(Clay_ElementId id, Clay_BoundingBox *out) {
    const BoundsEntry *e = entry_of(id.id);
    if (e == nullptr) return false;
    if (out != nullptr) *out = e->box;
    return true;
}

// Hit testing, OURS. It used to be Clay_PointerOver(), and that could not work
// with more than one pass: Clay_SetPointerState() runs against whatever tree is
// in Clay at the time, and begin() called it before Clay_BeginLayout — so pass 0
// was tested against last frame's pass 1 and pass 1 against this frame's pass 0.
// Neither pass ever met its own geometry, so with a pause menu over a HUD
// nothing in either scene could be hovered or clicked.
//
// This reads the per-pass snapshot instead, which is the same one-frame-old
// geometry every other interaction here uses, and it is per pass by
// construction. What the box test does not get for free is what Clay's tree
// walk gave us, so the two that matter are kept explicitly: the clipping
// ancestor an element was declared in, and anything declared in front of it.
bool pointer_over(Clay_ElementId id) { return pointer_over(id, 0.0f); }

bool pointer_over(Clay_ElementId id, float slop_y) {
    // The gate for a pass input cannot reach lives in pointer_present(), so no
    // widget has to remember it.
    if (!pointer_present()) return false;

    const BoundsEntry *e = entry_of(id.id);
    if (e == nullptr) return false;
    // An element with no area is not on screen, so nothing can be over it. It
    // is not a hypothetical: a headless frame has a viewport of 0x0, every box
    // in it is 0x0 at the origin, and a pointer resting at the origin is inside
    // every single one of them.
    if (e->box.width <= 0.0f || e->box.height <= 0.0f) return false;

    const Clay_Vector2 p = pointer_position();
    Clay_BoundingBox box = e->box;
    box.y -= slop_y;
    box.height += slop_y * 2.0f;
    if (!inside_box(p, box)) return false;

    // Clipped out of the container it lives in: on screen it is not there at
    // all, and the box it remembers is wherever it was pushed to. The innermost
    // one only — a floating list escapes the clipping of whatever it was
    // declared inside, so walking the whole chain would stop the items of a
    // dropdown that lives in a scroll area from being clickable.
    if (e->clip != 0) {
        const BoundsEntry *clip = entry_of(e->clip);
        if (clip != nullptr && !inside_box(p, clip->box)) return false;
    }

    // Something in front of it has the pointer.
    const int back = 1 - bounds.front;
    for (int i = 0; i < blockers.count[back]; ++i) {
        const Blocker &b = blockers.entries[back][i];
        if (b.pass != current_pass() || b.id == id.id) continue;
        const BoundsEntry *front = entry_of(b.id);
        if (front == nullptr || !inside_box(p, front->box)) continue;
        if (!descends_from(e, b.id)) return false;
    }
    return true;
}

namespace {

// `box` as far as `clip` lets it show; nothing when it does not.
Clay_BoundingBox cut_to(Clay_BoundingBox box, const Clay_BoundingBox &clip) {
    const float left = box.x > clip.x ? box.x : clip.x;
    const float top = box.y > clip.y ? box.y : clip.y;
    const float right = (box.x + box.width) < (clip.x + clip.width) ? box.x + box.width
                                                                    : clip.x + clip.width;
    const float bottom = (box.y + box.height) < (clip.y + clip.height)
        ? box.y + box.height
        : clip.y + clip.height;
    if (right <= left || bottom <= top) return Clay_BoundingBox{ 0, 0, 0, 0 };
    return Clay_BoundingBox{ left, top, right - left, bottom - top };
}

// Recorded for THIS frame, for a pass input can reach; a box with no area
// takes nothing.
void claim(const Clay_BoundingBox &box) {
    if (!pass_input() || box.width <= 0.0f || box.height <= 0.0f) return;
    const int front = bounds.front;
    if (claims.count[front] >= MAX_CLAIMS) {
        claims.full[front] = true;
        return;
    }
    claims.rects[front][claims.count[front]++] = box;
}

} // namespace

void claim_control(uint32_t id) {
    const std::optional<BoundsEntry> at = drawn_this_frame(id);
    if (!at) return;
    Clay_BoundingBox box = at->box;
    if (at->clip != 0) {
        const std::optional<BoundsEntry> area = drawn_this_frame(at->clip);
        if (area) box = cut_to(box, area->box);
    }
    claim(box);
}

bool pointer_claimed_now() {
    // Before the first begin() of a frame the pointer the UI sampled is last
    // frame's, or there is none yet: the frame is prepared -- the pointer
    // sampled -- by the first begin(), after the game's own code has asked,
    // even where the runner marked it before the hook (RMP_ENTRY_POINT). Then
    // it is read here, the same way update_pointer() reads it.
    Clay_Vector2 p = pointer.position;
    bool down = pointer.down;
    if (!frame.prepared) read_pointer(p, down);
    if (!down && touch_only()) return false; // no finger, no pointer
    // The frame drawn last: the back buffer once this one is prepared, the
    // front one until then.
    const int last = frame.prepared ? 1 - bounds.front : bounds.front;
    for (int i = 0; i < claims.count[last]; ++i) {
        if (inside_box(p, claims.rects[last][i])) return true;
    }
    return false;
}

bool claims_complete() {
    const int last = frame.prepared ? 1 - bounds.front : bounds.front;
    return !claims.full[last];
}

void claim_pointer_over_painted(Clay_RenderCommandArray commands) {
    // The gate for a pass input cannot reach, and for a touch screen nothing is
    // touching, is the same one hover has. What is painted is recorded either
    // way, for a pointer that arrives before the next frame's UI has run.
    bool hit = !pointer_present();
    const Clay_Vector2 p = pointer_position();

    // The clip rectangles open at this point of the list, outermost first. A
    // row scrolled out of its list still has a box, and the pointer must not
    // find it there.
    std::array<Clay_BoundingBox, MAX_CLIP_DEPTH> open_clips{};
    int depth = 0;
    int overflow = 0;
    auto shown_under_pointer = [&](const Clay_BoundingBox &b) {
        // No area, nothing to be over: see pointer_over().
        if (b.width <= 0.0f || b.height <= 0.0f || !inside_box(p, b)) return false;
        for (int i = 0; i < depth; ++i) {
            if (!inside_box(p, open_clips[i])) return false;
        }
        return true;
    };

    // What of `b` the open clips let show.
    auto shown = [&](Clay_BoundingBox b) {
        for (int i = 0; i < depth; ++i) b = cut_to(b, open_clips[i]);
        return b;
    };
    auto surface = [&](const Clay_RenderCommand &cmd) {
        // Painted inside at(): over the world, and not a wall in front of it.
        if (world_passthrough(cmd)) return;
        const Clay_BoundingBox &b = cmd.boundingBox;
        claim(shown(b));
        if (!hit && shown_under_pointer(b)) {
            set_pointer_over_ui();
            hit = true;
        }
    };

    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand &cmd = commands.internalArray[i];
        switch (cmd.commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            case CLAY_RENDER_COMMAND_TYPE_BORDER:
            case CLAY_RENDER_COMMAND_TYPE_IMAGE:
            case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                surface(cmd);
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                // A scroll area is interface even where it paints nothing:
                // dragging inside it scrolls it.
                surface(cmd);
                if (depth < MAX_CLIP_DEPTH) {
                    open_clips[depth++] = cmd.boundingBox;
                } else {
                    ++overflow;
                }
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                if (overflow > 0) {
                    --overflow;
                } else if (depth > 0) {
                    --depth;
                }
                break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT: // over the game, text is not a wall
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START: // a tint, not a surface
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
            case CLAY_RENDER_COMMAND_TYPE_NONE:
                break;
        }
    }
}

Clay_ElementId peek_sub_id(Clay_ElementId base, uint32_t which) {
    Clay_ElementId out = base;
    // Knuth's multiplicative constant: cheap, and it scatters the derived ids
    // far enough from the originals that a collision would be bad luck rather
    // than a pattern.
    out.id = base.id ^ ((which + 1) * 2654435761u);
    return out;
}

Clay_ElementId sub_id(Clay_ElementId base, uint32_t which) {
    Clay_ElementId out = peek_sub_id(base, which);
    // Remembered like any other id. A slider's rail is derived here rather than
    // through element_id(), and forgetting to record it is what made the rail
    // have no box to aim at the first time this snapshot existed — the layout
    // test caught it, which is exactly what it is for.
    //
    // Which is why the version that does NOT remember exists: an id ASKED about
    // before the element is declared -- a dropdown checking whether the pointer
    // is over one of its items -- would otherwise be recorded first, outside the
    // list it belongs to, and the record of the declaration would lose to it.
    remember_id(out.id);
    return out;
}

bool touch_only() {
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS)
    return true;
#else
    return false;
#endif
}

namespace {

void forget_frame() {
    set_frame_open(false);
    frame.marked = false;
    frame.self_marked = false;
    frame.prepared = false;
    runner.marked = false;
    this_pass.index = -1;
    this_pass.input = true;
    this_pass.behind = false;
    reset_id_counters();
    bounds.count = {};
    blockers.count = {};
    forget_claims();
    modality.this_frame = -1;
    modality.last_frame = -1;
    reset_frame_arena();
    clay.over_capacity = false;
    revealed.id = 0;
    pointer = {};
}

} // namespace

float safe_area_inset() {
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS)
    // An approximation, and deliberately so. raylib exposes no safe-area API,
    // and the real values need platform code — WindowInsets over JNI on
    // Android, safeAreaInsets on iOS. 24 design units clears a typical status
    // bar and gesture bar, costs nothing on a centred menu, and is the
    // difference between a corner element being visible or under the camera.
    // Replacing this with the real insets is a post-MVP job and touches nothing
    // above this function.
    return px(24.0f);
#else
    return 0.0f;
#endif
}

namespace {

// Where the world is drawn inside the viewport: rmp::app::detail::
// world_viewport(), the same numbers the camera draws by, against the
// viewport this UI lays out in -- so set_test_viewport() moves it too.
Rectangle world_area() {
    const Clay_Dimensions v = viewport();
    return rmp::app::detail::world_viewport(rmp::app::stretch(),
                                            Vector2{ v.width, v.height })
        .area;
}

} // namespace

Insets content_insets() {
    const Clay_Dimensions v = viewport();
    const float safe = safe_area_inset();
    const Rectangle area = world_area();
    // No bars when the area is the whole viewport -- NONE, EXPAND, or the
    // design's own shape -- and nothing but the safe area then.
    const auto wider = [safe](float bar) { return bar > safe ? bar : safe; };
    if (v.width <= 0 || v.height <= 0) return Insets{ safe, safe, safe, safe };
    return Insets{ wider(area.x), wider(area.y), wider(v.width - (area.x + area.width)),
                   wider(v.height - (area.y + area.height)) };
}

Clay_Dimensions content_area() {
    const Clay_Dimensions v = viewport();
    if (v.width <= 0 || v.height <= 0) return v;
    const Rectangle area = world_area();
    return Clay_Dimensions{ area.width, area.height };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public scale controls
// ---------------------------------------------------------------------------

float scale() { return detail::ui_scale(); }

void set_scale(float s) { detail::set_scale_override(s); }

// ---------------------------------------------------------------------------
// Breakpoints
//
// By aspect ratio, and that is the whole trick. A pixel threshold would call a
// 1080-pixel-wide phone a desktop, and scale() has already dealt with how big
// everything is — so the only question left, and the only one that decides
// whether a row still fits, is how wide the viewport is next to how tall.
// ---------------------------------------------------------------------------

// The shape of what the game is drawn in: the window, or the world inside its
// letterbox bars, whose shape is the design's whatever the window does.
Breakpoint current_breakpoint() {
    const Clay_Dimensions v = detail::content_area();
    if (v.height <= 0.0f) return Breakpoint::MEDIUM;
    const float aspect = v.width / v.height;
    if (aspect < 1.0f) return Breakpoint::COMPACT; // taller than wide
    if (aspect < 1.6f) return Breakpoint::MEDIUM; // up to about 16:10
    return Breakpoint::EXPANDED;
}

bool compact() { return current_breakpoint() == Breakpoint::COMPACT; }

} // namespace rmp::ui
