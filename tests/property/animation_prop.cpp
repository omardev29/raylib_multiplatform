// ---------------------------------------------------------------------------
// Sprite sheets -- the Aseprite reader (src/rmp/aseprite.cpp's walk, then
// cute_aseprite, then parse_sheet() in src/rmp/animation.cpp) and the
// animation clock -- held to the campaign's rules:
//
//   - a well-formed .aseprite of any shape the generator below writes --
//     every colour depth, groups, raw cels and compressed ones of fixed and
//     dynamic codes, linked cels, every tag direction, and the chunks a sheet
//     does not use -- reads back the frames, durations and tags it was
//     written with;
//   - what the walk hands cute_aseprite draws the pixels the file draws;
//   - one change anywhere in such a file -- a bit, a field set to an edge, a
//     cut, bytes again -- is refused, or read with every promise kept, and
//     what the walk hands over walks to itself;
//   - load_sheet(name) reads a file as parse_sheet() reads its bytes, and a
//     name that is not there is an empty sheet;
//   - SheetData::frame() and tag() answer any index;
//   - the clock: a played tag never shows a frame outside it, whatever the
//     step and the speed; steps split any way show what one step shows; a
//     speed that is not a number warns once and never reaches the clock; a
//     frame set anywhere comes back into the tag; play() of a name the sheet
//     does not have changes nothing, and warns once a name.
//
// rmp::Sprite's six functions are exercised here too; their waivers are in
// object.h's block of tests/property/waivers.txt, package P5's, and leave it
// at integration.
//
// covers: rmp::SheetData::frame(int) const
// covers: rmp::SheetData::frame_count() const
// covers: rmp::SheetData::tag(int) const
// covers: rmp::SheetData::tag_count() const
// covers: rmp::assets::load_sheet(std::string_view)
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/animation_internal.h"
#include "../../src/rmp/internal.h"
#include "../aseprite_builder.h"

#include <rmp/assets.h>
#include <rmp/object.h>

#include <cute_aseprite.h>
#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace ase = rmp::test::aseprite;

// ---- files -----------------------------------------------------------------

// What the file besides the picture holds.
enum class Extras {
    // Only what cute_aseprite reads in the original file without a fault --
    // so the original can be read too, and the pictures compared.
    SAFE,
    // Everything: user data anywhere (Aseprite 1.3's sprite user data and
    // properties), slices, ICC profiles, named palette colours, blend modes.
    ALL,
};

// A generated file and what parse_sheet() must read out of it.
struct Written {
    ase::File file;
    std::vector<float> seconds;
    std::vector<rmp::SheetTag> tags;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Written &written, std::ostream &os) {
    os << written.file.width << "x" << written.file.height << " depth "
       << written.file.depth << ", " << written.file.frames.size() << " frame(s), "
       << written.tags.size() << " tag(s), " << written.file.bytes().size() << " bytes";
}

// A dynamic block that writes every byte as a literal: 9 bits a byte, 2 for
// the end, an incomplete code -- as cute_aseprite's table can hold.
std::vector<unsigned char> dynamic_stream(const std::vector<unsigned char> &pixels) {
    std::vector<int> literal(286, 9);
    literal[256] = 2;
    ase::Stream stream;
    stream.dynamic_block(true, literal, std::vector<int>(30, 5));
    stream.literals(pixels);
    stream.end();
    return stream.zlib();
}

// A tag's name as the file may write it, NUL and all: cute_aseprite reads
// it as a C string, so what parse_sheet() keeps ends at the first NUL.
rc::Gen<std::string> tag_name() {
    return rc::gen::oneOf(
        rc::gen::element<std::string>("walk", "idle", "", "Walk", "a b"),
        rmp::test::gen::hostile_string());
}

