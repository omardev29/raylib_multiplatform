// ---------------------------------------------------------------------------
// The rres pack, fuzzed: src/rmp/pack.cpp's reader, from memory and from the
// file a game opens.
//
// An input is a mode byte and then the pack. The mode's bits edit the pack the
// way chance never would, so the fuzzer gets past the checks that guard the
// interesting parts:
//
//   bit 0  every chunk's CRC32 made right (after bit 1);
//   bit 1  every encrypted chunk SEALED: its bytes before the trailer are
//          taken as the plaintext, encrypted with the cheap test key, and the
//          MD5 (AES) or MAC (XChaCha20) written in -- so what follows the
//          decryption is fuzzed too, not just a refused MD5.
//
// The oracle is everything pack_internal.h and internal.h promise:
//
//   - nothing crashes, nothing is undefined, nothing leaks, whatever the pack
//     (the sanitizers, and libFuzzer's -rss_limit_mb and -timeout);
//   - a lookup is the whole name: "" is no entry, and neither is a name
//     longer or shorter by a character, unless the directory has that one;
//   - what a chunk opens to obeys its own properties: a RAWD file is no
//     longer than its data, an IMGE picture has the pixels its size says;
//   - the file the game opens reads exactly what the same bytes read from
//     memory: open_pack() opens it when there is an index with entries,
//     pack_has() is find(), and pack_read() and load_data() hand back the
//     same bytes -- for the chunks that are not encrypted, since the game's
//     key stretch is Argon2i, two seconds a chunk at -O0 under ASan;
//   - reading a pack reports nothing, and close_pack() closes it.
//
// Inputs: tests/fuzz/corpus/rres/ and tests/fuzz/regressions/rres/, one pack
// of each defect the reader in rres itself had (pack.cpp lists them).
//
//   bash tools/fuzz.sh replay rres         every committed input, once
//   bash tools/fuzz.sh run rres 60         a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/pack_internal.h"
#include "../pack_writer.h"

#include <rmp/assets.h>
#include <rmp/config.h>

#include <raylib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using rmp::test::fuzz::require;
namespace pack = rmp::test::pack;
namespace rres = rmp::assets::detail::rres;

constexpr std::string_view PASSWORD = RMP_RRES_PASSWORD;

struct {
    std::filesystem::path folder;
} scratch;

void remove_scratch() {
    std::error_code error;
    std::filesystem::remove_all(scratch.folder, error);
}

rres::Key cheap(std::string_view password, const rres::Salt &salt) {
    return pack::cheap(password, salt);
}

// Bit 1: every encrypted chunk's plaintext encrypted and summed with the cheap key.
void seal(pack::Bytes &bytes) {
    for (const pack::Located &chunk : pack::chunks(bytes)) {
        const std::uint8_t cipher = bytes[chunk.info + 9];
        const std::span<std::uint8_t> data =
            std::span(bytes).subspan(chunk.data, chunk.size);
        if (cipher == pack::CIPHER_AES && data.size() >= 32) {
            const auto body = data.first(data.size() - 32);
            pack::Salt salt{};
            std::ranges::copy(data.subspan(body.size(), 16), salt.begin());
            const auto sum = pack::md5(body);
            pack::aes_ctr(body, pack::cheap(PASSWORD, salt));
            std::ranges::copy(
                sum, data.begin() + static_cast<std::ptrdiff_t>(body.size() + 16));
        } else if (cipher == pack::CIPHER_XCHACHA20_POLY1305 && data.size() >= 56) {
            const auto body = data.first(data.size() - 56);
            pack::Salt salt{};
            std::ranges::copy(data.subspan(body.size(), 16), salt.begin());
            const pack::Key key = pack::cheap(PASSWORD, salt);
            const auto nonce = data.subspan(body.size() + 16, 24);
            const auto mac = data.subspan(body.size() + 40, 16);
            const pack::Bytes plain(body.begin(), body.end());
            crypto_aead_lock(body.data(), mac.data(), key.data(), nonce.data(), nullptr,
                             0, plain.data(), plain.size());
        }
    }
}

// What the oracle checks of one chunk, opened from memory.
void check_contents(const rres::Contents &contents) {
    if (const std::optional<rres::Bytes> file = rres::file_of(contents)) {
        require(!contents.props.empty() && file->size() == contents.props[0] &&
                    file->size() <= contents.data.size(),
                "a file is its size property's bytes, and no more than the chunk holds");
    }
    const ::Image image = rres::image_of(contents);
    if (image.data != nullptr) {
        require(image.width > 0 && image.height > 0 && image.mipmaps == 1,
                "a picture has a size and one level");
    }
    UnloadImage(image);
}

