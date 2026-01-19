#include "EmbossJob.hpp"

#include <stdexcept>
#include <type_traits>
#include <boost/log/trivial.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/Format/OBJ.hpp> // load_obj for default mesh
#include <libslic3r/CutSurface.hpp> // use surface cuts
#include <libslic3r/BuildVolume.hpp> // create object
#include <libslic3r/SLA/ReprojectPointsOnMesh.hpp>

#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoEmboss.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp" 
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/Utils/RaycastManager.hpp"

// #define EXECUTE_UPDATE_ON_MAIN_THREAD // debug execution on main thread

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

// Private implementation for create volume and objects jobs
namespace {
/// <summary>
/// Hold neccessary data to create ModelVolume in job
/// Volume is created on the surface of existing volume in object.
/// NOTE: EmbossDataBase::font_file doesn't have to be valid !!!
/// </summary>
struct DataCreateVolume
{
    // Hold data about shape
    DataBasePtr base;

    // define embossed volume type
    ModelVolumeType volume_type;

    // parent ModelObject index where to create volume
    ObjectID object_id;

    // new created volume transformation
    std::optional<Transform3d> trmat;

    // Define which gizmo open on the success
    GLGizmosManager::EType gizmo;
};

// Offset of clossed side to model
constexpr float SAFE_SURFACE_OFFSET = 0.015f; // [in mm]

/// <summary>
/// Create new TextVolume on the surface of ModelObject
/// Should not be stopped
/// NOTE: EmbossDataBase::font_file doesn't have to be valid !!!
/// </summary>
class CreateVolumeJob : public Job
{
    DataCreateVolume m_input;
    TriangleMesh     m_result;

public:
    explicit CreateVolumeJob(DataCreateVolume &&input);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
};

/// <summary>
/// Hold neccessary data to create ModelObject in job
/// Object is placed on bed under screen coor
/// OR to center of scene when it is out of bed shape
/// </summary>
struct DataCreateObject
{
    // Hold data about shape
    DataBasePtr base;

    // define position on screen where to create object
    Vec2d screen_coor;

    // projection property
    Camera camera;

    // shape of bed in case of create volume on bed
    std::vector<Vec2d> bed_shape;

    // Define which gizmo open on the success
    GLGizmosManager::EType gizmo;

    // additionl rotation around Z axe, given by style settings
    std::optional<float> angle = {};
};

/// <summary>
/// Create new TextObject on the platter
/// Should not be stopped
/// </summary>
class CreateObjectJob : public Job
{
    DataCreateObject m_input;
    TriangleMesh     m_result;
    Transform3d      m_transformation;

public:
    explicit CreateObjectJob(DataCreateObject &&input);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
};

/// <summary>
/// Hold neccessary data to create(cut) volume from surface object in job
/// </summary>
struct CreateSurfaceVolumeData : public SurfaceVolumeData
{
    // Hold data about shape
    DataBasePtr base;

    // define embossed volume type
    ModelVolumeType volume_type;

    // parent ModelObject index where to create volume
    ObjectID object_id;

    // Define which gizmo open on the success
    GLGizmosManager::EType gizmo;
};

/// <summary>
/// Cut surface from object and create cutted volume
/// Should not be stopped
/// </summary>
class CreateSurfaceVolumeJob : public Job
{
    CreateSurfaceVolumeData m_input;
    TriangleMesh            m_result;

public:
    explicit CreateSurfaceVolumeJob(CreateSurfaceVolumeData &&input);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
};

/// <summary>
/// Assert check of inputs data
/// </summary>
bool check(const DataBase &input, bool check_fontfile = true, bool use_surface = false);
bool check(GLGizmosManager::EType gizmo);
bool check(const CreateVolumeParams& input);
bool check(const DataCreateVolume &input, bool is_main_thread = false);
bool check(const DataCreateObject &input);
bool check(const DataUpdate &input, bool is_main_thread = false, bool use_surface = false);
bool check(const CreateSurfaceVolumeData &input, bool is_main_thread = false);
bool check(const UpdateSurfaceVolumeData &input, bool is_main_thread = false);

template<typename Fnc> static ExPolygons create_shape(DataBase &input, Fnc was_canceled);

// create sure that emboss object is bigger than source object [in mm]
constexpr float safe_extension = 1.0f;

// <summary>
/// Try to create mesh from text
/// </summary>
/// <param name="input">Text to convert on mesh
/// + Shape of characters + Property of font</param>
/// <param name="font">Font file with cache
/// NOTE: Cache glyphs is changed</param>
/// <param name="was_canceled">To check if process was canceled</param>
/// <returns>Triangle mesh model</returns>
template<typename Fnc> TriangleMesh try_create_mesh(DataBase &input, const Fnc& was_canceled);
template<typename Fnc> TriangleMesh create_mesh(DataBase &input, const Fnc& was_canceled, Job::Ctl &ctl);

/// <summary>
/// Create default mesh for embossed text
/// </summary>
/// <returns>Not empty model(index trinagle set - its)</returns>
TriangleMesh create_default_mesh();

/// <summary>
/// Must be called on main thread
/// </summary>
/// <param name="mesh">New mesh data</param>
/// <param name="data">Text configuration, ...</param>
/// <param name="mesh">Transformation of volume</param>
void update_volume(TriangleMesh &&mesh, const DataUpdate &data, const Transform3d *tr = nullptr);

/// <summary>
/// Update name in right panel
/// </summary>
/// <param name="obj_list">Right panel data</param>
/// <param name="volume">Volume with just changed name</param>
void update_name_in_list(const ObjectList &object_list, const ModelVolume &volume);

/// <summary>
/// Add new volume to object
/// </summary>
/// <param name="mesh">triangles of new volume</param>
/// <param name="object_id">Object where to add volume</param>
/// <param name="type">Type of new volume</param>
/// <param name="trmat">Transformation of volume inside of object</param>
/// <param name="data">Text configuration and New VolumeName</param>
/// <param name="gizmo">Gizmo to open</param>
void create_volume(TriangleMesh &&mesh, const ObjectID& object_id, const ModelVolumeType type, 
    const std::optional<Transform3d>& trmat, const DataBase &data, GLGizmosManager::EType gizmo);

/// <summary>
/// Create projection for cut surface from mesh
/// </summary>
/// <param name="tr">Volume transformation in object</param>
/// <param name="shape_scale">Convert shape to milimeters</param>
/// <param name="z_range">Bounding box 3d of model volume for projection ranges</param> 
/// <returns>Orthogonal cut_projection</returns>
OrthoProject create_projection_for_cut(Transform3d tr, double shape_scale, const std::pair<float, float> &z_range);

/// <summary>
/// Create tranformation for emboss Cutted surface
/// </summary>
/// <param name="is_outside">True .. raise, False .. engrave</param>
/// <param name="emboss">Depth of embossing</param>
/// <param name="tr">Text voliume transformation inside object</param>
/// <param name="cut">Cutted surface from model</param>
/// <returns>Projection</returns>
OrthoProject3d create_emboss_projection(bool is_outside, float emboss, Transform3d tr, SurfaceCut &cut);

/// <summary>
/// Cut surface into triangle mesh
/// </summary>
/// <param name="base">(can't be const - cache of font)</param>
/// <param name="input2">SurfaceVolume data</param>
/// <param name="was_canceled">Check to interupt execution</param>
/// <returns>Extruded object from cuted surace</returns>
template<typename Fnc>
TriangleMesh cut_surface(/*const*/ DataBase &input1, const SurfaceVolumeData &input2, const Fnc& was_canceled);

/// <summary>
/// Copied triangles from object to be able create mesh for cut surface from
/// </summary>
/// <param name="volumes">Source object volumes for cut surface from</param>
/// <param name="text_volume_id">Source volume id</param>
/// <returns>Source data for cut surface from</returns>
SurfaceVolumeData::ModelSources create_sources(const ModelVolumePtrs &volumes, std::optional<size_t> text_volume_id = {});

void create_message(const std::string &message); // only in finalize
bool process(std::exception_ptr &eptr);
bool finalize(bool canceled, std::exception_ptr &eptr, const DataBase &input);

class JobException : public std::runtime_error { 
public: using std::runtime_error::runtime_error;};
auto was_canceled(const Job::Ctl &ctl, const DataBase &base){
    return [&ctl, &cancel = base.cancel]() {
        if (cancel->load())
            return true;
        return ctl.was_canceled();
    };
}

} // namespace

