#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "Camera.hpp"
#include "NavigatorGeometry.hpp"
#include "Shortcuts.hpp"
#include "CanvasUi/Availability.hpp"
#include "CanvasUi/Menu.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
namespace GUI {

namespace CanvasUi {
class Card;
}

struct NavigatorToggle
{
    std::string            label;
    CanvasUi::Indicator    indicator{CanvasUi::Indicator::Visibility};
    bool                   checked{false};
    std::function<void()>  toggle;
    CanvasUi::Availability availability;
};

struct NavigatorToggleGroup
{
    std::string                  title;
    std::vector<NavigatorToggle> toggles;
};

struct NavigatorFrame
{
    NavLayout layout;
    // Another drag holds the mouse, so a press over the cube is not the navigator's.
    bool      blocked{false};
    // The view the user set as home; without one the home button turns to the default view.
    std::optional<Eigen::Quaterniond> home_view;
    bool                              auto_perspective{false};

    std::function<std::string(Shortcut)>                 shortcut;
    std::function<std::vector<NavigatorToggleGroup>()>   display_toggles;
    std::function<void()>                                fit;
    // Frames the bed at the end of the turn to the default view, as its shortcut does.
    std::function<void()>                                frame_bed;
    std::function<void(bool perspective)>                set_perspective;
    std::function<void()>                                toggle_auto_perspective;
    std::function<void(const std::optional<Eigen::Quaterniond>&)> set_home_view;
};

// The 3D navigator in the bottom-left corner of the scene: the cube on the bed, which follows the
// camera and turns it, and the toolbar of navigation buttons with the Views and Display menus.
class Navigator3D
{
public:
    Navigator3D();

    // Draws into the current ImGui frame, asking the toolkit context for the frames it needs next.
    void render(Camera& camera, const NavigatorFrame& frame);
    // From the press on the cube or the bed to its release.
    bool holding() const { return m_press.has_value(); }
    bool dragging() const { return m_press.has_value() && m_press->dragging; }
    bool animating() const { return m_animation.has_value(); }
    void cancel_drag() { m_press.reset(); }

private:
    using Clock = std::chrono::steady_clock;

    struct Animation
    {
        Eigen::Quaterniond from;
        Eigen::Quaterniond to;
        Clock::time_point     start;
        std::function<void()> done;
        // What the turn last set, and in which frame.
        Eigen::Quaterniond    last;
        int                   last_frame;
    };

    void animate_to(Camera& camera, const Eigen::Quaterniond& target, std::function<void()> done = {});
    void animate_to_zone(Camera& camera, size_t zone);
    void rotate_about_z(Camera& camera, double angle);
    void step_animation(Camera& camera);

    int  hovered_target(const NavProjection& projection, const Vec2d& mouse, bool& bed) const;
    void handle_cube_input(Camera& camera, const NavProjection& projection, bool mouse_free, const NavigatorFrame& frame);
    void draw_cube(const NavProjection& projection, const NavigatorFrame& frame, int hovered, const CanvasUi::Card& card) const;
    void cube_tooltip(const NavigatorFrame& frame, int hovered);

    void render_toolbar(Camera& camera, const NavigatorFrame& frame, CanvasUi::Card& card);
    void render_views_menu(Camera& camera, const NavigatorFrame& frame);
    void render_display_menu(const NavigatorFrame& frame);

    // Each canvas has its own navigator window and menus, as all canvases share one ImGui context.
    std::string              m_card_id;
    std::optional<NavPress>  m_press;
    std::optional<Animation> m_animation;
    bool                     m_turned{false};
};

} // namespace GUI
} // namespace Slic3r
