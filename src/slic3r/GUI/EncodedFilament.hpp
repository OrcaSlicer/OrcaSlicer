#ifndef slic3r_ENCODED_FILAMENT_hpp_
#define slic3r_ENCODED_FILAMENT_hpp_

#include <vector>
#include <map>
#include <math.h>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <wx/colour.h>
#include <wx/string.h>

#include <chrono>

namespace Slic3r
{

//Previous definitions
class FilamentColorCode;
class FilamentColorCodes;
class FilamentColorCodeQuery;

// Represents a color in HSV format
struct ColourHSV
{
    double h, s, v;
};

inline ColourHSV wxColourToHSV(const wxColour& c)
{
    double r = c.Red() / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue() / 255.0;

    double maxc = std::max({ r, g, b });
    double minc = std::min({ r, g, b });
    double delta = maxc - minc;

    double h = 0, s = 0, v = maxc;
    if (delta > 0.00001) {
        if (maxc == r) {
            h = 60.0 * (fmod(((g - b) / delta), 6.0));
        } else if (maxc == g) {
            h = 60.0 * (((b - r) / delta) + 2.0);
        } else {
            h = 60.0 * (((r - g) / delta) + 4.0);
        }

        if (h < 0) h += 360.0;
        s = delta / maxc;
    } else {
        h = 0;
        s = 0;
    }
    return { h, s, v };
}

// FilamentColorSorter
struct wxColorSorter
{
    std::size_t operator()(const wxColour& lhs_it, const wxColour& rhs_it) const noexcept {
        ColourHSV ha = wxColourToHSV(lhs_it);
        ColourHSV hb = wxColourToHSV(rhs_it);
        if (ha.h != hb.h) return ha.h < hb.h;
        if (ha.s != hb.s) return ha.s < hb.s;
        if (ha.v != hb.v) return ha.v < hb.v;
        if (lhs_it.Alpha() != rhs_it.Alpha()) return lhs_it.Alpha() < rhs_it.Alpha();
        return false;
    }
};

struct FilamentColor
{
    enum class ColorType : char
    {
        SINGLE_CLR = 0, // single color filament
        MULTI_CLR,  // multi-color filament
        GRADIENT_CLR,    // gradient filament
    };

    ColorType m_color_type = ColorType::SINGLE_CLR; // default to single color
    std::set<wxColour, wxColorSorter> m_colors;

public:
    size_t ColorCount() const noexcept { return m_colors.size(); }

    void EndSet(int ctype)
    {
        if (m_colors.size() < 2)
        {
            m_color_type = ColorType::SINGLE_CLR;
        }
        else
        {
            if (ctype == 0)
            {
                m_color_type = ColorType::GRADIENT_CLR;
            }
            else
            {
                m_color_type = ColorType::MULTI_CLR;
            }
        }
    }

public:
    bool operator<(const FilamentColor& other) const { 
        if (ColorCount() != other.ColorCount()) { return ColorCount() < other.ColorCount(); };
        if (m_color_type != other.m_color_type) { return m_color_type < other.m_color_type; }
        if (m_colors == other.m_colors) { return false;}

        // Compare colors in HSV format
        auto lhs_it = m_colors.begin();
        auto rhs_it = other.m_colors.begin();
        while ((lhs_it != m_colors.end()))
        {
            ColourHSV ha = wxColourToHSV(*lhs_it);
            ColourHSV hb = wxColourToHSV(*rhs_it);
            if (ha.h != hb.h) return ha.h < hb.h;
            if (ha.s != hb.s) return ha.s < hb.s;
            if (ha.v != hb.v) return ha.v < hb.v;

            int lhs_alpha = lhs_it->Alpha();
            int rhs_alpha = rhs_it->Alpha();
            if (lhs_alpha != rhs_alpha) return lhs_alpha < rhs_alpha;

            lhs_it++;
            rhs_it++;
        }

        return false;
    }
};

// Compare function for EncodedFilaColor
struct EncodedFilaColorEqual
{
    bool operator()(const FilamentColor& lhs, const FilamentColor& rhs) const noexcept { return lhs < rhs; }
};
using FilamentColor2CodeMap = std::map<FilamentColor, FilamentColorCode*, EncodedFilaColorEqual>;


// FilamentColorCodeQuery class is used to query filament color codes and their associated information
class FilamentColorCodeQuery
{
public:
    FilamentColorCodeQuery();
    virtual ~FilamentColorCodeQuery();

public:
    FilamentColorCodes* GetFilaInfoMap(const wxString& fila_id) const;
    wxString GetFilaColorName(const wxString& fila_id, const FilamentColor& colors) const;

private:
    FilamentColorCode* GetFilaInfo(const wxString& fila_id, const FilamentColor& colors) const;

protected:
    void  LoadFromLocal();

public:
    void  CreateFilaCode(const wxString& fila_id,
                         const wxString& fila_type,
                         const wxString& fila_color_code,
                         FilamentColor&& fila_color,
                         std::unordered_map<wxString, wxString>&& fila_color_names);

private:
    /* loaded info*/
    std::string m_fila_path;

    std::unordered_map<wxString, FilamentColorCodes*>* m_fila_id2colors_map; //
};

// EncodedFilaColorsInfo class holds a mapping of filament codes to specific filamet type
class FilamentColorCodes
{
public:
    FilamentColorCodes(const wxString& fila_id, const wxString& fila_type);
    virtual ~FilamentColorCodes();

public:
    wxString GetFilaCode() const { return m_fila_id; }
    wxString GetFilaType() const { return m_fila_type; }

    FilamentColor2CodeMap* GetFilamentColor2CodeMap() const { return m_fila_colors_map; }
    FilamentColorCode* GetColorCode(const FilamentColor& colors) const;

    void Debug(const char* prefix);

public:
    void AddColorCode(FilamentColorCode* code);

private:
    wxString m_fila_id;//eg. 54600
    wxString m_fila_type;//eg. PEBA 90A
    FilamentColor2CodeMap* m_fila_colors_map; // key is the color set, value is the info
};

// The EncodedFilaColorInfo class holds information about a specific filament color
class FilamentColorCode
{
public:
    FilamentColorCode() = delete;
    FilamentColorCode(const wxString& color_code, FilamentColorCodes* owner, FilamentColor&& color, std::unordered_map<wxString, wxString>&& name_map);
    ~FilamentColorCode() {};

public:
    wxString GetFilaCode() const { return m_owner->GetFilaCode(); }
    wxString GetFilaType() const { return m_owner->GetFilaType(); }


    wxString         GetFilaColorCode() const { return m_fila_color_code; } // eg. Q01B00
    FilamentColor    GetFilaColor() const { return m_fila_color; }
    wxString         GetFilaColorName() const;

    void Debug(const char* prefix);

private:
    FilamentColorCodes* m_owner;

    /* color info*/
    wxString                               m_fila_color_code; // eg. Q01B00
    FilamentColor                          m_fila_color;
    std::unordered_map<wxString, wxString> m_fila_color_names; // eg. en -> Red
};

}
#endif
