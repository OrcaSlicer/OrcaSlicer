#include <catch2/catch_test_macros.hpp>

#include "libslic3r/OnlineModels.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace fs = boost::filesystem;

TEST_CASE("Online Models download directory is resolved below data dir", "[OnlineModels]")
{
    const fs::path previous_data_dir = Slic3r::data_dir();
    const fs::path temp_data_dir = fs::temp_directory_path() / fs::unique_path("orca-online-models-%%%%-%%%%");
    fs::create_directories(temp_data_dir);

    Slic3r::set_data_dir(temp_data_dir.string());
    CHECK(Slic3r::online_models_download_dir() == (temp_data_dir / "OnlineModels").make_preferred());
    CHECK(Slic3r::online_models_download_dir(temp_data_dir) == (temp_data_dir / "OnlineModels").make_preferred());

    Slic3r::set_data_dir(previous_data_dir.string());
    boost::system::error_code ec;
    fs::remove_all(temp_data_dir, ec);
}

TEST_CASE("Online Models default provider configuration is valid", "[OnlineModels]")
{
    const auto& providers = Slic3r::online_models_default_providers();
    REQUIRE(providers.size() == 4);

    std::set<std::string> ids;
    for (const Slic3r::OnlineModelsProvider& provider : providers) {
        CHECK(!provider.id.empty());
        CHECK(!provider.display_name.empty());
        CHECK(provider.homepage_url.rfind("https://", 0) == 0);
        CHECK(!provider.allowed_hostnames.empty());
        CHECK(provider.enabled);
        CHECK(provider.built_in);
        CHECK(ids.insert(provider.id).second);
    }
}

TEST_CASE("Online Models providers serialize with stable identities", "[OnlineModels]")
{
    auto providers = Slic3r::online_models_default_providers();
    providers[0].display_name = "Renamed Printables";
    providers[1].enabled = false;
    providers.push_back({"custom-123", "Example Models", "https://example.com/", {}, "", true, false});

    const auto restored = Slic3r::online_models_deserialize_providers(
        Slic3r::online_models_serialize_providers(providers));
    REQUIRE(restored);
    CHECK(*restored == providers);
    CHECK((*restored)[0].id == "printables");
    CHECK((*restored)[0].display_name == "Renamed Printables");
    CHECK_FALSE((*restored)[1].enabled);
}

TEST_CASE("Online Models provider registry supports edits removal reorder and restoration", "[OnlineModels]")
{
    auto providers = Slic3r::online_models_default_providers();
    providers.erase(providers.begin());
    providers[0].enabled = false;
    providers.push_back({"custom-1", "Custom", "https://example.com/", {}, "", true, false});
    std::rotate(providers.begin(), providers.begin() + 1, providers.end());

    const std::string custom_id = providers[providers.size() - 2].id;
    Slic3r::online_models_restore_default_providers(providers);
    CHECK(providers.size() == 5);
    CHECK(std::count_if(providers.begin(), providers.end(), [](const auto& provider) {
        return provider.id == "printables";
    }) == 1);
    CHECK(std::any_of(providers.begin(), providers.end(), [&](const auto& provider) {
        return provider.id == custom_id;
    }));
    CHECK(Slic3r::online_models_enabled_providers({}).empty());
}

TEST_CASE("Online Models rejects unsafe and malformed provider URLs", "[OnlineModels]")
{
    bool unencrypted = false;
    std::string normalized;
    CHECK(Slic3r::online_models_validate_homepage_url("https://example.com", &unencrypted, &normalized));
    CHECK_FALSE(unencrypted);
    CHECK(normalized == "https://example.com/");
    CHECK(Slic3r::online_models_validate_homepage_url("http://example.com/models", &unencrypted));
    CHECK(unencrypted);
    for (const std::string& url : {"", "example.com", "file:///tmp/model.stl", "javascript:alert(1)",
                                   "data:text/plain,test", "https://", "https://bad host/"}) {
        CHECK_FALSE(Slic3r::online_models_validate_homepage_url(url));
    }
}

