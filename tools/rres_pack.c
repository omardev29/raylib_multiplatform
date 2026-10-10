// rres_pack — minimal, open resource packer for the rres format.
//
// Packs files as RRES_DATA_RAW chunks plus a central directory, optionally
// AES-256-CTR encrypted in the exact layout rrespacker / rres-raylib.h expect:
//
//   packed = AES-CTR(plaintext) || salt[16] || MD5(plaintext)[16]
//   key    = Argon2i(password, salt)   (16 MiB, 3 passes, 1 lane, 32-byte key)
//   IV     = all zeros (tiny-AES-c AES_init_ctx leaves the CTR counter zeroed)
//
// This is NOT a replacement for the full rrespacker (no image/font decoding,
// no DEFLATE/LZ4/QOI compression); it exists so the template can produce and
// test .rres files without a paid/closed binary. For production assets you may
// prefer the official rrespacker.
//
// Usage:
//   rres_pack <output.rres> <password|-> <file> [file ...]
//   A '-' password disables encryption. Files are stored under their basename.

#define RRES_IMPLEMENTATION
#include "rres.h"

#include "md5.h"

// tiny-AES-c (CTR + AES256 are its defaults) and monocypher (Argon2i). Their
// .c files are translation units of their own in the rres_pack target
// (CMakeLists.txt), compiled quietly like the rest of thirdparty/: included
// here, the static analyzer followed main() into Argon2 and reported
// monocypher's code as ours.
#include "../thirdparty/rres/external/aes.h"
#include "../thirdparty/rres/external/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Every allocation goes through here. None of them was checked, and a NULL
// from calloc() was dereferenced on the next line -- the entry table and each
// chunk's plaintext, which gcc's analyzer found, and the rest the same way.
// Out of memory is a sentence and exit 1, never a segfault in the middle of a
// release build. Exiting is the whole recovery: this is a command-line tool,
// and the process returning its memory is what frees it. Always zeroed, so
// nothing uninitialised can reach a pack whatever a read leaves short.
static void *checked_calloc(size_t size) {
    void *p = calloc(size, 1);
    if (!p) {
        fprintf(stderr, "ERROR: out of memory (%lu bytes)\n", (unsigned long)size);
        exit(1);
    }
    return p;
}

// A failure after something was opened or allocated: say it and stop. Exiting
// from here rather than returning from main() is not a shortcut either -- each
// `return 1` used to leave the entry table and every chunk behind it, which
// the analyzer reports, correctly, as a leak.
static void fail(const char *what, const char *path) {
    fprintf(stderr, "ERROR: %s %s\n", what, path);
    exit(1);
}

