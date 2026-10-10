#include "ColorPickerDialog.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include <wx/dialog.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/toplevel.h>
#include <wx/frame.h>
#include <wx/nonownedwnd.h>
#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/pen.h>
#include <wx/region.h>
#include <algorithm>
#include <cmath>
#include <wx/webview.h>

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/defs.h>
#include <wx/chartype.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/window.h>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

#include "libslic3r/Color.hpp"
#include <optional>
#include <variant>
#include <cstddef>
#include <stdexcept>
#include <set>
#include <string>
#include <map>
#include <array>
#include <charconv>
#include <system_error>

namespace Slic3r::GUI {

namespace {

using ColorGradient = std::array<ColorRGBA, 2>;

struct ColorPickerFavoritesState {
    std::vector<ColorSelection> favorites;
    bool writable = true;
};

bool normalize_color(ColorRGBA& color, bool allow_alpha)
{
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0.0f || color[i] > 1.0f)
            return false;
    if (!allow_alpha)
        color.a(1.0f);
    return true;
}

bool parse_color(const nlohmann::json& value, ColorRGBA& color)
{
    if (!value.is_string())
        return false;
    const std::string& hex = value.get_ref<const std::string&>();
    if (!can_decode_color(hex))
        return false;
    // decode_color accepts invalid hex digits, so validate before reusing it.
    for (std::size_t i = 1; i < hex.size(); ++i) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return decode_color(hex, color);
}

std::string rgba_hex(const ColorRGBA& color)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string hex(9, '#');
    for (int i = 0; i < 4; ++i) {
        const unsigned int byte = static_cast<unsigned int>(std::lround(color[i] * 255.0f));
        hex[1 + 2 * i] = digits[byte >> 4];
        hex[2 + 2 * i] = digits[byte & 15];
    }
    return hex;
}

std::optional<ColorSelection> normalize_color_selection(const ColorSelection& selection, ColorPickerOptions options)
{
    ColorSelection normalized = selection;
    if (auto* solid = std::get_if<ColorRGBA>(&normalized)) {
        if (!normalize_color(*solid, options.allow_alpha))
            return std::nullopt;
    } else {
        if (!options.allow_gradient)
            return std::nullopt;
        for (ColorRGBA& color : std::get<ColorGradient>(normalized))
            if (!normalize_color(color, options.allow_alpha))
                return std::nullopt;
    }
    return normalized;
}

std::optional<ColorSelection> color_selection_from_json(const nlohmann::json& value, ColorPickerOptions options)
{
    if (!value.is_object())
        return std::nullopt;
    const auto type = value.find("type");
    const auto colors = value.find("colors");
    if (type == value.end() || !type->is_string() || colors == value.end() || !colors->is_array())
        return std::nullopt;
    if (*type == "solid" && colors->size() == 1) {
        ColorRGBA solid;
        if (parse_color((*colors)[0], solid))
            return normalize_color_selection(solid, options);
    } else if (*type == "gradient" && colors->size() == 2 && options.allow_gradient) {
        ColorGradient gradient;
        if (parse_color((*colors)[0], gradient[0]) && parse_color((*colors)[1], gradient[1]))
            return normalize_color_selection(gradient, options);
    }
    return std::nullopt;
}

nlohmann::json color_selection_to_json(const ColorSelection& selection)
{
    const auto normalized = normalize_color_selection(selection, {true, true});
    if (!normalized)
        throw std::invalid_argument("Invalid color selection channels");
    if (const auto* solid = std::get_if<ColorRGBA>(&*normalized))
        return {{"type", "solid"}, {"colors", {rgba_hex(*solid)}}};
    const auto& gradient = std::get<ColorGradient>(*normalized);
    return {{"type", "gradient"}, {"colors", {rgba_hex(gradient[0]), rgba_hex(gradient[1])}}};
}

std::optional<std::vector<ColorSelection>> color_favorites_from_json(const nlohmann::json& values)
{
    if (!values.is_array() || values.size() > 24)
        return std::nullopt;
    std::vector<ColorSelection> favorites;
    favorites.reserve(values.size());
    std::set<std::string> seen;
    for (const auto& value : values) {
        const auto selection = color_selection_from_json(value, {true, true});
        if (!selection)
            return std::nullopt;
        if (seen.insert(color_selection_to_json(*selection).dump()).second)
            favorites.push_back(*selection);
    }
    return favorites;
}

std::optional<ColorSelection> legacy_color(const std::string& value)
{
    if (!value.empty() && value.front() == '#')
        return color_selection_from_json({{"type", "solid"}, {"colors", {value}}}, {false, true});

    // string_to_wxColor uses stoi without checking the consumed length or byte
    // range. Parse the actual color_to_string RGBA format strictly for migration.
    std::array<unsigned int, 4> bytes;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto comma = value.find(',', begin);
        if ((i < 3 && comma == std::string::npos) || (i == 3 && comma != std::string::npos))
            return std::nullopt;
        const auto end = comma == std::string::npos ? value.size() : comma;
        const auto parsed = std::from_chars(value.data() + begin, value.data() + end, bytes[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + end || bytes[i] > 255)
            return std::nullopt;
        begin = end + 1;
    }
    return ColorRGBA(static_cast<unsigned char>(bytes[0]), static_cast<unsigned char>(bytes[1]),
                     static_cast<unsigned char>(bytes[2]), static_cast<unsigned char>(bytes[3]));
}
bool save_color_picker_favorites(AppConfig& config, const nlohmann::json& values)
{
    if (config.has_section("color_picker") && config.get("color_picker", "version") != "1")
        return false;
    const auto favorites = color_favorites_from_json(values);
    if (!favorites)
        return false;
    nlohmann::json canonical = nlohmann::json::array();
    for (const auto& selection : *favorites)
        canonical.push_back(color_selection_to_json(selection));
    // set_section is a raw replacement and does not mark AppConfig dirty itself.
    config.set_section("color_picker", {{"version", "1"}, {"favorites", canonical.dump()}});
    config.set_dirty();
    config.save();
    return true;
}