std::vector<std::string> names_to_ask(const rres::Index &index) {
    std::vector<std::string> out = { "" };
    for (const rres::Entry &entry : index.entries) {
        if (out.size() > 9) break;
        out.push_back(entry.name);
        out.push_back(entry.name + "x");
        if (!entry.name.empty())
            out.push_back(entry.name.substr(0, entry.name.size() - 1));
    }
    return out;
}

bool in_directory(const rres::Index &index, std::string_view name) {
    return std::ranges::any_of(index.entries,
                               [&](const rres::Entry &e) { return e.name == name; });
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    std::error_code error;
    scratch.folder = std::filesystem::temp_directory_path(error) /
        ("rmp-fuzz-rres-" + std::to_string(rmp::test::fuzz::detail::process_id()));
    std::filesystem::remove_all(scratch.folder, error);
    std::filesystem::create_directories(scratch.folder, error);
    std::atexit(remove_scratch);
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    if (input.empty()) return 0;
    pack::Bytes bytes(input.begin() + 1, input.end());
    if ((input[0] & 2U) != 0) seal(bytes);
    if ((input[0] & 1U) != 0) pack::fix_crcs(bytes);
    const int reports = rmp::detail::report_count();

    // From memory, with the cheap key.
    rres::MemorySource memory(bytes);
    const std::optional<rres::Index> index = rres::read_index(memory);
    std::vector<std::string> names;
    if (index) {
        names = names_to_ask(*index);
        for (const std::string &name : names) {
            const std::optional<rres::Chunk> chunk = rres::find(*index, name);
            require(!chunk || in_directory(*index, name), "a lookup is the whole name");
            if (!chunk) continue;
            if (const auto contents = rres::open_chunk(memory, *chunk, PASSWORD, cheap)) {
                require(contents->type == chunk->info.type,
                        "a chunk opens as its own type");
                check_contents(*contents);
            }
        }
        require(!rres::find(*index, ""), "an empty name is no entry");
    }

    // From the file a game opens, which has to read the same.
    pack::save(scratch.folder / "resources.rres", bytes);
    const std::string root = (scratch.folder / "").string();
    rmp::assets::detail::set_resources_root(root.c_str());
    const bool opened = rmp::assets::detail::open_pack();
    require(opened == (index && !index->entries.empty()),
            "the game opens the pack exactly when it has a directory with names");
    require(opened == rmp::assets::detail::pack_is_open(),
            "open_pack() says what it did");
    require(opened == rmp::assets::using_pack(), "using_pack() says it too");
    for (const std::string &name : names) {
        // A game's name is a file in the resources folder: the directory is
        // asked for the plain name that file has, which is not always the
        // name the directory spells ("./x" is "x"; "a/b" is no file of it).
        const std::optional<std::string> as_file = rres::packed_name(root, root + name);
        const std::optional<rres::Chunk> chunk =
            opened && index && as_file ? rres::find(*index, *as_file) : std::nullopt;
        require(rmp::assets::detail::pack_has(name.c_str()) == chunk.has_value(),
                "pack_has() is the directory's answer for the file the name means");
        if (!chunk || chunk->info.cipher != pack::CIPHER_NONE) continue;
        const auto contents = rres::open_chunk(memory, *chunk, PASSWORD, cheap);
        const std::optional<rres::Bytes> expected =
            contents ? rres::file_of(*contents) : std::nullopt;
        int got = -1;
        const std::unique_ptr<unsigned char, decltype(&UnloadFileData)> read(
            rmp::assets::detail::pack_read(name.c_str(), &got), &UnloadFileData);
        require(read != nullptr || got == 0, "a refused read has size 0");
        require((read != nullptr) == expected.has_value(),
                "the file reads a chunk exactly when memory does");
        if (read != nullptr && expected) {
            const std::span<const unsigned char> was(read.get(),
                                                     static_cast<std::size_t>(got));
            require(std::ranges::equal(was, *expected), "and the same bytes");
            require(std::ranges::equal(rmp::assets::load_data(name), *expected),
                    "load_data() hands back what the pack holds");
        }
        UnloadImage(rmp::assets::detail::pack_read_image(name.c_str()));
    }
    rmp::assets::detail::close_pack();
    require(!rmp::assets::detail::pack_is_open(), "close_pack() closes it");
    require(rmp::detail::report_count() == reports, "reading a pack reports nothing");
    const std::string empty = (rmp::test::fuzz::detail::empty.folder / "").string();
    rmp::assets::detail::set_resources_root(empty.c_str());
    return 0;
}
