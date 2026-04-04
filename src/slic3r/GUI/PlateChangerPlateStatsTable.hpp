#ifndef slic3r_GUI_PlateChangerPlateStatsTable_hpp_
#define slic3r_GUI_PlateChangerPlateStatsTable_hpp_

#include <functional>
#include <vector>

class wxFlexGridSizer;
class wxWindow;

namespace Slic3r { namespace GUI {

class PartPlateList;

// Clears `grid_sizer` and fills a 4-column grid (Include | Plate | Time | Weight): header, separator,
// highlighted "All" row (totals over included plates only), then one row per plate with a checkbox.
// Unchecked plates use dimmed text; totals exclude them. `plate_included[i]` is true when plate i is in the job.
// `on_plate_included_changed` is invoked after a checkbox change (e.g. refresh AMS / repopulate).
// `plate_included` must have size == get_plate_count() (if not, it is resized to all true).
void populate_plate_changer_time_weight_grid(wxFlexGridSizer* grid_sizer,
                                             wxWindow* table_panel,
                                             wxWindow* icon_bitmap_parent,
                                             PartPlateList& partplate_list,
                                             std::vector<bool>& plate_included,
                                             const std::function<void()>& on_plate_included_changed = {});

}} // namespace Slic3r::GUI

#endif
