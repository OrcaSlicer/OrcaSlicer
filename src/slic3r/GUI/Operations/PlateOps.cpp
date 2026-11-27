#include "PlateOps.hpp"

#include "Outcome.hpp"

#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>
#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Plater& the_plater()
{
    assert(wxThread::IsMain());
    return *wxGetApp().plater();
}

Refusal plate_not_selected(int plate)
{
    return {RefusalKind::Failed, "plate_not_selected", L("Plate %1% could not be selected."), {std::to_string(plate)}};
}

Refusal no_layers() { return {RefusalKind::Precondition, "no_layers", L("The preview has no layers. Slice the plate first.")}; }

} // namespace

Refusal no_such_plate(int plate, int count)
{
    if (count == 1)
        // TRN %1% is the plate index asked for.
        return {RefusalKind::Precondition, "no_such_plate", L("Plate %1% does not exist (the project has plate 0 only)"), {std::to_string(plate)}};
    // TRN %1% is the plate index asked for, %2% the last plate's index.
    return {RefusalKind::Precondition, "no_such_plate", L("Plate %1% does not exist (the project has plates 0..%2%)"),
            {std::to_string(plate), std::to_string(count - 1)}};
}

std::optional<Refusal> plate_index_refusal(int plate, int count)
{
    if (plate < 0 || plate >= count)
        return no_such_plate(plate, count);
    return std::nullopt;
}

std::optional<Refusal> plates_refusal()
{
    Plater& plater = the_plater();
    if (plater.printer_technology() != ptFFF || plater.only_gcode_mode())
        return Refusal{RefusalKind::Precondition, "no_plates", L("Plates are a filament (FFF) feature.")};
    return std::nullopt;
}

Refusal job_running() { return {RefusalKind::Busy, "job_running", L("Wait for the running background job to finish.")}; }

void show_prepare_view()
{
    Plater& plater = the_plater();
    if (plater.is_view3D_shown())
        return;
    plater.select_view_3D("3D");
    if (MainFrame* frame = wxGetApp().mainframe)
        frame->select_tab(wxString(TAB_ID_PREPARE));
}

GLCanvas3D& shown_prepare_canvas()
{
    show_prepare_view();
    GLCanvas3D& canvas = *the_plater().get_view3D_canvas3D();
    if (canvas.is_reload_delayed())
        canvas.reload_scene(true);
    return canvas;
}

Outcome<Done> select_plate(const SelectPlateArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = plate_index_refusal(args.plate, plater.get_partplate_list().get_plate_count()))
        return *refusal;
    if (plater.select_plate(args.plate) != 0)
        return plate_not_selected(args.plate);
    return Done{};
}

Outcome<PlateLocked> set_plate_locked(const SetPlateLockedArgs& args)
{
    if (std::optional<Refusal> refusal = plates_refusal())
        return *refusal;
    Plater&        plater = the_plater();
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = args.plate.value_or(plates.get_curr_plate_index());
    if (std::optional<Refusal> refusal = plate_index_refusal(index, plates.get_plate_count()))
        return *refusal;
    const bool locked = args.locked.value_or(!plates.is_locked(index));
    if (plates.is_locked(index) == locked)
        return PlateLocked{index, locked, false};
    plater.set_plate_locked(index, locked);
    return PlateLocked{index, locked, true};
}

std::optional<Refusal> delete_plate_refusal(int plate, int count)
{
    if (std::optional<Refusal> refusal = plate_index_refusal(plate, count))
        return refusal;
    if (count <= 1)
        return Refusal{RefusalKind::Precondition, "last_plate", L("Cannot delete the only plate.")};
    return std::nullopt;
}

Outcome<int> delete_plate(const PlateArgs& args)
{
    if (std::optional<Refusal> refusal = plates_refusal())
        return *refusal;
    Plater&        plater = the_plater();
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = args.plate.value_or(plates.get_curr_plate_index());
    if (std::optional<Refusal> refusal = delete_plate_refusal(index, plates.get_plate_count()))
        return *refusal;
    plater.delete_plate(index);
    return index;
}

std::optional<Refusal> duplicate_plate_refusal(int plate, int count, bool slicing)
{
    if (std::optional<Refusal> refusal = plate_index_refusal(plate, count))
        return refusal;
    if (count >= int(PartPlateList::MAX_PLATES_COUNT))
        return Refusal{RefusalKind::Precondition, "too_many_plates", L("Cannot duplicate a plate (maximum reached).")};
    if (slicing)
        return Refusal{RefusalKind::Busy, "slicing_in_progress", L("Wait for the slicing to finish.")};
    return std::nullopt;
}

Outcome<PlateCopy> duplicate_plate(const PlateArgs& args)
{
    if (std::optional<Refusal> refusal = plates_refusal())
        return *refusal;
    Plater&        plater = the_plater();
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = args.plate.value_or(plates.get_curr_plate_index());
    if (std::optional<Refusal> refusal = duplicate_plate_refusal(index, plates.get_plate_count(), plater.is_background_process_slicing()))
        return *refusal;
    return PlateCopy{index, plater.duplicate_plate(index)};
}

Outcome<int> rename_plate(const RenamePlateArgs& args)
{
    if (std::optional<Refusal> refusal = plates_refusal())
        return *refusal;
    Plater&        plater = the_plater();
    PartPlateList& plates = plater.get_partplate_list();
    const int      index  = args.plate.value_or(plates.get_curr_plate_index());
    if (std::optional<Refusal> refusal = plate_index_refusal(index, plates.get_plate_count()))
        return *refusal;
    // The name the plate has already changes nothing: neither the current plate nor the undo history.
    if (plates.get_plate(index)->get_plate_name() == args.name)
        return index;
    // The name icon makes its plate the current one before its dialog names it.
    if (index != plates.get_curr_plate_index())
        if (const Outcome<Done> selected = select_plate({index}); !selected)
            return selected.refusal();
    plater.rename_plate(index, args.name);
    return index;
}

