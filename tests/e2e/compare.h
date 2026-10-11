#pragma once
// ---------------------------------------------------------------------------
// A frame the game drew, against the golden PNG it has to match.
//
// EXACT, by default and by decision (Omar, 2026-10-11). The goldens are drawn
// by raylib's software renderer inside the pinned build image, with a fixed
// step and a fixed seed, so the same script gives the same bytes; a
// tolerance would only be somewhere for a real change to hide. compare()
// still takes one, so a shot that proves it needs it can have it -- with the
// reason written next to it -- but none does.
//
// A failure leaves three pictures: what was expected, what came out, and the
// difference, the expected one darkened with every pixel that differs in red.
// ---------------------------------------------------------------------------

#include <raylib.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace e2e {

// A raylib image somebody here made, unloaded when it goes. raylib hands
// images out by value with their pixels behind a pointer; this is the owner.
class OwnedImage {
public:
    explicit OwnedImage(::Image image) : _image(image) {}
    ~OwnedImage() { UnloadImage(_image); }
    OwnedImage(const OwnedImage &) = delete;
    OwnedImage &operator=(const OwnedImage &) = delete;
    OwnedImage(OwnedImage &&) = delete;
    OwnedImage &operator=(OwnedImage &&) = delete;

    ::Image &image() { return _image; }

private:
    ::Image _image;
};

// A picture by value: RGBA, one byte a channel, the top row first.
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba; // width * height * 4
};

struct Difference {
    bool same_size = true;
    int pixels = 0; // pixels with a channel further than the tolerance apart
    int largest = 0; // the largest difference in one channel, 0-255
    int first_x = -1; // the first of them, reading row by row
    int first_y = -1;

    bool none() const { return same_size && pixels == 0; }
};

// Two sizes are one difference, and then nothing else is counted.
Difference compare(const Picture &expected, const Picture &actual, int tolerance);

// FNV-1a over the bytes: the fingerprint the run prints for every shot, so two
// runs -- or the image and a laptop -- can be compared by eye.
std::uint32_t fingerprint(const Picture &picture);

// A copy of a raylib image as a Picture, whatever its pixel format.
Picture picture_of(const ::Image &image);

// Nothing when the file is not there or is not a picture.
std::optional<Picture> load_png(const std::filesystem::path &path);
bool write_png(const Picture &picture, const std::filesystem::path &path);

// The difference, as a picture: the expected one at a quarter of its
// brightness, and every pixel that differs pure red. As large as the larger
// of the two, so a size mismatch shows where.
Picture difference_picture(const Picture &expected, const Picture &actual, int tolerance);

// <dir>/<name>.expected.png, <name>.actual.png and <name>.diff.png. False if
// any of the three could not be written.
bool write_failure(const Picture &expected, const Picture &actual,
                   const std::filesystem::path &dir, const std::string &name);

} // namespace e2e
