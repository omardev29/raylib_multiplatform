#pragma once
// ---------------------------------------------------------------------------
// An rres pack, written byte for byte the way tools/rres_pack.c writes one --
// and the walk a fuzz target edits one by. The tests' own copy of the format,
// written from the packer and not from src/rmp/pack.cpp, so the reader is
// checked against the writer and not against itself:
//
//   header (16)       "rres", version 100, chunk count, directory offset
//                     (from the END of the header: rres seeks from byte 16), 0
//   per file          info (32): "RAWD", CRC32(name), comp, cipher, flags,
//                     packed size, base size, next offset, 0, CRC32(packed)
//                     packed: count 4, [size, ext[0..3], ext[4..7], 0], bytes
//                     -- AES: AES-CTR(that) || salt[16] || MD5(that)[16]
//   the directory     info "CDIR", id 0; count 1, [entries], and per entry
//                     id, offset, 0, name size (NUL and padding to 4), name
//
// Little-endian, like every machine the framework ships to and like the
// packer, which writes byte by byte.
//
// The key is stretched by whatever `stretch` the caller hands in: Argon2i
// with rres's parameters is what a pack really carries, and it costs two
// seconds a chunk under ASan at -O0 -- so a test of the format uses a cheap
// one, and the one test that proves the real thing pays for it once.
// ---------------------------------------------------------------------------

#include <raylib.h> // ComputeMD5

#include <external/aes.h>
#include <external/monocypher.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::pack {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
using Salt = std::array<std::uint8_t, 16>;
using Stretch = Key (*)(std::string_view password, const Salt &salt);

inline constexpr std::size_t HEADER_SIZE = 16;
inline constexpr std::size_t INFO_SIZE = 32;
inline constexpr std::uint8_t CIPHER_NONE = 0;
inline constexpr std::uint8_t CIPHER_AES = 30;
inline constexpr std::uint8_t CIPHER_XCHACHA20_POLY1305 = 73;

// rres's own key: Argon2i, 16 MiB, three passes, one lane, 32 bytes.
inline Key argon2i(std::string_view password, const Salt &salt) {
    Key key{};
    const crypto_argon2_config config = {
        .algorithm = CRYPTO_ARGON2_I, .nb_blocks = 16384, .nb_passes = 3, .nb_lanes = 1
    };
    const crypto_argon2_inputs inputs = {
        .pass = reinterpret_cast<const std::uint8_t *>(password.data()),
        .salt = salt.data(),
        .pass_size = static_cast<std::uint32_t>(password.size()),
        .salt_size = static_cast<std::uint32_t>(salt.size()),
    };
    std::vector<std::uint64_t> work(std::size_t{ 16384 } * 1024 / sizeof(std::uint64_t));
    crypto_argon2(key.data(), static_cast<std::uint32_t>(key.size()), work.data(), config,
                  inputs, crypto_argon2_no_extras);
    return key;
}

// A key that costs nothing: what the format's tests and the fuzz targets
// stretch with, on both sides.
inline Key cheap(std::string_view password, const Salt &salt) {
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const auto from_password = password.empty()
            ? 0
            : static_cast<unsigned char>(password[i % password.size()]);
        key[i] =
            static_cast<std::uint8_t>(from_password ^ salt[i % salt.size()] ^ (i * 37));
    }
    return key;
}

inline void put_u32(Bytes &out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFU));
}
inline void put_u16(Bytes &out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}
inline std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(bytes[at + i]) << (8 * i);
    return v;
}
inline std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}
inline void set_u32(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
}

