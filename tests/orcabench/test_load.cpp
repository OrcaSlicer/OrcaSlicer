#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Catalog.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"
#include "core/Runner.hpp"
#include "core/Workload.hpp"
#include "slicer/Environment.hpp"
#include "slicer/Fixtures.hpp"
#include "slicer/Output.hpp"

#include "orcabench_slicer_test_utils.hpp"
#include "test_utils.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <miniz.h>

#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;

namespace {

Policy timing(StageSet stages, unsigned iterations)
{
    PolicyOverrides overrides;
    overrides.warmup     = 0;
    overrides.iterations = iterations;
    overrides.stages     = std::move(stages);
    return Policy::resolve("quick", overrides, 2);
}

CatalogEntry loading(std::string fixture, Properties config = {})
{
    CatalogEntry entry;
    entry.name    = "load/test/file";
    entry.kind    = "load";
    entry.fixture = std::move(fixture);
    entry.config  = std::move(config);
    return entry;
}

// A run whose temporary directory does not exist, so nothing can be written there.
class MissingTemporaryDir : public RunEnvironment
{
public:
    void enter(const Policy& policy) override
    {
        m_environment.enter(policy);
        set_temporary_dir(temporary_dir() + "/missing");
    }
    void leave() noexcept override { m_environment.leave(); }

private:
    SlicerEnvironment m_environment {ORCABENCH_RESOURCES_DIR};
};

WorkloadResult load(const CatalogEntry& entry, const Policy& policy)
{
    SlicerEnvironment environment(ORCABENCH_RESOURCES_DIR);
    Result            result = run_suite({entry}, policy, WorkloadKinds::instance(), environment);
    REQUIRE(result.workloads.size() == 1);
    return std::move(result.workloads.front());
}

} // namespace

TEST_CASE("a load times reading the file as its one step, with the facets, objects and parts it read", "[OrcaBench][Load]")
{
    const WorkloadResult loaded = load(loading("handy:OrcaBadge.3mf"), timing({Stage::Load}, 1));
    CAPTURE(loaded.reason);
    REQUIRE(loaded.outcome == Outcome::Ran);
    REQUIRE(loaded.iterations.size() == 1);
    const IterationResult& pass = loaded.iterations.front();
    REQUIRE(pass.timeline.size() == 1);
    const StageSpan& span = pass.timeline.front();
    CHECK(span.stage == "read_from_file");
    CHECK(span.scope == "print");
    CHECK(span.metrics.at("facets") == 72586.);
    CHECK(span.metrics.at("objects") == 3.);
    CHECK(span.metrics.at("volumes") == 39.);
    CHECK(pass.unfinished.empty());
    CHECK(pass.not_run.empty());
    CHECK(loaded.output_hash.has_value());
}

TEST_CASE("every pass of a load reads the same geometry", "[OrcaBench][Load]")
{
    const WorkloadResult loaded = load(loading("handy:Stanford_Bunny.drc"), timing({Stage::Load}, 3));
    CAPTURE(loaded.reason);
    REQUIRE(loaded.outcome == Outcome::Ran);
    CHECK(loaded.iterations.size() == 3);
    CHECK(loaded.output_hash.has_value());
}

TEST_CASE("a load reads the fixture written as a binary or an ASCII STL when its format asks, the same geometry either way",
          "[OrcaBench][Load]")
{
    const WorkloadResult binary = load(loading("procedural:smoke-cube", {{"format", "stl"}}), timing({Stage::Load}, 1));
    const WorkloadResult ascii  = load(loading("procedural:smoke-cube", {{"format", "stl-ascii"}}), timing({Stage::Load}, 1));
    for (const WorkloadResult* loaded : {&binary, &ascii}) {
        CAPTURE(loaded->reason);
        REQUIRE(loaded->outcome == Outcome::Ran);
        REQUIRE(loaded->iterations.size() == 1);
        REQUIRE(loaded->iterations.front().timeline.size() == 1);
        CHECK(loaded->iterations.front().timeline.front().metrics.at("facets") == 12.);
        REQUIRE(loaded->output_hash.has_value());
    }
    CHECK(binary.output_hash == ascii.output_hash);
}

TEST_CASE("a model's hash changes with a part's vertices, triangles or placement and an instance's placement", "[OrcaBench][Load]")
{
    using Change                 = std::function<void(Model&)>;
    const auto [changed, change] = GENERATE(table<std::string, Change>({
        {"a vertex",
         [](Model& model) {
             ModelVolume&         part = *model.objects.front()->volumes.front();
             indexed_triangle_set mesh = part.mesh().its;
             mesh.vertices.front().x() += 1.f;
             part.set_mesh(std::move(mesh));
         }},
        {"a triangle",
         [](Model& model) {
             ModelVolume&         part = *model.objects.front()->volumes.front();
             indexed_triangle_set mesh = part.mesh().its;
             std::swap(mesh.indices.front()[1], mesh.indices.front()[2]);
             part.set_mesh(std::move(mesh));
         }},
        {"a part's placement", [](Model& model) { model.objects.front()->volumes.front()->set_offset(Vec3d(1., 0., 0.)); }},
        {"an instance's placement",
         [](Model& model) {
             ModelInstance& instance = *model.objects.front()->instances.front();
             instance.set_offset(instance.get_offset() + Vec3d(1., 0., 0.));
         }},
    }));
    CAPTURE(changed);
    const FixtureModel cube = load_fixture("procedural:smoke-cube");
    REQUIRE(cube.model);
    Model copy(*cube.model);
    REQUIRE(model_hash(copy) == model_hash(*cube.model));
    change(copy);
    CHECK(model_hash(copy) != model_hash(*cube.model));
}

