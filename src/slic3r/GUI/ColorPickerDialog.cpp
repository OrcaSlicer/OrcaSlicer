#include "ColorPickerDialog.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "Widgets/WebView.hpp"
#include <wx/dialog.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/toplevel.h>
#include <wx/frame.h>
#include <wx/colour.h>
#include <wx/display.h>
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
#include <functional>
#include <system_error>

// ColorSampler
#if defined(__WXMSW__)
#include <wx/msw/wrapwin.h>
#include <cstdint>
#elif defined(__WXGTK__)
#include <gio/gio.h>
#endif

namespace Slic3r::GUI {

namespace {

// - macOS: system NSColorSampler loupe. The system captures the screen and only the chosen
//   color reaches Orca, so the Screen Recording permission is never requested.
// - Windows: one GDI snapshot of the virtual screen in a topmost overlay with a magnifier loupe.
//   GDI reads need no permission
// - Linux: Uses XDG Screenshot portal’s PickColor method
class ColorSampler
{
public:
    using Callback = std::function<void(std::optional<ColorRGBA>)>; // std::nullopt when user cancels; otherwise an opaque sRGB color
    ColorSampler() = delete;
    #if defined(__WXOSX__) || defined(__WXMSW__) || defined(__WXGTK__)
        static inline bool m_sampler_available = true;
    #else
        static inline bool m_sampler_available = false; // No sampler: start() is left undefined, and dialog never calls it here
    #endif
    static void start(Callback done); // Runs `done` exactly once on the GUI thread

private:
#ifdef __WXMSW__
    class Overlay;
#endif
#if defined(__WXOSX__) || defined(__WXMSW__) || defined(__WXGTK__)
    static inline bool s_running = false; // Only platforms with a sampler use this
#endif
};

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
        auto& gradient = std::get<ColorGradient>(normalized);
        if (!options.allow_gradient || gradient.size() < 2 || gradient.size() > COLOR_PICKER_MAX_GRADIENT_COLORS)
            return std::nullopt;
        for (ColorRGBA& color : gradient)
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
    } else if (*type == "gradient" && options.allow_gradient && colors->size() >= 2 && colors->size() <= COLOR_PICKER_MAX_GRADIENT_COLORS) {
        ColorGradient gradient(colors->size());
        for (std::size_t i = 0; i < gradient.size(); ++i)
            if (!parse_color((*colors)[i], gradient[i]))
                return std::nullopt;
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
    nlohmann::json colors = nlohmann::json::array();
    for (const ColorRGBA& color : std::get<ColorGradient>(*normalized))
        colors.push_back(rgba_hex(color));
    return {{"type", "gradient"}, {"colors", std::move(colors)}};
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

// Sidebar state lives in its own section: "color_picker" is replaced wholesale when favorites are
// saved, and its presence alone marks favorites as already migrated.
constexpr const char* COLOR_PICKER_UI_SECTION = "color_picker_ui";

// The page's own size in DIP (style.css): body padding 10 oneach side around the 330 x 360 picker, 
// and the 160px sidebar plus its 10px gap. Opening at this size means the
// first resize message from the page normally changes nothing, so the window doesn't jump.
constexpr int COLOR_PICKER_PAGE_WIDTH    = 330 + 2 * 10;
constexpr int COLOR_PICKER_PAGE_HEIGHT   = 360 + 2 * 10;
constexpr int COLOR_PICKER_SIDEBAR_WIDTH = 160 + 10;

// Palette ids are short lowercase words defined by the page (basic, orca, ral, gradient, ...).
bool valid_palette_id(const std::string& palette)
{
    return !palette.empty() && palette.size() <= 32 &&
           std::all_of(palette.begin(), palette.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

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
        {"Solid", _u8L_CONTEXT("Solid", "Color picker")},
        {"Gradient", _u8L("Gradient")},
        {"Gradient start", _u8L("Gradient start")},
        {"Gradient end", _u8L("Gradient end")},
        {"Gradient color", _u8L("Gradient color")},
        {"Add color", _u8L("Add color")},
        {"Remove color", _u8L("Remove color")},
        {"Saturation", _u8L("Saturation")},
        {"Original color", _u8L("Original color")},
        // TRN: Clicking the current color preview adds it to the saved favorites.
        {"Save current color", _u8L("Save current color")},
        {"Palettes", _u8L("Palettes")},
        {"Pick color from screen", _u8L("Pick color from screen")},
        {"Search colors", _u8L("Search colors")},
        {"Filter colors", _u8L("Filter colors")},
        {"No matching colors", _u8L("No matching colors")},
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
        {"Up to %d colors supported.", _u8L("Up to %d colors supported.")},
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
        {"Pale violet red", _u8L("Pale violet red")},
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
        {"Electric blue", _u8L("Electric blue")},
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
        {"Daffodil yellow", _u8L("Daffodil yellow")},
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
        {"Capri blue", _u8L("Capri blue")},
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
        {"Olive grey", _u8L("Olive grey")},
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
        {"Metal: Silver", _u8L("Metal: Silver")},
        {"Metal: Brass", _u8L("Metal: Brass")},
        {"Metal: Gold", _u8L("Metal: Gold")},
        {"Metal: Copper", _u8L("Metal: Copper")},
        {"Metal: Bronze", _u8L("Metal: Bronze")},
        {"Nature: Pearl", _u8L("Nature: Pearl")},
        {"Nature: Rainbow", _u8L("Nature: Rainbow")},
        {"Nature: Clear Sky", _u8L("Nature: Clear Sky")},
        {"Nature: Sunset", _u8L("Nature: Sunset")},
        {"Wood: Dark", _u8L("Wood: Dark")},
        {"Wood: Medium", _u8L("Wood: Medium")},
        {"Wood: Light", _u8L("Wood: Light")}
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
                        wxCAPTION | wxSYSTEM_MENU | wxCLOSE_BOX | wxFRAME_NO_TASKBAR),
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
    wxGetApp().UpdateDlgDarkUI(this);
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
    // Read the sidebar state before the WebView exists: add_user_scripts() passes it to the page,
    // and the window opens at the matching size instead of resizing after the page loads.
    if (wxGetApp().app_config) {
        const AppConfig& config = *wxGetApp().app_config;
        m_sidebar_open = config.get(COLOR_PICKER_UI_SECTION, "sidebar_open") == "1";
        if (const std::string palette = config.get(COLOR_PICKER_UI_SECTION, "palette"); valid_palette_id(palette))
            m_palette = palette;
    }
    m_content_width  = COLOR_PICKER_PAGE_WIDTH + (m_sidebar_open ? COLOR_PICKER_SIDEBAR_WIDTH : 0);
    m_content_height = COLOR_PICKER_PAGE_HEIGHT;
    if (!normalized || !create_webview("web/dialog/ColorPickerDialog/index.html", _L("Color Picker"),
                                      wxSize(m_content_width, m_content_height),
                                      wxSize(COLOR_PICKER_PAGE_WIDTH, COLOR_PICKER_PAGE_HEIGHT))) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, wxS("wxWebView unavailable")), wxSizerFlags().Border(wxALL, FromDIP(20)));
        sizer->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), wxSizerFlags().Right().Border(wxALL, FromDIP(8)));
        SetSizerAndFit(sizer);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { finish(wxID_CANCEL); }, wxID_CANCEL);
        Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
            if (event.IsShown() && !m_initial_positioned) {
                move_to_anchor();
                wxGetApp().CallAfter([this, alive = m_alive] {
                    if (alive->load(std::memory_order_acquire) && !m_closing) {
                        move_to_anchor();
                        m_initial_positioned = true;
                    }
                });
            }
            event.Skip();
        });
        wxGetApp().UpdateDlgDarkUI(this);
        move_to_anchor();
        return;
    }
    browser()->EnableBrowserAcceleratorKeys(false);
    browser()->Bind(EVT_WEBVIEW_RECREATED, [this](wxCommandEvent& event) {
        wxGetApp().UpdateDlgDarkUI(this);
        event.Skip(); // The shared host updates the page theme.
    });
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
    move_to_anchor();
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown() && !m_initial_positioned) {
            resize_to_content(m_content_height);
            move_to_anchor();
            // GTK may ignore the requested position until the window is shown.
            wxGetApp().CallAfter([this, alive = m_alive] {
                if (alive->load(std::memory_order_acquire) && !m_closing) {
                    resize_to_content(m_content_height);
                    move_to_anchor();
                    m_initial_positioned = true;
                    focus_webview();
                }
            });
        }
        event.Skip();
    });
}

