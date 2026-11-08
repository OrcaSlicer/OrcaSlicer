// Built-in backends for the per-vendor send dialogs (see VendorSendDialog.hpp).

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "ElegooLoginDialog.hpp"
#include "I18N.hpp"
#include "VendorSendDialog.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"
#include "slic3r/Utils/MarkforgedEiger.hpp"

#include <boost/filesystem.hpp>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace {

PrintHostType host_type_of(const DynamicPrintConfig& cfg)
{
    const auto* opt = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return opt != nullptr ? opt->value : htOctoPrint;
}

std::string print_host_of(const DynamicPrintConfig& cfg) { return cfg.has("print_host") ? cfg.opt_string("print_host") : std::string(); }

PrintHostPostUploadAction action_for(bool start) { return start ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None; }

// ---------------------------------------------------------------------------------------------
// Elegoo: account printers through the Elegoo cloud, plus the LAN printer set in the preset.
// ---------------------------------------------------------------------------------------------

constexpr const char* LAN_ID = "lan";

class ElegooBackend : public VendorSendBackend
{
public:
    explicit ElegooBackend(const DynamicPrintConfig& cfg) : m_config(cfg)
    {
        m_has_lan = host_type_of(cfg) == htElegooLink && !print_host_of(cfg).empty();
    }

    bool     needs_sign_in() const override { return true; }
    bool     signed_in() const override { return ElegooCloud::instance().signed_in(); }
    bool     has_local_printers() const override { return m_has_lan; }
    bool     sign_in(wxWindow* parent) override { return ElegooLoginDialog::sign_in(parent); }
    void     sign_out() override { ElegooCloud::instance().sign_out(); }
    wxString account_label() const override
    {
        return ElegooCloud::instance().signed_in() ? wxString::FromUTF8(ElegooCloud::instance().account_label()) : _L("not signed in");
    }

    std::vector<SendPrinter> printers() override
    {
        std::vector<SendPrinter> out;
        std::string              cloud_error;
        if (ElegooCloud::instance().signed_in()) {
            try {
                for (const ElegooCloudPrinter& p : ElegooCloud::instance().printers()) {
                    SendPrinter sp;
                    sp.id     = "cloud:" + p.serial;
                    sp.name   = wxString::FromUTF8(p.name);
                    sp.detail = "SN: " + wxString::FromUTF8(p.serial);
                    sp.model  = p.model;
                    sp.group  = _L("Cloud");
                    const int state = ElegooCloud::instance().printer_state(p.serial);
                    sp.busy   = state == 2;
                    sp.status = state == 1 ? _L("Idle") : state == 2 ? _L("Printing") : _L("Status unknown");
                    out.push_back(std::move(sp));
                }
            } catch (const std::exception& e) {
                cloud_error = e.what();
            }
        }
        if (m_has_lan) {
            SendPrinter sp;
            sp.id     = LAN_ID;
            sp.name   = m_config.has("printer_model") ? wxString::FromUTF8(m_config.opt_string("printer_model")) : _L("LAN printer");
            sp.detail = wxString::FromUTF8(print_host_of(m_config));
            sp.group  = _L("LAN");
            sp.status = _L("Local network");
            out.push_back(std::move(sp));
        }
        if (out.empty() && !cloud_error.empty())
            throw ElegooCloudError(cloud_error);
        return out;
    }

    SendSlots slots(const SendPrinter& printer) override
    {
        SendSlots s;
        s.system_name = "Canvas";
        if (printer.id.rfind("cloud:", 0) != 0) {
            s.message = _L("Filament mapping is chosen on the printer for LAN prints.");
            return s;
        }
        const ElegooCanvasInfo info = ElegooCloud::instance().canvas_info(printer.id.substr(6));
        if (!info.connected || info.trays.empty()) {
            s.message = _L("No Canvas is connected. The printer uses the loaded filament.");
            return s;
        }
        s.available            = true;
        s.supports_auto_refill = true;
        s.auto_refill          = info.auto_refill;
        for (const ElegooCanvasTray& t : info.trays) {
            SendSlot slot;
            slot.id     = std::to_string(t.canvas_id) + ":" + std::to_string(t.tray_id);
            slot.label  = wxString::Format("%d%c", t.canvas_id + 1, char('A' + t.tray_id));
            slot.type   = t.filament_type;
            slot.name   = wxString::FromUTF8(t.brand.empty() ? t.filament_name : t.brand + " " + t.filament_name);
            slot.color  = wxColour(wxString::FromUTF8(t.color.empty() ? "#FFFFFF" : t.color));
            slot.loaded = t.loaded();
            s.slots.push_back(std::move(slot));
        }
        return s;
    }