rc::Gen<Written> files(Extras extras) {
    return rc::gen::exec([extras] {
        Written out;
        ase::File &file = out.file;
        file.depth = static_cast<std::uint16_t>(*rc::gen::element(8, 16, 32));
        file.width = *rc::gen::inRange<std::uint16_t>(1, 17);
        file.height = *rc::gen::inRange<std::uint16_t>(1, 17);
        file.speed = static_cast<std::uint16_t>(*rc::gen::element(0, 1, 100, 250));
        file.transparent = *rc::gen::arbitrary<std::uint8_t>();
        const int bpp = file.bytes_per_pixel();
        const int frames = *rc::gen::inRange(1, 7);
        file.frames.resize(static_cast<std::size_t>(frames));
        for (ase::Frame &frame : file.frames) {
            frame.duration = *rc::gen::oneOf(rc::gen::just<std::uint16_t>(0),
                                             rc::gen::inRange<std::uint16_t>(1, 1000),
                                             rc::gen::just<std::uint16_t>(65535));
            const int ms = frame.duration > 0 ? frame.duration : file.speed;
            out.seconds.push_back((ms > 0 ? static_cast<float>(ms) : 1.0f) / 1000.0f);
        }
        std::vector<ase::Chunk> &first = file.frames[0].chunks;

        // The palette, as Aseprite writes it: the new chunk, and the old one
        // after it. An indexed sheet's pixels are its colours; any other's
        // palette is not read at all.
        const bool named_colours = extras == Extras::ALL && *rc::gen::arbitrary<bool>();
        if (file.depth == 8 || *rc::gen::arbitrary<bool>()) {
            const int size = *rc::gen::inRange(1, 33);
            std::vector<ase::PaletteEntry> colours;
            colours.reserve(static_cast<std::size_t>(size));
            for (int i = 0; i < size; ++i) {
                colours.push_back(
                    { *rc::gen::arbitrary<std::uint8_t>(),
                      *rc::gen::arbitrary<std::uint8_t>(),
                      *rc::gen::arbitrary<std::uint8_t>(), 255,
                      named_colours && i % 3 == 0 ? "c" + std::to_string(i) : "" });
            }
            first.push_back(ase::palette(static_cast<std::uint32_t>(size), 0, colours));
            if (*rc::gen::arbitrary<bool>())
                first.push_back(ase::old_palette(0, colours));
        }

        // The layers: one, or a group and its children a level down.
        const int layers = *rc::gen::inRange(1, 5);
        const bool grouped = layers > 1 && *rc::gen::arbitrary<bool>();
        for (int l = 0; l < layers; ++l) {
            ase::LayerOptions layer;
            layer.flags = *rc::gen::weightedOneOf<std::uint16_t>(
                { { 4, rc::gen::just<std::uint16_t>(1) },
                  { 1, rc::gen::inRange<std::uint16_t>(0, 0x80) } });
            layer.type = grouped && l == 0 ? 1 : 0;
            layer.level = grouped && l > 0 ? 1 : 0;
            layer.blend =
                extras == Extras::ALL ? *rc::gen::inRange<std::uint16_t>(0, 19) : 0;
            layer.opacity = *rc::gen::arbitrary<std::uint8_t>();
            layer.name = "layer " + std::to_string(l);
            first.push_back(ase::layer(layer));
        }

        // The tags.
        const int tags = *rc::gen::inRange(0, 5);
        std::vector<ase::Tag> list;
        for (int t = 0; t < tags; ++t) {
            const int from = *rc::gen::inRange(0, frames);
            const int to = *rc::gen::inRange(from, frames);
            const int direction = *rc::gen::inRange(0, 4);
            const std::string name = *tag_name();
            list.push_back({ from, to, direction, name });
            rmp::SheetTag ours;
            ours.name = name.substr(0, name.find('\0'));
            ours.from = from;
            ours.to = to;
            ours.ping_pong = direction == 2 || direction == 3;
            ours.reverse = direction == 1 || direction == 3;
            out.tags.push_back(ours);
        }
        if (tags > 0) first.push_back(ase::tags(list));

        // The cels: on a layer that is not a group, raw, compressed either
        // way, or linked to a frame before with a cel on the same layer.
        std::vector<std::vector<bool>> drawn(
            static_cast<std::size_t>(frames),
            std::vector<bool>(static_cast<std::size_t>(layers)));
        for (int f = 0; f < frames; ++f) {
            for (int l = grouped ? 1 : 0; l < layers; ++l) {
                if (!*rc::gen::weightedElement<bool>({ { 3, true }, { 1, false } }))
                    continue;
                std::vector<int> earlier;
                for (int g = 0; g < f; ++g) {
                    if (drawn[static_cast<std::size_t>(g)][static_cast<std::size_t>(l)])
                        earlier.push_back(g);
                }
                const int kind = *rc::gen::inRange(0, earlier.empty() ? 3 : 4);
                ase::Chunk cel;
                if (kind == 3) {
                    cel = ase::linked_cel(l, *rc::gen::elementOf(earlier));
                } else {
                    const int x =
                        *rc::gen::inRange(-static_cast<int>(file.width), file.width + 1);
                    const int y = *rc::gen::inRange(-static_cast<int>(file.height),
                                                    file.height + 1);
                    const int w = *rc::gen::inRange(1, 2 * file.width + 1);
                    const int h = *rc::gen::inRange(1, 2 * file.height + 1);
                    const auto pixels = *rc::gen::container<std::vector<unsigned char>>(
                        static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                            static_cast<std::size_t>(bpp),
                        rc::gen::arbitrary<unsigned char>());
                    if (kind == 0) {
                        cel = ase::raw_cel(l, x, y, w, h, pixels);
                    } else if (kind == 1) {
                        cel = ase::compressed_cel(
                            l, x, y, w, h, ase::zlib(pixels, ase::Deflate::FIXED, bpp));
                    } else {
                        cel = ase::compressed_cel(l, x, y, w, h, dynamic_stream(pixels));
                    }
                }
                cel.body[6] = *rc::gen::arbitrary<std::uint8_t>(); // its opacity
                file.frames[static_cast<std::size_t>(f)].chunks.push_back(cel);
                drawn[static_cast<std::size_t>(f)][static_cast<std::size_t>(l)] = true;
            }
        }

        // What a sheet does not use, anywhere: an sRGB profile, a cel extra,
        // an external files chunk -- and, when everything goes, user data
        // (first in the file too), slices and ICC profiles.
        const int more = *rc::gen::inRange(0, 4);
        for (int i = 0; i < more; ++i) {
            ase::Frame &frame =
                file.frames[*rc::gen::inRange<std::size_t>(0, file.frames.size())];
            const std::size_t at =
                *rc::gen::inRange<std::size_t>(0, frame.chunks.size() + 1);
            ase::Bytes srgb;
            srgb.u16(1).u16(0).u32(0).zeros(8);
            ase::Bytes external;
            external.u32(1).zeros(8).u32(1).u8(0).zeros(7).string("palette.aseprite");
            std::vector<ase::Chunk> choices{ { ase::COLOR_PROFILE, srgb.data },
                                             ase::cel_extra(),
                                             { ase::EXTERNAL_FILES, external.data } };
            if (extras == Extras::ALL) {
                choices.push_back(ase::user_data(*tag_name()));
                choices.push_back(ase::slice("slice", *rc::gen::inRange(0, 3)));
                choices.push_back(
                    ase::icc_profile(*rc::gen::inRange<std::uint32_t>(0, 16)));
            }
            frame.chunks.insert(frame.chunks.begin() + static_cast<std::ptrdiff_t>(at),
                                *rc::gen::elementOf(choices));
        }
        return out;
    });
}

