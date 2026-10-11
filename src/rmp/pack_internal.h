#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The rres pack's reader, in the pieces a test can hold.
//
// src/rmp/pack.cpp reads resources.rres through these, from its file; the
// tests and the rres fuzz target read packs through the same functions from
// bytes in memory, with a key stretch that costs nothing. Every field of a
// pack is checked here against the bytes there are before it is used, and
// nothing of a pack reaches rres's own readers -- see pack.cpp for why.
// ---------------------------------------------------------------------------

#include <raylib.h> // ::Image

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmp::assets::detail::rres {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
using Salt = std::array<std::uint8_t, 16>;

// How a password becomes a key. A pack's is argon2i(); a test of the format
// hands in a cheap one, because Argon2i costs two seconds a chunk under ASan.
using Stretch = Key (*)(std::string_view password, const Salt &salt);

// A path as its folders and its name -- '/' between them, and '\' too on
// Windows, where the system reads it as one: "." and empty
// segments gone and ".." taken back where it can be, so "./x", "sub/../x" and
// "x" are one path -- lexically, which is what the system does when the
// folders on the way exist. An absolute path keeps an empty first segment, so
// it never matches a relative one.
std::vector<std::string_view> segments(std::string_view path);

// The name in the pack that `path` means, when the resources folder is
// `root`: a file directly in that folder, however the path spells it --
// "resources/x.png", "./resources/x.png", "resources/sub/../x.png" -- and
// nothing for a file below it (the packer stores every file under its bare
// name and does not recurse), outside it, or a path that ends in a separator,
// "." or "..", which names a folder. The loader hook asks this of the paths
// raylib reads, and the loaders of root + the name they were given, so a
// release finds exactly what a development build finds in resources/.
std::optional<std::string> packed_name(std::string_view root, std::string_view path);

// rres's key stretch: Argon2i, 16 MiB, three passes, one lane -- what
// rrespacker and tools/rres_pack.c encrypt with.
Key argon2i(std::string_view password, const Salt &salt);

// Where a pack's bytes come from: its file in a game, memory in a test.
class Source {
public:
    Source() = default;
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;
    Source(Source &&) = delete;
    Source &operator=(Source &&) = delete;
    virtual ~Source() = default;
    [[nodiscard]] virtual std::uint64_t size() = 0;
    // Exactly out.size() bytes from `offset`, or false.
    [[nodiscard]] virtual bool read(std::uint64_t offset,
                                    std::span<std::uint8_t> out) = 0;
};

// A pack already in memory.
class MemorySource final : public Source {
public:
    explicit MemorySource(std::span<const std::uint8_t> bytes) : _bytes(bytes) {}
    [[nodiscard]] std::uint64_t size() override { return _bytes.size(); }
    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::uint8_t> out) override;

private:
    std::span<const std::uint8_t> _bytes;
};

// A chunk's 32-byte info, as rres lays it out.
struct ChunkInfo {
    std::array<char, 4> type{};
    std::uint32_t id = 0;
    std::uint8_t compression = 0;
    std::uint8_t cipher = 0;
    std::uint32_t packed_size = 0;
    std::uint32_t base_size = 0;
    std::uint32_t crc = 0;
};

// A chunk of the pack: where its info is, its info, and where its packed
// bytes start (32 bytes later).
struct Chunk {
    std::uint64_t at = 0;
    std::uint64_t data = 0;
    ChunkInfo info;
};

// One name of the pack's directory: the id of the chunk it names, and where
// in the file that chunk's info is.
struct Entry {
    std::string name;
    std::uint32_t id = 0;
    std::uint32_t offset = 0;
};

// What a pack holds: its directory, in its order, and every chunk met
// walking the file from the header.
struct Index {
    std::vector<Entry> entries;
    std::vector<Chunk> chunks;
};

// The header, every chunk's info and the directory, each checked against the
// bytes there are. nullopt for a pack that is not what rres writes, is cut
// short anywhere, or whose directory is corrupt: a pack is used whole or not
// at all.
std::optional<Index> read_index(Source &pack);

// The chunk the directory calls `name`: the whole name, compared whole, and
// the chunk where the directory says it is, with the id it says it has. rres
// looked the id up by walking the file, which a chunk whose info was corrupt
// into another's id answered for that other name -- the CRC covers a chunk's
// bytes and not its info, so the place is what ties a name to its bytes.
std::optional<Chunk> find(const Index &index, std::string_view name);

// A chunk opened: its CRC checked, decrypted, decompressed, and split into
// its properties and what follows them. nullopt for a chunk whose bytes are
// not what its info says, that does not decrypt with `password`, or that is
// compressed or encrypted some way this does not read.
struct Contents {
    std::array<char, 4> type{};
    std::vector<std::uint32_t> props;
    Bytes data;
};
std::optional<Contents> open_chunk(Source &pack, const Chunk &chunk,
                                   std::string_view password, Stretch stretch);

// A RAWD chunk's file, byte for byte as the packer read it. nullopt for any
// other chunk, or one whose size property says more than it holds.
std::optional<Bytes> file_of(const Contents &contents);

// The picture in a chunk: an IMGE's pixels as they are, or a RAWD's file
// decoded by the extension the packer stored with it. Empty (no data) when
// it is neither, or does not hold what its properties say.
::Image image_of(const Contents &contents);

} // namespace rmp::assets::detail::rres
