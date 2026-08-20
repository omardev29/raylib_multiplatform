// ===========================================================================
// The UI context: starting Clay, the scale, the font, the frame arena and
// element identity.
//
// Nothing here is public. What the user sees is begin()/end()/button()/text(),
// and the reason they need no configuration is that everything they would have
// had to configure is decided in this file.
// ===========================================================================

#include "internal.h"

#include <raylib_multiplatform/assets.h>
#include <raylib_multiplatform/generated/app_config.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Defaults, so this still compiles against a generated header from before the
// [ui] section existed. tools/configure.py normally provides all four.
#ifndef APP_UI_FONT
#define APP_UI_FONT ""
#endif
#ifndef APP_UI_FONT_SIZE
#define APP_UI_FONT_SIZE 20
#endif
#ifndef APP_UI_SCALE
#define APP_UI_SCALE 0.0f
#endif
#ifndef APP_UI_MAX_ELEMENTS
#define APP_UI_MAX_ELEMENTS 512
#endif

namespace rmp::ui {

// current_theme() / set_theme() live in theme.cpp.

namespace detail {

namespace {

bool  g_started    = false;
bool  g_frameOpen  = false;
void *g_arena      = nullptr;

float g_scale      = 1.0f;
float g_scaleOverride = APP_UI_SCALE;   // 0 = automatic

// The font. When [ui] font is empty we use raylib's built-in one, which needs
// no asset, no licence and no loading — and is a bitmap font, which is why its
// scale is rounded to a whole number below.
Font  g_font       = {0};
bool  g_fontLoaded = false;    // true only when we loaded a file and must unload it
float g_fontScale  = 1.0f;
int   g_bakedSize  = 0;
// A configured font that cannot be loaded is a one-time problem, not a
// per-draw one. Without this we would go back to the filesystem and log the
// same warning for every piece of text, every frame.
bool  g_fontFailed = false;

// Text arena. Clay keeps pointers into whatever we hand it and reads them at
// Clay_EndLayout, so the memory has to survive the frame. 8 KB is a lot of
// menu; if a UI ever needs more, the truncation below says so out loud.
constexpr int kArenaSize = 8 * 1024;
char g_textArena[kArenaSize];
int  g_arenaUsed = 0;
bool g_arenaWarned = false;

// Occurrence counters, so two buttons with the same label are two elements.
constexpr int kMaxLabels = 128;
struct LabelCount { uint32_t hash; uint16_t count; };
LabelCount g_labels[kMaxLabels];
int        g_labelCount = 0;
int16_t    g_layerZ = 0;

measure_fn g_measure = measure_with_raylib;
pointer_fn g_pointer = pointer_from_raylib;

// Test viewport. 0 means "ask raylib", which is every real run.
float g_testWidth  = 0.0f;
float g_testHeight = 0.0f;

void on_clay_error(Clay_ErrorData e) {
    TraceLog(LOG_WARNING, "UI: clay: %.*s", e.errorText.length, e.errorText.chars);
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

bool ensure_started() {
    if (g_started) return true;

    // Order matters: the element count is what sizes the arena, so it has to be
    // set before asking how much memory Clay needs. Clay's own default is 8192
    // elements, which reserves megabytes for a three-button menu — noticeable
    // on a phone, and pure waste everywhere.
    Clay_SetMaxElementCount(APP_UI_MAX_ELEMENTS);

    uint32_t size = Clay_MinMemorySize();
    g_arena = std::malloc(size);
    if (g_arena == nullptr) {
        TraceLog(LOG_ERROR, "UI: could not allocate %u bytes for the layout arena", size);
        return false;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, g_arena);
    Clay_Initialize(arena, viewport(), Clay_ErrorHandler{ on_clay_error, nullptr });
    Clay_SetMeasureTextFunction(g_measure, nullptr);

    g_started = true;
    TraceLog(LOG_INFO, "UI: ready (%u bytes, up to %d elements)", size, APP_UI_MAX_ELEMENTS);
    return true;
}

void shutdown_context() {
    if (!g_started) return;
    if (g_fontLoaded) {
        UnloadFont(g_font);
        g_fontLoaded = false;
    }
    std::free(g_arena);
    g_arena = nullptr;
    g_started = false;
}

bool frame_open() { return g_frameOpen; }
void set_frame_open(bool open) { g_frameOpen = open; }

// ---------------------------------------------------------------------------
// Scale
// ---------------------------------------------------------------------------

void update_scale() {
    if (g_scaleOverride > 0.0f) {
        g_scale = g_scaleOverride;
    } else {
        // Against the design resolution declared in [window]. min(), not max():
        // a UI that does not fit is worse than one with room to spare, so the
        // tighter axis wins and everything stays on screen.
        Clay_Dimensions v = viewport();
        float sx = v.width  / static_cast<float>(APP_WINDOW_WIDTH);
        float sy = v.height / static_cast<float>(APP_WINDOW_HEIGHT);
        float s  = sx < sy ? sx : sy;
        if (s < 0.5f) s = 0.5f;
        if (s > 4.0f) s = 4.0f;
        g_scale = s;
    }

    // The built-in font is a bitmap. Drawn at 1.73x it is a smeared mess, so
    // its scale is rounded to a whole number and the text size steps instead of
    // sliding. A TTF rasterises at any size, so it keeps the continuous scale.
    const bool builtin = (APP_UI_FONT[0] == '\0');
    if (builtin) {
        float rounded = std::floor(g_scale + 0.5f);
        g_fontScale = rounded < 1.0f ? 1.0f : rounded;
    } else {
        g_fontScale = g_scale;
    }
}

float ui_scale()   { return g_scale; }
float font_scale() { return g_fontScale; }

void set_scale_override(float s) {
    g_scaleOverride = (s > 0.0f) ? s : 0.0f;
    if (g_started) update_scale();
}

Font ui_font() {
    const bool builtin = (APP_UI_FONT[0] == '\0');
    if (builtin || g_fontFailed) return GetFontDefault();

    int wanted = static_cast<int>(std::floor(current_theme().font_size * g_fontScale + 0.5f));
    if (wanted < 1) wanted = 1;

    // Baking at 20 and drawing at 48 is how UI text ends up blurry. Re-bake
    // when the size the layout actually asks for has moved.
    if (g_fontLoaded && wanted == g_bakedSize) return g_font;
    if (g_fontLoaded) UnloadFont(g_font);

    g_font = rmp::assets::load_font(APP_UI_FONT, wanted);
    if (g_font.glyphCount <= 0) {
        TraceLog(LOG_WARNING, "UI: [ui] font '%s' could not be loaded; using the built-in font",
                 APP_UI_FONT);
        g_fontLoaded = false;
        g_bakedSize  = 0;
        g_fontFailed = true;       // say it once, then stop asking
        return GetFontDefault();   // a missing font must not switch the UI off
    }
    g_fontLoaded = true;
    g_bakedSize  = wanted;
    return g_font;
}

// ---------------------------------------------------------------------------
// Frame arena
// ---------------------------------------------------------------------------

void reset_frame_arena() {
    g_arenaUsed = 0;
    g_arenaWarned = false;
}

void *frame_alloc(size_t bytes) {
    // Everything stored here is at most pointer-aligned, so rounding the
    // cursor up to 8 is enough and costs a few bytes a frame.
    int aligned = (g_arenaUsed + 7) & ~7;
    if (aligned + static_cast<int>(bytes) > kArenaSize) return nullptr;
    void *p = g_textArena + aligned;
    g_arenaUsed = aligned + static_cast<int>(bytes);
    return p;
}

Clay_String intern(std::string_view s) {
    int len = static_cast<int>(s.size());
    if (len > kArenaSize - g_arenaUsed) {
        len = kArenaSize - g_arenaUsed;
        if (!g_arenaWarned) {
            TraceLog(LOG_WARNING, "UI: text arena full (%d bytes); labels are being truncated",
                     kArenaSize);
            g_arenaWarned = true;
        }
    }
    if (len <= 0) return Clay_String{ false, 0, g_textArena };

    char *dst = g_textArena + g_arenaUsed;
    std::memcpy(dst, s.data(), static_cast<size_t>(len));
    g_arenaUsed += len;
    // isStaticallyAllocated stays false: this lives exactly one frame, which is
    // the contract Clay asks for.
    return Clay_String{ false, len, dst };
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

void reset_id_counters() {
    g_labelCount = 0;
    g_layerZ = 0;
}

int16_t next_layer_z() { return ++g_layerZ; }

Clay_ElementId element_id(std::string_view label, const char *explicit_id) {
    if (explicit_id != nullptr) {
        // WithIndex(…, 0) rather than Clay_GetElementId, so there is exactly one
        // id scheme in the whole layer. Clay's two hashes disagree even at
        // offset 0 — the offset is mixed in before the final avalanche — so
        // using both would mean an element created one way could never be found
        // the other way. That is precisely how the headless test failed to see
        // a panel that was on screen.
        return Clay_GetElementIdWithIndex(intern(std::string_view{explicit_id}), 0);
    }

    uint32_t h = fnv1a(label);
    uint16_t occurrence = 0;
    int slot = -1;
    for (int i = 0; i < g_labelCount; i++) {
        if (g_labels[i].hash == h) { slot = i; break; }
    }
    if (slot >= 0) {
        occurrence = ++g_labels[slot].count;
    } else if (g_labelCount < kMaxLabels) {
        g_labels[g_labelCount++] = LabelCount{ h, 0 };
    }
    // Same label twice in one frame => different index => different element,
    // so hovering one does not light up the other.
    return Clay_GetElementIdWithIndex(intern(label), occurrence);
}

// ---------------------------------------------------------------------------
// Providers (swapped out by the headless layout tests)
// ---------------------------------------------------------------------------

Clay_Dimensions measure_with_raylib(Clay_StringSlice text, Clay_TextElementConfig *config, void *) {
    Font f = ui_font();
    float size = static_cast<float>(config->fontSize);
    Vector2 m = MeasureTextEx(f, cstr(text), size, size / 10.0f);
    return Clay_Dimensions{ m.x, m.y };
}

void pointer_from_raylib(Clay_Vector2 *position, bool *down) {
    Vector2 p = GetMousePosition();
    *position = Clay_Vector2{ p.x, p.y };
    *down     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
}

void set_measure_provider(measure_fn fn) {
    g_measure = (fn != nullptr) ? fn : measure_with_raylib;
    if (g_started) Clay_SetMeasureTextFunction(g_measure, nullptr);
}

void set_pointer_provider(pointer_fn fn) {
    g_pointer = (fn != nullptr) ? fn : pointer_from_raylib;
}

void set_test_viewport(float width, float height) {
    g_testWidth  = width;
    g_testHeight = height;
}

bool test_mode() { return g_testWidth > 0.0f && g_testHeight > 0.0f; }

Clay_Dimensions viewport() {
    if (test_mode()) return Clay_Dimensions{ g_testWidth, g_testHeight };
    return Clay_Dimensions{ static_cast<float>(GetScreenWidth()),
                            static_cast<float>(GetScreenHeight()) };
}

bool bounds_of(std::string_view label, unsigned occurrence, Clay_BoundingBox *out) {
    // Hashing only: no interning, so this does not disturb the frame arena or
    // the occurrence counters.
    Clay_String s{ false, static_cast<int32_t>(label.size()), label.data() };
    Clay_ElementData data = Clay_GetElementData(Clay_GetElementIdWithIndex(s, occurrence));
    if (!data.found) return false;
    if (out != nullptr) *out = data.boundingBox;
    return true;
}

void read_pointer(Clay_Vector2 *position, bool *down) { g_pointer(position, down); }

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

void set_scale(float s) {
    detail::set_scale_override(s);
}

} // namespace rmp::ui
