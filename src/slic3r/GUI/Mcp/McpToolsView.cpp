// screenshot, get_preview and set_view: what the agent looks at and reads off the Preview, and
// pointing the view before looking.

#include "libvgcode/include/ColorRange.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#ifdef SLIC3R_CAD
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#endif

#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/GCodeStatistics.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <wx/bookctrl.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

std::string base64(const std::string& bytes)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(bytes.size()), '\0');
    out.resize(b64::encode(out.data(), bytes.data(), bytes.size()));
    return out;
}

const char* tab_id(const std::string& name)
{
    if (name == "home") return TAB_ID_HOME;
    if (name == "prepare") return TAB_ID_PREPARE;
    if (name == "preview") return TAB_ID_PREVIEW;
    if (name == "device" || name == "monitor") return TAB_ID_MONITOR;
    if (name == "multi_device") return TAB_ID_MULTI_DEVICE;
    if (name == "project") return TAB_ID_PROJECT;
    if (name == "calibration") return TAB_ID_CALIBRATION;
#ifdef SLIC3R_CAD
    if (name == "design") return TAB_ID_DESIGN;
#endif
    return nullptr;
}

// Rounded for a reply, which reads like the legend rather than the raw floats.
double rounded(double value, double scale) { return std::round(value * scale) / scale; }

// The view types the legend shows a colour range for; the logarithmic layer time repeats the linear one.
constexpr libvgcode::EViewType range_view_types[] = {
    libvgcode::EViewType::Height,       libvgcode::EViewType::Width,
    libvgcode::EViewType::Speed,        libvgcode::EViewType::ActualSpeed,
    libvgcode::EViewType::Acceleration, libvgcode::EViewType::Jerk,
    libvgcode::EViewType::VolumetricFlowRate, libvgcode::EViewType::ActualVolumetricFlowRate,
    libvgcode::EViewType::LayerTimeLinear,    libvgcode::EViewType::FanSpeed,
    libvgcode::EViewType::Temperature,        libvgcode::EViewType::PressureAdvance};

// preview_state() of the Preview's toolpaths, plus which legend rows are visible and the colour range of
// every view type. Ranges span the whole print, but count travels, wipes and Custom extrusions only
// while the legend shows them. UI thread.
json preview_view(GLCanvas3D& canvas)
{
    // The viewer recomputes its ranges at its next frame after a legend change; nothing draws while
    // the Preview is hidden, and nothing changes the legend then either.
    canvas.set_as_dirty();
    canvas.render();
    GCodeViewer&             viewer    = canvas.get_gcode_viewer();
    const libvgcode::Viewer& toolpaths = viewer.get_viewer();
    json                     view      = preview_state(viewer);
    json                     features  = json::object();
    for (const libvgcode::EGCodeExtrusionRole role : toolpaths.get_extrusion_roles())
        features[legend_feature_key(role)] = toolpaths.is_extrusion_role_visible(role);
    json options = json::object();
    for (const libvgcode::EOptionType type : toolpaths.get_options())
        if (const char* key = legend_option_key(type))
            options[key] = toolpaths.is_option_visible(type);
    view["legend"] = {{"features", std::move(features)}, {"options", std::move(options)}};
    json ranges    = json::object();
    for (const libvgcode::EViewType type : range_view_types) {
        // [FLT_MAX, -FLT_MAX] when the print has no such value.
        const std::array<float, 2>& range = toolpaths.get_color_range(type).get_range();
        if (range[0] <= range[1])
            ranges[GCodeViewer::view_type_to_config_name(type)] = {rounded(range[0], 1e4), rounded(range[1], 1e4)};
    }
    view["ranges"] = std::move(ranges);
    return view;
}

// A legend key in any case, with _ or a space between words: "Filament changes" is filament_changes,
// "sparse_infill" is Sparse infill.
bool same_legend_key(const std::string& given, const std::string& name)
{
    const auto fold = [](char c) { return c == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(c))); };
    return given.size() == name.size() && std::equal(given.begin(), given.end(), name.begin(), [&fold](char a, char b) { return fold(a) == fold(b); });
}

// Whether `key` names a legend row some print has, this one or not.
bool any_legend_row(const std::string& key)
{
    for (std::size_t r = 0; r < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++r)
        if (same_legend_key(key, legend_feature_key(libvgcode::EGCodeExtrusionRole(r))))
            return true;
    for (std::size_t o = 0; o < libvgcode::OPTION_TYPES_COUNT; ++o)
        if (const char* name = legend_option_key(libvgcode::EOptionType(o)); name != nullptr && same_legend_key(key, name))
            return true;
    return false;
}