    std::vector<SendOption> options(const SendPrinter& printer) const override
    {
        std::vector<SendOption> o;
        SendOption plate;
        plate.key            = "plate_side";
        plate.label          = _L("Build plate");
        plate.kind           = SendOption::Kind::Choice;
        plate.choices        = {{"A", _L("Side A (PTE)")}, {"B", _L("Side B (PEI)")}};
        plate.default_choice = "A";
        plate.tooltip        = _L("The side of the build plate that faces up.");
        o.push_back(plate);
        SendOption level;
        level.key        = "bed_leveling";
        level.label      = _L("Auto bed leveling");
        level.default_on = true;
        o.push_back(level);
        SendOption lapse;
        lapse.key   = "timelapse";
        lapse.label = _L("Timelapse");
        o.push_back(lapse);
        (void) printer;
        return o;
    }

    PrintHostJob make_job(const SendRequest& req, const boost::filesystem::path& default_output, wxString& error) override
    {
        auto opt = [&](const char* key, const char* def) {
            auto it = req.options.find(key);
            return it == req.options.end() ? std::string(def) : it->second;
        };
        PrintHostJob job;
        if (req.printer_id == LAN_ID) {
            job = PrintHostJob(&m_config);
            if (job.empty()) {
                error = _L("The printer's network settings are incomplete.");
                return job;
            }
            job.upload_data.extended_info = {{"bedType", std::to_string(int(opt("plate_side", "A") == "B" ? btPC : btPTE))},
                                             {"timeLapse", opt("timelapse", "0")},
                                             {"heatedBedLeveling", opt("bed_leveling", "1")}};
        } else if (req.printer_id.rfind("cloud:", 0) == 0) {
            ElegooPrintOptions o;
            o.start_print     = req.start_print;
            o.bed_leveling    = opt("bed_leveling", "1") == "1";
            o.timelapse       = opt("timelapse", "0") == "1";
            o.plate_side      = opt("plate_side", "A") == "B" ? 'B' : 'A';
            o.set_auto_refill = req.options.count("auto_refill") > 0;
            o.auto_refill     = opt("auto_refill", "0") == "1";
            for (const auto& [filament, slot] : req.slot_map) {
                const auto colon = slot.find(':');
                if (colon == std::string::npos)
                    continue;
                o.slot_map.push_back({filament, std::stoi(slot.substr(0, colon)), std::stoi(slot.substr(colon + 1))});
            }
            job.printhost = std::make_unique<ElegooCloudHost>();
            ElegooCloudHost::to_extended_info(req.printer_id.substr(6), o, job.upload_data.extended_info);
        } else {
            error = _L("Choose a printer.");
            return job;
        }
        job.upload_data.upload_path = boost::filesystem::path(req.file_name.ToUTF8().data());
        job.upload_data.post_action = action_for(req.start_print);
        (void) default_output;
        return job;
    }

private:
    DynamicPrintConfig m_config;
    bool               m_has_lan{false};
};

// ---------------------------------------------------------------------------------------------
// Creality: the LAN printer set in the preset, with its CFS slots.
// ---------------------------------------------------------------------------------------------

class CrealityBackend : public VendorSendBackend
{
public:
    explicit CrealityBackend(const DynamicPrintConfig& cfg) : m_config(cfg) {}

