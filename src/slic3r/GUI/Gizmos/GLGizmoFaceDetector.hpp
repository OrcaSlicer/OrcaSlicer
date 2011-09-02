#ifndef slic3r_GLGizmoFaceDetector_hpp_
#define slic3r_GLGizmoFaceDetector_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/3DScene.hpp"

namespace Slic3r {

namespace GUI {

class GLGizmoFaceDetector : public GLGizmoBase
{
public:
    GLGizmoFaceDetector(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
        : GLGizmoBase(parent, icon_filename, sprite_id) {}

protected:
    void on_render() override;
    void on_render_for_picking() override {}
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    void on_set_state() override;
    bool on_is_activable() const override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    bool on_init() override;
    void perform_recognition(const Selection& selection);
    void display_exterior_face();

    GUI::GLModel model;
    double m_sample_interval = {0.5};
};

} // namespace GUI
} // namespace Slic3r


#endif // slic3r_GLGizmoFaceDetector_hpp_