// ---- reading -----------------------------------------------------------------

bool parse(const std::vector<unsigned char> &bytes, rmp::SheetData &out) {
    return rmp::animation::detail::parse_sheet(bytes.data(),
                                               static_cast<int>(bytes.size()), &out);
}

// cute_aseprite's pictures of `bytes`, RGBA, every frame -- and its size.
struct Pictures {
    int width = 0;
    int height = 0;
    std::vector<std::vector<unsigned char>> frames;
    bool operator==(const Pictures &) const = default;
};

Pictures pictures_of(const std::vector<unsigned char> &bytes) {
    Pictures out;
    ase_t *read = cute_aseprite_load_from_memory(bytes.data(),
                                                 static_cast<int>(bytes.size()), nullptr);
    RC_ASSERT(read != nullptr);
    out.width = read->w;
    out.height = read->h;
    for (int f = 0; f < read->frame_count; ++f) {
        std::vector<unsigned char> frame;
        for (int i = 0; i < read->w * read->h; ++i) {
            const ase_color_t &c = read->frames[f].pixels[i];
            frame.insert(frame.end(), { c.r, c.g, c.b, c.a });
        }
        out.frames.push_back(std::move(frame));
    }
    cute_aseprite_free(read);
    return out;
}

// What every sheet the reader accepts keeps, whatever file it came from.
void check_sheet(const rmp::SheetData &sheet) {
    RC_ASSERT(sheet.width > 0);
    RC_ASSERT(sheet.height > 0);
    RC_ASSERT(sheet.frame_count() > 0);
    for (int i = 0; i < sheet.frame_count(); ++i) {
        const rmp::SheetFrame &frame = sheet.frame(i);
        RC_ASSERT(std::isfinite(frame.seconds));
        RC_ASSERT(frame.seconds > 0);
        RC_ASSERT(frame.source.x == static_cast<float>(i * sheet.width));
        RC_ASSERT(frame.source.width == static_cast<float>(sheet.width));
        RC_ASSERT(frame.source.height == static_cast<float>(sheet.height));
    }
    for (int i = 0; i < sheet.tag_count(); ++i) {
        RC_ASSERT(sheet.tag(i).from >= 0);
        RC_ASSERT(sheet.tag(i).from <= sheet.tag(i).to);
        RC_ASSERT(sheet.tag(i).to < sheet.frame_count());
    }
}

