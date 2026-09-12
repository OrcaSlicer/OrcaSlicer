#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AIModelOutputDirectory.hpp"
#include "test_utils.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/image.h>
#include <wx/imagpng.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <stdexcept>

using Slic3r::GUI::AIModelGenerationClient;
using namespace Slic3r::GUI::ModelGenerationPresentation;

namespace {

class ScopedEnvironmentValue
{
public:
    ScopedEnvironmentValue(const char* name, const char* value) : m_name(name)
    {
        if (const char* previous = boost::nowide::getenv(name)) m_previous = previous;
        const int result = value != nullptr ? boost::nowide::setenv(name, value, 1)
                                             : boost::nowide::unsetenv(name);
        if (result != 0) throw std::runtime_error("Unable to set test environment variable");
    }
    ~ScopedEnvironmentValue()
    {
        if (m_previous) boost::nowide::setenv(m_name.c_str(), m_previous->c_str(), 1);
        else boost::nowide::unsetenv(m_name.c_str());
    }

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

class ScopedWorkingDirectory
{
public:
    explicit ScopedWorkingDirectory(const boost::filesystem::path& directory)
        : m_previous(boost::filesystem::current_path())
    {
        boost::filesystem::current_path(directory);
    }
    ~ScopedWorkingDirectory()
    {
        boost::system::error_code ignored;
        boost::filesystem::current_path(m_previous, ignored);
    }

private:
    boost::filesystem::path m_previous;
};

nlohmann::json write_design_fixture(const boost::filesystem::path& directory)
{
    boost::filesystem::create_directories(directory);
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler());
    wxImage image(64, 64);
    image.SetRGB(wxRect(0, 0, 64, 64), 24, 128, 200);
    REQUIRE(image.SaveFile((directory / "preview.png").wstring(), wxBITMAP_TYPE_PNG));
    boost::filesystem::copy_file(directory / "preview.png", directory / "input.png");
    nlohmann::json record = {
        {"version", 1}, {"id", directory.filename().string()}, {"source", "image"},
        {"state", "awaiting_confirmation"}, {"user_prompt", "cat with a blue scarf"},
        {"input_path", "input.png"}, {"preview_path", "preview.png"},
        {"raw_preview_path", "preview.png"}, {"updated_at", 1789132000.25}
    };
    REQUIRE(write_json(directory / "job.json", record));
    return record;
}

} // namespace

TEST_CASE("Design history preserves separate image versions before any model exists",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-design-history");
    const std::string first = "11111111-1111-4111-8111-111111111111";
    const std::string second = "22222222-2222-4222-8222-222222222222";
    auto record = write_design_fixture(temporary.path() / first);
    write_design_fixture(temporary.path() / second);
    const auto original = read_design_history_entry(temporary.path(), first);
    REQUIRE(original);
    CHECK(original->job_id == first);
    CHECK(original->prompt == "cat with a blue scarf");
    CHECK(original->state == "awaiting_confirmation");
    CHECK(original->input_path == temporary.path() / first / "input.png");
    CHECK(read_design_history_entry(temporary.path(), second).has_value());
    CHECK(has_persisted_generation_assets(temporary.path(), first));
    record["updated_at"] = 1789233000.875;
    REQUIRE(write_json(temporary.path() / first / "job.json", record));
    const auto restored = read_design_history_entry(temporary.path(), first);
    REQUIRE(restored);
    CHECK(restored->generated_at == original->generated_at);
}

TEST_CASE("Stopped and failed designs retain raw images without becoming confirmed jobs",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-stopped-design");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const auto directory = temporary.path() / id;
    auto record = write_design_fixture(directory);
    record["preview_path"] = nullptr;
    record["input_path"] = nullptr;
    for (const char* state : {"stopped", "failed"}) {
        record["state"] = state;
        REQUIRE(write_json(directory / "job.json", record));
        const auto entry = read_design_history_entry(temporary.path(), id);
        REQUIRE(entry);
        CHECK(entry->state == state);
        CHECK(entry->input_path.empty());
        CHECK(entry->preview_path == directory / "preview.png");
        CHECK(has_persisted_generation_assets(temporary.path(), id));
    }
    record["state"] = "preprocessing";
    REQUIRE(write_json(directory / "job.json", record));
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
}

