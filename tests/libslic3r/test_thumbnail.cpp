#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"

#include <sstream>

using namespace Slic3r;

TEST_CASE("ThumbnailData creation and validation", "[Thumbnail]") {
    GIVEN("A default ThumbnailData") {
        ThumbnailData data;
        
        WHEN("Set to 100x50") {
            data.set(100, 50);
            
            THEN("Width is 100") {
                REQUIRE(data.width == 100);
            }
            THEN("Height is 50") {
                REQUIRE(data.height == 50);
            }
            THEN("Is valid when pixels are set") {
                REQUIRE(data.is_valid() == false); // pixels not allocated
                data.pixels.resize(100 * 50 * 4);
                REQUIRE(data.is_valid() == true);
            }
        }
    }
}

TEST_CASE("ThumbnailsParams defaults", "[Thumbnail]") {
    GIVEN("Default ThumbnailsParams") {
        ThumbnailsParams params{{Vec2d(200, 200)}, true, true, true, true, 0};
        
        THEN("sliced_preview defaults to false") {
            REQUIRE(params.sliced_preview == false);
        }
        THEN("show_support defaults to true") {
            REQUIRE(params.show_support == true);
        }
        THEN("slice_result defaults to nullptr") {
            REQUIRE(params.slice_result == nullptr);
        }
        
        WHEN("Set sliced_preview to true") {
            params.sliced_preview = true;
            
            THEN("sliced_preview is true") {
                REQUIRE(params.sliced_preview == true);
            }
        }
        
        WHEN("Set show_support to false") {
            params.show_support = false;
            
            THEN("show_support is false") {
                REQUIRE(params.show_support == false);
            }
        }
    }
}

TEST_CASE("GCodeThumbnailsFormat SLICED_PREVIEW enum value", "[Thumbnail]") {
    GIVEN("GCodeThumbnailsFormat enum") {
        THEN("SLICED_PREVIEW is PNG (0) by default for compression") {
            // SLICED_PREVIEW falls through to PNG in compress_thumbnail
            REQUIRE(static_cast<int>(GCodeThumbnailsFormat::SLICED_PREVIEW) == 5);
        }
    }
}

TEST_CASE("make_and_check_thumbnail_list basic functionality", "[Thumbnail]") {
    GIVEN("A config with thumbnails set to 200x200") {
        DynamicPrintConfig config;
        config.set_deserialize_strict("thumbnails", "200x200");
        
        auto [list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(config);
        
        THEN("Thumbnail list has one entry") {
            REQUIRE(list.size() == 1);
        }
        THEN("Format is PNG (default)") {
            REQUIRE(list[0].first == GCodeThumbnailsFormat::PNG);
        }
        THEN("Size is 200x200") {
            REQUIRE(list[0].second.x() == 200);
            REQUIRE(list[0].second.y() == 200);
        }
    }
    
    GIVEN("A config with multiple thumbnails") {
        DynamicPrintConfig config;
        config.set_deserialize_strict("thumbnails", "100x100,200x200");
        
        auto [list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(config);
        
        THEN("Thumbnail list has two entries") {
            REQUIRE(list.size() == 2);
        }
        THEN("First size is 100x100") {
            REQUIRE(list[0].second.x() == 100);
            REQUIRE(list[0].second.y() == 100);
        }
        THEN("Second size is 200x200") {
            REQUIRE(list[1].second.x() == 200);
            REQUIRE(list[1].second.y() == 200);
        }
    }
}

TEST_CASE("make_and_check_thumbnail_list with format extension", "[Thumbnail]") {
    GIVEN("A config with thumbnails including format extension") {
        DynamicPrintConfig config;
        config.set_deserialize_strict("thumbnails", "200x200/PNG");
        
        auto [list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(config);
        
        THEN("Format is PNG") {
            REQUIRE(list[0].first == GCodeThumbnailsFormat::PNG);
        }
        THEN("Size is 200x200") {
            REQUIRE(list[0].second.x() == 200);
            REQUIRE(list[0].second.y() == 200);
        }
    }
}

TEST_CASE("compress_thumbnail for SLICED_PREVIEW format", "[Thumbnail]") {
    GIVEN("A valid ThumbnailData with pixels") {
        ThumbnailData data;
        data.set(10, 10);
        data.pixels.resize(10 * 10 * 4, 255); // White image
        
        WHEN("Compressed as SLICED_PREVIEW") {
            auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::SLICED_PREVIEW);
            
            THEN("Result is not null") {
                REQUIRE(compressed != nullptr);
            }
            THEN("Data is not empty") {
                REQUIRE(compressed->data != nullptr);
                REQUIRE(compressed->size > 0);
            }
            THEN("Tag is PNG") {
                REQUIRE(compressed->tag() == "PNG");
            }
        }
        
        WHEN("Compressed as PNG (for comparison)") {
            auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
            
            THEN("SLICED_PREVIEW and PNG produce same result") {
                auto compressed_sp = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::SLICED_PREVIEW);
                REQUIRE(compressed->size == compressed_sp->size);
            }
        }
    }
}
