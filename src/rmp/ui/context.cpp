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

#include <rmp/assets.h>
#include <rmp/config.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

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
    std::unique_ptr<unsigned char[]> arena; // Clay's memory, ours to own
} context;

struct {
    float scale = 1.0f;
    float scale_override = RMP_UI_SCALE; // 0 = automatic
} sizing;

// The font. When [ui] font is empty we use raylib's built-in one, which needs
// no asset, no licence and no loading — and is a bitmap font, which is why its
// scale is rounded to a whole number below.
struct {
    rmp::Font handle; // counted, like every other resource: nothing here unloads it
    bool loaded = false; // true once a file has been baked at baked_size
    float scale = 1.0f;
    int baked_size = 0;
    // A configured font that cannot be loaded is a one-time problem, not a
    // per-draw one. Without this we would go back to the filesystem and log the
    // same warning for every piece of text, every frame.
    bool failed = false;
} font;

// Text arena. Clay keeps pointers into whatever we hand it and reads them at
// Clay_EndLayout, so the memory has to survive the frame. 8 KB is a lot of
// menu; if a UI ever needs more, the truncation below says so out loud.
constexpr int ARENA_SIZE = 8 * 1024;
struct {
    char bytes[ARENA_SIZE];
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
    LabelCount table[MAX_LABELS];
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
    BoundsEntry entries[2][MAX_BOUNDS];
    int count[2] = { 0, 0 };
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
    Blocker entries[2][MAX_BLOCKERS];
    int count[2] = { 0, 0 };
} blockers;

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
    PassId ids[MAX_BOUNDS];
    int id_count = 0;
    int16_t layer_z = 0;
} this_pass;

// The clipping containers open right now, innermost last. Pushed by
// open_scroll() and popped by close_scroll(); reset per pass so an imbalance
// cannot leak into the next one.
constexpr int MAX_CLIP_DEPTH = 8;
struct {
    uint32_t stack[MAX_CLIP_DEPTH];
    int depth = 0;
    int overflow = 0; // pushed past the limit, so the pops still pair up
} clips;

// Where measuring, the pointer and navigation come from: raylib, unless a test
// has swapped one out.
struct {
    MeasureFn measure = measure_with_raylib;
    PointerFn pointer = pointer_from_raylib;
    NavFn nav = nav_from_raylib;
} providers;

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
    clay.errors++;
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
    for (char c : s) {
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

    uint32_t size = Clay_MinMemorySize();
    // new[] of char is aligned for anything Clay puts in it (the default new
    // alignment is 16 on every toolchain here); Clay itself only needs 8.
    context.arena = std::make_unique<unsigned char[]>(size);

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, context.arena.get());
    Clay_Initialize(arena, viewport(), Clay_ErrorHandler{ on_clay_error, nullptr });
    Clay_SetMeasureTextFunction(providers.measure, nullptr);

    context.started = true;
    TraceLog(LOG_INFO, "UI: ready (%u bytes, up to %d elements)", size,
             RMP_UI_MAX_ELEMENTS);
    return true;
}

void shutdown_context() {
    if (!context.started) return;
    if (font.loaded) {
        font.handle = rmp::Font{};
        font.loaded = false;
    }
    context.arena.reset();
    context.started = false;
}

bool frame_open() { return context.frame_open; }
void set_frame_open(bool open) { context.frame_open = open; }

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
        Clay_Dimensions v = viewport();
        float sx = v.width / static_cast<float>(RMP_WINDOW_WIDTH);
        float sy = v.height / static_cast<float>(RMP_WINDOW_HEIGHT);
        float s = sx < sy ? sx : sy;
        if (s < 0.5f) s = 0.5f;
        if (s > 4.0f) s = 4.0f;
        sizing.scale = s;
    }

    // The built-in font is a bitmap. Drawn at 1.73x it is a smeared mess, so
    // its scale is rounded to a whole number and the text Size steps instead of
    // sliding. A TTF rasterises at any size, so it keeps the continuous scale.
    const bool builtin = (RMP_UI_FONT[0] == '\0');
    if (builtin) {
        float rounded = std::floor(sizing.scale + 0.5f);
        font.scale = rounded < 1.0f ? 1.0f : rounded;
    } else {
        font.scale = sizing.scale;
    }
}

float ui_scale() { return sizing.scale; }
float font_scale() { return font.scale; }

void set_scale_override(float s) {
    sizing.scale_override = (s > 0.0f) ? s : 0.0f;
    if (context.started) update_scale();
}