ColorPickerDialog::~ColorPickerDialog() { m_alive->store(false, std::memory_order_release); }

void ColorPickerDialog::position_panel()
{
    if (m_positioning)
        return;
    m_positioning = true;
    // Both dimensions come from the page: width changes when the palette sidebar opens or closes.
    wxSize size = FromDIP(wxSize(m_content_width, m_content_height));
    // The work-area limit includes the native title bar and fixed frame.
    const wxSize frame = GetSize() - GetClientSize();
    size.x = std::min(size.x, std::max(1, m_work_area.width - frame.x));
    size.y = std::min(size.y, std::max(1, m_work_area.height - frame.y - FromDIP(16)));
    SetMinSize(wxSize(std::min(FromDIP(320), size.x), std::min(FromDIP(360), size.y)));
    if (size != GetClientSize())
        SetClientSize(size);
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

void ColorPickerDialog::on_sys_color_changed()
{
    wxGetApp().UpdateDlgDarkUI(this);
}

void ColorPickerDialog::add_user_scripts()
{
    if (wxWebView* view = browser()) {
        auto language = wxGetApp().current_language_code().ToStdString();
        std::replace(language.begin(), language.end(), '_', '-');
        const std::string script = "window.ORCA_UI_STRINGS = " +
            color_picker_ui_strings().dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore) +
            ";window.ORCA_COLOR_PICKER_LANGUAGE = " + nlohmann::json(language).dump() +
            // Lets the page lay out the sidebar before its first paint, matching the window size.
            ";window.ORCA_COLOR_PICKER_UI = " + nlohmann::json({{"sidebar_open", m_sidebar_open}, {"palette", m_palette}}).dump() + ";";
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
    // Not named `id`: on macOS this file is Objective-C++, where `id` is a built-in type.
    const auto& page = page_id->get_ref<const std::string&>();
    if (page.empty() || page.size() > 64)
        return;
    if (*command == "ready") {
        // A repeated ready from the same document must not reset its edits. A new
        // document (e.g. WebView recreation) gets a fresh initialization handshake.
        if (page != m_page_id) {
            m_page_id = page;
            m_page_ready = false;
            m_init_sent = false;
        }
        if (!m_init_sent)
            send_initial_state();
        return;
    }
    if (page != m_page_id)
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
        // Width is optional so an older page that reports only height keeps the current width.
        const auto width = payload.find("width");
        if (width != payload.end()) {
            if (!width->is_number())
                return;
            const double width_value = width->get<double>();
            if (!std::isfinite(width_value) || width_value < 300 || width_value > 1200)
                return;
            m_content_width = static_cast<int>(std::ceil(width_value));
        }
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
    } else if (*command == "sample_screen") {
        sample_screen();
    } else if (*command == "update_ui") {
        // Sidebar open/closed and the chosen palette, restored the next time the picker opens.
        const auto open = payload.find("sidebar_open");
        const auto palette = payload.find("palette");
        if (open == payload.end() || !open->is_boolean() || palette == payload.end() || !palette->is_string() || !wxGetApp().app_config)
            return;
        const auto& palette_id = palette->get_ref<const std::string&>();
        if (!valid_palette_id(palette_id))
            return;
        m_sidebar_open = open->get<bool>();
        m_palette = palette_id;
        // set() marks the config dirty; it is written with the rest of the app config.
        AppConfig& config = *wxGetApp().app_config;
        config.set(COLOR_PICKER_UI_SECTION, "sidebar_open", m_sidebar_open ? "1" : "0");
        config.set(COLOR_PICKER_UI_SECTION, "palette", m_palette);
    }
}

