#include "SLAImportJob.hpp"

#include "libslic3r/Format/SL1.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/filename.h>

namespace Slic3r { namespace GUI {

class SLAImportJob::priv {
public:
    Plater *plater;

    Sel sel = Sel::modelAndProfile;

    indexed_triangle_set mesh;
    DynamicPrintConfig   profile;
    wxString             path;
    Vec2i32                win = {2, 2};
    std::string          err;
    ConfigSubstitutions config_substitutions;

    const SLAImportJobView * import_dlg;

    priv(Plater *plt, const SLAImportJobView *view) : plater{plt}, import_dlg{view} {}
};

SLAImportJob::SLAImportJob(const SLAImportJobView *view)
    : p{std::make_unique<priv>(wxGetApp().plater(), view)}
{
    prepare();
}

SLAImportJob::~SLAImportJob() = default;

void SLAImportJob::process(Ctl &ctl)
{
    auto statustxt = _u8L("Importing SLA archive");
    ctl.update_status(0, statustxt);

    auto progr = [&ctl, &statustxt](int s) {
        if (s < 100)
            ctl.update_status(int(s), statustxt);
        return !ctl.was_canceled();
    };

    if (p->path.empty()) return;

    std::string path = p->path.ToUTF8().data();
    try {
        switch (p->sel) {
        case Sel::modelAndProfile:
        case Sel::modelOnly:
            p->config_substitutions = import_sla_archive(path, p->win, p->mesh, p->profile, progr);
            break;
        case Sel::profileOnly:
            p->config_substitutions = import_sla_archive(path, p->profile);
            break;
        }
    } catch (MissingProfileError &) {
        p->err = _L("The SLA archive doesn't contain any presets. "
                    "Please activate some SLA printer preset first before importing that SLA archive.").ToStdString();
    } catch (std::exception &ex) {
        p->err = ex.what();
    }

    ctl.update_status(100, ctl.was_canceled() ? _u8L("Importing canceled.") :
                                        _u8L("Importing done."));
}

void SLAImportJob::reset()
{
    p->sel     = Sel::modelAndProfile;
    p->mesh    = {};
    p->profile = p->plater->sla_print().full_print_config();
    p->win     = {2, 2};
    p->path.Clear();
}

void SLAImportJob::prepare()
{
    reset();

    auto path = p->import_dlg->get_path();
    auto nm = wxFileName(path);
    p->path = !nm.Exists(wxFILE_EXISTS_REGULAR) ? "" : nm.GetFullPath();
    p->sel  = p->import_dlg->get_selection();
    p->win  = p->import_dlg->get_marchsq_windowsize();
    p->config_substitutions.clear();
}

void SLAImportJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    if (!p->err.empty()) {
        show_error(p->plater, p->err);
        p->err = "";
        return;
    }

    std::string name = wxFileName(p->path).GetName().ToUTF8().data();

    if (p->profile.empty()) {
        p->plater->get_notification_manager()->push_notification(
        NotificationType::CustomNotification,
        NotificationManager::NotificationLevel::WarningNotificationLevel,
            _L("The imported SLA archive did not contain any presets. "
               "The current SLA presets were used as fallback.").ToStdString());
    }

    if (p->sel != Sel::modelOnly) {
        if (p->profile.empty())
            p->profile = p->plater->sla_print().full_print_config();

        const ModelObjectPtrs& objects = p->plater->model().objects;
        for (auto object : objects)
            if (object->volumes.size() > 1)
            {
                Slic3r::GUI::show_info(nullptr,
                                       _(L("You cannot load SLA project with a multi-part object on the bed")) + "\n\n" +
                                       _(L("Please check your object list before preset changing.")),
                                       _(L("Attention!")) );
                return;
            }

        DynamicPrintConfig config = {};
        config.apply(SLAFullPrintConfig::defaults());
        config += std::move(p->profile);

        wxGetApp().preset_bundle->load_config_model(name, std::move(config));
        wxGetApp().load_current_presets();
    }

    if (!p->mesh.empty()) {
        bool is_centered = false;
        p->plater->sidebar().obj_list()->load_mesh_object(TriangleMesh{std::move(p->mesh)},
                                                          name, is_centered);
    }

    if (! p->config_substitutions.empty())
        show_substitutions_info(p->config_substitutions, p->path.ToUTF8().data());

    reset();
}

}} // namespace Slic3r::GUI
