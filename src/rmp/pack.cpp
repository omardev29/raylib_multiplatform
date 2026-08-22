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
#include <rmp/config.h> // APP_RRES_PASSWORD

#include <cstdio>
#include <utility>

#ifndef RRES_PACK_FILE
#define RRES_PACK_FILE "resources.rres"
#endif

// Password used to decrypt encrypted packs. Obfuscation, not real security:
// it ends up in the binary.
//
// It used to be hard-coded here AND in CMakeLists.txt. Only the desktop build
// passes -DRRES_PASSWORD, so Android and iOS silently used this copy — change
// one and those two platforms could no longer read their own asset pack, with
// no error that pointed at the cause. Both now come from the config.
#ifndef RRES_PASSWORD
#define RRES_PASSWORD APP_RRES_PASSWORD
#endif

namespace rmp::assets::detail {

namespace {

bool g_using_pack = false;
rresCentralDir g_cdir = { 0, nullptr };
char g_pack_path[2048] = { 0 };

} // namespace

bool open_pack() {
    if (g_using_pack) return true; // idempotent: the lifecycle macro already called it

    std::snprintf(g_pack_path, sizeof(g_pack_path), "%s%s", RESOURCES_PATH,
                  RRES_PACK_FILE);

    if (!FileExists(g_pack_path)) {
        TraceLog(LOG_INFO, "ASSETS: No resource pack found, using loose files from %s",
                 RESOURCES_PATH);
        return false;
    }

    rresSetCipherPassword(RRES_PASSWORD);
    g_cdir = rresLoadCentralDirectory(g_pack_path);
    if (g_cdir.count <= 0) {
        TraceLog(LOG_WARNING, "ASSETS: %s has no central directory, using loose files",
                 g_pack_path);
        return false;
    }

    g_using_pack = true;
    TraceLog(LOG_INFO, "ASSETS: Using resource pack %s (%d entries)", g_pack_path,
             g_cdir.count);
    return true;
}

void close_pack() {
    if (!g_using_pack) return;
    rresUnloadCentralDirectory(g_cdir);
    g_cdir.count = 0;
    g_cdir.entries = nullptr;
    g_using_pack = false;
}

bool pack_is_open() { return g_using_pack; }

unsigned char *pack_read(const char *name, int *size) {
    if (size != nullptr) *size = 0;
    if (!g_using_pack || name == nullptr) return nullptr;

    unsigned int id = rresGetResourceId(g_cdir, name);
    if (id == 0) return nullptr;

    rresResourceChunk chunk = rresLoadResourceChunk(g_pack_path, id);
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
Image pack_read_image(const char *name) {
    Image img = { nullptr };
    if (!g_using_pack || name == nullptr) return img;

    unsigned int id = rresGetResourceId(g_cdir, name);
    if (id == 0) {
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not found in pack, falling back to loose file", name);
        return img;
    }

    rresResourceMulti multi = rresLoadResourceMulti(g_pack_path, id);
    if (multi.count > 0) {
        bool ok = true;
        for (int i = 0; std::cmp_less(i, multi.count); i++) {
            int r = UnpackResourceChunk(&multi.chunks[i]);
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