ColorPickerFavoritesState load_color_picker_favorites(AppConfig& config)
{
    if (config.has_section("color_picker")) {
        if (config.get("color_picker", "version") != "1")
            return {{}, false};
        const auto values = nlohmann::json::parse(config.get("color_picker", "favorites"), nullptr, false);
        const auto favorites = color_favorites_from_json(values);
        return {favorites ? *favorites : std::vector<ColorSelection>{}, true};
    }

    nlohmann::json imported = nlohmann::json::array();
    for (const std::string& value_string : config.get_custom_color_from_config()) {
        const auto selection = legacy_color(value_string);
        if (!selection)
            continue;
        const auto value = color_selection_to_json(*selection);
        bool duplicate = false;
        for (const auto& existing : imported)
            if (existing == value) {
                duplicate = true;
                break;
            }
        if (!duplicate)
            imported.push_back(value);
        if (imported.size() == 24)
            break;
    }
    // Persist the version even for an empty import: clearing never reimports.
    save_color_picker_favorites(config, imported);
    return {*color_favorites_from_json(imported), true};
}

} // namespace

std::optional<ColorSelection> filament_color_picker_selection(const FilamentColorPickerValue& value)
{
    return color_selection_from_json({{"type", value.gradient ? "gradient" : "solid"}, {"colors", value.colors}}, {true, true});
}

ColorSelection filament_color_picker_initial(const FilamentColorPickerValue& value)
{
    if (const auto exact = filament_color_picker_selection(value))
        return *exact;
    if (!value.colors.empty()) {
        if (const auto primary = color_selection_from_json({{"type", "solid"}, {"colors", {value.colors.front()}}}, {false, true}))
            return *primary;
    }
    if (const auto primary = color_selection_from_json({{"type", "solid"}, {"colors", {value.primary}}}, {false, true}))
        return *primary;
    return ColorRGBA(0.f, 0.f, 0.f, 1.f);
}

FilamentColorPickerValue filament_color_picker_result(const ColorSelection& selection)
{
    // Keep legacy RGB spelling for opaque colors; retain RGBA for transparency.
    const auto canonical = color_selection_to_json(selection);
    FilamentColorPickerValue value;
    value.gradient = canonical.at("type") == "gradient";
    for (const auto& color : canonical.at("colors")) {
        auto hex = color.get<std::string>();
        if (hex.compare(7, 2, "FF") == 0) hex.resize(7);
        value.colors.push_back(std::move(hex));
    }
    return value;
}

bool filament_color_picker_changed(const ColorSelection& initial, const std::optional<ColorSelection>& result)
{
    // Compare encoded bytes, not ColorRGBA's tolerant floating point equality.
    // An unchanged seed also leaves unrepresentable multi-color data untouched.
    return result && color_selection_to_json(initial) != color_selection_to_json(*result);
}