Refusal plate_not_renamed() { return {RefusalKind::Failed, "not_renamed", L("The plate was not renamed.")}; }

std::optional<Refusal> arrange_refusal(const ArrangeArgs& args)
{
    Plater& plater = the_plater();
    if (plater.model().objects.empty())
        return Refusal{RefusalKind::Precondition, "no_objects_to_arrange", L("There are no objects to arrange.")};
    const int count = plater.get_partplate_list().get_plate_count();
    if (args.plate && (*args.plate < 0 || *args.plate >= count))
        return no_such_plate(*args.plate, count);
    // Plater::arrange() starts nothing while another job runs.
    if (!plater.get_ui_job_worker().is_idle())
        return job_running();
    return std::nullopt;
}

Outcome<Done> start_arrange(const ArrangeArgs& args)
{
    show_prepare_view();
    if (std::optional<Refusal> refusal = arrange_refusal(args))
        return *refusal;
    Plater& plater = the_plater();
    if (args.plate && *args.plate != plater.get_partplate_list().get_curr_plate_index() && plater.select_plate(*args.plate) != 0)
        return plate_not_selected(*args.plate);
    plater.arrange(/*current_plate_only=*/args.plate.has_value());
    return Done{};
}

Outcome<std::vector<int>> slice_plates(const SliceArgs& args)
{
    PartPlateList&   list = the_plater().get_partplate_list();
    std::vector<int> plates;
    if (args.all_plates) {
        for (int i = 0; i < list.get_plate_count(); ++i)
            plates.push_back(i);
    } else if (args.plate) {
        if (*args.plate < 0 || *args.plate >= list.get_plate_count())
            return no_such_plate(*args.plate, list.get_plate_count());
        plates.push_back(*args.plate);
    } else {
        plates.push_back(list.get_curr_plate_index());
    }
    return plates;
}

Outcome<SliceTicket> start_slice(const SliceArgs& args)
{
    Outcome<std::vector<int>> plates = slice_plates(args);
    if (!plates)
        return plates.refusal();
    Plater&     plater = the_plater();
    SliceTicket ticket;
    ticket.plates = plates.value();
    if (!args.all_plates && ticket.plates.front() != plater.get_partplate_list().get_curr_plate_index())
        plater.select_plate(ticket.plates.front());
    ticket.run_seq_before = plater.slice_run_seq();
    ticket.revision       = plater.project_revision();
    plater.request_slice(args.all_plates);
    ticket.request_seq = plater.slice_request_seq();
    ticket.slicing     = plater.is_slicing_any();
    // A request that returned early (a gizmo in editing mode, say) left a run that was slicing
    // already to finish; its result is the one the request gets.
    if (plater.slice_run_seq() == ticket.run_seq_before)
        if (const std::optional<Plater::SliceRun> running = plater.slice_run(ticket.run_seq_before); running && !running->finished)
            ticket.adopted_run = running->seq;
    // The Preview tab shows the result, as after the Slice button.
    if (MainFrame* frame = wxGetApp().mainframe)
        frame->select_tab(wxString(TAB_ID_PREVIEW));
    return ticket;
}

LayerTicks layer_ticks(std::optional<int> first, int last, int count)
{
    const int  max = std::max(count - 1, 0);
    LayerTicks ticks;
    ticks.high = std::clamp(last - 1, 0, max);
    if (first) {
        ticks.low  = std::clamp(*first - 1, 0, max);
        ticks.high = std::max(ticks.high, *ticks.low);
    }
    return ticks;
}

Outcome<int> layer_count()
{
    GLCanvas3D* canvas = the_plater().get_preview_canvas3D();
    if (canvas == nullptr)
        return no_layers();
    GCodeViewer&                viewer = canvas->get_gcode_viewer();
    IMSlider*                   layers = viewer.get_layers_slider();
    const GCodeProcessorResult* result = the_plater().get_partplate_list().get_curr_plate()->get_slice_result();
    // The slider alone says nothing: its range starts at 0..100 before a print gives it layers, and the Preview
    // hides it without clearing it for a plate that has none. Only a viewer that holds the current plate's
    // toolpaths has the layers of that plate, the test app_state's preview makes too.
    if (layers == nullptr || layers->values_count() == 0 || result == nullptr || !viewer.has_data() ||
        viewer.last_result_id() != result->id)
        return no_layers();
    return layers->GetMaxValue() + 1;
}

Outcome<ShownLayers> show_layer_range(const ShowLayerRangeArgs& args)
{
    const Outcome<int> count = layer_count();
    if (!count)
        return count.refusal();
    GCodeViewer&     viewer = the_plater().get_preview_canvas3D()->get_gcode_viewer();
    IMSlider*        layers = viewer.get_layers_slider();
    const LayerTicks ticks  = layer_ticks(args.first, args.last, count.value());
    viewer.show_layer_range(ticks.low, ticks.high);

    ShownLayers shown;
    shown.first       = layers->GetLowerValue() + 1;
    shown.last        = layers->GetHigherValue() + 1;
    shown.z_first     = layers->GetLowerValueD();
    shown.z_last      = layers->GetHigherValueD();
    shown.asked_first = ticks.low ? *ticks.low + 1 : shown.first;
    shown.asked_last  = ticks.high + 1;
    shown.count       = count.value();
    return shown;
}

}}} // namespace Slic3r::GUI::Ops
