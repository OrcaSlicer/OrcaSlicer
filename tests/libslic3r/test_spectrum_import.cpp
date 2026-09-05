#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <string>

using namespace Slic3r;

namespace {

size_t substitution_count(const ConfigSubstitutionContext &ctxt, const std::string &opt_key)
{
    size_t n = 0;
    for (const ConfigSubstitution &s : ctxt.substitutions)
        if (s.opt_def && s.opt_def->opt_key == opt_key)
            ++n;
    return n;
}

EnsureVerticalShellThickness shell_thickness(const DynamicPrintConfig &cfg)
{
    return cfg.option<ConfigOptionEnum<EnsureVerticalShellThickness>>("ensure_vertical_shell_thickness")->value;
}

} // namespace

TEST_CASE("Studio ensure_vertical_shell_thickness aliases deserialize without substitution",
          "[spectrum][spectrum_import]")
{
    const auto alias = GENERATE(as<std::string>{},
                                "enabled", "Enabled", "ENABLED", "true", "TRUE",
                                "disabled", "Disabled", "false", "FALSE",
                                "partial", "Partial", "PARTIAL");

    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    REQUIRE(cfg.set_deserialize_nothrow("ensure_vertical_shell_thickness", alias, ctxt, false));
    CHECK(substitution_count(ctxt, "ensure_vertical_shell_thickness") == 0);

    if (boost::iequals(alias, "enabled") || boost::iequals(alias, "true"))
        CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstAll);
    else if (boost::iequals(alias, "disabled") || boost::iequals(alias, "false"))
        CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstNone);
    else
        CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstModerate);
}

TEST_CASE("legacy 1/0 ensure_vertical_shell_thickness mappings stay", "[spectrum][spectrum_import]")
{
    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};

    REQUIRE(cfg.set_deserialize_nothrow("ensure_vertical_shell_thickness", "1", ctxt, false));
    CHECK(substitution_count(ctxt, "ensure_vertical_shell_thickness") == 0);
    CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstAll);

    REQUIRE(cfg.set_deserialize_nothrow("ensure_vertical_shell_thickness", "0", ctxt, false));
    CHECK(substitution_count(ctxt, "ensure_vertical_shell_thickness") == 0);
    CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstModerate);
}

TEST_CASE("unknown ensure_vertical_shell_thickness still substitutes", "[spectrum][spectrum_import]")
{
    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    REQUIRE(cfg.set_deserialize_nothrow("ensure_vertical_shell_thickness", "studio_bogus_enum", ctxt, false));
    REQUIRE(substitution_count(ctxt, "ensure_vertical_shell_thickness") == 1);
    CHECK(shell_thickness(cfg) == EnsureVerticalShellThickness::evstAll);
}

TEST_CASE("Bambu raft -1 clamps to pin default 2 and validates", "[spectrum][spectrum_import]")
{
    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    REQUIRE(cfg.set_deserialize_nothrow("raft_first_layer_expansion", "-1", ctxt, false));
    CHECK(cfg.opt_float("raft_first_layer_expansion") == 2.0);
    const auto validity = cfg.validate();
    CHECK(validity.find("raft_first_layer_expansion") == validity.end());
}

TEST_CASE("tree_support_wall_count sentinels clamp and validate", "[spectrum][spectrum_import]")
{
    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};

    REQUIRE(cfg.set_deserialize_nothrow("tree_support_wall_count", "-1", ctxt, false));
    CHECK(cfg.opt_int("tree_support_wall_count") == 0);
    {
        const auto validity = cfg.validate();
        CHECK(validity.find("tree_support_wall_count") == validity.end());
    }

    REQUIRE(cfg.set_deserialize_nothrow("tree_support_wall_count", "9", ctxt, false));
    CHECK(cfg.opt_int("tree_support_wall_count") == 2);
    {
        const auto validity = cfg.validate();
        CHECK(validity.find("tree_support_wall_count") == validity.end());
    }
}

TEST_CASE("handle_legacy numeric sentinels do not throw on garbage", "[spectrum][spectrum_import]")
{
    t_config_option_key key   = "raft_first_layer_expansion";
    std::string         value = "not-a-number";
    CHECK_NOTHROW(PrintConfigDef::handle_legacy(key, value));
    CHECK(key == "raft_first_layer_expansion");
    CHECK(value == "not-a-number");

    key   = "raft_first_layer_expansion";
    value = "";
    CHECK_NOTHROW(PrintConfigDef::handle_legacy(key, value));
    CHECK(value.empty());

    key   = "tree_support_wall_count";
    value = "nope";
    CHECK_NOTHROW(PrintConfigDef::handle_legacy(key, value));
    CHECK(value == "nope");

    DynamicPrintConfig        cfg = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    CHECK_NOTHROW(cfg.set_deserialize_nothrow("raft_first_layer_expansion", "xyz", ctxt, false));
    CHECK_NOTHROW(cfg.set_deserialize_nothrow("tree_support_wall_count", "", ctxt, false));
}
