#ifndef slic3r_IllustratedTooltip_hpp_
#define slic3r_IllustratedTooltip_hpp_

#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui.h>
#include <wx/string.h>

#include "slic3r/GUI/AnimatedTexture.hpp"

namespace Slic3r::GUI {

class GLCanvas3D;

// A tooltip that waits before it appears and can carry pictures under its text: stills, or GIFs that
// loop while it is open. Used where a control is better shown than described - the texture
// displacement panel's settings and the tools of the gizmo toolbar.
//
// The owner decides where the tooltip goes and how wide it is; this class keeps the timing and the
// pictures, which is the part that has to stay the same everywhere. See
// docs/HLSD/tooltip-illustrations.md.
class IllustratedTooltip
{
public:
    // One picture under the text, named by file within the folder the tooltip was made with, with a
    // translated caption under it. Several in one tooltip are laid out side by side at equal widths,
    // which is how a setting is shown as a comparison - the same model at two values of it.
    struct Media
    {
        std::string file;
        wxString    caption;
    };

    // `media_dir`: the folder under resources/images the pictures are named in.
    explicit IllustratedTooltip(std::string media_dir) : m_media_dir(std::move(media_dir)) {}
    // A copy shows the same folder of pictures but starts with nothing on screen and nothing decoded:
    // the rest is the state of whoever draws it, and the decoded textures cannot be shared. Copies happen
    // because owners are copied, e.g. GLGizmoRotate3D builds its three axis gizmos from temporaries.
    IllustratedTooltip(const IllustratedTooltip &other) : m_media_dir(other.m_media_dir) {}
    IllustratedTooltip &operator=(const IllustratedTooltip &other)
    {
        if (this != &other)
            *this = IllustratedTooltip(other.m_media_dir);
        return *this;
    }
    IllustratedTooltip(IllustratedTooltip &&)            = default;
    IllustratedTooltip &operator=(IllustratedTooltip &&) = default;

    // Call on every frame the pointer rests on a control, with something that tells controls apart
    // (their screen position does). True once the wait has run out and the tooltip should be drawn;
    // until then it asks `canvas` for the frames the wait needs, because a pointer resting still
    // produces no events. False as well while any control is being used.
    bool ready(const ImVec2 &control, GLCanvas3D &canvas);
    // The pointer has left the controls: the next one hovered waits the full delay again. Not calling
    // ready() for a frame does the same, so owners that call it only while hovered need not call this.
    void reset() { m_item = ImVec2(-1.f, -1.f); }

    // The width a tooltip needs, padding included. One carrying pictures takes the full `wrap`, since a
    // picture has to be laid out against something; a plain one takes what its text needs.
    static float width(const std::string &text, bool with_media, float wrap);

    // The usual case: the tooltip of the ImGui item just submitted, shown once the pointer has rested on
    // it. Call it right after the item, on every frame; it does nothing while the item is not hovered.
    // It goes beside the window the item is in (its root window, so a child region does not count): to
    // the right, or to the left when there is no room on the right, and level with the item. A tooltip
    // that followed the mouse would cover the panel it is explaining.
    void item_tooltip(const std::string &text, const std::vector<Media> &media, GLCanvas3D &canvas);

    // Draws it with its top-left corner at `pos`, `width` wide. `title`, when not empty, goes first in
    // bold; its lines after the first are drawn dimmed, which is where a reason the control cannot be
    // used goes. Pictures that are missing or will not decode are skipped, so a tooltip whose art has
    // not been made yet is a plain text tooltip.
    void render(const ImVec2 &pos, float width, const std::string &title, const std::string &text,
                const std::vector<Media> &media, GLCanvas3D &canvas);

private:
    void render_media(const std::vector<Media> &media, float width, GLCanvas3D &canvas);

    std::string m_media_dir;
    // Pictures any tooltip has shown, decoded and uploaded once.
    AnimatedTextureCache m_cache;
    // Which pictures are on screen and when they first appeared: an animation plays from its first frame
    // each time its tooltip opens, rather than from wherever a free-running clock happened to be.
    std::string m_media_key;
    double      m_media_start = 0.;
    int         m_media_frame = -2; // the frame they were last drawn on, to spot the tooltip reopening
    // The control the pointer rests on, since when, and when a tooltip was last on screen.
    ImVec2 m_item{-1.f, -1.f};
    int    m_asked_frame = -2; // the frame ready() was last called on
    double m_hover_since = 0.;
    double m_last_shown  = -1e9;
    // The frame a tooltip was last drawn on, and how many it has been drawn on in a row: ImGui hides one
    // on its first frame while it measures it, so it has to be kept on screen to be seen at all.
    int m_shown_frame = -2;
    int m_settled     = 0;
};

} // namespace Slic3r::GUI

#endif // slic3r_IllustratedTooltip_hpp_
