// ===========================================================================
// The resource pack: finding it, opening it, reading one entry out of it.
//
// A release ships a single resources.rres — the container rres defines, with
// every file in resources/ inside it, AES-encrypted. Development ships the
// loose files instead. Everything above this file is written so that the
// difference never shows.
//
// THE PACK IS READ HERE, NOT BY RRES. rres's own readers (rres.h and
// rres-raylib.h, pinned unmodified) believe every field of the file: a
// directory with no properties was a null dereference, a name longer than
// rres's 1024-byte entry a heap overflow WRITE, a property count past its
// chunk, an encrypted chunk with a bad CRC or with fewer bytes than its own
// salt and MD5 a read through null or a 4 GiB allocation, and a chunk that did
// not decrypt -- a pack made with another password -- leaked what rres
// decrypted it into, on every load. Its lookup compared a PREFIX: asked for
// "music" it answered with "music.ogg", and asked for "" with the first file
// in the pack. tests/fuzz/regressions/rres/ holds one pack of each, and
// tests/fuzz/rres.cpp found most of them in its first minute.
//
// So every field is checked against the bytes there are before it is used
// (src/rmp/pack_internal.h), and from rres this file takes its format -- the
// enums -- and the cipher it packs with: tiny-AES, monocypher and raylib's
// MD5, compiled in rres_impl.cpp. A pack is used whole or not at all: one cut
// short or with a corrupt directory is refused at open_pack(), and the game
// reads the loose files. A chunk that does not open is refused alone, and its
// loader falls back to the loose file. What this reads is what rres_pack
// writes (RAWD, AES), and what rrespacker writes besides as far as rres
// itself read it safely: IMGE pictures and XChaCha20-Poly1305. A LINK chunk
// is refused -- it names a file outside the pack, by a path inside it -- and
// so is every compressed one: DEFLATE (raylib's inflater has undefined
// behaviour on a corrupt stream, see open_chunk), LZ4, which rres was never
// built with, and QOI, which rres read the properties of out of the pixels.
// ===========================================================================

#include <raylib.h>
#include "rres.h" // the format's enums and its password; nothing that reads a file

#include <external/aes.h>
#include <external/monocypher.h>

#include "internal.h"
#include "pack_internal.h"
#include <rmp/config.h> // RMP_RRES_PASSWORD

#include <algorithm>
#include <climits>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

// The pack's password is RMP_RRES_PASSWORD, [resources] rres_password in the
// .toml, and the packer in CMakeLists.txt encrypts with the same value.
// Obfuscation, not real security: it ends up in the binary.
//
// It used to be hard-coded here AND in CMakeLists.txt, and only the desktop
// build passed its copy as a -D, so Android and iOS silently used this one --
// change one and those two platforms could no longer read their own asset
// pack, with no error that pointed at the cause. One value, from the config.