namespace {

// English palette names are stable translation keys; RAL codes stay in the page data.
nlohmann::json color_picker_ui_strings()
{
    return {
        {"Color Picker", _u8L("Color Picker")},
        {"Basic Colors", _u8L("Basic Colors")},
        // TRN: Keep the brand name "Orca" unchanged; translate only the palette descriptor.
        {"Orca Palette", _u8L("Orca Palette")},
        // TRN: Keep the standard/brand name "RAL" unchanged.
        {"RAL Classic", _u8L("RAL Classic")},
        {"Gradients", _u8L("Gradients")},
        {"Color", _u8L("Color")},
        {"Gradient", _u8L("Gradient")},
        {"Hex color", _u8L("Hex color")},
        {"Opacity", _u8L("Opacity")},
        {"Opacity percent", _u8L("Opacity percent")},
        {"Save color", _u8L("Save color")},
        {"Cancel", _u8L("Cancel")},
        {"OK", _u8L("OK")},
        {"Enter a complete hexadecimal color", _u8L("Enter a complete hexadecimal color")},
        {"Invalid input", _u8L("Invalid input")},
        {"Value is out of range.", _u8L("Value is out of range.")},
        {"The current multi-color selection is kept until you choose a different color.", _u8L("The current multi-color selection is kept until you choose a different color.")},
        {"Red", _u8L("Red")},
        {"Crimson", _u8L("Crimson")},
        {"Orange red", _u8L("Orange red")},
        {"Tomato", _u8L("Tomato")},
        {"Orange", _u8L("Orange")},
        {"Dark orange", _u8L("Dark orange")},
        {"Gold", _u8L("Gold")},
        {"Burly wood", _u8L("Burly wood")},
        {"Bisque", _u8L("Bisque")},
        {"Misty rose", _u8L("Misty rose")},
        {"Yellow", _u8L("Yellow")},
        {"Light yellow", _u8L("Light yellow")},
        {"Lemon chiffon", _u8L("Lemon chiffon")},
        {"Beige", _u8L("Beige")},
        {"Light golden rod yellow", _u8L("Light golden rod yellow")},
        {"Pale golden rod", _u8L("Pale golden rod")},
        {"Khaki", _u8L("Khaki")},
        {"Dark khaki", _u8L("Dark khaki")},
        {"Peach puff", _u8L("Peach puff")},
        {"Moccasin", _u8L("Moccasin")},
        {"Papaya whip", _u8L("Papaya whip")},
        {"Blanched almond", _u8L("Blanched almond")},
        {"Antique white", _u8L("Antique white")},
        {"Navajo white", _u8L("Navajo white")},
        {"Wheat", _u8L("Wheat")},
        {"Golden rod", _u8L("Golden rod")},
        {"Chocolate", _u8L("Chocolate")},
        {"Peru", _u8L("Peru")},
        {"Fire brick", _u8L("Fire brick")},
        {"Sienna", _u8L("Sienna")},
        {"Saddle brown", _u8L("Saddle brown")},
        {"Brown", _u8L("Brown")},
        {"Rosy brown", _u8L("Rosy brown")},
        {"Salmon", _u8L("Salmon")},
        {"Light salmon", _u8L("Light salmon")},
        {"Dark salmon", _u8L("Dark salmon")},
        {"Sandy brown", _u8L("Sandy brown")},
        {"Coral", _u8L("Coral")},
        {"Light coral", _u8L("Light coral")},
        {"Indian red", _u8L("Indian red")},
        {"Dark red", _u8L("Dark red")},
        {"Maroon", _u8L("Maroon")},
        {"Green yellow", _u8L("Green yellow")},
        {"Yellow green", _u8L("Yellow green")},
        {"Chartreuse", _u8L("Chartreuse")},
        {"Lawn green", _u8L("Lawn green")},
        {"Lime green", _u8L("Lime green")},
        {"Spring green", _u8L("Spring green")},
        {"Medium spring green", _u8L("Medium spring green")},
        {"Lime", _u8L("Lime")},
        {"Green", _u8L("Green")},
        {"Forest green", _u8L("Forest green")},
        {"Dark green", _u8L("Dark green")},
        {"Olive drab", _u8L("Olive drab")},
        {"Olive", _u8L("Olive")},
        {"Dark olive green", _u8L("Dark olive green")},
        {"Pale green", _u8L("Pale green")},
        {"Light green", _u8L("Light green")},
        {"Medium sea green", _u8L("Medium sea green")},
        {"Sea green", _u8L("Sea green")},
        {"Dark sea green", _u8L("Dark sea green")},
        {"Cadet blue", _u8L("Cadet blue")},
        {"Medium aqua marine", _u8L("Medium aqua marine")},
        {"Turquoise", _u8L("Turquoise")},
        {"Medium turquoise", _u8L("Medium turquoise")},
        {"Dark turquoise", _u8L("Dark turquoise")},
        {"Pale turquoise", _u8L("Pale turquoise")},
        {"Aqua", _u8L("Aqua")},
        {"Cyan", _u8L("Cyan")},
        {"Light cyan", _u8L("Light cyan")},
        {"Mint cream", _u8L("Mint cream")},
        {"Honey dew", _u8L("Honey dew")},
        {"Aquamarine", _u8L("Aquamarine")},
        {"Light sea green", _u8L("Light sea green")},
        {"Dark cyan", _u8L("Dark cyan")},
        {"Teal", _u8L("Teal")},
        {"Dark slate gray", _u8L("Dark slate gray")},
        {"Light slate gray", _u8L("Light slate gray")},
        {"Slate gray", _u8L("Slate gray")},
        {"Deep sky blue", _u8L("Deep sky blue")},
        {"Dodger Blue", _u8L("Dodger Blue")},
        {"Sky blue", _u8L("Sky blue")},
        {"Light sky blue", _u8L("Light sky blue")},
        {"Light blue", _u8L("Light blue")},
        {"Powder blue", _u8L("Powder blue")},
        {"Light steel blue", _u8L("Light steel blue")},
        {"Steel blue", _u8L("Steel blue")},
        {"Cornflower blue", _u8L("Cornflower blue")},
        {"Royal blue", _u8L("Royal blue")},
        {"Blue", _u8L("Blue")},
        {"Medium blue", _u8L("Medium blue")},
        {"Midnight blue", _u8L("Midnight blue")},
        {"Dark blue", _u8L("Dark blue")},
        {"Navy", _u8L("Navy")},
        {"Slate blue", _u8L("Slate blue")},
        {"Dark slate blue", _u8L("Dark slate blue")},
        {"Medium slate blue", _u8L("Medium slate blue")},
        {"Blue violet", _u8L("Blue violet")},
        {"Indigo", _u8L("Indigo")},
        {"Dark violet", _u8L("Dark violet")},
        {"Dark orchid", _u8L("Dark orchid")},
        {"Medium orchid", _u8L("Medium orchid")},
        {"Orchid", _u8L("Orchid")},
        {"Violet", _u8L("Violet")},
        {"Medium purple", _u8L("Medium purple")},
        {"Purple", _u8L("Purple")},
        {"Rebecca purple", _u8L("Rebecca purple")},
        {"Medium violet red", _u8L("Medium violet red")},
        {"PaleV violet red", _u8L("PaleV violet red")},
        {"Deep pink", _u8L("Deep pink")},
        {"Hot pink", _u8L("Hot pink")},
        {"Light pink", _u8L("Light pink")},
        {"Pink", _u8L("Pink")},
        {"Plum", _u8L("Plum")},
        {"Thistle", _u8L("Thistle")},
        {"Magenta", _u8L("Magenta")},
        {"Fuchsia", _u8L("Fuchsia")},
        {"Dark magenta", _u8L("Dark magenta")},
        {"Linen", _u8L("Linen")},
        {"Old lace", _u8L("Old lace")},
        {"Sea shell", _u8L("Sea shell")},
        {"Ivory", _u8L("Ivory")},
        {"Floral white", _u8L("Floral white")},
        {"Lavender blush", _u8L("Lavender blush")},
        {"Ghost white", _u8L("Ghost white")},
        {"Alice blue", _u8L("Alice blue")},
        {"Lavender", _u8L("Lavender")},
        {"White smoke", _u8L("White smoke")},
        {"Gainsboro", _u8L("Gainsboro")},
        {"Black", _u8L("Black")},
        {"White", _u8L("White")},
        {"Coral fire", _u8L("Coral fire")},
        {"Sea anemone", _u8L("Sea anemone")},
        {"Deep abyss", _u8L("Deep abyss")},
        {"Lobster red", _u8L("Lobster red")},
        {"Sandy clay", _u8L("Sandy clay")},
        {"Driftwood", _u8L("Driftwood")},
        {"Sea buckthorn", _u8L("Sea buckthorn")},
        {"Sunset reef", _u8L("Sunset reef")},
        {"Tiger coral", _u8L("Tiger coral")},
        {"Pink coral", _u8L("Pink coral")},
        {"Seashell glow", _u8L("Seashell glow")},
        {"Sand", _u8L("Sand")},
        {"Golden kelp", _u8L("Golden kelp")},
        {"Sunlit dune", _u8L("Sunlit dune")},
        {"Amber nautilus", _u8L("Amber nautilus")},
        {"Sea lemon", _u8L("Sea lemon")},
        {"Pale starfish", _u8L("Pale starfish")},
        {"Spring tide", _u8L("Spring tide")},
        {"Lime algae", _u8L("Lime algae")},
        {"Sea grass", _u8L("Sea grass")},
        {"Neon plankton", _u8L("Neon plankton")},
        {"Kelp forest", _u8L("Kelp forest")},
        {"Emerald wave", _u8L("Emerald wave")},
        {"Whale shadow", _u8L("Whale shadow")},
        {"Sea turtle", _u8L("Sea turtle")},
        {"Lagoon", _u8L("Lagoon")},
        {"Tropical current", _u8L("Tropical current")},
        {"Midnight trench", _u8L("Midnight trench")},
        {"Dark kelp", _u8L("Dark kelp")},
        {"Crystal surf", _u8L("Crystal surf")},
        {"Turquoise bay", _u8L("Turquoise bay")},
        {"Sky over reef", _u8L("Sky over reef")},
        {"Shallow sky", _u8L("Shallow sky")},
        {"Electric bel", _u8L("Electric bel")},
        {"Dolphin", _u8L("Dolphin")},
        {"Ocean depths", _u8L("Ocean depths")},
        {"Whale", _u8L("Whale")},
        {"Indigo abyss", _u8L("Indigo abyss")},
        {"Sea urchin", _u8L("Sea urchin")},
        {"Lavender coral", _u8L("Lavender coral")},
        {"Violet tide", _u8L("Violet tide")},
        {"Deep sea orchid", _u8L("Deep sea orchid")},
        {"Neon jelly", _u8L("Neon jelly")},
        {"Fuchsia coral", _u8L("Fuchsia coral")},
        {"Pink jellyfish", _u8L("Pink jellyfish")},
        {"Foam", _u8L("Foam")},
        {"Seashell white", _u8L("Seashell white")},
        {"Pebble gray", _u8L("Pebble gray")},
        {"Storm cloud", _u8L("Storm cloud")},
        {"Ink black", _u8L("Ink black")},
        {"Green beige", _u8L("Green beige")},
        {"Sand yellow", _u8L("Sand yellow")},
        {"Signal yellow", _u8L("Signal yellow")},
        {"Golden yellow", _u8L("Golden yellow")},
        {"Honey yellow", _u8L("Honey yellow")},
        {"Maize yellow", _u8L("Maize yellow")},
        {"Dalmatian yellow", _u8L("Dalmatian yellow")},
        {"Brown beige", _u8L("Brown beige")},
        {"Lemon yellow", _u8L("Lemon yellow")},
        {"Oyster white", _u8L("Oyster white")},
        {"Light ivory", _u8L("Light ivory")},
        {"Zinc yellow", _u8L("Zinc yellow")},
        {"Grey beige", _u8L("Grey beige")},
        {"Olive yellow", _u8L("Olive yellow")},
        {"Rape yellow", _u8L("Rape yellow")},
        {"Traffic yellow", _u8L("Traffic yellow")},
        {"Ochre yellow", _u8L("Ochre yellow")},
        {"Traffic orange yellow", _u8L("Traffic orange yellow")},
        {"Curry", _u8L("Curry")},
        {"Melon yellow", _u8L("Melon yellow")},
        {"Broom yellow", _u8L("Broom yellow")},
        {"Dahlia yellow", _u8L("Dahlia yellow")},
        {"Pastel yellow", _u8L("Pastel yellow")},
        {"Pearl beige", _u8L("Pearl beige")},
        {"Pearl gold", _u8L("Pearl gold")},
        {"Sun yellow", _u8L("Sun yellow")},
        {"Yellow orange", _u8L("Yellow orange")},
        {"Deep orange", _u8L("Deep orange")},
        {"Vermilion", _u8L("Vermilion")},
        {"Pearl orange", _u8L("Pearl orange")},
        {"Pure orange", _u8L("Pure orange")},
        {"Luminous orange", _u8L("Luminous orange")},
        {"Luminous bright orange", _u8L("Luminous bright orange")},
        {"Bright red orange", _u8L("Bright red orange")},
        {"Traffic orange", _u8L("Traffic orange")},
        {"Signal orange", _u8L("Signal orange")},
        {"Salmon orange", _u8L("Salmon orange")},
        {"Pearl pink", _u8L("Pearl pink")},
        {"Copper orange", _u8L("Copper orange")},
        {"Signal brown", _u8L("Signal brown")},
        {"Fir orange", _u8L("Fir orange")},
        {"Signal red", _u8L("Signal red")},
        {"Luminous red", _u8L("Luminous red")},
        {"Carmine red", _u8L("Carmine red")},
        {"Pearl carmine red", _u8L("Pearl carmine red")},
        {"Pure red", _u8L("Pure red")},
        {"Luminous deep red", _u8L("Luminous deep red")},
        {"Currant red", _u8L("Currant red")},
        {"Pearl ruby red", _u8L("Pearl ruby red")},
        {"Fire red", _u8L("Fire red")},
        {"Ruby red", _u8L("Ruby red")},
        {"Purple red", _u8L("Purple red")},
        {"Wine red", _u8L("Wine red")},
        {"Black red", _u8L("Black red")},
        {"Oxide red", _u8L("Oxide red")},
        {"Brown red", _u8L("Brown red")},
        {"Pine red", _u8L("Pine red")},
        {"Tomato red", _u8L("Tomato red")},
        {"Antique pink", _u8L("Antique pink")},
        {"Coral red", _u8L("Coral red")},
        {"Rose", _u8L("Rose")},
        {"Strawberry red", _u8L("Strawberry red")},
        {"Traffic red", _u8L("Traffic red")},
        {"Salmon pink", _u8L("Salmon pink")},
        {"Luminous rose", _u8L("Luminous rose")},
        {"Luminous bright red", _u8L("Luminous bright red")},
        {"Raspberry red", _u8L("Raspberry red")},
        {"Orient red", _u8L("Orient red")},
        {"Pearl ruby", _u8L("Pearl ruby")},
        {"Red purple", _u8L("Red purple")},
        {"Red violet", _u8L("Red violet")},
        {"Heather violet", _u8L("Heather violet")},
        {"Bordeaux violet", _u8L("Bordeaux violet")},
        {"Mahogany", _u8L("Mahogany")},
        {"Grey purple", _u8L("Grey purple")},
        {"Signal violet", _u8L("Signal violet")},
        {"Fir green", _u8L("Fir green")},
        {"Sabine grey", _u8L("Sabine grey")},
        {"Pearl violet", _u8L("Pearl violet")},
        {"Pearl blackberry", _u8L("Pearl blackberry")},
        {"Green blue", _u8L("Green blue")},
        {"Ultramarine blue", _u8L("Ultramarine blue")},
        {"Sapphire blue", _u8L("Sapphire blue")},
        {"Black blue", _u8L("Black blue")},
        {"Signal blue", _u8L("Signal blue")},
        {"Brilliant blue", _u8L("Brilliant blue")},
        {"Grey blue", _u8L("Grey blue")},
        {"Azure blue", _u8L("Azure blue")},
        {"Gentian blue", _u8L("Gentian blue")},
        {"Cobalt blue", _u8L("Cobalt blue")},
        {"Pigeon blue", _u8L("Pigeon blue")},
        {"Traffic blue", _u8L("Traffic blue")},
        {"Turquoise blue", _u8L("Turquoise blue")},
        {"Capsanthin violet", _u8L("Capsanthin violet")},
        {"Ocean blue", _u8L("Ocean blue")},
        {"Water blue", _u8L("Water blue")},
        {"Night blue", _u8L("Night blue")},
        {"Distant blue", _u8L("Distant blue")},
        {"Pastel blue", _u8L("Pastel blue")},
        {"Pearl gentian blue", _u8L("Pearl gentian blue")},
        {"Pearl midnight blue", _u8L("Pearl midnight blue")},
        {"Patina green", _u8L("Patina green")},
        {"Emerald green", _u8L("Emerald green")},
        {"Leaf green", _u8L("Leaf green")},
        {"Olive green", _u8L("Olive green")},
        {"Blue green", _u8L("Blue green")},
        {"Moss green", _u8L("Moss green")},
        {"Grey olive", _u8L("Grey olive")},
        {"Pine green", _u8L("Pine green")},
        {"Pine brown", _u8L("Pine brown")},
        {"Grass green", _u8L("Grass green")},
        {"Reseda green", _u8L("Reseda green")},
        {"Brown green", _u8L("Brown green")},
        {"Reed green", _u8L("Reed green")},
        {"Black green", _u8L("Black green")},
        {"Turquoise green", _u8L("Turquoise green")},
        {"May green", _u8L("May green")},
        {"Chrome oxide green", _u8L("Chrome oxide green")},
        {"Traffic green", _u8L("Traffic green")},
        {"Fern green", _u8L("Fern green")},
        {"Opal green", _u8L("Opal green")},
        {"Mint green", _u8L("Mint green")},
        {"Bronze green", _u8L("Bronze green")},
        {"Pearl green", _u8L("Pearl green")},
        {"Sax turquoise", _u8L("Sax turquoise")},
        {"Pearl turquoise", _u8L("Pearl turquoise")},
        {"Pearl jade", _u8L("Pearl jade")},
        {"Pure green", _u8L("Pure green")},
        {"Luminous green", _u8L("Luminous green")},
        {"Squirrel grey", _u8L("Squirrel grey")},
        {"Silver grey", _u8L("Silver grey")},
        {"Olivestone", _u8L("Olivestone")},
        {"Moss grey", _u8L("Moss grey")},
        {"Signal grey", _u8L("Signal grey")},
        {"Mouse grey", _u8L("Mouse grey")},
        {"Beige grey", _u8L("Beige grey")},
        {"Khaki grey", _u8L("Khaki grey")},
        {"Green grey", _u8L("Green grey")},
        {"Tarpaulin grey", _u8L("Tarpaulin grey")},
        {"Iron grey", _u8L("Iron grey")},
        {"Basalt grey", _u8L("Basalt grey")},
        {"Brown grey", _u8L("Brown grey")},
        {"Slate grey", _u8L("Slate grey")},
        {"Anthracite grey", _u8L("Anthracite grey")},
        {"Light grey", _u8L("Light grey")},
        {"Umber grey", _u8L("Umber grey")},
        {"Concrete grey", _u8L("Concrete grey")},
        {"Graphite grey", _u8L("Graphite grey")},
        {"Granite grey", _u8L("Granite grey")},
        {"Stone grey", _u8L("Stone grey")},
        {"Pebble grey", _u8L("Pebble grey")},
        {"Cement grey", _u8L("Cement grey")},
        {"Yellow grey", _u8L("Yellow grey")},
        {"Platinum grey", _u8L("Platinum grey")},
        {"Dusty grey", _u8L("Dusty grey")},
        {"Agate grey", _u8L("Agate grey")},
        {"Quartz grey", _u8L("Quartz grey")},
        {"Window grey", _u8L("Window grey")},
        {"Traffic grey A", _u8L("Traffic grey A")},
        {"Traffic grey B", _u8L("Traffic grey B")},
        {"Silk grey", _u8L("Silk grey")},
        {"Telegrey 1", _u8L("Telegrey 1")},
        {"Telegrey 2", _u8L("Telegrey 2")},
        {"Telegrey 4", _u8L("Telegrey 4")},
        {"Pearl mouse grey", _u8L("Pearl mouse grey")},
        {"Green brown", _u8L("Green brown")},
        {"Ochre brown", _u8L("Ochre brown")},
        {"Clay brown", _u8L("Clay brown")},
        {"Copper brown", _u8L("Copper brown")},
        {"Chocolate brown", _u8L("Chocolate brown")},
        {"Olive brown", _u8L("Olive brown")},
        {"Nut brown", _u8L("Nut brown")},
        {"Red brown", _u8L("Red brown")},
        {"Sepia brown", _u8L("Sepia brown")},
        {"Chestnut brown", _u8L("Chestnut brown")},
        {"Mahogany brown", _u8L("Mahogany brown")},
        {"Grey brown", _u8L("Grey brown")},
        {"Pearl copper", _u8L("Pearl copper")},
        {"Beige brown", _u8L("Beige brown")},
        {"Pearl mocha brown", _u8L("Pearl mocha brown")},
        {"Earth brown", _u8L("Earth brown")},
        {"Pearl brown", _u8L("Pearl brown")},
        {"Cream", _u8L("Cream")},
        {"Grey white", _u8L("Grey white")},
        {"Signal white", _u8L("Signal white")},
        {"Signal black", _u8L("Signal black")},
        {"Jet black", _u8L("Jet black")},
        {"White aluminium", _u8L("White aluminium")},
        {"Grey aluminium", _u8L("Grey aluminium")},
        {"Pure white", _u8L("Pure white")},
        {"Graphite black", _u8L("Graphite black")},
        {"Traffic white", _u8L("Traffic white")},
        {"Traffic black", _u8L("Traffic black")},
        {"Papyrus white", _u8L("Papyrus white")},
        {"Pearl light grey", _u8L("Pearl light grey")},
        {"Pearl", _u8L("Pearl")},
        {"Silver", _u8L("Silver")},
        {"Copper", _u8L("Copper")},
        {"Brass", _u8L("Brass")}
    };
}

} // namespace


