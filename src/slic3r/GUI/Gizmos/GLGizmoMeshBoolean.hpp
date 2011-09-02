#ifndef slic3r_GLGizmoMeshBoolean_hpp_
#define slic3r_GLGizmoMeshBoolean_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

namespace GUI {

enum class MeshBooleanSelectingState {
    Undef,
    SelectSource,
    SelectTool,

};
enum class MeshBooleanOperation{
    Undef,
    Union,
    Difference,
    Intersection,
};
struct VolumeInfo {
    ModelVolume* mv{ nullptr };
    int volume_idx{-1};
    Transform3d trafo;
    void reset() {
        mv = nullptr;
        volume_idx = -1;
        trafo = Transform3d::Identity();
    };
    template<class Archive>
    void serialize(Archive& ar) {
        ar(volume_idx, trafo);
    }
};
class GLGizmoMeshBoolean : public GLGizmoBase
{
public:
    GLGizmoMeshBoolean(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoMeshBoolean();

    void set_enable(bool enable) { m_enable = enable; }
    bool get_enable() { return m_enable; }
    MeshBooleanSelectingState get_selecting_state() { return m_selecting_state; }
    void set_src_volume(ModelVolume* mv) { 
        m_src.mv = mv;
        if (m_src.mv == m_tool.mv)
            m_tool.reset();
    }
    void set_tool_volume(ModelVolume* mv) { 
        m_tool.mv = mv;
        if (m_tool.mv == m_src.mv)
            m_src.reset();
    }

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);

    /// <summary>
    /// Implement when want to process mouse events in gizmo
    /// Click, Right click, move, drag, ...
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information and don't want to
    /// propagate it otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

protected:
    virtual bool on_init() override;
    virtual std::string on_get_name() const override;
    virtual bool on_is_activable() const override;
    virtual void on_render() override;
    virtual void on_set_state() override;
    virtual CommonGizmosDataID on_get_requirements() const override;
    virtual void on_render_input_window(float x, float y, float bottom_limit);

    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;

private:
    bool m_enable{ false };
    MeshBooleanOperation m_operation_mode;
    MeshBooleanSelectingState m_selecting_state;
    bool m_diff_delete_input = false;
    bool m_inter_delete_input = false;
    VolumeInfo m_src;
    VolumeInfo m_tool;

    void generate_new_volume(bool delete_input, const TriangleMesh& mesh_result);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoMeshBoolean_hpp_