namespace rmp::assets::detail::rres {

namespace {

constexpr std::uint64_t HEADER_SIZE = 16;
constexpr std::uint64_t INFO_SIZE = 32;
constexpr std::uint16_t VERSION = 100;
// A chunk's CRC and MD5 take an int: a chunk past 2 GiB is not one this reads.
constexpr std::uint64_t CHUNK_LIMIT = INT_MAX;
// After the encrypted bytes: AES's salt and MD5, XChaCha20's salt, nonce, MAC.
constexpr std::size_t AES_TRAILER = 16 + 16;
constexpr std::size_t XCHACHA_TRAILER = 16 + 24 + 16;
// rres lays out an IMGE's pixel count no larger than what GetPixelDataSize
// can compute in an int at 16 bytes a pixel.
constexpr std::uint64_t PIXEL_LIMIT = INT_MAX / 16;

// What separates the folders of a path: '/' everywhere, and '\' too where the
// system says so. On Linux "art\x.png" is one file with a backslash in its
// name, which a development build does not find in art/ -- and the pack did,
// until tests/property/assets_prop.cpp compared the two.
#if defined(_WIN32)
constexpr std::string_view SEPARATORS = "/\\";
#else
constexpr std::string_view SEPARATORS = "/";
#endif

std::uint32_t u32_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
        (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint16_t u16_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

bool is_type(const std::array<char, 4> &type, std::string_view four) {
    return std::string_view(type.data(), type.size()) == four;
}

std::optional<ChunkInfo> read_info(Source &pack, std::uint64_t at) {
    std::array<std::uint8_t, INFO_SIZE> bytes{};
    if (pack.size() < at || pack.size() - at < INFO_SIZE || !pack.read(at, bytes)) {
        return std::nullopt;
    }
    ChunkInfo info;
    std::copy_n(bytes.begin(), 4, info.type.begin());
    info.id = u32_at(bytes, 4);
    info.compression = bytes[8];
    info.cipher = bytes[9];
    info.packed_size = u32_at(bytes, 12);
    info.base_size = u32_at(bytes, 16);
    info.crc = u32_at(bytes, 28);
    // Its bytes have to be there, and small enough to check.
    if (info.packed_size > CHUNK_LIMIT || info.base_size > CHUNK_LIMIT ||
        pack.size() - at - INFO_SIZE < info.packed_size) {
        return std::nullopt;
    }
    return info;
}

// The directory: a count, then per entry its id, its offset, a reserved word,
// the size of its name (with the NUL and the padding to 4) and the name.
std::optional<std::vector<Entry>> entries_of(const Contents &directory) {
    if (directory.props.empty()) return std::nullopt;
    const std::span<const std::uint8_t> data = directory.data;
    const std::uint32_t count = directory.props[0];
    std::vector<Entry> out;
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (data.size() - at < 16) return std::nullopt;
        const std::uint32_t id = u32_at(data, at);
        const std::uint32_t offset = u32_at(data, at + 4);
        const std::uint32_t name_size = u32_at(data, at + 12);
        at += 16;
        if (data.size() - at < name_size) return std::nullopt;
        const std::span<const std::uint8_t> name = data.subspan(at, name_size);
        // NUL-terminated and not empty, or not a name: "" would be the file
        // every load_data("") found, where the loose files have none.
        const auto end = std::ranges::find(name, std::uint8_t{ 0 });
        if (end == name.end() || end == name.begin()) return std::nullopt;
        out.push_back({ std::string(name.begin(), end), id, offset });
        at += name_size;
    }
    return out;
}

bool unpack_aes(Bytes &packed, std::string_view password, Stretch stretch) {
    if (packed.size() < AES_TRAILER) return false;
    const std::size_t body = packed.size() - AES_TRAILER;
    Salt salt{};
    std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(body), salt.size(),
                salt.begin());
    std::array<std::uint8_t, 16> sum{};
    std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(body + salt.size()),
                sum.size(), sum.begin());
    packed.resize(body);

    Key key = stretch(password, salt);
    AES_ctx ctx{}; // a zero counter, as rres and the packer leave it
    AES_init_ctx(&ctx, key.data());
    AES_CTR_xcrypt_buffer(&ctx, packed.data(), packed.size());
    crypto_wipe(key.data(), key.size());
    crypto_wipe(&ctx, sizeof(ctx));

    // The MD5 of what was encrypted is the only way to tell a wrong password.
    return std::memcmp(ComputeMD5(packed.data(), static_cast<int>(packed.size())),
                       sum.data(), sum.size()) == 0;
}

bool unpack_xchacha(Bytes &packed, std::string_view password, Stretch stretch) {
    if (packed.size() < XCHACHA_TRAILER) return false;
    const std::size_t body = packed.size() - XCHACHA_TRAILER;
    const auto trailer = packed.begin() + static_cast<std::ptrdiff_t>(body);
    Salt salt{};
    std::array<std::uint8_t, 24> nonce{};
    std::array<std::uint8_t, 16> mac{};
    std::copy_n(trailer, salt.size(), salt.begin());
    std::copy_n(trailer + 16, nonce.size(), nonce.begin());
    std::copy_n(trailer + 16 + 24, mac.size(), mac.begin());

    Key key = stretch(password, salt);
    Bytes plain(body);
    const int refused = crypto_aead_unlock(plain.data(), mac.data(), key.data(),
                                           nonce.data(), nullptr, 0, packed.data(), body);
    crypto_wipe(key.data(), key.size());
    if (refused != 0) return false;
    packed = std::move(plain);
    return true;
}

// What raylib allocates, given back with MemFree() whichever way this leaves.
using RaylibBytes = std::unique_ptr<unsigned char, decltype(&MemFree)>;

