#include "HeatingCalibrationDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <cstring>

#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

namespace {

constexpr int default_moonraker_port = 7125;

void parse_print_host(std::string url, std::string &host, int &port)
{
    bool https = false, has_scheme = false;
    for (const char *scheme : {"http://", "https://"})
        if (boost::istarts_with(url, scheme)) {
            https = std::strcmp(scheme, "https://") == 0;
            has_scheme = true;
            url = url.substr(std::strlen(scheme));
            break;
        }
    if (const size_t slash = url.find('/'); slash != std::string::npos)
        url.resize(slash);
    host = (https ? "https://" : "") + url;
    port = has_scheme ? (https ? 443 : 80) : default_moonraker_port;
    size_t colon = url.rfind(':');
    if (!url.empty() && url.front() == '[') {
        const size_t closing = url.find(']');
        colon = closing != std::string::npos && closing + 1 < url.size() && url[closing + 1] == ':'
                    ? closing + 1 : std::string::npos;
    }
    if (colon != std::string::npos) {
        host = (https ? "https://" : "") + url.substr(0, colon);
        try {
            port = std::stoi(url.substr(colon + 1));
        } catch (...) {}
    }
}

wxString format_seconds(double seconds) { return wxString::Format("%.0f s", seconds); }

} // namespace

HeatingCalibrationDialog::HeatingCalibrationDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Calibrate heating"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);
    const wxSize input_size = FromDIP(wxSize(160, -1));

    auto add_section = [&](const wxString &title) {
        auto *box   = new LabeledStaticBox(this, title);
        auto *sizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto *grid  = new wxFlexGridSizer(2, FromDIP(4), FromDIP(10));
        grid->AddGrowableCol(0);
        sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(5));
        v_sizer->Add(sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        return grid;
    };
    auto add_input = [&](wxFlexGridSizer *grid, const wxString &label, const wxString &sidetext, bool numeric) {
        auto *input = new TextInput(this, wxEmptyString, sidetext, "", wxDefaultPosition, input_size);
        if (numeric)
            input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(input, 0, wxALIGN_CENTER_VERTICAL);
        return input;
    };
    const wxString celsius = _L(u8"\u2103");

    wxFlexGridSizer *grid = add_section(_L("Moonraker"));
    m_host    = add_input(grid, _L("Printer IP"), wxEmptyString, false);
    m_port    = add_input(grid, _L("Port"), wxEmptyString, true);
    m_api_key = add_input(grid, _L("API key (optional)"), wxEmptyString, false);

    grid          = add_section(_L("Temperature range"));
    m_nozzle_min  = add_input(grid, _L("Nozzle minimum"), celsius, true);
    m_nozzle_max  = add_input(grid, _L("Nozzle maximum"), celsius, true);
    m_bed_min     = add_input(grid, _L("Bed minimum"), celsius, true);
    m_bed_max     = add_input(grid, _L("Bed maximum"), celsius, true);
    m_nozzle_min->GetTextCtrl()->SetValue("30");
    m_nozzle_max->GetTextCtrl()->SetValue("300");
    m_bed_min->GetTextCtrl()->SetValue("30");
    m_bed_max->GetTextCtrl()->SetValue("100");

    m_progress     = new wxStaticBoxSizer(new LabeledStaticBox(this, _L("Progress")), wxVERTICAL);
    m_status       = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_temperatures = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_gauge        = new wxGauge(this, wxID_ANY, 1, wxDefaultPosition, FromDIP(wxSize(-1, 6)));
    m_result_text  = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_progress->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(5));
    m_progress->Add(m_temperatures, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(5));
    m_progress->Add(m_gauge, 0, wxEXPAND | wxALL, FromDIP(5));
    m_progress->Add(m_result_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    v_sizer->Add(m_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    v_sizer->Show(m_progress, false);
    m_result_text->Hide();

    auto *buttons = new DialogButtons(this, {"Apply", "Start", "Close"}, _L("Start"));
    m_apply_btn   = buttons->GetAPPLY();
    m_start_btn   = buttons->GetButtonFromIndex(1);
    m_apply_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_apply(); });
    m_start_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_start(); });
    buttons->GetButtonFromIndex(2)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
    m_apply_btn->Hide();
    v_sizer->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(10));

    Bind(wxEVT_CLOSE_WINDOW, &HeatingCalibrationDialog::on_close, this);

    fill_from_preset();
    wxGetApp().UpdateDlgDarkUI(this);
    SetSizerAndFit(v_sizer);
}

HeatingCalibrationDialog::~HeatingCalibrationDialog()
{
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_alive.reset();
    }
    if (m_calibration)
        m_calibration->cancel();
    if (m_worker.joinable())
        m_worker.join();
}