    std::vector<SendPrinter> printers() override
    {
        std::vector<SendPrinter> out;
        if (host_type_of(m_config) != htCrealityPrint || print_host_of(m_config).empty())
            throw std::runtime_error(_u8L("Set the printer's IP address in the printer settings (Connection) to send to it."));
        CrealityPrint             host(&m_config);
        const CrealityPrinterInfo info = host.read_printer_info();
        SendPrinter               sp;
        sp.id     = "lan";
        sp.name   = wxString::FromUTF8(info.model_name);
        if (sp.name.empty())
            sp.name = _L("Creality printer");
        sp.detail = wxString::FromUTF8(host.get_host());
        sp.online = info.reachable;
        sp.status = info.reachable ? _L("Online") : _L("Offline");
        m_info    = info;
        out.push_back(std::move(sp));
        return out;
    }

    SendSlots slots(const SendPrinter&) override
    {
        SendSlots s;
        s.system_name = "CFS";
        m_slots.clear();
        if (!m_info.multi_color || m_info.boxes_json.empty()) {
            s.message = m_info.no_cfs ? _L("No CFS is connected. The printer uses the loaded filament.") :
                                        _L("This printer has no CFS. It uses the loaded filament.");
            return s;
        }
        const nlohmann::json resp = nlohmann::json::parse(m_info.boxes_json, nullptr, false);
        if (!resp.is_object() || !resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs"))
            return s;
        for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
            const int box_id = box.value("id", 0);
            if (box.value("type", 0) == 0 && box.value("state", 0) != 1) // inactive CFS; the spool holder (type 1) is always there
                continue;
            for (const auto& mat : box.value("materials", nlohmann::json::array())) {
                const int   slot_id = mat.value("id", 0);
                std::string color   = mat.value("color", "#FFFFFF");
                if (color.size() == 8 && color[0] == '#') // Creality sends "#0RRGGBB"
                    color = "#" + color.substr(2);
                SendSlot slot;
                slot.id     = std::to_string(m_slots.size());
                slot.label  = box.value("type", 0) == 1 ? wxString(_L("Ext")) : wxString::Format("%d%c", box_id, char('A' + slot_id));
                slot.type   = mat.value("type", "");
                slot.color  = wxColour(wxString::FromUTF8(color));
                slot.loaded = !slot.type.empty();
                s.slots.push_back(slot);
                m_slots.push_back({slot.type, color, box_id, slot_id});
            }
        }
        s.available = !s.slots.empty();
        s.enable_label = _L("Enable CFS");
        return s;
    }

    std::vector<SendOption> options(const SendPrinter&) const override
    {
        SendOption calib;
        calib.key        = "self_test";
        calib.label      = _L("Print Calibration");
        calib.default_on = true;
        calib.tooltip    = _L("Runs the printer's leveling and calibration before the print starts.");
        return {calib};
    }

    PrintHostJob make_job(const SendRequest& req, const boost::filesystem::path&, wxString& error) override
    {
        PrintHostJob job(&m_config);
        if (job.empty()) {
            error = _L("The printer's network settings are incomplete.");
            return job;
        }
        auto it = req.options.find("self_test");
        job.upload_data.extended_info["enableSelfTest"] = it != req.options.end() ? it->second : "1";
        for (const auto& [filament, slot_id] : req.slot_map) {
            const size_t idx = std::stoul(slot_id);
            if (idx >= m_slots.size())
                continue;
            const Slot& s = m_slots[idx];
            // The firmware matches by G-code tool (T1A for the first filament, ...), not by CFS slot.
            job.upload_data.extended_info["colorMatch_" + std::to_string(filament)] = "T1" + std::string(1, char('A' + filament)) + "\t" +
                                                                                    s.type + "\t" + s.color + "\t" + std::to_string(s.box) +
                                                                                    "\t" + std::to_string(s.material);
        }
        job.upload_data.upload_path = boost::filesystem::path(req.file_name.ToUTF8().data());
        job.upload_data.post_action = action_for(req.start_print);
        return job;
    }

private:
    struct Slot
    {
        std::string type, color;
        int         box, material;
    };
    DynamicPrintConfig  m_config;
    CrealityPrinterInfo m_info;
    std::vector<Slot>   m_slots;
};


