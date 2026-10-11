#pragma once

#include "core/Result.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace Bench {

// The plates and embedded presets Model::read_from_file() allocates for a 3MF, freed when this goes.
struct ProjectParts
{
    ProjectParts() = default;
    ~ProjectParts();

    ProjectParts(const ProjectParts&)            = delete;
    ProjectParts& operator=(const ProjectParts&) = delete;

    PlateDataPtrs        plates;
    std::vector<Preset*> presets;
};

// A fixture's model, centered on the bed hermetic_config() describes, or why it is not available.
struct FixtureModel
{
    std::optional<Model> model;
    // Empty when the model is there.
    std::string unavailable;
};

// The file "handy:<file>" names under resources_dir()'s handy_models, whether or not it is there, or
// nothing for "procedural:<shape>", which is built in code. Throws std::invalid_argument for an id that
// names neither.
std::optional<std::string> fixture_path(const std::string& id);

// Reads "handy:<file>", one of the models under resources_dir()'s handy_models, or builds
// "procedural:<shape>", and throws std::invalid_argument for an id that names neither.
FixtureModel load_fixture(const std::string& id);

// The full default print config on a bed every handy model fits on, with the layer change G-code validate()
// requires and object labels off, then the keys given, throwing for a key the slicer does not know or
// a value it cannot read.
DynamicPrintConfig hermetic_config(const Properties& keys);

}} // namespace Slic3r::Bench
