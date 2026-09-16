#ifndef slic3r_FilamentCompaction_hpp_
#define slic3r_FilamentCompaction_hpp_

#include <vector>

namespace Slic3r {

class Model;
class DynamicPrintConfig;

// Orca: dense logical tool numbering for printers whose firmware only accepts T0..T(n-1).
//
// A project may hold more filaments than the printer has tools (see
// device_resolves_filament_mapping), so a plate can legitimately print with project slots 3 and
// 6 while the machine has four heads. The Snapmaker U1 takes those slot numbers as-is -- its
// 32-entry extruder_map_table is indexed by them -- but the WonderMaker ZR Ultra S has no macro
// past T(tool_count-1), so slots 3 and 6 have to reach it as T0 and T1 with the mapping saying
// which box each of the two pulls from.
//
// Rather than translating tool numbers at every point that emits one (the toolchange command,
// M104's T parameter, every `[next_extruder]`-indexed template in the vendor profile,
// CoolingBuffer's and GCodeProcessor's parsers), the whole filament index space is renumbered
// once, at the top of Print::apply, before config normalization and variant expansion. Every
// consumer downstream indexes filaments by that same space, so all of them become dense by
// construction and none of them needs to know this exists.
//
// Gated on protocol_requires_dense_tool_numbering(): printers without a native protocol never
// build a compaction, so nothing below runs for them.
//
// Mixed-colour filaments are virtual slots: ToolOrdering resolves each to its component
// filaments and only those are ever commanded as tools. The dense space still has to hold the
// mix itself -- objects reference it and its per-filament config rows travel with it -- so a
// used mix is numbered AFTER the physical tools. T0..T(n-1) stay exactly the filaments the
// printer loads, and the printer never sees a mix's number.
struct FilamentCompaction
{
    // The 0-based project filament slot each dense tool number prints: the used physical slots
    // in ascending order, then any used mixed slots. Empty means "no renumbering" -- either the
    // printer doesn't ask for it or the plate already uses a dense prefix.
    std::vector<int> slot_of_tool;

    bool is_identity() const;
    // The dense tool number that prints a project slot, or -1 when the plate doesn't use it.
    int  tool_of_slot(int slot_0based) const;
};

// The 0-based PHYSICAL filament slots the printable objects of the model's current plate use,
// sorted and deduplicated, with every mixed slot replaced by its components. Mirrors
// PartPlate::get_extruders(true), which is what the mapping widget lists, so the slicer's tool
// order and the widget's row order cannot drift.
std::vector<int> used_filament_slots(const Model& model, const DynamicPrintConfig& config);

// is_identity() unless the used slots are something other than a dense prefix (0..n-1).
FilamentCompaction build_filament_compaction(const Model& model, const DynamicPrintConfig& config);

// Renumber every filament reference in the model: object / volume / layer-range configs,
// multi-material painting, and the plate's custom tool changes. References to slots the
// compaction doesn't cover are left alone -- they belong to features this plate doesn't print.
//
// `source` is the model `model` was copied from (same objects, same order -- Model's assign_copy
// preserves both). Every mutated config and painting annotation gets its timestamp MIRRORED from
// the source: the copy is a deterministic derivation, so change detection must key on the
// source's edit history. Without the mirror, each Print::apply rebuilds the copy with fresh
// timestamps, apply's timestamp-compared state (painting especially) reads as "changed", and the
// slice that just finished is silently invalidated -- an endless slice/discard loop in the GUI.
// Passing the same object as both arguments is allowed (in-place, used by tests); the mirror is
// then a no-op.
void apply_filament_compaction(Model& model, const Model& source, const FilamentCompaction& compaction);

// Gather every per-filament config vector down to the used slots, in dense order, and renumber
// the scalar 1-based filament references (support_filament, *_filament_id, wipe_tower_filament)
// and the component lists of the mixed slots.
void apply_filament_compaction(DynamicPrintConfig& config, const FilamentCompaction& compaction);

} // namespace Slic3r

#endif // slic3r_FilamentCompaction_hpp_
