#ifndef slic3r_GUI_TickCode_hpp_
#define slic3r_GUI_TickCode_hpp_

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Color.hpp"
#include <set>

namespace Slic3r {
using namespace CustomGCode;
namespace GUI {

struct TickCode
{
    bool operator<(const TickCode& other) const { return other.tick > this->tick; }
    bool operator>(const TickCode& other) const { return other.tick < this->tick; }

    int         tick = 0;
    Type        type = ColorChange;
    int         extruder = 0;
    std::string color;
    std::string extra;
};

class TickCodeInfo
{
    std::string pause_print_msg;
    bool        m_suppress_plus = false;
    bool        m_suppress_minus = false;
    bool        m_use_default_colors = false;

    std::vector<std::string>* m_colors{ nullptr };// reference to IMSlider::m_extruder_colors

    std::string get_color_for_tick(TickCode tick, Type type, const int extruder);

public:
    std::set<TickCode>  ticks{};
    Mode                mode = Undef;

    bool empty() const { return ticks.empty(); }
    void set_pause_print_msg(const std::string& message) { pause_print_msg = message; }

    bool add_tick(const int tick, Type type, int extruder, double print_z);
    bool edit_tick(std::set<TickCode>::iterator it, double print_z);
    void switch_code(Type type_from, Type type_to);
    bool switch_code_for_tick(std::set<TickCode>::iterator it, Type type_to, const int extruder);
    void erase_all_ticks_with_code(Type type);

    bool            has_tick_with_code(Type type);
    bool            has_tick(int tick);

    void suppress_plus(bool suppress) { m_suppress_plus = suppress; }
    void suppress_minus(bool suppress) { m_suppress_minus = suppress; }
    bool suppressed_plus() { return m_suppress_plus; }
    bool suppressed_minus() { return m_suppress_minus; }
    void set_default_colors(bool default_colors_on) { m_use_default_colors = default_colors_on; }

    void set_extruder_colors(std::vector<std::string>* extruder_colors) { m_colors = extruder_colors; }
};

}} // Slic3r

#endif // slic3r_GUI_TickCode_hpp_