// One change to a file: a bit, a field set to an edge, a cut, bytes again,
// bytes put in -- the declared size made to match, or not.
std::vector<unsigned char> changed(std::vector<unsigned char> bytes) {
    const auto at = [&bytes](std::size_t width) {
        return bytes.size() > width
            ? *rc::gen::inRange<std::size_t>(0, bytes.size() - width)
            : std::size_t{ 0 };
    };
    switch (*rc::gen::inRange(0, 6)) {
        case 0: {
            const std::size_t byte = at(0);
            bytes[byte] ^= static_cast<unsigned char>(1U << *rc::gen::inRange(0, 8));
            return bytes;
        }
        case 1: {
            const std::size_t byte = at(2);
            const unsigned value =
                *rc::gen::element(0U, 1U, 2U, 3U, 4U, 63U, 64U, 65U, 0x7FU, 0x80U, 0xFFU,
                                  0x100U, 0x7FFFU, 0x8000U, 0xFFFFU);
            bytes[byte] = static_cast<unsigned char>(value & 0xFFU);
            bytes[byte + 1] = static_cast<unsigned char>(value >> 8);
            return bytes;
        }
        case 2: {
            const std::size_t byte = at(4);
            const auto size = static_cast<std::uint32_t>(bytes.size());
            const std::uint32_t value = *rc::gen::element<std::uint32_t>(
                0U, 1U, 5U, 6U, 7U, 16U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFFU, size,
                size + 1U);
            for (int b = 0; b < 4; ++b) {
                bytes[byte + static_cast<std::size_t>(b)] =
                    static_cast<unsigned char>((value >> (8 * b)) & 0xFFU);
            }
            return bytes;
        }
        case 3:
            bytes.resize(at(0));
            break;
        case 4: {
            const std::size_t from = at(0);
            const std::size_t count = *rc::gen::inRange<std::size_t>(1, 64);
            const std::size_t to = at(0);
            const std::vector<unsigned char> copy(
                bytes.begin() + static_cast<std::ptrdiff_t>(from),
                bytes.begin() +
                    static_cast<std::ptrdiff_t>(std::min(bytes.size(), from + count)));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(to), copy.begin(),
                         copy.end());
            break;
        }
        default: {
            const auto more = *rc::gen::container<std::vector<unsigned char>>(
                rc::gen::arbitrary<unsigned char>());
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at(0)), more.begin(),
                         more.end());
            break;
        }
    }
    if (bytes.size() >= 4 && *rc::gen::arbitrary<bool>()) {
        const auto size = static_cast<std::uint32_t>(bytes.size());
        for (int b = 0; b < 4; ++b) {
            bytes[static_cast<std::size_t>(b)] =
                static_cast<unsigned char>((size >> (8 * b)) & 0xFFU);
        }
    }
    return bytes;
}

