#define NOMINMAX
#include <catch2/catch_all.hpp>

#include <numeric>

#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/AGGRaster.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;

static sla::RasterGrayscaleAA create_raster(const sla::RasterBase::Resolution &res)
{
    sla::RasterBase::PixelDim pixdim{1., 1.};

    auto bb = BoundingBox({0, 0}, {scaled(1.), scaled(1.)});
    sla::RasterBase::Trafo trafo;
    trafo.center_x = bb.center().x();
    trafo.center_y = bb.center().y();

    return sla::RasterGrayscaleAA{res, pixdim, trafo, agg::gamma_threshold(.5)};
}

TEST_CASE("PNG read", "[PNG]") {
    auto rst = create_raster({100, 100});

    size_t rstsum = 0;
    for (size_t r = 0; r < rst.resolution().height_px; ++r)
        for (size_t c = 0; c < rst.resolution().width_px; ++c)
            rstsum += rst.read_pixel(c, r);

    SECTION("Correct png buffer should be recognized as such.") {
        auto enc_rst = rst.encode(sla::PNGRasterEncoder{});
        REQUIRE(Slic3r::png::is_png({enc_rst.data(), enc_rst.size()}));
    }

    SECTION("Fake png buffer should be recognized as such.") {
        std::vector<uint8_t> fake(10, '\0');
        REQUIRE(!Slic3r::png::is_png({fake.data(), fake.size()}));
    }

    SECTION("Decoded PNG buffer resolution should match the original") {
        auto enc_rst = rst.encode(sla::PNGRasterEncoder{});

        png::ImageGreyscale img;
        png::decode_png({enc_rst.data(), enc_rst.size()}, img);

        REQUIRE(img.rows == rst.resolution().height_px);
        REQUIRE(img.cols == rst.resolution().width_px);

        size_t sum = std::accumulate(img.buf.begin(), img.buf.end(), size_t(0));

        REQUIRE(sum == rstsum);
    }
}
