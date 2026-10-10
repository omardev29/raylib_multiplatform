// ===========================================================================
// The resource pack: finding it, opening it, reading one entry out of it.
//
// A release ships a single resources.rres — the container rres defines, with
// every file in resources/ inside it, AES-encrypted. Development ships the
// loose files instead. Everything above this file is written so that the
// difference never shows.
// ===========================================================================

#include <raylib.h>
#include "rres-raylib.h" // declarations only; the implementation is rres_impl.cpp

#include "internal.h"
#include <rmp/config.h> // RMP_RRES_PASSWORD

#include <cstdio>
#include <utility>

// The pack's password is RMP_RRES_PASSWORD, [resources] rres_password in the
// .toml, and the packer in CMakeLists.txt encrypts with the same value.
// Obfuscation, not real security: it ends up in the binary.
//
// It used to be hard-coded here AND in CMakeLists.txt, and only the desktop
// build passed its copy as a -D, so Android and iOS silently used this one --
// change one and those two platforms could no longer read their own asset
// pack, with no error that pointed at the cause. One value, from the config.

namespace rmp::assets::detail {

namespace {

// The pack, when there is one. The dot at every use says "file state".
struct {
    bool in_use = false;
    rresCentralDir directory = { 0, nullptr };
    char path[2048] = { 0 };
} pack;

} // namespace

bool open_pack() {
    if (pack.in_use) return true; // idempotent: the lifecycle macro already called it

    std::snprintf(pack.path, sizeof(pack.path), "%s%s",
                  rmp::assets::detail::resources_root(), "resources.rres");

    if (!FileExists(pack.path)) {
        TraceLog(LOG_INFO, "ASSETS: No resource pack found, using loose files from %s",
                 rmp::assets::detail::resources_root());
        return false;
    }

    rresSetCipherPassword(RMP_RRES_PASSWORD);
    pack.directory = rresLoadCentralDirectory(pack.path);
    if (pack.directory.count <= 0) {
        // Given back, not dropped: rres allocates the entry array before it
        // knows the count is zero, and close_pack() returns early while no
        // pack is open, so nothing else would ever free it. Zeroed as well,
        // because a non-null pack.directory.entries sitting behind a false
        // pack_is_open() is a trap for anyone who reads one without the other.
        rresUnloadCentralDirectory(pack.directory);
        pack.directory.count = 0;
        pack.directory.entries = nullptr;
        TraceLog(LOG_WARNING, "ASSETS: %s has no central directory, using loose files",
                 pack.path);
        return false;
    }

    pack.in_use = true;
    TraceLog(LOG_INFO, "ASSETS: Using resource pack %s (%d entries)", pack.path,
             pack.directory.count);
    return true;
}

void close_pack() {
    if (!pack.in_use) return;
    rresUnloadCentralDirectory(pack.directory);
    pack.directory.count = 0;
    pack.directory.entries = nullptr;
    pack.in_use = false;
}

bool pack_is_open() { return pack.in_use; }

bool pack_has(const char *name) {
    // The central directory alone: nothing is read, decrypted or allocated,
    // which is what makes it cheap enough to try five extensions per name.
    return pack.in_use && name != nullptr && rresGetResourceId(pack.directory, name) != 0;
}

unsigned char *pack_read(const char *name, int *size) {
    if (size != nullptr) *size = 0;
    if (!pack.in_use || name == nullptr) return nullptr;

    const unsigned int id = rresGetResourceId(pack.directory, name);
    if (id == 0) return nullptr;

    rresResourceChunk chunk = rresLoadResourceChunk(pack.path, id);
    if (UnpackResourceChunk(&chunk) != 0) {
        rresUnloadResourceChunk(chunk);
        return nullptr;
    }

    unsigned int data_size = 0;
    // LoadDataFromResource, not ...FromResourceChunk: the latter is static
    // inside rres-raylib.h. The public one also transparently follows a LINK
    // chunk to an external file.
    void *data = LoadDataFromResource(chunk, &data_size);
    rresUnloadResourceChunk(chunk);
    if (data == nullptr) return nullptr;

    if (size != nullptr) *size = static_cast<int>(data_size);
    return static_cast<unsigned char *>(data);
}

// Images are the one thing the pack can hold as a *decoded* resource rather
// than as the original file: rrespacker stores them as an IMGE chunk, and rres
// gives us back an Image directly. So this cannot go through pack_read(), which
// hands out bytes. Everything else (sounds, fonts, raw data) is stored
// verbatim and is loaded from memory by the caller.
//
// Returns a zeroed Image if the name is not packed or does not decode; the
// caller falls back to the loose file.
::Image pack_read_image(const char *name) {
    ::Image img = { nullptr };
    if (!pack.in_use || name == nullptr) return img;

    const unsigned int id = rresGetResourceId(pack.directory, name);
    if (id == 0) {
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not found in pack, falling back to loose file", name);
        return img;
    }

    const rresResourceMulti multi = rresLoadResourceMulti(pack.path, id);
    if (multi.count > 0) {
        bool ok = true;
        for (int i = 0; std::cmp_less(i, multi.count); ++i) {
            const int r = UnpackResourceChunk(&multi.chunks[i]);
            if (r != 0) {
                ok = false;
                TraceLog(LOG_WARNING,
                         "ASSETS: Failed to unpack '%s' (code %d, wrong password?)", name,
                         r);
            }
        }
        if (ok) img = LoadImageFromResource(multi.chunks[0]);
    }
    rresUnloadResourceMulti(multi);

    if (img.data == nullptr) {
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack, falling back to loose file", name);
    }
    return img;
}

} // namespace rmp::assets::detail
