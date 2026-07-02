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

    // why: local_activated and mine_activated tie on Status, so base order breaks the tie by
    //   declared source priority - Mine (ordinal 0) before Local (ordinal 2).
    const std::vector<std::string> expected = {
        "mine_activated",
        "local_activated",
        "mine_error",
        "local_inactive",
        "subscribed_loading",
    };
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Status, PluginSortOrder::Desc);

    // why: Desc reverses the status ordinal, but the Activated tie still resolves by ascending
    //   base order (Mine before Local) - the direction only flips the primary key.
    const std::vector<std::string> desc_expected = {
        "subscribed_loading",
        "local_inactive",
        "mine_error",
        "mine_activated",
        "local_activated",
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

TEST_CASE("plugin dialog name sort is case-insensitive and numeric-aware", "[plugin][sort]")
{
    std::vector<SortFixtureItem> items = {
        {"rig10", PluginSource::Local, PluginStatus::Activated, "script", "Rig 10"},
        {"ada_lower", PluginSource::Local, PluginStatus::Activated, "script", "ada"},
        {"rig2", PluginSource::Local, PluginStatus::Activated, "script", "Rig 2"},
        {"ada_upper", PluginSource::Local, PluginStatus::Activated, "script", "Ada"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Asc);

    const std::vector<std::string> expected = {"ada_upper", "ada_lower", "rig2", "rig10"};
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Desc);

    // why: names reverse ("Rig 10" before "Rig 2"), but "Ada"/"ada" tie on the case-insensitive
    //   key and keep ascending base order (case-sensitive "Ada" < "ada").
    const std::vector<std::string> desc_expected = {"rig10", "rig2", "ada_upper", "ada_lower"};
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

TEST_CASE("plugin dialog sort request parsing keeps previous state on invalid values", "[plugin][sort]")
{
    CHECK(plugin_sort_key_from_string("source", PluginSortKey::Status) == PluginSortKey::Source);
    CHECK(plugin_sort_key_from_string("missing", PluginSortKey::Name) == PluginSortKey::Name);

    CHECK(plugin_sort_order_from_string("desc", PluginSortOrder::Asc) == PluginSortOrder::Desc);
    CHECK(plugin_sort_order_from_string("down", PluginSortOrder::Asc) == PluginSortOrder::Asc);
}