// rres's CRC32 is zlib's: the standard table, an inverted start and end.
inline std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
inline std::uint32_t crc32(std::string_view text) {
    return crc32(
        std::span(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

// The packer's extension: up to eight characters from the last dot, as two
// big-endian words.
inline std::uint32_t extension_word(std::string_view name, std::size_t start) {
    const std::size_t dot = name.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? "" : name.substr(dot);
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t at = start + i;
        v = (v << 8) | (at < ext.size() ? static_cast<unsigned char>(ext[at]) : 0U);
    }
    return v;
}

struct File {
    std::string name;
    Bytes bytes;
    std::uint8_t cipher = CIPHER_NONE;
};

// RapidCheck finds it by its name, in the File's namespace.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const File &file, std::ostream &os) {
    os << "{" << file.name << ", " << file.bytes.size() << " bytes, cipher "
       << static_cast<int>(file.cipher) << "}";
}

struct Options {
    std::string_view password = "raylib-template";
    Stretch stretch = cheap;
    std::uint8_t salt_seed =
        1; // the packer reads /dev/urandom; a test wants the same bytes
};

// AES-CTR with a zero IV, as tiny-AES leaves it, over `text` in place.
inline void aes_ctr(std::span<std::uint8_t> text, const Key &key) {
    AES_ctx ctx{};
    AES_init_ctx(&ctx, key.data());
    AES_CTR_xcrypt_buffer(&ctx, text.data(), text.size());
}

inline std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> text) {
    std::array<std::uint8_t, 16> out{};
    // raylib's MD5 takes a pointer to mutable bytes, and only reads them.
    std::memcpy(out.data(),
                ComputeMD5(const_cast<unsigned char *>(text.data()),
                           static_cast<int>(text.size())),
                out.size());
    return out;
}

// A chunk's plaintext: its property count, its properties, then its data.
inline Bytes plain(const std::vector<std::uint32_t> &props,
                   std::span<const std::uint8_t> data) {
    Bytes out;
    put_u32(out, static_cast<std::uint32_t>(props.size()));
    for (const std::uint32_t prop : props) put_u32(out, prop);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// `text` encrypted the way rres_pack and rrespacker do it: AES-CTR, then the
// salt and the MD5 of the plaintext; or XChaCha20-Poly1305, then the salt,
// the nonce and the MAC.
inline Bytes encrypt(Bytes text, std::uint8_t cipher, const Options &options,
                     std::uint8_t salt_byte) {
    Salt salt{};
    for (std::size_t i = 0; i < salt.size(); ++i)
        salt[i] = static_cast<std::uint8_t>(salt_byte + i * 7);
    const Key key = options.stretch(options.password, salt);
    if (cipher == CIPHER_AES) {
        const auto sum = md5(text);
        aes_ctr(text, key);
        text.insert(text.end(), salt.begin(), salt.end());
        text.insert(text.end(), sum.begin(), sum.end());
        return text;
    }
    std::array<std::uint8_t, 24> nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i)
        nonce[i] = static_cast<std::uint8_t>(std::size_t{ salt_byte } * 3 + i);
    std::array<std::uint8_t, 16> mac{};
    Bytes out(text.size());
    crypto_aead_lock(out.data(), mac.data(), key.data(), nonce.data(), nullptr, 0,
                     text.data(), text.size());
    out.insert(out.end(), salt.begin(), salt.end());
    out.insert(out.end(), nonce.begin(), nonce.end());
    out.insert(out.end(), mac.begin(), mac.end());
    return out;
}

inline void put_info(Bytes &out, std::string_view type, std::uint32_t id,
                     std::uint8_t compression, std::uint8_t cipher,
                     std::span<const std::uint8_t> data, std::uint32_t base_size) {
    for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
    put_u32(out, id);
    out.push_back(compression);
    out.push_back(cipher);
    put_u16(out, 0); // flags
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    put_u32(out, base_size);
    put_u32(out, 0); // next offset
    put_u32(out, 0); // reserved
    put_u32(out, crc32(data));
}

// One chunk as it goes into the file: already packed, so a test can put in
// anything -- another type, a compression, a size that lies.
struct Chunk {
    std::string name;
    std::string type = "RAWD";
    std::uint8_t compression = 0;
    std::uint8_t cipher = CIPHER_NONE;
    Bytes data;
    std::uint32_t base_size = 0;
};

