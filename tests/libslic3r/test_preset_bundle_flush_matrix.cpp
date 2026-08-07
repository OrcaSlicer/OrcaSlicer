#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

namespace {

// Put the bundle in a known multi-material state: the edited printer's nozzle count, the number
// of selected filaments, and the stored flush matrix/multiplier the scenario starts from.
// flush_volumes_vector is fixed at 140 per filament, so a matrix cell seeded from it (rather
// than preserved) is exactly 280 (= from-filament 140 + to-filament 140).
void setup_flush_state(PresetBundle &bundle, size_t nozzle_count, size_t filament_count,
                       std::vector<double> matrix, std::vector<double> multiplier)
{
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter", true)->values
        .assign(nozzle_count, 0.4);
    bundle.filament_presets.assign(filament_count,
        bundle.filament_presets.empty() ? bundle.filaments.default_preset().name : bundle.filament_presets.front());
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::move(matrix);
    bundle.project_config.option<ConfigOptionFloats>("flush_multiplier")->values     = std::move(multiplier);
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values
        .assign(2 * filament_count, 140.);
}

const std::vector<double> &flush_matrix(const PresetBundle &bundle)
{
    return bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
}

const std::vector<double> &flush_multiplier(const PresetBundle &bundle)
{
    return bundle.project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
}

} // namespace

// The matrix must hold one (filaments x filaments) block per nozzle. Growing the printer from
// one to two nozzles with the filament count unchanged has to add a block, preserving the
// existing nozzle's values and replicating them to the new nozzle (the same policy as
// Plater::update_flush_volume_matrix), so tuned flush volumes survive a printer switch.
TEST_CASE("Adding an extruder replicates the tuned flush matrix block to the new nozzle", "[Preset][FlushMatrix]")
{
    PresetBundle bundle;
    setup_flush_state(bundle, 2, 2, /*matrix=*/{ 0., 100., 200., 0. }, /*multiplier=*/{ 1. });

    bundle.update_multi_material_filament_presets();

    CHECK(flush_matrix(bundle) == std::vector<double>{ 0., 100., 200., 0.,     // nozzle 0: preserved
                                                       0., 100., 200., 0. });  // nozzle 1: replicated
    CHECK(flush_multiplier(bundle).size() == 2);
}

// When a filament and a nozzle are added at once, known filament pairs keep (and replicate)
// their tuned values while pairs involving the new filament are seeded from flush_volumes_vector.
TEST_CASE("Adding a filament and an extruder together seeds only the new filament's pairs", "[Preset][FlushMatrix]")
{
    PresetBundle bundle;
    setup_flush_state(bundle, 2, 3, /*matrix=*/{ 0., 100., 200., 0. }, /*multiplier=*/{ 1. });

    bundle.update_multi_material_filament_presets();

    const std::vector<double> block { 0.,   100., 280.,
                                      200., 0.,   280.,
                                      280., 280., 0. };
    std::vector<double> expected(block);
    expected.insert(expected.end(), block.begin(), block.end());
    CHECK(flush_matrix(bundle) == expected);
}

TEST_CASE("Removing an extruder keeps only the remaining nozzle's flush matrix block", "[Preset][FlushMatrix]")
{
    PresetBundle bundle;
    setup_flush_state(bundle, 1, 2, /*matrix=*/{ 0., 101., 201., 0.,     // nozzle 0
                                                 0., 303., 403., 0. },   // nozzle 1
                      /*multiplier=*/{ 1., 1. });

    bundle.update_multi_material_filament_presets();

    CHECK(flush_matrix(bundle) == std::vector<double>{ 0., 101., 201., 0. });
    CHECK(flush_multiplier(bundle).size() == 1);
}

// A stale flush_multiplier length must not be trusted as the block count: a single 2x2 block
// with a 2-entry multiplier describes one nozzle's worth of data, and "repairing" it with the
// multiplier's layout would scramble the stored values.
TEST_CASE("A flush matrix whose multiplier lies about the block count survives unscrambled", "[Preset][FlushMatrix]")
{
    PresetBundle bundle;
    setup_flush_state(bundle, 1, 2, /*matrix=*/{ 0., 100., 200., 0. }, /*multiplier=*/{ 1., 1. });

    bundle.update_multi_material_filament_presets();

    CHECK(flush_matrix(bundle) == std::vector<double>{ 0., 100., 200., 0. });
    CHECK(flush_multiplier(bundle).size() == 1);
}

TEST_CASE("Flush matrix dimensions come from the block count that squares up with the stored size", "[Preset][FlushMatrix]")
{
    // Two 3x3 blocks: only a block count of 2 partitions 18 into squares.
    CHECK(get_flush_volumes_matrix_dims(18, 2).nozzle_nums == 2);
    CHECK(get_flush_volumes_matrix_dims(18, 2).filament_nums == 3);

    // One 2x2 block with a nozzle count that outgrew it, the case a printer switch leaves behind.
    // Reading it as two blocks would halve the row stride and index past the block.
    CHECK(get_flush_volumes_matrix_dims(4, 2).nozzle_nums == 1);
    CHECK(get_flush_volumes_matrix_dims(4, 2).filament_nums == 2);

    // A count of zero says "unknown", not "zero blocks".
    CHECK(get_flush_volumes_matrix_dims(4, 0).nozzle_nums == 1);
    CHECK(get_flush_volumes_matrix_dims(4, 0).filament_nums == 2);

    // Nothing partitions 12 into equal squares: fall back to the whole option as one block, so a
    // caller slices everything it has rather than a wrongly-sized window into it.
    CHECK(get_flush_volumes_matrix_dims(12, 5).nozzle_nums == 1);
}

// Both flush_multiplier and nozzle_diameter claim to say how many blocks are stored and either can
// be stale, so callers pass both. The alternate rescues a wrong preferred hint, and where both fit
// the stored size the preferred one settles the ambiguity.
TEST_CASE("The alternate block count is used only when the preferred one does not fit", "[Preset][FlushMatrix]")
{
    // Preferred 3 does not divide 18 into squares, alternate 2 does.
    CHECK(get_flush_volumes_matrix_dims(18, 3, 2).nozzle_nums == 2);
    CHECK(get_flush_volumes_matrix_dims(18, 3, 2).filament_nums == 3);

    // 16 is four 2x2 blocks or one 4x4; the preferred hint decides.
    CHECK(get_flush_volumes_matrix_dims(16, 4, 1).nozzle_nums == 4);
    CHECK(get_flush_volumes_matrix_dims(16, 4, 1).filament_nums == 2);
    CHECK(get_flush_volumes_matrix_dims(16, 1, 4).nozzle_nums == 1);
    CHECK(get_flush_volumes_matrix_dims(16, 1, 4).filament_nums == 4);
}