void Slic3r::GUI::Emboss::DataBase::write(ModelVolume &volume) const{
    volume.name         = volume_name;
    volume.emboss_shape = shape;
    volume.emboss_shape->fix_3mf_tr.reset();
}

/////////////////
/// Create Volume
CreateVolumeJob::CreateVolumeJob(DataCreateVolume &&input): m_input(std::move(input)){ assert(check(m_input, true)); }

void CreateVolumeJob::process(Ctl &ctl) {
    if (!check(m_input)) 
        throw std::runtime_error("Bad input data for EmbossCreateVolumeJob.");
    m_result = create_mesh(*m_input.base, was_canceled(ctl, *m_input.base), ctl);
}
void CreateVolumeJob::finalize(bool canceled, std::exception_ptr &eptr) {
    if (!::finalize(canceled, eptr, *m_input.base))
        return;
    if (m_result.its.empty()) 
        return create_message("Can't create empty volume.");
    create_volume(std::move(m_result), m_input.object_id, m_input.volume_type, m_input.trmat, *m_input.base, m_input.gizmo);
}


/////////////////
/// Create Object
CreateObjectJob::CreateObjectJob(DataCreateObject &&input): m_input(std::move(input)){ assert(check(m_input)); }

void CreateObjectJob::process(Ctl &ctl) 
{
    if (!check(m_input))
        throw JobException("Bad input data for EmbossCreateObjectJob.");

    // can't create new object with using surface
    if (m_input.base->shape.projection.use_surface)
        m_input.base->shape.projection.use_surface = false;

    auto was_canceled = ::was_canceled(ctl, *m_input.base);
    m_result = create_mesh(*m_input.base, was_canceled, ctl);
    if (was_canceled()) return;

    // Create new object
    // calculate X,Y offset position for lay on platter in place of
    // mouse click
    Vec2d bed_coor = CameraUtils::get_z0_position(
        m_input.camera, m_input.screen_coor);

    // check point is on build plate:
    Points bed_shape_;
    bed_shape_.reserve(m_input.bed_shape.size());
    for (const Vec2d &p : m_input.bed_shape)
        bed_shape_.emplace_back(p.cast<coord_t>());
    Slic3r::Polygon bed(bed_shape_);
    if (!bed.contains(bed_coor.cast<coord_t>()))
        // mouse pose is out of build plate so create object in center of plate
        bed_coor = bed.centroid().cast<double>();

    double z = m_input.base->shape.projection.depth / 2;
    Vec3d  offset(bed_coor.x(), bed_coor.y(), z);
    offset -= m_result.center();
    Transform3d::TranslationType tt(offset.x(), offset.y(), offset.z());
    m_transformation = Transform3d(tt);

    // rotate around Z by style settings
    if (m_input.angle.has_value()) {
        std::optional<float> distance; // new object ignore surface distance from style settings
        apply_transformation(m_input.angle, distance, m_transformation);
    }
}

void CreateObjectJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!::finalize(canceled, eptr, *m_input.base))
        return;

    // only for sure
    if (m_result.empty()) 
        return create_message("Can't create empty object.");

    GUI_App &app    = wxGetApp();
    Plater  *plater = app.plater();
    plater->take_snapshot(_u8L("Add Emboss text object"));

    Model& model = plater->model();
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */
    {
        // INFO: inspiration for create object is from ObjectList::load_mesh_object()
        ModelObject *new_object = model.add_object();
        new_object->name = m_input.base->volume_name;
        new_object->add_instance(); // each object should have at list one instance
        new_object->config.set_key_value("extruder", new ConfigOptionInt(1));
        ModelVolume *new_volume = new_object->add_volume(std::move(m_result));
        // set a default extruder value, since user can't add it manually
        new_volume->config.set_key_value("extruder", new ConfigOptionInt(1));
        // write emboss data into volume
        m_input.base->write(*new_volume);

        // set transformation
        Slic3r::Geometry::Transformation tr(m_transformation);
        new_object->instances.front()->set_transformation(tr);
        new_object->ensure_on_bed();

        // Actualize right panel and set inside of selection
        app.obj_list()->paste_objects_into_list({model.objects.size() - 1});
    }
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    // When add new object selection is empty.
    // When cursor move and no one object is selected than
    // Manager::reset_all() So Gizmo could be closed before end of creation object
    GLCanvas3D      *canvas  = plater->get_view3D_canvas3D();
    GLGizmosManager &manager = canvas->get_gizmos_manager();
    if (manager.get_current_type() != m_input.gizmo)
        manager.open_gizmo(m_input.gizmo);

    // redraw scene
    canvas->reload_scene(true);
}

/////////////////
/// Update Volume
UpdateJob::UpdateJob(DataUpdate&& input): m_input(std::move(input)){ assert(check(m_input, true)); }

void UpdateJob::process(Ctl &ctl)
{
    if (!check(m_input))
        throw JobException("Bad input data for EmbossUpdateJob.");

    auto was_canceled = ::was_canceled(ctl, *m_input.base);
    m_result = ::try_create_mesh(*m_input.base, was_canceled);
    if (was_canceled()) return;
    if (m_result.its.empty())
        throw JobException("Created text volume is empty. Change text or font.");
}

void UpdateJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!::finalize(canceled, eptr, *m_input.base))
        return;
    ::update_volume(std::move(m_result), m_input);
}

void UpdateJob::update_volume(ModelVolume *volume, TriangleMesh &&mesh, const DataBase &base)
{
    // check inputs
    bool is_valid_input = volume != nullptr && !mesh.empty() && !base.volume_name.empty();
    assert(is_valid_input);
    if (!is_valid_input)
        return;

    // update volume
    volume->set_mesh(std::move(mesh));
    volume->set_new_unique_id();
    volume->calculate_convex_hull();

    GUI_App &app = wxGetApp(); // may be move to input

    if (volume->name != base.volume_name) {
        // write data from base into volume
        base.write(*volume);

        const ObjectList *obj_list = app.obj_list();
        if (obj_list != nullptr)
            update_name_in_list(*obj_list, *volume);
    } else {
        // write data from base into volume
        base.write(*volume);
    }

    ModelObject *object = volume->get_object();
    assert(object != nullptr);
    if (object == nullptr)
        return;

    Plater *plater = app.plater();
    if (plater->printer_technology() == ptSLA)
        sla::reproject_points_and_holes(object);
    plater->changed_object(*object);
}

/////////////////
/// Create Surface volume
CreateSurfaceVolumeJob::CreateSurfaceVolumeJob(CreateSurfaceVolumeData &&input) 
    : m_input(std::move(input))
{
    assert(check(m_input, true));
}

