#include "core/Catalog.hpp"

#include "core/Workload.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

using Json = nlohmann::json;

const std::set<std::string> entry_fields = {"name", "kind", "fixture", "tier", "pgo_eligible", "tags", "config"};

std::string text_of(const Json& value, const std::string& what)
{
    if (!value.is_string())
        throw CatalogError(what + " is not a string: " + value.dump());
    return value.get<std::string>();
}

std::string required_text(const Json& json, const char* key, const std::string& whose)
{
    if (!json.contains(key))
        throw CatalogError(whose + " has no " + key);
    return text_of(json[key], whose + "'s " + key);
}

CatalogEntry read_entry(const Json& json)
{
    if (!json.is_object())
        throw CatalogError("an entry is not an object: " + json.dump());
    for (const auto& field : json.items())
        if (entry_fields.count(field.key()) == 0)
            throw CatalogError("an entry has a field this build does not know: " + field.key());
    CatalogEntry entry;
    entry.name = required_text(json, "name", "an entry");
    entry.kind = required_text(json, "kind", entry.name);
    if (json.contains("fixture"))
        entry.fixture = text_of(json["fixture"], entry.name + "'s fixture");
    if (json.contains("tier")) {
        const std::string tier = text_of(json["tier"], entry.name + "'s tier");
        if (tier != "macro" && tier != "micro")
            throw CatalogError(entry.name + "'s tier is neither macro nor micro: " + tier);
        entry.tier = tier == "macro" ? Tier::Macro : Tier::Micro;
    }
    if (json.contains("pgo_eligible")) {
        if (!json["pgo_eligible"].is_boolean())
            throw CatalogError(entry.name + "'s pgo_eligible is not true or false");
        entry.pgo_eligible = json["pgo_eligible"].get<bool>();
    }
    if (json.contains("tags")) {
        if (!json["tags"].is_array())
            throw CatalogError(entry.name + "'s tags are not an array");
        for (const Json& tag : json["tags"])
            entry.tags.insert(text_of(tag, entry.name + "'s tag"));
    }
    if (json.contains("config")) {
        if (!json["config"].is_object())
            throw CatalogError(entry.name + "'s config is not an object");
        for (const auto& [key, value] : json["config"].items())
            entry.config[key] = text_of(value, entry.name + "'s " + key);
    }
    try {
        validate(entry);
    } catch (const WorkloadError& error) {
        throw CatalogError(error.what());
    }
    return entry;
}

} // namespace

std::vector<CatalogEntry> read_catalog(std::string_view text)
{
    try {
        const Json json = Json::parse(text.begin(), text.end());
        if (!json.is_object() || json.size() != 1 || !json.contains("entries") || !json["entries"].is_array())
            throw CatalogError("a catalog is an object holding an entries array and nothing else");
        std::vector<CatalogEntry> catalog;
        std::set<std::string>     names;
        for (const Json& item : json["entries"]) {
            CatalogEntry entry = read_entry(item);
            if (!names.insert(entry.name).second)
                throw CatalogError(entry.name + " appears twice");
            catalog.push_back(std::move(entry));
        }
        return catalog;
    } catch (const Json::exception& error) {
        // Converts the parser's exceptions, so no caller needs json.hpp to catch them.
        throw CatalogError(error.what());
    }
}

std::vector<CatalogEntry> read_catalog_dir(const std::string& directory)
{
    std::error_code                    error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator item(std::filesystem::u8path(directory), error), end; !error && item != end;
         item.increment(error))
        if (item->path().extension() == ".json")
            files.push_back(item->path());
    if (error)
        throw CatalogError("cannot read the catalog directory " + directory + ": " + error.message());
    std::sort(files.begin(), files.end());

    std::vector<CatalogEntry> catalog;
    std::set<std::string>     names;
    for (const std::filesystem::path& path : files) {
        std::ifstream     file(path, std::ios::binary);
        const std::string text {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!file.is_open() || file.bad())
            throw CatalogError("cannot read " + path.u8string());
        try {
            for (CatalogEntry& entry : read_catalog(text)) {
                if (!names.insert(entry.name).second)
                    throw CatalogError(entry.name + " appears twice");
                catalog.push_back(std::move(entry));
            }
        } catch (const CatalogError& refused) {
            throw CatalogError(path.u8string() + ": " + refused.what());
        }
    }
    return catalog;
}

std::vector<CatalogEntry> select_entries(const std::vector<CatalogEntry>& catalog, const Selection& selection)
{
    std::vector<CatalogEntry> selected;
    std::copy_if(catalog.begin(), catalog.end(), std::back_inserter(selected), [&selection](const CatalogEntry& entry) {
        return (!selection.filter || matches(*selection.filter, entry.name)) &&
               (!selection.exclude || !matches(*selection.exclude, entry.name)) && (!selection.kind || entry.kind == *selection.kind) &&
               (!selection.tag || entry.tags.count(*selection.tag) != 0);
    });
    if (selected.empty()) {
        std::string asked;
        for (const auto& [flag, value] : {std::pair {"--filter", selection.filter}, std::pair {"--exclude", selection.exclude},
                                          std::pair {"--kind", selection.kind}, std::pair {"--tag", selection.tag}})
            if (value)
                asked += std::string(asked.empty() ? "" : " ") + flag + " '" + *value + "'";
        throw CatalogError(asked.empty() ? "the catalog has no workloads" : "no workload in the catalog matches " + asked);
    }
    return selected;
}

}} // namespace Slic3r::Bench
