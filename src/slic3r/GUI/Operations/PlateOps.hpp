#pragma once

#include "Outcome.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Plates, what is arranged and sliced on them, and the sliced plate's layers in the Preview.
namespace Slic3r { namespace GUI { namespace Ops {

// The refusal for a plate index the project lacks (no_such_plate), `count` being how many it has.
Refusal no_such_plate(int plate, int count);

struct SelectPlateArgs
{
    int plate = 0;
};
// Makes the plate the current one.
// Refuses: no_such_plate, plate_not_selected.
Outcome<Done> select_plate(const SelectPlateArgs& args);

struct SetPlateLockedArgs
{
    std::optional<int> plate; // none: the current plate
    bool               locked = true;
};
struct PlateLocked
{
    int  plate   = 0;
    bool changed = false; // false: the plate was in that state already, and no undo step was taken
};
// Locks or unlocks the plate, as one undo step.
// Refuses: no_such_plate.
Outcome<PlateLocked> set_plate_locked(const SetPlateLockedArgs& args);

// The refusal while the UI job worker runs a job, such as an arrangement or an orientation (job_running).
Refusal job_running();

// Shows the Prepare view when another view is shown. start_arrange() does it first; a caller with
// work of its own in that view before the start calls it earlier.
void show_prepare_view();

struct ArrangeArgs
{
    std::optional<int> plate; // none: every unlocked plate, as the toolbar's Arrange; else that plate alone, as its Arrange icon
};
// What start_arrange() would refuse now (no_objects_to_arrange, no_such_plate, job_running), for a
// caller with work of its own between the check and the start.
std::optional<Refusal> arrange_refusal(const ArrangeArgs& args);
// Shows the Prepare view, selects the plate when one is given and starts the arrangement in the
// background, as one undo step; the UI job worker is busy until it ends. As the toolbar's Arrange
// does, it runs whatever gizmo is open.
// Refuses: no_objects_to_arrange, no_such_plate, job_running, plate_not_selected.
Outcome<Done> start_arrange(const ArrangeArgs& args);

struct SliceArgs
{
    bool               all_plates = false;
    std::optional<int> plate; // none: the current plate
};
// A slice request, in Plater's own numbering (slice_request_seq(), slice_run_seq(), project_revision()),
// from which a caller builds slice_status()'s state.
struct SliceTicket
{
    std::vector<int> plates;             // the plates asked for
    uint64_t         run_seq_before = 0; // the run sequence before the request
    uint64_t         request_seq    = 0; // the request sequence right after it
    uint64_t         revision       = 0; // the project revision when it was made
    uint64_t         adopted_run    = 0; // a run already slicing that the request left running, or 0
    bool             slicing        = false; // slicing in any form right after the request
};
// The plates a slice of `args` covers.
// Refuses: no_such_plate.
Outcome<std::vector<int>> slice_plates(const SliceArgs& args);
// Selects the plate when it is not the current one, requests the slice as the Slice button does and
// shows the Preview tab. Returns at once; slice_status() says how the slice stands.
// Refuses: no_such_plate.
Outcome<SliceTicket> start_slice(const SliceArgs& args);

// How many layers the Preview has to show, for a caller that turns an argument of its own into a
// layer. The slider keeps the last slice's layers until another slice replaces them, so a plate that
// is empty or not sliced can still have layers to show.
// Refuses: no_layers (the layer slider is absent or was given no layers, as before the first slice).
Outcome<int> layer_count();

struct ShowLayerRangeArgs
{
    // 1-based. None: the upper thumb alone is dragged to `last`. The bottom of the shown range stays
    // while `last` is above it; at or below it, or where the slider shows one layer at a time, the one
    // layer `last` is shown.
    std::optional<int> first;
    int                last = 1; // 1-based
};
// The slider ticks (0-based) a range of layers is shown by.
struct LayerTicks
{
    std::optional<int> low; // none: the lower thumb is not moved
    int                high = 0;
};
// The ticks for layers `first`..`last` (1-based) of a plate with `count` layers: each number is brought
// into the plate's layers, and a last below the first is raised to it, so a range given backwards
// shows its first layer alone.
LayerTicks layer_ticks(std::optional<int> first, int last, int count);

struct ShownLayers
{
    // The range the slider shows afterwards, 1-based, and the heights of its bottom and top layer.
    int    first   = 1;
    int    last    = 1;
    double z_first = 0.;
    double z_last  = 0.;
    // The range that was asked for, brought into the plate's layers by layer_ticks(); `asked_first` is
    // `first` when no first layer was given. It is the range shown, except where the slider shows one
    // layer at a time: it then shows the first layer asked for when one was given, else the last.
    int    asked_first = 1;
    int    asked_last  = 1;
    int    count       = 0; // layers the plate has
};
// Shows these layers of the sliced plate in the Preview, with every move of the top one. The numbers
// are brought into the plate's layers by layer_ticks().
// Refuses: no_layers.
Outcome<ShownLayers> show_layer_range(const ShowLayerRangeArgs& args);

}}} // namespace Slic3r::GUI::Ops
