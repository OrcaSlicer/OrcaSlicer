#include "PlateLook.hpp"

#include <algorithm>
#include <limits>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <wx/image.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Plater.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

namespace {

const char *const CONFIG_SECTION = "plate_look";
constexpr double  SKIN_MAX_PIXELS = 2048.; // the 3D view caps its textures at this size

BoundingBoxf bed_extents(const Preset &printer)
{
    const auto *printable_area = printer.config.opt<ConfigOptionPoints>("printable_area");
    if (printable_area == nullptr)
        return {};
    return get_extents(printable_area->values);
}

std::string bed_size_text(const Preset &printer)
{
    const BoundingBoxf bed = bed_extents(printer);
    if (!bed.defined)
        return {};
    return (boost::format("%1%x%2%") % int(bed.size().x()) % int(bed.size().y())).str();
}

std::string config_key(BedType bed_type, const char *field)
{
    return (boost::format("%1%|%2%|%3%") % plate_look_printer_key() % int(bed_type) % field).str();
}

std::string builtin_icon_name(BedType bed_type, bool big)
{
    const std::string name = bed_type_thumbnail(bed_type);
    return big ? "big_" + name : name;
}

wxBitmap load_fitted_image(const std::string &image_path, const wxSize &box)
{
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(image_path)) || !image.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load plate image %1% failed") % image_path;
        return {};
    }
    const double scale = std::min(double(box.GetWidth()) / image.GetWidth(), double(box.GetHeight()) / image.GetHeight());
    const int width    = std::max(1, int(image.GetWidth() * scale));
    const int height   = std::max(1, int(image.GetHeight() * scale));
    return wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
}

} // namespace

std::string plate_look_printer_key()
{
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};

    const Preset &printer = bundle->printers.get_edited_preset();
    const VendorProfile::PrinterModel *model = wxGetApp().plater()->get_curr_printer_model();
    if (model == nullptr || printer.vendor == nullptr)
        return "preset:" + bundle->printers.get_selected_preset_name();

    return printer.vendor->id + ":" + model->id + ":" + bed_size_text(printer);
}

PlateLook get_plate_look(BedType bed_type)
{
    const AppConfig *config = wxGetApp().app_config;
    return {
        config->get(CONFIG_SECTION, config_key(bed_type, "label")),
        config->get(CONFIG_SECTION, config_key(bed_type, "icon")),
        config->get(CONFIG_SECTION, config_key(bed_type, "skin")),
    };
}

void set_plate_look(BedType bed_type, const PlateLook &look)
{
    AppConfig *config = wxGetApp().app_config;
    const auto store  = [&](const char *field, const std::string &value) {
        if (value.empty())
            config->erase(CONFIG_SECTION, config_key(bed_type, field));
        else
            config->set(CONFIG_SECTION, config_key(bed_type, field), value);
    };
    store("label", look.label);
    store("icon", look.icon_path);
    store("skin", look.skin_path);
}

wxSize skin_pixel_size()
{
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};

    const BoundingBoxf bed = bed_extents(bundle->printers.get_edited_preset());
    if (!bed.defined || bed.size().x() <= 0 || bed.size().y() <= 0)
        return {};

    const double scale = SKIN_MAX_PIXELS / std::max(bed.size().x(), bed.size().y());
    return wxSize(int(bed.size().x() * scale), int(bed.size().y() * scale));
}

wxString builtin_plate_label(BedType bed_type)
{
    const ConfigOptionDef *bed_type_def = print_config_def.get("curr_bed_type");
    const size_t           label_index  = size_t(bed_type) - 1; // btDefault has no label
    if (bed_type_def == nullptr || label_index >= bed_type_def->enum_labels.size())
        return {};
    return _L(bed_type_def->enum_labels[label_index]);
}

wxString plate_label(BedType bed_type)
{
    const std::string custom = get_plate_look(bed_type).label;
    return custom.empty() ? builtin_plate_label(bed_type) : wxString::FromUTF8(custom);
}

wxBitmap icon_bitmap(BedType bed_type, const std::string &icon_path, wxWindow *window, int px, bool big)
{
    if (!icon_path.empty()) {
        wxBitmap bitmap = load_fitted_image(icon_path, wxSize(std::numeric_limits<int>::max(), px));
        if (bitmap.IsOk())
            return bitmap;
    }
    return create_scaled_bitmap(builtin_icon_name(bed_type, big), window, px);
}

wxBitmap plate_icon_bitmap(BedType bed_type, wxWindow *window, int px, bool big)
{
    return icon_bitmap(bed_type, get_plate_look(bed_type).icon_path, window, px, big);
}

wxBitmap skin_preview_bitmap(const std::string &skin_path, const wxSize &box)
{
    if (skin_path.empty())
        return {};
    return load_fitted_image(skin_path, box);
}

}} // namespace Slic3r::GUI
