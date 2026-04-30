#include "MCP/CommandDispatch.h"
#include "MCP/ToolSchemas.h"
#include "GUI/GUI_App.hpp"
#include "GUI/MainFrame.hpp"
#include "GUI/Plater.hpp"
#include "GUI/GLCanvas3D.hpp"
#include "GUI/Camera.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <miniz.h>
#include <GL/glew.h>

#include <future>
#include <set>
#include <wx/app.h>
#include "GUI/OpenGLManager.hpp"
#include "GUI/Tab.hpp"

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// Helper: build a JSON response
static McpApiServer::Response json_response(const std::string& body, int status = 200) {
    McpApiServer::Response resp;
    resp.status_code = status;
    resp.status_text = (status == 200) ? "OK" : "Bad Request";
    resp.content_type = "application/json";
    resp.body = body;
    return resp;
}

CommandDispatch& CommandDispatch::instance() {
    static CommandDispatch inst;
    return inst;
}

CommandDispatch::json CommandDispatch::call_on_gui_thread(std::function<json()> fn) {
    std::promise<json> promise;
    auto future = promise.get_future();
    wxGetApp().CallAfter([&promise, fn = std::move(fn)]() {
        try {
            promise.set_value(fn());
        } catch (const std::exception& e) {
            json err;
            err["error"] = e.what();
            promise.set_value(err);
        }
    });
    return future.get();
}

void CommandDispatch::register_command(const std::string& action, handler_fn fn) {
    m_commands[action] = std::move(fn);
}

CommandDispatch::json CommandDispatch::dispatch(const std::string& action, const json& params) {
    auto it = m_commands.find(action);
    if (it == m_commands.end()) {
        return json{{"error", "Unknown action: " + action}};
    }
    return it->second(params);
}

static int parse_query_int(const std::string& url, const std::string& key, int default_val) {
    auto pos = url.find(key + "=");
    if (pos == std::string::npos) return default_val;
    pos += key.size() + 1;
    auto end = url.find('&', pos);
    std::string val = (end == std::string::npos) ? url.substr(pos) : url.substr(pos, end - pos);
    try { return std::stoi(val); } catch (...) { return default_val; }
}

McpApiServer::Response CommandDispatch::handle_screenshot(const std::string& url) {
    std::string mode = "viewport";
    {
        auto pos = url.find("mode=");
        if (pos != std::string::npos) {
            auto end = url.find('&', pos + 5);
            mode = (end == std::string::npos) ? url.substr(pos + 5) : url.substr(pos + 5, end - pos - 5);
        }
    }

    int req_width = parse_query_int(url, "width", 0);
    int req_height = parse_query_int(url, "height", 0);

    std::vector<unsigned char> png_bytes;
    std::string error_msg;
    bool ok = false;

    call_on_gui_thread([&]() -> json {
        auto* plater = wxGetApp().plater();
        if (!plater) { error_msg = "No plater"; return json{}; }

        auto* canvas = plater->get_view3D_canvas3D();
        if (!canvas) { error_msg = "No canvas"; return json{}; }

        // Ensure GL context is current and scene is rendered
        canvas->set_as_dirty();
        canvas->render();

        if (mode == "thumbnail") {
            // Legacy FBO thumbnail mode
            int tw = (req_width > 0) ? std::max(64, std::min(req_width, 2048)) : 800;
            int th = (req_height > 0) ? std::max(64, std::min(req_height, 2048)) : 600;

            ThumbnailData thumb;
            auto& plate_list = plater->get_partplate_list();
            ThumbnailsParams params = { {}, false, true, true, true,
                                        plate_list.get_curr_plate_index() };
            canvas->render_thumbnail(thumb, (unsigned)tw, (unsigned)th,
                                     params, Camera::EType::Ortho);

            if (!thumb.is_valid() || thumb.pixels.empty()) {
                error_msg = "Thumbnail render produced no data";
                return json{};
            }

            size_t png_size = 0;
            void* png_data = tdefl_write_image_to_png_file_in_memory_ex(
                thumb.pixels.data(), thumb.width, thumb.height, 4,
                &png_size, MZ_DEFAULT_LEVEL, 1);

            if (png_data && png_size > 0) {
                png_bytes.assign((unsigned char*)png_data,
                                 (unsigned char*)png_data + png_size);
                mz_free(png_data);
                ok = true;
            } else {
                error_msg = "PNG encoding failed";
            }
        } else {
            // Default: capture actual viewport via glReadPixels
            Size cnv_size = canvas->get_canvas_size();
            int vp_w = cnv_size.get_width();
            int vp_h = cnv_size.get_height();

            if (vp_w <= 0 || vp_h <= 0) {
                error_msg = "Canvas size is zero";
                return json{};
            }

            // Unbind any FBO to ensure we read from the default framebuffer
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));

            std::vector<unsigned char> pixels(vp_w * vp_h * 4);
            glsafe(::glReadPixels(0, 0, vp_w, vp_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));

            // Encode to PNG (flip=1 for GL bottom-up to top-down)
            size_t png_size = 0;
            void* png_data = tdefl_write_image_to_png_file_in_memory_ex(
                pixels.data(), vp_w, vp_h, 4,
                &png_size, MZ_DEFAULT_LEVEL, 1);

            if (png_data && png_size > 0) {
                png_bytes.assign((unsigned char*)png_data,
                                 (unsigned char*)png_data + png_size);
                mz_free(png_data);
                ok = true;
            } else {
                error_msg = "PNG encoding failed";
            }
        }
        return json{};
    });

    if (!ok) {
        json resp;
        resp["ok"] = false;
        resp["error"] = error_msg.empty() ? "Screenshot capture failed" : error_msg;
        return json_response(resp.dump(), 500);
    }

    McpApiServer::Response resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.content_type = "image/png";
    resp.is_binary = true;
    resp.binary_body = std::move(png_bytes);
    return resp;
}

