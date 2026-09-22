// ===========================================================================
// The reference-counted resource table behind rmp::Texture and friends.
//
// One slot per loaded resource. The count lives on the slot, not on the handle,
// which is what makes two handles to the same name share one GPU object rather
// than load it twice.
//
// The table is a vector of slots searched linearly, and that is not laziness:
// a game has tens of distinct resources, not thousands, and a linear scan over
// a few dozen small structs beats a hash map at that size on every machine. If
// a game ever arrives with a thousand, this is the file to change and nothing
// else moves -- the table is not visible from any header.
//
// EACH SLOT OWNS ITS PAYLOAD through a std::shared_ptr<void>, which carries the
// payload type's destructor with it: the table never learns what a SheetData
// is, and the sheet's tables are freed by the language when the slot lets go.
// The raylib Unload* is the table's job and happens first. It used to be a
// 64-byte byte array memcpy'd from the caller, at offset 109 of a struct with
// alignof 4, read back through reinterpret_cast -- undefined behaviour on every
// access, and a type with a vector in it could not have lived there at all.
//
// SLOTS ARE NEVER FREED, only emptied. A handle that outlives release_all()
// (a global rmp::Texture, say) still points at a slot that exists and reads
// refs == 0, so its destructor is a no-op instead of a use after free.
//
// The seam for tests: a resource is loaded through the function pointers in
// assets.cpp, so a headless test can install one that fabricates payloads and
// never touches the GPU. That is what makes the counting testable without a
// window.
// ===========================================================================

#include <rmp/assets.h>

#include <raylib.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rmp::detail {

namespace {

struct SlotData {
    // The WHOLE name, compared whole. It used to be a 96-byte buffer compared
    // with strncmp, so two names sharing a 95-character prefix were one cache
    // entry and the second load handed back the first texture.
    std::string name;
    ResourceKind kind{};
    int font_size = 0;
    int refs = 0;
    bool named = false; // false = adopted, never matched by name
    std::shared_ptr<void> payload;
};

// unique_ptr so a slot's address is stable while the vector grows: a handle
// IS that address.
std::vector<std::unique_ptr<SlotData>> g_slots;

void unload_payload(SlotData &slot) {
    // The one place in the framework where an Unload* is called. Everywhere
    // else, a resource going out of scope is what does it.
    void *p = slot.payload.get();
    if (p == nullptr) return;
    switch (slot.kind) {
        case ResourceKind::TEXTURE:
            UnloadTexture(*static_cast<Texture2D *>(p));
            break;
        case ResourceKind::IMAGE:
            UnloadImage(*static_cast<::Image *>(p));
            break;
        case ResourceKind::FONT:
            UnloadFont(*static_cast<::Font *>(p));
            break;
        case ResourceKind::SOUND:
            UnloadSound(*static_cast<::Sound *>(p));
            break;
        case ResourceKind::MUSIC:
            UnloadMusicStream(*static_cast<::Music *>(p));
            break;
        case ResourceKind::SHADER:
            UnloadShader(*static_cast<::Shader *>(p));
            break;
        case ResourceKind::RENDER_TEXTURE:
            UnloadRenderTexture(*static_cast<RenderTexture2D *>(p));
            break;
        case ResourceKind::SHEET: {
            // The texture is raylib's to unload; the frame and tag tables are
            // vectors and go with the SheetData when the shared_ptr lets go.
            auto *sheet = static_cast<rmp::SheetData *>(p);
            if (sheet->texture.id != 0) UnloadTexture(sheet->texture);
            break;
        }
    }
}

void empty(SlotData &slot) {
    unload_payload(slot);
    slot.payload.reset();
    slot.name.clear();
    slot.named = false;
    slot.font_size = 0;
    slot.refs = 0;
}

SlotData *find_named(ResourceKind kind, const char *name, int font_size) {
    for (const auto &s : g_slots) {
        if (s->refs > 0 && s->named && s->kind == kind && s->font_size == font_size &&
            s->name == name) {
            return s.get();
        }
    }
    return nullptr;
}

SlotData *free_slot() {
    for (const auto &s : g_slots) {
        if (s->refs == 0) return s.get();
    }
    g_slots.push_back(std::make_unique<SlotData>());
    return g_slots.back().get();
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

Slot *adopt_owned(ResourceKind kind, std::shared_ptr<void> payload) {
    if (payload == nullptr) return nullptr;
    SlotData *slot = free_slot();
    slot->kind = kind;
    slot->refs = 1;
    slot->named = false;
    slot->font_size = 0;
    slot->name.clear();
    slot->payload = std::move(payload);
    return reinterpret_cast<Slot *>(slot);
}

Slot *adopt_named_owned(ResourceKind kind, const char *name, int font_size,
                        std::shared_ptr<void> payload) {
    if (name == nullptr) return nullptr;
    Slot *slot = adopt_owned(kind, std::move(payload));
    if (slot == nullptr) return nullptr;
    auto *data = reinterpret_cast<SlotData *>(slot);
    data->named = true;
    data->font_size = font_size;
    data->name = name;
    return slot;
}

void retain(Slot *slot) {
    if (slot != nullptr) reinterpret_cast<SlotData *>(slot)->refs++;
}

void release(Slot *slot) {
    if (slot == nullptr) return;
    auto *data = reinterpret_cast<SlotData *>(slot);
    if (data->refs <= 0) return;
    if (--data->refs == 0) empty(*data);
}

const void *payload(const Slot *slot) {
    if (slot == nullptr) return nullptr;
    return reinterpret_cast<const SlotData *>(slot)->payload.get();
}

int live_count() {
    int n = 0;
    for (const auto &s : g_slots) {
        if (s->refs > 0) n++;
    }
    return n;
}

int ref_count(const char *name) {
    if (name == nullptr) return 0;
    int n = 0;
    for (const auto &s : g_slots) {
        if (s->refs > 0 && s->named && s->name == name) n += s->refs;
    }
    return n;
}

void release_all() {
    // Called from rmp::assets::shutdown(), which the entry point runs BEFORE
    // the window closes. A texture released after CloseWindow() is a write to a
    // GL context that no longer exists. The slots stay allocated: see the note
    // at the top about handles that outlive this.
    for (const auto &s : g_slots) {
        if (s->refs > 0) empty(*s);
    }
}

} // namespace rmp::detail
