// ---------------------------------------------------------------------------
// What a .aseprite may hand cute_aseprite.
//
// cute_aseprite (thirdparty/cute_aseprite, pinned unmodified) trusts the file.
// It ASSERTS on what it cannot read -- a frame without its magic number, a
// chunk that is not the size it says, a deflate stream cut short -- and an
// assert is abort(); under NDEBUG it reads on instead, out of bounds. It
// indexes its tables with a layer's nesting level, a cel's layer and a
// palette entry unchecked, follows linked cels round in a circle forever, and
// leaks what a second chunk of a kind writes over. A corrupt sheet in a
// shipped game would take the process down, and every other loader here comes
// back empty instead: a hole in the picture, not a dead process. tests/fuzz/
// aseprite.cpp found 315 crashing files in its first 90 seconds.
//
// So every file walks through here first, chunk by chunk, every read checked
// against the bytes there are. It is refused, saying why, for whatever
// cute_aseprite would assert on, read or write outside what it owns for, loop
// on, leak, or allocate past the budgets in animation_internal.h -- and the
// walk reads what cute_aseprite reads in the order it reads it, down to its
// own deflate decoder, so "accepted here" means "read there without a fault".
// What is accepted is handed over as a COPY with two kinds of change, neither
// of which moves a pixel:
//
//   - the chunks the sheet does not need are retyped as one cute_aseprite
//     steps over: user data (an Aseprite 1.3 file's sprite user data and its
//     properties make it assert, and a tag's text leaks), slices (every slice
//     chunk after the first leaks its name), colour profiles (a second ICC
//     profile leaks the first), cel extras (written to the cel AFTER the last,
//     past the table on a frame's 64th), and the palette when the colours are
//     not indexed -- or, when they are, the old palette chunk beside a new
//     one: Aseprite writes both, and the old one overwrites the new one's
//     colour names, which then leak;
//   - every layer's blend mode is 0: cute_aseprite draws each one as normal
//     whatever it says, and prints a warning to stdout for any other.
//
// What cute_aseprite cannot read at all is refused rather than half read: a
// tilemap layer or cel and layer UUIDs (Aseprite 1.3), and a stored deflate
// block -- its own check of one counts the stream's Adler-32 as data and
// always fails, leaving the cel's pixels uninitialised.
// ---------------------------------------------------------------------------

#include "animation_internal.h"

#include <rmp/assets.h> // SheetSlice, which the walk reads slices into

#include <cute_aseprite.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::animation::detail {

namespace {

constexpr std::uint16_t FILE_MAGIC = 0xA5E0;
constexpr std::uint16_t FRAME_MAGIC = 0xF1FA;
constexpr std::size_t FILE_HEADER = 128;
constexpr std::size_t FRAME_HEADER = 16;
constexpr std::size_t CHUNK_HEADER = 6;

// The chunks, by the specification's names.
constexpr std::uint16_t OLD_PALETTE = 0x0004;
constexpr std::uint16_t LAYER = 0x2004;
constexpr std::uint16_t CEL = 0x2005;
constexpr std::uint16_t CEL_EXTRA = 0x2006;
constexpr std::uint16_t COLOR_PROFILE = 0x2007;
constexpr std::uint16_t TAGS = 0x2018;
constexpr std::uint16_t PALETTE = 0x2019;
constexpr std::uint16_t USER_DATA = 0x2020;
constexpr std::uint16_t SLICE = 0x2022;
// A type cute_aseprite does not know, and so steps over.
constexpr std::uint16_t SKIPPED = 0xFFFF;

// What cute_aseprite's fixed tables hold. It asserts tag_count < its maximum,
// so a file has at most one tag fewer.
constexpr int MAX_LAYERS = CUTE_ASEPRITE_MAX_LAYERS;
constexpr int MAX_TAGS = CUTE_ASEPRITE_MAX_TAGS - 1;
constexpr int MAX_PALETTE = CUTE_ASEPRITE_MAX_PALETTE_ENTRIES;
// The layer flags Aseprite writes (visible .. reference layer): one past them
// is outside cute_aseprite's enum, and loading it is undefined behaviour.
constexpr unsigned LAYER_FLAGS = 0x7F;
constexpr unsigned HEADER_LAYER_UUIDS = 4; // the header's flag: layers carry a UUID

// The reasons a file is refused, as parse_sheet() logs them.
constexpr std::string_view SHORT = "a chunk that is not the size it says";
constexpr std::string_view TOO_BIG = "bigger than a sheet may be";
constexpr std::string_view NOT_DEFLATE =
    "a compressed cel that does not decompress to its pixels";

std::uint32_t le16(std::span<const unsigned char> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
        (static_cast<std::uint32_t>(bytes[at + 1]) << 8);
}

std::uint32_t le32(std::span<const unsigned char> bytes, std::size_t at) {
    return le16(bytes, at) | (le16(bytes, at + 2) << 16);
}

// Reads one span and never past its end: a read that does not fit fails, and
// so does every read after it, each answering 0. ok() asks whether every read
// fitted, whole() whether they read exactly all of it -- cute_aseprite asserts
// that a chunk's reads add up to its size.
class Cursor {
public:
    explicit Cursor(std::span<const unsigned char> bytes) : _bytes(bytes) {}

