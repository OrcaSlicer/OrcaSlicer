#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/AVVideoDecoder.hpp"
#include "slic3r/GUI/Printer/BambuTunnel.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <thread>
#include <vector>
#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/init.h>
#include <wx/mstream.h>

TEST_CASE("JPEG texture decoding coexists with thumbnails and video frames", "[TimelapseCodecs][Regression]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This integration test needs a display (for example, xvfb-run).");
#endif
    wxInitializer init;
    REQUIRE(init.IsOk());
    wxInitAllImageHandlers();

    // Generate a JPEG through the same encoder used for G-code thumbnails.
    constexpr int width = 160, height = 90, iterations = 1000;
    Slic3r::ThumbnailData thumbnail;
    thumbnail.set(width, height);
    for (std::size_t i = 0; i < thumbnail.pixels.size(); i += 4) {
        thumbnail.pixels[i] = 180;
        thumbnail.pixels[i + 1] = 70;
        thumbnail.pixels[i + 2] = 30;
        thumbnail.pixels[i + 3] = 255;
    }
    const auto jpeg = Slic3r::GCodeThumbnails::compress_thumbnail(thumbnail, Slic3r::GCodeThumbnailsFormat::JPG);
    REQUIRE(jpeg);
    REQUIRE(jpeg->size > 0);
    const auto* bytes = static_cast<const unsigned char*>(jpeg->data);
    Slic3r::TextureImage texture;
    texture.width = texture.height = -1;
    texture.data.assign(bytes, bytes + jpeg->size);

    std::atomic<bool> texture_ok{true};
    std::thread texture_worker([&] {
        for (int i = 0; i < iterations; ++i) {
            std::vector<unsigned char> pixels;
            int decoded_width = 0, decoded_height = 0;
            if (!Slic3r::decode_texture_to_pixels(texture, pixels, decoded_width, decoded_height) ||
                decoded_width != width || decoded_height != height || pixels.size() != width * height * 3)
                texture_ok = false;
        }
    });

    // PrinterFileSystem decodes timelapse thumbnails with wxImage. Do not
    // assert until the worker is joined: Catch assertions can unwind the scope.
    bool thumbnails_ok = true;
    for (int i = 0; i < iterations; ++i) {
        wxMemoryInputStream stream(jpeg->data, jpeg->size);
        wxImage image(stream, "image/jpeg");
        if (!image.IsOk() || image.GetWidth() != width || image.GetHeight() != height ||
            image.GetRed(width / 2, height / 2) < 170 || image.GetGreen(width / 2, height / 2) > 80)
            thumbnails_ok = false;
    }
    texture_worker.join();
    REQUIRE(texture_ok.load());
    REQUIRE(thumbnails_ok);

    // Exercise Orca's real FFmpeg decoder and its platform display conversion.
    AVVideoDecoder decoder;
    Bambu_StreamInfo info{};
    info.type = VIDE;
    info.sub_type = MJPG;
    info.format_type = video_jpeg;
    REQUIRE(decoder.open(info) == 0);
    Bambu_Sample sample{};
    sample.size = static_cast<int>(jpeg->size);
    sample.buffer = bytes;
    for (int i = 0; i < iterations; ++i) {
        REQUIRE(decoder.decode(sample) == 0);
        wxImage image;
        REQUIRE(decoder.toWxImage(image, wxSize(width, height)));
        REQUIRE(image.IsOk());
        REQUIRE(image.GetWidth() == width);
        REQUIRE(image.GetHeight() == height);
#ifdef _WIN32
        // Playback uses this raw 32-bit bitmap conversion only on Windows.
        wxBitmap bitmap;
        REQUIRE(decoder.toWxBitmap(bitmap, wxSize(width, height)));
        REQUIRE(bitmap.IsOk());
        REQUIRE(bitmap.GetWidth() == width);
        REQUIRE(bitmap.GetHeight() == height);
#endif
    }
}
