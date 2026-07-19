#include <catch2/catch_all.hpp>

#include <slic3r/GUI/PluginSort.hpp>

#include <string>
#include <vector>

using Slic3r::GUI::compare_ascii_case_insensitive_natural;
using Slic3r::GUI::PluginSortKey;
using Slic3r::GUI::PluginSortOrder;
using Slic3r::GUI::PluginSource;
using Slic3r::GUI::PluginStatus;
using Slic3r::GUI::plugin_sort_key_from_string;
using Slic3r::GUI::plugin_sort_order_from_string;
using Slic3r::GUI::sort_plugin_items_for_dialog;

namespace {

struct SortFixtureItem
{
    std::string plugin_key;
    PluginSource source;
    PluginStatus status;
    std::string type_key;
    std::string display_name;
    std::string sort_version;
};

std::vector<std::string> keys(const std::vector<SortFixtureItem>& items)
{
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const SortFixtureItem& item : items)
        result.push_back(item.plugin_key);
    return result;
}

} // namespace

TEST_CASE("plugin dialog status sort uses requested priority and base-order ties", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"local_inactive", PluginSource::Local, PluginStatus::Inactive, "script", "Local Inactive"},
        {"mine_error", PluginSource::Mine, PluginStatus::Error, "script", "Mine Error"},
        {"mine_activated", PluginSource::Mine, PluginStatus::Activated, "script", "Mine Activated"},
        {"local_activated", PluginSource::Local, PluginStatus::Activated, "script", "Local Activated"},
        {"subscribed_loading", PluginSource::Subscribed, PluginStatus::Loading, "script", "Subscribed Loading"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Status, PluginSortOrder::Asc);

    // why: local_activated and mine_activated tie on Status, so base order breaks the tie by name
    //   (case-insensitive) - "Local Activated" before "Mine Activated".
    const std::vector<std::string> expected = {
        "local_activated",
        "mine_activated",
        "mine_error",
        "local_inactive",
        "subscribed_loading",
    };
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Status, PluginSortOrder::Desc);

    // why: Desc reverses the status ordinal, but the Activated tie still resolves by ascending
    //   base order (name: "Local Activated" before "Mine Activated") - direction only flips the key.
    const std::vector<std::string> desc_expected = {
        "subscribed_loading",
        "local_inactive",
        "mine_error",
        "local_activated",
        "mine_activated",
    };
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("plugin dialog source sort uses enum priority", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"local", PluginSource::Local, PluginStatus::Activated, "script", "Local"},
        {"mine", PluginSource::Mine, PluginStatus::Activated, "script", "Mine"},
        {"subscribed", PluginSource::Subscribed, PluginStatus::Activated, "script", "Subscribed"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Source, PluginSortOrder::Asc);
    const std::vector<std::string> asc_expected = {"mine", "subscribed", "local"};
    CHECK(keys(items) == asc_expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Source, PluginSortOrder::Desc);
    const std::vector<std::string> desc_expected = {"local", "subscribed", "mine"};
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("plugin dialog version sort is semver-aware with base-order ties", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"v_1_2_0",  PluginSource::Local, PluginStatus::Activated, "script", "B", "1.2.0"},
        {"v_1_10_0", PluginSource::Local, PluginStatus::Activated, "script", "A", "1.10.0"},
        {"v_0_9_3",  PluginSource::Local, PluginStatus::Activated, "script", "C", "0.9.3"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Version, PluginSortOrder::Asc);
    // why: semver numeric compare - 1.10.0 > 1.2.0 (not lexical "1.10" < "1.2"), so ascending is
    //   0.9.3 < 1.2.0 < 1.10.0.
    const std::vector<std::string> asc_expected = {"v_0_9_3", "v_1_2_0", "v_1_10_0"};
    CHECK(keys(items) == asc_expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Version, PluginSortOrder::Desc);
    const std::vector<std::string> desc_expected = {"v_1_10_0", "v_1_2_0", "v_0_9_3"};
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("plugin dialog name sort is case-insensitive and numeric-aware", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"rig10", PluginSource::Local, PluginStatus::Activated, "script", "Rig 10"},
        {"ada_lower", PluginSource::Local, PluginStatus::Activated, "script", "ada"},
        {"rig2", PluginSource::Local, PluginStatus::Activated, "script", "Rig 2"},
        {"ada_upper", PluginSource::Local, PluginStatus::Activated, "script", "Ada"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Asc);

    // why: "Ada"/"ada" tie on the case-insensitive name (primary AND base name level), so the tie
    //   falls through source/status/type to plugin_key: "ada_lower" before "ada_upper".
    const std::vector<std::string> expected = {"ada_lower", "ada_upper", "rig2", "rig10"};
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Desc);

    // why: names reverse ("Rig 10" before "Rig 2"), but "Ada"/"ada" tie on the case-insensitive
    //   key and keep ascending base order, which resolves by plugin_key ("ada_lower" < "ada_upper").
    const std::vector<std::string> desc_expected = {"rig10", "rig2", "ada_lower", "ada_upper"};
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("natural compare handles digits, case, prefixes and leading zeros", "[plugin][sort]")
{
    // numeric runs compare by value, not lexically
    CHECK(compare_ascii_case_insensitive_natural("item2", "item10") < 0);
    CHECK(compare_ascii_case_insensitive_natural("item10", "item2") > 0);
    CHECK(compare_ascii_case_insensitive_natural("2", "10") < 0);

    // case is ignored on the primary comparison
    CHECK(compare_ascii_case_insensitive_natural("Camera", "camera") == 0);

    // a prefix is less than the longer string it prefixes
    CHECK(compare_ascii_case_insensitive_natural("app", "apple") < 0);
    CHECK(compare_ascii_case_insensitive_natural("apple", "app") > 0);

    // equal numeric value: fewer leading zeros wins the tie
    CHECK(compare_ascii_case_insensitive_natural("1", "01") < 0);
    CHECK(compare_ascii_case_insensitive_natural("01", "1") > 0);

    // reflexivity and empty-string boundaries
    CHECK(compare_ascii_case_insensitive_natural("plugin", "plugin") == 0);
    CHECK(compare_ascii_case_insensitive_natural("", "") == 0);
    CHECK(compare_ascii_case_insensitive_natural("", "a") < 0);
}

