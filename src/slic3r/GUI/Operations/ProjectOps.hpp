#pragma once

#include "Outcome.hpp"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

struct ImportModelsArgs
{
    std::vector<boost::filesystem::path> paths;
};
struct ImportModelsResult
{
    size_t added = 0; // objects the import added
};
// Adds the files' geometry to the open project (LoadStrategy::LoadModel: a project file's settings
// are not loaded), as one "Import Objects: <files>" undo step. The owner's import can open a dialog
// of its own (a STEP file's mesh options).
// Refuses: nothing_loaded. The undo step stays in the history then, as after an import from the
// File menu whose load fails.
Outcome<ImportModelsResult> import_models(const ImportModelsArgs& args);

struct OpenProjectArgs
{
    boost::filesystem::path path;
    UnsavedChanges          unsaved = UnsavedChanges::Refuse;
};
// Replaces the open project with the file. Unsaved changes of the open project are dropped without a
// question when the caller says so; what Plater::load_project() asks beyond that (modified presets,
// how to open the 3mf) it still asks.
// Refuses: unsaved_changes.
Outcome<ProjectSummary> open_project(const OpenProjectArgs& args);

struct NewProjectArgs
{
    UnsavedChanges unsaved               = UnsavedChanges::Refuse;
    bool           discard_preset_changes = false; // drop the presets' unsaved changes first; else they carry over
};
// Starts a new, empty project without a question. Plugins get the NewProject event; the shown tab
// stays as it is.
// Refuses: unsaved_changes.
Outcome<ProjectSummary> new_project(const NewProjectArgs& args);

struct SaveProjectAsArgs
{
    boost::filesystem::path path;
};
// Saves the project to the path, which becomes the project's file. A name ending in .gcode.3mf is a
// sliced file's, and a project file holds no G-code, so it is refused.
// Refuses: sliced_file_name, project_not_saved.
Outcome<Done> save_project_as(const SaveProjectAsArgs& args);

// What keeps the number of instances of the object selected in the Prepare view from being set
// (no_object_selected, cut_object, unprintable_instance, job_running), or none: the reasons the
// object menu's item is disabled, each with its own message.
std::optional<Refusal> instance_count_refusal();
// The refusal for a number of instances outside 1..1000 (bad_instance_count).
Refusal bad_instance_count();

struct SetInstanceCountArgs
{
    int count = 1;
};
struct InstanceCount
{
    int    object    = 0; // the selected object's index
    size_t instances = 0; // the instances it has now
};
// Sets how many instances the object selected in the Prepare view has, as the object menu's item does
// once its dialog is answered.
// Refuses: bad_instance_count, and what instance_count_refusal() gives.
Outcome<InstanceCount> set_instance_count(const SetInstanceCountArgs& args);

}}} // namespace Slic3r::GUI::Ops
