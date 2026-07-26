#include "PluginsConfigDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginResolver.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

namespace {

wxString preset_type_title(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT: return _L("Process plugins");
    case Preset::TYPE_FILAMENT: return _L("Filament plugins");
    case Preset::TYPE_PRINTER: return _L("Printer plugins");
    default: return _L("Plugins");
    }
}

} // namespace

PluginsConfigDialog::PluginsConfigDialog(wxWindow* parent, Preset::Type type, const std::string& overrides_json)
    : WebViewHostDialog(parent, wxID_ANY, preset_type_title(type))
    , m_type(type)
{
    // On failure the document stays empty and every row goes read-only (see m_parse_error), so a preset
    // we cannot understand is never silently overwritten.
    if (!parse_plugin_overrides(overrides_json, m_overrides, m_parse_error))
        BOOST_LOG_TRIVIAL(error) << "Plugins Config dialog: " << m_parse_error;

    create_webview("web/dialog/PluginsConfigDialog/index.html", preset_type_title(type), wxSize(820, 660),
                   wxSize(640, 520));
}

PluginsConfigDialog::~PluginsConfigDialog() { m_alive->store(false, std::memory_order_release); }

const Preset* PluginsConfigDialog::current_preset() const
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return nullptr;

    switch (m_type) {
    case Preset::TYPE_PRINT: return &bundle->prints.get_edited_preset();
    case Preset::TYPE_PRINTER: return &bundle->printers.get_edited_preset();
    case Preset::TYPE_FILAMENT: return &bundle->filaments.get_edited_preset();
    default: return nullptr;
    }
}

PluginCapabilityId PluginsConfigDialog::identifier_from(const nlohmann::json& payload) const
{
    return {plugin_capability_type_from_string(payload.value("capability_type", "")),
            payload.value("capability_name", ""),
            payload.value("plugin_key", "")};
}

void PluginsConfigDialog::on_script_message(const nlohmann::json& payload)
{
    if (handle_common_script_command(payload))
        return;

    // Defer command handling out of the webview script-message callback, exactly as PluginsDialog
    // does: GTK and macOS deliver it synchronously inside the native webview callback, and window
    // work on that stack is the crash class fixed in b779a7bfed/f2ccbfc8b5. remove_preset_override
    // puts a modal message box on that stack, which is the same bug.
    wxGetApp().CallAfter([this, alive = m_alive, payload]() {
        if (alive->load(std::memory_order_acquire))
            handle_web_command(payload);
    });
}

void PluginsConfigDialog::handle_web_command(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");
    if (command == "request_capabilities") {
        send_capabilities();
        return;
    }

    const PluginCapabilityId id = identifier_from(payload);

    if (command == "get_capability_config") {
        send_capability_config(id);
    } else if (command == "save_capability_config") {
        if (!m_parse_error.empty()) {
            send_save_error(id, m_parse_error);
            return;
        }

        nlohmann::json value = payload.contains("config") ? payload.at("config") : nlohmann::json::object();
        if (value.is_string()) {
            value = nlohmann::json::parse(value.get<std::string>(), nullptr, /* allow_exceptions */ false);
            if (value.is_discarded()) {
                send_save_error(id, into_u8(_L("The configuration is not valid JSON. Your changes were not saved.")));
                return;
            }
        }

        const MutationResult result = m_service.set_preset_override(m_overrides, id, value);
        if (!result.ok) {
            send_save_error(id, result.error);
            return;
        }
        send_capability_config(id);
        show_status(_L("Configuration updated. Save the preset to persist it."), "success");
    } else if (command == "remove_preset_override") {
        // "Restore defaults" for a preset means holding no override at all: the capability falls back
        // to the global configuration, not to the plugin's own get_default_config().
        if (!m_parse_error.empty()) {
            send_save_error(id, m_parse_error);
            return;
        }

        const int rc = wxMessageBox(wxString::Format(_L("Restore the default configuration for \"%s\"?\n\n"
                                                        "This discards the preset's override and uses the global configuration."),
                                                     from_u8(id.name)),
                                    _L("Restore defaults"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (rc != wxYES)
            return;

        const MutationResult result = m_service.remove_preset_override(m_overrides, id);
        if (!result.ok) {
            send_save_error(id, result.error);
            return;
        }
        send_capability_config(id);
        show_status(_L("Using global configuration. Save the preset to persist it."), "success");
    }
}

void PluginsConfigDialog::send_capabilities()
{
    const Preset* preset = current_preset();
    if (preset == nullptr)
        return;

    nlohmann::json response;
    response["command"]     = "list_capabilities";
    response["preset_type"] = static_cast<int>(m_type);
    response["preset_name"] = preset->name;
    response["data"]        = PluginConfig::capabilities_payload(capabilities_in_use(m_type, *preset));

    BOOST_LOG_TRIVIAL(info) << "Prepared " << response["data"].size() << " capability rows for the Plugins Config dialog";
    call_web_handler(response);
}

void PluginsConfigDialog::send_capability_config(const PluginCapabilityId& id)
{
    const Preset* preset = current_preset();

    nlohmann::json response;
    response["command"]         = "capability_config";
    response["plugin_key"]      = id.plugin_key;
    response["capability_name"] = id.name;
    response["capability_type"] = plugin_capability_type_to_string(id.type);
    response["config"]          = nlohmann::json::object();
    response["custom_html"]     = "";
    response["error"]           = "";

    const auto cap = PluginManager::instance().get_plugin_capability(id, false);
    if (!cap || preset == nullptr) {
        response["error"] = into_u8(_L("This capability is no longer available."));
        call_web_handler(response);
        return;
    }

    const EffectiveCapabilityConfig effective = m_service.get_effective_config(m_overrides, id);
    response["config"]                 = effective.config;
    response["has_preset_override"]    = effective.has_preset_override;
    response["has_base_config"]        = effective.has_base_config;
    response["stored_plugin_version"]  = effective.stored_plugin_version;
    response["running_plugin_version"] = effective.running_plugin_version;
    response["read_only"]              = !m_parse_error.empty();
    if (!m_parse_error.empty())
        response["error"] = m_parse_error;

    if (cap->config_ui_available()) {
        try {
            wxBusyCursor  busy;
            PythonGILState gil;
            response["custom_html"] = cap->get_config_ui();
        } catch (const std::exception& ex) {
            response["error"] = into_u8(GUI::format_wxstr(_L("The plugin's configuration UI failed to load (%1%). Showing the default editor."),
                                                          from_u8(ex.what())));
        }
    }

    call_web_handler(response);
}

void PluginsConfigDialog::send_save_error(const PluginCapabilityId& id, const std::string& error)
{
    call_web_handler({{"command", "capability_config_saved"},
                      {"plugin_key", id.plugin_key},
                      {"capability_name", id.name},
                      {"capability_type", plugin_capability_type_to_string(id.type)},
                      {"ok", false},
                      {"error", error}});
}

void PluginsConfigDialog::show_status(const wxString& message, const char* level)
{
    nlohmann::json payload;
    payload["command"] = "status_message";
    payload["level"]   = level;
    payload["message"] = into_u8(message);
    call_web_handler(payload);
}

}} // namespace Slic3r::GUI