void CreateSurfaceVolumeJob::process(Ctl &ctl) {
    if (!check(m_input)) 
        throw JobException("Bad input data for CreateSurfaceVolumeJob.");
    m_result = cut_surface(*m_input.base, m_input, was_canceled(ctl, *m_input.base));
}

void CreateSurfaceVolumeJob::finalize(bool canceled, std::exception_ptr &eptr) {
    if (!::finalize(canceled, eptr, *m_input.base))
        return; 
    create_volume(std::move(m_result), m_input.object_id,
        m_input.volume_type, m_input.transform, *m_input.base, m_input.gizmo);
}

/////////////////
/// Cut Surface
UpdateSurfaceVolumeJob::UpdateSurfaceVolumeJob(UpdateSurfaceVolumeData &&input)
    : m_input(std::move(input))
{
    assert(check(m_input, true));
}

void UpdateSurfaceVolumeJob::process(Ctl &ctl)
{
    if (!check(m_input)) 
        throw JobException("Bad input data for UseSurfaceJob.");
    m_result = cut_surface(*m_input.base, m_input, was_canceled(ctl, *m_input.base));
}

void UpdateSurfaceVolumeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!::finalize(canceled, eptr, *m_input.base))
        return;

    // when start using surface it is wanted to move text origin on surface of model
    // also when repeteadly move above surface result position should match
    ::update_volume(std::move(m_result), m_input, &m_input.transform);
}

namespace {
/// <summary>
/// Check if volume type is possible use for new text volume
/// </summary>
/// <param name="volume_type">Type</param>
/// <returns>True when allowed otherwise false</returns>
bool is_valid(ModelVolumeType volume_type);

/// <summary>
/// Start job for add new volume to object with given transformation
/// </summary>
/// <param name="worker">Define where to queue the job. e.g. wxGetApp().plater()->get_ui_job_worker()</param>
/// <param name="object">Define where to add</param>
/// <param name="volume_tr">Wanted volume transformation, when not set will be calculated after creation to be near the object</param>
/// <param name="data">Define what to emboss - shape</param>
/// <param name="volume_type">Type of volume: Part, negative, modifier</param>
/// <param name="gizmo">Define which gizmo open on the success</param>
/// <returns>Nullptr when job is sucessfully add to worker otherwise return data to be processed different way</returns>
bool start_create_volume_job(Worker                           &worker,
                             const ModelObject                &object,
                             const std::optional<Transform3d> &volume_tr,
                             DataBasePtr                       data,
                             ModelVolumeType                   volume_type,
                             GLGizmosManager::EType            gizmo);

/// <summary>
/// Find volume in selected objects with closest convex hull to screen center.
/// </summary>
/// <param name="selection">Define where to search for closest</param>
/// <param name="screen_center">Canvas center(dependent on camera settings)</param>
/// <param name="objects">Actual objects</param>
/// <param name="closest_center">OUT: coordinate of controid of closest volume</param>
/// <returns>closest volume when exists otherwise nullptr</returns>
const GLVolume *find_closest(
    const Selection &selection, const Vec2d &screen_center, const Camera &camera, const ModelObjectPtrs &objects, Vec2d *closest_center);

/// <summary>
/// Start job for add object with text into scene
/// </summary>
/// <param name="input">Contain worker, build shape, gizmo</param>
/// <param name="emboss_data">Define params for create volume</param>
/// <param name="coor">Screen coordinat, where to create new object laying on bed</param>
/// <returns>True when can add job to worker otherwise FALSE</returns>
bool start_create_object_job(const CreateVolumeParams &input, DataBasePtr emboss_data, const Vec2d &coor);

/// <summary>
/// Start job to create volume on the surface of object
/// </summary>
/// <param name="input">Variabless needed to create volume</param>
/// <param name="data">Describe what to emboss - shape</param>
/// <param name="screen_coor">Where to add</param>
/// <param name="try_no_coor">True .. try to create volume without screen_coor,
/// False .. </param>
/// <returns>Nullptr when job is sucessfully add to worker otherwise return data to be processed different way</returns>
bool start_create_volume_on_surface_job(CreateVolumeParams &input, DataBasePtr data, const Vec2d &screen_coor, bool try_no_coor);

} // namespace

namespace Slic3r::GUI::Emboss {

SurfaceVolumeData::ModelSources create_volume_sources(const ModelVolume &text_volume)
{
    const ModelVolumePtrs &volumes = text_volume.get_object()->volumes;
    // no other volume in object
    if (volumes.size() <= 1)
        return {};
    return ::create_sources(volumes, text_volume.id().id);
}

bool start_create_volume(CreateVolumeParams &input, DataBasePtr data, const Vec2d &mouse_pos)
{
    if (data == nullptr)
        return false;
    if (!check(input))
        return false;

    if (input.gl_volume == nullptr)
        // object is not under mouse position soo create object on plater
        return ::start_create_object_job(input, std::move(data), mouse_pos);

    bool try_no_coor = true;
    return ::start_create_volume_on_surface_job(input, std::move(data), mouse_pos, try_no_coor);
}

bool start_create_volume_without_position(CreateVolumeParams &input, DataBasePtr data)
{
    assert(data != nullptr);
    if (data == nullptr)
        return false;
    if (!check(input))
        return false;

    // select position by camera position and view direction
    const Selection &selection = input.canvas.get_selection();
    int object_idx = selection.get_object_idx();

    Size s = input.canvas.get_canvas_size();
    Vec2d screen_center(s.get_width() / 2., s.get_height() / 2.);
    const ModelObjectPtrs &objects = selection.get_model()->objects;

    // No selected object so create new object
    if (selection.is_empty() || object_idx < 0 || 
        static_cast<size_t>(object_idx) >= objects.size()) 
        // create Object on center of screen
        // when ray throw center of screen not hit bed it create object on center of bed
        return ::start_create_object_job(input, std::move(data), screen_center);

    // create volume inside of selected object
    Vec2d coor;
    const Camera &camera = wxGetApp().plater()->get_camera();
    input.gl_volume = ::find_closest(selection, screen_center, camera, objects, &coor);
    if (input.gl_volume == nullptr)
        return ::start_create_object_job(input, std::move(data), screen_center);
    
    bool try_no_coor = false;
    return ::start_create_volume_on_surface_job(input, std::move(data), coor, try_no_coor);
}

#ifdef EXECUTE_UPDATE_ON_MAIN_THREAD
namespace {
// Run Job on main thread (blocking) - ONLY DEBUG
static inline bool execute_job(std::shared_ptr<Job> j)
{
    struct MyCtl : public Job::Ctl
    {
        void              update_status(int st, const std::string &msg = "") override{};
        bool              was_canceled() const override { return false; }
        std::future<void> call_on_main_thread(std::function<void()> fn) override { return std::future<void>{}; }
    } ctl;
    j->process(ctl);
    wxGetApp().plater()->CallAfter([j]() {
        std::exception_ptr e_ptr = nullptr;
        j->finalize(false, e_ptr);
    });
    return true;
}
} // namespace
#endif

bool start_update_volume(DataUpdate &&data, const ModelVolume &volume, const Selection &selection, RaycastManager& raycaster)
{
    assert(data.volume_id == volume.id());

    // check cutting from source mesh
    bool &use_surface = data.base->shape.projection.use_surface;
    if (use_surface && volume.is_the_only_one_part())
        use_surface = false;

    std::unique_ptr<Job> job = nullptr;
    if (use_surface) {
        // Model to cut surface from.
        SurfaceVolumeData::ModelSources sources = create_volume_sources(volume);
        if (sources.empty())
            return false;

        Transform3d volume_tr = volume.get_matrix();
        const std::optional<Transform3d> &fix_3mf = volume.emboss_shape->fix_3mf_tr;
        if (fix_3mf.has_value())
            volume_tr = volume_tr * fix_3mf->inverse();

        // when it is new applying of use surface than move origin onto surfaca
        if (!volume.emboss_shape->projection.use_surface) {
            auto offset = calc_surface_offset(selection, raycaster);
            if (offset.has_value())
                volume_tr *= Eigen::Translation<double, 3>(*offset);
        }

        UpdateSurfaceVolumeData surface_data{std::move(data), {volume_tr, std::move(sources)}};
        job = std::make_unique<UpdateSurfaceVolumeJob>(std::move(surface_data));
    } else {
        job = std::make_unique<UpdateJob>(std::move(data));
    }

#ifndef EXECUTE_UPDATE_ON_MAIN_THREAD
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    return queue_job(worker, std::move(job));
#else
    // Run Job on main thread (blocking) - ONLY DEBUG
    return execute_job(std::move(job));
#endif // EXECUTE_UPDATE_ON_MAIN_THREAD
}

} // namespace Slic3r::GUI::Emboss