TEST_CASE("a load reads a 3MF that embeds a preset", "[OrcaBench][Load]")
{
    const ScopedTemporaryDir resources("orcabench");
    boost::filesystem::create_directories(resources.path() / "handy_models");
    const std::string project = (resources.path() / "handy_models" / "Embedded.3mf").string();
    boost::filesystem::copy_file(std::string(ORCABENCH_RESOURCES_DIR) + "/handy_models/OrcaBadge.3mf", project);
    const std::string preset = R"({"filament_settings_id": ["Embedded"], "from": "project", "name": "Embedded", "version": "2.5.0.0"})";
    REQUIRE(mz_zip_add_mem_to_archive_file_in_place(project.c_str(), "Metadata/filament_settings_1.config", preset.data(), preset.size(),
                                                    nullptr, 0, MZ_DEFAULT_COMPRESSION));

    SlicerEnvironment environment(resources.string());
    const Result      result = run_suite({loading("handy:Embedded.3mf")}, timing({Stage::Load}, 1), WorkloadKinds::instance(), environment);
    REQUIRE(result.workloads.size() == 1);
    CAPTURE(result.workloads.front().reason);
    CHECK(result.workloads.front().outcome == Outcome::Ran);
}

TEST_CASE("a load times nothing when the run does not time loading", "[OrcaBench][Load]")
{
    const WorkloadResult loaded = load(loading("handy:Stanford_Bunny.drc"), timing({Stage::Process, Stage::Export}, 1));
    CAPTURE(loaded.reason);
    REQUIRE(loaded.outcome == Outcome::Ran);
    REQUIRE(loaded.iterations.size() == 1);
    CHECK(loaded.iterations.front().timeline.empty());
    CHECK_FALSE(loaded.output_hash.has_value());
}

TEST_CASE("a load whose fixture is missing is skipped with the path it looked for", "[OrcaBench][Load]")
{
    for (const CatalogEntry& entry : {loading("handy:missing.drc"), loading("handy:missing.drc", {{"format", "stl"}})}) {
        const WorkloadResult loaded = load(entry, timing({Stage::Load}, 1));
        CAPTURE(entry.config);
        CHECK(loaded.outcome == Outcome::Skipped);
        CHECK_THAT(loaded.reason, StartsWith("fixture not available: ") && EndsWith("missing.drc is missing"));
    }
}

TEST_CASE("a load refuses a procedural fixture without a format, a format it does not know and any other setting", "[OrcaBench][Load]")
{
    const auto [entry, refusal] = GENERATE(table<CatalogEntry, std::string>({
        {loading("procedural:smoke-cube"), "procedural:smoke-cube has no file to read, so the entry needs a format"},
        {loading("handy:3DBenchy.drc", {{"format", "obj"}}), "a load's format is stl or stl-ascii, not 'obj'"},
        {loading("handy:3DBenchy.drc", {{"layer_height", "0.2"}}), "a load has no setting called 'layer_height'"},
    }));
    CAPTURE(entry.fixture);
    const WorkloadResult loaded = load(entry, timing({Stage::Load}, 1));
    CHECK(loaded.outcome == Outcome::Failed);
    CHECK_THAT(loaded.reason, StartsWith("setup() threw: ") && ContainsSubstring(refusal));
}

TEST_CASE("a load that cannot write its STL fails in setup, naming the file", "[OrcaBench][Load]")
{
    MissingTemporaryDir environment;
    const Result result = run_suite({loading("procedural:smoke-cube", {{"format", "stl"}})}, timing({Stage::Load}, 1),
                                    WorkloadKinds::instance(), environment);
    REQUIRE(result.workloads.size() == 1);
    CHECK(result.workloads.front().outcome == Outcome::Failed);
    CHECK_THAT(result.workloads.front().reason, StartsWith("setup() threw: cannot write "));
}

TEST_CASE("the loading catalog reads three formats of one mesh, two 3MF projects and a procedural STL", "[OrcaBench][Load]")
{
    const std::vector<CatalogEntry> catalog = shipped_catalog("loading.json");
    REQUIRE_FALSE(catalog.empty());
    std::set<std::string> names;
    for (const CatalogEntry& entry : catalog) {
        CAPTURE(entry.name);
        CHECK(entry.kind == "load");
        CHECK(std::regex_match(entry.name, std::regex("load/[a-z0-9-]+/(drc|3mf|stl|stl-ascii)")));
        names.insert(entry.name);
    }
    CHECK(names == std::set<std::string> {"load/3dbenchy/drc", "load/3dbenchy/stl", "load/3dbenchy/stl-ascii", "load/stanford-bunny/drc",
                                          "load/orcabadge/3mf", "load/orcasliced/3mf", "load/fine-sphere/stl"});
}

TEST_CASE("every file the loading catalog reads is there", "[OrcaBench][Load]")
{
    const TreeResources resources;
    for (const CatalogEntry& entry : shipped_catalog("loading.json")) {
        CAPTURE(entry.name);
        const std::optional<std::string> path = fixture_path(entry.fixture);
        if (entry.config.empty())
            REQUIRE(path);
        if (path)
            CHECK(boost::filesystem::exists(*path));
    }
}