    unsigned u8() { return take(1) ? _bytes[_at - 1] : 0U; }
    unsigned u16() { return take(2) ? le16(_bytes, _at - 2) : 0U; }
    std::uint32_t u32() { return take(4) ? le32(_bytes, _at - 4) : 0U; }
    int s16() {
        const unsigned value = u16();
        return value < 0x8000U ? static_cast<int>(value)
                               : static_cast<int>(value) - 0x10000;
    }
    std::int32_t s32() { return static_cast<std::int32_t>(u32()); }
    void skip(std::size_t count) { take(count); }
    // The format's STRING: its length, then that many bytes.
    void string() { skip(u16()); }
    // The same, read: the bytes it holds, or "" when they do not fit.
    std::string_view text() {
        const std::size_t length = u16();
        if (!take(length)) return {};
        return { reinterpret_cast<const char *>(_bytes.data() + _at - length), length };
    }
    // What is left, taken whole.
    std::span<const unsigned char> rest() {
        const std::span<const unsigned char> left = _bytes.subspan(_at);
        _at = _bytes.size();
        return left;
    }
    [[nodiscard]] std::size_t at() const { return _at; }
    [[nodiscard]] bool ok() const { return _ok; }
    [[nodiscard]] bool whole() const { return _ok && _at == _bytes.size(); }

private:
    bool take(std::size_t count) {
        if (!_ok || count > _bytes.size() - _at) {
            _ok = false;
            return false;
        }
        _at += count;
        return true;
    }

    std::span<const unsigned char> _bytes;
    std::size_t _at = 0;
    bool _ok = true;
};

// ---- deflate, as cute_aseprite decodes it ----------------------------------
//
// A zlib stream's deflate data, low bit first. Every read is checked against
// `limit`, the stream less its four bytes of Adler-32: decoding that ends in
// them is a stream cut short, and stopping before them is what keeps every bit
// cute_aseprite's reader buffers a real one (its last partial word is counted
// wrong, and harmlessly so only away from the end).
class Bits {
public:
    Bits(std::span<const unsigned char> bytes, std::uint64_t limit)
        : _bytes(bytes), _limit(limit) {}

    // The next `count` (at most 16) bits without taking them; zeros past the
    // end, as cute_aseprite's buffer has.
    [[nodiscard]] std::uint32_t peek(int count) const {
        const std::uint64_t byte = _at / 8;
        std::uint32_t window = 0;
        for (std::uint64_t k = 0; k < 4 && byte + k < _bytes.size(); ++k) {
            window |=
                static_cast<std::uint32_t>(_bytes[static_cast<std::size_t>(byte + k)])
                << (8 * k);
        }
        return (window >> (_at % 8)) & ((1U << count) - 1U);
    }
    bool take(int count) {
        _at += static_cast<std::uint64_t>(count);
        return _at <= _limit;
    }
    bool read(int count, std::uint32_t &out) {
        out = peek(count);
        return take(count);
    }

private:
    std::span<const unsigned char> _bytes;
    std::uint64_t _limit;
    std::uint64_t _at = 0;
};

// A Huffman table built and searched the way cute_aseprite's s_build() and
// s_decode() do it -- every code left-aligned in 32 bits, its symbol and
// length packed below, in canonical order -- so a code that is missing,
// over-subscribed or out of order decodes here exactly as it will there.
struct Table {
    std::array<std::uint32_t, 288> entries{};
    int count = 0;
};

void build(Table &table, std::span<const std::uint8_t> lengths) {
    std::array<std::uint32_t, 16> counts{};
    std::array<std::uint32_t, 16> codes{};
    std::array<std::uint32_t, 16> first{};
    for (const std::uint8_t length : lengths) ++counts[length];
    counts[0] = 0;
    for (std::size_t n = 1; n < 16; ++n) {
        codes[n] = (codes[n - 1] + counts[n - 1]) << 1;
        first[n] = first[n - 1] + counts[n - 1];
    }
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const std::uint32_t length = lengths[symbol];
        if (length == 0) continue;
        const std::uint32_t code = codes[length]++;
        table.entries[first[length]++] =
            (code << (32 - length)) | (static_cast<std::uint32_t>(symbol) << 4) | length;
    }
    table.count = static_cast<int>(first[15]);
}

