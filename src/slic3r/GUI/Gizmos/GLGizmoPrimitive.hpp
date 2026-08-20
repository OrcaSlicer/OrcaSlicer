#ifndef slic3r_GLGizmoPrimitive_hpp_
#define slic3r_GLGizmoPrimitive_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"

namespace Slic3r {
namespace GUI {

class GLGizmoPrimitive : public GLGizmoBase
{
public:
    GLGizmoPrimitive(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoPrimitive() = default;

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
    void apply_primitive();
    void apply_preset(const char* name, double w, double h, double d);

    PrimitiveParams m_params;
    TriangleMesh    m_preview_mesh;
    bool            m_preview_dirty{true};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoPrimitive_hpp_