McpApiServer::Response CommandDispatch::handle_api_request(
    const std::string& method, const std::string& url, const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    // MCP protocol endpoint
    if (method == "POST" && url == "/mcp") {
        return m_mcp_protocol.handle_mcp_request(body, headers);
    }

    // GET /api/screenshot?width=W&height=H
    if (url.find("/api/screenshot") == 0) {
        return handle_screenshot(url);
    }

    // POST /api/execute
    if (url.find("/api/execute") == 0 && method == "POST") {
        try {
            auto req = json::parse(body);
            std::string action = req.value("action", "");
            json params = req.value("params", json::object());

            json result = dispatch(action, params);

            if (result.contains("error") && !result.contains("ok")) {
                json resp;
                resp["ok"] = false;
                resp["error"] = result["error"];
                return json_response(resp.dump(), 400);
            }

            json resp;
            resp["ok"] = true;
            resp["result"] = result;
            return json_response(resp.dump());
        } catch (const json::exception& e) {
            json resp;
            resp["ok"] = false;
            resp["error"] = std::string("JSON parse error: ") + e.what();
            return json_response(resp.dump(), 400);
        }
    }

    // Unknown route
    json resp;
    resp["ok"] = false;
    resp["error"] = "Not found: " + url;
    return json_response(resp.dump(), 404);
}

