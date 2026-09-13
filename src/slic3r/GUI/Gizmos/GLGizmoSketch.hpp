#ifndef slic3r_GLGizmoSketch_hpp_
#define slic3r_GLGizmoSketch_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include <imgui/imgui.h>

namespace Slic3r {
namespace GUI {

enum class SketchTool { Line, Rectangle, Circle, Polygon, COUNT };

class GLGizmoSketch : public GLGizmoBase
{
public:
    GLGizmoSketch(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent& mouse_event) override;

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    void on_render() override;
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void on_render_input_window(float x, float y, float bottom_limit) override;

    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;

private:
    SketchTool    m_tool{SketchTool::Line};
    std::vector<SketchProfile> m_profiles;  // multiple profiles (outer + holes)
    SketchPlane   m_plane{SketchPlane::XY()};
    SketchParams  m_sp;

    // Shape presets
    double m_rect_w{20}, m_rect_h{15};
    double m_circle_r{10}; int m_circle_seg{32};
    int    m_poly_sides{6}; double m_poly_r{10};

    // Canvas
    std::vector<ImVec2> m_canvas_points;
    Vec2d               m_canvas_center{0,0};
    float               m_canvas_scale{5.0f};
    bool                m_snap_grid{true};
    float               m_grid_step{5.0f};

    // Current profile being drawn
    int m_active_profile{-1};

    SketchProfile& active_profile();
    bool has_closed_profile() const;

    void build_preset_profile();
    void add_closed_profile();
    void delete_profile(int idx);
    void clear_all();

    TopoDS_Shape build_combined_shape(); // all profiles as face with holes
    void apply_extrude();
    void apply_revolve();
    void apply_pocket();
    void draw_canvas();
    void handle_canvas_click(ImVec2 pos);
    Vec2d snap(Vec2d pt) const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSketch_hpp_
