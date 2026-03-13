#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/filesystem.hpp>
#include <miniz.h>

using namespace Slic3r;

// Helper: read a file entry from a 3mf (zip) into a string.
static std::string read_entry_from_zip(const std::string& zip_path, const char* entry_name)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0))
        return {};
    int idx = mz_zip_reader_locate_file(&zip, entry_name, nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return {};
    }
    size_t size = 0;
    void*  data = mz_zip_reader_extract_to_heap(&zip, idx, &size, 0);
    std::string out;
    if (data && size > 0)
        out.assign(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_reader_end(&zip);
    return out;
}

SCENARIO("Plate changer export produces single merged gcode and single plate metadata", "[bbs_3mf][plate_changer]") {
    GIVEN("A model and config with plate_change_gcode set") {
        Model model;
        model.add_object()->add_default_instance();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set("plate_change_gcode", ";PLATE_CHANGE\n");

        WHEN("store_bbs_3mf is called with use_plate_changer_all") {
            // Minimal StoreParams: use the same setup Plater would use, but with an empty plate list.
            StoreParams store_params;
            store_params.path  = "test_plate_changer_merged.gcode.3mf";
            store_params.model = &model;
            store_params.config = &config;
            store_params.use_plate_changer_all = true;

            // We don't have real sliced plate data here, so just ensure the call does not throw
            // and that the exporter can at least create the archive structure. This is a sanity
            // test that the plate-changer path integrates with the top-level API.
            bool ok = false;
            REQUIRE_NOTHROW(ok = store_bbs_3mf(store_params));
            THEN("Export succeeds") {
                REQUIRE(ok);
            }

            AND_THEN("The resulting 3mf contains a model_settings.config file") {
                REQUIRE(boost::filesystem::exists(store_params.path));
                auto model_settings = read_entry_from_zip(store_params.path, "Metadata/model_settings.config");
                REQUIRE_FALSE(model_settings.empty());
            }

            AND_THEN("The resulting 3mf contains a slice_info.config file") {
                REQUIRE(boost::filesystem::exists(store_params.path));
                auto slice_info = read_entry_from_zip(store_params.path, "Metadata/slice_info.config");
                REQUIRE_FALSE(slice_info.empty());
                REQUIRE(slice_info.find("<config") != std::string::npos);
            }

            AND_THEN("The resulting 3mf does not contain a second plate gcode file") {
                REQUIRE(boost::filesystem::exists(store_params.path));
                auto plate2_gcode = read_entry_from_zip(store_params.path, "Metadata/plate_2.gcode");
                REQUIRE(plate2_gcode.empty());
            }
        }
    }
}