::Font ui_font() {
    const bool builtin = (RMP_UI_FONT[0] == '\0');
    if (builtin || font.failed) return GetFontDefault();

    int wanted =
        static_cast<int>(std::floor(current_theme().font_size * font.scale + 0.5f));
    if (wanted < 1) wanted = 1;

    // Baking at 20 and drawing at 48 is how UI text ends up blurry. Re-bake
    // when the size the layout actually asks for has moved.
    if (font.loaded && wanted == font.baked_size) return font.handle.raw();
    // Assigning releases the old size; the resource table unloads it.
    font.handle = rmp::assets::load_font(RMP_UI_FONT, wanted);
    if (font.handle.raw().glyphCount <= 0) {
        RMP_REPORT_ONCE("UI: [ui] font '%s' could not be loaded; using the built-in font",
                        RMP_UI_FONT);
        font.loaded = false;
        font.baked_size = 0;
        font.failed = true; // say it once, then stop asking
        return GetFontDefault(); // a missing font must not switch the UI off
    }
    font.loaded = true;
    font.baked_size = wanted;
    return font.handle.raw();
}

// ---------------------------------------------------------------------------
// Frame arena
// ---------------------------------------------------------------------------

void reset_frame_arena() { text_arena.used = 0; }

void *frame_alloc(size_t bytes) {
    // Everything stored here is at most pointer-aligned, so rounding the
    // cursor up to 8 is enough and costs a few bytes a frame.
    int aligned = (text_arena.used + 7) & ~7;
    if (aligned + static_cast<int>(bytes) > ARENA_SIZE) return nullptr;
    void *p = text_arena.bytes + aligned;
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
    if (len <= 0) return Clay_String{ false, 0, text_arena.bytes };

    char *dst = text_arena.bytes + text_arena.used;
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
    this_pass.id_count = 0;
    clips.depth = 0;
    clips.overflow = 0;
}

bool frame_marked() { return frame.marked; }
bool frame_self_marked() { return frame.self_marked; }
void set_frame_self_marked(bool self) { frame.self_marked = self; }

void begin_pass() {
    this_pass.index++;
    reset_id_counters();
    begin_pass_focus();
    // The first pass of the frame is where wants_pointer() and wants_keyboard()
    // start again from nothing. See begin_capture_frame().
    if (this_pass.index == 0) begin_capture_frame();
}

int current_pass() { return this_pass.index < 0 ? 0 : this_pass.index; }
bool pass_input() { return this_pass.input; }

// Ask Clay for the box of every id this pass used, now that the layout is
// finished, and write them where the next frame's matching pass will look.
void capture_pass_bounds() {
    int &count = bounds.count[bounds.front];
    for (int i = 0; i < this_pass.id_count; i++) {
        if (count >= MAX_BOUNDS) {
            RMP_REPORT_ONCE("UI: more than %d elements in one frame; the extra ones lose "
                            "their remembered geometry, so a grid or slider among them "
                            "may size itself oddly",
                            MAX_BOUNDS);
            return;
        }
        Clay_ElementId key{};
        key.id = this_pass.ids[i].id;
        Clay_ElementData d = Clay_GetElementData(key);
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
        clips.overflow++;
        RMP_REPORT_ONCE("UI: scroll areas nested more than %d deep; the ones past that "
                        "do not clip what can be clicked inside them",
                        MAX_CLIP_DEPTH);
    }
}

