#pragma once

#include "Outcome.hpp"
#include "RunStatus.hpp"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <optional>
#include <string>

// Writing the project or its G-code to a file the caller names, without a file dialog.
namespace Slic3r { namespace GUI { namespace Ops {

struct ExportGcodeArgs
{
    boost::filesystem::path path;
    std::optional<int>      plate; // none: the current plate
};

// A started export, in Plater's own numbering (export_seq(), project_revision()).
struct ExportTicket
{
    uint64_t    seq_before = 0;
    uint64_t    seq_after  = 0;
    uint64_t    revision   = 0; // the project revision when it started
    std::string path;
};

// Selects the plate when one is given, then starts writing its G-code in the background and returns
// at once; gcode_export_state() says how the export stands.
// Refuses: no_objects, no_such_plate, export_already_running, slicing_in_progress, slice_has_errors,
//          plate_not_sliced, export_not_started.
Outcome<ExportTicket> start_gcode_export(const ExportGcodeArgs& args);

// export_status() over Plater's record of the ticket's export. `error` gets the export's own error
// text when it has one.
ExportStatus gcode_export_state(const ExportTicket& ticket, std::string* error = nullptr);

struct ExportSlicedArgs
{
    boost::filesystem::path path;
    bool                    all_plates = false; // else the current plate
    std::optional<int>      plate;              // made the current plate first
};
// Writes the sliced plate, or every plate, as a .gcode.3mf. The owner writes a plate that is not
// sliced too, as a file with no G-code.
// Refuses: no_objects, no_such_plate, plate_not_selected, sliced_file_not_written
//          (Plater::export_gcode_3mf() declined: a slice of the current plate that ended with an
//          error, or a configuration that fails validation), file_not_written.
Outcome<Done> export_sliced_3mf(const ExportSlicedArgs& args);

struct ExportFileArgs
{
    boost::filesystem::path path;
    std::optional<int>      plate; // made the current plate first; the file holds every plate's objects all the same
};
// Writes the project's model as a .3mf without slicing data. A project with no objects is written
// too: one that holds only a Design-tab recipe has none.
// Refuses: no_such_plate, plate_not_selected, file_not_written.
Outcome<Done> export_model_3mf(const ExportFileArgs& args);
// Writes all objects as one STL.
// Refuses: no_objects, no_such_plate, plate_not_selected, file_not_written.
Outcome<Done> export_stl(const ExportFileArgs& args);

struct SendGcodeToHostArgs
{};
// Opens the Send G-code dialog for the printer's print host, as the Print button does for such a
// printer; the upload follows what the person answers there.
// Refuses: not_a_print_host_printer, no_print_host.
Outcome<Done> send_gcode_to_host(const SendGcodeToHostArgs& args = {});

}}} // namespace Slic3r::GUI::Ops