namespace {

// All geometry uses native screen coordinates, including negative monitor origins.
wxPoint color_picker_panel_position(const wxRect& anchor, const wxRect& work_area, const wxSize& size, int gap)
{
    const int below = anchor.y + anchor.height + gap;
    const int above = anchor.y - gap - size.y;
    const int y = below + size.y <= work_area.y + work_area.height ? below : above;
    return {std::clamp(anchor.x, work_area.x, work_area.x + std::max(0, work_area.width - size.x)),
            std::clamp(y, work_area.y, work_area.y + std::max(0, work_area.height - size.y))};
}

} // namespace

ColorPickerDialog::ColorPickerDialog(wxWindow* parent, const ColorSelection& initial, ColorPickerOptions options,
                                   bool preserve_multi_color, wxWindow* anchor)
    : WebViewHostDialog(parent, wxID_ANY, _L("Color Picker"), wxDefaultPosition, wxDefaultSize,
                        wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_SHAPED),
      m_initial(initial), m_options(options), m_preserve_multi_color(preserve_multi_color)
{
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { finish(wxID_CANCEL); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE)
            finish(wxID_CANCEL);
        else
            event.Skip();
    });
    SetBackgroundColour(wxGetApp().get_window_default_clr());
#ifdef __WXGTK__
    // wxGTK dialogs default to CENTER_ON_PARENT, which conflicts with the anchor.
    gtk_window_set_position(GTK_WINDOW(GetHandle()), GTK_WIN_POS_NONE);
