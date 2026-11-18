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

// Orca: numbers a plate's filaments to the printer's tools T0..T(n-1) while the project may hold
// more filaments, numbered differently. The whole filament index space is renumbered once, at the
// top of Print::apply, so every downstream consumer sees tool numbers by construction.
// Used mixed slots are numbered after the physical tools; the printer never sees a mix's number.
struct FilamentCompaction
{
    // 0-based project slot per tool number; -1 is a filler tool no filament takes (its config column
    // copies the first used slot's). Empty means no renumbering.
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
    // The assignment as the slice info stores it: a 1-based extruder per project filament. A filament
    // with no tool reports 1. Without a renumbering, filament i is extruder i within the tools.
    std::vector<int> project_filament_map(size_t filament_count, size_t namespace_size) const;
};

// Thrown by resolve_filament_tools when a plate's manual map pins two used filaments to one tool.
class DuplicateToolAssignment : public SlicingError
{
public:
    DuplicateToolAssignment(int tool, int slot_a, int slot_b);
    int tool;   // 0-based
    int slot_a; // 0-based project slots
    int slot_b;
};

// The tool of every used physical slot of one plate. `order`: the used 0-based slots, surest to
// print first. `manual`: a 0-based extruder per project slot, -1 = automatic; empty means every
// slot is automatic. `demoted`: slots the last slice showed never print; they are placed after
// every other slot and never keep their own tool. Returns slot_of_tool over tools
// 0..namespace_size-1, trailing free tools trimmed, then the slots that found no tool. A pinned
// slot takes its tool; a slot numbered within the tools keeps its own when free; the rest take
// the lowest free tool in order. A tool below the highest taken one that no used slot takes holds
// an unused project slot (its own number when possible), or -1 when none is left: a filler the
// printer is never told about.
std::vector<int> resolve_filament_tools(const std::vector<int>& order, const std::vector<int>& manual, const std::vector<int>& demoted,
                                        size_t namespace_size, size_t filament_count);

// slot_of_tool is resolve_filament_tools() over the plate's physical slots, surest tier first
// (parts, features, wipe tower, tool changes, support), pinned by the config's `filament_map` when
// its `filament_map_mode` is manual, then used mixed slots; empty when that is the identity.
// `demoted`: slots the last slice showed never print; they are numbered last.
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
