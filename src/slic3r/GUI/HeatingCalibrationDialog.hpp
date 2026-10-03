#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/Utils/HeatingCalibration.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class Button;
class TextInput;
class wxGauge;
class wxStaticText;
class wxStaticBoxSizer;

namespace Slic3r { namespace GUI {

// Modeless because a heating calibration can take hours.
class HeatingCalibrationDialog : public DPIDialog
{
public:
    HeatingCalibrationDialog(wxWindow *parent);
    ~HeatingCalibrationDialog() override;

    void show_for_current_printer();
    bool stop_for_shutdown(bool can_veto);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void fill_from_preset();
    bool read_params(HeatingCalibration::Params &params);
    void on_start();
    void on_apply();
    void on_close(wxCloseEvent &event);
    void on_status(const HeatingCalibration::Status &status);
    void on_finished(std::optional<HeatingCalibration::Result> result, const std::string &error);
    void set_running(bool running);
    void refit();

    TextInput   *m_host;
    TextInput   *m_port;
    TextInput   *m_api_key;
    TextInput   *m_nozzle_min;
    TextInput   *m_nozzle_max;
    TextInput   *m_bed_min;
    TextInput   *m_bed_max;
    wxStaticText *m_status;
    wxStaticText *m_temperatures;
    wxStaticText *m_result_text;
    wxGauge     *m_gauge;
    wxStaticBoxSizer *m_progress;
    Button      *m_start_btn;
    Button      *m_apply_btn;

    std::vector<Vec2d>                        m_bed_area;
    std::shared_ptr<HeatingCalibration>       m_calibration;
    std::thread                               m_worker;
    std::mutex                                m_callback_mutex;
    std::string                               m_preset_name;
    std::shared_ptr<int>                      m_alive;
    std::optional<HeatingCalibration::Result> m_result;
};

}} // namespace Slic3r::GUI