void ColorPickerDialog::send_initial_state()
{
    nlohmann::json favorites = nlohmann::json::array();
    for (const ColorSelection& selection : m_favorites)
        favorites.push_back(color_selection_to_json(selection));
    const nlohmann::json payload = {{"command", "init"}, {"page_id", m_page_id},
                                   {"options", {{"allow_gradient", m_options.allow_gradient}, {"allow_alpha", m_options.allow_alpha},
                                                {"max_gradient_colors", COLOR_PICKER_MAX_GRADIENT_COLORS}}},
                                   {"can_sample_screen", ColorSampler::m_sampler_available},
                                   {"selection", color_selection_to_json(m_initial)}, {"favorites", std::move(favorites)},
                                   {"favorites_writable", m_favorites_writable}, {"preserve_multi_color", m_preserve_multi_color},
                                   {"ui", {{"sidebar_open", m_sidebar_open}, {"palette", m_palette}}}};
    m_init_sent = true;
    // Already deferred and guarded: avoid the shared call_web_handler's unguarded
    // second CallAfter, which could run after this modal dialog is destroyed.
    if (!run_script(wxString::FromUTF8("window.ColorPickerDialog.handleMessage(" + payload.dump() + ")")))
        m_init_sent = false;
}

void ColorPickerDialog::sample_screen()
{
#if defined(__WXOSX__) || defined(__WXMSW__) || defined(__WXGTK__)
    // One sampler at a time; a second click while the loupe is up is ignored
    if (m_sampling)
        return;
    m_sampling = true;
    // Loupe can outlive this dialog (e.g. Escape reaches dialog first),
    // so result is only applied while the lifetime token says dialog still exists
    ColorSampler::start([this, alive = m_alive](std::optional<ColorRGBA> color) {
        if (!alive->load(std::memory_order_acquire))
            return;
        m_sampling = false;
        if (m_closing || !m_page_ready || !browser())
            return;
        const auto byte = [](float value) { return static_cast<int>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f)); };
        nlohmann::json response = {{"command", "sampled_color"}, {"page_id", m_page_id}, {"picked", color.has_value()}};
        if (color)
            response["rgb"] = {byte(color->r()), byte(color->g()), byte(color->b())};
        run_script(wxString::FromUTF8("window.ColorPickerDialog.handleMessage(" + response.dump() + ")"));
        focus_webview();
    });