bool HeatingCalibrationDialog::stop_for_shutdown(bool can_veto)
{
    if (!m_calibration)
        return true;
    if (can_veto && m_calibration->waiting_for_firmware() &&
        MessageDialog(this, _L("A heater is waiting. Closing may take up to an hour before cleanup. Continue?"),
                      _L("Calibrate heating"), wxICON_WARNING | wxYES_NO | wxNO_DEFAULT).ShowModal() != wxID_YES)
        return false;
    if (!m_calibration)
        return true;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_alive.reset();
    }
    m_calibration->cancel();
    if (m_worker.joinable())
        m_worker.join();
    m_calibration.reset();
    return true;
}

void HeatingCalibrationDialog::show_for_current_printer()
{
    if (!m_calibration)
        fill_from_preset();
    Show();
    Raise();
}

void HeatingCalibrationDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

void HeatingCalibrationDialog::fill_from_preset()
{
    const DynamicPrintConfig &config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::string               host;
    int                       port = default_moonraker_port;
    if (const auto *print_host = config.option<ConfigOptionString>("print_host"))
        parse_print_host(print_host->value, host, port);
    m_host->GetTextCtrl()->SetValue(from_u8(host));
    m_port->GetTextCtrl()->SetValue(wxString::Format("%d", port));
    if (const auto *api_key = config.option<ConfigOptionString>("printhost_apikey"))
        m_api_key->GetTextCtrl()->SetValue(from_u8(api_key->value));

    m_bed_area.clear();
    if (const auto *area = config.option<ConfigOptionPoints>("printable_area"))
        m_bed_area = area->values;
}

bool HeatingCalibrationDialog::read_params(HeatingCalibration::Params &params)
{
    long port = 0;
    const bool read = m_port->GetTextCtrl()->GetValue().ToLong(&port) &&
                      m_nozzle_min->GetTextCtrl()->GetValue().ToDouble(&params.nozzle_min) &&
                      m_nozzle_max->GetTextCtrl()->GetValue().ToDouble(&params.nozzle_max) &&
                      m_bed_min->GetTextCtrl()->GetValue().ToDouble(&params.bed_min) &&
                      m_bed_max->GetTextCtrl()->GetValue().ToDouble(&params.bed_max);
    params.host = into_u8(m_host->GetTextCtrl()->GetValue().Trim().Trim(false));
    params.use_https = boost::istarts_with(params.host, "https://");
    if (params.use_https)
        params.host.erase(0, 8);
    else if (boost::istarts_with(params.host, "http://"))
        params.host.erase(0, 7);
    params.api_key    = into_u8(m_api_key->GetTextCtrl()->GetValue().Trim().Trim(false));
    params.bed_area = m_bed_area;
    if (!read || params.host.empty() || port <= 0 || port > 65535 || params.nozzle_min >= params.nozzle_max || params.bed_min >= params.bed_max) {
        MessageDialog(this, _L("Enter a printer IP, a port from 1 to 65535, and minimum temperatures below maximum temperatures."),
                      wxEmptyString, wxICON_WARNING | wxOK)
            .ShowModal();
        return false;
    }
    if (params.nozzle_max - params.nozzle_min < 10. || params.bed_max - params.bed_min < 10.) {
        MessageDialog(this, _L(u8"Nozzle and bed ranges must each span at least 10 \u2103."),
                      wxEmptyString, wxICON_WARNING | wxOK)
            .ShowModal();
        return false;
    }
    params.port        = int(port);
    return true;
}

void HeatingCalibrationDialog::on_start()
{
    if (m_calibration) {
        m_calibration->cancel();
        m_status->SetLabel(_L("Stopping..."));
        return;
    }
    HeatingCalibration::Params params;
    if (!read_params(params))
        return;
    const wxString question = format_wxstr(_L(u8"The printer at %1% will home and move the nozzle above the bed center. "
                                              u8"Clear the bed before starting one heating pass up to %2% \u2103 (nozzle) and %3% \u2103 (bed). Continue?"),
                                           params.host, int(params.nozzle_max), int(params.bed_max));
    if (MessageDialog(this, question, _L("Calibrate heating"), wxICON_QUESTION | wxYES_NO).ShowModal() != wxID_YES)
        return;

    m_preset_name = wxGetApp().preset_bundle->printers.get_edited_preset().name;
    m_result.reset();
    m_result_text->SetLabel(wxEmptyString);
    m_result_text->Hide();
    m_status->SetLabel(_L("Connecting..."));
    m_temperatures->SetLabel(wxEmptyString);
    m_gauge->SetValue(0);
    GetSizer()->Show(m_progress, true);
    m_alive = std::make_shared<int>(0);
    const std::weak_ptr<int> alive = m_alive;
    auto *callback_mutex = &m_callback_mutex;
    auto *dialog = this;
    auto calibration = std::make_shared<HeatingCalibration>(params, [dialog, alive, callback_mutex](const HeatingCalibration::Status &status) {
        std::lock_guard<std::mutex> lock(*callback_mutex);
        if (!alive.expired())
            wxGetApp().CallAfter([dialog, alive, status]() {
                if (!alive.expired())
                    dialog->on_status(status);
            });
    });
    m_calibration = calibration;
    m_worker = std::thread([dialog, alive, callback_mutex, calibration]() {
        std::optional<HeatingCalibration::Result> result;
        std::string error;
        try {
            result = calibration->run();
        } catch (const std::exception &e) {
            error = e.what();
        }
        std::lock_guard<std::mutex> lock(*callback_mutex);
        if (!alive.expired())
            wxGetApp().CallAfter([dialog, alive, result = std::move(result), error = std::move(error)]() mutable {
                if (!alive.expired())
                    dialog->on_finished(std::move(result), error);
            });
    });
    set_running(true);
}

