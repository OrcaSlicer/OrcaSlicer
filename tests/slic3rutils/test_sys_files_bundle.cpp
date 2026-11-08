#include <catch2/catch_all.hpp>

#include <string>

#include "slic3r/Utils/SysFilesBundle.hpp"

using namespace Slic3r;

namespace {

SysFile make_file(const std::string &path, const std::string &text)
{
    SysFile f;
    f.path = path;
    f.data.assign(text.begin(), text.end());
    return f;
}

} // namespace

TEST_CASE("The fork number is read from the +unlayered suffix", "[SysFilesBundle]")
{
    CHECK(unlayered_fork_number("3.7.0-beta.1+unlayered.9") == 9);
    CHECK(unlayered_fork_number("3.7.0-beta.1+unlayered.15") == 15);
    CHECK(unlayered_fork_number("3.7.0+unlayered.123-dirty") == 123);
    CHECK(unlayered_fork_number("+unlayered.12") == 12);
}

TEST_CASE("A version without a fork number reads as -1", "[SysFilesBundle]")
{
    CHECK(unlayered_fork_number("3.7.0-beta.1") == -1);
    CHECK(unlayered_fork_number("3.6.2+1") == -1);
    CHECK(unlayered_fork_number("3.7.0-beta.1+unlayered.") == -1);
    CHECK(unlayered_fork_number("") == -1);
}

TEST_CASE("Firmware is ordered by fork number, not by semver", "[SysFilesBundle]")
{
    // Semver ignores build metadata and would call these equal.
    CHECK_FALSE(sys_firmware_satisfies("3.7.0-beta.1+unlayered.9", "+unlayered.12"));
    CHECK(sys_firmware_satisfies("3.7.0-beta.1+unlayered.12", "+unlayered.12"));
    CHECK(sys_firmware_satisfies("3.7.0-beta.1+unlayered.15", "3.7.0-beta.1+unlayered.12"));
}

TEST_CASE("Stock firmware never satisfies a fork requirement", "[SysFilesBundle]")
{
    CHECK_FALSE(sys_firmware_satisfies("3.6.2+1", "+unlayered.12"));
    CHECK_FALSE(sys_firmware_satisfies("3.7.0", "+unlayered.12"));
}

TEST_CASE("An empty or incomparable requirement does not refuse", "[SysFilesBundle]")
{
    CHECK(sys_firmware_satisfies("3.7.0-beta.1+unlayered.9", ""));
    CHECK(sys_firmware_satisfies("not a version", "also not one"));
}

TEST_CASE("A plain RRF requirement uses semver order", "[SysFilesBundle]")
{
    CHECK(sys_firmware_satisfies("3.7.0", "3.6.2"));
    CHECK_FALSE(sys_firmware_satisfies("3.6.2", "3.7.0"));
}

TEST_CASE("M558 P12 in any file requires +unlayered.12", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    pkg.files.push_back(make_file("sys/homez.g", "G1 Z5\n"));
    pkg.files.push_back(make_file("sys/config.g",
                                  "M558 K0 P12 C\"!io6.in\" H5 F300   ; differential probe\r\nG31 P120\r\n"));
    std::string reason;
    CHECK(sys_required_firmware(pkg, &reason) == "+unlayered.12");
    CHECK_THAT(reason, Catch::Matchers::ContainsSubstring("sys/config.g"));
}

TEST_CASE("M558 P12 inside a comment or on another probe type is ignored", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    pkg.files.push_back(make_file("sys/config.g",
                                  ";M558 K0 P12 C\"io6.in\"\n"
                                  "M558 K0 P3 C\"io6.in\"   ; was P12\n"
                                  "M558 K0 P120\n"
                                  "G31 P12\n"));
    CHECK(sys_required_firmware(pkg).empty());
}

TEST_CASE("config.g's fwNeed raises the requirement above P12", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    pkg.files.push_back(make_file("sys/config.g",
                                  "var fwNeed = 15                ; lowest +unlayered.N\n"
                                  "M558 K0 P12 C\"!io6.in\"\n"));
    std::string reason;
    const std::string need = sys_required_firmware(pkg, &reason);
    CHECK(need == "+unlayered.15");
    CHECK_THAT(reason, Catch::Matchers::ContainsSubstring("fwNeed"));
    CHECK_FALSE(sys_firmware_satisfies("3.7.0-beta.1+unlayered.12", need));
}

TEST_CASE("fwNeed is only read from sys/config.g", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    pkg.files.push_back(make_file("macros/test.g", "var fwNeed = 99\n"));
    CHECK(sys_required_firmware(pkg).empty());
}

TEST_CASE("The manifest's minFirmware stands when the files need less", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    std::string    error;
    REQUIRE(parse_sys_manifest(R"({ "version": "1", "minFirmware": "3.7.0-beta.1+unlayered.20" })", pkg, error));
    CHECK(pkg.min_firmware == "3.7.0-beta.1+unlayered.20");
    pkg.files.push_back(make_file("sys/config.g", "M558 K0 P12\n"));
    CHECK(sys_required_firmware(pkg) == "3.7.0-beta.1+unlayered.20");
}

TEST_CASE("The files raise a manifest minFirmware that is too low", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    pkg.min_firmware = "+unlayered.9";
    pkg.files.push_back(make_file("sys/config.g", "M558 K0 P12\n"));
    CHECK(sys_required_firmware(pkg) == "+unlayered.12");
}

TEST_CASE("A binary file is not scanned", "[SysFilesBundle]")
{
    SysFilePackage pkg;
    std::string    blob = "M558 P12\n";
    blob.push_back('\0');
    pkg.files.push_back(make_file("sys/blob.bin", blob));
    CHECK(sys_required_firmware(pkg).empty());
}