TEST_CASE("Online Models provider parser tolerates corrupt and incomplete storage", "[OnlineModels]")
{
    CHECK_FALSE(Slic3r::online_models_deserialize_providers("not json"));
    CHECK_FALSE(Slic3r::online_models_deserialize_providers(R"({"version":99,"providers":[]})"));
    CHECK_FALSE(Slic3r::online_models_deserialize_providers(
        R"({"version":1,"providers":[{"id":"","name":"","url":"file:///x"}]})"));

    const auto parsed = Slic3r::online_models_deserialize_providers(
        R"({"version":1,"providers":[{"id":"good","name":"Good","url":"https://example.com/"},{"id":"bad","name":"","url":"file:///x"},{"name":"missing id"}]})");
    REQUIRE(parsed);
    REQUIRE(parsed->size() == 1);
    CHECK(parsed->front().id == "good");
}

TEST_CASE("Online Models path components are sanitized", "[OnlineModels]")
{
    CHECK(Slic3r::online_models_sanitize_path_component("Creality Cloud") == "Creality Cloud");
    CHECK(Slic3r::online_models_sanitize_path_component("../bad:name") == ".._bad_name");
    CHECK(Slic3r::online_models_sanitize_path_component("..") == "unknown");
    CHECK(Slic3r::online_models_sanitize_path_component("   ") == "unknown");

    const Slic3r::OnlineModelsProvider provider{"bad/source", "Bad Source", "https://example.com/", {"example.com"}, "", true, false};
    CHECK(Slic3r::online_models_provider_dir("root", provider) == fs::path("root") / "bad_source");
    CHECK(Slic3r::online_models_sanitize_filename("../../evil.stl") == "evil.stl");
}

TEST_CASE("Online Models download paths and importable files are safe", "[OnlineModels]")
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("orca-online-models-path-%%%%");
    fs::create_directories(root);
    const Slic3r::OnlineModelsProvider provider{"custom-123", "Custom", "https://example.com/", {}, "", true, false};
    CHECK(Slic3r::online_models_download_date_dir(root, provider, "2026-08-28")
          == root / "custom-123" / "2026-08-28");

    std::ofstream((root / "model.stl").string()).put('\n');
    CHECK(Slic3r::online_models_unique_download_path(root, "model.stl").filename() == "model (2).stl");
    CHECK(Slic3r::online_models_unique_download_path(root, "../other.3mf").filename() == "other.3mf");

    for (const char* file : {"model.stl", "project.3MF", "models.zip", "part.step"})
        CHECK(Slic3r::online_models_is_importable_file(file));
    for (const char* file : {"readme.txt", "installer.exe", "archive.rar"})
        CHECK_FALSE(Slic3r::online_models_is_importable_file(file));

    boost::system::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("Online Models download queue tracks progress and terminal states", "[OnlineModels]")
{
    Slic3r::OnlineModelsDownloadQueue queue;
    CHECK(queue.add({"one", "printables", "model.stl"}));
    CHECK_FALSE(queue.add({"one", "makerworld", "duplicate.stl"}));
    REQUIRE(queue.find("one"));
    CHECK_FALSE(queue.find("one")->progress_percent());

    CHECK(queue.update("one", 25, 100, Slic3r::OnlineModelsDownloadState::Downloading));
    REQUIRE(queue.find("one")->progress_percent());
    CHECK(*queue.find("one")->progress_percent() == 25);
    CHECK(queue.find("one")->provider_id == "printables");

    // Source selection changes do not mutate the provider captured at start.
    CHECK(queue.add({"two", "makerworld", "other.3mf"}));
    CHECK(queue.find("one")->provider_id == "printables");
    CHECK(queue.find("two")->provider_id == "makerworld");

    CHECK(queue.update("one", 100, 100, Slic3r::OnlineModelsDownloadState::Completed));
    CHECK(queue.find("one")->terminal());
    CHECK_FALSE(queue.update("one", 50, 100, Slic3r::OnlineModelsDownloadState::Downloading));
    CHECK(queue.update("two", 0, -1, Slic3r::OnlineModelsDownloadState::Cancelled));
    CHECK(queue.find("two")->state == Slic3r::OnlineModelsDownloadState::Cancelled);
    CHECK(queue.remove("one"));
    CHECK_FALSE(queue.remove("missing"));
}

TEST_CASE("Online Models download queue records failures and clamps progress", "[OnlineModels]")
{
    Slic3r::OnlineModelsDownloadQueue queue;
    REQUIRE(queue.add({"failed", "thingiverse", "failed.zip"}));
    CHECK(queue.update("failed", 500, 100, Slic3r::OnlineModelsDownloadState::Failed, "disk full"));
    REQUIRE(queue.find("failed"));
    CHECK(queue.find("failed")->error == "disk full");
    CHECK(*queue.find("failed")->progress_percent() == 100);
    CHECK(queue.find("failed")->terminal());
}