// A folder of its own for load_sheet(), as the resources root for one case.
struct Resources {
    std::filesystem::path folder;
    std::string previous;
    Resources() : previous(rmp::assets::detail::resources_root()) {
        static int made = 0;
        std::error_code error;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        folder = std::filesystem::temp_directory_path(error) /
            ("rmp-animation-prop-" + std::to_string(now) + "-" + std::to_string(++made));
        std::filesystem::create_directories(folder, error);
        const std::string root = (folder / "").string();
        rmp::assets::detail::set_resources_root(root.c_str());
    }
    ~Resources() {
        rmp::assets::detail::set_resources_root(previous.c_str());
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Resources(const Resources &) = delete;
    Resources &operator=(const Resources &) = delete;
    void write(const std::string &name, const std::vector<unsigned char> &bytes) const {
        std::ofstream file(folder / name, std::ios::binary);
        for (const unsigned char byte : bytes) file.put(static_cast<char>(byte));
    }
};

bool same_sheet(const rmp::SheetData &a, const rmp::SheetData &b) {
    if (a.width != b.width || a.height != b.height ||
        a.frame_count() != b.frame_count() || a.tag_count() != b.tag_count())
        return false;
    for (int i = 0; i < a.frame_count(); ++i) {
        if (a.frame(i).seconds != b.frame(i).seconds ||
            a.frame(i).source.x != b.frame(i).source.x)
            return false;
    }
    for (int i = 0; i < a.tag_count(); ++i) {
        const rmp::SheetTag &x = a.tag(i);
        const rmp::SheetTag &y = b.tag(i);
        if (x.name != y.name || x.from != y.from || x.to != y.to ||
            x.ping_pong != y.ping_pong || x.reverse != y.reverse)
            return false;
    }
    return true;
}

// ---- the clock -------------------------------------------------------------

// A sheet built in memory: frames of the holds given, the tags given.
struct Built {
    std::vector<float> holds;
    std::vector<rmp::SheetTag> tags;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Built &built, std::ostream &os) {
    os << "holds {";
    for (const float hold : built.holds) os << " " << hold;
    os << " }, tags {";
    for (const rmp::SheetTag &tag : built.tags) {
        os << " " << tag.from << ".." << tag.to << (tag.ping_pong ? " ping-pong" : "")
           << (tag.reverse ? " reverse" : "");
    }
    os << " }";
}

// Holds in sixteenths of a second, and tags inside the frames, every
// direction: every sum and every lap the clock takes is exact in a float, so
// two ways of stepping the same time must agree to the bit.
rc::Gen<Built> built() {
    return rc::gen::exec([] {
        Built out;
        const int frames = *rc::gen::inRange(1, 7);
        for (int i = 0; i < frames; ++i)
            out.holds.push_back(static_cast<float>(*rc::gen::inRange(1, 9)) / 16.0f);
        const int tags = *rc::gen::inRange(1, 4);
        for (int t = 0; t < tags; ++t) {
            rmp::SheetTag tag;
            tag.name = "t" + std::to_string(t);
            tag.from = *rc::gen::inRange(0, frames);
            tag.to = *rc::gen::inRange(tag.from, frames);
            tag.ping_pong = *rc::gen::arbitrary<bool>();
            tag.reverse = *rc::gen::arbitrary<bool>();
            out.tags.push_back(tag);
        }
        return out;
    });
}

// A sprite holding a sheet of `built`, released when it goes.
struct Animated {
    rmp::Sprite sprite;
    explicit Animated(const Built &built) {
        rmp::SheetData data{};
        data.width = 4;
        data.height = 4;
        for (std::size_t i = 0; i < built.holds.size(); ++i) {
            data.frames.push_back(
                { Rectangle{ static_cast<float>(i * 4), 0, 4, 4 }, built.holds[i] });
        }
        data.tags = built.tags;
        sprite.sheet = rmp::SpriteSheet{ rmp::detail::adopt(
            rmp::detail::ResourceKind::SHEET, std::move(data)) };
    }
    ~Animated() { sprite.sheet = rmp::SpriteSheet{}; }
    Animated(const Animated &) = delete;
    Animated &operator=(const Animated &) = delete;
};

void step(rmp::Sprite &sprite, float seconds) {
    rmp::animation::detail::advance(sprite, seconds);
}

// The frames a tag covers.
bool inside(const rmp::SheetTag &tag, int frame) {
    return frame >= tag.from && frame <= tag.to;
}

} // namespace

