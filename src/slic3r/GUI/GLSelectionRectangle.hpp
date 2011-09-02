#ifndef slic3r_GLSelectionRectangle_hpp_
#define slic3r_GLSelectionRectangle_hpp_

#include "libslic3r/Point.hpp"
#include "GLModel.hpp"

namespace Slic3r {
namespace GUI {
    
struct Camera;
class GLCanvas3D;

class GLSelectionRectangle {
public:
    enum EState {
            Off,
            Select,
            Deselect
    };

    // Initiates the rectangle. 
    void start_dragging(const Vec2d& mouse_position, EState state);

    // To be called on mouse move.
    void dragging(const Vec2d& mouse_position);

    // Given a vector of points in world coordinates, the function returns indices of those
    // that are in the rectangle.
    std::vector<unsigned int> contains(const std::vector<Vec3d>& points) const;

    // Disables the rectangle.
    void stop_dragging();

    void render(const GLCanvas3D& canvas);

    bool is_dragging() const { return m_state != Off; }
    EState get_state() const { return m_state; }

    float get_width() const  { return std::abs(m_start_corner.x() - m_end_corner.x()); }
    float get_height() const { return std::abs(m_start_corner.y() - m_end_corner.y()); }
    float get_left() const   { return std::min(m_start_corner.x(), m_end_corner.x()); }
    float get_right() const  { return std::max(m_start_corner.x(), m_end_corner.x()); }
    float get_top() const    { return std::max(m_start_corner.y(), m_end_corner.y()); }
    float get_bottom() const { return std::min(m_start_corner.y(), m_end_corner.y()); }

private:
    EState m_state{ Off };
    Vec2d m_start_corner{ Vec2d::Zero() };
    Vec2d m_end_corner{ Vec2d::Zero() };
    GLModel m_rectangle;
    Vec2d m_old_start_corner{ Vec2d::Zero() };
    Vec2d m_old_end_corner{ Vec2d::Zero() };
};

    
} // namespace GUI
} // namespace Slic3r


#endif // slic3r_GLGizmoSlaSupports_hpp_