std::uint32_t reverse16(std::uint32_t a) {
    a = ((a & 0xAAAAU) >> 1) | ((a & 0x5555U) << 1);
    a = ((a & 0xCCCCU) >> 2) | ((a & 0x3333U) << 2);
    a = ((a & 0xF0F0U) >> 4) | ((a & 0x0F0FU) << 4);
    return ((a & 0xFF00U) >> 8) | ((a & 0x00FFU) << 8);
}

// The next symbol, or -1 where cute_aseprite would assert: a table with no
// codes (it reads the entry before the first), bits no code starts with, or
// a code that runs into the checksum.
int decode(Bits &bits, const Table &table) {
    const std::uint32_t search = (reverse16(bits.peek(16)) << 16) | 0xFFFFU;
    int lo = 0;
    int hi = table.count;
    while (lo < hi) {
        const int guess = (lo + hi) >> 1;
        if (search < table.entries[static_cast<std::size_t>(guess)])
            hi = guess;
        else
            lo = guess + 1;
    }
    if (lo == 0) return -1;
    const std::uint32_t key = table.entries[static_cast<std::size_t>(lo - 1)];
    const int length = static_cast<int>(key & 0xFU);
    const int unused = 32 - length;
    if ((search >> unused) != (key >> unused)) return -1;
    if (!bits.take(length)) return -1;
    return static_cast<int>((key >> 4) & 0xFFFU);
}