#endif
    // Snapshot the trigger instead of following the mouse or retaining a raw pointer.
    wxWindow* trigger = anchor ? anchor : parent;
    if (trigger)
        m_anchor_rect = trigger->GetScreenRect();
    int display_index = wxDisplay::GetFromPoint(m_anchor_rect.GetPosition() + wxPoint(m_anchor_rect.width / 2, m_anchor_rect.height / 2));
    if (display_index == wxNOT_FOUND)
        display_index = 0;
    m_work_area = wxDisplay(static_cast<unsigned int>(display_index)).GetClientArea();
    const auto normalized = normalize_color_selection(initial, options);
    if (normalized)
        m_initial = *normalized;
    if (!normalized || !create_webview("web/dialog/ColorPickerDialog/index.html", _L("Color Picker"),
                                      wxSize(550, 520), wxSize(550, 300))) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, wxS("wxWebView unavailable")), wxSizerFlags().Border(wxALL, FromDIP(20)));
        sizer->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), wxSizerFlags().Right().Border(wxALL, FromDIP(8)));
        SetSizerAndFit(sizer);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { finish(wxID_CANCEL); }, wxID_CANCEL);
        Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
            if (event.IsShown()) {
                move_to_anchor();
                apply_rounded_shape();
                wxGetApp().CallAfter([this, alive = m_alive] {
                    if (alive->load(std::memory_order_acquire) && !m_closing)
                        move_to_anchor();
                });
            }
            event.Skip();
        });
        move_to_anchor();
        return;
    }
    browser()->EnableBrowserAcceleratorKeys(false);
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (event.GetActive() && IsShown() && !m_closing)
            focus_webview();
        event.Skip();
    });
    if (wxGetApp().app_config) {
        const auto stored = load_color_picker_favorites(*wxGetApp().app_config);
        m_favorites = stored.favorites;
        m_favorites_writable = stored.writable;
    }
    // The shared host attaches its sizer before constructing the full layout.
    GetSizer()->SetSizeHints(this);
    Move(m_work_area.GetPosition());
    // SetSizeHints may fit the window to the WebView's small initial best size.
    // Restore the intended dialog size only after applying the layout hints.
    resize_to_content(m_content_height);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        apply_rounded_shape();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) {
            resize_to_content(m_content_height);
            // GTK can require a realized window before applying its shape.
            wxGetApp().CallAfter([this, alive = m_alive] {
                if (alive->load(std::memory_order_acquire) && !m_closing) {
                    resize_to_content(m_content_height);
                    focus_webview();
                }
            });
        }
        event.Skip();
    });
    apply_rounded_shape();
}