////////////////////////////
/// private namespace implementation
namespace {
bool check(const DataBase &input, bool check_fontfile, bool use_surface)
{
    bool res = true;
    // if (check_fontfile) {
    //     assert(input.font_file.has_value());
    //     res &= input.font_file.has_value();
    // }
    // assert(!input.text_configuration.fix_3mf_tr.has_value());
    // res &= !input.text_configuration.fix_3mf_tr.has_value();
    // assert(!input.text_configuration.text.empty());
    // res &= !input.text_configuration.text.empty();
    assert(!input.volume_name.empty());
    res &= !input.volume_name.empty();
    //const FontProp& prop = input.text_configuration.style.prop;
    //assert(prop.per_glyph == !input.text_lines.empty());
    //res &= prop.per_glyph == !input.text_lines.empty();
    //if (prop.per_glyph) {
    //    assert(get_count_lines(input.text_configuration.text) == input.text_lines.size());
    //    res &= get_count_lines(input.text_configuration.text) == input.text_lines.size();
    //}
    return res; 
}

bool check(GLGizmosManager::EType gizmo)
{
    assert(gizmo == GLGizmosManager::Emboss || gizmo == GLGizmosManager::Svg);
    return gizmo == GLGizmosManager::Emboss || gizmo == GLGizmosManager::Svg;
}

bool check(const CreateVolumeParams &input)
{
    bool res        = is_valid(input.volume_type);
    auto gizmo_type = static_cast<GLGizmosManager::EType>(input.gizmo);
    res &= ::check(gizmo_type);
    return res;
}

bool check(const DataCreateVolume &input, bool is_main_thread)
{
    bool check_fontfile = false;
    assert(input.base != nullptr);
    bool res = input.base != nullptr;
    res &= check(*input.base, check_fontfile);
    res &= is_valid(input.volume_type);
    res &= check(input.gizmo);
    assert(!input.base->shape.projection.use_surface);
    res &= !input.base->shape.projection.use_surface;
    return res;
}
bool check(const DataCreateObject &input)
{
    bool check_fontfile = false;
    assert(input.base != nullptr);
    bool res = input.base != nullptr;
    res &= check(*input.base, check_fontfile);
    assert(input.screen_coor.x() >= 0.);
    res &= input.screen_coor.x() >= 0.;
    assert(input.screen_coor.y() >= 0.);
    res &= input.screen_coor.y() >= 0.;
    assert(input.bed_shape.size() >= 3); // at least triangle
    res &= input.bed_shape.size() >= 3;
    res &= check(input.gizmo);
    assert(!input.base->shape.projection.use_surface);
    res &= !input.base->shape.projection.use_surface;
    return res;
}
bool check(const DataUpdate &input, bool is_main_thread, bool use_surface)
{
    bool check_fontfile = true;
    assert(input.base != nullptr);
    bool res = input.base != nullptr;
    res &= check(*input.base, check_fontfile, use_surface);
    if (is_main_thread)
        assert(get_model_volume(input.volume_id, wxGetApp().model().objects) != nullptr);
    assert(input.base->cancel != nullptr);
    res &= input.base->cancel != nullptr;
    if (is_main_thread)
        assert(!input.base->cancel->load());
    assert(!input.base->shape.projection.use_surface);
    res &= !input.base->shape.projection.use_surface;
    return res;
}
bool check(const CreateSurfaceVolumeData &input, bool is_main_thread)
{
    bool use_surface = true;
    assert(input.base != nullptr);
    bool res = input.base != nullptr;
    res &= check(*input.base, is_main_thread, use_surface);
    assert(!input.sources.empty());
    res &= !input.sources.empty();
    res &= check(input.gizmo);
    assert(input.base->shape.projection.use_surface);
    res &= input.base->shape.projection.use_surface;
    return res;
}
bool check(const UpdateSurfaceVolumeData &input, bool is_main_thread)
{
    bool use_surface = true;
    assert(input.base != nullptr);
    bool res = input.base != nullptr;
    res &= check(*input.base, is_main_thread, use_surface);
    assert(!input.sources.empty());
    res &= !input.sources.empty();
    assert(input.base->shape.projection.use_surface);
    res &= input.base->shape.projection.use_surface;
    return res;
}

template<typename Fnc> 
ExPolygons create_shape(DataBase &input, Fnc was_canceled) {
    EmbossShape &es = input.create_shape();
    // TODO: improve to use real size of volume
    // ... need world matrix for volume
    // ... printer resolution will be fine too
    return union_with_delta(es, UNION_DELTA, UNION_MAX_ITERATIN);
}

//#define STORE_SAMPLING
#ifdef STORE_SAMPLING
#include "libslic3r/SVG.hpp"
#endif // STORE_SAMPLING

std::vector<BoundingBoxes> create_line_bounds(const ExPolygonsWithIds &shapes, size_t count_lines = 0)
{
    if (count_lines == 0)
        count_lines = get_count_lines(shapes);
    assert(count_lines == get_count_lines(shapes));

    std::vector<BoundingBoxes> result(count_lines);
    size_t text_line_index = 0;
    // s_i .. shape index
    for (const ExPolygonsWithId &shape_id: shapes) {
        const ExPolygons &shape = shape_id.expoly;
        BoundingBox       bb;
        if (!shape.empty()) {
            bb = get_extents(shape);
        }
        BoundingBoxes &line_bbs = result[text_line_index];
        line_bbs.push_back(bb);
        if (shape_id.id == ENTER_UNICODE) {
            // skip enters on beginig and tail
            ++text_line_index;
        }
    }
    return result;
}

template<typename Fnc> TriangleMesh create_mesh_per_glyph(DataBase &input, Fnc was_canceled)
{
    // method use square of coord stored into int64_t
    // static_assert(std::is_same<Point::coord_type, int32_t>());
    const EmbossShape &shape = input.create_shape();
    if (shape.shapes_with_ids.empty())
        return {};
    
    // Precalculate bounding boxes of glyphs
    // Separate lines of text to vector of Bounds
    assert(get_count_lines(shape.shapes_with_ids) == input.text_lines.size());
    size_t count_lines = input.text_lines.size();
    std::vector<BoundingBoxes> bbs = create_line_bounds(shape.shapes_with_ids, count_lines);
        
    double depth = shape.projection.depth / shape.scale;
    auto scale_tr = Eigen::Scaling(shape.scale); 
    
    // half of font em size for direction of letter emboss
    // double  em_2_mm      = prop.size_in_mm / 2.; // TODO: fix it
    double em_2_mm = 5.;
    coord_t em_2_polygon = static_cast<coord_t>(std::round(scale_(em_2_mm)));

    size_t s_i_offset = 0; // shape index offset(for next lines)
    indexed_triangle_set result;
    for (size_t text_line_index = 0; text_line_index < input.text_lines.size(); ++text_line_index) {
        const BoundingBoxes &line_bbs = bbs[text_line_index];
        const TextLine      &line     = input.text_lines[text_line_index];
        PolygonPoints        samples  = sample_slice(line, line_bbs, shape.scale);
        std::vector<double>  angles   = calculate_angles(em_2_polygon, samples, line.polygon);

        for (size_t i = 0; i < line_bbs.size(); ++i) {
            const BoundingBox &letter_bb = line_bbs[i];
            if (!letter_bb.defined)
                continue;

            Vec2d to_zero_vec = letter_bb.center().cast<double>() * shape.scale; // [in mm]
            float surface_offset = input.is_outside ? -SAFE_SURFACE_OFFSET : (-shape.projection.depth + SAFE_SURFACE_OFFSET);
            
            if (input.from_surface.has_value())
                surface_offset += *input.from_surface;

            Eigen::Translation<double, 3> to_zero(-to_zero_vec.x(), 0., static_cast<double>(surface_offset));

            const double &angle = angles[i];
            Eigen::AngleAxisd rotate(angle + M_PI_2, Vec3d::UnitY());

            const PolygonPoint &sample = samples[i];
            Vec2d offset_vec = unscale(sample.point); // [in mm]
            Eigen::Translation<double, 3> offset_tr(offset_vec.x(), 0., -offset_vec.y());
            Transform3d tr = offset_tr * rotate * to_zero * scale_tr;

            const ExPolygons &letter_shape = shape.shapes_with_ids[s_i_offset + i].expoly;
            assert(get_extents(letter_shape) == letter_bb);
            auto projectZ = std::make_unique<ProjectZ>(depth);
            ProjectTransform project(std::move(projectZ), tr);
            indexed_triangle_set glyph_its = polygons2model(letter_shape, project);
            its_merge(result, std::move(glyph_its));

            if (((s_i_offset + i) % 15) && was_canceled())
                return {};
        }
        s_i_offset += line_bbs.size();

#ifdef STORE_SAMPLING
        { // Debug store polygon
            //std::string stl_filepath = "C:/data/temp/line" + std::to_string(text_line_index) + "_model.stl";
            //bool suc = its_write_stl_ascii(stl_filepath.c_str(), "label", result);

            BoundingBox bbox      = get_extents(line.polygon);
            std::string file_path = "C:/data/temp/line" + std::to_string(text_line_index) + "_letter_position.svg";
            SVG         svg(file_path, bbox);
            svg.draw(line.polygon);
            int32_t radius = bbox.size().x() / 300; 
            for (size_t i = 0; i < samples.size(); i++) {
                const PolygonPoint &pp = samples[i];
                const Point& p = pp.point;
                svg.draw(p, "green", radius);
                std::string label = std::string(" ")+tc.text[i];
                svg.draw_text(p, label.c_str(), "black");

                double a = angles[i];
                double length = 3.0 * radius;
                Point  n(length * std::cos(a), length * std::sin(a));
                svg.draw(Slic3r::Line(p - n, p + n), "Lime");
            }
        }
#endif // STORE_SAMPLING
    }
    return TriangleMesh(std::move(result));
}

template<typename Fnc>
TriangleMesh try_create_mesh(DataBase &input, const Fnc& was_canceled)
{
    if (!input.text_lines.empty()) {
        TriangleMesh tm = create_mesh_per_glyph(input, was_canceled);
        if (was_canceled()) return {};
        if (!tm.empty()) return tm;
    }

    ExPolygons shapes = create_shape(input, was_canceled);
    if (shapes.empty()) return {};
    if (was_canceled()) return {};

    // NOTE: SHAPE_SCALE is applied in ProjectZ
    double scale = input.shape.scale;
    double depth = input.shape.projection.depth / scale;    
    auto projectZ = std::make_unique<ProjectZ>(depth);    
    float offset = input.is_outside ? -SAFE_SURFACE_OFFSET : (SAFE_SURFACE_OFFSET - input.shape.projection.depth);
    if (input.from_surface.has_value())
        offset += *input.from_surface;
    Transform3d tr = Eigen::Translation<double, 3>(0., 0.,static_cast<double>(offset)) * Eigen::Scaling(scale);
    ProjectTransform project(std::move(projectZ), tr);
    if (was_canceled()) return {};
    return TriangleMesh(polygons2model(shapes, project));
}

template<typename Fnc>
TriangleMesh create_mesh(DataBase &input, const Fnc& was_canceled, Job::Ctl& ctl)
{
    // It is neccessary to create some shape
    // Emboss text window is opened by creation new emboss text object
    TriangleMesh result = try_create_mesh(input, was_canceled);
    if (was_canceled())
        return {};

    if (result.its.empty()) {
        result = create_default_mesh();
        if (was_canceled())
            return {};
        // only info
        ctl.call_on_main_thread([]() {
            create_message("It is used default volume for embossed text, try to change text or font to fix it.");
        });
    }

    assert(!result.its.empty());
    return result;
}

TriangleMesh create_default_mesh()
{
    // When cant load any font use default object loaded from file
    std::string  path = Slic3r::resources_dir() + "/data/embossed_text.obj";
    TriangleMesh triangle_mesh;
    std::string  message;
    ObjInfo                 obj_info;
    if (!load_obj(path.c_str(), &triangle_mesh, obj_info, message)) {
        // when can't load mesh use cube
        return TriangleMesh(its_make_cube(36., 4., 2.5));
    }
    return triangle_mesh;
}

void update_name_in_list(const ObjectList& object_list, const ModelVolume& volume)
{
    const ModelObjectPtrs *objects_ptr = object_list.objects();
    if (objects_ptr == nullptr)
        return;

    const ModelObjectPtrs &objects   = *objects_ptr;
    const ModelObject     *object    = volume.get_object();
    const ObjectID        &object_id = object->id();

    // search for index of object
    int object_index = -1;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i]->id() == object_id) {
            object_index = static_cast<int>(i);
            break;
        }

    const ModelVolumePtrs volumes   = object->volumes;
    const ObjectID       &volume_id = volume.id();

    // search for index of volume
    int volume_index = -1;
    for (size_t i = 0; i < volumes.size(); ++i)
        if (volumes[i]->id() == volume_id) {
            volume_index = static_cast<int>(i);
            break;
        }

    if (object_index < 0 || volume_index < 0)
        return;

    object_list.update_name_in_list(object_index, volume_index);
}

