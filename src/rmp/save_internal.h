#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The parts of rmp::save that tests/save_test.cpp drives
// without a real disk layout: the file format as bytes, the checksum, and the
// seams that point the save folder somewhere temporary.
//
// THE FILE FORMAT, because it is the part a future version has to keep
// reading. One line of ASCII, then the payload:
//
//     rmp-save 1 <version> <plain|sealed> <payload bytes> <crc32, 8 hex>\n
//     <payload>
//
// `1` is the format of this layout, and a reader that meets a bigger one says
// UNREADABLE rather than guessing. <version> is [save] version, the game's.
// The payload is the JSON, or -- sealed -- a 24-byte nonce, a 16-byte tag and
// the JSON encrypted with XChaCha20-Poly1305 (monocypher, the same copy rres
// ships). The CRC-32 covers the header up to and including the length, a
// '\n', and the payload, so a changed version or length is caught like a
// changed byte; the header must be exactly as the writer puts it, so the CRC
// covers the bytes on disk. The seal authenticates the same header as
// associated data.
//
// Why a text header: a person who opens a plain save sees what it is and can
// read the JSON under it, and a truncation is recognisable by eye.
// ---------------------------------------------------------------------------

#include <rmp/save.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rmp::detail {

// The one door into Value's version for the framework.
struct ValueAccess {
    static void set_version(Value &value, int version) { value.version_ = version; }
    // The number as stored, a double: as_float() would round it to a float,
    // and a save must write back exactly what it read.
    static double number(const Value &value) { return value.number_; }
};

} // namespace rmp::detail

namespace rmp::save::detail {

using Bytes = std::vector<unsigned char>;

// CRC-32 (IEEE 802.3, the zlib one): crc32("123456789") == 0xCBF43926.
std::uint32_t crc32(const unsigned char *data, std::size_t size);

// A whole save file, as bytes. False only when the Value cannot be expressed
// (nested past the depth cJSON would read back).
bool encode(const Value &value, int version, bool sealed, Bytes *out);

// The inverse. On OK, *out is replaced and carries the file's version. With
// `sealed_only`, a plain file is MODIFIED: see rmp::save::ReadOptions.
Status decode(const Bytes &file, Value *out, bool sealed_only = false);

// How deep a save may nest, writing and reading alike. src/rmp/cjson_impl.c
// sets CJSON_NESTING_LIMIT to the same number, and tests/save_test.cpp checks
// that the two agree. Small on purpose: the conversion is recursive, and the
// web's default stack of 64 KB overflowed at about 400 levels.
constexpr int MAX_DEPTH = 64;

// JSON alone, no header: what goes inside, and what a test compares.
std::string to_json(const Value &value);
bool from_json(std::string_view json, Value *out);

// Whether a slot name is one we accept (see rmp::save::write).
bool valid_slot(std::string_view slot);

// The user's data folder on this platform, from the environment, with a
// trailing '/'. What directory() uses unless [save] portable wins.
std::string user_folder();

// Point the two candidate folders somewhere else, and forget which one was
// chosen, so the next directory() resolves again. `portable` empty means
// [save] portable is off. For tests only.
void set_folders_for_tests(const std::string &portable, const std::string &user);
void reset_for_tests();

// How many times the portable folder was given up on, for the test that
// proves it is said once.
int fallbacks();

} // namespace rmp::save::detail
