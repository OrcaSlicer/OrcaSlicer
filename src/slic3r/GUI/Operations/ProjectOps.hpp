#pragma once

#include "Outcome.hpp"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
}

// The project as a whole: models imported into it, a project file opened, saved or started anew, and
// its objects' instance counts.
namespace Slic3r { namespace GUI { namespace Ops {

// What becomes of the open project's unsaved changes when a call replaces the project. No operation
// asks about them: a caller that wants the person asked calls the owner's own method.
enum class UnsavedChanges { Refuse, Discard };

// The refusal for a project with unsaved changes that the caller did not say to drop
// (unsaved_changes), or none when the call may go ahead.
std::optional<Refusal> unsaved_changes_refusal(UnsavedChanges unsaved);

// What a project holds once a call has replaced it.
struct ProjectSummary
{
    std::vector<std::string> object_names;
    int                      plates = 0;
};

// The model files of the ZIP archive at `archive` that Import ZIP Archive's file list offers, each by its path in the
// archive with / between folders ("parts/cube.stl"); none when the file cannot be read as a ZIP archive.
std::optional<std::vector<std::string>> zip_model_entries(const boost::filesystem::path& archive);
// What keeps the files `asked` names from being taken from an archive whose model files are `in_archive`, or none: a
// name the archive has no model file of (no_such_entry), with the names it has.
std::optional<Refusal> zip_entries_refusal(const std::vector<std::string>& in_archive, const std::vector<std::string>& asked);

struct ImportModelsArgs
{
    std::vector<boost::filesystem::path>    paths;
    std::optional<std::vector<std::string>> entries; // the files to take from the .zip among paths; none: each one's every model file
};
// A ZIP archive an import took files from.
struct ImportedArchive
{
    boost::filesystem::path  path;
    std::vector<std::string> entries; // the files taken, as zip_model_entries() names them
};
struct ImportModelsResult
{
    size_t                       added = 0; // objects the import added
    std::vector<ImportedArchive> archives;
};
// Adds the files' geometry to the open project (LoadStrategy::LoadModel: a project file's settings
// are not loaded), as one "Import Objects: <files>" undo step. A .zip archive is taken apart as Import
// ZIP Archive does (Plater::import_zip_archive()): the files `entries` names, or every model file it
// holds, in place of its file list. The owner's import can open a dialog of its own (a STEP file's
// mesh options). An untitled project is named after the first file that is not an archive, as File >
// Import names it.
// Refuses: unreadable_archive and what zip_entries_refusal() gives, before anything is imported;
// nothing_loaded. The undo step stays in the history then, as after an import from the File menu
// whose load fails.
Outcome<ImportModelsResult> import_models(const ImportModelsArgs& args);

struct OpenProjectArgs
{
    boost::filesystem::path path;
    UnsavedChanges          unsaved = UnsavedChanges::Refuse;
};
// Replaces the open project with the file, with the settings it was saved with: a project opens as File > Open
// Project with Load behavior set to load all, whatever the Preferences say. Unsaved changes of the open project are
// dropped without a question when the caller says so; what Plater::load_project() asks beyond that (modified
// presets) it still asks.
// Refuses: unsaved_changes.
Outcome<ProjectSummary> open_project(const OpenProjectArgs& args);

struct NewProjectArgs
{
    UnsavedChanges unsaved               = UnsavedChanges::Refuse;
    bool           discard_preset_changes = false; // drop the presets' unsaved changes first; else they carry over
};
// Starts a new, empty project without a question, as New Project does otherwise: the Prepare tab shows, and plugins
// get the NewProject event.
// Refuses: unsaved_changes.
Outcome<ProjectSummary> new_project(const NewProjectArgs& args);

struct SaveProjectAsArgs
{
    boost::filesystem::path path;
};
// Saves the project to the path, which becomes the project's file. A name ending in .gcode.3mf is a
// sliced file's, and a project file holds no G-code, so it is refused.
// Refuses: sliced_file_name, project_not_saved (project_not_saved()).
Outcome<Done> save_project_as(const SaveProjectAsArgs& args);
// The refusal of a save that wrote nothing (project_not_saved): a dialog it opened was cancelled, such as the
// warning about settings that name an unknown plugin, or the file could not be written. Whatever the file
// held before stays.
Refusal project_not_saved();

// What keeps instances from being added to object `object` of `model`, or none: the reasons of the object itself that
// disable Increase Instances and Set Number of Instances, each with its own message: a cut object (cut_object) and an
// instance that is not printable (unprintable_instance).
std::optional<Refusal> add_instances_refusal(const Model& model, int object);
// What keeps object `object` of `model` from having `count` instances, or none: no instance left for an object of one
// (last_instance), a count outside 1..1000 (bad_instance_count), what add_instances_refusal() gives for more instances
// than it has, and a cut object for fewer (cut_object). Fewer instances have no printable rule, as Decrease Instances
// has none.
std::optional<Refusal> instance_count_refusal(const Model& model, int object, int count);
// The refusal for a number of instances outside 1..1000 (bad_instance_count).
Refusal bad_instance_count();
// What keeps any object's number of instances from changing now, or none: a running job (job_running) or a gizmo
// editing an object (gizmo_editing), which disable the instance items of the object menu and the toolbar.
std::optional<Refusal> instance_change_blocked();

struct SetInstanceCountArgs
{
    int object = 0; // its index in the project
    int count  = 1;
};
struct InstanceCount
{
    int    object    = 0; // the object's index
    size_t instances = 0; // the instances it has now
};
// Sets how many instances the object has, as the object menu's item does once its dialog is answered, with the
// object selected in the Prepare view, as the item reads it.
// Refuses: what instance_count_refusal() and instance_change_blocked() give, before the selection changes.
Outcome<InstanceCount> set_instance_count(const SetInstanceCountArgs& args);

}}} // namespace Slic3r::GUI::Ops
