#include "assets.h"

#include <cstdio>
#include <cstring>

// rres declarations/types (implementation lives in assets_rres.c). rres-raylib.h
// includes rres.h. raylib.h must come first (already included via assets.h).
#include "rres-raylib.h"

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
#include <generated/app_config.h>
#ifndef RRES_PASSWORD
#define RRES_PASSWORD APP_RRES_PASSWORD
#endif

namespace {

bool g_usingPack = false;
rresCentralDir g_cdir = {0, nullptr};
char g_packPath[2048] = {0};

}  // namespace

namespace Assets {

void Init() {
    std::snprintf(g_packPath, sizeof(g_packPath), "%s%s", RESOURCES_PATH, RRES_PACK_FILE);

    if (FileExists(g_packPath)) {
        rresSetCipherPassword(RRES_PASSWORD);
        g_cdir = rresLoadCentralDirectory(g_packPath);
        if (g_cdir.count > 0) {
            g_usingPack = true;
            TraceLog(LOG_INFO, "ASSETS: Using resource pack %s (%d entries)", g_packPath, g_cdir.count);
        } else {
            g_usingPack = false;
            TraceLog(LOG_WARNING, "ASSETS: %s has no central directory, using loose files", g_packPath);
        }
    } else {
        g_usingPack = false;
        TraceLog(LOG_INFO, "ASSETS: No resource pack found, using loose files from %s", RESOURCES_PATH);
    }
}

void Shutdown() {
    if (g_usingPack) {
        rresUnloadCentralDirectory(g_cdir);
        g_cdir.count = 0;
        g_cdir.entries = nullptr;
    }
    g_usingPack = false;
}

bool UsingPack() { return g_usingPack; }

Image LoadImage(const char *name) {
    if (g_usingPack) {
        unsigned int id = rresGetResourceId(g_cdir, name);
        if (id != 0) {
            rresResourceMulti multi = rresLoadResourceMulti(g_packPath, id);
            if (multi.count > 0) {
                bool ok = true;
                for (int i = 0; i < multi.count; i++) {
                    int r = UnpackResourceChunk(&multi.chunks[i]);
                    if (r != 0) {
                        ok = false;
                        TraceLog(LOG_WARNING, "ASSETS: Failed to unpack '%s' (code %d, wrong password?)", name, r);
                    }
                }
                Image img = {0};
                if (ok) img = LoadImageFromResource(multi.chunks[0]);
                rresUnloadResourceMulti(multi);
                if (img.data != nullptr) return img;
            } else {
                rresUnloadResourceMulti(multi);
            }
            TraceLog(LOG_WARNING, "ASSETS: '%s' not usable from pack, falling back to loose file", name);
        } else {
            TraceLog(LOG_WARNING, "ASSETS: '%s' not found in pack, falling back to loose file", name);
        }
    }

    char path[2048];
    std::snprintf(path, sizeof(path), "%s%s", RESOURCES_PATH, name);
    return ::LoadImage(path);
}

Texture2D LoadTexture(const char *name) {
    Image img = LoadImage(name);
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

}  // namespace Assets