ColorPickerDialog::~ColorPickerDialog() { m_alive->store(false, std::memory_order_release); }

void ColorPickerDialog::position_panel()
{
    if (m_positioning)
        return;
    m_positioning = true;
    wxSize size = FromDIP(wxSize(550, m_content_height));
    size.x = std::min(size.x, m_work_area.width);
    size.y = std::min(size.y, std::max(1, m_work_area.height - FromDIP(16)));
    SetMinSize(wxSize(std::min(FromDIP(550), size.x), std::min(FromDIP(300), size.y)));
    if (size != GetClientSize())
        SetClientSize(size);
    move_to_anchor();
    m_positioning = false;
}

void ColorPickerDialog::move_to_anchor()
{
    const wxPoint position = color_picker_panel_position(m_anchor_rect, m_work_area, GetSize(), FromDIP(4));
    if (position != GetPosition())
        Move(position);
#ifdef __WXGTK__
    // wxGTK caches requested coordinates: Move can skip a post-show request even
    // when the window manager ignored the initial one. Reapply at native level.
    gtk_window_move(GTK_WINDOW(GetHandle()), position.x, position.y);
#endif
}

void ColorPickerDialog::resize_to_content(int height)
{
    m_content_height = height;
    position_panel();
    Layout();
#ifdef __WXOSX__
    // Like SpeedDial, explicitly match WKWebView's viewport even at unchanged size.
    if (wxWebView* view = browser())
        view->SetSize(GetClientSize());
#endif
    apply_rounded_shape();
    repaint_webview();
}