// ---------------------------------------------------------------------------
// Model commands
// ---------------------------------------------------------------------------
void CommandDispatch::register_model_commands() {
    register_command("model.list", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            const auto& model = plater->model();
            json objects = json::array();
            for (size_t i = 0; i < model.objects.size(); ++i) {
                const auto* obj = model.objects[i];
                json o;
                o["index"] = i;
                o["name"] = obj->name;
                o["volumes"] = obj->volumes.size();
                auto bb = obj->bounding_box_approx();
                o["bounding_box"] = {
                    {"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                    {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}
                };
                objects.push_back(o);
            }
            return json{{"objects", objects}};
        });
    });

    register_command("model.add_primitive", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            std::string type = params.value("type", "cube");
            double x = params.value("x", 10.0);
            double y = params.value("y", 10.0);
            double z = params.value("z", 10.0);

            TriangleMesh mesh;
            if (type == "cube")
                mesh = make_cube(x, y, z);
            else if (type == "cylinder")
                mesh = make_cylinder(x, z); // x=radius, z=height
            else if (type == "sphere")
                mesh = make_sphere(x);      // x=radius
            else
                return json{{"error", "Unknown primitive type: " + type}};

            auto* plater = wxGetApp().plater();
            auto& model = plater->model();

            ModelObject* obj = model.add_object();
            obj->name = type + "_" + std::to_string(model.objects.size());
            obj->add_volume(std::move(mesh));
            obj->add_instance();

            // Reload the 3D scene so GL volumes are created
            plater->get_view3D_canvas3D()->reload_scene(true, true);

            json result;
            result["object_index"] = model.objects.size() - 1;
            result["name"] = obj->name;
            return result;
        });
    });

    register_command("model.load_file", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            std::string path = params.value("path", "");
            if (path.empty())
                return json{{"error", "No path provided"}};

            auto* plater = wxGetApp().plater();
            std::vector<std::string> paths = {path};
            auto loaded = plater->load_files(paths);
            if (loaded.empty())
                return json{{"error", "Failed to load file: " + path}};

            json result;
            result["loaded_objects"] = loaded.size();
            return result;
        });
    });

    register_command("model.delete", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            int index = params.value("index", -1);
            auto* plater = wxGetApp().plater();
            if (index < 0 || index >= (int)plater->model().objects.size())
                return json{{"error", "Invalid object index"}};
            plater->delete_object_from_model(index);
            return json{{"deleted", index}};
        });
    });

    register_command("model.transform", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            int index = params.value("index", -1);
            auto* plater = wxGetApp().plater();
            auto& model = plater->model();
            if (index < 0 || index >= (int)model.objects.size())
                return json{{"error", "Invalid object index"}};

            auto* obj = model.objects[index];
            if (obj->instances.empty())
                return json{{"error", "Object has no instances"}};

            auto* inst = obj->instances[0];

            if (params.contains("translate")) {
                auto t = params["translate"];
                inst->set_offset(Vec3d(t[0].get<double>(), t[1].get<double>(), t[2].get<double>()));
            }
            if (params.contains("scale")) {
                auto s = params["scale"];
                inst->set_scaling_factor(Vec3d(s[0].get<double>(), s[1].get<double>(), s[2].get<double>()));
            }
            if (params.contains("rotate")) {
                auto r = params["rotate"];
                inst->set_rotation(Vec3d(r[0].get<double>(), r[1].get<double>(), r[2].get<double>()));
            }

            obj->invalidate_bounding_box();
            plater->update();

            return json{{"transformed", index}};
        });
    });

    register_command("model.export", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            std::string path = params.value("path", "");
            std::string format = params.value("format", "3mf");
            if (path.empty())
                return json{{"error", "No path provided"}};

            auto* plater = wxGetApp().plater();
            if (format == "3mf") {
                int result = plater->export_3mf(boost::filesystem::path(path));
                if (result < 0)
                    return json{{"error", "Failed to export 3MF"}};
                return json{{"exported", path}, {"format", "3mf"}};
            } else if (format == "stl") {
                plater->export_stl();
                return json{{"exported", path}, {"format", "stl"}};
            }
            return json{{"error", "Unknown format: " + format}};
        });
    });
}

// ---------------------------------------------------------------------------
// Config commands
// ---------------------------------------------------------------------------
void CommandDispatch::register_config_commands() {
    register_command("config.get", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            auto config = wxGetApp().preset_bundle->full_config();
            json result;
            if (!params.contains("keys"))
                return json{{"error", "No keys provided"}};
            for (const auto& key : params["keys"]) {
                std::string k = key.get<std::string>();
                if (config.has(k)) {
                    result[k] = config.opt_serialize(k);
                }
            }
            return result;
        });
    });

    register_command("config.set", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            const json& settings = unwrap_config_settings(params);
            DynamicPrintConfig new_config;
            for (auto it = settings.begin(); it != settings.end(); ++it) {
                const std::string& key = it.key();
                const std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                if (print_config_def.has(key)) {
                    new_config.set_deserialize_strict(key, val);
                }
            }

            // Apply to the appropriate Tab so changes persist in the preset system.
            // Try print tab first (most settings), then filament, then printer.
            for (auto type : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER}) {
                Tab* tab = wxGetApp().get_tab(type);
                if (!tab) continue;
                DynamicPrintConfig tab_subset;
                for (auto& opt_key : new_config.keys()) {
                    if (tab->get_config()->has(opt_key))
                        tab_subset.set_key_value(opt_key, new_config.option(opt_key)->clone());
                }
                if (!tab_subset.empty())
                    tab->load_config(tab_subset);
            }

            return json{{"updated", true}};
        });
    });

    register_command("config.list", [](const json& params) -> json {
        std::string filter = params.value("filter", "");
        json result = json::array();
        for (const auto& [key, def] : print_config_def.options) {
            if (!filter.empty() && key.find(filter) == std::string::npos)
                continue;
            json entry;
            entry["key"] = key;
            entry["type"] = static_cast<int>(def.type);
            if (!def.tooltip.empty())
                entry["tooltip"] = def.tooltip;
            result.push_back(entry);
        }
        return json{{"options", result}};
    });

    register_command("config.load_profile", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            std::string name = params.value("name", "");
            std::string path = params.value("path", "");

            if (name.empty() && path.empty())
                return json{{"error", "Provide either 'name' or 'path'"}};

            auto* preset_bundle = wxGetApp().preset_bundle;
            if (!name.empty()) {
                bool found = false;
                for (auto* collection : {
                    &preset_bundle->prints,
                    &preset_bundle->filaments
                }) {
                    const Preset* preset = collection->find_preset(name);
                    if (preset != nullptr) {
                        collection->select_preset_by_name(name, true);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    const Preset* preset = preset_bundle->printers.find_preset(name);
                    if (preset != nullptr) {
                        preset_bundle->printers.select_preset_by_name(name, true);
                        found = true;
                    }
                }
                if (!found)
                    return json{{"error", "Profile not found: " + name}};

                wxGetApp().plater()->on_config_change(preset_bundle->full_config());
                return json{{"loaded", name}};
            }

            return json{{"error", "Loading profiles from file path is not yet supported"}};
        });
    });
}

