#ifndef slic3r_GUI_StateColor_hpp_
#define slic3r_GUI_StateColor_hpp_

#include <wx/colour.h>

#include <map>

#define WXCOLOUR_GREY700 wxColour(107, 107, 107)
#define WXCOLOUR_GREY500 wxColour(158, 158, 158)
#define WXCOLOUR_GREY400 wxColour("#CECECE")
#define WXCOLOUR_GREY300 wxColour(238, 238, 238)
#define WXCOLOUR_GREY200 wxColour(248, 248, 248)

class StateColor
{
public:
    enum State {
        Normal = 0, 
        Enabled = 1,
        Checked = 2,
        Focused = 4,
        Hovered = 8,
        Pressed = 16,
        Disabled = 1 << 16,
        NotChecked = 2 << 16,
        NotFocused = 4 << 16,
        NotHovered = 8 << 16,
        NotPressed = 16 << 16,
    };

public:
    static std::tuple<double, double, double> GetLAB(const wxColour& color);
    static double GetLightness(const wxColour& color);
    static wxColour SetLightness(const wxColour& color, double lightness);
    static wxColour LightenDarkenColor(const wxColour& color, int amount);
    static double GetColorDifference(const wxColour& c1, const wxColour& c2);
    static double LAB_Delta_E(const wxColour& c1, const wxColour& c2);

    static void SetDarkMode(bool dark);

    static std::map<wxColour, wxColour> const & GetDarkMap();
    static wxColour darkModeColorFor(wxColour const &color);
    static wxColour lightModeColorFor(wxColour const &color);

    // Button style
    static StateColor createButtonStyleGray();

public:
    template<typename ...Colors>
    StateColor(std::pair<Colors, int>... colors) {
        fill(colors...);
    }

    // single color
    StateColor(wxColour const & color);

    // single color
    StateColor(wxString const &color);

    // single color
    StateColor(unsigned long color);

    // operator==
    bool operator==(StateColor const& other) const{
        return statesList_ == other.statesList_ && colors_ == other.colors_ && takeFocusedAsHovered_ == other.takeFocusedAsHovered_;
    };

    // operator!=
    bool operator!=(StateColor const& other) const{
        return !(*this == other);
    };

public:
    void append(wxColour const & color, int states);

    void append(wxString const &color, int states);

    void append(unsigned long color, int states);

    void clear();

public:
    int count() const { return statesList_.size(); }

    int states() const;

public:
    wxColour defaultColor();

    wxColour colorForStates(int states);

    wxColour colorForStatesNoDark(int states);

    int colorIndexForStates(int states);

    bool setColorForStates(wxColour const & color, int states);

    void setTakeFocusedAsHovered(bool set);

private:
    template<typename Color, typename ...Colors>
    void fill(std::pair<Color, int> color, std::pair<Colors, int>... colors) {
        fillOne(color);
        fill(colors...);
    }

    template<typename Color>
    void fillOne(std::pair<Color, int> color) {
        append(color.first, color.second);
    }

    void fill() {
    }

private:
    std::vector<int> statesList_;
    std::vector<wxColour> colors_;
    bool takeFocusedAsHovered_ = true;
};

#endif // !slic3r_GUI_StateColor_hpp_