TEST_CASE("Design history rejects foreign paths invalid images and malformed records",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-design-record-validation");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const std::string sibling = "22222222-2222-4222-8222-222222222222";
    const auto directory = temporary.path() / id;
    const auto record = write_design_fixture(directory);
    write_design_fixture(temporary.path() / sibling);
    for (const std::string& value : std::vector<std::string>{"../" + sibling + "/preview.png", "/missing.png", "missing.png"}) {
        auto invalid = record;
        invalid["preview_path"] = value;
        invalid["raw_preview_path"] = value;
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    for (const auto& invalid_value : std::vector<nlohmann::json>{7, nullptr, nlohmann::json::array()}) {
        auto invalid = record;
        invalid["state"] = invalid_value;
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    for (const nlohmann::json& patch : std::vector<nlohmann::json>{
             {{"id", sibling}}, {{"version", 2}}, {{"source", "local_finishing"}},
             {{"user_prompt", false}}, {{"user_prompt", std::string(65536, 'x')}}}) {
        auto invalid = record;
        invalid.update(patch);
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    REQUIRE(write_json(directory / "job.json", record));
    boost::filesystem::ofstream(directory / "preview.png", std::ios::binary) << "not an image";
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    CHECK_FALSE(has_persisted_generation_assets(temporary.path(), id));
    CHECK_FALSE(read_design_history_entry(temporary.path(), "../" + id));
}

TEST_CASE("Existing model history takes precedence over design entries and remains preserved",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-model-before-design");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const auto directory = temporary.path() / id;
    write_design_fixture(directory);
    boost::filesystem::ofstream(directory / "model-vertex-color.obj") << "v 0 0 0\n";
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    CHECK(has_persisted_generation_assets(temporary.path(), id));
    boost::filesystem::remove(directory / "job.json");
    CHECK(has_persisted_generation_assets(temporary.path(), id));
}

TEST_CASE("AI model history follows the Orca data directory across launch locations",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path user_data = temporary.path() / "user data";
    const fs::path first_launch = temporary.path() / "first launch";
    const fs::path second_launch = temporary.path() / "second launch";
    fs::create_directories(first_launch);
    fs::create_directories(second_launch);
    for (const char* override_value : {static_cast<const char*>(nullptr), ""}) {
        DYNAMIC_SECTION((override_value == nullptr ? "unset override" : "empty override")) {
            ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", override_value);
            ScopedWorkingDirectory working_directory(first_launch);
            const Slic3r::GUI::AIModelOutputDirectory first(user_data);
            fs::current_path(second_launch);
            const Slic3r::GUI::AIModelOutputDirectory second(user_data);
            CHECK(first.root() == user_data / "generated_models");
            CHECK(second.root() == first.root());
            CHECK_FALSE(fs::exists(first.root()));
            CHECK_FALSE(fs::exists(first_launch / "generated_models"));
        }
    }
}

TEST_CASE("AI model history preserves an explicit absolute output directory",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path custom_output = temporary.path() / "custom model output";
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", custom_output.string().c_str());
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "other user data");
    CHECK(directory.root() == custom_output);
    CHECK_FALSE(fs::exists(directory.root()));
}

TEST_CASE("AI model output remains fixed after working directory and environment changes",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path parent_start = temporary.path() / "parent start";
    const fs::path child_start = temporary.path() / "bootstrap directory";
    fs::create_directories(parent_start);
    fs::create_directories(child_start);
    ScopedWorkingDirectory working_directory(parent_start);
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", "relative output/../model assets");
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "user data");
    const fs::path expected = parent_start / "model assets";
    REQUIRE(directory.root().is_absolute());
    REQUIRE(directory.root() == expected);

    fs::current_path(child_start);
    ScopedEnvironmentValue changed_environment("ORCASLICER_AI_OUTPUT_DIR", "different output");
    boost::process::environment child_environment;
    directory.configure_child_environment(child_environment);
    CHECK(directory.root() == expected);
    CHECK(fs::path(child_environment["ORCASLICER_AI_OUTPUT_DIR"].to_string()) == expected);
    CHECK_FALSE(fs::exists(child_start / "model assets"));
}

TEST_CASE("AI model output adds only its explicit path to the child environment",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    ScopedTemporaryDir temporary("orca-model-output");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    ScopedEnvironmentValue unrelated("ORCA_MODEL_OUTPUT_TEST_UNRELATED", "not-for-child");
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path());
    boost::process::environment child_environment;
    child_environment["ORCA_MODEL_OUTPUT_TEST_EXISTING"] = "preserved";
    directory.configure_child_environment(child_environment);
    CHECK(child_environment["ORCA_MODEL_OUTPUT_TEST_EXISTING"].to_string() == "preserved");
    CHECK(child_environment["ORCASLICER_AI_OUTPUT_DIR"].to_string() == directory.root().string());
    CHECK(child_environment.find("ORCA_MODEL_OUTPUT_TEST_UNRELATED") == child_environment.end());
}

