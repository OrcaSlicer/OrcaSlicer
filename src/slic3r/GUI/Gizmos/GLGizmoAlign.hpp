#ifndef slic3r_GLGizmoAlign_hpp_
#define slic3r_GLGizmoAlign_hpp_

#include "GLGizmoBase.hpp"

namespace Slic3r {
namespace GUI {

class GLGizmoAlign : public GLGizmoBase
{
public:
    GLGizmoAlign(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoAlign() override = default;

protected:
    std::string on_get_name() const override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override;
    bool on_init() override { return true; }
    void on_render() override {}
    CommonGizmosDataID on_get_requirements() const override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAlign_hpp_
