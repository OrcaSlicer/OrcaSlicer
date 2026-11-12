#include "ProjectOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
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

ProjectSummary summary(Plater& plater)
{
    ProjectSummary out;
    for (const ModelObject* object : plater.model().objects)
        out.object_names.push_back(object->name);
    out.plates = plater.get_partplate_list().get_plate_count();
    return out;
}

} // namespace

std::optional<Refusal> unsaved_changes_refusal(UnsavedChanges unsaved)
{
    if (unsaved == UnsavedChanges::Discard || !the_plater().is_project_dirty())
        return std::nullopt;
    return Refusal{RefusalKind::Precondition, "unsaved_changes", L("The project has unsaved changes.")};
}

Outcome<ImportModelsResult> import_models(const ImportModelsArgs& args)
{
    Plater& plater = the_plater();
    // The label File > Import gives its own undo step (Plater::add_file()).
    std::string label = "Import Objects";
    for (size_t i = 0; i < args.paths.size(); ++i)
        label += (i == 0 ? ": " : ", ") + encode_path(args.paths[i].filename().string().c_str());

    const size_t           before = plater.model().objects.size();
    Plater::TakeSnapshot   snapshot(&plater, label);
    const std::vector<size_t> loaded = plater.load_files(args.paths, LoadStrategy::LoadModel);
    if (loaded.empty() && plater.model().objects.size() == before)
        return Refusal{RefusalKind::Failed, "nothing_loaded", L("Nothing was imported.")};
    return ImportModelsResult{plater.model().objects.size() - before};
}

Outcome<ProjectSummary> open_project(const OpenProjectArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = unsaved_changes_refusal(args.unsaved))
        return *refusal;
    // Dropped here, so Plater::load_project() finds nothing to ask about; the reset tells no plugin
    // and switches no tab, since the project that counts is the one about to load.
    if (plater.is_project_dirty())
        plater.new_project(/*skip_confirm=*/true, /*silent=*/true);
    plater.load_project(from_path(args.path));
    return summary(plater);
}

Outcome<ProjectSummary> new_project(const NewProjectArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = unsaved_changes_refusal(args.unsaved))
        return *refusal;
    // New Project's prompt drops them before the reset; only with changes to drop, since dropping
    // reloads the presets, which can ask about the printer's.
    if (args.discard_preset_changes && wxGetApp().has_current_preset_changes())
        wxGetApp().discard_current_preset_changes();
    plater.new_project(/*skip_confirm=*/true, /*silent=*/true, wxString(), /*reload_presets=*/true, /*tell_plugins=*/true);
    return summary(plater);
}

Outcome<Done> save_project_as(const SaveProjectAsArgs& args)
{
    if (boost::algorithm::iends_with(args.path.string(), ".gcode.3mf"))
        return Refusal{RefusalKind::Precondition, "sliced_file_name",
                       L("A project is saved without G-code, so its file name cannot end in .gcode.3mf. Export the sliced file instead.")};
    the_plater().save_project(/*saveAs=*/true, from_path(args.path));
    boost::system::error_code ec;
    if (!boost::filesystem::exists(args.path, ec))
        return Refusal{RefusalKind::Failed, "project_not_saved", L("The project was not saved.")};
    return Done{};
}

std::optional<Refusal> instance_count_refusal()
{
    Plater&   plater  = the_plater();
    const int obj_idx = plater.get_selected_object_idx();
    if (obj_idx < 0)
        return Refusal{RefusalKind::Precondition, "no_object_selected", L("Select an object first.")};
    const ModelObject* object = plater.model().objects[size_t(obj_idx)];
    if (const ObjectList* list = wxGetApp().obj_list(); list != nullptr && list->has_selected_cut_object())
        return Refusal{RefusalKind::Precondition, "cut_object", L("A cut object keeps its number of instances.")};
    if (std::any_of(object->instances.begin(), object->instances.end(), [](const ModelInstance* i) { return !i->printable; }))
        return Refusal{RefusalKind::Precondition, "unprintable_instance", L("Make every instance of the object printable first.")};
    if (!plater.can_increase_instances())
        return job_running();
    return std::nullopt;
}

Refusal bad_instance_count()
{
    return {RefusalKind::Precondition, "bad_instance_count", L("The number of instances is a whole number from 1 to 1000.")};
}

Outcome<InstanceCount> set_instance_count(const SetInstanceCountArgs& args)
{
    Plater& plater = the_plater();
    if (args.count < 1 || args.count > 1000)
        return bad_instance_count();
    if (std::optional<Refusal> refusal = instance_count_refusal())
        return *refusal;
    const int obj_idx = plater.get_selected_object_idx();
    plater.set_number_of_copies(args.count);
    return InstanceCount{obj_idx, plater.model().objects[size_t(obj_idx)]->instances.size()};
}

}}} // namespace Slic3r::GUI::Ops