#ifdef _WIN32
TEST_CASE("AI model paths retain Unicode in the Windows child environment",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    ScopedTemporaryDir temporary("orca-model-output");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    ScopedEnvironmentValue unrelated("ORCA_MODEL_OUTPUT_TEST_UNRELATED", "not-for-child");
    const boost::filesystem::path user_data = temporary.path() / L"\u7528\u6237 \u6a21\u578b";
    const Slic3r::GUI::AIModelOutputDirectory directory(user_data);
    boost::process::environment inherited;
    inherited["ORCA_MODEL_OUTPUT_TEST_EXISTING"] = "preserved";
    boost::process::wenvironment launch_environment(inherited);
    directory.configure_child_environment(launch_environment);
    CHECK(launch_environment[L"ORCASLICER_AI_OUTPUT_DIR"].to_string() ==
          (user_data / "generated_models").wstring());
    CHECK(launch_environment[L"ORCA_MODEL_OUTPUT_TEST_EXISTING"].to_string() == L"preserved");
    CHECK(launch_environment.find(L"ORCA_MODEL_OUTPUT_TEST_UNRELATED") == launch_environment.end());
}
#endif

TEST_CASE("model-generation progress maps service phases to stable UI milestones",
          "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::JobStatus status;
    REQUIRE(status.palette_color_count == Slic3r::AI::kLegacyDefaultTargetPaletteColors);

    status.state = "recommending_palette";
    status.progress = 5;
    REQUIRE(display_progress(status) == 3);
    status.progress = 10;
    REQUIRE(display_progress(status) == 10);

    status.state = "awaiting_palette_confirmation";
    REQUIRE(display_progress(status) == 10);
    status.state = "preprocessing";
    status.progress = 15;
    REQUIRE(display_progress(status) == 25);
    status.state = "awaiting_confirmation";
    REQUIRE(display_progress(status) == 35);

    status.state.clear();
    status.phase = "generating";
    status.progress = 70;
    REQUIRE(display_progress(status) == 78);
    status.phase = "converting";
    status.progress = 95;
    REQUIRE(display_progress(status) == 90);
    status.phase = "downloading_artifact";
    REQUIRE(display_progress(status) == 92);
    status.phase = "checking_model";
    status.progress = 99;
    REQUIRE(display_progress(status) == 97);
    status.phase = "checking_visual";
    REQUIRE(display_progress(status) == 98);

    status.phase.clear();
    status.state = "ready";
    REQUIRE(display_progress(status) == 100);
}