// rres's pixel formats, by name: raylib 6 added formats in the middle of its
// enum, so the numbers no longer agree.
int pixel_format(std::uint32_t format) {
    switch (format) {
        case RRES_PIXELFORMAT_UNCOMP_GRAYSCALE:
            return PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
        case RRES_PIXELFORMAT_UNCOMP_GRAY_ALPHA:
            return PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
        case RRES_PIXELFORMAT_UNCOMP_R5G6B5:
            return PIXELFORMAT_UNCOMPRESSED_R5G6B5;
        case RRES_PIXELFORMAT_UNCOMP_R8G8B8:
            return PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        case RRES_PIXELFORMAT_UNCOMP_R5G5B5A1:
            return PIXELFORMAT_UNCOMPRESSED_R5G5B5A1;
        case RRES_PIXELFORMAT_UNCOMP_R4G4B4A4:
            return PIXELFORMAT_UNCOMPRESSED_R4G4B4A4;
        case RRES_PIXELFORMAT_UNCOMP_R8G8B8A8:
            return PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        case RRES_PIXELFORMAT_UNCOMP_R32:
            return PIXELFORMAT_UNCOMPRESSED_R32;
        case RRES_PIXELFORMAT_UNCOMP_R32G32B32:
            return PIXELFORMAT_UNCOMPRESSED_R32G32B32;
        case RRES_PIXELFORMAT_UNCOMP_R32G32B32A32:
            return PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
        case RRES_PIXELFORMAT_COMP_DXT1_RGB:
            return PIXELFORMAT_COMPRESSED_DXT1_RGB;
        case RRES_PIXELFORMAT_COMP_DXT1_RGBA:
            return PIXELFORMAT_COMPRESSED_DXT1_RGBA;
        case RRES_PIXELFORMAT_COMP_DXT3_RGBA:
            return PIXELFORMAT_COMPRESSED_DXT3_RGBA;
        case RRES_PIXELFORMAT_COMP_DXT5_RGBA:
            return PIXELFORMAT_COMPRESSED_DXT5_RGBA;
        case RRES_PIXELFORMAT_COMP_ETC1_RGB:
            return PIXELFORMAT_COMPRESSED_ETC1_RGB;
        case RRES_PIXELFORMAT_COMP_ETC2_RGB:
            return PIXELFORMAT_COMPRESSED_ETC2_RGB;
        case RRES_PIXELFORMAT_COMP_ETC2_EAC_RGBA:
            return PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA;
        case RRES_PIXELFORMAT_COMP_PVRT_RGB:
            return PIXELFORMAT_COMPRESSED_PVRT_RGB;
        case RRES_PIXELFORMAT_COMP_PVRT_RGBA:
            return PIXELFORMAT_COMPRESSED_PVRT_RGBA;
        case RRES_PIXELFORMAT_COMP_ASTC_4x4_RGBA:
            return PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA;
        case RRES_PIXELFORMAT_COMP_ASTC_8x8_RGBA:
            return PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA;
        default:
            return 0;
    }
}

// Pixels raylib owns: UnloadImage() frees them with RL_FREE, which is what
// MemAlloc() pairs with.
::Image raylib_image(std::span<const std::uint8_t> pixels, int width, int height,
                     int format) {
    ::Image image{};
    image.data = MemAlloc(static_cast<unsigned int>(pixels.size()));
    if (image.data == nullptr) return image;
    std::memcpy(image.data, pixels.data(), pixels.size());
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = format;
    return image;
}

} // namespace

std::vector<std::string_view> segments(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t end = path.find_first_of(SEPARATORS, at);
        const std::size_t stop = end == std::string_view::npos ? path.size() : end;
        const std::string_view part = path.substr(at, stop - at);
        const bool absolute = part.empty() && at == 0 && end == 0; // a leading separator
        if (part == "..") {
            const bool folder = !out.empty() && out.back() != ".." && !out.back().empty();
            const bool root = !out.empty() && out.back().empty();
            if (folder) {
                out.pop_back(); // a folder, and back out of it
            } else if (!root) {
                out.push_back(part); // above where a relative path starts
            } // and above the root of an absolute one is the root
        } else if (absolute || (!part.empty() && part != ".")) {
            out.push_back(part);
        }
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    return out;
}