void update_volume(TriangleMesh &&mesh, const DataUpdate &data, const Transform3d *tr)
{
    // for sure that some object will be created
    if (mesh.its.empty())
        return create_message("Empty mesh can't be created.");

    Plater *plater = wxGetApp().plater();
    // Check gizmo is still open otherwise job should be canceled
    assert(plater->canvas3D()->get_gizmos_manager().get_current_type() == GLGizmosManager::Emboss ||
           plater->canvas3D()->get_gizmos_manager().get_current_type() == GLGizmosManager::Svg);

    if (data.make_snapshot) {
        // TRN: This is the title of the action appearing in undo/redo stack.
        // It is same for Text and SVG.
        std::string snap_name = _u8L("Emboss attribute change");
        Plater::TakeSnapshot snapshot(plater, snap_name, UndoRedo::SnapshotType::GizmoAction);
    }

    ModelVolume *volume = get_model_volume(data.volume_id, plater->model().objects);   

    // could appear when user delete edited volume
    if (volume == nullptr)
        return;

    if (tr) {
        volume->set_transformation(*tr);
    } else {
        // apply fix matrix made by store to .3mf
        const std::optional<EmbossShape> &emboss_shape = volume->emboss_shape;
        assert(emboss_shape.has_value());
        if (emboss_shape.has_value() && emboss_shape->fix_3mf_tr.has_value())
            volume->set_transformation(volume->get_matrix() * emboss_shape->fix_3mf_tr->inverse());
    }

    UpdateJob::update_volume(volume, std::move(mesh), *data.base);
}

