#include "TickCode.hpp"

namespace Slic3r {
namespace GUI {
std::string TickCodeInfo::get_color_for_tick(TickCode tick, Type type, const int extruder)
{
    auto opposite_one_color = [](const std::string& color) {
        ColorRGB rgb;
        decode_color(color, rgb);
        return encode_color(opposite(rgb));
    };
    auto opposite_two_colors = [](const std::string& a, const std::string& b) {
        ColorRGB rgb1; decode_color(a, rgb1);
        ColorRGB rgb2; decode_color(b, rgb2);
        return encode_color(opposite(rgb1, rgb2));
    };

    if (mode == SingleExtruder && type == ColorChange && m_use_default_colors) {
#if 1
        if (ticks.empty()) return opposite_one_color((*m_colors)[0]);

        auto before_tick_it = std::lower_bound(ticks.begin(), ticks.end(), tick);
        if (before_tick_it == ticks.end()) {
            while (before_tick_it != ticks.begin())
                if (--before_tick_it; before_tick_it->type == ColorChange) break;
            if (before_tick_it->type == ColorChange) return opposite_one_color(before_tick_it->color);
            return opposite_one_color((*m_colors)[0]);
        }

        if (before_tick_it == ticks.begin()) {
            const std::string &frst_color = (*m_colors)[0];
            if (before_tick_it->type == ColorChange) return opposite_two_colors(frst_color, before_tick_it->color);

            auto next_tick_it = before_tick_it;
            while (next_tick_it != ticks.end())
                if (++next_tick_it; next_tick_it->type == ColorChange) break;
            if (next_tick_it->type == ColorChange) return opposite_two_colors(frst_color, next_tick_it->color);

            return opposite_one_color(frst_color);
        }

        std::string frst_color = "";
        if (before_tick_it->type == ColorChange)
            frst_color = before_tick_it->color;
        else {
            auto next_tick_it = before_tick_it;
            while (next_tick_it != ticks.end())
                if (++next_tick_it; next_tick_it->type == ColorChange) {
                    frst_color = next_tick_it->color;
                    break;
                }
        }

        while (before_tick_it != ticks.begin())
            if (--before_tick_it; before_tick_it->type == ColorChange) break;

        if (before_tick_it->type == ColorChange) {
            if (frst_color.empty()) return opposite_one_color(before_tick_it->color);
            return opposite_two_colors(before_tick_it->color, frst_color);
        }

        if (frst_color.empty()) return opposite_one_color((*m_colors)[0]);
        return opposite_two_colors((*m_colors)[0], frst_color);
#else
        const std::vector<std::string> &colors = ColorPrintColors::get();
        if (ticks.empty()) return colors[0];
        m_default_color_idx++;

        return colors[m_default_color_idx % colors.size()];
#endif
    }

    std::string color = (*m_colors)[extruder - 1];

    if (type == ColorChange) {
        if (!ticks.empty()) {
            auto before_tick_it = std::lower_bound(ticks.begin(), ticks.end(), tick);
            while (before_tick_it != ticks.begin()) {
                --before_tick_it;
                if (before_tick_it->type == ColorChange && before_tick_it->extruder == extruder) {
                    color = before_tick_it->color;
                    break;
                }
            }
        }

        //TODO
        //color = get_new_color(color);
    }
    return color;
}


bool TickCodeInfo::add_tick(const int tick, Type type, const int extruder, double print_z)
{
    std::string color;
    std::string extra;
    if (type == Custom) // custom Gcode
    {
        //extra = get_custom_code(custom_gcode, print_z);
        //if (extra.empty()) return false;
        //custom_gcode = extra;
    } else if (type == PausePrint) {
        //BBS do not set pause extra message
        //extra = get_pause_print_msg(pause_print_msg, print_z);
        //if (extra.empty()) return false;
        pause_print_msg = extra;
    }
    else {
        color = get_color_for_tick(TickCode{ tick }, type, extruder);
        if (color.empty()) return false;
    }

    if (mode == SingleExtruder) m_use_default_colors = true;

    ticks.emplace(TickCode{tick, type, extruder, color, extra});

    return true;
}

bool TickCodeInfo::edit_tick(std::set<TickCode>::iterator it, double print_z)
{
    std::string edited_value;
    //TODO
    /* BBS
    if (it->type == ColorChange)
        edited_value = get_new_color(it->color);
    else if (it->type == PausePrint)
        edited_value = get_pause_print_msg(it->extra, print_z);
    else
        edited_value = get_custom_code(it->type == Template ? gcode(Template) : it->extra, print_z);
    */
    if (edited_value.empty()) return false;

    TickCode changed_tick = *it;
    if (it->type == ColorChange) {
        if (it->color == edited_value) return false;
        changed_tick.color = edited_value;
    } else if (it->type == Template) {
        //if (gcode(Template) == edited_value) return false;
        //changed_tick.extra = edited_value;
        //changed_tick.type  = Custom;
        ;
    } else if (it->type == Custom || it->type == PausePrint) {
        if (it->extra == edited_value) return false;
        changed_tick.extra = edited_value;
    }

    ticks.erase(it);
    ticks.emplace(changed_tick);

    return true;
}

void TickCodeInfo::switch_code(Type type_from, Type type_to)
{
    for (auto it{ticks.begin()}, end{ticks.end()}; it != end;)
        if (it->type == type_from) {
            TickCode tick = *it;
            tick.type     = type_to;
            tick.extruder = 1;
            ticks.erase(it);
            it = ticks.emplace(tick).first;
        } else
            ++it;
}

bool TickCodeInfo::switch_code_for_tick(std::set<TickCode>::iterator it, Type type_to, const int extruder)
{
    const std::string color = get_color_for_tick(*it, type_to, extruder);
    if (color.empty()) return false;

    TickCode changed_tick = *it;
    changed_tick.type     = type_to;
    changed_tick.extruder = extruder;
    changed_tick.color    = color;

    ticks.erase(it);
    ticks.emplace(changed_tick);

    return true;
}

void TickCodeInfo::erase_all_ticks_with_code(Type type)
{
    for (auto it{ticks.begin()}, end{ticks.end()}; it != end;) {
        if (it->type == type)
            it = ticks.erase(it);
        else
            ++it;
    }
}

bool TickCodeInfo::has_tick_with_code(Type type)
{
    for (const TickCode &tick : ticks)
        if (tick.type == type) return true;

    return false;
}

bool TickCodeInfo::has_tick(int tick) { return ticks.find(TickCode{tick}) != ticks.end(); }

}}