// set_view's legend argument: each named row shown or hidden as a click on it does, clicked only when its
// state differs. The rows are the feature types and options the print has, whatever the view type: the
// viewer keeps one visibility set, though the legend lists feature types only in Line Type. The keys are
// checked here, before the Preview changes; the returned function applies them and gives each row's
// state afterwards. UI thread.
std::function<json()> legend_change(GCodeViewer& viewer, const json& legend)
{
    const libvgcode::Viewer&                        toolpaths = viewer.get_viewer();
    const std::vector<libvgcode::EGCodeExtrusionRole> roles   = toolpaths.get_extrusion_roles();
    const std::vector<libvgcode::EOptionType>&      options   = toolpaths.get_options();
    std::vector<std::pair<libvgcode::EGCodeExtrusionRole, bool>> role_rows;
    std::vector<std::pair<libvgcode::EOptionType, bool>>         option_rows;
    for (auto item = legend.begin(); item != legend.end(); ++item) {
        const std::string& key = item.key();
        if (!item.value().is_boolean())
            throw ToolFailure("legend \"" + key + "\" must be true (show) or false (hide)");
        const auto role   = std::find_if(roles.begin(), roles.end(), [&key](libvgcode::EGCodeExtrusionRole r) { return same_legend_key(key, legend_feature_key(r)); });
        const auto option = std::find_if(options.begin(), options.end(), [&key](libvgcode::EOptionType o) {
            const char* name = legend_option_key(o);
            return name != nullptr && same_legend_key(key, name);
        });
        if (role != roles.end())
            role_rows.emplace_back(*role, item.value().get<bool>());
        else if (option != options.end())
            option_rows.emplace_back(*option, item.value().get<bool>());
        else if (!item.value().get<bool>() && any_legend_row(key))
            continue; // a row this print lacks shows nothing, so it is hidden already
        else {
            std::string features, keys;
            for (const libvgcode::EGCodeExtrusionRole r : roles)
                features += (features.empty() ? "" : ", ") + legend_feature_key(r);
            for (const libvgcode::EOptionType o : options)
                if (const char* name = legend_option_key(o))
                    keys += (keys.empty() ? "" : ", ") + std::string(name);
            throw ToolFailure("Unknown legend item \"" + key + "\"; this print has " + features +
                              (keys.empty() ? std::string() : " and the options " + keys));
        }
    }
    return [&viewer, role_rows, option_rows]() {
        const libvgcode::Viewer& toolpaths = viewer.get_viewer();
        json                     done      = json::object();
        for (const auto& [role, show] : role_rows) {
            if (toolpaths.is_extrusion_role_visible(role) != show)
                viewer.toggle_extrusion_role_visibility(role);
            done[legend_feature_key(role)] = toolpaths.is_extrusion_role_visible(role);
        }
        for (const auto& [option, show] : option_rows) {
            if (toolpaths.is_option_visible(option) != show)
                viewer.toggle_option_visibility(option);
            done[legend_option_key(option)] = toolpaths.is_option_visible(option);
        }
        return done;
    };
}

// set_view's orbit: the camera moved around the point it looks at, `yaw` degrees to its right
// (counterclockwise seen from above) and `pitch` degrees up, at the same distance. It is placed from its
// position rather than turned as a mouse drag turns it, so the result is the same whatever the user's
// mouse settings and free-camera preference. The elevation stops a degree short of straight up or down,
// where an upright camera has no up direction. Returns where it ends up, in degrees: elevation above the
// bed's plane and azimuth (0 on the +X side, -90 in front).
json orbit_camera(Camera& camera, double yaw, double pitch)
{
    const Vec3d  target   = camera.get_target();
    const Vec3d  offset   = camera.get_position() - target;
    const double distance = offset.norm();
    // Seen from straight above or below, the camera's horizontal side is the one the screen's bottom faces.
    Vec3d flat(offset.x(), offset.y(), 0.);
    if (flat.norm() < 1e-6 * distance)
        flat = -camera.get_dir_up();
    const double limit     = Geometry::deg2rad(89.);
    const double azimuth   = std::atan2(flat.y(), flat.x()) + Geometry::deg2rad(yaw);
    const double elevation = std::clamp(std::asin(std::clamp(offset.z() / distance, -1., 1.)) + Geometry::deg2rad(pitch), -limit, limit);
    const Vec3d  dir(std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
    camera.look_at(target + distance * dir, target, Vec3d::UnitZ());
    const double azimuth_deg = std::remainder(Geometry::rad2deg(azimuth), 360.);
    return {{"elevation", std::lround(Geometry::rad2deg(elevation))}, {"azimuth", std::lround(azimuth_deg)}};
}

} // namespace

std::string legend_feature_key(libvgcode::EGCodeExtrusionRole role) { return ExtrusionEntity::role_to_string(libvgcode::convert(role)); }

