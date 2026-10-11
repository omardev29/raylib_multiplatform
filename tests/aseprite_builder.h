#pragma once
// ---------------------------------------------------------------------------
// .aseprite files written byte by byte from the published format
// (https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md), for
// the tests of the reader that stands in front of cute_aseprite
// (src/rmp/aseprite.cpp): tests/animation_test.cpp builds the file each case
// is about, tests/property/animation_prop.cpp generates them, and both break
// them on purpose.
//
//   File      the header's fields and the frames; bytes() writes it, every
//             size filled in -- the file's, each frame's, each chunk's
//   Chunk     a type and a body, made by the functions below or by hand
//   zlib()    pixels compressed the three ways a deflate stream can be: fixed
//             Huffman codes (written here), dynamic ones (raylib's sdefl,
//             which falls back to a stored block when that is smaller) and
//             stored
//
// Nothing here checks what it is asked to write: a test that wants a layer
// nested under nothing or a cel of a layer that is not there asks for it.
// tools/make_aseprite_fixture.py writes tests/fixtures/anim.aseprite the same
// way, in Python.
// ---------------------------------------------------------------------------

#include <raylib.h> // CompressData, for the dynamic Huffman codes

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::aseprite {

// The chunk types this reader knows, by the specification's names.
inline constexpr std::uint16_t OLD_PALETTE = 0x0004;
inline constexpr std::uint16_t LAYER = 0x2004;
inline constexpr std::uint16_t CEL = 0x2005;
inline constexpr std::uint16_t CEL_EXTRA = 0x2006;
inline constexpr std::uint16_t COLOR_PROFILE = 0x2007;
inline constexpr std::uint16_t EXTERNAL_FILES = 0x2008;
inline constexpr std::uint16_t TAGS = 0x2018;
inline constexpr std::uint16_t PALETTE = 0x2019;
inline constexpr std::uint16_t USER_DATA = 0x2020;
inline constexpr std::uint16_t SLICE = 0x2022;

// Little-endian, as the whole format is.
class Bytes {
public:
    std::vector<unsigned char> data;

    Bytes &u8(unsigned value) {
        data.push_back(static_cast<unsigned char>(value & 0xFFU));
        return *this;
    }
    Bytes &u16(unsigned value) { return u8(value).u8(value >> 8); }
    Bytes &s16(int value) { return u16(static_cast<unsigned>(value) & 0xFFFFU); }
    Bytes &u32(std::uint32_t value) { return u16(value & 0xFFFFU).u16(value >> 16); }
    Bytes &zeros(std::size_t count) {
        data.insert(data.end(), count, 0);
        return *this;
    }
    Bytes &raw(std::span<const unsigned char> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
        return *this;
    }
    // The format's STRING: its length in bytes, then the bytes, no NUL.
    Bytes &string(std::string_view text) {
        u16(static_cast<unsigned>(text.size()));
        data.insert(data.end(), text.begin(), text.end());
        return *this;
    }
};

struct Chunk {
    std::uint16_t type = 0;
    std::vector<unsigned char> body;
};

struct Frame {
    std::uint16_t duration = 100; // milliseconds; 0 means the header's speed
    std::vector<Chunk> chunks;
};

struct File {
    std::uint16_t width = 4;
    std::uint16_t height = 4;
    std::uint16_t depth = 32; // bits per pixel: 32 RGBA, 16 grayscale, 8 indexed
    std::uint32_t flags = 1; // 1: the layers' opacity is valid
    std::uint16_t speed = 100; // the deprecated frame time, for a duration of 0
    std::uint8_t transparent = 0; // the palette entry that is transparent
    std::vector<Frame> frames;

    // How many bytes a pixel takes in a cel.
    [[nodiscard]] int bytes_per_pixel() const { return depth / 8; }

    [[nodiscard]] std::vector<unsigned char> bytes() const {
        Bytes body;
        for (const Frame &frame : frames) {
            Bytes chunks;
            for (const Chunk &chunk : frame.chunks) {
                chunks.u32(static_cast<std::uint32_t>(chunk.body.size() + 6))
                    .u16(chunk.type)
                    .raw(chunk.body);
            }
            const auto count = static_cast<std::uint32_t>(frame.chunks.size());
            body.u32(static_cast<std::uint32_t>(chunks.data.size() + 16))
                .u16(0xF1FA)
                .u16(count < 0xFFFF ? count : 0xFFFF)
                .u16(frame.duration)
                .zeros(2)
                .u32(count)
                .raw(chunks.data);
        }
        Bytes header;
        header.u32(static_cast<std::uint32_t>(128 + body.data.size()))
            .u16(0xA5E0)
            .u16(static_cast<unsigned>(frames.size()))
            .u16(width)
            .u16(height)
            .u16(depth)
            .u32(flags)
            .u16(speed)
            .zeros(8)
            .u8(transparent)
            .zeros(3)
            .u16(0) // colours in the palette
            .u8(1)
            .u8(1) // pixel width and height
            .s16(0)
            .s16(0) // grid position
            .u16(16)
            .u16(16) // grid size
            .zeros(84);
        header.raw(body.data);
        return std::move(header.data);
    }
};

// ---- chunks --------------------------------------------------------------

struct LayerOptions {
    std::uint16_t flags = 1; // 1 visible
    std::uint16_t type = 0; // 0 normal, 1 group, 2 tilemap
    std::uint16_t level = 0; // the child level: 0 at the top
    std::uint16_t blend = 0; // 0 normal
    std::uint8_t opacity = 255;
    std::string name = "Layer";
};

inline Chunk layer(const LayerOptions &options = {}) {
    Bytes body;
    body.u16(options.flags)
        .u16(options.type)
        .u16(options.level)
        .u16(0)
        .u16(0) // default width and height, ignored
        .u16(options.blend)
        .u8(options.opacity)
        .zeros(3)
        .string(options.name);
    return { LAYER, std::move(body.data) };
}

// The 16 bytes every cel starts with.
inline Bytes cel_head(int layer_index, int x, int y, int type, unsigned opacity = 255) {
    Bytes body;
    body.u16(static_cast<unsigned>(layer_index))
        .s16(x)
        .s16(y)
        .u8(opacity)
        .u16(static_cast<unsigned>(type))
        .s16(0) // z-index
        .zeros(5);
    return body;
}

inline Chunk raw_cel(int layer_index, int x, int y, int w, int h,
                     std::span<const unsigned char> pixels) {
    Bytes body = cel_head(layer_index, x, y, 0);
    body.u16(static_cast<unsigned>(w)).u16(static_cast<unsigned>(h)).raw(pixels);
    return { CEL, std::move(body.data) };
}

inline Chunk linked_cel(int layer_index, int frame) {
    Bytes body = cel_head(layer_index, 0, 0, 1);
    body.u16(static_cast<unsigned>(frame));
    return { CEL, std::move(body.data) };
}

// `stream` is a zlib stream: its two header bytes, the deflate data, the
// Adler-32 -- what zlib() makes.
inline Chunk compressed_cel(int layer_index, int x, int y, int w, int h,
                            std::span<const unsigned char> stream) {
    Bytes body = cel_head(layer_index, x, y, 2);
    body.u16(static_cast<unsigned>(w)).u16(static_cast<unsigned>(h)).raw(stream);
    return { CEL, std::move(body.data) };
}

struct Tag {
    int from = 0;
    int to = 0;
    int direction = 0; // 0 forward, 1 reverse, 2 ping-pong, 3 ping-pong reverse
    std::string name = "tag";
};

inline Chunk tags(const std::vector<Tag> &list) {
    Bytes body;
    body.u16(static_cast<unsigned>(list.size())).zeros(8);
    for (const Tag &tag : list) {
        body.u16(static_cast<unsigned>(tag.from))
            .u16(static_cast<unsigned>(tag.to))
            .u8(static_cast<unsigned>(tag.direction))
            .u16(0) // repeat: forever
            .zeros(6)
            .zeros(3) // the deprecated colour
            .u8(0)
            .string(tag.name);
    }
    return { TAGS, std::move(body.data) };
}

struct PaletteEntry {
    unsigned r = 0, g = 0, b = 0, a = 255;
    std::string name; // written only when not empty
};

inline Chunk palette(std::uint32_t size, std::uint32_t first,
                     const std::vector<PaletteEntry> &entries) {
    Bytes body;
    body.u32(size)
        .u32(first)
        .u32(static_cast<std::uint32_t>(first + entries.size() - 1))
        .zeros(8);
    for (const PaletteEntry &entry : entries) {
        body.u16(entry.name.empty() ? 0 : 1)
            .u8(entry.r)
            .u8(entry.g)
            .u8(entry.b)
            .u8(entry.a);
        if (!entry.name.empty()) body.string(entry.name);
    }
    return { PALETTE, std::move(body.data) };
}

// One packet of the old palette chunk: entries from `skip`, opaque.
inline Chunk old_palette(unsigned skip, const std::vector<PaletteEntry> &entries) {
    Bytes body;
    body.u16(1).u8(skip).u8(static_cast<unsigned>(entries.size() & 0xFF));
    for (const PaletteEntry &entry : entries) body.u8(entry.r).u8(entry.g).u8(entry.b);
    return { OLD_PALETTE, std::move(body.data) };
}

inline Chunk user_data(std::string_view text) {
    Bytes body;
    body.u32(text.empty() ? 2U : 3U);
    if (!text.empty()) body.string(text);
    body.u8(10).u8(20).u8(30).u8(255);
    return { USER_DATA, std::move(body.data) };
}

inline Chunk slice(std::string_view name, int count) {
    Bytes body;
    body.u32(static_cast<std::uint32_t>(count)).u32(0).u32(0).string(name);
    for (int i = 0; i < count; ++i) body.u32(0).u32(0).u32(0).u32(4).u32(4);
    return { SLICE, std::move(body.data) };
}

// An embedded ICC profile of `length` bytes.
inline Chunk icc_profile(std::uint32_t length) {
    Bytes body;
    body.u16(2).u16(0).u32(0).zeros(8).u32(length).zeros(length);
    return { COLOR_PROFILE, std::move(body.data) };
}

inline Chunk cel_extra() {
    Bytes body;
    body.u32(1).zeros(16).zeros(16);
    return { CEL_EXTRA, std::move(body.data) };
}

// ---- pixels and zlib -----------------------------------------------------

// `count` pixels of one colour, `bytes_per_pixel` bytes each.
inline std::vector<unsigned char> fill(int count, int bytes_per_pixel,
                                       std::uint32_t colour) {
    std::vector<unsigned char> out;
    for (int i = 0; i < count; ++i) {
        for (int b = 0; b < bytes_per_pixel; ++b) {
            out.push_back(static_cast<unsigned char>((colour >> (8 * b)) & 0xFFU));
        }
    }
    return out;
}

enum class Deflate {
    FIXED, // fixed Huffman codes, literals and runs of one pixel's bytes
    DYNAMIC, // raylib's sdefl: dynamic codes, or a stored block when smaller
    STORED, // one stored block
};

namespace detail {

// Deflate's bits are written from the low bit up; a Huffman code's from its
// high bit down.
struct BitWriter {
    std::vector<unsigned char> out;
    std::uint32_t pending = 0;
    int count = 0;

    void bits(std::uint32_t value, int n) {
        for (int i = 0; i < n; ++i) {
            pending |= ((value >> i) & 1U) << count;
            if (++count == 8) {
                out.push_back(static_cast<unsigned char>(pending));
                pending = 0;
                count = 0;
            }
        }
    }
    void code(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) bits((value >> i) & 1U, 1);
    }
    void flush() {
        if (count > 0) out.push_back(static_cast<unsigned char>(pending));
        pending = 0;
        count = 0;
    }
};

inline void fixed_literal(BitWriter &w, unsigned symbol) {
    if (symbol < 144)
        w.code(0x30 + symbol, 8);
    else if (symbol < 256)
        w.code(0x190 + (symbol - 144), 9);
    else if (symbol < 280)
        w.code(symbol - 256, 7);
    else
        w.code(0xC0 + (symbol - 280), 8);
}

// RFC 1951 3.2.5: a length of 3..258 as its symbol and extra bits.
inline void fixed_length(BitWriter &w, int length) {
    static constexpr std::array<int, 29> BASE{ 3,   4,   5,   6,   7,  8,  9,  10,
                                               11,  13,  15,  17,  19, 23, 27, 31,
                                               35,  43,  51,  59,  67, 83, 99, 115,
                                               131, 163, 195, 227, 258 };
    static constexpr std::array<int, 29> EXTRA{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                                1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                4, 4, 4, 4, 5, 5, 5, 5, 0 };
    std::size_t code = BASE.size() - 1;
    while (BASE[code] > length) --code;
    fixed_literal(w, 257U + static_cast<unsigned>(code));
    w.bits(static_cast<std::uint32_t>(length - BASE[code]), EXTRA[code]);
}

inline std::vector<unsigned char> deflate_fixed(std::span<const unsigned char> data,
                                                int distance) {
    BitWriter w;
    w.bits(1, 1); // the last block
    w.bits(1, 2); // fixed codes
    std::size_t at = 0;
    while (at < data.size()) {
        // A run: the bytes `distance` back, again, three or more times.
        std::size_t run = 0;
        if (std::cmp_greater_equal(at, distance)) {
            while (at + run < data.size() && run < 258 &&
                   data[at + run] == data[at + run - static_cast<std::size_t>(distance)])
                ++run;
        }
        if (run >= 3) {
            fixed_length(w, static_cast<int>(run));
            w.code(static_cast<std::uint32_t>(distance - 1), 5); // distances 1..4
            at += run;
        } else {
            fixed_literal(w, data[at]);
            ++at;
        }
    }
    fixed_literal(w, 256);
    w.flush();
    return w.out;
}

inline std::vector<unsigned char> deflate_stored(std::span<const unsigned char> data) {
    Bytes out;
    out.u8(1); // the last block, stored, padded to the byte
    out.u16(static_cast<unsigned>(data.size()))
        .u16(~static_cast<unsigned>(data.size()) & 0xFFFFU)
        .raw(data);
    return std::move(out.data);
}

inline std::vector<unsigned char> deflate_dynamic(std::span<const unsigned char> data) {
    int size = 0;
    const std::unique_ptr<unsigned char, decltype(&MemFree)> packed(
        CompressData(data.data(), static_cast<int>(data.size()), &size), &MemFree);
    if (!packed || size <= 0) return {};
    const std::span<const unsigned char> bytes(packed.get(),
                                               static_cast<std::size_t>(size));
    return { bytes.begin(), bytes.end() };
}

// RFC 1951 3.2.2: the canonical code of each symbol, from the lengths.
inline std::vector<std::uint32_t> canonical(std::span<const int> lengths) {
    std::array<std::uint32_t, 16> count{};
    for (const int length : lengths) ++count[static_cast<std::size_t>(length)];
    count[0] = 0;
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (std::size_t bits = 1; bits < 16; ++bits) {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    std::vector<std::uint32_t> codes(lengths.size());
    for (std::size_t n = 0; n < lengths.size(); ++n) {
        if (lengths[n] != 0) codes[n] = next[static_cast<std::size_t>(lengths[n])]++;
    }
    return codes;
}

inline std::uint32_t adler32(std::span<const unsigned char> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const unsigned char byte : data) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

} // namespace detail

// A zlib stream of `pixels`: header, deflate data, Adler-32 (big-endian, as
// zlib writes it). `distance` is how far back FIXED looks for a run: one
// pixel's bytes.
inline std::vector<unsigned char> zlib(std::span<const unsigned char> pixels, Deflate how,
                                       int distance = 4) {
    std::vector<unsigned char> deflated;
    switch (how) {
        case Deflate::FIXED:
            deflated = detail::deflate_fixed(pixels, distance);
            break;
        case Deflate::DYNAMIC:
            deflated = detail::deflate_dynamic(pixels);
            break;
        case Deflate::STORED:
            deflated = detail::deflate_stored(pixels);
            break;
    }
    Bytes out;
    out.u8(0x78).u8(0x9C).raw(deflated);
    const std::uint32_t check = detail::adler32(pixels);
    out.u8(check >> 24).u8(check >> 16).u8(check >> 8).u8(check);
    return std::move(out.data);
}

// A deflate stream written symbol by symbol, for the blocks no encoder writes:
// a dynamic header whose repeats run past its lengths, a code its table does
// not have, a distance from before the first pixel.
class Stream {
public:
    // A block of fixed codes.
    void fixed_block(bool last) {
        _w.bits(last ? 1 : 0, 1);
        _w.bits(1, 2);
        _literal = fixed_lengths();
        _distance = std::vector<int>(30, 5);
        _literal_codes = detail::canonical(_literal);
        _distance_codes = detail::canonical(_distance);
    }

    // A block of dynamic codes: `literal` (257..288 lengths) and `distance`
    // (1..32) are the codes the block's data is written with. `spelled` is how
    // the header writes them, as code-length symbols 0..18 with their extra
    // bits; empty, every length is written as itself. The code-length code
    // gives each of its 19 symbols 5 bits.
    void dynamic_block(bool last, std::vector<int> literal, std::vector<int> distance,
                       const std::vector<std::pair<int, int>> &spelled = {}) {
        static constexpr std::array<int, 19> ORDER{ 16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                    11, 4,  12, 3, 13, 2, 14, 1, 15 };
        static constexpr std::array<int, 19> EXTRA{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                    0, 0, 0, 0, 0, 0, 2, 3, 7 };
        _w.bits(last ? 1 : 0, 1);
        _w.bits(2, 2);
        _w.bits(static_cast<std::uint32_t>(literal.size() - 257), 5);
        _w.bits(static_cast<std::uint32_t>(distance.size() - 1), 5);
        _w.bits(19 - 4, 4);
        const std::vector<int> lengths_of_lengths(19, 5);
        for (std::size_t i = 0; i < ORDER.size(); ++i) _w.bits(5, 3);
        const std::vector<std::uint32_t> length_codes =
            detail::canonical(lengths_of_lengths);
        std::vector<std::pair<int, int>> plain;
        if (spelled.empty()) {
            for (const int length : literal) plain.emplace_back(length, 0);
            for (const int length : distance) plain.emplace_back(length, 0);
        }
        for (const auto &[symbol, extra] : spelled.empty() ? plain : spelled) {
            _w.code(length_codes[static_cast<std::size_t>(symbol)], 5);
            _w.bits(static_cast<std::uint32_t>(extra),
                    EXTRA[static_cast<std::size_t>(symbol)]);
        }
        _literal = std::move(literal);
        _distance = std::move(distance);
        _literal_codes = detail::canonical(_literal);
        _distance_codes = detail::canonical(_distance);
    }

    // A literal/length symbol, with the block's codes (256 ends the block).
    void symbol(int value) {
        const auto at = static_cast<std::size_t>(value);
        _w.code(_literal_codes[at], _literal[at]);
    }
    void literals(std::span<const unsigned char> bytes) {
        for (const unsigned char byte : bytes) symbol(byte);
    }
    // A match: `length` 3..258 bytes from `distance` 1..32768 back.
    void match(int length, int distance) {
        static constexpr std::array<int, 29> LENGTH{ 3,   4,   5,   6,   7,  8,  9,  10,
                                                     11,  13,  15,  17,  19, 23, 27, 31,
                                                     35,  43,  51,  59,  67, 83, 99, 115,
                                                     131, 163, 195, 227, 258 };
        static constexpr std::array<int, 29> LENGTH_EXTRA{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                                           1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                           4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static constexpr std::array<int, 30> DISTANCE{
            1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
            33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
        };
        static constexpr std::array<int, 30> DISTANCE_EXTRA{
            0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
        };
        std::size_t l = LENGTH.size() - 1;
        while (LENGTH[l] > length) --l;
        symbol(257 + static_cast<int>(l));
        _w.bits(static_cast<std::uint32_t>(length - LENGTH[l]), LENGTH_EXTRA[l]);
        std::size_t d = DISTANCE.size() - 1;
        while (DISTANCE[d] > distance) --d;
        _w.code(_distance_codes[d], _distance[d]);
        _w.bits(static_cast<std::uint32_t>(distance - DISTANCE[d]), DISTANCE_EXTRA[d]);
    }
    void end() { symbol(256); }
    // Bits as they are, low bit first: a code the table does not have, say.
    void bits(std::uint32_t value, int count) { _w.bits(value, count); }

    // The stream as a cel holds it: zlib's two bytes, the deflate data, and
    // four bytes of Adler-32 (cute_aseprite does not check them).
    [[nodiscard]] std::vector<unsigned char> zlib() const {
        detail::BitWriter copy = _w;
        copy.flush();
        Bytes out;
        out.u8(0x78).u8(0x9C).raw(copy.out).u32(0);
        return std::move(out.data);
    }

private:
    static std::vector<int> fixed_lengths() {
        std::vector<int> lengths(288, 8);
        for (std::size_t i = 144; i < 256; ++i) lengths[i] = 9;
        for (std::size_t i = 256; i < 280; ++i) lengths[i] = 7;
        return lengths;
    }

    detail::BitWriter _w;
    std::vector<int> _literal;
    std::vector<int> _distance;
    std::vector<std::uint32_t> _literal_codes;
    std::vector<std::uint32_t> _distance_codes;
};

// The type of the first block of a zlib stream: 0 stored, 1 fixed, 2 dynamic.
inline int first_block_type(std::span<const unsigned char> stream) {
    return stream.size() > 2 ? (stream[2] >> 1) & 3 : -1;
}

} // namespace rmp::test::aseprite