std::optional<std::string> packed_name(std::string_view root, std::string_view path) {
    // A path whose last part is a separator, "." or ".." names a folder,
    // whatever its segments come to.
    const std::size_t last = path.find_last_of(SEPARATORS);
    const std::string_view tail =
        last == std::string_view::npos ? path : path.substr(last + 1);
    if (tail.empty() || tail == "." || tail == "..") return std::nullopt;
    const std::vector<std::string_view> folder = segments(root);
    const std::vector<std::string_view> file = segments(path);
    if (file.size() != folder.size() + 1 ||
        !std::equal(folder.begin(), folder.end(), file.begin()))
        return std::nullopt;
    return std::string(file.back());
}

bool MemorySource::read(std::uint64_t offset, std::span<std::uint8_t> out) {
    if (offset > _bytes.size() || _bytes.size() - offset < out.size()) return false;
    std::copy_n(_bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(),
                out.begin());
    return true;
}

Key argon2i(std::string_view password, const Salt &salt) {
    Key key{};
    constexpr std::uint32_t BLOCKS = 16384; // 16 MiB of 1 KiB blocks
    const crypto_argon2_config config = {
        .algorithm = CRYPTO_ARGON2_I, .nb_blocks = BLOCKS, .nb_passes = 3, .nb_lanes = 1
    };
    const crypto_argon2_inputs inputs = {
        .pass = reinterpret_cast<const std::uint8_t *>(password.data()),
        .salt = salt.data(),
        .pass_size = static_cast<std::uint32_t>(password.size()),
        .salt_size = static_cast<std::uint32_t>(salt.size()),
    };
    // 8-byte words: monocypher's work area is an array of them.
    std::vector<std::uint64_t> work(std::size_t{ BLOCKS } * 128);
    crypto_argon2(key.data(), static_cast<std::uint32_t>(key.size()), work.data(), config,
                  inputs, crypto_argon2_no_extras);
    return key;
}

std::optional<Index> read_index(Source &pack) {
    std::array<std::uint8_t, HEADER_SIZE> header{};
    if (pack.size() < HEADER_SIZE || !pack.read(0, header)) return std::nullopt;
    if (!std::equal(header.begin(), header.begin() + 4, "rres") ||
        u16_at(header, 4) != VERSION) {
        return std::nullopt;
    }
    Index index;
    std::uint64_t at = HEADER_SIZE;
    for (std::uint16_t i = 0, count = u16_at(header, 6); i < count; ++i) {
        const std::optional<ChunkInfo> info = read_info(pack, at);
        if (!info) return std::nullopt; // the table runs past the end of the file
        index.chunks.push_back({ at, at + INFO_SIZE, *info });
        at += INFO_SIZE + info->packed_size;
    }

    // rres seeks to the directory from the end of the header.
    const std::uint64_t where = HEADER_SIZE + u32_at(header, 8);
    if (u32_at(header, 8) == 0) return std::nullopt; // no directory, no names
    const std::optional<ChunkInfo> info = read_info(pack, where);
    if (!info || !is_type(info->type, "CDIR") || info->compression != RRES_COMP_NONE ||
        info->cipher != RRES_CIPHER_NONE) {
        return std::
            nullopt; // a directory that needed a password would need it to list names
    }
    const std::optional<Contents> directory =
        open_chunk(pack, { where, where + INFO_SIZE, *info }, "", nullptr);
    if (!directory) return std::nullopt;
    std::optional<std::vector<Entry>> entries = entries_of(*directory);
    if (!entries) return std::nullopt;
    index.entries = std::move(*entries);
    return index;
}

std::optional<Chunk> find(const Index &index, std::string_view name) {
    const auto named = std::ranges::find(index.entries, name, &Entry::name);
    if (named == index.entries.end()) return std::nullopt;
    const auto chunk =
        std::ranges::find(index.chunks, std::uint64_t{ named->offset }, &Chunk::at);
    if (chunk == index.chunks.end() || chunk->info.id != named->id) return std::nullopt;
    return *chunk;
}