void create_volume(TriangleMesh                    &&mesh,
                   const ObjectID                   &object_id,
                   const ModelVolumeType             type,
                   const std::optional<Transform3d> &trmat,
                   const DataBase                   &data,
                   GLGizmosManager::EType            gizmo)
{
    GUI_App         &app      = wxGetApp();
    Plater          *plater   = app.plater();
    ObjectList      *obj_list = app.obj_list();
    GLCanvas3D      *canvas   = plater->get_view3D_canvas3D();
    ModelObjectPtrs &objects  = plater->model().objects;

    ModelObject *obj        = nullptr;
    size_t       object_idx = 0;
    for (; object_idx < objects.size(); ++object_idx) {
        ModelObject *o = objects[object_idx];
        if (o->id() == object_id) {
            obj = o;
            break;
        }
    }

    // Parent object for text volume was propably removed.
    // Assumption: User know what he does, so text volume is no more needed.
    if (obj == nullptr) 
        return create_message("Bad object to create volume.");

    if (mesh.its.empty()) 
        return create_message("Can't create empty volume.");

    plater->take_snapshot(_u8L("Add Emboss text Volume"));

    BoundingBoxf3 instance_bb;
    if (!trmat.has_value()) {
        // used for align to instance
        size_t instance_index = 0; // must exist
        instance_bb           = obj->instance_bounding_box(instance_index);
    }

    // NOTE: be carefull add volume also center mesh !!!
    // So first add simple shape(convex hull is also calculated)
    ModelVolume *volume = obj->add_volume(make_cube(1., 1., 1.), type);

    // TODO: Refactor to create better way to not set cube at begining
    // Revert mesh centering by set mesh after add cube
    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();

    // set a default extruder value, since user can't add it manually
    volume->config.set_key_value("extruder", new ConfigOptionInt(0));

    // do not allow model reload from disk
    volume->source.is_from_builtin_objects = true;

    volume->name = data.volume_name; // copy

    if (trmat.has_value()) {
        volume->set_transformation(*trmat);
    } else {
        assert(!data.shape.projection.use_surface);
        // Create transformation for volume near from object(defined by glVolume)
        // Transformation is inspired add generic volumes in ObjectList::load_generic_subobject
        Vec3d volume_size = volume->mesh().bounding_box().size();
        // Translate the new modifier to be pickable: move to the left front corner of the instance's bounding box, lift to print bed.
        Vec3d offset_tr(0, // center of instance - Can't suggest width of text before it will be created
                        -instance_bb.size().y() / 2 - volume_size.y() / 2, // under
                        volume_size.z() / 2 - instance_bb.size().z() / 2); // lay on bed
        // use same instance as for calculation of instance_bounding_box
        Transform3d tr           = obj->instances.front()->get_transformation().get_matrix_no_offset().inverse();
        Transform3d volume_trmat = tr * Eigen::Translation3d(offset_tr);
        volume->set_transformation(volume_trmat);
    }

    data.write(*volume);

    // update printable state on canvas
    if (type == ModelVolumeType::MODEL_PART) {
        volume->get_object()->ensure_on_bed();
        canvas->update_instance_printable_state_for_object(object_idx);
    }

    // update volume name in object list
    // updata selection after new volume added
    // change name of volume in right panel
    // select only actual volume
    // when new volume is created change selection to this volume
    auto                add_to_selection = [volume](const ModelVolume *vol) { return vol == volume; };
    wxDataViewItemArray sel              = obj_list->reorder_volumes_and_get_selection(object_idx, add_to_selection);
    if (!sel.IsEmpty())
        obj_list->select_item(sel.front());

    obj_list->selection_changed();

    // Now is valid text volume selected open emboss gizmo
    GLGizmosManager &manager = canvas->get_gizmos_manager();
    if (manager.get_current_type() != gizmo)
        manager.open_gizmo(gizmo);

    // update model and redraw scene
    //canvas->reload_scene(true);
    plater->update();
}

OrthoProject create_projection_for_cut(Transform3d tr, double shape_scale, const std::pair<float, float> &z_range)
{
    double min_z = z_range.first - safe_extension;
    double max_z = z_range.second + safe_extension;
    assert(min_z < max_z);
    // range between min and max value
    double   projection_size           = max_z - min_z;
    Matrix3d transformation_for_vector = tr.linear();
    // Projection must be negative value.
    // System of text coordinate
    // X .. from left to right
    // Y .. from bottom to top
    // Z .. from text to eye
    Vec3d untransformed_direction(0., 0., projection_size);
    Vec3d project_direction = transformation_for_vector * untransformed_direction;

    // Projection is in direction from far plane
    tr.translate(Vec3d(0., 0., min_z));
    tr.scale(shape_scale);
    return OrthoProject(tr, project_direction);
}

OrthoProject3d create_emboss_projection(bool is_outside, float emboss, Transform3d tr, SurfaceCut &cut)
{
    float 
        front_move = (is_outside) ? emboss : SAFE_SURFACE_OFFSET,
        back_move  = -((is_outside) ? SAFE_SURFACE_OFFSET : emboss);    
    its_transform(cut, tr.pretranslate(Vec3d(0., 0., front_move)));    
    Vec3d from_front_to_back(0., 0., back_move - front_move);
    return OrthoProject3d(from_front_to_back);
}