const char* legend_option_key(libvgcode::EOptionType type)
{
    switch (type) {
    case libvgcode::EOptionType::Travels: return "travel";
    case libvgcode::EOptionType::Wipes: return "wipe";
    case libvgcode::EOptionType::Retractions: return "retract";
    case libvgcode::EOptionType::Unretractions: return "unretract";
    case libvgcode::EOptionType::Seams: return "seams";
    case libvgcode::EOptionType::ToolChanges: return "filament_changes";
    case libvgcode::EOptionType::ColorChanges: return "color_changes";
    case libvgcode::EOptionType::PausePrints: return "pauses";
    case libvgcode::EOptionType::CustomGCodes: return "custom_gcode";
    default: return nullptr;
    }
}

json preview_features(const json& statistics)
{
    const json&  summary = statistics.at("summary");
    const double total   = summary.at("time").get<double>();
    const auto   row     = [total](double seconds) {
        return json{{"time_s", rounded(seconds, 100.)}, {"percent", total > 0. ? rounded(100. * seconds / total, 10.) : 0.}};
    };
    json out = json::object();
    for (const auto& feature : summary.at("features").items()) {
        json r = row(feature.value().at("time").get<double>());
        r["g"] = rounded(feature.value().at("weight").get<double>(), 100.);
        out[feature.key()] = std::move(r);
    }
    json travel   = row(summary.at("travel_time").get<double>());
    travel["mm"]  = rounded(summary.at("travel_distance").get<double>(), 10.);
    out["Travel"] = std::move(travel);
    return out;
}

json preview_layers(const json& statistics, bool with_features)
{
    json   items = json::array();
    double below = 0.;
    for (const json& layer : statistics.at("layers")) {
        const double z = layer.at("z").get<double>();
        json         item{{"layer", items.size() + 1},
                          {"z", z},
                          {"height", rounded(z > below ? z - below : z, 1000.)},
                          {"time_s", rounded(layer.at("time").get<double>(), 100.)},
                          {"g", rounded(layer.at("weight").get<double>(), 1000.)}};
        if (with_features) {
            json features = json::object();
            for (const auto& feature : layer.at("features").items())
                features[feature.key()] = rounded(feature.value().at("time").get<double>(), 100.);
            item["features"] = std::move(features);
        }
        items.push_back(std::move(item));
        below = z;
    }
    return items;
}

json preview_objects(const json& statistics, const std::vector<std::string>& object_names)
{
    json out = json::array();
    for (const json& object : statistics.at("objects")) {
        const std::string name = object.at("name").get<std::string>();
        json              item{{"name", name},
                               {"instance", object.at("instance")},
                               {"g", rounded(object.at("weight").get<double>(), 100.)},
                               {"support_g", rounded(object.at("support_weight").get<double>(), 100.)}};
        const auto first = std::find(object_names.begin(), object_names.end(), name);
        if (first != object_names.end() && std::find(std::next(first), object_names.end(), name) == object_names.end())
            item["object"] = std::distance(object_names.begin(), first);
        out.push_back(std::move(item));
    }
    return out;
}

bool viewer_holds(const GCodeViewer& viewer, const GCodeProcessorResult* result)
{
    return result != nullptr && viewer.has_data() && viewer.last_result_id() == result->id;
}

json preview_state(GCodeViewer& viewer)
{
    IMSlider* layers = viewer.get_layers_slider();
    if (!viewer.has_data() || layers == nullptr)
        return nullptr;
    const bool silent = viewer.get_viewer().get_time_mode() == libvgcode::ETimeMode::Stealth;
    return json{{"view_type", GCodeViewer::view_type_to_config_name(viewer.get_view_type())},
                {"time_mode", silent ? "silent" : "normal"},
                {"layers", {layers->GetLowerValue() + 1, layers->GetHigherValue() + 1}},
                {"z", {rounded(layers->GetLowerValueD(), 1000.), rounded(layers->GetHigherValueD(), 1000.)}},
                {"layer_count", layers->GetMaxValue() + 1}};
}

