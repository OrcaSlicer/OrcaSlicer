#include "ProjectOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/FileArchiveDialog.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <miniz.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <wx/defs.h>
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

Refusal cut_keeps_instances() { return {RefusalKind::Precondition, "cut_object", L("A cut object keeps its number of instances.")}; }

} // namespace

std::optional<Refusal> unsaved_changes_refusal(UnsavedChanges unsaved)
{
    if (unsaved == UnsavedChanges::Discard || !the_plater().is_project_dirty())
        return std::nullopt;
    return Refusal{RefusalKind::Precondition, "unsaved_changes", L("The project has unsaved changes.")};
}

std::optional<std::vector<std::string>> zip_model_entries(const boost::filesystem::path& archive)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, archive.string()))
        return std::nullopt;
    std::vector<std::string> names;
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&zip); i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&zip, i, &stat))
            if (const boost::filesystem::path path(decode_archive_entry_path(&zip, stat)); is_archive_model(path))
                names.push_back(path.generic_string());
    }
    close_zip_reader(&zip);
    return names;
}

std::optional<Refusal> zip_entries_refusal(const std::vector<std::string>& in_archive, const std::vector<std::string>& asked)
{
    for (const std::string& name : asked)
        if (std::find(in_archive.begin(), in_archive.end(), name) == in_archive.end())
            // TRN %1% is a file's path inside a ZIP archive.
            return Refusal{RefusalKind::Precondition, "no_such_entry", L("The archive holds no model file %1%."), {name}, in_archive};
    return std::nullopt;
}

Outcome<ImportModelsResult> import_models(const ImportModelsArgs& args)
{
    Plater& plater = the_plater();
    // The files each archive gives, checked before anything is imported.
    std::vector<boost::filesystem::path> files;
    ImportModelsResult                   result;
    for (const boost::filesystem::path& path : args.paths) {
        if (!boost::algorithm::iends_with(path.string(), ".zip")) {
            files.push_back(path);
            continue;
        }
        const std::optional<std::vector<std::string>> in_archive = zip_model_entries(path);
        if (!in_archive)
            // TRN %1% is a file's path.
            return Refusal{RefusalKind::Failed, "unreadable_archive", L("%1% could not be read as a ZIP archive."),
                           {into_u8(from_path(path))}};
        if (args.entries)
            if (std::optional<Refusal> refusal = zip_entries_refusal(*in_archive, *args.entries))
                return *refusal;
        result.archives.push_back({path, args.entries ? *args.entries : *in_archive});
    }
    // The label File > Import gives its own undo step (Plater::add_file()).
    std::string label = "Import Objects";
    for (size_t i = 0; i < args.paths.size(); ++i)
        label += (i == 0 ? ": " : ", ") + encode_path(args.paths[i].filename().string().c_str());

    const size_t         before = plater.model().objects.size();
    Plater::TakeSnapshot snapshot(&plater, label);
    if (!files.empty())
        plater.load_files(files, LoadStrategy::LoadModel);
    for (const ImportedArchive& archive : result.archives)
        plater.import_zip_archive(archive.path, &archive.entries);
    if (plater.model().objects.size() == before)
        return Refusal{RefusalKind::Failed, "nothing_loaded", L("Nothing was imported.")};
    // Named as File > Import names it, after the first model file; an archive alone names nothing, as Import ZIP
    // Archive names nothing.
    if (!files.empty())
        plater.name_untitled_project(files.front());
    result.added = plater.model().objects.size() - before;
    return result;
}

Outcome<ProjectSummary> open_project(const OpenProjectArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = unsaved_changes_refusal(args.unsaved))
        return *refusal;
    // Dropped here, so Plater::load_project() finds nothing to ask about; the reset tells no plugin
    // and switches no tab, since the project that counts is the one about to load.
    if (plater.is_project_dirty())
        plater.new_project(Prompts::Skip, /*silent=*/true);
    // The project with its settings, as opening one is asked for: the load-behaviour preference, which can load the
    // geometry alone or ask how to open it, is a person's choice for the files they open themselves.
    plater.load_project(from_path(args.path), "<loadall>");
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
    plater.new_project(Prompts::Skip);
    return summary(plater);
}

Outcome<Done> save_project_as(const SaveProjectAsArgs& args)
{
    if (boost::algorithm::iends_with(args.path.string(), ".gcode.3mf"))
        return Refusal{RefusalKind::Precondition, "sliced_file_name",
                       L("A project is saved without G-code, so its file name cannot end in .gcode.3mf. Export the sliced file instead.")};
    if (the_plater().save_project(/*saveAs=*/true, from_path(args.path)) != wxID_YES)
        return project_not_saved();
    return Done{};
}

Refusal project_not_saved() { return Refusal{RefusalKind::Failed, "project_not_saved", L("The project was not saved.")}; }

std::optional<Refusal> add_instances_refusal(const Model& model, int object)
{
    const ModelObject& target = *model.objects[size_t(object)];
    if (target.is_cut())
        return cut_keeps_instances();
    if (std::any_of(target.instances.begin(), target.instances.end(), [](const ModelInstance* i) { return !i->printable; }))
        return Refusal{RefusalKind::Precondition, "unprintable_instance", L("Make every instance of the object printable first.")};
    return std::nullopt;
}

std::optional<Refusal> instance_count_refusal(const Model& model, int object, int count)
{
    const ModelObject& target = *model.objects[size_t(object)];
    const int          had    = int(target.instances.size());
    if (count < 1 && had == 1)
        // TRN %1% is an object's name.
        return Refusal{RefusalKind::Precondition, "last_instance", L("\"%1%\" has one instance, and an object keeps at least one."), {target.name}};
    if (count < 1 || count > 1000)
        return bad_instance_count();
    if (count > had)
        return add_instances_refusal(model, object);
    if (count < had && target.is_cut())
        return cut_keeps_instances();
    return std::nullopt;
}

Refusal bad_instance_count()
{
    return {RefusalKind::Precondition, "bad_instance_count", L("The number of instances is a whole number from 1 to 1000.")};
}

std::optional<Refusal> instance_change_blocked()
{
    Plater& plater = the_plater();
    if (!plater.get_ui_job_worker().is_idle())
        return job_running();
    if (plater.get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode())
        return Refusal{RefusalKind::Precondition, "gizmo_editing", L("Close the gizmo that is editing the object first.")};
    return std::nullopt;
}

Outcome<InstanceCount> set_instance_count(const SetInstanceCountArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = instance_count_refusal(plater.model(), args.object, args.count))
        return *refusal;
    if (std::optional<Refusal> refusal = instance_change_blocked())
        return *refusal;
    // The item reads the object selected in the Prepare view, an instance or a part of it as well.
    Selection& selection = plater.get_view3D_canvas3D()->get_selection();
    if (selection.get_object_idx() != args.object) {
        selection.add_object(unsigned(args.object), true);
        wxGetApp().obj_list()->update_selections();
    }
    plater.set_number_of_copies(args.count);
    return InstanceCount{args.object, plater.model().objects[size_t(args.object)]->instances.size()};
}

}}} // namespace Slic3r::GUI::Ops
