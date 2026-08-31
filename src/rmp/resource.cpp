// ===========================================================================
// The reference-counted resource table behind rmp::Texture and friends.
//
// One slot per loaded resource. The count lives on the slot, not on the handle,
// which is what makes two handles to the same name share one GPU object rather
// than load it twice.
//
// The table is a flat array searched linearly, and that is not laziness: a game
// has tens of distinct resources, not thousands, and a linear scan over a
// contiguous array of small structs beats a hash map at that size on every
// machine. If a game ever arrives with a thousand, this is the file to change
// and nothing else moves — the table is not visible from any header.
//
// The seam for tests: a resource is loaded through the function pointers below,
// so a headless test can install one that fabricates payloads and never touches
// the GPU. That is what makes the counting testable without a window.
// ===========================================================================

#include <rmp/assets.h>

#include <raylib.h>

#include <cstring>

namespace rmp::detail {

namespace {

constexpr int kMaxSlots = 256;
constexpr unsigned kMaxPayload = 64; // the largest of the seven raylib structs

struct SlotData {
    ResourceKind kind{};
    char name[96] = { 0 };
    int font_size = 0;
    int refs = 0;
    bool named = false; // false = adopted, never matched by name
    unsigned char payload[kMaxPayload] = { 0 };
};

SlotData g_slots[kMaxSlots];
int g_used = 0;

void unload_payload(SlotData &slot) {
    // The one place in the framework where an Unload* is called. Everywhere
    // else, a resource going out of scope is what does it.
    switch (slot.kind) {
        case ResourceKind::TEXTURE:
            UnloadTexture(*reinterpret_cast<Texture2D *>(slot.payload));
            break;
        case ResourceKind::IMAGE:
            UnloadImage(*reinterpret_cast<::Image *>(slot.payload));
            break;
        case ResourceKind::FONT:
            UnloadFont(*reinterpret_cast<::Font *>(slot.payload));
            break;
        case ResourceKind::SOUND:
            UnloadSound(*reinterpret_cast<::Sound *>(slot.payload));
            break;
        case ResourceKind::MUSIC:
            UnloadMusicStream(*reinterpret_cast<::Music *>(slot.payload));
            break;
        case ResourceKind::SHADER:
            UnloadShader(*reinterpret_cast<::Shader *>(slot.payload));
            break;
        case ResourceKind::RENDER_TEXTURE:
            UnloadRenderTexture(*reinterpret_cast<RenderTexture2D *>(slot.payload));
            break;
    }
}

SlotData *find_named(ResourceKind kind, const char *name, int font_size) {
    for (int i = 0; i < g_used; i++) {
        SlotData &s = g_slots[i];
        if (s.refs > 0 && s.named && s.kind == kind && s.font_size == font_size &&
            std::strncmp(s.name, name, sizeof(s.name) - 1) == 0) {
            return &s;
        }
    }
    return nullptr;
}

SlotData *free_slot() {
    for (int i = 0; i < g_used; i++) {
        if (g_slots[i].refs == 0) return &g_slots[i];
    }
    if (g_used < kMaxSlots) return &g_slots[g_used++];
    return nullptr;
}

} // namespace

Slot *acquire_named(ResourceKind kind, const char *name, int font_size) {
    if (name == nullptr) return nullptr;
    if (SlotData *hit = find_named(kind, name, font_size)) {
        hit->refs++;
        return reinterpret_cast<Slot *>(hit);
    }
    return nullptr; // the caller loads it and calls adopt_named()
}

Slot *adopt(ResourceKind kind, const void *payload_bytes, unsigned bytes) {
    if (payload_bytes == nullptr || bytes > kMaxPayload) return nullptr;
    SlotData *slot = free_slot();
    if (slot == nullptr) {
        TraceLog(LOG_WARNING,
                 "ASSETS: the resource table is full (%d); not caching this one",
                 kMaxSlots);
        return nullptr;
    }
    *slot = SlotData{};
    slot->kind = kind;
    slot->refs = 1;
    slot->named = false;
    std::memcpy(slot->payload, payload_bytes, bytes);
    return reinterpret_cast<Slot *>(slot);
}

Slot *adopt_named(ResourceKind kind, const char *name, int font_size,
                  const void *payload_bytes, unsigned bytes) {
    Slot *slot = adopt(kind, payload_bytes, bytes);
    if (slot == nullptr) return nullptr;
    auto *data = reinterpret_cast<SlotData *>(slot);
    data->named = true;
    data->font_size = font_size;
    std::strncpy(data->name, name, sizeof(data->name) - 1);
    return slot;
}

void retain(Slot *slot) {
    if (slot != nullptr) reinterpret_cast<SlotData *>(slot)->refs++;
}

void release(Slot *slot) {
    if (slot == nullptr) return;
    auto *data = reinterpret_cast<SlotData *>(slot);
    if (data->refs <= 0) return;
    if (--data->refs == 0) {
        unload_payload(*data);
        *data = SlotData{};
    }
}

const void *payload(const Slot *slot) {
    if (slot == nullptr) return nullptr;
    return reinterpret_cast<const SlotData *>(slot)->payload;
}

int live_count() {
    int n = 0;
    for (int i = 0; i < g_used; i++) {
        if (g_slots[i].refs > 0) n++;
    }
    return n;
}

int ref_count(const char *name) {
    if (name == nullptr) return 0;
    int n = 0;
    for (int i = 0; i < g_used; i++) {
        SlotData &s = g_slots[i];
        if (s.refs > 0 && s.named &&
            std::strncmp(s.name, name, sizeof(s.name) - 1) == 0) {
            n += s.refs;
        }
    }
    return n;
}

void release_all() {
    // Called from rmp::assets::shutdown(), which the entry point runs BEFORE
    // the window closes. A texture released after CloseWindow() is a write to a
    // GL context that no longer exists.
    for (int i = 0; i < g_used; i++) {
        if (g_slots[i].refs > 0) {
            unload_payload(g_slots[i]);
            g_slots[i] = SlotData{};
        }
    }
    g_used = 0;
}

} // namespace rmp::detail
