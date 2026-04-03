#ifndef slic3r_GUI_PlateChangerPlateStatsTable_hpp_
#define slic3r_GUI_PlateChangerPlateStatsTable_hpp_

class wxFlexGridSizer;
class wxWindow;

namespace Slic3r { namespace GUI {

class PartPlateList;

// Clears `grid_sizer` and fills a 3-column grid (Plate | Time | Weight): header, separator lines,
// highlighted "All" row, then one row per plate. Uses the same layout for Send / Print / Export plate-changer UIs.
// `table_panel` parents cell widgets and supplies background colour and DPI scaling (FromDIP).
// `icon_bitmap_parent` is passed to create_scaled_bitmap for the Time/Weight header icons.
void populate_plate_changer_time_weight_grid(wxFlexGridSizer* grid_sizer,
                                             wxWindow*        table_panel,
                                             wxWindow*        icon_bitmap_parent,
                                             PartPlateList&   partplate_list);

}} // namespace Slic3r::GUI

#endif