void register_view_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef screenshot = inspect_tool(
        "screenshot", L("Take a screenshot"),
        "Capture what OrcaSlicer shows and return it as an image, downscaled to max_dim on the long edge (default 1280), "
        "plus the full-size PNG written to save_path (or the session's output folder). target: \"window\" (the main "
        "window), \"canvas\" (the visible 3D view exactly as rendered: scene, toolpaths, toolbars, ImGui panels; on the "
        "Design tab its view), "
        "\"dialog\" (the topmost modal dialog), or a widget's ref from ui_snapshot (a window that is not modal, or just "
        "that widget). The result says when parts may differ from the screen (approximate). Point the view first with "
        "set_view.",
        object_schema({{"target", {{"type", json::array({"string", "object"})},
                                    {"description", "window (default), canvas, dialog, or a widget's ref or locator from ui_snapshot."}}},
                       {"max_dim", {{"type", "integer"}, {"minimum", 256}, {"maximum", 2000}, {"description", "Long edge of the returned image."}}},
                       {"save_path", prop("string", "Absolute .png path for the full-size image.")},
                       {"overwrite", prop("boolean", "Replace an existing save_path.")},
                       {"include_image", prop("boolean", "Return the image itself (default true); false returns only the path.")}}),
        [&service](const json& args, ToolContext& ctx) {
            const json        target_arg = args.value("target", json("window"));
            const std::string target     = target_arg.is_string() ? target_arg.get<std::string>() : std::string();
            json              locator;
            fs::path          path;
            if (args.contains("save_path")) {
                std::string err;
                auto        p = resolve_output_path(arg_string(args, "save_path"), arg_bool(args, "overwrite", false), err);
                if (!p)
                    throw ToolFailure(err);
                path = *p;
            } else {
                path = timestamped_path(output_dir(), "screenshot", ".png");
            }
            const int max_dim = std::clamp(arg_int(args, "max_dim", 1280), 256, 2000);
            json      info;
            wxImage   full;
            if (target != "window" && target != "canvas" && target != "dialog")
                // Scrolled into view first, and painted before the capture below runs.
                ctx.run_ui([&] {
                    UiTarget t = service.inspector().resolve(target_arg);
                    service.inspector().reveal(t);
                    return json();
                });
            // A canvas that comes back blank has not drawn yet; it does within moments.
            bool blank = false;
            for (int attempt = 0;; ++attempt) {
                ctx.run_ui([&] {
                    std::string             err;
                    std::optional<Capture>  cap;
                    if (target == "canvas") {
                        ui_plater(); // refuses while OrcaSlicer starts or closes
                        GLCanvas3D* canvas = shown_canvas();
                        if (canvas == nullptr)
                            throw ToolFailure("No 3D view is shown; switch to one with set_view tab");
                        cap = capture_canvas(*canvas, err);
                    } else if (target == "dialog") {
                        wxDialog* top = service.modals().top();
                        if (top == nullptr)
                            throw ToolFailure("No modal dialog is open");
                        cap = capture_window(*top, err);
                    } else if (target != "window") {
                        // A widget: its top-level window, cropped to the widget unless it is that window.
                        const UiTarget t = service.inspector().resolve(target_arg);
                        locator          = service.inspector().locator_for(t);
                        wxWindow*      w = t.window.get();
                        if (t.kind != UiTarget::Kind::Window || w == nullptr)
                            throw ToolFailure("target must be window, canvas, dialog or the ref of a widget");
                        if (t.page >= 0) {
                            // A tab: its page, which only the selected tab shows.
                            auto* book = dynamic_cast<wxBookCtrlBase*>(w);
                            if (book == nullptr || t.page >= int(book->GetPageCount()))
                                throw ToolFailure("The tab is gone; take a new ui_snapshot");
                            if (book->GetSelection() != t.page)
                                throw ToolFailure("The tab's page is not shown; click the tab (ui_click) first");
                            w = book->GetPage(size_t(t.page));
                        }
                        if (GLCanvas3D* canvas = service.inspector().canvas_of(&t); canvas != nullptr && canvas->get_wxglcanvas() == w) {
                            cap = capture_canvas(*canvas, err);
                        } else {
                            wxWindow* top = wxGetTopLevelParent(w);
                            // A popup is a native window of its own, which window captures leave out.
                            if (dynamic_cast<wxPopupWindow*>(top) != nullptr)
                                throw ToolFailure("The widget is in a popup, which screenshots don't capture; ui_snapshot {root: \"popup\"} lists it");
                            auto* tlw = dynamic_cast<wxTopLevelWindow*>(top);
                            if (tlw == nullptr)
                                throw ToolFailure("The widget has no window");
                            tlw->Update();
                            cap = capture_window(*tlw, err);
                            if (cap && w != tlw) {
                                // The capture is in physical pixels, like client_offset_in().
                                const wxPoint at    = client_offset_in(*w, *tlw);
                                const double  scale = tlw->GetContentScaleFactor();
                                const wxSize  size(int(std::lround(w->GetSize().x * scale)), int(std::lround(w->GetSize().y * scale)));
                                const wxRect  r = wxRect(at, size).Intersect(wxRect(cap->image.GetSize()));
                                if (r.IsEmpty())
                                    throw ToolFailure("The widget is outside its window's visible area");
                                cap->image = cap->image.GetSubImage(r);
                            }
                        }
                    } else {
                        wxTopLevelWindow* frame = wxGetApp().mainframe;
                        if (frame == nullptr)
                            throw ToolFailure("The main window is not up yet");
                        cap = capture_window(*frame, err);
                    }
                    if (!cap)
                        throw ToolFailure(err);
                    full = cap->image;
                    info = {{"source", target.empty() ? json(target_arg) : json(target)}, {"width", full.GetWidth()}, {"height", full.GetHeight()}};
                    if (cap->approximate)
                        info["approximate"] = true;
                    if (!cap->note.empty())
                        info["note"] = cap->note;
                    blank = cap->blank_canvas;
                    return json();
                });
                if (!blank || attempt >= 10 || ctx.cancelled())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            if (blank) {
                info["approximate"] = true;
                info["note"] = info.value("note", std::string()) + (info.contains("note") ? "; " : "") +
                               "the 3D view came back blank, as if not drawn yet";
            }
            if (target_arg.is_string() && !locator.is_null()) {
                // A replay needs the locator; refs mean nothing in another session.
                json recorded      = args;
                recorded["target"] = locator;
                ctx.set_journal_args(std::move(recorded));
            }
            // Encoding and scaling run off the UI thread.
            if (!full.SaveFile(from_path(path), wxBITMAP_TYPE_PNG))
                throw ToolFailure("Could not write " + path_to_utf8(path));
            info["path"]   = path_to_utf8(path);
            ToolResult out = ToolResult::ok(info);
            if (arg_bool(args, "include_image", true)) {
                const wxSize  size  = fit_within(full.GetSize(), max_dim);
                const wxImage scaled = size == full.GetSize() ? full : full.Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH);
                out.data["image_width"]  = size.x;
                out.data["image_height"] = size.y;
                out.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", base64(encode_png(scaled))}});
            }
            return out;
        });
    screenshot.journal = true; // a replay takes the same screenshots
    reg.add(screenshot);

    reg.add(inspect_tool(
        "get_preview", L("Read the Preview"),
        "What the Preview shows for a sliced plate. summary: the slice reply's, plus prepare and travel time and travel "
        "distance. features: seconds, share of the print time and grams per feature type (the Line Type legend), and "
        "travel. layers: per layer (1-based, as set_view's layer) Z, height, seconds and grams, paged; layer_features adds "
        "each feature type's seconds. objects: grams per object instance, with its list_objects index when its name is "
        "unique. view, for the plate the Preview shows: view type, time mode, layer range, which legend rows are visible, "
        "and each view type's colour range for what the legend shows. Times are normal-mode estimates. A G-code file "
        "opened for viewing reads the same.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Plate index; default the current plate."}}},
                       {"include", array_prop({{"type", "string"}, {"enum", json::array({"features", "layers", "layer_features", "objects", "view"})}},
                                              "Parts besides the summary; default [\"features\", \"view\"].")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Layers per page (default 100)."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            const std::set<std::string> parts = args.contains("include") ? args["include"].get<std::set<std::string>>() :
                                                                           std::set<std::string>{"features", "view"};
            return ToolResult::ok(ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (plater.printer_technology() != ptFFF)
                    throw ToolFailure("The Preview needs an FFF printer");
                // The background process may be rewriting a result whatever its valid flag says.
                if (plater.is_slicing_any())
                    throw ToolFailure("Slicing is in progress; wait {\"for\": \"idle\"} first");
                PartPlateList& plates  = plater.get_partplate_list();
                const int      current = plates.get_curr_plate_index();
                const int      index   = arg_int(args, "plate", current);
                if (index >= plates.get_plate_count())
                    throw ToolFailure("plate index out of range (0.." + std::to_string(plates.get_plate_count() - 1) + ")");
                GLCanvas3D* canvas = plater.get_preview_canvas3D();
                if (canvas == nullptr)
                    throw ToolFailure("The preview is not available");
                PartPlate*                  plate  = plates.get_plate(index);
                const GCodeProcessorResult* result = plate->get_slice_result();
                const bool                  shown  = index == current && viewer_holds(canvas->get_gcode_viewer(), result);
                // A G-code file opened for viewing fills the current plate's result, which the Preview
                // shows, without marking the plate sliced.
                const bool        gcode_file = plater.only_gcode_mode() && shown;
                const std::string name       = "Plate " + std::to_string(index);
                const std::string slice_it   = "slice {\"plates\": " + std::to_string(index) + "}";
                if (result == nullptr || !(plate->is_slice_result_valid() || gcode_file))
                    throw ToolFailure(name + " has no up-to-date slice; " + slice_it + " first");
                // Of a project opened from a sliced 3MF, a plate the Preview has not shown keeps only the file's totals.
                if (result->moves.empty())
                    throw ToolFailure(name + " has no toolpaths, only the totals of a sliced 3MF; " + slice_it + " again for layer data");

                const json statistics      = json::parse(gcode_statistics_json(*result));
                const json& totals         = statistics.at("summary");
                json        summary        = plate_summary(statistics, *result);
                summary["prepare_time_s"]  = std::lround(totals.at("prepare_time").get<double>());
                summary["travel_time_s"]   = std::lround(totals.at("travel_time").get<double>());
                summary["travel_mm"]       = std::lround(totals.at("travel_distance").get<double>());
                json out{{"plate", index}, {"summary", std::move(summary)}};
                if (parts.count("features") > 0)
                    out["features"] = preview_features(statistics);
                if (parts.count("layers") > 0 || parts.count("layer_features") > 0)
                    out["layers"] = page(preview_layers(statistics, parts.count("layer_features") > 0), args, 100);
                if (parts.count("objects") > 0) {
                    std::vector<std::string> names;
                    for (const ModelObject* object : plater.model().objects)
                        names.push_back(object->name);
                    out["objects"] = preview_objects(statistics, names);
                }
                if (parts.count("view") > 0) {
                    out["view"] = shown ? preview_view(*canvas) : json();
                    if (index != current)
                        out["note"] = "The Preview shows plate " + std::to_string(current) + "; show plate " + std::to_string(index) +
                                      " there (run_action orca_command:plate_goto:orca, param \"" + std::to_string(index) + "\") to read its legend";
                    else if (!shown)
                        out["note"] = "The Preview has not loaded this plate's toolpaths; show it with set_view {\"tab\": \"preview\"}";
                }
                return out;
            }));
        }));

    // The legend combo's view types; the first-layer inspection type has no name and no combo entry.
    std::vector<std::string> view_types;
    for (int i = 0; i < int(libvgcode::EViewType::COUNT); ++i)
        if (std::string name = GCodeViewer::view_type_to_config_name(libvgcode::EViewType(i)); !name.empty())
            view_types.push_back(std::move(name));

    std::vector<std::string> tabs{"home", "prepare", "preview", "device", "multi_device", "project", "calibration"};