void ColorPickerDialog::focus_webview()
{
    wxWebView* view = browser();
    if (!view)
        return;
#ifdef __linux__
    if (void* backend = view->GetNativeBackend())
        gtk_widget_grab_focus(static_cast<GtkWidget*>(backend));
#else
    view->SetFocus();
#endif
    if (m_page_ready)
        run_script("window.ColorPickerDialog.focusInput();");
}

void ColorPickerDialog::repaint_webview()
{
    wxWebView* view = browser();
    if (!view)
        return;
    view->Refresh();
#ifdef __WXOSX__
    if (void* backend = view->GetNativeBackend())
        WKWebView_force_display(backend);
    view->Update();
#elif defined(__linux__)
    if (void* backend = view->GetNativeBackend())
        gtk_widget_queue_draw(static_cast<GtkWidget*>(backend));
#else
    view->Update();
#endif
}

void ColorPickerDialog::on_dpi_changed(const wxRect&)
{
    resize_to_content(m_content_height);
    Refresh();
}

void ColorPickerDialog::apply_rounded_shape()
{
    // wxOSX SetShape resizes NSWindow and synchronously fires wxEVT_SIZE.
    // Like SpeedDial, clip its native layer instead and guard shape re-entry.
    if (m_applying_shape)
        return;
    const wxSize size = GetClientSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    m_applying_shape = true;
#ifdef __WXOSX__
    // Reapply after showing: the native view layer may not exist at construction.
    set_window_corner_radius(this, FromDIP(m_corner_radius));
#else
    m_shape_bmp.Create(size.x, size.y, 32);
    if (m_shape_bmp.IsOk()) {
        wxMemoryDC dc(m_shape_bmp);
        if (dc.IsOk()) {
            dc.SetBackground(wxBrush(*wxBLACK));
            dc.Clear();
            dc.SetBrush(wxBrush(*wxWHITE));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(m_corner_radius));
            dc.SelectObject(wxNullBitmap);
            const wxRegion region(m_shape_bmp, *wxBLACK);
            if (region.IsOk())
                SetShape(region);
        }
    }
