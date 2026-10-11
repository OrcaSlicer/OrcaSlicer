#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Workload.hpp"
#include "slicer/Fixtures.hpp"
#include "slicer/Output.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r { namespace Bench {

namespace {

// Each object step's name, in the enum's order, so a step upstream adds fails to compile until it has one.
// Nothing in the tree starts posSimplifyWall, so no stage reports it.
constexpr const char* object_steps[] = {"posSlice",
                                        "posPerimeters",
                                        "posEstimateCurledExtrusions",
                                        "posPrepareInfill",
                                        "posInfill",
                                        "posIroning",
                                        "posContouring",
                                        "posSupportMaterial",
                                        "posSimplifyPath",
                                        "posSimplifySupportPath",
                                        "posDetectOverhangsForLift",
                                        nullptr,
                                        "posSimplifyInfill"};
static_assert(std::size(object_steps) == posCount, "name every PrintObjectStep");

struct PrintStepOf
{
    const char*          name;
    std::optional<Stage> stage;
};

// Each print step's name and the stage that times it, in the enum's order. Nothing in the tree sets
// psConflictCheck, so no stage reports it.
constexpr PrintStepOf print_steps[] = {{"psWipeTower", Stage::Process},
                                       {"psSkirtBrim", Stage::Process},
                                       {"psGCodeExport", Stage::Export},
                                       {"psConflictCheck", std::nullopt}};
static_assert(std::size(print_steps) == psCount, "name every PrintStep");

bool timed(const RunContext& context, Stage stage) { return context.timed.count(stage) != 0; }

void report(Measurement& measurement, const PrintStateBase::StateWithTimeStamp& state, const char* step, const Scope& scope)
{
    switch (state.state) {
    case PrintStateBase::DONE: measurement.span(step, scope, state.started_at, state.done_at); break;
    case PrintStateBase::STARTED: measurement.unfinished(step, scope, state.started_at); break;
    case PrintStateBase::INVALID: measurement.not_run(step, scope); break;
    }
}

// Slices a fixture under the entry's config, timing process() and export_gcode() as the run asks.
class SliceWorkload : public Workload
{
public:
    explicit SliceWorkload(CatalogEntry entry) : m_entry(std::move(entry)) {}

    std::optional<std::string> setup(const RunContext&) override
    {
        FixtureModel fixture = load_fixture(m_entry.fixture);
        if (!fixture.model)
            return "fixture not available: " + fixture.unavailable;
        m_model  = std::move(*fixture.model);
        m_config = hermetic_config(m_entry.config);
        m_gcode  = (boost::filesystem::path(temporary_dir()) / boost::filesystem::unique_path("%%%%-%%%%-%%%%.gcode")).string();
        return std::nullopt;
    }

    void prepare(const RunContext& context) override
    {
        // A fresh Print, since invalidating its steps would leave the last pass's layers in place, and the
        // last one goes first, so two never hold their layers at once.
        m_print.reset();
        m_print = std::make_unique<Print>();
        m_print->apply(m_model, m_config);
        if (const StringObjectException error = m_print->validate(); !error.string.empty())
            throw std::runtime_error("validate(): " + error.string);
        m_print->set_status_silent();
        if (timed(context, Stage::Export) && !timed(context, Stage::Process))
            m_print->process();
        // The export writes straight to the dump directory when there is one, so keeping the G-code adds
        // nothing to the timed pass.
        m_output = m_gcode;
        if (!context.dump_dir.empty() && timed(context, Stage::Export)) {
            const boost::filesystem::path kept =
                boost::filesystem::path(context.dump_dir) / m_entry.name / (std::to_string(++m_dumped) + ".gcode");
            boost::filesystem::create_directories(kept.parent_path());
            m_output = kept.string();
        }
    }

    void execute(const RunContext& context, Measurement& measurement) override
    {
        if (timed(context, Stage::Process))
            m_print->process();
        GCodeProcessorResult result;
        if (timed(context, Stage::Export))
            m_print->export_gcode(m_output, &result);

        const Print& print = *m_print;
        for (int step = 0; step < psCount; ++step)
            if (print_steps[step].stage && timed(context, *print_steps[step].stage))
                report(measurement, print.step_state_with_timestamp(PrintStep(step)), print_steps[step].name, Scope::print());
        if (timed(context, Stage::Process))
            for (std::size_t object = 0; object < print.objects().size(); ++object)
                for (int step = 0; step < posCount; ++step)
                    if (object_steps[step])
                        report(measurement, print.objects()[object]->step_state_with_timestamp(PrintObjectStep(step)), object_steps[step],
                               Scope::object(object));

        if (timed(context, Stage::Export)) {
            const GCodeDigest digest = gcode_digest(m_output);
            measurement.output_hash(digest.hash);
            measurement.work(work_of(result, digest.bytes));
            if (m_output == m_gcode)
                boost::filesystem::remove(m_gcode);
        }
    }

private:
    CatalogEntry           m_entry;
    Model                  m_model;
    DynamicPrintConfig     m_config;
    std::string            m_gcode;
    // Where this pass exports, the run's temporary file or its file in the dump directory.
    std::string            m_output;
    // The passes whose G-code went to the dump directory, counted from the first warmup.
    unsigned               m_dumped = 0;
    std::unique_ptr<Print> m_print;
};

const WorkloadKindRegistrar registrar("slice", [](const CatalogEntry& entry) -> std::unique_ptr<Workload> {
    return std::make_unique<SliceWorkload>(entry);
});

} // namespace

}} // namespace Slic3r::Bench
