#pragma once

#include "Outcome.hpp"
#include "RunStatus.hpp"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
}

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
// at once; gcode_export_state() says how the export stands. An export that names a plate and is refused
// leaves the current plate as it was: what can be told on the plate is refused before it is selected, and
// a failure after that selects the plate that was current back. The exports below do the same.
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

// What keeps an export in `format` (gcode, sliced_3mf, 3mf, stl, drc or toolpaths_obj) of `objects` (indices in `model`;
// none: every object), or of a file per object with `per_object`, from being written, or none: objects named for a
// format that writes no objects of its own choosing (objects_not_taken), objects named, or a file per object, in a
// project with none (no_objects), and an index the project lacks (no_such_object).
std::optional<Refusal> export_objects_refusal(const std::string& format, const std::vector<int>& objects, bool per_object,
                                              const Model& model);

struct ExportStlArgs
{
    boost::filesystem::path path;               // the file, or with per_object an existing folder
    std::optional<int>      plate;              // made the current plate first; the files hold objects of every plate
    std::vector<int>        objects;            // their indices; none: every object
    bool                    per_object = false; // a file per object, named after it, into the folder
    bool                    draco      = false; // Draco (.drc) files in place of STL
};
// Writes objects as one STL or Draco file, as File > Export writes every object, or each into a file of its own, as
// Export All Objects as STLs does and the object menu's Export as STLs does for the objects selected
// (Plater::export_stl()). A file per object is named after it, with a number added where the folder has that name.
// Returns the files written.
// Refuses: no_objects, what export_objects_refusal() gives, no_such_plate, plate_not_selected, file_not_written.
Outcome<std::vector<boost::filesystem::path>> export_stl(const ExportStlArgs& args);

// What keeps the toolpaths from being written as an OBJ, or none: none loaded in the Preview (no_toolpaths), which
// shows a plate's toolpaths once the plate is sliced and the Preview has been shown.
std::optional<Refusal> toolpaths_refusal();

struct ExportToolpathsArgs
{
    boost::filesystem::path path;
};
// Writes the toolpaths the Preview shows as an OBJ, as Export Toolpaths as OBJ does.
// Refuses: no_toolpaths, file_not_written.
Outcome<Done> export_toolpaths_obj(const ExportToolpathsArgs& args);

}}} // namespace Slic3r::GUI::Ops