// ---------------------------------------------------------------------------
// Diagnostics commands
// ---------------------------------------------------------------------------
void CommandDispatch::register_diagnostics_commands() {
    register_command("diagnostics.state", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            const auto& model = plater->model();
            auto* preset_bundle = wxGetApp().preset_bundle;

            json result;
            result["object_count"] = model.objects.size();

            // Object details
            json objects = json::array();
            for (size_t i = 0; i < model.objects.size(); ++i) {
                const auto* obj = model.objects[i];
                json o;
                o["index"] = i;
                o["name"] = obj->name;
                auto bb = obj->bounding_box_approx();
                o["bounding_box"] = {
                    {"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                    {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}
                };
                objects.push_back(o);
            }
            result["objects"] = objects;

            // Plate info
            auto config = preset_bundle->full_config();
            json plate;
            plate["index"] = plater->get_partplate_list().get_curr_plate_index();
            plate["printable_area"] = config.opt_serialize("printable_area");
            result["plate"] = plate;

            // Camera info
            const auto& camera = plater->get_camera();
            json cam;
            cam["type"] = camera.get_type_as_string();
            auto pos = camera.get_position();
            cam["position"] = {pos.x(), pos.y(), pos.z()};
            auto target = const_cast<Camera&>(plater->get_camera()).get_target();
            cam["target"] = {target.x(), target.y(), target.z()};
            cam["zoom"] = camera.get_zoom();
            result["camera"] = cam;

            // Active profiles
            result["printer"] = preset_bundle->printers.get_selected_preset().name;
            result["filament"] = preset_bundle->filaments.get_selected_preset().name;
            result["process"] = preset_bundle->prints.get_selected_preset().name;

            // Status
            result["slicing"] = plater->is_background_process_slicing();

            return result;
        });
    });

    register_command("diagnostics.mesh_stats", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            int index = params.value("index", -1);
            auto* plater = wxGetApp().plater();
            const auto& model = plater->model();
            if (index < 0 || index >= (int)model.objects.size())
                return json{{"error", "Invalid object index"}};

            const auto* obj = model.objects[index];
            json result;
            result["name"] = obj->name;
            result["volumes"] = json::array();
            for (size_t vi = 0; vi < obj->volumes.size(); ++vi) {
                const auto* vol = obj->volumes[vi];
                const auto& stats = vol->mesh().stats();
                json vs;
                vs["index"] = vi;
                vs["name"] = vol->name;
                vs["facets"] = stats.number_of_facets;
                vs["volume"] = stats.volume;
                vs["open_edges"] = stats.open_edges;
                result["volumes"].push_back(vs);
            }
            auto bb = obj->bounding_box_approx();
            result["bounding_box"] = {
                {"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}
            };
            return result;
        });
    });

    register_command("diagnostics.validate", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            const auto& print = plater->fff_print();
            StringObjectException warning;
            auto err = print.validate(&warning);
            json result;
            result["valid"] = err.string.empty();
            if (!err.string.empty())
                result["error"] = err.string;
            if (!warning.string.empty())
                result["warning"] = warning.string;
            return result;
        });
    });

    register_command("diagnostics.slice", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            plater->reslice();
            return json{{"slicing", "started"}};
        });
    });

    register_command("diagnostics.slice_status", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            const auto& print = plater->fff_print();
            bool slicing = plater->is_background_process_slicing();
            bool finished = print.finished();

            json result;
            result["slicing"] = slicing;
            result["finished"] = finished;

            const auto& stats = print.print_statistics();
            if (!stats.estimated_normal_print_time.empty()) {
                result["print_time"] = stats.estimated_normal_print_time;
                result["filament_used_mm"] = stats.total_used_filament;
                result["filament_used_cm3"] = stats.total_extruded_volume;
                result["filament_cost"] = stats.total_cost;
                result["filament_weight_g"] = stats.total_weight;
                result["total_toolchanges"] = stats.total_toolchanges;
            }

            // Check for validation errors
            StringObjectException warning;
            auto err = print.validate(&warning);
            if (!err.string.empty())
                result["error"] = err.string;
            if (!warning.string.empty())
                result["warning"] = warning.string;

            return result;
        });
    });
}