std::optional<Contents> open_chunk(Source &pack, const Chunk &chunk,
                                   std::string_view password, Stretch stretch) {
    const ChunkInfo &info = chunk.info;
    if (info.packed_size > CHUNK_LIMIT || info.base_size > CHUNK_LIMIT)
        return std::nullopt;
    Bytes bytes(info.packed_size);
    if (!pack.read(chunk.data, bytes)) return std::nullopt;
    if (rresComputeCRC32(bytes.data(), static_cast<int>(bytes.size())) != info.crc) {
        return std::nullopt;
    }

    switch (info.cipher) {
        case RRES_CIPHER_NONE:
            break;
        case RRES_CIPHER_AES:
            if (stretch == nullptr || !unpack_aes(bytes, password, stretch))
                return std::nullopt;
            break;
        case RRES_CIPHER_XCHACHA20_POLY1305:
            if (stretch == nullptr || !unpack_xchacha(bytes, password, stretch)) {
                return std::nullopt;
            }
            break;
        default:
            return std::nullopt;
    }
    // Nothing compressed: rres_pack never compresses, and the one inflater
    // there is -- raylib's DecompressData(), sinfl -- shifts past its word and
    // indexes past its table on a corrupt stream (undefined behaviour,
    // tests/fuzz/regressions/rres/deflate_*). A DEFLATE chunk falls back to
    // the loose file, as LZ4 and QOI do.
    if (info.compression != RRES_COMP_NONE) return std::nullopt;
    // What came out is what the info says went in.
    if (bytes.size() != info.base_size || bytes.size() < 4) return std::nullopt;

    const std::span<const std::uint8_t> plain = bytes;
    const std::uint64_t count = u32_at(plain, 0);
    if ((plain.size() - 4) / 4 < count) return std::nullopt;
    Contents out;
    out.type = info.type;
    out.props.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.props.push_back(u32_at(plain, 4 + 4 * i));
    const auto data = plain.subspan(4 + 4 * count);
    out.data.assign(data.begin(), data.end());
    return out;
}

std::optional<Bytes> file_of(const Contents &contents) {
    if (!is_type(contents.type, "RAWD") || contents.props.empty()) return std::nullopt;
    const std::uint32_t size = contents.props[0];
    if (size > contents.data.size()) return std::nullopt;
    return Bytes(contents.data.begin(), contents.data.begin() + size);
}

::Image image_of(const Contents &contents) {
    if (is_type(contents.type, "IMGE")) {
        if (contents.props.size() < 4) return ::Image{};
        const std::uint32_t width = contents.props[0];
        const std::uint32_t height = contents.props[1];
        const int format = pixel_format(contents.props[2]);
        // One level: rres checks the data against the first level's size
        // alone, so a picture that claimed more would be read past its end.
        if (width == 0 || height == 0 || format == 0 || contents.props[3] != 1 ||
            std::uint64_t{ width } * height > PIXEL_LIMIT) {
            return ::Image{};
        }
        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        if (std::cmp_not_equal(GetPixelDataSize(w, h, format), contents.data.size())) {
            return ::Image{};
        }
        return raylib_image(contents.data, w, h, format);
    }
    const std::optional<Bytes> file = file_of(contents);
    if (!file || file->empty() || contents.props.size() < 3) return ::Image{};
    // The packer's extension: two big-endian words, at most eight characters.
    std::array<char, 9> ext{};
    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint32_t word = contents.props[1 + i / 4];
        ext[i] = static_cast<char>((word >> (24 - 8 * (i % 4))) & 0xFFU);
    }
    return LoadImageFromMemory(ext.data(), file->data(), static_cast<int>(file->size()));
}

} // namespace rmp::assets::detail::rres

namespace rmp::assets::detail {

namespace {

// resources.rres, read where it is, a range at a time.
class FileSource final : public rres::Source {
public:
    explicit FileSource(const std::string &path) : _file(path, std::ios::binary) {
        if (!_file) return;
        _file.seekg(0, std::ios::end);
        const std::streamoff end = _file.tellg();
        _size = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    }
    [[nodiscard]] std::uint64_t size() override { return _size; }
    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (!_file || offset > _size || _size - offset < out.size()) return false;
        _file.clear();
        _file.seekg(static_cast<std::streamoff>(offset));
        _file.read(reinterpret_cast<char *>(out.data()),
                   static_cast<std::streamsize>(out.size()));
        return _file.good() && std::cmp_equal(_file.gcount(), out.size());
    }

private:
    std::ifstream _file;
    std::uint64_t _size = 0;
};

// The pack, when there is one. The dot at every use says "file state".
struct {
    bool in_use = false;
    std::string path;
    rres::Index index;
} pack;

// The named chunk, opened -- with the password rres keeps, which open_pack()
// set and a test may change.
// The chunk a loader's name means: the file at root + name, by the rule
// packed_name() states -- the loader hook's rule, so the two cannot disagree.
std::optional<rres::Chunk> chunk_named(std::string_view name) {
    if (!pack.in_use) return std::nullopt;
    const std::string_view root = rmp::assets::detail::resources_root();
    const std::optional<std::string> entry =
        rres::packed_name(root, std::string(root) + std::string(name));
    if (!entry) return std::nullopt;
    return rres::find(pack.index, *entry);
}

std::optional<rres::Contents> open_named(std::string_view name) {
    const std::optional<rres::Chunk> chunk = chunk_named(name);
    if (!chunk) return std::nullopt;
    FileSource file(pack.path);
    return rres::open_chunk(file, *chunk, rresGetCipherPassword(), rres::argon2i);
}

} // namespace

