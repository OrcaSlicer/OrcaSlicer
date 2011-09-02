#ifndef slic3r_EmbossJob_hpp_
#define slic3r_EmbossJob_hpp_

#include <atomic>
#include <memory>
#include <string>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/EmbossShape.hpp> // ExPolygonsWithIds
#include "libslic3r/Point.hpp" // Transform3d
#include "libslic3r/ObjectID.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/TextLines.hpp"

#include "Job.hpp"

// forward declarations
namespace Slic3r {
class TriangleMesh;
class ModelVolume;
enum class ModelVolumeType : int;
class BuildVolume;
namespace GUI {
class RaycastManager;
class Plater;
class GLCanvas3D;
class Worker;
class Selection;
}}

namespace Slic3r::GUI::Emboss {

/// <summary>
/// Base data hold data for create emboss shape
/// </summary>
class DataBase
{
public:
    DataBase(const std::string& volume_name, std::shared_ptr<std::atomic<bool>> cancel) 
        : volume_name(volume_name), cancel(std::move(cancel)) {}
    DataBase(const std::string& volume_name, std::shared_ptr<std::atomic<bool>> cancel, EmbossShape&& shape)
        : volume_name(volume_name), cancel(std::move(cancel)), shape(std::move(shape)){}
    DataBase(DataBase &&) = default;
    virtual ~DataBase() = default;

    /// <summary>
    /// Create shape
    /// e.g. Text extract glyphs from font
    /// Not 'const' function because it could modify shape
    /// </summary>
    virtual EmbossShape& create_shape() { return shape; };

    /// <summary>
    /// Write data how to reconstruct shape to volume
    /// </summary>
    /// <param name="volume">Data object for store emboss params</param>
    virtual void write(ModelVolume &volume) const;

    // Define projection move
    // True (raised) .. move outside from surface (MODEL_PART)    
    // False (engraved).. move into object (NEGATIVE_VOLUME)
    bool is_outside = true;

    // Define per letter projection on one text line
    // [optional] It is not used when empty
    Slic3r::Emboss::TextLines text_lines = {};

    // [optional] Define distance for surface
    // It is used only for flat surface (not cutted)
    // Position of Zero(not set value) differ for MODEL_PART and NEGATIVE_VOLUME
    std::optional<float> from_surface;
        
    // new volume name
    std::string volume_name;

    // flag that job is canceled
    // for time after process.
    std::shared_ptr<std::atomic<bool>> cancel;

    // shape to emboss
    EmbossShape shape;
};

/// <summary>
/// Hold neccessary data to create ModelVolume in job
/// Volume is created on the surface of existing volume in object.
/// NOTE: EmbossDataBase::font_file doesn't have to be valid !!!
/// </summary>
struct DataCreateVolume : public DataBase
{
    // define embossed volume type
    ModelVolumeType volume_type;

    // parent ModelObject index where to create volume
    ObjectID object_id;

    // new created volume transformation
    Transform3d trmat;
};
using DataBasePtr = std::unique_ptr<DataBase>;

/// <summary>
/// Hold neccessary data to update embossed text object in job
/// </summary>
struct DataUpdate
{
    // Hold data about shape
    DataBasePtr base;

    // unique identifier of volume to change
    ObjectID volume_id;

    // Used for prevent flooding Undo/Redo stack on slider.
    bool make_snapshot;
};

/// <summary>
/// Update text shape in existing text volume
/// Predict that there is only one runnig(not canceled) instance of it
/// </summary>
class UpdateJob : public Job
{
    DataUpdate   m_input;
    TriangleMesh m_result;

public:
    // move params to private variable
    explicit UpdateJob(DataUpdate &&input);

    /// <summary>
    /// Create new embossed volume by m_input data and store to m_result
    /// </summary>
    /// <param name="ctl">Control containing cancel flag</param>
    void process(Ctl &ctl) override;

    /// <summary>
    /// Update volume - change object_id
    /// </summary>
    /// <param name="canceled">Was process canceled.
    /// NOTE: Be carefull it doesn't care about
    /// time between finished process and started finalize part.</param>
    /// <param name="">unused</param>
    void finalize(bool canceled, std::exception_ptr &eptr) override;

