#include "ExportOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"
#include "RunStatus.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <cassert>
#include <optional>
#include <string>
#include <vector>

#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Refusal file_not_written() { return {RefusalKind::Failed, "file_not_written", L("The file was not written.")}; }

// The one success check of an export that writes its file before it returns.
Outcome<Done> written(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec))
        return file_not_written();
    return Done{};
}

// The refusal for a project with nothing in it to write.
Refusal no_objects() { return {RefusalKind::Precondition, "no_objects", L("The project has no objects.")}; }

// Makes the plate a caller named the current one, or says why it is not.
std::optional<Refusal> select_named_plate(const std::optional<int>& plate)
{
    if (!plate)
        return std::nullopt;
    const Outcome<Done> selected = select_plate({*plate});
    return selected ? std::nullopt : std::optional<Refusal>(selected.refusal());
}

// An export that selected the plate it named and then failed makes the plate that was current before current
// again, so that a failed export leaves the current plate as it was. Both selections stay in the undo history.
template<class T> T select_back(const std::optional<int>& named, int previous, T failure)
{
    if (named && *named != previous)
        wxGetApp().plater()->select_plate(previous);
    return failure;
}

Refusal export_already_running() { return {RefusalKind::Busy, "export_already_running", L("Another export job is running.")}; }
Refusal slicing_in_progress()
{
    return {RefusalKind::Busy, "slicing_in_progress", L("Slicing is in progress. Wait for it to finish and export again.")};
}
Refusal slice_has_errors()
{
    return {RefusalKind::Precondition, "slice_has_errors",
            L("The plate's slice ended with an error. Fix what it reports and slice again before exporting.")};
}
Refusal plate_not_sliced() { return {RefusalKind::Precondition, "plate_not_sliced", L("The plate is not sliced. Slice it first.")}; }
Refusal export_not_started() { return {RefusalKind::Failed, "export_not_started", L("The export did not start.")}; }
Refusal sliced_file_not_written()
{
    return {RefusalKind::Precondition, "sliced_file_not_written",
            L("The sliced file was not written. Slice first and check for slicing errors.")};
}

// What Plater::export_gcode_to() would refuse on `plate`, in its order, asked before the export selects the plate
// so that a refused export changes nothing.
std::optional<Refusal> gcode_export_refusal(Plater& plater, int plate)
{
    if (plater.is_export_gcode_scheduled())
        return export_already_running();
    if (plater.is_slicing_any())
        return slicing_in_progress();
    if (!plater.plate_validation_error(plate).empty())
        return slice_has_errors();
    if (!plater.get_partplate_list().get_plate(plate)->is_slice_result_valid())
        return plate_not_sliced();
    return std::nullopt;
}

} // namespace

Outcome<ExportTicket> start_gcode_export(const ExportGcodeArgs& args)
{
    assert(wxThread::IsMain());
    Plater&        plater = *wxGetApp().plater();
    PartPlateList& plates = plater.get_partplate_list();
    if (plater.model().objects.empty())
        return no_objects();
    const int previous = plates.get_curr_plate_index();
    if (args.plate) {
        if (*args.plate < 0 || *args.plate >= plates.get_plate_count())
            return no_such_plate(*args.plate, plates.get_plate_count());
        if (std::optional<Refusal> refusal = gcode_export_refusal(plater, *args.plate))
            return *refusal;
        // The owner exports the current plate.
        plater.select_plate(*args.plate);
    }

    ExportTicket ticket;
    ticket.seq_before = plater.export_seq();
    ticket.revision   = plater.project_revision();
    ticket.path       = args.path.string();
    std::optional<Refusal> refusal;
    switch (plater.export_gcode_to(args.path)) {
    case Plater::ExportRefusal::None: break;
    case Plater::ExportRefusal::NoObjects: refusal = no_objects(); break;
    case Plater::ExportRefusal::AlreadyExporting: refusal = export_already_running(); break;
    case Plater::ExportRefusal::Slicing: refusal = slicing_in_progress(); break;
    case Plater::ExportRefusal::SliceHasErrors: refusal = slice_has_errors(); break;
    case Plater::ExportRefusal::NotSliced: refusal = plate_not_sliced(); break;
    case Plater::ExportRefusal::NotStarted: refusal = export_not_started(); break;
    }
    ticket.seq_after = plater.export_seq();
    if (!refusal && ticket.seq_after == ticket.seq_before)
        refusal = export_not_started();
    if (refusal)
        return select_back(args.plate, previous, Outcome<ExportTicket>(*refusal));
    return ticket;
}

