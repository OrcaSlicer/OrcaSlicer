#include <catch2/catch_all.hpp>

#include "libslic3r/LocalesUtils.hpp"

#include <boost/filesystem/path.hpp>
#include <locale>

TEST_CASE("Filesystem initialization preserves UTF-8 paths and global locales", "[LocalesUtils]")
{
    const std::string original_c_locale = std::setlocale(LC_ALL, nullptr);
    const std::locale original_cpp_locale;

    REQUIRE_NOTHROW(Slic3r::init_utf8_filesystem());

    // Include both Chinese characters and a character outside the BMP.
    const std::string utf8_name = "\xE6\xA8\xA1\xE5\x9E\x8B-\xF0\x9F\x90\x99.3mf";
    const std::wstring wide_name = L"\u6A21\u578B-\U0001F419.3mf";
    CHECK(boost::filesystem::path(utf8_name).wstring() == wide_name);
    CHECK(boost::filesystem::path(wide_name).string() == utf8_name);

    REQUIRE_NOTHROW(Slic3r::init_utf8_filesystem());
    CHECK(boost::filesystem::path(utf8_name).wstring() == wide_name);
    CHECK(std::string(std::setlocale(LC_ALL, nullptr)) == original_c_locale);
    CHECK(std::locale() == original_cpp_locale);
}