// ---------------------------------------------------------------------------------------------
// Markforged: organization printers through the Eiger cloud API.
// ---------------------------------------------------------------------------------------------

bool ask_eiger_keys(wxWindow* parent, EigerKeys& keys)
{
    wxDialog dlg(parent, wxID_ANY, _L("Sign in to Eiger"));
    auto*    s = new wxBoxSizer(wxVERTICAL);
    s->Add(new wxStaticText(&dlg, wxID_ANY, _L("Enter the API keys from Eiger (Settings > API keys).")), 0, wxALL, dlg.FromDIP(12));
    auto* grid = new wxFlexGridSizer(2, dlg.FromDIP(6), dlg.FromDIP(10));
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Access key")), 0, wxALIGN_CENTER_VERTICAL);
    auto* access = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(keys.access), wxDefaultPosition, wxSize(dlg.FromDIP(320), -1));
    grid->Add(access, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Secret key")), 0, wxALIGN_CENTER_VERTICAL);
    auto* secret = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(keys.secret), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    grid->Add(secret, 1, wxEXPAND);
    s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP(12));
    s->Add(new wxHyperlinkCtrl(&dlg, wxID_ANY, _L("Open Eiger"), "https://www.eiger.io/"), 0, wxALL, dlg.FromDIP(12));
    s->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, dlg.FromDIP(12));
    dlg.SetSizerAndFit(s);
    dlg.CentreOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return false;
    keys.access = access->GetValue().Trim().Trim(false).ToUTF8().data();
    keys.secret = secret->GetValue().Trim().Trim(false).ToUTF8().data();
    return !keys.empty();
}

class MarkforgedBackend : public VendorSendBackend
{
public:
    explicit MarkforgedBackend(const DynamicPrintConfig& cfg) : m_keys(Eiger::keys_for(&cfg)) {}

    bool     needs_sign_in() const override { return true; }
    bool     signed_in() const override { return !m_keys.empty(); }
    bool     sign_in(wxWindow* parent) override
    {
        EigerKeys keys = m_keys;
        if (!ask_eiger_keys(parent, keys))
            return false;
        Eiger::save_keys(keys);
        m_keys = keys;
        return true;
    }
    void     sign_out() override
    {
        m_keys = EigerKeys();
        Eiger::save_keys(m_keys);
    }
    wxString account_label() const override { return m_keys.empty() ? wxString() : _L("Eiger API key"); }
    bool     has_filament_mapping() const override { return false; }
    bool     supports_upload_only() const override { return false; }

    std::vector<SendPrinter> printers() override
    {
        Eiger api(m_keys);
        std::vector<SendPrinter> out;
        for (const nlohmann::json& d : api.devices()) {
            auto str = [&](const char* k) {
                return d.contains(k) && d[k].is_string() ? d[k].get<std::string>() : d.contains(k) && !d[k].is_null() ? d[k].dump() : std::string();
            };
            SendPrinter p;
            p.id               = str("id");
            p.name             = wxString::FromUTF8(str("name"));
            p.model            = str("device_type");
            const std::string state = str("state");
            p.status           = wxString::FromUTF8(state);
            p.busy             = state != "Ready";
            p.online           = !(state.empty() || state == "Offline" || state == "Disabled" || state == "Unknown");
            wxString detail    = wxString::FromUTF8(p.model);
            if (const std::string mat = str("loaded_primary_material"); !mat.empty())
                detail += "  ·  " + wxString::FromUTF8(mat);
            if (d.contains("queue_length") && d["queue_length"].is_number_integer() && d["queue_length"].get<int>() > 0) {
                const int n    = d["queue_length"].get<int>();
                const int mins = d.value("queue_estimated_time_seconds", 0) / 60;
                detail += "  ·  " + wxString::Format(_L_PLURAL("%d job queued", "%d jobs queued", n), n);
                if (mins > 0)
                    detail += wxString::Format(" (%dh %02dm)", mins / 60, mins % 60);
            }
            p.detail = detail;
            out.push_back(std::move(p));
        }
        try {
            m_presets = api.settings_presets();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Eiger: settings presets unavailable: " << e.what();
        }
        return out;
    }