void pop_clip() {
    if (clips.overflow > 0) {
        clips.overflow--;
    } else if (clips.depth > 0) {
        clips.depth--;
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

// Marking is unconditional: from here to end_frame() there is one frame, and
// nobody else gets to open another. Preparing is not, because the UI may not
// exist yet — see prepare_frame().
void begin_frame() {
    frame.marked = true;
    this_pass.index = -1;
    this_pass.input = true;
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

    reset_frame_arena();
    update_scale();
    anim_begin_frame();

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
    Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, Clay_Vector2{ wheel.x * 30.0f, wheel.y * 30.0f },
                                frame_time());

    // Focus spans the whole frame on purpose: arrow-key navigation has to see
    // the focusable widgets of every layer before it can decide where to go.
    begin_focus_frame();
}

void end_frame() {
    if (!frame.marked) return;
    if (frame.prepared) end_focus_frame();
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
}

void set_pass_input(bool reachable) { this_pass.input = reachable; }

float frame_time() { return test_mode() ? 0.0f : GetFrameTime(); }

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
    Clay_String s{ false, static_cast<int32_t>(label.size()), label.data() };
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

Clay_ElementId element_id(std::string_view label, const char *explicit_id) {
    // An explicit id is the escape hatch, and it says "this element, whatever
    // else is on screen" — so it never counts as an occurrence of anything.
    if (explicit_id != nullptr) return finish_id(std::string_view{ explicit_id }, 0);

    uint32_t h = fnv1a(label);
    uint16_t occurrence = 0;
    int slot = -1;
    for (int i = 0; i < labels.count; i++) {
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
        if (labels.overflow < static_cast<int>(INDICES_PER_PASS) / 2) labels.overflow++;
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

Clay_Dimensions measure_with_raylib(Clay_StringSlice text, Clay_TextElementConfig *config,
                                    void * /*unused*/) {
    ::Font f = ui_font();
    auto size = static_cast<float>(config->fontSize);
    Vector2 m = MeasureTextEx(f, cstr(text), size, size / 10.0f);
    return Clay_Dimensions{ m.x, m.y };
}

void pointer_from_raylib(Clay_Vector2 *position, bool *down) {
    Vector2 p = GetMousePosition();
    *position = Clay_Vector2{ p.x, p.y };
    *down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
}

// Down/Up on the keyboard, the d-pad, or the left stick pushed far enough to
// be deliberate; Left/Right the same way; the press that means "do it", and
// the one that means "never mind" (Escape, the B button).
// This function is the whole of what the UI reads from the keyboard and the
// gamepad -- everything else works from the NavState it fills in.
void nav_from_raylib(NavState *out) {
    int y = 0;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_TAB)) y += 1;
    if (IsKeyDown(KEY_UP)) y -= 1;
    int x = 0;
    if (IsKeyDown(KEY_RIGHT)) x += 1;
    if (IsKeyDown(KEY_LEFT)) x -= 1;

    bool submit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    bool cancel = IsKeyPressed(KEY_ESCAPE);

    if (IsGamepadAvailable(0)) {
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) y += 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP)) y -= 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) x += 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) x -= 1;
        float ly = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (ly > 0.5f) y += 1;
        if (ly < -0.5f) y -= 1;
        float lx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        if (lx > 0.5f) x += 1;
        if (lx < -0.5f) x -= 1;
        submit = submit || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        cancel = cancel || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
    }

    // Shift+Tab is "backwards", which is the one convention people expect
    // without being told.
    if (y > 0 && IsKeyDown(KEY_TAB) &&
        (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
        y = -1;
    }

    out->x = x > 0 ? 1 : (x < 0 ? -1 : 0);
    out->y = y > 0 ? 1 : (y < 0 ? -1 : 0);
    // Space activates like Enter, and is the one a text field types instead.
    out->activate = submit || IsKeyPressed(KEY_SPACE);
    out->submit = submit;
    out->cancel = cancel;
}

void read_nav(NavState *out) { providers.nav(out); }

void set_nav_provider(NavFn fn) {
    providers.nav = (fn != nullptr) ? fn : nav_from_raylib;
}

void set_measure_provider(MeasureFn fn) {
    providers.measure = (fn != nullptr) ? fn : measure_with_raylib;
    if (context.started) Clay_SetMeasureTextFunction(providers.measure, nullptr);
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
}

void set_test_viewport(float width, float height) {
    test_viewport.width = width;
    test_viewport.height = height;
    begin_capture_frame();
}

bool test_mode() { return test_viewport.width > 0.0f && test_viewport.height > 0.0f; }

Clay_Dimensions viewport() {
    if (test_mode()) return Clay_Dimensions{ test_viewport.width, test_viewport.height };
    return Clay_Dimensions{ static_cast<float>(GetScreenWidth()),
                            static_cast<float>(GetScreenHeight()) };
}

bool bounds_of(std::string_view label, unsigned occurrence, int pass,
               Clay_BoundingBox *out) {
    Clay_ElementData data = Clay_GetElementData(peek_element_id(label, occurrence, pass));
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

void read_pointer(Clay_Vector2 *position, bool *down) {
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
    read_pointer(&pointer.position, &pointer.down);
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
bool pointer_down() { return this_pass.input && pointer.down; }
bool pointer_present() { return this_pass.input && pointer.present; }
bool pointer_just_pressed() {
    return this_pass.input && pointer.down && !pointer.was_down;
}
bool pointer_released() { return this_pass.input && !pointer.down && pointer.was_down; }

namespace {
// The BACK buffer: what the last frame measured. The front one is being filled
// by the frame we are inside, and half of it does not exist yet.
const BoundsEntry *entry_of(uint32_t id) {
    const int back = 1 - bounds.front;
    for (int i = 0; i < bounds.count[back]; i++) {
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
    for (int depth = 0; clip != 0 && depth <= MAX_CLIP_DEPTH; depth++) {
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
    for (int i = 0; i < blockers.count[back]; i++) {
        const Blocker &b = blockers.entries[back][i];
        if (b.pass != current_pass() || b.id == id.id) continue;
        const BoundsEntry *front = entry_of(b.id);
        if (front == nullptr || !inside_box(p, front->box)) continue;
        if (!descends_from(e, b.id)) return false;
    }
    return true;
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

Breakpoint current_breakpoint() {
    Clay_Dimensions v = detail::viewport();
    if (v.height <= 0.0f) return Breakpoint::MEDIUM;
    const float aspect = v.width / v.height;
    if (aspect < 1.0f) return Breakpoint::COMPACT; // taller than wide
    if (aspect < 1.6f) return Breakpoint::MEDIUM; // up to about 16:10
    return Breakpoint::EXPANDED;
}

bool compact() { return current_breakpoint() == Breakpoint::COMPACT; }

} // namespace rmp::ui