// RFC 1951 3.2.5. Symbols 286 and 287 and distances 30 and 31 are in no
// table: cute_aseprite reads them as a length of 0 and a distance of 0 -- a
// byte copied onto itself, uninitialised.
constexpr std::array<int, 29> LENGTH_BASE{ 3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                           15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr std::array<int, 29> LENGTH_EXTRA{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr std::array<int, 30> DISTANCE_BASE{
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
constexpr std::array<int, 30> DISTANCE_EXTRA{ 0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                              4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                              9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
// The order a dynamic block sends its code-length code in (3.2.7).
constexpr std::array<std::size_t, 19> LENGTH_ORDER{ 16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                    11, 4,  12, 3, 13, 2, 14, 1, 15 };

// One block's data, counting the bytes it makes: every literal and every
// copy inside the cel's pixels, every copy from bytes already made.
bool inflate_block(Bits &bits, const Table &literal, const Table &distance,
                   std::int64_t &made, std::int64_t size) {
    for (;;) {
        const int symbol = decode(bits, literal);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (made >= size) return false;
            ++made;
            continue;
        }
        if (symbol == 256) return true;
        const auto at = static_cast<std::size_t>(symbol - 257);
        if (at >= LENGTH_BASE.size()) return false;
        std::uint32_t extra = 0;
        if (!bits.read(LENGTH_EXTRA[at], extra)) return false;
        const std::int64_t length = LENGTH_BASE[at] + static_cast<std::int64_t>(extra);
        const int code = decode(bits, distance);
        if (code < 0 || static_cast<std::size_t>(code) >= DISTANCE_BASE.size())
            return false;
        const auto d = static_cast<std::size_t>(code);
        if (!bits.read(DISTANCE_EXTRA[d], extra)) return false;
        const std::int64_t back = DISTANCE_BASE[d] + static_cast<std::int64_t>(extra);
        if (back > made || length > size - made) return false;
        made += length;
    }
}

// A dynamic block's header: the code-length code, then the literal and
// distance code lengths it spells -- whose repeats may cross from one table
// into the other, and may not run past the second or repeat nothing.
bool dynamic_tables(Bits &bits, Table &literal, Table &distance) {
    std::uint32_t literals = 0;
    std::uint32_t distances = 0;
    std::uint32_t spelled = 0;
    if (!bits.read(5, literals) || !bits.read(5, distances) || !bits.read(4, spelled))
        return false;
    const std::size_t nlit = 257 + literals;
    const std::size_t ndist = 1 + distances;
    std::array<std::uint8_t, 19> length_lengths{};
    for (std::size_t i = 0; i < 4 + spelled; ++i) {
        std::uint32_t value = 0;
        if (!bits.read(3, value)) return false;
        length_lengths[LENGTH_ORDER[i]] = static_cast<std::uint8_t>(value);
    }
    Table lengths_code;
    build(lengths_code, length_lengths);

    std::array<std::uint8_t, 288 + 32> lengths{};
    std::size_t n = 0;
    while (n < nlit + ndist) {
        const int symbol = decode(bits, lengths_code);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[n++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint32_t extra = 0;
        std::size_t repeat = 0;
        std::uint8_t value = 0;
        if (symbol == 16) {
            if (n == 0 || !bits.read(2, extra)) return false;
            repeat = 3 + extra;
            value = lengths[n - 1];
        } else if (symbol == 17) {
            if (!bits.read(3, extra)) return false;
            repeat = 3 + extra;
        } else {
            if (!bits.read(7, extra)) return false;
            repeat = 11 + extra;
        }
        if (repeat > nlit + ndist - n) return false;
        std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(n), repeat, value);
        n += repeat;
    }
    build(literal, std::span<const std::uint8_t>(lengths).first(nlit));
    build(distance, std::span<const std::uint8_t>(lengths).subspan(nlit, ndist));
    return true;
}

// Whether a cel's deflate data decompresses, the way cute_aseprite decodes
// it, to exactly `size` bytes: "" when it does, or why not.
std::string_view inflates_to(std::span<const unsigned char> stream, std::int64_t size) {
    // The Adler-32 and at least one byte of data.
    if (stream.size() < 5) return NOT_DEFLATE;
    Bits bits(stream, (static_cast<std::uint64_t>(stream.size()) - 4) * 8);
    Table fixed_literal;
    Table fixed_distance;
    {
        std::array<std::uint8_t, 288> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, 8);
        std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
        std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
        std::fill(lengths.begin() + 280, lengths.end(), 8);
        build(fixed_literal, lengths);
        std::array<std::uint8_t, 32> distances{};
        distances.fill(5);
        build(fixed_distance, distances);
    }
    std::int64_t made = 0;
    std::uint32_t last = 0;
    do {
        std::uint32_t type = 0;
        if (!bits.read(1, last) || !bits.read(2, type)) return NOT_DEFLATE;
        if (type == 0) return "a stored deflate block, which cute_aseprite cannot read";
        if (type == 1) {
            if (!inflate_block(bits, fixed_literal, fixed_distance, made, size))
                return NOT_DEFLATE;
        } else if (type == 2) {
            Table literal;
            Table distance;
            if (!dynamic_tables(bits, literal, distance) ||
                !inflate_block(bits, literal, distance, made, size))
                return NOT_DEFLATE;
        } else {
            return NOT_DEFLATE;
        }
    } while (last == 0);
    return made == size ? std::string_view{} : NOT_DEFLATE;
}

// ---- the chunks ------------------------------------------------------------

// A cel as the blend at the end of cute_aseprite's read sees it.
struct Cel {
    int layer = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int link = -1; // the frame a linked cel shows, or -1
};

class Walk {
public:
    Walk(std::span<const unsigned char> file, std::vector<unsigned char> &out)
        : _file(file), _out(out) {}

    // The slices it read on the way: cute_aseprite never sees them (see the
    // top of this file), so this walk is what reads them for the sheet.
    [[nodiscard]] std::vector<SheetSlice> take_slices() { return std::move(_slices); }

    std::string_view run() {
        if (const std::string_view why = header(); !why.empty()) return why;
        _out.assign(_file.begin(), _file.end());
        std::size_t at = FILE_HEADER;
        for (int f = 0; f < _frames; ++f) {
            if (const std::string_view why = frame(at); !why.empty()) return why;
        }
        return finish();
    }

private:
    std::string_view header() {
        if (_file.size() < FILE_HEADER) return "not an Aseprite file";
        // RMP_COVERAGE_OFF(two gigabytes: no test allocates a file past int's range)
        if (_file.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return TOO_BIG;
        // RMP_COVERAGE_ON
        Cursor head(_file.first(FILE_HEADER));
        const std::uint32_t declared = head.u32();
        if (head.u16() != FILE_MAGIC) return "not an Aseprite file";
        if (declared != _file.size()) return "a file that is not the size it says";
        _frames = static_cast<int>(head.u16());
        _width = static_cast<int>(head.u16());
        _height = static_cast<int>(head.u16());
        const unsigned depth = head.u16();
        _flags = head.u32();
        if (_frames == 0 || _width == 0 || _height == 0)
            return "a sheet of no frames or no size";
        if (depth != 8 && depth != 16 && depth != 32)
            return "a colour depth Aseprite does not write";
        _bpp = static_cast<int>(depth / 8);
        _indexed = depth == 8;
        // Every frame's entry in cute_aseprite's table (room for 64 cels), and
        // its picture, RGBA, before a single chunk is read.
        const std::int64_t picture = std::int64_t{ _width } * _height * 4;
        return charge(std::int64_t{ _frames } *
                      (static_cast<std::int64_t>(sizeof(ase_frame_t)) + picture));
    }

    std::string_view charge(std::int64_t bytes) {
        _bytes += bytes;
        return _bytes > MAX_SHEET_BYTES ? TOO_BIG : std::string_view{};
    }

    std::string_view frame(std::size_t &at) {
        if (_file.size() - at < FRAME_HEADER) return "a file cut short";
        Cursor head(_file.subspan(at, FRAME_HEADER));
        head.u32(); // the frame's size, which cute_aseprite does not read either
        if (head.u16() != FRAME_MAGIC) return "a frame without its magic number";
        const unsigned old_count = head.u16();
        head.u16(); // its duration
        head.skip(2);
        const std::uint32_t new_count = head.u32();
        // cute_aseprite's arithmetic: a count past INT_MAX is negative, and
        // then the frame has no chunks.
        const std::int64_t count = new_count != 0
            ? std::int64_t{ static_cast<std::int32_t>(new_count) }
            : old_count;
        at += FRAME_HEADER;

        // Where its chunks are, and whether there is a palette chunk among
        // them, before any of them is read.
        std::vector<std::size_t> chunks;
        bool new_palette = false;
        for (std::int64_t c = 0; c < count; ++c) {
            if (_file.size() - at < CHUNK_HEADER) return "a file cut short";
            const std::uint32_t size = le32(_file, at);
            if (size < CHUNK_HEADER || size > _file.size() - at) return SHORT;
            new_palette = new_palette || le16(_file, at + 4) == PALETTE;
            chunks.push_back(at);
            at += size;
        }
        _frame_cels.push_back(_cels.size());
        for (const std::size_t chunk : chunks) {
            if (const std::string_view why = read_chunk(chunk, new_palette); !why.empty())
                return why;
        }
        return {};
    }

    std::string_view read_chunk(std::size_t at, bool new_palette) {
        const std::span<const unsigned char> body =
            _file.subspan(at + CHUNK_HEADER, le32(_file, at) - CHUNK_HEADER);
        switch (le16(_file, at + 4)) {
            case OLD_PALETTE:
                if (!_indexed || new_palette) return skip(at);
                return old_palette(body);
            case PALETTE:
                if (!_indexed) return skip(at);
                return palette(body);
            case LAYER:
                return layer(body, at + CHUNK_HEADER);
            case CEL:
                return cel(body);
            case TAGS:
                return tags(body);
            case SLICE:
                read_slice(body);
                return skip(at);
            case CEL_EXTRA:
            case COLOR_PROFILE:
            case USER_DATA:
                return skip(at);
            default:
                return {}; // cute_aseprite steps over a chunk it does not know
        }
    }

    // A slice chunk: its name, and its first key -- where it is on the frame
    // that key is for, and its 9-slices centre when it has one. One that does
    // not read whole, or does not sit inside the canvas, is left out: a slice
    // is never a reason to refuse the pictures. The bounds are moved to where
    // that frame is in the packed texture, which lays the frames in one row.
    void read_slice(std::span<const unsigned char> body) {
        if (_slices.size() >= MAX_SLICES) return;
        Cursor c(body);
        const std::uint32_t keys = c.u32();
        const std::uint32_t flags = c.u32();
        c.u32(); // reserved
        const std::string_view name = c.text();
        if (!c.ok() || keys == 0) return;
        const std::uint32_t frame = c.u32();
        const std::int32_t x = c.s32();
        const std::int32_t y = c.s32();
        const std::uint32_t w = c.u32();
        const std::uint32_t h = c.u32();
        SheetSlice slice;
        slice.name = std::string(name);
        if ((flags & 1U) != 0) {
            const std::int32_t cx = c.s32();
            const std::int32_t cy = c.s32();
            const std::uint32_t cw = c.u32();
            const std::uint32_t ch = c.u32();
            const bool inside = cx >= 0 && cy >= 0 && cw > 0 && ch > 0 &&
                std::int64_t{ cx } + cw <= w && std::int64_t{ cy } + ch <= h;
            if (inside) {
                slice.nine = true;
                slice.center =
                    Rectangle{ static_cast<float>(cx), static_cast<float>(cy),
                               static_cast<float>(cw), static_cast<float>(ch) };
            }
        }
        if (!c.ok() || w == 0 || h == 0 || x < 0 || y < 0) return;
        if (std::int64_t{ x } + w > _width || std::int64_t{ y } + h > _height) return;
        if (frame >= static_cast<std::uint32_t>(_frames)) return;
        slice.bounds = Rectangle{ static_cast<float>(std::int64_t{ frame } * _width + x),
                                  static_cast<float>(y), static_cast<float>(w),
                                  static_cast<float>(h) };
        _slices.push_back(std::move(slice));
    }

    // The chunk at `at`, retyped in the copy as one cute_aseprite steps over.
    std::string_view skip(std::size_t at) {
        _out[at + 4] = SKIPPED & 0xFFU;
        _out[at + 5] = SKIPPED >> 8;
        return {};
    }

    std::string_view layer(std::span<const unsigned char> body, std::size_t at) {
        if (_layers >= MAX_LAYERS) return "more than 64 layers";
        Cursor c(body);
        const unsigned flags = c.u16();
        const unsigned type = c.u16();
        const unsigned level = c.u16();
        c.skip(4); // the default width and height, which nothing reads
        const std::size_t blend = c.at();
        c.u16();
        c.u8(); // opacity
        c.skip(3);
        c.string();
        if (type == 2) return "a tilemap layer, which cute_aseprite cannot read";
        if (type > 1) return "a layer of a kind Aseprite does not write";
        if (!c.whole()) {
            return (_flags & HEADER_LAYER_UUIDS) != 0
                ? "layer UUIDs, which cute_aseprite cannot read"
                : SHORT;
        }
        if (flags > LAYER_FLAGS) return "layer flags Aseprite does not write";
        // cute_aseprite keeps the last layer of every level, and a layer's
        // parent is the last one a level up: one that is not there is a
        // pointer nobody wrote.
        if (level >= static_cast<unsigned>(MAX_LAYERS) ||
            (level > 0 && !_levels[level - 1]))
            return "a layer nested under no layer";
        _levels[level] = true;
        ++_layers;
        _out[at + blend] = 0;
        _out[at + blend + 1] = 0;
        return {};
    }

    std::string_view cel(std::span<const unsigned char> body) {
        if (_cels.size() - _frame_cels.back() >= static_cast<std::size_t>(MAX_LAYERS))
            return "more than 64 cels in a frame";
        Cursor c(body);
        Cel cel;
        cel.layer = static_cast<int>(c.u16());
        cel.x = c.s16();
        cel.y = c.s16();
        c.u8(); // opacity
        const unsigned type = c.u16();
        c.skip(7);
        if (type == 1) {
            cel.link = static_cast<int>(c.u16());
            if (!c.whole()) return SHORT;
            _cels.push_back(cel);
            return {};
        }
        if (type == 3) return "a tilemap cel, which cute_aseprite cannot read";
        if (type != 0 && type != 2) return "a cel of a kind Aseprite does not write";
        cel.w = static_cast<int>(c.u16());
        cel.h = static_cast<int>(c.u16());
        if (!c.ok()) return SHORT;
        if (cel.w == 0 || cel.h == 0) return "a cel of no size";
        const std::int64_t pixels = std::int64_t{ cel.w } * cel.h * _bpp;
        if (const std::string_view why = charge(pixels); !why.empty()) return why;
        if (type == 0) {
            c.skip(static_cast<std::size_t>(pixels));
            if (!c.whole()) return SHORT;
        } else {
            const unsigned method = c.u8();
            const unsigned flags = c.u8();
            if (!c.ok()) return SHORT;
            // What cute_aseprite asserts: deflate, a window it can hold, no
            // preset dictionary.
            if ((method & 0x0FU) != 8 || (method & 0xF0U) > 0x70U || (flags & 0x20U) != 0)
                return "a compressed cel that is not zlib's deflate";
            if (const std::string_view why = inflates_to(c.rest(), pixels); !why.empty())
                return why;
        }
        _cels.push_back(cel);
        return {};
    }

    std::string_view tags(std::span<const unsigned char> body) {
        // cute_aseprite keeps the second chunk's tags, and loses the names of
        // the first.
        if (_tags) return "two tags chunks";
        _tags = true;
        Cursor c(body);
        const unsigned count = c.u16();
        c.skip(8);
        if (count > static_cast<unsigned>(MAX_TAGS)) return "more than 255 tags";
        for (unsigned k = 0; k < count; ++k) {
            const unsigned from = c.u16();
            const unsigned to = c.u16();
            const unsigned direction = c.u8();
            c.skip(2 + 6 + 3 + 1); // repeat, reserved, the old colour, a zero
            c.string();
            if (!c.ok()) return SHORT;
            // Forward, reverse, ping-pong, ping-pong reverse: one past is
            // outside cute_aseprite's enum.
            if (direction > 3) return "a tag direction Aseprite does not write";
            if (from > to || std::cmp_greater_equal(to, _frames))
                return "a tag outside the frames";
        }
        return c.whole() ? std::string_view{} : SHORT;
    }

    // Indexed colour only: the palette is what the pixels mean.
    std::string_view palette(std::span<const unsigned char> body) {
        Cursor c(body);
        const std::uint32_t size = c.u32();
        const auto first = static_cast<std::int32_t>(c.u32());
        const auto last = static_cast<std::int32_t>(c.u32());
        c.skip(8);
        if (!c.ok()) return SHORT;
        if (size > static_cast<std::uint32_t>(MAX_PALETTE))
            return "a palette past 1024 colours";
        _palette = static_cast<int>(size);
        if (first <= last && (first < 0 || last >= MAX_PALETTE))
            return "a palette colour past the 1024 there can be";
        for (std::int32_t k = first; k <= last; ++k) {
            const unsigned named = c.u16();
            c.skip(4);
            if (named != 0) c.string();
            if (!c.ok()) return SHORT;
            if (const std::string_view why = recolour(k, named != 0); !why.empty())
                return why;
        }
        return c.whole() ? std::string_view{} : SHORT;
    }

    // The old palette chunk, of a frame with no new one.
    std::string_view old_palette(std::span<const unsigned char> body) {
        Cursor c(body);
        const unsigned packets = c.u16();
        for (unsigned p = 0; p < packets; ++p) {
            const unsigned skip = c.u8();
            unsigned count = c.u8();
            if (count == 0) count = 256;
            c.skip(3 * std::size_t{ count });
            if (!c.ok()) return SHORT;
            for (unsigned l = 0; l < count; ++l) {
                const std::string_view why = recolour(static_cast<int>(skip + l), false);
                if (!why.empty()) return why;
            }
            _palette = static_cast<int>(skip + count);
        }
        return c.whole() ? std::string_view{} : SHORT;
    }

    // A colour written over entry `k`. One that had a name loses it -- the
    // pointer is overwritten, and cute_aseprite never frees it.
    std::string_view recolour(int k, bool named) {
        auto &entry = _named[static_cast<std::size_t>(k)];
        if (entry) return "a named palette colour written over";
        entry = named;
        return {};
    }

    std::string_view finish() {
        // cute_aseprite frees the names of the palette's first `size` colours.
        for (std::size_t k = static_cast<std::size_t>(std::max(_palette, 0));
             k < _named.size(); ++k) {
            if (_named[k]) return "a named palette colour past the palette's size";
        }
        _frame_cels.push_back(_cels.size());
        std::vector<int> shown(_cels.size(), -1);
        if (const std::string_view why = resolve_links(shown); !why.empty()) return why;
        // What the blend costs: every cel, clipped to the canvas, as the one
        // it shows. cute_aseprite blends the visible ones; counting all is
        // the bound.
        std::int64_t blended = 0;
        for (std::size_t i = 0; i < _cels.size(); ++i) {
            const Cel &cel = _cels[static_cast<std::size_t>(shown[i])];
            const std::int64_t w =
                std::min<std::int64_t>(_width, std::int64_t{ cel.x } + cel.w) -
                std::max(cel.x, 0);
            const std::int64_t h =
                std::min<std::int64_t>(_height, std::int64_t{ cel.y } + cel.h) -
                std::max(cel.y, 0);
            if (w > 0 && h > 0) blended += w * h;
            if (blended > MAX_BLEND_PIXELS) return TOO_BIG;
        }
        return {};
    }

    // Every cel's layer, and the cel each one shows: itself, or -- linked --
    // the first cel on its layer in the frame it names, followed until one is
    // not linked. A cel a link lands on is followed once and remembered, and
    // one landed on again while it is being followed is a circle, which
    // cute_aseprite would go round forever.
    std::string_view resolve_links(std::vector<int> &shown) {
        constexpr int UNSEEN = -2;
        constexpr int FOLLOWING = -1;
        std::vector<int> landed(_cels.size(), UNSEEN);
        std::vector<std::size_t> path;
        for (std::size_t i = 0; i < _cels.size(); ++i) {
            if (_cels[i].layer >= _layers)
                return "a cel of a layer the file does not have";
            if (_cels[i].link < 0) shown[i] = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < _cels.size(); ++i) {
            if (_cels[i].link < 0) continue;
            const int layer = _cels[i].layer;
            int frame = _cels[i].link;
            int result = -1;
            path.clear();
            for (;;) {
                if (frame >= _frames)
                    return "a linked cel to a frame the file does not have";
                const std::size_t begin = _frame_cels[static_cast<std::size_t>(frame)];
                const std::size_t end = _frame_cels[static_cast<std::size_t>(frame) + 1];
                std::size_t on = end;
                for (std::size_t j = begin; j < end; ++j) {
                    if (_cels[j].layer == layer) {
                        on = j;
                        break;
                    }
                }
                if (on == end) return "a linked cel to a frame with nothing on its layer";
                if (_cels[on].link < 0) {
                    result = static_cast<int>(on);
                    break;
                }
                if (landed[on] >= 0) {
                    result = landed[on];
                    break;
                }
                if (landed[on] == FOLLOWING)
                    return "linked cels that link round in a circle";
                landed[on] = FOLLOWING;
                path.push_back(on);
                frame = _cels[on].link;
            }
            for (const std::size_t on : path) landed[on] = result;
            shown[i] = result;
        }
        return {};
    }

    std::span<const unsigned char> _file;
    std::vector<unsigned char> &_out;
    int _frames = 0;
    int _width = 0;
    int _height = 0;
    int _bpp = 4;
    bool _indexed = false;
    std::uint32_t _flags = 0;
    std::int64_t _bytes = 0; // what cute_aseprite will allocate, so far
    int _layers = 0;
    std::array<bool, MAX_LAYERS> _levels{}; // the nesting levels a layer has been on
    bool _tags = false;
    std::array<bool, MAX_PALETTE> _named{}; // the palette colours holding a name
    int _palette = 0; // the palette's size, as cute_aseprite last set it
    std::vector<Cel> _cels; // every frame's, in order
    std::vector<std::size_t> _frame_cels; // where each frame's cels start in _cels
    std::vector<SheetSlice> _slices; // read_slice()'s, in the file's order
};

} // namespace

std::string_view prepare_aseprite(std::span<const unsigned char> file,
                                  std::vector<unsigned char> &out) {
    std::vector<SheetSlice> slices;
    return prepare_aseprite(file, out, slices);
}

std::string_view prepare_aseprite(std::span<const unsigned char> file,
                                  std::vector<unsigned char> &out,
                                  std::vector<SheetSlice> &slices) {
    out.clear();
    slices.clear();
    Walk walk(file, out);
    const std::string_view why = walk.run();
    if (!why.empty()) {
        out.clear();
        return why;
    }
    slices = walk.take_slices();
    return why;
}

} // namespace rmp::animation::detail
