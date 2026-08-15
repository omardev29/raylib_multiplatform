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

// tiny-AES-c (CTR + AES256 are its defaults) and monocypher (Argon2i)
#include "../thirdparty/rres/external/aes.h"
#include "../thirdparty/rres/external/aes.c"
#include "../thirdparty/rres/external/monocypher.h"
#include "../thirdparty/rres/external/monocypher.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Read a whole file, or fail. Every step is checked, which is not paranoia:
// glibc lets fopen() succeed on a *directory*, and the loose version of this
// function then packed one — malloc'd buffer, fread() reads nothing, and the
// uninitialised heap behind it went into the .rres as a resource. Where ftell()
// returns -1 for the directory instead, the (unsigned) size became 4294967295
// and the chunk arithmetic downstream overflowed.
static unsigned char *read_file(const char *path, unsigned int *outSize) {
    *outSize = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)calloc(sz > 0 ? (size_t)sz : 1, 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (unsigned int)sz;
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
    const char *password = argv[2];
    int encrypt = (strcmp(password, "-") != 0);
    int fileCount = argc - 3;
    const char **files = (const char **)&argv[3];

    Entry *entries = (Entry *)calloc(fileCount, sizeof(Entry));

    // Build each resource chunk
    for (int i = 0; i < fileCount; i++) {
        const char *path = files[i];
        const char *name = base_name(path);

        unsigned int fileSize = 0;
        unsigned char *fileData = read_file(path, &fileSize);
        if (!fileData) { fprintf(stderr, "ERROR: cannot read %s\n", path); return 1; }

        // Raw chunk plaintext: propCount(4) + props[4](16) + raw(fileSize)
        unsigned int baseSize = 4 + 16 + fileSize;
        unsigned char *plain = (unsigned char *)calloc(baseSize, 1);
        put_u32(plain + 0, 4);                       // propCount
        put_u32(plain + 4, fileSize);                // props[0] = size
        const char *dot = strrchr(name, '.');
        const char *ext = dot ? dot : "";
        put_u32(plain + 8, pack_ext_be(ext, 0));     // props[1] = ext part 1
        put_u32(plain + 12, pack_ext_be(ext, 4));    // props[2] = ext part 2
        put_u32(plain + 16, 0);                      // props[3] = reserved
        if (fileSize > 0) memcpy(plain + 20, fileData, fileSize);

        Entry *e = &entries[i];
        e->id = rresComputeCRC32((const unsigned char *)name, (int)strlen(name));
        e->name = name;
        e->baseSize = baseSize;
        e->cipherType = encrypt ? RRES_CIPHER_AES : RRES_CIPHER_NONE;

        if (encrypt) {
            // salt[16]
            unsigned char salt[16];
            FILE *rnd = fopen("/dev/urandom", "rb");
            if (rnd) { fread(salt, 1, 16, rnd); fclose(rnd); }
            else { for (int b = 0; b < 16; b++) salt[b] = (unsigned char)(rand() & 0xff); }

            // key = Argon2i(password, salt)
            uint8_t key[32] = {0};
            crypto_argon2_config config = {
                .algorithm = CRYPTO_ARGON2_I,
                .nb_blocks = 16384,   // 16 MiB
                .nb_passes = 3,
                .nb_lanes  = 1
            };
            crypto_argon2_inputs inputs = {
                .pass = (const uint8_t *)password,
                .salt = salt,
                .pass_size = (uint32_t)strlen(password),
                .salt_size = 16
            };
            crypto_argon2_extras extras = {0};
            void *work = malloc(config.nb_blocks * 1024);
            crypto_argon2(key, 32, work, config, inputs, extras);
            free(work);

            // AES-256-CTR encrypt the plaintext (zero IV)
            unsigned char *cipher = (unsigned char *)malloc(baseSize);
            memcpy(cipher, plain, baseSize);
            struct AES_ctx ctx = {0};
            AES_init_ctx(&ctx, key);                 // leaves CTR counter (Iv) zeroed
            AES_CTR_xcrypt_buffer(&ctx, cipher, baseSize);
            crypto_wipe(key, 32);

            // MD5 integrity over the plaintext
            unsigned int *md5 = ComputeMD5(plain, baseSize);

            // packed = cipher || salt[16] || MD5[16]
            e->packedSize = baseSize + 32;
            e->packed = (unsigned char *)malloc(e->packedSize);
            memcpy(e->packed, cipher, baseSize);
            memcpy(e->packed + baseSize, salt, 16);
            memcpy(e->packed + baseSize + 16, md5, 16);
            free(cipher);
        } else {
            e->packedSize = baseSize;
            e->packed = (unsigned char *)malloc(baseSize);
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
    unsigned char *cdData = (unsigned char *)calloc(cdBaseSize, 1);
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

    // Write the file
    FILE *out = fopen(outPath, "wb");
    if (!out) { fprintf(stderr, "ERROR: cannot open %s for writing\n", outPath); return 1; }

    // Header: id, version=100, chunkCount (data chunks + CD), cdOffset (relative
    // to end of header, because the loader does fseek(cdOffset, SEEK_CUR) from 16)
    unsigned char header[16];
    memcpy(header, "rres", 4);
    put_u16(header + 4, 100);
    put_u16(header + 6, (unsigned short)(fileCount + 1));
    put_u32(header + 8, cdAbsOffset - 16);
    put_u32(header + 12, 0);
    fwrite(header, 1, 16, out);

    // Data chunks
    for (int i = 0; i < fileCount; i++) {
        Entry *e = &entries[i];
        unsigned char info[32];
        memcpy(info, "RAWD", 4);
        put_u32(info + 4, e->id);
        info[8] = RRES_COMP_NONE;         // compType
        info[9] = e->cipherType;          // cipherType
        put_u16(info + 10, 0);            // flags
        put_u32(info + 12, e->packedSize);
        put_u32(info + 16, e->baseSize);
        put_u32(info + 20, 0);            // nextOffset
        put_u32(info + 24, 0);            // reserved
        put_u32(info + 28, rresComputeCRC32(e->packed, (int)e->packedSize));
        fwrite(info, 1, 32, out);
        fwrite(e->packed, 1, e->packedSize, out);
    }

    // Central directory chunk (never encrypted)
    {
        unsigned char info[32];
        memcpy(info, "CDIR", 4);
        put_u32(info + 4, 0);                       // id
        info[8] = RRES_COMP_NONE;
        info[9] = RRES_CIPHER_NONE;
        put_u16(info + 10, 0);
        put_u32(info + 12, cdBaseSize);             // packedSize
        put_u32(info + 16, cdBaseSize);             // baseSize
        put_u32(info + 20, 0);
        put_u32(info + 24, 0);
        put_u32(info + 28, rresComputeCRC32(cdData, (int)cdBaseSize));
        fwrite(info, 1, 32, out);
        fwrite(cdData, 1, cdBaseSize, out);
    }

    fclose(out);
    printf("Packed %d resource(s) into %s (%s)\n", fileCount, outPath,
           encrypt ? "AES-256-CTR encrypted" : "unencrypted");

    for (int i = 0; i < fileCount; i++) free(entries[i].packed);
    free(entries);
    free(cdData);
    return 0;
}