#ifdef SLIC3R_CAD
    tabs.insert(tabs.begin() + 1, "design");
#endif

    reg.add(mutate_tool(
        "set_view", L("Point the view"),
        "Set up what the window shows, for screenshots and GUI work: the main tab, the 3D view (Prepare's, the preview or "
        "the assemble view, which has its own gizmos), the 3D camera (a named direction, the default view, or fit to bed / "
        "objects / selection, then orbit to look from between the named directions; the reply gives the elevation and "
        "azimuth it ends at), the preview's color scheme (picked as in the legend's combo, which shows every feature type "
        "again), legend rows, time estimate and layers (1-based, or up to a height), and the window size. Only the given "
        "fields change. On the Design tab the camera points its view (default is its Home view; no fit_bed). The reply "
        "echoes them; preview layers as [first, last, count] with z, the heights of first and last.",
        object_schema({{"tab", enum_prop(tabs, "Main window tab.")},
                       {"view", enum_prop({"3d", "preview", "assemble"}, "3D view: Prepare's, the preview, or the assemble view (needs an object; leave it with 3d, as its Return button does).")},
                       {"camera", enum_prop({"iso", "top", "bottom", "front", "rear", "left", "right", "default", "fit_bed", "fit_objects", "fit_selection"},
                                            "Camera direction or fit.")},
                       {"orbit", array_prop({{"type", "number"}}, "[yaw, pitch] in degrees: move the camera around what it looks at, "
                                                                 "yaw to its right and pitch up (negative looks from lower), after "
                                                                 "camera. The same on every install, unlike a mouse drag.")},
                       {"layer", {{"type", "integer"}, {"minimum", 1}, {"description", "Preview: show layers up to this one (1-based)."}}},
                       {"layer_range", array_prop({{"type", "integer"}}, "Preview: [first, last] layers, 1-based.")},
                       {"layer_z", {{"type", "number"}, {"minimum", 0}, {"description", "Preview: show layers up to the first whose top is at or above this height (mm), as the slider places a custom G-code."}}},
                       {"view_type", enum_prop(view_types, "Preview color scheme.")},
                       {"legend", {{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}},
                                   {"description", "Preview: show (true) or hide (false) legend rows: feature types by name as get_preview lists them "
                                                   "(\"Sparse infill\"), and travel, wipe, retract, unretract, seams, filament_changes, color_changes, "
                                                   "pauses, custom_gcode, in any case and with _ or a space between words. Hiding a row this "
                                                   "print lacks does nothing. Applied after view_type."}}},
                       {"time_mode", enum_prop({"normal", "silent"}, "Preview: the time estimate the legend and layer times show; silent only when the print has its own.")},
                       {"window_size", array_prop({{"type", "integer"}}, "[width, height] of the main window in pixels.")}}),
        [](const json& args, ToolContext& ctx) {
            const bool preview_args = args.contains("layer") || args.contains("layer_range") || args.contains("layer_z") ||
                                      args.contains("view_type") || args.contains("legend") || args.contains("time_mode");
            if (arg_string(args, "tab") == "design" && (args.contains("view") || preview_args))
                throw ToolFailure("view and the Preview fields show Prepare or Preview; give them without tab design");
            json out = ctx.run_ui([&]() -> json {
                Plater&    plater = ui_plater();
                MainFrame* frame  = wxGetApp().mainframe;
                json       done   = json::object();
                if (args.contains("window_size")) {
                    const json& s = args["window_size"];
                    if (!s.is_array() || s.size() != 2)
                        throw ToolFailure("window_size must be [width, height]");
                    if (frame->IsMaximized())
                        frame->Maximize(false);
                    frame->SetSize(wxSize(s[0].get<int>(), s[1].get<int>()));
                    done["window_size"] = {frame->GetSize().x, frame->GetSize().y};
                }
                if (args.contains("tab")) {
                    const char* id = tab_id(arg_string(args, "tab"));
                    if (id == nullptr)
                        throw ToolFailure("Unknown tab");
#ifdef SLIC3R_CAD
                    if (id == std::string(TAB_ID_DESIGN) && frame->m_design_page == nullptr)
                        throw ToolFailure("The Design tab is off; enable the CAD feature in Preferences and restart");
#endif
                    frame->select_tab(wxString(id));
                    done["tab"] = arg_string(args, "tab");
                }
                if (args.contains("view")) {
                    const std::string view     = arg_string(args, "view");
                    GLCanvas3D*       assemble = plater.get_assmeble_canvas3D();
                    const bool        in_assemble = assemble != nullptr && plater.get_current_canvas3D() == assemble;
                    if (view == "assemble") {
                        if (!plater.has_assmeble_view())
                            throw ToolFailure("The assemble view needs an object on the plate");
                        frame->select_tab(wxString(TAB_ID_PREPARE));
                        plater.select_view_3D("Assemble");
                    } else if (in_assemble) {
                        // What the assemble view's Return button does.
                        assemble->get_gizmos_manager().reset_all_states();
                        plater.select_view_3D("3D");
                        if (GLCanvas3D* view3d = plater.get_view3D_canvas3D()) {
                            view3d->get_gizmos_manager().reset_all_states();
                            view3d->reload_scene(true);
                        }
                    }
                    if (view == "3d")
                        frame->select_tab(wxString(TAB_ID_PREPARE));
                    else if (view == "preview")
                        frame->select_tab(wxString(TAB_ID_PREVIEW));
                    done["view"] = view;
                }
                if (preview_args && !args.contains("tab") && !args.contains("view")) {
                    plater.select_view_3D("Preview", false);
                    frame->select_tab(wxString(TAB_ID_PREVIEW));
                }
                if (preview_args) {
                    GLCanvas3D* canvas = plater.get_preview_canvas3D();
                    if (canvas == nullptr)
                        throw ToolFailure("The preview is not available");
                    GCodeViewer& viewer = canvas->get_gcode_viewer();
                    if (args.contains("layer_z") && (args.contains("layer") || args.contains("layer_range")))
                        throw ToolFailure("Give layer_z or layer/layer_range, not both");
                    if ((args.contains("legend") || args.contains("time_mode")) && !viewer.has_data())
                        throw ToolFailure("legend and time_mode need the Preview's toolpaths; slice first");
                    const std::function<json()> show_legend = args.contains("legend") ? legend_change(viewer, args["legend"]) : nullptr;
                    const bool silent = arg_string(args, "time_mode") == "silent";
                    if (silent && !viewer.can_switch_time_mode())
                        throw ToolFailure("This print has no separate silent-mode estimate");
                    // In this order, as a person would: picking Line Type shows every feature type again.
                    if (args.contains("view_type")) {
                        libvgcode::EViewType type;
                        if (!GCodeViewer::view_type_from_config_name(arg_string(args, "view_type"), type))
                            throw ToolFailure("Unknown view_type");
                        // Without toolpaths there is no legend to pick from; the type then applies to the next ones.
                        if (viewer.has_data())
                            viewer.pick_view_type(type);
                        else
                            viewer.select_view_type(type);
                        done["view_type"] = arg_string(args, "view_type");
                    }
                    if (show_legend)
                        done["legend"] = show_legend();
                    if (args.contains("time_mode")) {
                        const libvgcode::ETimeMode mode = silent ? libvgcode::ETimeMode::Stealth : libvgcode::ETimeMode::Normal;
                        if (viewer.get_viewer().get_time_mode() != mode)
                            viewer.set_time_mode(mode);
                        done["time_mode"] = arg_string(args, "time_mode");
                    }
                    if (args.contains("layer") || args.contains("layer_range") || args.contains("layer_z")) {
                        IMSlider* layers = viewer.get_layers_slider();
                        if (layers == nullptr || layers->GetMaxValue() <= 0)
                            throw ToolFailure("The preview has no layers; slice first");
                        const int max = layers->GetMaxValue();
                        int       lo = 0, hi = max;
                        if (args.contains("layer"))
                            hi = std::clamp(args["layer"].get<int>() - 1, 0, max);
                        if (args.contains("layer_range")) {
                            const json& r = args["layer_range"];
                            if (!r.is_array() || r.size() != 2)
                                throw ToolFailure("layer_range must be [first, last]");
                            lo = std::clamp(r[0].get<int>() - 1, 0, max);
                            hi = std::clamp(r[1].get<int>() - 1, lo, max);
                        }
                        if (args.contains("layer_z")) {
                            // The slider's own rule for a height, which set_layer_gcode's z follows too.
                            const double z = args["layer_z"].get<double>();
                            hi             = layers->get_tick_from_value(z, true);
                            if (hi < 0 || hi > max)
                                throw ToolFailure("layer_z " + json(z).dump() + " is above the top layer (" +
                                                  json(rounded(layers->GetMaxValueD(), 1000.)).dump() + " mm)");
                        }
                        layers->SetHigherValue(hi);
                        layers->SetLowerValue(lo);
                        layers->set_as_dirty();
                        if (IMSlider* moves = viewer.get_moves_slider()) {
                            moves->SetHigherValue(moves->GetMaxValue());
                            moves->set_as_dirty();
                        }
                        done["layers"] = {lo + 1, hi + 1, max + 1};
                        done["z"]      = {rounded(layers->GetLowerValueD(), 1000.), rounded(layers->GetHigherValueD(), 1000.)};
                    }
                    canvas->set_as_dirty();
                }
                if (GLCanvas3D* canvas = plater.get_current_canvas3D())
                    canvas->set_as_dirty();
                return done;
            });
            if (args.contains("camera")) {
                // A tab's page shows, and the Design tab takes the shared camera, once the tab change is
                // handled; the camera then points the view on screen.
                settle(ctx);
                out["camera"] = ctx.run_ui([&]() -> json {
                    Plater&           plater = ui_plater();
                    const std::string cam    = arg_string(args, "camera");
#ifdef SLIC3R_CAD
                    if (DesignPanel* design = wxGetApp().mainframe->shown_design_panel()) {
                        DesignCanvas* view = design->mcp_viewport();
                        if (cam == "fit_bed")
                            throw ToolFailure("fit_bed is not offered in the Design tab");
                        if (cam == "fit_objects")
                            view->fit_view();
                        else if (cam == "fit_selection") {
                            if (!view->zoom_to_box(view->selection_box()))
                                throw ToolFailure("Nothing is selected in the Design tab");
                        } else
                            view->set_view(cam == "default" ? "iso" : cam); // default: the tab's Home view
                        return cam;
                    }
#endif
                    GLCanvas3D* canvas = plater.get_current_canvas3D();
                    if (cam == "default") {
                        plater.select_view("plate");
                        if (canvas)
                            canvas->zoom_to_bed();
                    } else if (cam == "fit_bed") {
                        if (canvas)
                            canvas->zoom_to_bed();
                    } else if (cam == "fit_objects") {
                        if (canvas)
                            canvas->zoom_to_volumes();
                    } else if (cam == "fit_selection") {
                        if (canvas)
                            canvas->zoom_to_selection();
                    } else {
                        plater.select_view(cam);
                    }
                    if (canvas)
                        canvas->set_as_dirty();
                    return cam;
                });
            }
            if (args.contains("orbit")) {
                if (args["orbit"].size() != 2)
                    throw ToolFailure("orbit is [yaw, pitch] in degrees");
                out["orbit"] = ctx.run_ui([&]() -> json {
#ifdef SLIC3R_CAD
                    if (wxGetApp().mainframe->shown_design_panel() != nullptr)
                        throw ToolFailure("orbit turns the Prepare and Preview camera; on the Design tab give camera a direction");
#endif
                    Plater& plater = ui_plater();
                    json    placed = orbit_camera(plater.get_camera(), args["orbit"][0].get<double>(), args["orbit"][1].get<double>());
                    if (GLCanvas3D* canvas = plater.get_current_canvas3D())
                        canvas->set_as_dirty();
                    return placed;
                });
            }
            if (out.value("view", "") == "assemble")
                // A canvas shown for the first time loads its scene at its first frame, once its GL
                // surface exists (a moment later on Wayland), and the next call may act on its objects.
                poll_ui(
                    ctx,
                    []() -> json {
                        const GLCanvas3D* assemble = ui_plater().get_assmeble_canvas3D();
                        return assemble != nullptr && assemble->is_initialized() && !assemble->is_reload_delayed() ? json(true) : json();
                    },
                    std::chrono::seconds(15), "the assemble view to show its objects");
            settle(ctx);
            return ToolResult::ok(out);
        }));
}

}}} // namespace Slic3r::GUI::Mcp