// ---------------------------------------------------------------------------
// Viewport commands
// ---------------------------------------------------------------------------
void CommandDispatch::register_viewport_commands() {
    register_command("viewport.select_view", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            std::string view = params.value("view", "");
            if (view.empty())
                return json{{"error", "No view specified"}};

            static const std::set<std::string> valid_views = {
                "front", "rear", "top", "bottom", "left", "right", "iso", "topfront"
            };
            if (valid_views.find(view) == valid_views.end())
                return json{{"error", "Invalid view: " + view + ". Valid: front, rear, top, bottom, left, right, iso, topfront"}};

            auto* canvas = wxGetApp().plater()->get_view3D_canvas3D();
            if (!canvas) return json{{"error", "No canvas"}};

            canvas->select_view(view);
            canvas->set_as_dirty();
            canvas->render();

            return json{{"view", view}};
        });
    });

    register_command("viewport.zoom_to_bed", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* canvas = wxGetApp().plater()->get_view3D_canvas3D();
            if (!canvas) return json{{"error", "No canvas"}};

            canvas->zoom_to_bed();
            canvas->set_as_dirty();
            canvas->render();

            return json{{"zoomed", "bed"}};
        });
    });

    register_command("viewport.zoom_to_volumes", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* canvas = wxGetApp().plater()->get_view3D_canvas3D();
            if (!canvas) return json{{"error", "No canvas"}};

            canvas->zoom_to_volumes();
            canvas->set_as_dirty();
            canvas->render();

            return json{{"zoomed", "volumes"}};
        });
    });

    register_command("viewport.zoom", [this](const json& params) -> json {
        return call_on_gui_thread([&]() -> json {
            double delta = params.value("delta", 0.0);
            if (delta == 0.0)
                return json{{"error", "No delta specified"}};

            auto& camera = wxGetApp().plater()->get_camera();
            camera.update_zoom(delta);

            auto* canvas = wxGetApp().plater()->get_view3D_canvas3D();
            if (canvas) {
                canvas->set_as_dirty();
                canvas->render();
            }

            return json{{"zoom", camera.get_zoom()}};
        });
    });

    register_command("viewport.camera_info", [this](const json& /*params*/) -> json {
        return call_on_gui_thread([&]() -> json {
            auto* plater = wxGetApp().plater();
            const auto& camera = plater->get_camera();
            auto* canvas = plater->get_view3D_canvas3D();

            json result;
            result["type"] = camera.get_type_as_string();
            auto pos = camera.get_position();
            result["position"] = {pos.x(), pos.y(), pos.z()};
            auto target = const_cast<Camera&>(plater->get_camera()).get_target();
            result["target"] = {target.x(), target.y(), target.z()};
            result["zoom"] = camera.get_zoom();

            if (canvas) {
                Size cnv_size = canvas->get_canvas_size();
                result["viewport_width"] = cnv_size.get_width();
                result["viewport_height"] = cnv_size.get_height();
            }

            return result;
        });
    });
}

void CommandDispatch::init() {
    register_model_commands();
    register_config_commands();
    register_diagnostics_commands();
    register_viewport_commands();
    m_mcp_protocol.init(get_all_tool_definitions());
}

}} // namespace Slic3r::GUI