ExportStatus gcode_export_state(const ExportTicket& ticket, std::string* error)
{
    assert(wxThread::IsMain());
    const std::optional<Plater::ExportRun> run = wxGetApp().plater()->export_run(ticket.seq_after);
    std::optional<ExportRecord>            record;
    if (run)
        record = ExportRecord{run->seq, run->path, run->finished, run->ok, run->cancelled, run->error, run->revision_at_finish};
    if (error != nullptr)
        *error = run ? run->error : std::string();
    return export_status(ticket.seq_before, ticket.seq_after, record, ticket.revision);
}

Outcome<Done> export_sliced_3mf(const ExportSlicedArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (plater.model().objects.empty())
        return no_objects();
    const int previous = plater.get_partplate_list().get_curr_plate_index();
    // The named plate's slice error refuses before the plate is selected, as export_gcode_3mf() refuses it after.
    if (args.plate && *args.plate >= 0 && *args.plate < plater.get_partplate_list().get_plate_count() &&
        !plater.plate_validation_error(*args.plate).empty())
        return sliced_file_not_written();
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    if (!plater.export_gcode_3mf(args.all_plates, args.path))
        return select_back(args.plate, previous, Outcome<Done>(sliced_file_not_written()));
    const Outcome<Done> done = written(args.path);
    return done ? done : select_back(args.plate, previous, done);
}

Outcome<Done> export_model_3mf(const ExportFileArgs& args)
{
    assert(wxThread::IsMain());
    const int previous = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    wxGetApp().plater()->export_core_3mf(from_path(args.path));
    const Outcome<Done> done = written(args.path);
    return done ? done : select_back(args.plate, previous, done);
}

std::optional<Refusal> export_objects_refusal(const std::string& format, const std::vector<int>& objects, bool per_object,
                                              const Model& model)
{
    if (!objects.empty() && format != "stl" && format != "drc")
        // TRN %1% is a file format, as project_export names it: gcode, sliced_3mf, 3mf or toolpaths_obj.
        return Refusal{RefusalKind::Precondition, "objects_not_taken", L("Only an STL or a DRC export takes objects, not %1%."), {format}};
    if ((per_object || !objects.empty()) && model.objects.empty())
        return no_objects();
    for (int object : objects)
        if (object < 0 || object >= int(model.objects.size()))
            // TRN %1% is the object index asked for, %2% the index of the project's last object.
            return Refusal{RefusalKind::Precondition, "no_such_object", L("Object %1% does not exist (the project has objects 0..%2%)"),
                           {std::to_string(object), std::to_string(model.objects.size() - 1)}};
    return std::nullopt;
}

Outcome<std::vector<boost::filesystem::path>> export_stl(const ExportStlArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (plater.model().objects.empty())
        return no_objects();
    if (std::optional<Refusal> refusal = export_objects_refusal(args.draco ? "drc" : "stl", args.objects, args.per_object, plater.model()))
        return *refusal;
    const int previous = plater.get_partplate_list().get_curr_plate_index();
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    using Files = Outcome<std::vector<boost::filesystem::path>>;
    std::vector<boost::filesystem::path> files;
    for (const std::string& file :
         plater.export_stl(false, false, args.per_object, args.draco ? FT_DRC : FT_STL, from_path(args.path), args.objects)) {
        files.push_back(into_path(from_u8(file)));
        if (!written(files.back()))
            return select_back(args.plate, previous, Files(file_not_written()));
    }
    if (files.empty())
        return select_back(args.plate, previous, Files(file_not_written()));
    return files;
}

std::optional<Refusal> toolpaths_refusal()
{
    assert(wxThread::IsMain());
    const Plater& plater = *wxGetApp().plater();
    if (plater.printer_technology() != ptFFF || !plater.is_preview_loaded() || !plater.has_toolpaths_to_export())
        return Refusal{RefusalKind::Precondition, "no_toolpaths",
                       L("The Preview shows no toolpaths. Slice the plate and show it in the Preview first.")};
    return std::nullopt;
}

Outcome<Done> export_toolpaths_obj(const ExportToolpathsArgs& args)
{
    assert(wxThread::IsMain());
    if (std::optional<Refusal> refusal = toolpaths_refusal())
        return *refusal;
    // A layer range shown since the canvas last drew reaches the toolpaths only when it draws again: the file
    // holds what the Preview shows.
    wxGetApp().plater()->get_preview_canvas3D()->apply_gcode_sliders();
    wxGetApp().plater()->export_toolpaths_to_obj(from_path(args.path));
    return written(args.path);
}

}}} // namespace Slic3r::GUI::Ops
