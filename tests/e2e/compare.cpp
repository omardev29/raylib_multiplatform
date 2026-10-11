// ---------------------------------------------------------------------------
// The pixel comparison -- see tests/e2e/compare.h. Checked by tests/e2e_test.cpp.
// ---------------------------------------------------------------------------

#include "compare.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <system_error>

namespace e2e {

namespace {

constexpr int CHANNELS = 4;

std::size_t pixel_count(const Picture &picture) {
    return static_cast<std::size_t>(picture.width) *
        static_cast<std::size_t>(picture.height);
}

// Its bytes are what its size says they are.
bool well_formed(const Picture &picture) {
    return picture.width >= 0 && picture.height >= 0 &&
        picture.rgba.size() == pixel_count(picture) * CHANNELS;
}

// The largest difference in one channel of pixel `i` of two pictures the
// same size.
int channel_gap(const Picture &a, const Picture &b, std::size_t i) {
    int worst = 0;
    for (std::size_t c = 0; c < CHANNELS; ++c) {
        const int gap = std::abs(static_cast<int>(a.rgba[(i * CHANNELS) + c]) -
                                 static_cast<int>(b.rgba[(i * CHANNELS) + c]));
        worst = std::max(worst, gap);
    }
    return worst;
}

} // namespace

Difference compare(const Picture &expected, const Picture &actual, int tolerance) {
    Difference d;
    if (!well_formed(expected) || !well_formed(actual) ||
        expected.width != actual.width || expected.height != actual.height) {
        d.same_size = false;
        return d;
    }
    const std::size_t count = pixel_count(expected);
    for (std::size_t i = 0; i < count; ++i) {
        const int gap = channel_gap(expected, actual, i);
        d.largest = std::max(d.largest, gap);
        if (gap <= tolerance) continue;
        if (d.pixels == 0) {
            d.first_x = static_cast<int>(i % static_cast<std::size_t>(expected.width));
            d.first_y = static_cast<int>(i / static_cast<std::size_t>(expected.width));
        }
        ++d.pixels;
    }
    return d;
}

std::uint32_t fingerprint(const Picture &picture) {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : picture.rgba) {
        hash = (hash ^ byte) * 16777619U;
    }
    return hash;
}

Picture picture_of(const ::Image &image) {
    Picture out;
    if (image.data == nullptr || image.width <= 0 || image.height <= 0) return out;
    OwnedImage copy(ImageCopy(image));
    ImageFormat(&copy.image(), PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (copy.image().data == nullptr) return out;
    out.width = copy.image().width;
    out.height = copy.image().height;
    const std::span<const unsigned char> bytes(
        static_cast<const unsigned char *>(copy.image().data),
        pixel_count(out) * CHANNELS);
    out.rgba.assign(bytes.begin(), bytes.end());
    return out;
}

std::optional<Picture> load_png(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    OwnedImage loaded(LoadImage(path.string().c_str()));
    if (loaded.image().data == nullptr) return std::nullopt;
    Picture picture = picture_of(loaded.image());
    if (picture.rgba.empty()) return std::nullopt;
    return picture;
}

bool write_png(const Picture &picture, const std::filesystem::path &path) {
    if (!well_formed(picture) || picture.rgba.empty()) return false;
    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    // ExportImage() takes the pixels through a non-const pointer and only reads
    // them: it gets a copy, so this does not have to cast the const away.
    Picture copy = picture;
    const ::Image view{ .data = copy.rgba.data(),
                        .width = copy.width,
                        .height = copy.height,
                        .mipmaps = 1,
                        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    return ExportImage(view, path.string().c_str());
}

Picture difference_picture(const Picture &expected, const Picture &actual,
                           int tolerance) {
    Picture out;
    out.width = std::max(expected.width, actual.width);
    out.height = std::max(expected.height, actual.height);
    out.rgba.assign(pixel_count(out) * CHANNELS, 0);
    const bool comparable = well_formed(expected) && well_formed(actual);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * out.width) + x;
            const bool in_expected =
                comparable && x < expected.width && y < expected.height;
            const bool in_actual = comparable && x < actual.width && y < actual.height;
            const std::size_t e = (static_cast<std::size_t>(y) * expected.width) + x;
            bool differs = !in_expected || !in_actual;
            if (!differs) {
                const std::size_t a = (static_cast<std::size_t>(y) * actual.width) + x;
                for (std::size_t c = 0; c < CHANNELS; ++c) {
                    const int gap =
                        std::abs(static_cast<int>(expected.rgba[(e * CHANNELS) + c]) -
                                 static_cast<int>(actual.rgba[(a * CHANNELS) + c]));
                    differs = differs || gap > tolerance;
                }
            }
            if (differs) {
                out.rgba[at * CHANNELS] = 255;
            } else {
                for (std::size_t c = 0; c < 3; ++c) {
                    out.rgba[(at * CHANNELS) + c] =
                        static_cast<unsigned char>(expected.rgba[(e * CHANNELS) + c] / 4);
                }
            }
            out.rgba[(at * CHANNELS) + 3] = 255;
        }
    }
    return out;
}

bool write_failure(const Picture &expected, const Picture &actual,
                   const std::filesystem::path &dir, const std::string &name) {
    const bool wrote_expected = write_png(expected, dir / (name + ".expected.png"));
    const bool wrote_actual = write_png(actual, dir / (name + ".actual.png"));
    const bool wrote_diff =
        write_png(difference_picture(expected, actual, 0), dir / (name + ".diff.png"));
    return wrote_expected && wrote_actual && wrote_diff;
}

} // namespace e2e
