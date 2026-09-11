#pragma once

#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <libslic3r/Preset.hpp>
#include <slic3r/plugin/PluginConfig.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

// Lists the plugin capabilities the edited preset of `m_type` uses (see capabilities_in_use) and edits
// each one's config, falling back to the global config where the preset has no override.
//
// A pure editor over a JSON document: it never writes to the preset and never writes to the base config
// file. The caller seeds it with the preset's raw override text and reads the edited text back from
// overrides_json(); PluginConfigField owns the value and feeds it through the normal field/dirty pipeline.
class PluginsConfigDialog : public WebViewHostDialog
{
public:
    PluginsConfigDialog(wxWindow* parent, Preset::Type type, const std::string& overrides_json);
    ~PluginsConfigDialog() override;

    // The edited overrides as compact JSON text; "" once no override remains.
    std::string overrides_json() const { return serialize_plugin_overrides(m_overrides); }

private:
    void on_script_message(const nlohmann::json& payload) override;
    // Runs one web command on a clean main-loop stack; see on_script_message.
    void handle_web_command(const nlohmann::json& payload);

    const Preset* current_preset() const;
    void          send_capabilities();
    void          send_capability_config(const PluginCapabilityId& id);
    void          send_save_error(const PluginCapabilityId& id, const std::string& error);
    void          show_status(const wxString& message, const char* level);

    PluginCapabilityId identifier_from(const nlohmann::json& payload) const;

    Preset::Type              m_type = Preset::TYPE_INVALID;
    PresetPluginConfigService m_service;
    // The working copy the dialog edits. Seeded from the preset's raw text, read back by the caller.
    CapabilityConfigDocument  m_overrides;
    // Set when the preset's stored text could not be parsed: the rows are shown read-only rather
    // than silently replacing data we did not understand.
    std::string               m_parse_error;
    // Guards the deferred command handlers against the dialog being destroyed while one is queued.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

}} // namespace Slic3r::GUI
