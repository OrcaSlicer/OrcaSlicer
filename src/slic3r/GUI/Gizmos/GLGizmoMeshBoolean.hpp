#ifndef slic3r_GLGizmoMeshBoolean_hpp_
#define slic3r_GLGizmoMeshBoolean_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <wx/event.h>

namespace Slic3r {

class ModelObject;

namespace GUI {

class Selection;

// Part mode works on the parts of one object, object mode on several whole objects.
enum class MeshBooleanTarget {
    Part,
    Object,
};

enum class MeshBooleanOperation {
    Union,
    Difference,
    Intersection,
};

// Why the last operation did not change the model. Messages are translated where they are shown.
enum class MeshBooleanWarning {
    None,
    Failed,
    NoOverlap,
    InputsChanged,
};

// Booleans between the parts of one object, or between whole objects on one plate. The parts or
// objects are picked into lists in the 3D view, then the operation runs on the plater's job worker.
// The first entry of the first list is the source: the result replaces it and takes its settings.
class GLGizmoMeshBoolean : public GLGizmoBase
{
public:
    GLGizmoMeshBoolean(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoMeshBoolean() override;

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);
    bool on_mouse(const wxMouseEvent &mouse_event) override;

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    void on_render() override;
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void on_render_input_window(float x, float y, float bottom_limit) override;

    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;

private:
    struct JobState;

    // Volume ids (part mode) or object ids (object mode) in the order the user picked them.
    using PartList = std::vector<ObjectID>;

    static std::optional<MeshBooleanTarget> target_for(const Selection &selection);
    // Part mode: the object whose parts are listed.
    ModelObject *selected_object() const;
    std::string entry_name(const ObjectID &id) const;
    bool uses_two_lists() const { return m_operation_mode == MeshBooleanOperation::Difference; }
    void toggle_part(const ObjectID &id);
    void reset_lists();
    // Takes the mode and the object(s) from the selection; the lists are cleared when they change.
    void sync_with_selection();
    void update_highlights();
    void clear_highlights();

    bool can_execute() const;
    void execute();
    void on_job_finished(std::uint64_t token, bool canceled, const JobState &job);
    void apply_part_result(const JobState &job);
    void apply_object_result(const JobState &job);

    // Returns true when the user removed an entry.
    bool render_list(PartList &list, const char *imgui_id);

    MeshBooleanTarget     m_target{MeshBooleanTarget::Part};
    // Object mode: the selected objects, the only ones that can be picked.
    std::vector<ObjectID> m_objects;
    // Part mode: the object whose parts are listed.
    ObjectID              m_object_id;
    MeshBooleanOperation  m_operation_mode{MeshBooleanOperation::Union};
    PartList              m_list_a;
    PartList              m_list_b;
    // Difference only: whether clicks in the 3D view add to the second list.
    bool                  m_picking_b{false};
    bool                  m_diff_delete_input{false};
    bool                  m_inter_delete_input{false};
    MeshBooleanWarning    m_warning{MeshBooleanWarning::None};
    bool                  m_highlighted{false};

    bool                      m_running{false};
    std::uint64_t             m_job_token{0};
    std::shared_ptr<JobState> m_job;
    // Expires with the gizmo, so a job finishing after it is gone does nothing.
    std::shared_ptr<char>     m_alive{std::make_shared<char>()};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoMeshBoolean_hpp_
