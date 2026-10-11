#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"
#include "core/Workload.hpp"
#include "slicer/Fixtures.hpp"
#include "slicer/Output.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r { namespace Bench {

namespace {

// Reads a fixture's file, or an STL written from the fixture when the entry's format asks, timing the
// read as Load's one step.
class LoadWorkload : public Workload
{
public:
    explicit LoadWorkload(CatalogEntry entry) : m_entry(std::move(entry)) {}
    // Detaches the last pass's backup folder instead of deleting it, since a destructor cannot report a
    // failure, and leave() deletes it with the run's temporary directory.
    ~LoadWorkload() override
    {
        if (m_model)
            m_model->set_backup_path("detach");
    }

    std::optional<std::string> setup(const RunContext&) override
    {
        std::string format;
        for (const auto& [key, value] : m_entry.config) {
            if (key != "format")
                throw std::invalid_argument("a load has no setting called '" + key + "'");
            if (value != "stl" && value != "stl-ascii")
                throw std::invalid_argument("a load's format is stl or stl-ascii, not '" + value + "'");
            format = value;
        }
        if (format.empty()) {
            const std::optional<std::string> path = fixture_path(m_entry.fixture);
            if (!path)
                throw std::invalid_argument(m_entry.fixture + " has no file to read, so the entry needs a format");
            if (!boost::filesystem::exists(*path))
                return "fixture not available: " + *path + " is missing";
            m_path = *path;
            return std::nullopt;
        }
        FixtureModel fixture = load_fixture(m_entry.fixture);
        if (!fixture.model)
            return "fixture not available: " + fixture.unavailable;
        m_path                  = (boost::filesystem::path(temporary_dir()) / boost::filesystem::unique_path("%%%%-%%%%-%%%%.stl")).string();
        const TriangleMesh mesh = fixture.model->mesh();
        if (!(format == "stl" ? mesh.write_binary(m_path.c_str()) : mesh.write_ascii(m_path.c_str())))
            throw std::runtime_error("cannot write " + m_path);
        return std::nullopt;
    }

    // The last pass's model goes here, so freeing it is never timed.
    void prepare(const RunContext&) override { free_model(); }

    void execute(const RunContext& context, Measurement& measurement) override
    {
        if (context.timed.count(Stage::Load) == 0)
            return;
        // A 3MF's settings, plates and embedded presets are read with its model and freed after the step.
        DynamicPrintConfig        config;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        ProjectParts              parts;
        const Clock::time_point   started_at = Clock::now();
        m_model = Model::read_from_file(m_path, &config, &substitutions,
                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances,
                                        &parts.plates, &parts.presets);
        const Clock::time_point done_at = Clock::now();

        double facets = 0., volumes = 0.;
        for (const ModelObject* object : m_model->objects)
            for (const ModelVolume* volume : object->volumes) {
                facets += double(volume->mesh().facets_count());
                ++volumes;
            }
        measurement.span("read_from_file", Scope::print(), started_at, done_at,
                         {{"facets", facets}, {"objects", double(m_model->objects.size())}, {"volumes", volumes}});
        measurement.output_hash(model_hash(*m_model));
    }

private:
    // Deletes the backup folder a 3MF read makes, so the model's destructor does not start libslic3r's backup
    // thread, which can deadlock the process's exit.
    void free_model()
    {
        if (m_model)
            m_model->remove_backup_path_if_exist();
        m_model.reset();
    }

    CatalogEntry         m_entry;
    std::string          m_path;
    std::optional<Model> m_model;
};

const WorkloadKindRegistrar registrar("load", [](const CatalogEntry& entry) -> std::unique_ptr<Workload> {
    return std::make_unique<LoadWorkload>(entry);
});

} // namespace

}} // namespace Slic3r::Bench