    /// <summary>
    /// Update text volume
    /// </summary>
    /// <param name="volume">Volume to be updated</param>
    /// <param name="mesh">New Triangle mesh for volume</param>
    /// <param name="base">Data to write into volume</param>
    static void update_volume(ModelVolume *volume, TriangleMesh &&mesh, const DataBase &base);
};

struct SurfaceVolumeData
{
    // Transformation of volume inside of object
    Transform3d transform;

    struct ModelSource
    {
        // source volumes
        std::shared_ptr<const TriangleMesh> mesh;
        // Transformation of volume inside of object
        Transform3d tr;
    };
    using ModelSources = std::vector<ModelSource>;
    ModelSources sources;
};

/// <summary>
/// Hold neccessary data to update embossed text object in job
/// </summary>
struct UpdateSurfaceVolumeData : public DataUpdate, public SurfaceVolumeData{};

/// <summary>
/// Update text volume to use surface from object
/// </summary>
class UpdateSurfaceVolumeJob : public Job
{
    UpdateSurfaceVolumeData m_input;
    TriangleMesh            m_result;

public:
    // move params to private variable
    explicit UpdateSurfaceVolumeJob(UpdateSurfaceVolumeData &&input);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
};

/// <summary>
/// Copied triangles from object to be able create mesh for cut surface from
/// </summary>
/// <param name="volume">Define embossed volume</param>
/// <returns>Source data for cut surface from</returns>
SurfaceVolumeData::ModelSources create_volume_sources(const ModelVolume &volume);

/// <summary>
/// shorten params for start_crate_volume functions
/// </summary>
struct CreateVolumeParams
{
    GLCanvas3D &canvas;

    // Direction of ray into scene
    const Camera &camera;

    // To put new object on the build volume
    const BuildVolume &build_volume;

    // used to emplace job for execution
    Worker &worker;

    // New created volume type
    ModelVolumeType volume_type;

    // Contain AABB trees from scene
    RaycastManager &raycaster;

    // Define which gizmo open on the success
    unsigned char gizmo; // GLGizmosManager::EType

    // Volume define object to add new volume
    const GLVolume *gl_volume;

    // Wanted additionl move in Z(emboss) direction of new created volume
    std::optional<float> distance = {};

    // Wanted additionl rotation around Z of new created volume
    std::optional<float> angle = {};
};

/// <summary>
/// Create new volume on position of mouse cursor
/// </summary>
/// <param name="plater_ptr">canvas + camera + bed shape + </param>
/// <param name="data">Shape of emboss</param>
/// <param name="volume_type">New created volume type</param>
/// <param name="raycaster">Knows object in scene</param>
/// <param name="gizmo">Define which gizmo open on the success - enum GLGizmosManager::EType</param>
/// <param name="mouse_pos">Define position where to create volume</param>
/// <param name="distance">Wanted additionl move in Z(emboss) direction of new created volume</param>
/// <param name="angle">Wanted additionl rotation around Z of new created volume</param>
/// <returns>True on success otherwise False</returns>
bool start_create_volume(CreateVolumeParams &input, DataBasePtr data, const Vec2d &mouse_pos);

/// <summary>
/// Same as previous function but without mouse position
/// Need to suggest position or put near the selection
/// </summary>
bool start_create_volume_without_position(CreateVolumeParams &input, DataBasePtr data);

/// <summary>
/// Start job for update embossed volume
/// </summary>
/// <param name="data">define update data</param>
/// <param name="volume">Volume to be updated</param>
/// <param name="selection">Keep model and gl_volumes - when start use surface volume must be selected</param>
/// <param name="raycaster">Could cast ray to scene</param>
/// <returns>True when start job otherwise false</returns>
bool start_update_volume(DataUpdate &&data, const ModelVolume &volume, const Selection &selection, RaycastManager &raycaster);

} // namespace Slic3r::GUI

#endif // slic3r_EmbossJob_hpp_
