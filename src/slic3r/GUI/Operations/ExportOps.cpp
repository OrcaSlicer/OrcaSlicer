#include "ExportOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"
#include "RunStatus.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <cassert>
#include <optional>
#include <string>

#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

// The one success check of an export that writes its file before it returns.
Outcome<Done> written(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec))
        return Refusal{RefusalKind::Failed, "file_not_written", L("The file was not written.")};
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

} // namespace

Outcome<ExportTicket> start_gcode_export(const ExportGcodeArgs& args)
{
    assert(wxThread::IsMain());
    Plater&        plater = *wxGetApp().plater();
    PartPlateList& plates = plater.get_partplate_list();
    if (plater.model().objects.empty())
        return no_objects();
    if (args.plate) {
        if (*args.plate < 0 || *args.plate >= plates.get_plate_count())
            return no_such_plate(*args.plate, plates.get_plate_count());
        // The owner exports the current plate.
        plater.select_plate(*args.plate);
    }

    ExportTicket ticket;
    ticket.seq_before = plater.export_seq();
    ticket.revision   = plater.project_revision();
    ticket.path       = args.path.string();
    switch (plater.export_gcode_to(args.path)) {
    case Plater::ExportRefusal::None: break;
    case Plater::ExportRefusal::NoObjects: return no_objects();
    case Plater::ExportRefusal::AlreadyExporting:
        return Refusal{RefusalKind::Busy, "export_already_running", L("Another export job is running.")};
    case Plater::ExportRefusal::Slicing:
        return Refusal{RefusalKind::Busy, "slicing_in_progress", L("Slicing is in progress. Wait for it to finish and export again.")};
    case Plater::ExportRefusal::SliceHasErrors:
        return Refusal{RefusalKind::Precondition, "slice_has_errors",
                       L("The plate's slice ended with an error. Fix what it reports and slice again before exporting.")};
    case Plater::ExportRefusal::NotSliced:
        return Refusal{RefusalKind::Precondition, "plate_not_sliced", L("The plate is not sliced. Slice it first.")};
    case Plater::ExportRefusal::NotStarted: return Refusal{RefusalKind::Failed, "export_not_started", L("The export did not start.")};
    }
    ticket.seq_after = plater.export_seq();
    if (ticket.seq_after == ticket.seq_before)
        return Refusal{RefusalKind::Failed, "export_not_started", L("The export did not start.")};
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
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    if (!plater.export_gcode_3mf(args.all_plates, args.path))
        return Refusal{RefusalKind::Precondition, "sliced_file_not_written",
                       L("The sliced file was not written. Slice first and check for slicing errors.")};
    return written(args.path);
}

Outcome<Done> export_model_3mf(const ExportFileArgs& args)
{
    assert(wxThread::IsMain());
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    wxGetApp().plater()->export_core_3mf(from_path(args.path));
    return written(args.path);
}

Outcome<Done> export_stl(const ExportFileArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (plater.model().objects.empty())
        return no_objects();
    if (std::optional<Refusal> refusal = select_named_plate(args.plate))
        return *refusal;
    plater.export_stl(false, false, false, FT_STL, from_path(args.path));
    return written(args.path);
}

Outcome<Done> send_gcode_to_host(const SendGcodeToHostArgs&)
{
    assert(wxThread::IsMain());
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    if (bundle.use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents"))
        // TRN "Print plate" is the print button's action, which sends to such a printer through its own dialog.
        return Refusal{RefusalKind::Precondition, "not_a_print_host_printer",
                       L("This printer prints with Print plate, not through a print host.")};
    // The Print button sends only to a printer with a print host address.
    const auto* host = bundle.printers.get_edited_preset().config.option<ConfigOptionString>("print_host");
    if (host == nullptr || host->value.empty())
        return Refusal{RefusalKind::Precondition, "no_print_host",
                       L("This printer has no print host. Set one in the printer's connection settings.")};
    wxGetApp().plater()->send_gcode_legacy();
    return Done{};
}

}}} // namespace Slic3r::GUI::Ops