TEST_CASE("plugin dialog None sort key falls to ascending base order in both directions", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"z_mine", PluginSource::Mine, PluginStatus::Activated, "script", "Zebra"},
        {"a_local", PluginSource::Local, PluginStatus::Activated, "script", "Apple"},
        {"m_sub", PluginSource::Subscribed, PluginStatus::Error, "script", "Mango"},
    };

    // why: no primary key -> pure name-first base order (Apple < Mango < Zebra). A source-first
    //   baseline would instead give {z_mine, m_sub, a_local}, so this pins the name-first order.
    const std::vector<std::string> base_expected = {"a_local", "m_sub", "z_mine"};

    sort_plugin_items_for_dialog(items, PluginSortKey::None, PluginSortOrder::Asc);
    CHECK(keys(items) == base_expected);

    // why: None has no direction - Desc must not reverse the baseline.
    sort_plugin_items_for_dialog(items, PluginSortKey::None, PluginSortOrder::Desc);
    CHECK(keys(items) == base_expected);
}

TEST_CASE("plugin dialog sort request parsing keeps previous state on invalid values", "[plugin][sort]")
{
    CHECK(plugin_sort_key_from_string("source", PluginSortKey::Status) == PluginSortKey::Source);
    CHECK(plugin_sort_key_from_string("none", PluginSortKey::Status) == PluginSortKey::None);
    CHECK(plugin_sort_key_from_string("missing", PluginSortKey::Name) == PluginSortKey::Name);

    CHECK(plugin_sort_order_from_string("desc", PluginSortOrder::Asc) == PluginSortOrder::Desc);
    CHECK(plugin_sort_order_from_string("down", PluginSortOrder::Asc) == PluginSortOrder::Asc);
}