#endif
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

#if defined(__WXMSW__)

// Left-click, Enter or Space picks.Escape, right-click or switching apps, cancels picking
// Shift key slow downs mouse movement for preciese picking
// DPI awware with using built-in Orca's manifest
class ColorSampler::Overlay
{
public:
    static void start(Callback done);

private:
    static constexpr wchar_t WINDOW_CLASS[]    = L"OrcaSlicerColorSampler";
    static constexpr int     LOUPE_SIZE        = 64;         // Side of the square loupe at 96 DPI
    static constexpr int     LOUPE_CELLS       = 9;          // Screen pixels per side; odd, so one sits in the middle
    static constexpr double  SLOW_SPEED        = 0.2;        // Cursor speed while Shift is held
    static constexpr UINT    WM_SAMPLER_MOVE   = WM_APP + 1; // Posted by the hook, which must return quickly
    static constexpr UINT    WM_SAMPLER_FINISH = WM_APP + 2; // wParam: picked

    // 32-bit top-down DIB selected into a memory DC, with direct access to its 0x00RRGGBB pixels
    struct Bitmap
    {
        HDC            dc     = nullptr;
        HBITMAP        bitmap = nullptr;
        HGDIOBJ        old    = nullptr;
        std::uint32_t* bits   = nullptr;
        int            width  = 0;
        int            height = 0;

        Bitmap() = default;
        Bitmap(const Bitmap&) = delete;
        Bitmap& operator=(const Bitmap&) = delete;
        ~Bitmap() { reset(); }
        bool create(int w, int h);
        void reset();
    };

    explicit Overlay(Callback done) : m_done(std::move(done)) {}

