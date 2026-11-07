#pragma once

#include <functional>
#include <vector>

#include "libslic3r/CAD/Quotes.hpp"

namespace Slic3r {

namespace GUI {

class Camera;
class ImGuiWrapper;

// The dimensions of the body in the viewport, drawn the way a drawing office draws them: thin extension lines, a dimension line
// with filled arrowheads and the number set in a break of it, a diameter with its leader, a radius pointing at its arc. It owns
// no document state: the owner hands it the quotes to draw, and a click on one that has a driving feature only EMITS it.
class DesignQuoteOverlay
{
public:
    std::function<const std::vector<Quotes::Quote>&()> quotes;   // what to draw this frame
    std::function<void(int feature)>                   edit;     // a quote with a driving feature was clicked

    void render(ImGuiWrapper& imgui, const Camera& camera, float scale);
    bool hovered() const { return m_hovered; }

private:
    bool m_hovered{false};
};

} // namespace GUI
} // namespace Slic3r