indexed_triangle_set cut_surface_to_its(const ExPolygons &shapes, const Transform3d& tr,const SurfaceVolumeData::ModelSources &sources, DataBase& input, std::function<bool()> was_canceled) {
    assert(!sources.empty());
    BoundingBox bb = get_extents(shapes);
    double shape_scale = input.shape.scale;

    const SurfaceVolumeData::ModelSource *biggest = &sources.front();

    size_t biggest_count = 0;
    // convert index from (s)ources to (i)ndexed (t)riangle (s)ets
    std::vector<size_t> s_to_itss(sources.size(), std::numeric_limits<size_t>::max());
    std::vector<indexed_triangle_set>  itss;
    itss.reserve(sources.size());
    for (const SurfaceVolumeData::ModelSource &s : sources) {
        Transform3d mesh_tr_inv       = s.tr.inverse();
        Transform3d cut_projection_tr = mesh_tr_inv * tr;
        std::pair<float, float> z_range{0., 1.};
        OrthoProject    cut_projection = create_projection_for_cut(cut_projection_tr, shape_scale, z_range);
        // copy only part of source model
        indexed_triangle_set its = its_cut_AoI(s.mesh->its, bb, cut_projection);
        if (its.indices.empty())
            continue;
        if (biggest_count < its.vertices.size()) {
            biggest_count = its.vertices.size();
            biggest       = &s;
        }
        size_t source_index     = &s - &sources.front();
        size_t its_index        = itss.size();
        s_to_itss[source_index] = its_index;
        itss.emplace_back(std::move(its));
    }
    if (itss.empty())
        return {};

    Transform3d tr_inv = biggest->tr.inverse();
    Transform3d cut_projection_tr = tr_inv * tr;

    size_t itss_index = s_to_itss[biggest - &sources.front()];
    BoundingBoxf3 mesh_bb    = bounding_box(itss[itss_index]);
    for (const SurfaceVolumeData::ModelSource &s : sources) {
        itss_index = s_to_itss[&s - &sources.front()];
        if (itss_index == std::numeric_limits<size_t>::max())
            continue;
        if (&s == biggest)
            continue;

        Transform3d           tr  = s.tr * tr_inv;
        bool        fix_reflected = true;
        indexed_triangle_set &its = itss[itss_index];
        its_transform(its, tr, fix_reflected);
        BoundingBoxf3 its_bb = bounding_box(its);
        mesh_bb.merge(its_bb);
    }

    // tr_inv = transformation of mesh inverted
    Transform3d             emboss_tr  = cut_projection_tr.inverse();
    BoundingBoxf3           mesh_bb_tr = mesh_bb.transformed(emboss_tr);
    std::pair<float, float> z_range{mesh_bb_tr.min.z(), mesh_bb_tr.max.z()};
    OrthoProject cut_projection = create_projection_for_cut(cut_projection_tr, shape_scale, z_range);
    float projection_ratio = (-z_range.first + safe_extension) / 
                              (z_range.second - z_range.first + 2 * safe_extension);

    ExPolygons shapes_data; // is used only when text is reflected to reverse polygon points order
    const ExPolygons *shapes_ptr = &shapes;
    bool is_text_reflected = Slic3r::has_reflection(tr);
    if (is_text_reflected) {
        // revert order of points in expolygons
        // CW --> CCW
        shapes_data = shapes; // copy
        for (ExPolygon &shape : shapes_data) {
            shape.contour.reverse();
            for (Slic3r::Polygon &hole : shape.holes)
                hole.reverse();
        }
        shapes_ptr = &shapes_data;
    }

    // Use CGAL to cut surface from triangle mesh
    SurfaceCut cut = cut_surface(*shapes_ptr, itss, cut_projection, projection_ratio);

    if (is_text_reflected) {
        for (SurfaceCut::Contour &c : cut.contours)
            std::reverse(c.begin(), c.end());
        for (Vec3i32 &t : cut.indices)
            std::swap(t[0], t[1]);
    }

    if (cut.empty()) return {}; // There is no valid surface for text projection.
    if (was_canceled()) return {};

    // !! Projection needs to transform cut
    OrthoProject3d projection = create_emboss_projection(input.is_outside, input.shape.projection.depth, emboss_tr, cut);
    return cut2model(cut, projection);
}

TriangleMesh cut_per_glyph_surface(DataBase &input1, const SurfaceVolumeData &input2, std::function<bool()> was_canceled) 
{
    // Precalculate bounding boxes of glyphs
    // Separate lines of text to vector of Bounds
    const EmbossShape &es = input1.create_shape();
    if (was_canceled()) return {};
    if (es.shapes_with_ids.empty())
        throw JobException(_u8L("Font doesn't have any shape for given text.").c_str());

    assert(get_count_lines(es.shapes_with_ids) == input1.text_lines.size());
    size_t count_lines = input1.text_lines.size();
    std::vector<BoundingBoxes> bbs = create_line_bounds(es.shapes_with_ids, count_lines);
        
    // half of font em size for direction of letter emboss
    double  em_2_mm      = 5.; // TODO: fix it
    int32_t em_2_polygon = static_cast<int32_t>(std::round(scale_(em_2_mm)));

    size_t s_i_offset = 0; // shape index offset(for next lines)
    indexed_triangle_set result;
    for (size_t text_line_index = 0; text_line_index < input1.text_lines.size(); ++text_line_index) {
        const BoundingBoxes &line_bbs = bbs[text_line_index];
        const TextLine      &line     = input1.text_lines[text_line_index];
        PolygonPoints        samples  = sample_slice(line, line_bbs, es.scale);
        std::vector<double>  angles   = calculate_angles(em_2_polygon, samples, line.polygon);

        for (size_t i = 0; i < line_bbs.size(); ++i) {
            const BoundingBox &glyph_bb = line_bbs[i];
            if (!glyph_bb.defined)
                continue;

            const double &angle = angles[i];
            auto rotate = Eigen::AngleAxisd(angle + M_PI_2, Vec3d::UnitY());

            const PolygonPoint &sample = samples[i];
            Vec2d offset_vec = unscale(sample.point); // [in mm]
            auto offset_tr = Eigen::Translation<double, 3>(offset_vec.x(), 0., -offset_vec.y());

            ExPolygons glyph_shape = es.shapes_with_ids[s_i_offset + i].expoly;
            assert(get_extents(glyph_shape) == glyph_bb);

            Point offset(-glyph_bb.center().x(), 0);
            for (ExPolygon& s: glyph_shape)
                s.translate(offset);

            Transform3d modify = offset_tr * rotate;
            Transform3d tr = input2.transform * modify;
            indexed_triangle_set glyph_its = cut_surface_to_its(glyph_shape, tr, input2.sources, input1, was_canceled);
            // move letter in volume on the right position
            its_transform(glyph_its, modify);

            // Improve: union instead of merge
            its_merge(result, std::move(glyph_its));

            if (((s_i_offset + i) % 15) && was_canceled())
                return {};
        }
        s_i_offset += line_bbs.size();
    }

    if (was_canceled()) return {};    
    if (result.empty()) 
        throw JobException(_u8L("There is no valid surface for text projection.").c_str());
    return TriangleMesh(std::move(result));
}

// input can't be const - cache of font
template<typename Fnc>
TriangleMesh cut_surface(DataBase& input1, const SurfaceVolumeData& input2, const Fnc& was_canceled)
{
    if (!input1.text_lines.empty())
        return cut_per_glyph_surface(input1, input2, was_canceled);
    
    ExPolygons shapes = create_shape(input1, was_canceled);
    if (was_canceled()) return {};
    if (shapes.empty())
        throw JobException(_u8L("Font doesn't have any shape for given text.").c_str());

    indexed_triangle_set its = cut_surface_to_its(shapes, input2.transform, input2.sources, input1, was_canceled);
    if (was_canceled()) return {};    
    if (its.empty()) 
        throw JobException(_u8L("There is no valid surface for text projection.").c_str());    

    return TriangleMesh(std::move(its));
}

SurfaceVolumeData::ModelSources create_sources(const ModelVolumePtrs &volumes, std::optional<size_t> text_volume_id)
{
    SurfaceVolumeData::ModelSources result;
    result.reserve(volumes.size() - 1);
    for (const ModelVolume *v : volumes) {
        if (text_volume_id.has_value() && v->id().id == *text_volume_id)
            continue;
        // skip modifiers and negative volumes, ...
        if (!v->is_model_part())
            continue;
        const TriangleMesh &tm = v->mesh();
        if (tm.empty())
            continue;
        if (tm.its.empty())
            continue;
        result.push_back({v->get_mesh_shared_ptr(), v->get_matrix()});
    }
    return result;
}