    bool          take_snapshot();
    std::uint32_t pixel(POINT p) const;
    void          follow_cursor();
    void          update_loupe();
    void          finish(bool picked);

    static int monitor_dpi(HMONITOR monitor);
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM lparam);

    Callback m_done;
    RECT     m_screen{};               // Virtual screen
    POINT    m_target{};               // Target pixel
    Bitmap   m_snapshot;
    Bitmap   m_loupe;                  // Loupe image: cells, target frame and border in one bitmap
    HWND     m_window       = nullptr; // Full-screen overlay showing snapshot
    HWND     m_loupe_window = nullptr; // Layered, click-through, owned by m_window
    bool     m_active       = false;   // Shown and focused; losing activation then cancels
    bool     m_finished     = false;

    static inline Overlay* s_overlay      = nullptr;
    static inline HHOOK    s_hook         = nullptr; // Swallows mouse buttons to prevent clicking items under sampler
    static inline bool     s_move_pending = false;
    static inline double   m_x            = 0.;      // Cursor position the hook last saw or set,
    static inline double   m_y            = 0.;      // kept fractional for slow movement
};

bool ColorSampler::Overlay::Bitmap::create(int w, int h)
{
    reset();
    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = w;
    info.bmiHeader.biHeight      = -h; // Top-down rows
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    dc     = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!dc) {
        reset();
        return false;
    }
    old    = SelectObject(dc, bitmap);
    bits   = static_cast<std::uint32_t*>(data);
    width  = w;
    height = h;
    return true;
}

