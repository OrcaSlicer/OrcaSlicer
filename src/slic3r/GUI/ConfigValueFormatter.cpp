#include "ConfigValueFormatter.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "I18N.hpp"
#include "GUI.hpp"
#include "Field.hpp"

namespace Slic3r {
namespace GUI {

std::string get_pure_opt_key(const std::string& opt_key)
{
    std::string pure_key = opt_key;
    const int pos = pure_key.find("#");
    if (pos > 0)
        boost::erase_tail(pure_key, pure_key.size() - pos);
    return pure_key;
}

wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill, int idx)
{
    const ConfigOptionDef& def = config.def()->options.at(opt_key);
    const std::vector<std::string>& names = def.enum_labels;//ConfigOptionEnum<T>::get_enum_names();
    int val = 0;

    if (idx >= 0)
        val = dynamic_cast<const ConfigOptionInts*>(config.option(opt_key))->get_at(idx);
    else
        val = config.option(opt_key)->getInt();

    // Each infill doesn't use all list of infill declared in PrintConfig.hpp.
    // So we should "convert" val to the correct one
    if (is_infill) {
        for (auto key_val : *def.enum_keys_map)
            if (int(key_val.second) == val) {
                auto it = std::find(def.enum_values.begin(), def.enum_values.end(), key_val.first);
                if (it == def.enum_values.end())
                    return "";
                return from_u8(_utf8(names[it - def.enum_values.begin()]));
            }
        return _L("Undefined");
    }
    return from_u8(_utf8(names[val]));
}

wxString get_full_label(const std::string& opt_key, const DynamicPrintConfig& config)
{
    const std::string pure_key = get_pure_opt_key(opt_key);
    auto option = config.option(pure_key);

    if (!option || option->is_nil())
        return _L("N/A");

    const ConfigOptionDef* opt = config.def()->get(pure_key);
    return opt->full_label.empty() ? opt->label : opt->full_label;
}

wxString get_string_value(const std::string& opt_key, const DynamicPrintConfig& config)
{
    int orig_opt_idx = -1;
    int opt_idx = -1;
    int pos = opt_key.find("#");
    std::string temp_str = opt_key;
    if (pos > 0) {
        boost::erase_head(temp_str, pos + 1);
        orig_opt_idx = static_cast<size_t>(atoi(temp_str.c_str()));
    }
    opt_idx = orig_opt_idx >= 0 ? orig_opt_idx : 0;
    const std::string pure_key = get_pure_opt_key(opt_key);
    auto option = config.option(pure_key);
    if (!option) {
        return _L("N/A");
    }
    auto opt_vector = dynamic_cast<const ConfigOptionVectorBase *>(option);

    if (option->is_scalar() && config.option(pure_key)->is_nil() ||
        option->is_vector() && opt_vector && opt_idx >= 0 && opt_idx < opt_vector->size() && opt_vector->is_nil(opt_idx))
        return _L("N/A");

    wxString out;

    const ConfigOptionDef* opt = config.def()->get(pure_key);
    bool is_nullable = opt->nullable;

    switch (opt->type) {
    case coInt:
        return from_u8((boost::format("%1%") % config.opt_int(pure_key)).str());
    case coInts: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionIntsNullable>(pure_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
        }
        else {
            auto values = config.opt<ConfigOptionInts>(pure_key);
            if (orig_opt_idx >= 0 && orig_opt_idx < values->size()) {
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
            }
            else {
                std::string value_str;
                for (int i = 0; i < values->size(); i++) {
                    value_str += std::to_string(values->get_at(i));
                    if (i != values->size() - 1) {
                        value_str += ",";
                    }
                }
                return from_u8(value_str);
            }
        }
        return _L("Undefined");
    }
    case coBool:
        return config.opt_bool(pure_key) ? "true" : "false";
    case coBools: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionBoolsNullable>(pure_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        }
        else {
            auto values = config.opt<ConfigOptionBools>(pure_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        }
        return _L("Undefined");
    }
    case coPercent:
        return from_u8((boost::format("%1%%%") % int(config.optptr(pure_key)->getFloat())).str());
    case coPercents: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionPercentsNullable>(pure_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        }
        else {
            auto values = config.opt<ConfigOptionPercents>(pure_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        }
        return _L("Undefined");
    }
    case coFloat:
        return double_to_string(config.opt_float(pure_key));
    case coFloats: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionFloatsNullable>(pure_key);
            if (opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        }
        else {
            auto values = config.opt<ConfigOptionFloats>(pure_key);
            if (values && opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        }
        return _L("Undefined");
    }
    case coString:
        return from_u8(config.opt_string(pure_key));
    case coStrings: {
        const ConfigOptionStrings* strings = config.opt<ConfigOptionStrings>(pure_key);
        if (strings) {
            if (pure_key == "compatible_printers" || pure_key == "compatible_prints") {
                if (strings->empty())
                    return _L("All");
                for (size_t id = 0; id < strings->size(); id++)
                    out += from_u8(strings->get_at(id)) + "\n";
                out.RemoveLast(1);
                return out;
            }
            if (!strings->empty() && opt_idx < strings->values.size())
                return from_u8(strings->get_at(opt_idx));
        }
        break;
        }
    case coFloatOrPercent: {
        const ConfigOptionFloatOrPercent* opt = config.opt<ConfigOptionFloatOrPercent>(pure_key);
        if (opt)
            out = double_to_string(opt->value) + (opt->percent ? "%" : "");
        return out;
    }
    case coEnum: {
        return get_string_from_enum(pure_key, config,
            pure_key == "top_surface_pattern" ||
            pure_key == "bottom_surface_pattern" ||
            pure_key == "internal_solid_infill_pattern" ||
            pure_key == "sparse_infill_pattern" ||
            pure_key == "ironing_pattern" ||
            pure_key == "support_ironing_pattern" ||
            pure_key == "support_pattern" ||
            pure_key == "support_interface_pattern")
            ;
    }
    case coEnums: {
        return get_string_from_enum(pure_key, config,
            pure_key == "top_surface_pattern" ||
            pure_key == "bottom_surface_pattern" ||
            pure_key == "internal_solid_infill_pattern" ||
            pure_key == "sparse_infill_pattern" ||
            pure_key == "ironing_pattern" ||
            pure_key == "support_ironing_pattern" ||
            pure_key == "support_pattern" ||
            pure_key == "support_interface_pattern"
            , opt_idx);
    }
    case coPoint: {
        Vec2d val = config.opt<ConfigOptionPoint>(pure_key)->value;
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    case coPoints: {
        //BBS: add bed_exclude_area
        if (pure_key == "printable_area" || pure_key == "thumbnails") {
            ConfigOptionPoints points = *config.option<ConfigOptionPoints>(pure_key);
            //BuildVolume build_volume = {points.values, 0.};
            return get_thumbnails_string(points.values);
        }
        else if (pure_key == "bed_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        else if (pure_key == "head_wrap_detect_zone") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        else if (pure_key == "wrapping_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        Vec2d val = config.opt<ConfigOptionPoints>(pure_key)->get_at(opt_idx);
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    default:
        break;
    }
    return out;
}

} // namespace GUI
} // namespace Slic3r
