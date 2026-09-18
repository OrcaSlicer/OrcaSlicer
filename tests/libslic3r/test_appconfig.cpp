#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r;

namespace {

void write_printer_config(AppConfig& config, const nlohmann::json& printer)
{
    boost::nowide::ofstream output(config.config_path());
    REQUIRE(output.is_open());
    output << nlohmann::json{{"local_machines", {{"test-printer", printer}}}} << '\n';
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("AppConfig network version helpers", "[AppConfig]") {
    AppConfig config;

    SECTION("skipped versions starts empty") {
        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.empty());
    }

    SECTION("add and check skipped version") {
        config.add_skipped_network_version("02.01.01.52");
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE_FALSE(config.is_network_version_skipped("02.03.00.62"));
    }

    SECTION("multiple skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.00.02.50");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 2);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE(config.is_network_version_skipped("02.00.02.50"));
    }

    SECTION("clear skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.clear_skipped_network_versions();
        REQUIRE_FALSE(config.is_network_version_skipped("02.01.01.52"));
    }

    SECTION("duplicate add is idempotent") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.01.01.52");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 1);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
    }
}

TEST_CASE("Local printer aliases survive configuration reloads", "[AppConfig][Regression]")
{
    ScopedTemporaryDir directory;
    ScopeGuard restore_directory([previous = data_dir()] { set_data_dir(previous); });
    set_data_dir(directory.string());
    save_main_thread_id();

    const std::string alias = GENERATE("Workshop Printer", u8"Atelier \"\u00e9tage\"");
    AppConfig config;
    write_printer_config(config, {{"dev_name", "Discovered Printer"}, {"local_name", alias}});
    REQUIRE(config.load().empty());

    // Discovery can replace the reported name without changing the user's alias.
    BBLocalMachine printer = config.get_local_machines().at("test-printer");
    printer.dev_name = "Updated Discovery Name";
    config.update_local_machine(printer);
    config.save();

    AppConfig reloaded;
    REQUIRE(reloaded.load().empty());
    CHECK(reloaded.get_local_machines().at("test-printer").dev_name == printer.dev_name);
    reloaded.save();

    boost::nowide::ifstream input(reloaded.config_path());
    nlohmann::json saved;
    input >> saved;
    const auto& saved_printer = saved.at("local_machines").at("test-printer");
    REQUIRE(saved_printer.contains("local_name"));
    CHECK(saved_printer.at("local_name").get<std::string>() == alias);
    CHECK(saved_printer.at("dev_name").get<std::string>() == printer.dev_name);
}

TEST_CASE("Legacy printer names migrate without filling explicit empty aliases", "[AppConfig][Regression]")
{
    ScopedTemporaryDir directory;
    ScopeGuard restore_directory([previous = data_dir()] { set_data_dir(previous); });
    set_data_dir(directory.string());
    save_main_thread_id();

    const bool legacy = GENERATE(true, false);
    const std::string saved_name = "Saved Printer Name";
    nlohmann::json printer = {{"dev_name", saved_name}};
    if (!legacy)
        printer["local_name"] = "";

    AppConfig config;
    write_printer_config(config, printer);
    REQUIRE(config.load().empty());
    config.save();

    // Saving an empty alias must keep it distinct from a legacy missing field.
    AppConfig reloaded;
    REQUIRE(reloaded.load().empty());
    reloaded.save();

    boost::nowide::ifstream input(reloaded.config_path());
    nlohmann::json saved;
    input >> saved;
    const auto& saved_printer = saved.at("local_machines").at("test-printer");
    REQUIRE(saved_printer.contains("local_name"));
    CHECK(saved_printer.at("local_name").get<std::string>() == (legacy ? saved_name : ""));
}

TEST_CASE("Changing only a printer alias marks its configuration dirty", "[AppConfig][Regression]")
{
    ScopedTemporaryDir directory;
    ScopeGuard restore_directory([previous = data_dir()] { set_data_dir(previous); });
    set_data_dir(directory.string());

    AppConfig config;
    write_printer_config(config, {{"dev_name", "Discovered Printer"}, {"local_name", "Original Alias"}});
    REQUIRE(config.load().empty());
    const BBLocalMachine original = config.get_local_machines().at("test-printer");
    config.update_local_machine(original);
    CHECK_FALSE(config.dirty());

    AppConfig replacement;
    write_printer_config(replacement, {{"dev_name", "Discovered Printer"}, {"local_name", "Updated Alias"}});
    REQUIRE(replacement.load().empty());
    const BBLocalMachine renamed = replacement.get_local_machines().at("test-printer");
    CHECK(original != renamed);

    config.update_local_machine(renamed);
    CHECK(config.dirty());
    CHECK(config.get_local_machines().at("test-printer") == renamed);
}
