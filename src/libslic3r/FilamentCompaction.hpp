#ifndef slic3r_FilamentCompaction_hpp_
#define slic3r_FilamentCompaction_hpp_

#include "Exception.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace Slic3r {

class Model;
class DynamicPrintConfig;
struct ModelFilaments;

// Orca: dense tool numbering for printers whose firmware only accepts T0..T(n-1) while the project
// holds more filaments. The whole filament index space is renumbered once, at the top of
// Print::apply, so every downstream consumer is dense by construction.
// Used mixed slots are numbered after the physical tools; the printer never sees a mix's number.
struct FilamentCompaction
{
    // 0-based project slot per dense tool number. Empty means no renumbering.
    std::vector<int> slot_of_tool;
    // The plate's physical filaments (mixed slots expanded), 0-based, ascending.
    std::vector<int> used_slots;
    // How many of used_slots are part filaments, which print for certain. Not a prefix of
    // slot_of_tool: a demoted slot moves to its end.
    size_t           certain_count = 0;

    bool is_identity() const;
    // The dense tool number that prints a project slot, or -1 when the plate doesn't use it.
    int  tool_of_slot(int slot_0based) const;
    // The 0-based project slot a g-code tool number prints. Data parsed from g-code is keyed by tool
    // and must come through here before indexing project-indexed vectors. Tools outside the
    // compaction are returned unchanged.
    int  project_slot_of_tool(int tool) const;
};

// slot_of_tool stays empty unless the plate's highest physical slot exceeds namespace_size; then
// physical slots are packed tier by tier (parts, features, wipe tower, tool changes, support), then
// used mixed slots. `demoted`: slots the last slice showed never print; they move to the end.
FilamentCompaction build_filament_compaction(const ModelFilaments& filaments, const std::vector<int>& demoted,
                                             const DynamicPrintConfig& config, size_t namespace_size);

// Renumber every filament reference in the model; references outside the compaction become 0
// (inherit). `source` is the model `model` was copied from (same objects, same order). Mutated
// configs and paintings copy their timestamps from it, otherwise each Print::apply would see them
// as changed and invalidate the finished slice. `model` and `source` may be the same object.
void apply_filament_compaction(Model& model, const Model& source, const FilamentCompaction& compaction);

// Gather per-filament config vectors to the used slots in dense order, and renumber scalar 1-based
// filament references and mixed-slot component lists.
void apply_filament_compaction(DynamicPrintConfig& config, const FilamentCompaction& compaction);

// Thrown by Print::process when the printed filaments fit the tools but a non-printing filament was
// numbered below one that prints. Applying the same inputs again renumbers so the reslice fits.
class FilamentRenumberRequired : public SlicingError
{
public:
    using SlicingError::SlicingError;
};

} // namespace Slic3r

#endif // slic3r_FilamentCompaction_hpp_