#endif
    m_applying_shape = false;
}

void ColorPickerDialog::add_user_scripts()
{
    if (wxWebView* view = browser()) {
        auto language = wxGetApp().current_language_code().ToStdString();
        std::replace(language.begin(), language.end(), '_', '-');
        const std::string script = "window.ORCA_UI_STRINGS = " +
            color_picker_ui_strings().dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore) +
            ";window.ORCA_COLOR_PICKER_LANGUAGE = " + nlohmann::json(language).dump() + ";";
        view->AddUserScript(wxString::FromUTF8(script));
    }
}

void ColorPickerDialog::on_script_message(const nlohmann::json& payload)
{
    // GTK/WebKit may call on the native script-message stack. All dialog work,
    // including EndModal, runs after that callback and checks the lifetime token.
    wxGetApp().CallAfter([this, alive = m_alive, payload] {
        if (alive->load(std::memory_order_acquire) && !m_closing)
            handle_web_command(payload);
    });
}

void ColorPickerDialog::handle_web_command(const nlohmann::json& payload)
{
    if (!browser() || !payload.is_object())
        return;
    const auto command = payload.find("command");
    const auto page_id = payload.find("page_id");
    if (command == payload.end() || !command->is_string() || page_id == payload.end() || !page_id->is_string())
        return;
    const auto& id = page_id->get_ref<const std::string&>();
    if (id.empty() || id.size() > 64)
        return;
    if (*command == "ready") {
        // A repeated ready from the same document must not reset its edits. A new
        // document (e.g. WebView recreation) gets a fresh initialization handshake.
        if (id != m_page_id) {
            m_page_id = id;
            m_page_ready = false;
            m_init_sent = false;
        }
        if (!m_init_sent)
            send_initial_state();
        return;
    }
    if (id != m_page_id)
        return;
    if (*command == "cancel") {
        finish(wxID_CANCEL);
        return;
    }
    if (*command == "initialized") {
        if (m_init_sent && !m_page_ready) {
            m_page_ready = true;
            focus_webview();
            repaint_webview();
        }
        return;
    }
    if (!m_page_ready)
        return;
    if (*command == "resize") {
        const auto height = payload.find("height");
        if (height == payload.end() || !height->is_number())
            return;
        const double value = height->get<double>();
        if (!std::isfinite(value) || value < 300 || value > 900)
            return;
        resize_to_content(static_cast<int>(std::ceil(value)));
        return;
    }
    if (*command == "confirm") {
        const auto value = payload.find("selection");
        if (value == payload.end())
            return;
        const auto selection = color_selection_from_json(*value, m_options);
        if (!selection)
            return;
        m_selection = *selection;
        finish(wxID_OK);
    } else if (*command == "update_favorites") {
        const auto values = payload.find("favorites");
        if (values == payload.end() || !m_favorites_writable || !wxGetApp().app_config)
            return;
        const auto favorites = color_favorites_from_json(*values);
        if (favorites && save_color_picker_favorites(*wxGetApp().app_config, *values)) {
            m_favorites = *favorites;
            nlohmann::json canonical = nlohmann::json::array();
            for (const ColorSelection& selection : m_favorites)
                canonical.push_back(color_selection_to_json(selection));
            const nlohmann::json response = {{"command", "favorites"}, {"page_id", m_page_id}, {"favorites", std::move(canonical)}};
            run_script(wxString::FromUTF8("window.ColorPickerDialog.handleMessage(" + response.dump() + ")"));
        }
    }
}

void ColorPickerDialog::send_initial_state()
{
    nlohmann::json favorites = nlohmann::json::array();
    for (const ColorSelection& selection : m_favorites)
        favorites.push_back(color_selection_to_json(selection));
    const nlohmann::json payload = {{"command", "init"}, {"page_id", m_page_id},
                                   {"options", {{"allow_gradient", m_options.allow_gradient}, {"allow_alpha", m_options.allow_alpha}}},
                                   {"selection", color_selection_to_json(m_initial)}, {"favorites", std::move(favorites)},
                                   {"favorites_writable", m_favorites_writable}, {"preserve_multi_color", m_preserve_multi_color}};
    m_init_sent = true;
    // Already deferred and guarded: avoid the shared call_web_handler's unguarded
    // second CallAfter, which could run after this modal dialog is destroyed.
    if (!run_script(wxString::FromUTF8("window.ColorPickerDialog.handleMessage(" + payload.dump() + ")")))
        m_init_sent = false;
}

void ColorPickerDialog::finish(int return_code)
{
    if (m_closing)
        return;
    m_closing = true;
    if (return_code != wxID_OK)
        m_selection.reset();
    if (IsModal())
        EndModal(return_code);
    else
        Hide();
}

} // namespace Slic3r::GUI