    std::vector<SendOption> options(const SendPrinter&) const override
    {
        SendOption preset;
        preset.key            = "settings_preset";
        preset.label          = _L("Settings preset");
        preset.kind           = SendOption::Kind::Choice;
        preset.choices.push_back({"", _L("Organization default")});
        for (const auto& [id, title] : m_presets)
            preset.choices.push_back({id, wxString::FromUTF8(title)});
        preset.default_choice = Eiger::saved_settings_preset();
        preset.tooltip        = _L("Material, layer height and fiber settings come from this Eiger preset.");
        SendOption action;
        action.key            = "action";
        action.label          = _L("When sent");
        action.kind           = SendOption::Kind::Choice;
        action.choices        = {{"now", _L("Print now")}, {"queue", _L("Add to queue")}, {"backlog", _L("Send to backlog")}};
        action.default_choice = "now";
        return {preset, action};
    }

    PrintHostJob make_job(const SendRequest& req, const boost::filesystem::path&, wxString& error) override
    {
        PrintHostJob job;
        // Eiger slices itself and takes STL only: write the plate's printable instances, placed as on the plate.
        Plater*        plater = wxGetApp().plater();
        PartPlate*     plate  = plater->get_partplate_list().get_curr_plate();
        Model&         model  = plater->model();
        TriangleMesh   all;
        for (size_t oi = 0; oi < model.objects.size(); ++oi) {
            const ModelObject* o   = model.objects[oi];
            const TriangleMesh raw = o->raw_mesh();
            for (size_t ii = 0; ii < o->instances.size(); ++ii) {
                if (!o->instances[ii]->printable || plate == nullptr || !plate->contain_instance(int(oi), int(ii)))
                    continue;
                TriangleMesh m = raw;
                m.transform(o->instances[ii]->get_matrix());
                all.merge(m);
            }
        }
        if (all.empty()) {
            error = _L("The plate has nothing to print.");
            return job;
        }
        const std::string title = boost::filesystem::path(req.file_name.ToUTF8().data()).stem().stem().string();
        const boost::filesystem::path stl =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("eiger-%%%%%%%%.stl");
        if (!store_stl(stl.string().c_str(), &all, true)) {
            error = _L("Could not write the plate for Eiger.");
            return job;
        }
        auto opt = [&](const char* key, const char* def) {
            auto it = req.options.find(key);
            return it == req.options.end() ? std::string(def) : it->second;
        };
        job.printhost               = std::make_unique<MarkforgedEigerHost>(m_keys);
        job.upload_data.source_path = stl;
        job.upload_data.upload_path = title + ".stl";
        job.upload_data.post_action = PrintHostPostUploadAction::StartPrint;
        job.upload_data.extended_info = {{"eiger_device", req.printer_id},
                                         {"eiger_action", opt("action", "now")},
                                         {"eiger_preset", opt("settings_preset", "")},
                                         {"eiger_title", title},
                                         {"vendor_source_ready", "1"}};
        return job;
    }

private:
    EigerKeys                                        m_keys;
    std::vector<std::pair<std::string, std::string>> m_presets;
};
} // namespace

void register_builtin_vendor_send_backends()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    register_vendor_send_backend(SendVendor::Elegoo, [](const DynamicPrintConfig& cfg) { return std::make_shared<ElegooBackend>(cfg); });
    register_vendor_send_backend(SendVendor::Creality, [](const DynamicPrintConfig& cfg) -> std::shared_ptr<VendorSendBackend> {
        // Creality printers driven by the printer-agent Device tab keep that flow.
        if (host_type_of(cfg) != htCrealityPrint)
            return nullptr;
        return std::make_shared<CrealityBackend>(cfg);
    });
    register_vendor_send_backend(SendVendor::Markforged, [](const DynamicPrintConfig& cfg) { return std::make_shared<MarkforgedBackend>(cfg); });
}

}} // namespace Slic3r::GUI