// The header, the chunks in order, and the directory naming each one.
inline Bytes write_chunks(const std::vector<Chunk> &chunks) {
    Bytes directory;
    put_u32(directory, 1);
    put_u32(directory, static_cast<std::uint32_t>(chunks.size()));
    auto offset = static_cast<std::uint32_t>(HEADER_SIZE);
    for (const Chunk &chunk : chunks) {
        const std::string &name = chunk.name;
        const auto name_size =
            static_cast<std::uint32_t>((name.size() + 1 + 3) & ~std::size_t{ 3 });
        put_u32(directory, crc32(name));
        put_u32(directory, offset);
        put_u32(directory, 0);
        put_u32(directory, name_size);
        directory.insert(directory.end(), name.begin(), name.end());
        directory.resize(directory.size() + (name_size - name.size()), 0);
        offset += static_cast<std::uint32_t>(INFO_SIZE + chunk.data.size());
    }

    Bytes out;
    for (const char c : std::string_view("rres"))
        out.push_back(static_cast<std::uint8_t>(c));
    put_u16(out, 100);
    put_u16(out, static_cast<std::uint16_t>(chunks.size() + 1));
    put_u32(out, offset - static_cast<std::uint32_t>(HEADER_SIZE));
    put_u32(out, 0);
    for (const Chunk &chunk : chunks) {
        put_info(out, chunk.type, crc32(chunk.name), chunk.compression, chunk.cipher,
                 chunk.data, chunk.base_size);
        out.insert(out.end(), chunk.data.begin(), chunk.data.end());
    }
    put_info(out, "CDIR", 0, 0, CIPHER_NONE, directory,
             static_cast<std::uint32_t>(directory.size()));
    out.insert(out.end(), directory.begin(), directory.end());
    return out;
}

// The whole pack, as rres_pack writes it for these files: each a RAWD chunk
// whose properties are its size and its extension.
inline Bytes write(const std::vector<File> &files, const Options &options = {}) {
    std::vector<Chunk> chunks;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const File &file = files[i];
        Bytes text =
            plain({ static_cast<std::uint32_t>(file.bytes.size()),
                    extension_word(file.name, 0), extension_word(file.name, 4), 0 },
                  file.bytes);
        const auto base_size = static_cast<std::uint32_t>(text.size());
        if (file.cipher != CIPHER_NONE) {
            text = encrypt(std::move(text), file.cipher, options,
                           static_cast<std::uint8_t>(options.salt_seed + i * 16));
        }
        chunks.push_back(
            { file.name, "RAWD", 0, file.cipher, std::move(text), base_size });
    }
    return write_chunks(chunks);
}

// Where a chunk is in a pack: its info, and its packed bytes after it. What a
// fuzz target walks to edit a pack the way a corrupted one would not be.
struct Located {
    std::size_t info = 0;
    std::size_t data = 0;
    std::size_t size = 0;
};

// The chunks a reader meets walking from the header, and the directory where
// the header says it is -- as far as each lies inside `bytes`.
inline std::vector<Located> chunks(std::span<const std::uint8_t> bytes) {
    std::vector<Located> out;
    if (bytes.size() < HEADER_SIZE) return out;
    const std::size_t count = get_u16(bytes, 6);
    std::size_t at = HEADER_SIZE;
    for (std::size_t i = 0; i < count && at + INFO_SIZE <= bytes.size(); ++i) {
        const std::size_t size = get_u32(bytes, at + 12);
        if (size > bytes.size() - at - INFO_SIZE) break;
        out.push_back({ at, at + INFO_SIZE, size });
        at += INFO_SIZE + size;
    }
    const std::size_t directory = HEADER_SIZE + std::size_t{ get_u32(bytes, 8) };
    if (directory >= HEADER_SIZE && directory + INFO_SIZE <= bytes.size()) {
        const std::size_t size = get_u32(bytes, directory + 12);
        if (size <= bytes.size() - directory - INFO_SIZE)
            out.push_back({ directory, directory + INFO_SIZE, size });
    }
    return out;
}

// Every chunk's CRC32 made right, so a fuzzer's edit gets past the check that
// a random one never would.
inline void fix_crcs(std::span<std::uint8_t> bytes) {
    for (const Located &chunk : chunks(bytes))
        set_u32(bytes, chunk.info + 28, crc32(bytes.subspan(chunk.data, chunk.size)));
}

inline bool save(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

inline Bytes bytes_of(std::string_view text) { return { text.begin(), text.end() }; }

} // namespace rmp::test::pack