TEST_SUITE("property: animation") {
    TEST_CASE("a well-formed sheet reads back what it was written with") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "a well-formed .aseprite reads back its frames, durations and tags", [] {
                const Written written =
                    *files(*rc::gen::element(Extras::SAFE, Extras::ALL));
                rmp::SheetData sheet;
                RC_ASSERT(parse(written.file.bytes(), sheet));
                RC_ASSERT(sheet.width == written.file.width);
                RC_ASSERT(sheet.height == written.file.height);
                RC_ASSERT(sheet.frame_count() ==
                          static_cast<int>(written.seconds.size()));
                for (int i = 0; i < sheet.frame_count(); ++i) {
                    RC_ASSERT(sheet.frame(i).seconds ==
                              written.seconds[static_cast<std::size_t>(i)]);
                }
                RC_ASSERT(sheet.tag_count() == static_cast<int>(written.tags.size()));
                for (int i = 0; i < sheet.tag_count(); ++i) {
                    const rmp::SheetTag &want = written.tags[static_cast<std::size_t>(i)];
                    RC_ASSERT(sheet.tag(i).name == want.name);
                    RC_ASSERT(sheet.tag(i).from == want.from);
                    RC_ASSERT(sheet.tag(i).to == want.to);
                    RC_ASSERT(sheet.tag(i).ping_pong == want.ping_pong);
                    RC_ASSERT(sheet.tag(i).reverse == want.reverse);
                }
                check_sheet(sheet);
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("the copy the walk hands over draws what the file draws") {
        rmp::test::check(
            "what the walk hands cute_aseprite draws the pixels the file draws", [] {
                const std::vector<unsigned char> bytes =
                    (*files(Extras::SAFE)).file.bytes();
                std::vector<unsigned char> copy;
                RC_ASSERT(rmp::animation::detail::prepare_aseprite(bytes, copy).empty());
                RC_ASSERT(copy.size() == bytes.size());
                RC_ASSERT(pictures_of(copy) == pictures_of(bytes));
            });
    }

    TEST_CASE("a file changed in one place is refused, or read with every promise kept") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "one change to a .aseprite is refused or read with every promise kept", [] {
                const std::vector<unsigned char> bytes = changed(
                    (*files(*rc::gen::element(Extras::SAFE, Extras::ALL))).file.bytes());
                const int reports = rmp::detail::report_count();
                std::vector<unsigned char> copy;
                const bool accepted =
                    rmp::animation::detail::prepare_aseprite(bytes, copy).empty();
                RC_ASSERT(accepted == !copy.empty());
                rmp::SheetData sheet;
                RC_ASSERT(parse(bytes, sheet) == accepted);
                RC_ASSERT(rmp::detail::report_count() == reports);
                if (!accepted) return;
                std::vector<unsigned char> again;
                RC_ASSERT(rmp::animation::detail::prepare_aseprite(copy, again).empty());
                RC_ASSERT(again == copy);
                check_sheet(sheet);
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("load_sheet reads a file as parse_sheet reads its bytes") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "load_sheet of a file is parse_sheet of its bytes, and of no file nothing",
            [] {
                const std::vector<unsigned char> good =
                    (*files(*rc::gen::element(Extras::SAFE, Extras::ALL))).file.bytes();
                const std::vector<unsigned char> maybe =
                    *rc::gen::oneOf(rc::gen::just(good),
                                    rc::gen::exec([&good] { return changed(good); }));
                const Resources resources;
                resources.write("sheet.aseprite", maybe);
                rmp::SheetData parsed;
                const bool readable = parse(maybe, parsed);
                {
                    const rmp::SpriteSheet sheet =
                        rmp::assets::load_sheet("sheet.aseprite");
                    RC_ASSERT(sheet.valid() == readable);
                    if (readable) {
                        RC_ASSERT(same_sheet(sheet.raw(), parsed));
                        // No GPU here: the texture is the hole, the rest is there.
                        RC_ASSERT(sheet.raw().texture.id == 0U);
                    }
                    // The file's name, a NUL, and more is no file's name: not
                    // the sheet above, cached under the part before the NUL,
                    // and not the file read again.
                    RC_ASSERT(
                        !rmp::assets::load_sheet(std::string("sheet.aseprite\0x", 16))
                             .valid());
                }
                RC_ASSERT(!rmp::assets::load_sheet("missing.aseprite").valid());
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("frame() and tag() answer any index") {
        rmp::test::check(
            "SheetData::frame and tag are the element, or the empty one", [] {
                rmp::SheetData sheet;
                const int frames = *rc::gen::inRange(0, 5);
                for (int i = 0; i < frames; ++i)
                    sheet.frames.push_back({ { 1, 2, 3, 4 }, 0.5f });
                const int tags = *rc::gen::inRange(0, 5);
                for (int i = 0; i < tags; ++i)
                    sheet.tags.push_back({ "t", 1, 2, true, true });
                RC_ASSERT(sheet.frame_count() == frames);
                RC_ASSERT(sheet.tag_count() == tags);
                const int index =
                    *rc::gen::oneOf(rc::gen::element(INT_MIN, -1, 0, 1, 4, 5, INT_MAX),
                                    rc::gen::arbitrary<int>());
                const rmp::SheetFrame &frame = sheet.frame(index);
                if (index >= 0 && index < frames) {
                    RC_ASSERT(&frame == &sheet.frames[static_cast<std::size_t>(index)]);
                } else {
                    RC_ASSERT(frame.seconds == 0.0f);
                    RC_ASSERT(frame.source.width == 0.0f);
                }
                const rmp::SheetTag &tag = sheet.tag(index);
                if (index >= 0 && index < tags) {
                    RC_ASSERT(&tag == &sheet.tags[static_cast<std::size_t>(index)]);
                } else {
                    RC_ASSERT(tag.name.empty());
                    RC_ASSERT(tag.from == 0);
                    RC_ASSERT(tag.to == 0);
                    RC_ASSERT(!tag.ping_pong);
                    RC_ASSERT(!tag.reverse);
                }
            });
    }

    TEST_CASE("a played tag never shows a frame outside it") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "the frame stays inside its tag for any tag, step and speed", [] {
                const Built sheet = *built();
                const Animated animated(sheet);
                rmp::Sprite sprite = animated.sprite;
                const auto t = *rc::gen::inRange<std::size_t>(0, sheet.tags.size());
                sprite.play(sheet.tags[t].name, *rc::gen::arbitrary<bool>());
                const rmp::SheetTag &tag = sheet.tags[t];
                RC_ASSERT(inside(tag, sprite.frame_index()));
                const int steps = *rc::gen::inRange(1, 20);
                for (int i = 0; i < steps; ++i) {
                    sprite.speed = *rmp::test::gen::hostile_float();
                    step(sprite, *rmp::test::gen::hostile_float());
                    RC_ASSERT(inside(tag, sprite.frame_index()));
                    RC_ASSERT(std::isfinite(sprite.ours.elapsed));
                    RC_ASSERT(sprite.ours.elapsed >= 0);
                }
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("steps split any way show what one step shows") {
        rmp::test::check("split steps show the frame one whole step shows", [] {
            const Built sheet = *built();
            const Animated whole(sheet);
            const Animated split(sheet);
            const auto t = *rc::gen::inRange<std::size_t>(0, sheet.tags.size());
            const bool loop = *rc::gen::arbitrary<bool>();
            const float speed = *rc::gen::element(1.0f, 2.0f, -1.0f, -4.0f, 0.5f);
            rmp::Sprite a = whole.sprite;
            rmp::Sprite b = split.sprite;
            a.play(sheet.tags[t].name, loop);
            b.play(sheet.tags[t].name, loop);
            a.speed = speed;
            b.speed = speed;
            // Sixteenths of a second: a frame, a lap, a thousand laps.
            // At most 30 of them, under 2^24 sixteenths in all: exact in a float.
            const auto parts = *rc::gen::container<std::vector<int>>(
                *rc::gen::inRange<std::size_t>(1, 31),
                rc::gen::oneOf(rc::gen::inRange(1, 40), rc::gen::inRange(1000, 100000)));
            int total = 0;
            for (const int part : parts) {
                step(b, static_cast<float>(part) / 16.0f);
                total += part;
            }
            if (total > 0) step(a, static_cast<float>(total) / 16.0f);
            RC_ASSERT(a.frame_index() == b.frame_index());
            RC_ASSERT(a.finished() == b.finished());
            RC_ASSERT(a.ours.elapsed == b.ours.elapsed);
            // And from there they go on alike.
            for (int i = 0; i < 8; ++i) {
                step(a, 0.0625f);
                step(b, 0.0625f);
                RC_ASSERT(a.frame_index() == b.frame_index());
            }
        });
    }

    TEST_CASE("a speed that is not a number warns once and never reaches the clock") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "no NaN or infinity reaches the clock, and the warning comes once", [] {
                rmp::detail::reset_reports_for_tests();
                const Built sheet = *built();
                const Animated animated(sheet);
                rmp::Sprite sprite = animated.sprite;
                sprite.play(sheet.tags[0].name);
                sprite.ours.elapsed = *rmp::test::gen::hostile_float();
                bool unreadable = false;
                const int steps = *rc::gen::inRange(1, 12);
                for (int i = 0; i < steps; ++i) {
                    sprite.speed = *rmp::test::gen::hostile_float();
                    unreadable = unreadable || !std::isfinite(sprite.speed);
                    step(sprite, *rc::gen::element(0.016f, 0.25f, 3.0f));
                    RC_ASSERT(std::isfinite(sprite.ours.elapsed));
                    RC_ASSERT(sprite.ours.elapsed >= 0);
                }
                RC_ASSERT(rmp::detail::report_count() <= 1);
                if (unreadable) RC_ASSERT(rmp::detail::report_count() == 1);
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("a frame set anywhere comes back into the tag") {
        rmp::test::check(
            "after set_frame(any int), the next frame is inside the tag", [] {
                const Built sheet = *built();
                const Animated animated(sheet);
                rmp::Sprite sprite = animated.sprite;
                const auto t = *rc::gen::inRange<std::size_t>(0, sheet.tags.size());
                sprite.play(sheet.tags[t].name, *rc::gen::arbitrary<bool>());
                sprite.speed = *rc::gen::element(1.0f, -1.0f, 3.0f);
                sprite.set_frame(
                    *rc::gen::oneOf(rc::gen::element(INT_MIN, INT_MAX, -1, 0, 6, 7),
                                    rc::gen::arbitrary<int>()));
                // Longer than any frame holds: the frame set is left.
                step(sprite, 0.5625f);
                RC_ASSERT(inside(sheet.tags[t], sprite.frame_index()));
            });
    }

    TEST_CASE("play() of a name the sheet does not have changes nothing") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "play of an unknown tag keeps the frame and warns once a name", [] {
                rmp::detail::reset_reports_for_tests();
                const Built sheet = *built();
                const Animated animated(sheet);
                rmp::Sprite sprite = animated.sprite;
                sprite.play(sheet.tags[0].name);
                step(sprite, *rc::gen::element(0.0f, 0.0625f, 0.25f));
                const auto before = sprite.ours;
                const std::string playing(sprite.playing());
                const auto names = *rc::gen::container<std::vector<std::string>>(
                    rmp::test::gen::hostile_string());
                std::vector<std::string> unknown;
                for (const std::string &name : names) {
                    const auto known = std::ranges::find_if(
                        sheet.tags,
                        [&name](const rmp::SheetTag &tag) { return tag.name == name; });
                    if (known != sheet.tags.end()) continue;
                    sprite.play(name);
                    sprite.play(name);
                    if (std::ranges::find(unknown, name) == unknown.end())
                        unknown.push_back(name);
                    RC_ASSERT(sprite.ours.frame == before.frame);
                    RC_ASSERT(sprite.ours.tag == before.tag);
                    RC_ASSERT(sprite.ours.elapsed == before.elapsed);
                    RC_ASSERT(sprite.playing() == playing);
                }
                // Keyed by the name as a C string: two names that differ only
                // after a NUL are one warning.
                std::vector<std::string> keys;
                for (const std::string &name : unknown) {
                    const std::string key = name.substr(0, name.find('\0'));
                    if (std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
                }
                RC_ASSERT(rmp::detail::report_count() == static_cast<int>(keys.size()));
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("play() of a name the sheet has plays the first tag of that name") {
        rmp::test::check("play of a known tag starts it at the end it plays from", [] {
            const Built sheet = *built();
            const Animated animated(sheet);
            rmp::Sprite sprite = animated.sprite;
            const auto t = *rc::gen::inRange<std::size_t>(0, sheet.tags.size());
            sprite.play(sheet.tags[t].name);
            const rmp::SheetTag &tag = sheet.tags[t];
            RC_ASSERT(rmp::animation::detail::tag_index(sprite.sheet.raw(), tag.name) ==
                      static_cast<int>(t));
            RC_ASSERT(sprite.playing() == tag.name);
            RC_ASSERT(sprite.frame_index() == (tag.reverse ? tag.to : tag.from));
            RC_ASSERT(!sprite.finished());
            sprite.stop();
            RC_ASSERT(sprite.finished());
        });
    }
}
