#include "StateColor.hpp"
#include <wx/gdicmn.h>

static bool gDarkMode = false;

static bool operator<(wxColour const &l, wxColour const &r) { return l.GetRGBA() < r.GetRGBA(); }

static std::map<wxColour, wxColour> gDarkColors{
    {"#00AE42", "#21A452"},/*green*/
    {"#1F8EEA", "#2778D2"},/*blue*/
    {"#FF6F00", "#D15B00"},
    {"#D01B1B", "#BB2A3A"},/*red*/
    {"#262E30", "#EFEFF0"},/*black*/
    {"#2C2C2E", "#B3B3B4"},/*black*/
    {"#E5E7EB", "#374151"},/*gray200 -> gray800*/
    {"#6B6B6B", "#818183"},/*gray -> */
    {"#ACACAC", "#54545A"},/*gray -> */
    {"#EEEEEE", "#4C4C55"},/*gray -> */
    {"#E8E8E8", "#3E3E45"},
    {"#323A3D", "#E5E5E4"},
    {"#FFFFFF", "#2D2D31"},
    {"#F8F8F8", "#36363C"},
    {"#F1F1F1", "#36363B"},
    {"#3B4446", "#2D2D30"},
    {"#CECECE", "#54545B"},
    {"#DBFDD5", "#3B3B40"},
    {"#000000", "#FFFFFE"},
    {"#F4F4F4", "#36363D"},
    {"#F7F7F7", "#333337"},
    {"#DBDBDB", "#4A4A51"},
    {"#EDFAF2", "#283232"},
    {"#323A3C", "#E5E5E6"},
    {"#6B6B6A", "#B3B3B5"},
    {"#303A3C", "#E5E5E5"},
    {"#FEFFFF", "#242428"},
    {"#A6A9AA", "#2D2D29"},
    {"#363636", "#B2B3B5"},
    {"#F0F0F1", "#404040"},
    {"#9E9E9E", "#53545A"},
    {"#D7E8DE", "#1F2B27"},
    {"#2B3436", "#808080"},
    {"#ABABAB", "#ABABAB"},
    {"#D9D9D9", "#2D2D32"},
    {"#EBF9F0", "#293F34"}
    //{"#F0F0F0", "#4C4C54"},
};

std::map<wxColour, wxColour> const & StateColor::GetDarkMap()
{
    return gDarkColors;
}

void StateColor::SetDarkMode(bool dark) { gDarkMode = dark; }

inline wxColour darkModeColorFor2(wxColour const &color)
{
    if (!gDarkMode)
        return color;
    auto iter = gDarkColors.find(color);
    if (iter != gDarkColors.end()) return iter->second;
    return color;
}

std::map<wxColour, wxColour> revert(std::map<wxColour, wxColour> const & map)
{
    std::map<wxColour, wxColour> map2;
    for (auto &p : map) map2.emplace(p.second, p.first);
    return map2;
}

wxColour StateColor::lightModeColorFor(wxColour const &color)
{
    static std::map<wxColour, wxColour> gLightColors = revert(gDarkColors);
    auto iter = gLightColors.find(color);
    if (iter != gLightColors.end()) return iter->second;
    return color;
}

wxColour StateColor::darkModeColorFor(wxColour const &color) { return darkModeColorFor2(color); }

StateColor::StateColor(wxColour const &color) { append(color, 0); }

StateColor::StateColor(wxString const &color) { append(color, 0); }

StateColor::StateColor(unsigned long color) { append(color, 0); }

void StateColor::append(wxColour const & color, int states)
{
    statesList_.push_back(states);
    colors_.push_back(color);
}

void StateColor::append(wxString const & color, int states)
{
    wxColour c1(color);
    append(c1, states);
}

void StateColor::append(unsigned long color, int states)
{
    if ((color & 0xff000000) == 0)
        color |= 0xff000000;
    wxColour cl; cl.SetRGBA(color & 0xff00ff00 | ((color & 0xff) << 16) | ((color >> 16) & 0xff));
    append(cl, states);
}

void StateColor::clear()
{
    statesList_.clear();
    colors_.clear();
}

int StateColor::states() const
{
    int states = 0;
    for (auto s : statesList_) states |= s;
    states = (states & 0xffff) | (states >> 16);
    if (takeFocusedAsHovered_ && (states & Hovered))
        states |= Focused;
    return states;
}

wxColour StateColor::defaultColor() {
    return colorForStates(0);
}

wxColour StateColor::colorForStates(int states)
{
    bool focused = takeFocusedAsHovered_ && (states & Focused);
    for (int i = 0; i < statesList_.size(); ++i) {
        int s = statesList_[i];
        int on = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) {
            return darkModeColorFor2(colors_[i]);
        }
        if (focused && (on & Hovered)) {
            on |= Focused;
            on &= ~Hovered;
            if ((on & states) == on && (off & ~states) == off) {
                return darkModeColorFor2(colors_[i]);
            }
        }
    }
    return wxColour(0, 0, 0, 0);
}

wxColour StateColor::colorForStatesNoDark(int states)
{
    bool focused = takeFocusedAsHovered_ && (states & Focused);
    for (int i = 0; i < statesList_.size(); ++i) {
        int s = statesList_[i];
        int on = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) {
            return colors_[i];
        }
        if (focused && (on & Hovered)) {
            on |= Focused;
            on &= ~Hovered;
            if ((on & states) == on && (off & ~states) == off) {
                return colors_[i];
            }
        }
    }
    return wxColour(0, 0, 0, 0);
}

int StateColor::colorIndexForStates(int states)
{
    for (int i = 0; i < statesList_.size(); ++i) {
        int s   = statesList_[i];
        int on  = s & 0xffff;
        int off = s >> 16;
        if ((on & states) == on && (off & ~states) == off) { return i; }
    }
    return -1;
}

bool StateColor::setColorForStates(wxColour const &color, int states)
{
    for (int i = 0; i < statesList_.size(); ++i) {
        if (statesList_[i] == states) {
            colors_[i] = color;
            return true;
        }
    }
    return false;
}

void StateColor::setTakeFocusedAsHovered(bool set) { takeFocusedAsHovered_ = set; }

StateColor StateColor::createButtonStyleGray()
{
    return StateColor(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Focused),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
}