bool open_pack() {
    if (pack.in_use) return true; // idempotent: the lifecycle macro already called it

    pack.path = std::string(rmp::assets::detail::resources_root()) + "resources.rres";

    if (!FileExists(pack.path.c_str())) {
        TraceLog(LOG_INFO, "ASSETS: No resource pack found, using loose files from %s",
                 rmp::assets::detail::resources_root());
        return false;
    }

    rresSetCipherPassword(RMP_RRES_PASSWORD);
    FileSource file(pack.path);
    std::optional<rres::Index> index = rres::read_index(file);
    // A pack is a machine's file -- cut short by a download, or not one this
    // version reads -- so this is a TraceLog, said once, and not a report.
    if (!index) {
        TraceLog(LOG_WARNING, "ASSETS: %s is cut short or corrupt, using loose files",
                 pack.path.c_str());
        return false;
    }
    if (index->entries.empty()) {
        TraceLog(LOG_WARNING,
                 "ASSETS: %s has no entries in its directory, using loose files",
                 pack.path.c_str());
        return false;
    }

    pack.index = std::move(*index);
    pack.in_use = true;
    TraceLog(LOG_INFO, "ASSETS: Using resource pack %s (%d entries)", pack.path.c_str(),
             static_cast<int>(pack.index.entries.size()));
    return true;
}

void close_pack() {
    if (!pack.in_use) return;
    pack.index = {};
    pack.in_use = false;
}

bool pack_is_open() { return pack.in_use; }

bool pack_has(const char *name) {
    // The directory alone: nothing is read, decrypted or allocated, which is
    // what makes it cheap enough to try five extensions per name.
    return name != nullptr && chunk_named(name).has_value();
}

unsigned char *pack_read(const char *name, int *size) {
    if (size != nullptr) *size = 0;
    if (name == nullptr) return nullptr;
    const std::optional<rres::Contents> contents = open_named(name);
    if (!contents) return nullptr;
    const std::optional<rres::Bytes> file = rres::file_of(*contents);
    if (!file || file->size() > static_cast<std::size_t>(INT_MAX)) return nullptr;

    // raylib's LoadFileData contract: the caller frees it with UnloadFileData,
    // which is RL_FREE, and MemAlloc is RL_CALLOC. One byte for an empty file,
    // so that an empty file found is not a file missing.
    rres::RaylibBytes data(
        static_cast<unsigned char *>(
            MemAlloc(static_cast<unsigned int>(std::max<std::size_t>(file->size(), 1)))),
        &MemFree);
    if (!data) return nullptr;
    std::ranges::copy(*file, data.get());
    if (size != nullptr) *size = static_cast<int>(file->size());
    return data.release();
}

// Images are the one thing the pack can hold as a *decoded* resource rather
// than as the original file: rrespacker stores them as an IMGE chunk. So this
// cannot go through pack_read(), which hands out bytes. Everything else
// (sounds, fonts, raw data) is stored verbatim and is loaded from memory by
// the caller.
//
// Returns a zeroed Image if the name is not packed or does not decode; the
// caller falls back to the loose file.
::Image pack_read_image(const char *name) {
    if (!pack.in_use || name == nullptr) return ::Image{};
    if (!chunk_named(name)) {
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not found in pack, falling back to loose file", name);
        return ::Image{};
    }
    const std::optional<rres::Contents> contents = open_named(name);
    ::Image img = contents ? rres::image_of(*contents) : ::Image{};
    if (img.data == nullptr) {
        TraceLog(LOG_WARNING,
                 "ASSETS: '%s' not usable from pack (corrupt, or another password), "
                 "falling back to loose file",
                 name);
    }
    return img;
}

} // namespace rmp::assets::detail