TEST_CASE("Server palette corrections preserve previews without overwriting user edits", "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette = {"#F4F4F0", "#1F1B1C", "#F2C9AE"};
    const AIModelGenerationClient::PaletteRoles submitted = {
        {"primary", palette[2]}, {"structure", palette[1]}, {"light", palette[0]}};
    const AIModelGenerationClient::PaletteRoles corrected = {
        {"primary", palette[0]}, {"structure", palette[1]}, {"light", palette[2]}};
    auto current = submitted;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == corrected);
    synchronize_palette_roles(palette, current, palette, corrected, palette, corrected);
    CHECK(current == corrected);

    auto edited_palette = palette;
    edited_palette[0] = "#FFFFFF";
    current = submitted;
    synchronize_palette_roles(edited_palette, current, palette, submitted, palette, corrected);
    CHECK(current == submitted);
    synchronize_palette_roles(palette, current, palette, submitted, edited_palette, corrected);
    CHECK(current == submitted);

    auto edited_roles = submitted;
    std::swap(edited_roles["structure"], edited_roles["primary"]);
    current = edited_roles;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == edited_roles);
    synchronize_palette_roles(palette, current, palette, edited_roles, palette, {});
    CHECK(current == edited_roles);
}

TEST_CASE("automatic printable palette roles remain deterministic and distinct",
          "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette {
        "#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0066FF", "#9933CC"
    };
    for (size_t count = 1; count <= palette.size(); ++count) {
        DYNAMIC_SECTION(count << " colors receive a complete stable role prefix") {
            const std::vector<std::string> active(palette.begin(), palette.begin() + count);
            const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(active);
            REQUIRE(roles.size() == count);
            for (size_t index = 0; index < PALETTE_ROLE_IDS.size(); ++index)
                CHECK(roles.count(PALETTE_ROLE_IDS[index]) == (index < count ? 1 : 0));
            std::set<std::string> assigned;
            for (const auto& [role, color] : roles) {
                CHECK(std::find(active.begin(), active.end(), color) != active.end());
                assigned.insert(color);
            }
            CHECK(assigned.size() == count);
            CHECK(automatic_palette_roles(active) == roles);
        }
    }

    const std::vector<std::string> legacy_palette(palette.begin(), palette.begin() + 4);
    const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(legacy_palette);
    REQUIRE(roles.at("structure") == "#000000");
    REQUIRE(roles.at("light") == "#FFFFFF");
    REQUIRE(roles.at("primary") == "#00FF00");
    REQUIRE(roles.at("accent") == "#FF0000");
    REQUIRE(same_palette_color("#aBc123", "#AbC123"));
    REQUIRE_FALSE(same_palette_color("#ABC123", "#ABC124"));
    CHECK(automatic_palette_roles({"#000000", "#FFFFFF", "#FF0000", "#00FF00",
                                   "#0066FF", "#9933CC", "#00FFFF"}).empty());
    CHECK(automatic_palette_roles({"#000000", "invalid"}).empty());
    CHECK(automatic_palette_roles({"#000000", "#000000"}).empty());
}

TEST_CASE("style families retain legacy styles in a compact secondary choice",
          "[ModelGenerationPresentation]")
{
    CHECK(style_selection("portrait_sketch") == 2);
    CHECK(style_selection("ink_relief") == 2);
    CHECK(style_selection("sculpture") == 0);
    CHECK(style_selection("realistic") == 1);
    for (const std::string style : {"portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"}) {
        CHECK(style_selection(style) == 2);
        CHECK(selected_style(2, stylized_style_selection(style)) == style);
    }
    CHECK(selected_style(0, 5) == "sculpture");
    CHECK(selected_style(1, 5) == "realistic");
    CHECK(selected_style(2, -1) == "cartoon");
    CHECK(style_uses_printable_colors("portrait_sketch"));
    CHECK(style_uses_printable_colors("ink_relief"));
}