void HeatingCalibrationDialog::on_status(const HeatingCalibration::Status &status)
{
    if (status.step > 0) {
        m_status->SetLabel(format_wxstr(_L("%1% (%2% of %3%)"), status.message, status.step, status.steps));
        m_gauge->SetRange(std::max(status.steps, 1));
        m_gauge->SetValue(status.step - 1);
    } else
        m_status->SetLabel(from_u8(status.message));
    m_temperatures->SetLabel(format_wxstr(_L(u8"Nozzle %1% / %2% \u2103, bed %3% / %4% \u2103"), int(std::lround(status.nozzle_temperature)),
                                          int(std::lround(status.nozzle_target)), int(std::lround(status.bed_temperature)),
                                          int(std::lround(status.bed_target))));
}

void HeatingCalibrationDialog::on_finished(std::optional<HeatingCalibration::Result> result, const std::string &error)
{
    m_alive.reset();
    if (m_worker.joinable())
        m_worker.join();
    m_calibration.reset();
    set_running(false);
    if (!result) {
        m_status->SetLabel(from_u8(error));
        m_status->Wrap(FromDIP(360));
        refit();
        return;
    }
    m_result = std::move(result);
    m_gauge->SetValue(m_gauge->GetRange());
    m_status->SetLabel(_L("Calibration complete."));

    const double    ambient = wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_float("heating_ambient_temperature");
    const HeaterCurve nozzle(m_result->nozzle.ramp, m_result->nozzle.settle);
    const HeaterCurve bed(m_result->bed.ramp, m_result->bed.settle);
    HeatingCalibration::Params params;
    read_params(params);
    m_result_text->SetLabel(format_wxstr(_L(u8"From %1% \u2103: nozzle to %2% \u2103 in %3%; bed to %4% \u2103 in %5%."),
                                         int(std::lround(ambient)), int(params.nozzle_max),
                                         format_seconds(nozzle.heat_time(ambient, params.nozzle_max)), int(params.bed_max),
                                         format_seconds(bed.heat_time(ambient, params.bed_max))));
    m_result_text->Wrap(FromDIP(360));
    m_result_text->Show();
    m_apply_btn->Show();
    m_apply_btn->Enable(true);
    refit();
}

void HeatingCalibrationDialog::on_apply()
{
    if (!m_result)
        return;
    if (wxGetApp().preset_bundle->printers.get_edited_preset().name != m_preset_name) {
        MessageDialog(this, _L("Select the printer preset used for this calibration before applying."),
                      _L("Calibrate heating"), wxICON_WARNING | wxOK).ShowModal();
        return;
    }
    DynamicPrintConfig curves;
    curves.set_key_value("nozzle_heating_ramp", new ConfigOptionPoints(m_result->nozzle.ramp));
    curves.set_key_value("nozzle_heating_settle", new ConfigOptionPoints(m_result->nozzle.settle));
    curves.set_key_value("bed_heating_ramp", new ConfigOptionPoints(m_result->bed.ramp));
    curves.set_key_value("bed_heating_settle", new ConfigOptionPoints(m_result->bed.settle));
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->load_config(curves);
    wxGetApp().plater()->on_config_change(curves);
    m_apply_btn->Hide();
    m_result_text->SetLabel(_L("Heating curves applied. Save the printer preset to keep them."));
    m_result_text->Wrap(FromDIP(360));
    refit();
}

void HeatingCalibrationDialog::on_close(wxCloseEvent &event)
{
    if (m_calibration) {
        if (event.CanVeto() &&
            MessageDialog(this, _L("Stop calibration and switch off the heaters?"), _L("Calibrate heating"),
                          wxICON_QUESTION | wxYES_NO)
                    .ShowModal() != wxID_YES) {
            event.Veto();
            return;
        }
        m_calibration->cancel();
    }
    Hide();
}

void HeatingCalibrationDialog::set_running(bool running)
{
    for (TextInput *input : {m_host, m_port, m_api_key, m_nozzle_min, m_nozzle_max, m_bed_min, m_bed_max})
        input->Enable(!running);
    m_start_btn->SetLabel(running ? _L("Stop") : _L("Start"));
    m_apply_btn->Show(!running && m_result.has_value());
    refit();
}

void HeatingCalibrationDialog::refit()
{
    m_apply_btn->GetParent()->Layout();
    Layout();
    GetSizer()->SetSizeHints(this);
}

}} // namespace Slic3r::GUI