// Read a whole file, or fail. Every step is checked, which is not paranoia:
// glibc lets fopen() succeed on a *directory*, and the loose version of this
// function then packed one — malloc'd buffer, fread() reads nothing, and the
// uninitialised heap behind it went into the .rres as a resource. Where ftell()
// returns -1 for the directory instead, the (unsigned) size became 4294967295
// and the chunk arithmetic downstream overflowed.
static unsigned char *read_file(const char *path, size_t *outSize) {
    *outSize = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    // One byte for an empty file: calloc(0) may answer NULL, which is not
    // running out of memory.
    unsigned char *buf = (unsigned char *)checked_calloc(sz > 0 ? (size_t)sz : 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (size_t)sz;
    return buf;
}

// Pack up to 8 extension chars (with leading dot) into two big-endian u32.
static unsigned int pack_ext_be(const char *ext, int start) {
    unsigned int v = 0;
    int len = (int)strlen(ext);
    for (int i = 0; i < 4; i++) {
        unsigned char c = 0;
        int idx = start + i;
        if (idx < len) c = (unsigned char)ext[idx];
        v = (v << 8) | c;
    }
    return v;
}

// Write a little-endian u32/u16 regardless of host endianness.
static void put_u32(unsigned char *p, unsigned int v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_u16(unsigned char *p, unsigned short v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

typedef struct {
    unsigned int id;
    unsigned int offset;       // absolute offset of the chunk info in the file
    const char *name;          // basename stored in the central directory
    unsigned char *packed;     // final packed chunk data (maybe encrypted)
    unsigned int packedSize;
    unsigned int baseSize;
    unsigned char cipherType;
} Entry;

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <output.rres> <password|-> <file> [file ...]\n", argv[0]);
        return 1;
    }

    const char *outPath = argv[1];
    // Not `password`: rres.h has a file-scope one, and this would hide it.
    const char *pass = argv[2];
    int encrypt = (strcmp(pass, "-") != 0);
    int fileCount = argc - 3;
    const char **files = (const char **)&argv[3];

    Entry *entries = (Entry *)checked_calloc((size_t)fileCount * sizeof(Entry));

    // Build each resource chunk
    for (int i = 0; i < fileCount; i++) {
        const char *path = files[i];
        const char *name = base_name(path);

        size_t fileSize = 0;
        unsigned char *fileData = read_file(path, &fileSize);
        if (!fileData) fail("cannot read", path);
        // rres keeps sizes in 32 bits, and a chunk adds its 20-byte header and
        // its 32-byte trailer to the file. The size used to come back from
        // read_file() already cut to 32 bits, so a 5 GiB file packed as its
        // first gigabyte, and one just under 4 GiB wrapped baseSize below and
        // wrote past the end of `plain`.
        if (fileSize > 0xFFFFFFFFu - 64) fail("too large for an rres pack (4 GiB):", path);

        // Raw chunk plaintext: propCount(4) + props[4](16) + raw(fileSize)
        unsigned int baseSize = 4 + 16 + (unsigned int)fileSize;
        unsigned char *plain = (unsigned char *)checked_calloc(baseSize);
        put_u32(plain + 0, 4);                       // propCount
        put_u32(plain + 4, (unsigned int)fileSize);  // props[0] = size
        const char *dot = strrchr(name, '.');
        const char *ext = dot ? dot : "";
        put_u32(plain + 8, pack_ext_be(ext, 0));     // props[1] = ext part 1
        put_u32(plain + 12, pack_ext_be(ext, 4));    // props[2] = ext part 2
        put_u32(plain + 16, 0);                      // props[3] = reserved
        // Unconditionally: read_file() hands back a real buffer of fileSize
        // bytes, one zeroed byte for an empty file, so there is no branch here
        // for an analyzer to lose track of. gcc 13's followed `fileSize > 0`
        // into a file it had just read as empty and called fileData
        // uninitialised.
        memcpy(plain + 20, fileData, fileSize);

        Entry *e = &entries[i];
        e->id = rresComputeCRC32((const unsigned char *)name, (int)strlen(name));
        e->name = name;
        e->baseSize = baseSize;
        e->cipherType = encrypt ? RRES_CIPHER_AES : RRES_CIPHER_NONE;

        if (encrypt) {
            // salt[16]. A short read from /dev/urandom used to leave the rest of
            // it whatever the stack held; now any shortfall takes the fallback.
            //
            // rand() is enough for the fallback, and that is not a shortcut:
            // the password ships inside the game ([resources] rres_password),
            // so the pack is obfuscation and the salt only has to exist.
            unsigned char salt[16] = {0};
            FILE *rnd = fopen("/dev/urandom", "rb");
            size_t got = 0;
            if (rnd) {
                got = fread(salt, 1, sizeof(salt), rnd);
                fclose(rnd);
            }
            if (got != sizeof(salt)) {
                // NOLINTNEXTLINE(misc-predictable-rand): see above, obfuscation only
                for (int b = 0; b < 16; b++) salt[b] = (unsigned char)(rand() & 0xff);
            }

            // key = Argon2i(password, salt)
            uint8_t key[32] = {0};
            crypto_argon2_config config = {
                .algorithm = CRYPTO_ARGON2_I,
                .nb_blocks = 16384,   // 16 MiB
                .nb_passes = 3,
                .nb_lanes  = 1
            };
            crypto_argon2_inputs inputs = {
                .pass = (const uint8_t *)pass,
                .salt = salt,
                .pass_size = (uint32_t)strlen(pass),
                .salt_size = 16
            };
            crypto_argon2_extras extras = {0};
            void *work = checked_calloc((size_t)config.nb_blocks * 1024);
            crypto_argon2(key, 32, work, config, inputs, extras);
            free(work);

            // AES-256-CTR encrypt the plaintext (zero IV)
            unsigned char *cipher = (unsigned char *)checked_calloc(baseSize);
            memcpy(cipher, plain, baseSize);
            struct AES_ctx ctx = {0};
            AES_init_ctx(&ctx, key);                 // leaves CTR counter (Iv) zeroed
            AES_CTR_xcrypt_buffer(&ctx, cipher, baseSize);
            crypto_wipe(key, 32);

            // MD5 integrity over the plaintext
            unsigned int *md5 = ComputeMD5(plain, baseSize);

            // packed = cipher || salt[16] || MD5[16]
            e->packedSize = baseSize + 32;
            e->packed = (unsigned char *)checked_calloc(e->packedSize);
            memcpy(e->packed, cipher, baseSize);
            memcpy(e->packed + baseSize, salt, 16);
            memcpy(e->packed + baseSize + 16, md5, 16);
            free(cipher);
        } else {
            e->packedSize = baseSize;
            e->packed = (unsigned char *)checked_calloc(baseSize);
            memcpy(e->packed, plain, baseSize);
        }

        free(plain);
        free(fileData);
    }

    // Compute layout offsets: header(16) + chunks..., then central directory
    unsigned int cursor = 16;
    for (int i = 0; i < fileCount; i++) {
        entries[i].offset = cursor;
        cursor += 32 + entries[i].packedSize;      // info(32) + data
    }
    unsigned int cdAbsOffset = cursor;

    // Build central directory chunk data: propCount(4) + props[1](4) + entries
    unsigned int cdRawSize = 0;
    for (int i = 0; i < fileCount; i++) {
        unsigned int nameSize = (unsigned int)strlen(entries[i].name) + 1;   // + NULL
        nameSize = (nameSize + 3) & ~3u;                                     // pad to 4
        cdRawSize += 16 + nameSize;
    }
    unsigned int cdBaseSize = 4 + 4 + cdRawSize;
    unsigned char *cdData = (unsigned char *)checked_calloc(cdBaseSize);
    put_u32(cdData + 0, 1);                          // propCount
    put_u32(cdData + 4, (unsigned int)fileCount);    // props[0] = entry count
    {
        unsigned char *ptr = cdData + 8;
        for (int i = 0; i < fileCount; i++) {
            unsigned int nameLen = (unsigned int)strlen(entries[i].name) + 1;
            unsigned int nameSize = (nameLen + 3) & ~3u;
            put_u32(ptr + 0, entries[i].id);
            put_u32(ptr + 4, entries[i].offset);
            put_u32(ptr + 8, 0);                     // reserved
            put_u32(ptr + 12, nameSize);
            memcpy(ptr + 16, entries[i].name, nameLen);   // pads with zeros to nameSize
            ptr += 16 + nameSize;
        }
    }

    // Write the file. Every write is counted and so is the close, which is
    // where a full disk shows up for a buffered stream: unchecked, a pack cut
    // short printed "Packed" and exited 0, and the release shipped it.
    FILE *out = fopen(outPath, "wb");
    if (!out) fail("cannot open for writing:", outPath);
    size_t shortWrites = 0;

    // Header: id, version=100, chunkCount (data chunks + CD), cdOffset (relative
    // to end of header, because the loader does fseek(cdOffset, SEEK_CUR) from 16)
    unsigned char header[16] = { 'r', 'r', 'e', 's' };
    put_u16(header + 4, 100);
    put_u16(header + 6, (unsigned short)(fileCount + 1));
    put_u32(header + 8, cdAbsOffset - 16);
    put_u32(header + 12, 0);
    shortWrites += fwrite(header, 1, 16, out) != 16;

    // Data chunks
    for (int i = 0; i < fileCount; i++) {
        Entry *e = &entries[i];
        unsigned char info[32] = { 'R', 'A', 'W', 'D' };
        put_u32(info + 4, e->id);
        info[8] = RRES_COMP_NONE;         // compType
        info[9] = e->cipherType;          // cipherType
        put_u16(info + 10, 0);            // flags
        put_u32(info + 12, e->packedSize);
        put_u32(info + 16, e->baseSize);
        put_u32(info + 20, 0);            // nextOffset
        put_u32(info + 24, 0);            // reserved
        put_u32(info + 28, rresComputeCRC32(e->packed, (int)e->packedSize));
        shortWrites += fwrite(info, 1, 32, out) != 32;
        shortWrites += fwrite(e->packed, 1, e->packedSize, out) != e->packedSize;
    }

    // Central directory chunk (never encrypted)
    {
        unsigned char info[32] = { 'C', 'D', 'I', 'R' };
        put_u32(info + 4, 0);                       // id
        info[8] = RRES_COMP_NONE;
        info[9] = RRES_CIPHER_NONE;
        put_u16(info + 10, 0);
        put_u32(info + 12, cdBaseSize);             // packedSize
        put_u32(info + 16, cdBaseSize);             // baseSize
        put_u32(info + 20, 0);
        put_u32(info + 24, 0);
        put_u32(info + 28, rresComputeCRC32(cdData, (int)cdBaseSize));
        shortWrites += fwrite(info, 1, 32, out) != 32;
        shortWrites += fwrite(cdData, 1, cdBaseSize, out) != cdBaseSize;
    }

    if (fclose(out) != 0 || shortWrites > 0) fail("cannot write all of", outPath);
    printf("Packed %d resource(s) into %s (%s)\n", fileCount, outPath,
           encrypt ? "AES-256-CTR encrypted" : "unencrypted");

    for (int i = 0; i < fileCount; i++) free(entries[i].packed);
    free(entries);
    free(cdData);
    return 0;
}
