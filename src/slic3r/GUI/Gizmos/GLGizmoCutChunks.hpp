#ifndef slic3r_GLGizmoCutChunks_hpp_
#define slic3r_GLGizmoCutChunks_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoCut.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "imgui/imgui.h"

namespace Slic3r {
enum class CutConnectorType : int;
class ModelVolume;
class GLShaderProgram;
struct CutConnectorAttributes;

namespace GUI {

class GLGizmoCutChunks3D : public GLGizmoCut3D
{
public:
    GLGizmoCutChunks3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
        : GLGizmoCut3D(parent, icon_filename, sprite_id)
    {
        // m_keep_as_parts = true;
        m_place_on_cut_upper = false;
    }

protected:
    // UNLAYERED_MOD — a chunk cut only means something on a machine with rotary axes: the pieces it
    // produces carry layer_rotation_b/c, which slicing now ignores unless enable_nonplanar is set.
    bool on_is_activable() const override;
    void perform_cut(const Selection& selection, bool surpress_diag) override;
    void perform_skeleton_cut(const Selection& selection);
    void render_cut_plane_input_window(CutConnectors& connectors, float x, float y, float bottom_limit) override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoCutChunks_hpp_
