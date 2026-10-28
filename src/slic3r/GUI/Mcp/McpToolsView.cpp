// screenshot and set_view: what the agent looks at, and pointing the view before looking.

#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "ModalTracker.hpp"
#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/toplevel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <string>
#include <thread>

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

std::string timestamp_name()
{
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t   = std::chrono::system_clock::to_time_t(now);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return std::string("screenshot-") + buf + "-" + std::to_string(ms) + ".png";
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
    return nullptr;
}

} // namespace

void register_view_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef screenshot = inspect_tool(
        "screenshot", "Take a screenshot",
        "Capture what OrcaSlicer shows and return it as an image, downscaled to max_dim on the long edge (default 1280), "
        "plus the full-size PNG written to save_path (or the session's output folder). target: \"window\" (the main "
        "window), \"canvas\" (the visible 3D view exactly as rendered: scene, toolpaths, toolbars, ImGui panels), "
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
                path = service.output_dir() / timestamp_name();
            }
            const int max_dim = std::clamp(arg_int(args, "max_dim", 1280), 256, 2000);
            json      info;
            wxImage   full;
            if (target != "window" && target != "canvas" && target != "dialog")
                // Scrolled into view first, and painted before the capture below runs.
                ctx.run_ui([&] {
                    service.inspector().reveal(service.inspector().resolve(target_arg));
                    return json();
                });
            // A canvas that comes back blank has not drawn yet; it does within moments.
            bool blank = false;
            for (int attempt = 0;; ++attempt) {
                ctx.run_ui([&] {
                    std::string             err;
                    std::optional<Capture>  cap;
                    if (target == "canvas") {
                        Plater&     plater = ui_plater();
                        GLCanvas3D* canvas = plater.get_current_canvas3D();
                        if (canvas == nullptr)
                            throw ToolFailure("No 3D view is shown; switch to Prepare or Preview (set_view tab)");
                        cap = capture_canvas(*canvas, err);
                    } else if (target == "dialog") {
                        wxDialog* top = service.modals().top();
                        if (top == nullptr)
                            throw ToolFailure("No modal dialog is open");
                        cap = capture_window(*top, err);
                    } else if (target != "window") {
                        // A widget: its top-level window, cropped to the widget unless it is that window.
                        const UiTarget t = service.inspector().resolve(target_arg);
                        locator          = t.locator;
                        wxWindow*      w = t.window.get();
                        if (t.kind != UiTarget::Kind::Window || w == nullptr)
                            throw ToolFailure("target must be window, canvas, dialog or the ref of a widget");
                        if (GLCanvas3D* canvas = service.inspector().canvas_of(&t); canvas != nullptr && canvas->get_wxglcanvas() == w) {
                            cap = capture_canvas(*canvas, err);
                        } else {
                            auto* tlw = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(w));
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
            if (!full.SaveFile(wxString::FromUTF8(path.string()), wxBITMAP_TYPE_PNG))
                throw ToolFailure("Could not write " + path.string());
            info["path"]   = path.string();
            ToolResult out = ToolResult::ok(info);
            if (arg_bool(args, "include_image", true)) {
                const wxSize  size  = fit_within(full.GetSize(), max_dim);
                const wxImage small = size == full.GetSize() ? full : full.Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH);
                out.data["image_width"]  = size.x;
                out.data["image_height"] = size.y;
                out.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", base64(encode_png(small))}});
            }
            return out;
        });
    screenshot.journal = true; // a replay takes the same screenshots
    reg.add(screenshot);

    std::vector<std::string> view_types;
    for (int i = 0; i < int(libvgcode::EViewType::COUNT); ++i)
        view_types.push_back(GCodeViewer::view_type_to_config_name(libvgcode::EViewType(i)));

    reg.add(mutate_tool(
        "set_view", "Point the view",
        "Set up what the window shows, for screenshots and GUI work: the main tab, the 3D camera (a named direction, the "
        "default view, or fit to bed / objects / selection), the preview's layer or layer range (1-based) and color "
        "scheme, and the window size. Only the given fields change.",
        object_schema({{"tab", enum_prop({"home", "prepare", "preview", "device", "multi_device", "project", "calibration"}, "Main window tab.")},
                       {"camera", enum_prop({"iso", "top", "bottom", "front", "rear", "left", "right", "default", "fit_bed", "fit_objects", "fit_selection"},
                                            "Camera direction or fit.")},
                       {"layer", {{"type", "integer"}, {"minimum", 1}, {"description", "Preview: show layers up to this one (1-based)."}}},
                       {"layer_range", array_prop({{"type", "integer"}}, "Preview: [first, last] layers, 1-based.")},
                       {"view_type", enum_prop(view_types, "Preview color scheme.")},
                       {"window_size", array_prop({{"type", "integer"}}, "[width, height] of the main window in pixels.")}}),
        [](const json& args, ToolContext& ctx) {
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
                    frame->select_tab(wxString(id));
                    done["tab"] = arg_string(args, "tab");
                }
                const bool preview_args = args.contains("layer") || args.contains("layer_range") || args.contains("view_type");
                if (preview_args && !args.contains("tab")) {
                    plater.select_view_3D("Preview", false);
                    frame->select_tab(wxString(TAB_ID_PREVIEW));
                }
                if (args.contains("camera")) {
                    const std::string cam    = arg_string(args, "camera");
                    GLCanvas3D*       canvas = plater.get_current_canvas3D();
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
                    done["camera"] = cam;
                }
                if (preview_args) {
                    GLCanvas3D* canvas = plater.get_preview_canvas3D();
                    if (canvas == nullptr)
                        throw ToolFailure("The preview is not available");
                    GCodeViewer& viewer = canvas->get_gcode_viewer();
                    if (args.contains("view_type")) {
                        libvgcode::EViewType type;
                        if (!GCodeViewer::view_type_from_config_name(arg_string(args, "view_type"), type))
                            throw ToolFailure("Unknown view_type");
                        viewer.select_view_type(type);
                        done["view_type"] = arg_string(args, "view_type");
                    }
                    if (args.contains("layer") || args.contains("layer_range")) {
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
                        layers->SetHigherValue(hi);
                        layers->SetLowerValue(lo);
                        layers->set_as_dirty();
                        if (IMSlider* moves = viewer.get_moves_slider()) {
                            moves->SetHigherValue(moves->GetMaxValue());
                            moves->set_as_dirty();
                        }
                        done["layers"] = {lo + 1, hi + 1, max + 1};
                    }
                    canvas->set_as_dirty();
                }
                if (GLCanvas3D* canvas = plater.get_current_canvas3D())
                    canvas->set_as_dirty();
                return done;
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
}

}}} // namespace Slic3r::GUI::Mcp
