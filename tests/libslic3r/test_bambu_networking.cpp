#include <catch2/catch_all.hpp>

#include "slic3r/Utils/bambu_networking.hpp"

using namespace BBL;

TEST_CASE("extract_base_version", "[BambuNetworking]") {
    SECTION("version without suffix returns unchanged") {
        REQUIRE(extract_base_version("02.03.00.62") == "02.03.00.62");
        REQUIRE(extract_base_version("01.00.00.00") == "01.00.00.00");
    }

    SECTION("version with suffix returns base only") {
        REQUIRE(extract_base_version("02.03.00.62-mod") == "02.03.00.62");
        REQUIRE(extract_base_version("02.03.00.62-patched") == "02.03.00.62");
        REQUIRE(extract_base_version("02.03.00.62-test-build") == "02.03.00.62");
    }

    SECTION("empty string returns empty") {
        REQUIRE(extract_base_version("") == "");
    }

    SECTION("suffix only returns empty") {
        REQUIRE(extract_base_version("-mod") == "");
    }
}

TEST_CASE("extract_suffix", "[BambuNetworking]") {
    SECTION("version without suffix returns empty") {
        REQUIRE(extract_suffix("02.03.00.62") == "");
        REQUIRE(extract_suffix("01.00.00.00") == "");
    }

    SECTION("version with suffix returns suffix without dash") {
        REQUIRE(extract_suffix("02.03.00.62-mod") == "mod");
        REQUIRE(extract_suffix("02.03.00.62-patched") == "patched");
    }

    SECTION("version with multiple dashes returns everything after first dash") {
        REQUIRE(extract_suffix("02.03.00.62-test-build") == "test-build");
    }

    SECTION("empty string returns empty") {
        REQUIRE(extract_suffix("") == "");
    }

    SECTION("suffix only returns suffix without leading dash") {
        REQUIRE(extract_suffix("-mod") == "mod");
    }
}

TEST_CASE("NetworkLibraryVersionInfo::from_static", "[BambuNetworking]") {
    SECTION("converts static version info correctly") {
        NetworkLibraryVersion static_ver{"02.03.00.62", "02.03.00.62", nullptr, true, nullptr};
        auto info = NetworkLibraryVersionInfo::from_static(static_ver);

        REQUIRE(info.version == "02.03.00.62");
        REQUIRE(info.base_version == "02.03.00.62");
        REQUIRE(info.suffix == "");
        REQUIRE(info.display_name == "02.03.00.62");
        REQUIRE(info.url_override == "");
        REQUIRE(info.is_latest == true);
        REQUIRE(info.warning == "");
        REQUIRE(info.is_discovered == false);
    }

    SECTION("handles version with warning") {
        NetworkLibraryVersion static_ver{"02.00.02.50", "02.00.02.50", nullptr, false, "This is a warning"};
        auto info = NetworkLibraryVersionInfo::from_static(static_ver);

        REQUIRE(info.version == "02.00.02.50");
        REQUIRE(info.is_latest == false);
        REQUIRE(info.warning == "This is a warning");
        REQUIRE(info.is_discovered == false);
    }

    SECTION("handles version with url override") {
        NetworkLibraryVersion static_ver{"02.01.01.52", "02.01.01.52", "https://custom.url/plugin.zip", false, nullptr};
        auto info = NetworkLibraryVersionInfo::from_static(static_ver);

        REQUIRE(info.url_override == "https://custom.url/plugin.zip");
    }
}

TEST_CASE("NetworkLibraryVersionInfo::from_discovered", "[BambuNetworking]") {
    SECTION("creates discovered version info correctly") {
        auto info = NetworkLibraryVersionInfo::from_discovered("02.03.00.62-mod", "02.03.00.62", "mod");

        REQUIRE(info.version == "02.03.00.62-mod");
        REQUIRE(info.base_version == "02.03.00.62");
        REQUIRE(info.suffix == "mod");
        REQUIRE(info.display_name == "02.03.00.62-mod");
        REQUIRE(info.url_override == "");
        REQUIRE(info.is_latest == false);
        REQUIRE(info.warning == "");
        REQUIRE(info.is_discovered == true);
    }
}