void ColorSampler::Overlay::Bitmap::reset()
{
    if (dc) {
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    if (bitmap)
        DeleteObject(bitmap);
    dc     = nullptr;
    bitmap = nullptr;
    old    = nullptr;
    bits   = nullptr;
    width  = 0;
    height = 0;
}

// Loaded at runtime so the sampler still starts where shcore.dll lacks it
int ColorSampler::Overlay::monitor_dpi(HMONITOR monitor)
{
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto get_dpi = [] {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"))) : nullptr;
    }();
    UINT dpi_x = 96, dpi_y = 96;
    if (get_dpi)
        get_dpi(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpi_x, &dpi_y);
    return std::clamp<int>(dpi_x, 96, 4 * 96);
}

bool ColorSampler::Overlay::take_snapshot()
{
    m_screen.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_screen.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_screen.right  = m_screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_screen.bottom = m_screen.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int w = m_screen.right  - m_screen.left;
    const int h = m_screen.bottom - m_screen.top;
    if (w <= 0 || h <= 0 || !m_snapshot.create(w, h))
        return false;
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    // CAPTUREBLT includes layered windows (menus, tooltips, many modern apps)
    const bool ok = BitBlt(m_snapshot.dc, 0, 0, w, h, screen, m_screen.left, m_screen.top, SRCCOPY | CAPTUREBLT) != FALSE;
    ReleaseDC(nullptr, screen);
    GdiFlush();
    return ok;
}

std::uint32_t ColorSampler::Overlay::pixel(POINT p) const
{
    const int x = p.x - m_screen.left;
    const int y = p.y - m_screen.top;
    return ((x >= 0 && y >= 0 && x < m_snapshot.width && y < m_snapshot.height)
        ? m_snapshot.bits[static_cast<std::size_t>(y) * m_snapshot.width + x]
        : 0); // Black past the screen edge
}

void ColorSampler::Overlay::follow_cursor()
{
    POINT cursor{};
    if (!GetCursorPos(&cursor) || (cursor.x == m_target.x && cursor.y == m_target.y))
        return;
    m_target = cursor;
    update_loupe();
}

// Draws every loupe pixel (magnified cells, target frame and border) into one bitmap with borders
void ColorSampler::Overlay::update_loupe()
{
    HMONITOR    monitor = MonitorFromPoint(m_target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const int side = LOUPE_SIZE * monitor_dpi(monitor) / 96;
    if (side != m_loupe.width && !m_loupe.create(side, side))
        return;

    // Centered on target pixel, then shifted to stay fully on its monitor
    const RECT& bounds = info.rcMonitor;
    POINT pos{
        std::clamp<LONG>(m_target.x - side / 2, bounds.left, std::max<LONG>(bounds.left, bounds.right  - side)),
        std::clamp<LONG>(m_target.y - side / 2, bounds.top , std::max<LONG>(bounds.top , bounds.bottom - side))
    };

    const auto cell  = [side](int offset) { return offset * LOUPE_CELLS / side; };                    // Cell of a loupe pixel
    const auto first = [side](int index ) { return (index * side + LOUPE_CELLS - 1) / LOUPE_CELLS; }; // First pixel of a cell
    // Target cell is the one under cursor; off center when loupe was shifted
    const int  t_col = cell(m_target.x - pos.x);
    const int  t_row = cell(m_target.y - pos.y);

    for (int y = 0; y < side; ++y) {
        const int row = cell(y);
        for (int x = 0; x < side; ++x) {
            const int col = cell(x);
            // Distance to loupe edge or to target cell's edge inside target cell
            int ring = std::min({x, y, side - 1 - x, side - 1 - y});
            if (col == t_col && row == t_row)
                ring = std::min({ring, x - first(col), first(col + 1) - 1 - x, y - first(row), first(row + 1) - 1 - y});
            m_loupe.bits[y * side + x] = ring == 0 ? 0x000000u // Black outer line
                                       : ring == 1 ? 0xFFFFFFu // White inner line
                                       : pixel(POINT{m_target.x + col - t_col, m_target.y + row - t_row});
        }
    }

    SIZE  size{side, side};
    POINT origin{0, 0};
    UpdateLayeredWindow(m_loupe_window, nullptr, &pos, &size, m_loupe.dc, &origin, 0, nullptr, ULW_OPAQUE);
}

// Windows, mouse hook and overlay state are gone before callback runs,
// so callback may start another sampler or close dialog that asked for this one
void ColorSampler::Overlay::finish(bool picked)
{
    if (m_finished)
        return;
    m_finished = true;
    const std::uint32_t value = pixel(m_target);
    Callback done = std::move(m_done);
    DestroyWindow(m_window);                   // Also destroys owned loupe window; WM_NCDESTROY frees this overlay
    SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Show an arrow until window under cursor sets its own
    done(picked ? std::optional<ColorRGBA>(ColorRGBA(static_cast<unsigned char>((value >> 16) & 0xFF),
                                                     static_cast<unsigned char>((value >> 8) & 0xFF),
                                                     static_cast<unsigned char>(value & 0xFF),
                                                     static_cast<unsigned char>(255)))
                : std::nullopt);
}

LRESULT CALLBACK ColorSampler::Overlay::mouse_hook(int code, WPARAM message, LPARAM lparam)
{
    if (code != HC_ACTION || !s_overlay)
        return CallNextHookEx(nullptr, code, message, lparam);
    const POINT pt = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)->pt;
    switch (message) {
    case WM_MOUSEMOVE: {
        // Shift: swallows move and advance cursor by a fraction without system-wide changes
        const bool slow = GetAsyncKeyState(VK_SHIFT) < 0;
        if (slow) {
            const RECT& scr = s_overlay->m_screen;
            m_x = std::clamp(m_x + (pt.x - std::lround(m_x)) * SLOW_SPEED, double(scr.left), double(scr.right  - 1));
            m_y = std::clamp(m_y + (pt.y - std::lround(m_y)) * SLOW_SPEED, double(scr.top) , double(scr.bottom - 1));
            SetCursorPos(static_cast<int>(std::lround(m_x)), static_cast<int>(std::lround(m_y)));
        } else {
            m_x = pt.x;
            m_y = pt.y;
        }
        if (!s_move_pending) // Coalesced; overlay reads latest position when it handles message
            s_move_pending = PostMessageW(s_overlay->m_window, WM_SAMPLER_MOVE, 0, 0) != FALSE;
        return slow ? 1 : CallNextHookEx(nullptr, code, message, lparam);
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        PostMessageW(s_overlay->m_window, WM_SAMPLER_FINISH, message == WM_LBUTTONUP, 0);
        return 1;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return 1; // Swallowed: nothing under the overlay sees these
    default:
        return CallNextHookEx(nullptr, code, message, lparam);
    }
}

LRESULT CALLBACK ColorSampler::Overlay::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    Overlay* overlay = s_overlay;
    if (!overlay || hwnd != overlay->m_window) // Loupe window, and the overlay while it is being created
        return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const RECT& r = ps.rcPaint; // Client coordinates match the snapshot's
        BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, overlay->m_snapshot.dc, r.left, r.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        // Hidden only while it is over overlay; this is no ShowCursor counter,
        // so system shows cursor again as soon as overlay is gone, even if app crashes
        SetCursor(nullptr);
        return TRUE;
    case WM_SAMPLER_MOVE:
        s_move_pending = false;
        overlay->follow_cursor();
        return 0;
    case WM_SAMPLER_FINISH:
        overlay->follow_cursor();
        overlay->finish(wparam != 0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
            overlay->finish(false);
        else if (wparam == VK_RETURN || wparam == VK_SPACE)
            overlay->finish(true);
        return 0;
    case WM_ACTIVATE:
        // Switching away (Alt+Tab, Win key) cancels instead of leaving a frozen screen behind
        if (LOWORD(wparam) == WA_INACTIVE && overlay->m_active)
            overlay->finish(false);
        return 0;
    case WM_CANCELMODE:
    case WM_CLOSE:
        overlay->finish(false);
        return 0;
    case WM_DPICHANGED:
        return 0; // Keep covering whole virtual screen; never resize to suggested rect
    case WM_NCDESTROY: {
        if (s_hook)
            UnhookWindowsHookEx(s_hook);
        s_hook         = nullptr;
        s_overlay      = nullptr;
        s_move_pending = false;
        s_running      = false;
        // Destroyed without finish() (e.g. at app exit): still report a cancel
        Callback done = overlay->m_finished ? nullptr : std::move(overlay->m_done);
        delete overlay;
        if (done)
            done(std::nullopt);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void ColorSampler::Overlay::start(Callback done)
{
    HINSTANCE   instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = window_proc;
    wc.hInstance     = instance;
    wc.lpszClassName = WINDOW_CLASS; // No class cursor: hidden over overlay
    const bool registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    std::unique_ptr<Overlay> overlay(new Overlay(std::move(done)));
    if (registered && overlay->take_snapshot()) {
        const RECT& s = overlay->m_screen;
        // Topmost tool window over every monitor; no taskbar button and no Alt+Tab entry
        overlay->m_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WINDOW_CLASS, L"", WS_POPUP,
                                            s.left, s.top, s.right - s.left, s.bottom - s.top, nullptr, nullptr, instance, nullptr);
    }
    if (!overlay->m_window) {
        Callback callback = std::move(overlay->m_done);
        overlay.reset();
        callback(std::nullopt);
        return;
    }

    // From here overlay window owns overlay: finish() reports result and frees everything
    Overlay* o = overlay.release();
    s_overlay  = o;
    s_running  = true;
    o->m_loupe_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0, o->m_window, nullptr, instance, nullptr);
    // Runs on this GUI thread, which pumps messages. Without it clicks could reach other windows
    s_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, instance, 0);
    if (!o->m_loupe_window || !s_hook || !GetCursorPos(&o->m_target)) {
        o->finish(false);
        return;
    }
    m_x = o->m_target.x;
    m_y = o->m_target.y;
    o->update_loupe();

    ShowWindow(o->m_window, SW_SHOW);
    ShowWindow(o->m_loupe_window, SW_SHOWNOACTIVATE);
    SetForegroundWindow(o->m_window);
    SetFocus(o->m_window);
    SetCursor(nullptr); // Hide it now rather than on first move
    UpdateWindow(o->m_window);
    if (s_overlay != o) // Showing can already end sampling (e.g. WM_CANCELMODE), which frees overlay
        return;
    o->m_active = GetForegroundWindow() == o->m_window;
    if (!o->m_active) // Without focus Escape would never reach overlay; give up rather than trap user
        o->finish(false);
}

void ColorSampler::start(Callback done)
{
    if (!done)
        return;
    if (s_running) {
        done(std::nullopt);
        return;
    }
    Overlay::start(std::move(done));
}

#elif defined(__WXOSX__)

void ColorSampler::start(Callback done)
{
    if (!done)
        return;
    if (s_running) {
        done(std::nullopt);
        return;
    }
    s_running = true;
    sample_screen_color([done = std::move(done)](std::optional<ColorRGBA> color) {
        s_running = false;
        done(color);
    });
}

#elif defined(__WXGTK__)
// Desktop portal picker (GNOME, KDE; X11 and Wayland): Desktop shows its own picker and only chosen sRGB color reaches Orca.
// Every failure (no session bus, no portal, no PickColor backend, portal exiting mid-pick) ends as a cancel
void ColorSampler::start(Callback done)
{
    if (!done)
        return;
    GDBusConnection* bus    = s_running ? nullptr : g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    const char*      unique = bus ? g_dbus_connection_get_unique_name(bus) : nullptr;
    if (!unique) { // Already picking, no session bus, or not a message bus connection
        if (bus)
            g_object_unref(bus);
        done(std::nullopt);
        return;
    }

    struct Request
    {
        Callback         done;
        GDBusConnection* bus;
        GCancellable*    cancellable  = g_cancellable_new();
        guint            subscription = 0;
        guint            watch        = 0;
        bool             portal_seen  = false;

        // Unhooks everything first, so no callback can reach this request after it is deleted
        void finish(std::optional<ColorRGBA> color)
        {
            g_cancellable_cancel(cancellable);
            g_object_unref(cancellable);
            g_dbus_connection_signal_unsubscribe(bus, subscription);
            g_bus_unwatch_name(watch);
            g_object_unref(bus);
            s_running = false;
            Callback callback = std::move(done);
            delete this;
            callback(color);
        }
    };
    auto* request = new Request{std::move(done), bus};
    s_running     = true;

    // Answer arrives as a Response signal on a Request object whose path follows from our bus name and a token;
    // subscribe before calling so it can't be missed
    static unsigned   counter = 0;
    const std::string token   = "orca_pick_" + std::to_string(++counter);
    std::string       sender  = unique + 1; // Without the leading ':'
    std::replace(sender.begin(), sender.end(), '.', '_');
    const std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;

    request->subscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request", "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
            guint32   response = 2;
            GVariant* results  = nullptr;
            g_variant_get(parameters, "(u@a{sv})", &response, &results);
            double r = 0., g = 0., b = 0.;
            std::optional<ColorRGBA> color;
            if (response == 0 && g_variant_lookup(results, "color", "(ddd)", &r, &g, &b)) // 0: picked, 1: cancelled, 2: other
                color = ColorRGBA(float(r), float(g), float(b), 1.f);
            g_variant_unref(results);
            static_cast<Request*>(data)->finish(color);
        }, request, nullptr);

    // Portal exiting mid-pick would never send Response. It starts on demand, so an inactive portal also reports
    // vanished before the call below launches it; only a portal that was seen running counts
    request->watch = g_bus_watch_name_on_connection(bus, "org.freedesktop.portal.Desktop", G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, gpointer data) { static_cast<Request*>(data)->portal_seen = true; },
        [](GDBusConnection*, const gchar*, gpointer data) {
            auto* request = static_cast<Request*>(data);
            if (request->portal_seen)
                request->finish(std::nullopt);
        }, request, nullptr);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_dbus_connection_call(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Screenshot", "PickColor", g_variant_new("(sa{sv})", "", &options),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, request->cancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            GError*   error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (reply) { // Accepted; the color comes with the Response signal
                g_variant_unref(reply);
                return;
            }
            const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            g_error_free(error);
            if (cancelled) // Request already finished and freed
                return;
            m_sampler_available = false; // No portal, or no backend implementing PickColor: hide the button for this session
            static_cast<Request*>(data)->finish(std::nullopt);
        }, request);
}
#endif

} // namespace Slic3r::GUI