bool process(std::exception_ptr &eptr)
{
    if (!eptr)
        return false;
    try {
        std::rethrow_exception(eptr);
    } catch (JobException &e) {
        create_message(e.what());
        eptr = nullptr;
    }
    return true;
}

bool finalize(bool canceled, std::exception_ptr &eptr, const DataBase &input)
{
    // doesn't care about exception when process was canceled by user
    if (canceled || input.cancel->load()) {
        eptr = nullptr;
        return false;
    }
    return !process(eptr);
}

bool is_valid(ModelVolumeType volume_type)
{
    assert(volume_type != ModelVolumeType::INVALID);
    assert(volume_type == ModelVolumeType::MODEL_PART || 
           volume_type == ModelVolumeType::NEGATIVE_VOLUME ||
           volume_type == ModelVolumeType::PARAMETER_MODIFIER);
    if (volume_type == ModelVolumeType::MODEL_PART || 
        volume_type == ModelVolumeType::NEGATIVE_VOLUME ||
        volume_type == ModelVolumeType::PARAMETER_MODIFIER)
        return true;

    BOOST_LOG_TRIVIAL(error) << "Can't create embossed volume with this type: " << (int) volume_type;
    return false;
}

bool start_create_volume_job(Worker                           &worker,
                             const ModelObject                &object,
                             const std::optional<Transform3d> &volume_tr,
                             DataBasePtr                       data,
                             ModelVolumeType                   volume_type,
                             GLGizmosManager::EType            gizmo)
{
    bool &use_surface = data->shape.projection.use_surface;
    std::unique_ptr<GUI::Job> job;
    if (use_surface) {
        // Model to cut surface from.
        SurfaceVolumeData::ModelSources sources = create_sources(object.volumes);
        if (sources.empty() || !volume_tr.has_value()) {
            use_surface = false;
        } else {
            SurfaceVolumeData sfvd{*volume_tr, std::move(sources)};
            CreateSurfaceVolumeData surface_data{std::move(sfvd), std::move(data), volume_type, object.id(), gizmo};
            job = std::make_unique<CreateSurfaceVolumeJob>(std::move(surface_data));
        }
    }
    if (!use_surface) {
        // create volume
        DataCreateVolume create_volume_data{std::move(data), volume_type, object.id(), volume_tr, gizmo};
        job = std::make_unique<CreateVolumeJob>(std::move(create_volume_data));
    }
    return queue_job(worker, std::move(job));
}

const GLVolume *find_closest(
    const Selection &selection, const Vec2d &screen_center, const Camera &camera, const ModelObjectPtrs &objects, Vec2d *closest_center)
{
    assert(closest_center != nullptr);
    const GLVolume               *closest = nullptr;
    const Selection::IndicesList &indices = selection.get_volume_idxs();
    assert(!indices.empty()); // no selected volume
    if (indices.empty())
        return closest;

    double center_sq_distance = std::numeric_limits<double>::max();
    for (unsigned int id : indices) {
        const GLVolume    *gl_volume = selection.get_volume(id);        
        if (const ModelVolume *volume = get_model_volume(*gl_volume, objects);
            volume == nullptr || !volume->is_model_part())
            continue;
        Slic3r::Polygon hull        = CameraUtils::create_hull2d(camera, *gl_volume);
        Vec2d           c           = hull.centroid().cast<double>();
        Vec2d           d           = c - screen_center;
        bool            is_bigger_x = std::fabs(d.x()) > std::fabs(d.y());
        if ((is_bigger_x && d.x() * d.x() > center_sq_distance) || 
           (!is_bigger_x && d.y() * d.y() > center_sq_distance))
            continue;

        double distance = d.squaredNorm();
        if (center_sq_distance < distance)
            continue;
        center_sq_distance = distance;

        *closest_center = c;
        closest         = gl_volume;
    }
    return closest;
}

bool start_create_object_job(const CreateVolumeParams &input, DataBasePtr emboss_data, const Vec2d &coor)
{
    const Pointfs   &bed_shape  = input.build_volume.printable_area();
    auto             gizmo_type = static_cast<GLGizmosManager::EType>(input.gizmo);
    DataCreateObject data{std::move(emboss_data), coor, input.camera, bed_shape, gizmo_type, input.angle};

    // Fix: adding text on print bed with style containing use_surface
    if (data.base->shape.projection.use_surface) 
        // Til the print bed is flat using surface for Object is useless
        data.base->shape.projection.use_surface = false;

    auto job = std::make_unique<CreateObjectJob>(std::move(data));
    return queue_job(input.worker, std::move(job));
}

bool start_create_volume_on_surface_job(CreateVolumeParams &input, DataBasePtr data, const Vec2d &screen_coor, bool try_no_coor)
{
    auto on_bad_state = [&input, try_no_coor](DataBasePtr data_, const ModelObject *object = nullptr) {
        if (try_no_coor) {
            // Can't create on coordinate try to create somewhere
            return start_create_volume_without_position(input, std::move(data_));
        } else {
            // In centroid of convex hull is not hit with object. e.g. torid
            // soo create transfomation on border of object

            // there is no point on surface so no use of surface will be applied
            if (data_->shape.projection.use_surface)
                data_->shape.projection.use_surface = false;

            if (object == nullptr)
                return false;

            auto gizmo_type = static_cast<GLGizmosManager::EType>(input.gizmo);
            return start_create_volume_job(input.worker, *object, {}, std::move(data_), input.volume_type, gizmo_type);
        }
    };

    assert(input.gl_volume != nullptr);
    if (input.gl_volume == nullptr)
        return on_bad_state(std::move(data));

    const Model *model = input.canvas.get_model();

    assert(model != nullptr);
    if (model == nullptr)
        return on_bad_state(std::move(data));

    const ModelObjectPtrs &objects = model->objects;
    const ModelVolume     *volume  = get_model_volume(*input.gl_volume, objects);
    assert(volume != nullptr);
    if (volume == nullptr)
        return on_bad_state(std::move(data));

    const ModelInstance *instance = get_model_instance(*input.gl_volume, objects);
    assert(instance != nullptr);
    if (instance == nullptr)
        return on_bad_state(std::move(data));

    const ModelObject *object = volume->get_object();
    assert(object != nullptr);
    if (object == nullptr)
        return on_bad_state(std::move(data));

    auto                   cond   = RaycastManager::AllowVolumes({volume->id().id});
    RaycastManager::Meshes meshes = create_meshes(input.canvas, cond);
    input.raycaster.actualize(*instance, &cond, &meshes);
    std::optional<RaycastManager::Hit> hit = ray_from_camera(input.raycaster, screen_coor, input.camera, &cond);

    // context menu for add text could be open only by right click on an
    // object. After right click, object is selected and object_idx is set
    // also hit must exist. But there is options to add text by object list
    if (!hit.has_value())
        // When model is broken. It could appear that hit miss the object.
        // So add part near by in simmilar manner as right panel do
        return on_bad_state(std::move(data), object);

    // Create result volume transformation
    Transform3d surface_trmat = create_transformation_onto_surface(hit->position, hit->normal, UP_LIMIT);
    apply_transformation(input.angle, input.distance, surface_trmat);
    Transform3d transform  = instance->get_matrix().inverse() * surface_trmat;
    auto        gizmo_type = static_cast<GLGizmosManager::EType>(input.gizmo);
    // Try to cast ray into scene and find object for add volume
    return start_create_volume_job(input.worker, *object, transform, std::move(data), input.volume_type, gizmo_type);
}

void create_message(const std::string &message) {
    show_error(nullptr, message.c_str());
}

} // namespace
