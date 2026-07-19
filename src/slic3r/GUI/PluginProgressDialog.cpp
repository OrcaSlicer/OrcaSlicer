#include "PluginProgressDialog.hpp"

#include <utility>

namespace Slic3r { namespace GUI {

PluginProgressDialog::PluginProgressDialog(wxWindow*       parent,
                                           const wxString& title,
                                           const wxString& message,
                                           int             maximum,
                                           int             style,
                                           CloseHandler    on_destroyed)
    : ProgressDialog(title, message, maximum, parent, style)
    , m_pulse_message(message)
    , m_on_destroyed(std::move(on_destroyed))
{
    // The base ProgressDialog already binds wxEVT_CLOSE_WINDOW (its OnClose vetoes
    // a non-cancelable dialog and marks a cancelable one Canceled). We deliberately
    // don't add a second handler: a user-initiated close surfaces to the plugin as
    // update()/pulse() returning false, and programmatic close() calls Destroy()
    // directly, so no extra wiring is needed here.
}

PluginProgressDialog::~PluginProgressDialog()
{
    stop_pulse();

    if (m_on_destroyed)
        m_on_destroyed();
}

PluginProgressDialog* PluginProgressDialog::create_dialog(wxWindow*       parent,
                                                          const wxString& title,
                                                          const wxString& message,
                                                          int             maximum,
                                                          int             style,
                                                          CloseHandler    on_destroyed)
{
    return new PluginProgressDialog(parent, title, message, maximum, style, std::move(on_destroyed));
}

bool PluginProgressDialog::pulse(PluginProgressDialog* dialog, const wxString& message)
{
    return dialog != nullptr ? dialog->pulse(message) : false;
}

bool PluginProgressDialog::update(PluginProgressDialog* dialog, int value, const wxString& message)
{
    return dialog != nullptr ? dialog->update(value, message) : false;
}

void PluginProgressDialog::start_pulse(PluginProgressDialog* dialog, int interval_ms, const wxString& message)
{
    if (dialog != nullptr)
        dialog->start_pulse(interval_ms, message);
}

void PluginProgressDialog::stop_pulse(PluginProgressDialog* dialog)
{
    if (dialog != nullptr)
        dialog->stop_pulse();
}

void PluginProgressDialog::request_close(PluginProgressDialog* dialog)
{
    if (dialog != nullptr)
        dialog->close();
}

bool PluginProgressDialog::pulse(const wxString& message)
{
    if (!m_open)
        return false;

    if (!message.empty())
        m_pulse_message = message;

    return Pulse(message);
}

bool PluginProgressDialog::update(int value, const wxString& message)
{
    if (!m_open)
        return false;

    if (!message.empty())
        m_pulse_message = message;

    return Update(value, message);
}

void PluginProgressDialog::start_pulse(int interval_ms, const wxString& message)
{
    if (!m_open)
        return;

    if (!message.empty())
        m_pulse_message = message;

    stop_pulse();

    m_timer = std::make_unique<wxTimer>(this);
    Bind(wxEVT_TIMER, &PluginProgressDialog::on_timer, this, m_timer->GetId());
    m_timer->Start(interval_ms > 0 ? interval_ms : 1);
}

void PluginProgressDialog::stop_pulse()
{
    if (!m_timer)
        return;

    Unbind(wxEVT_TIMER, &PluginProgressDialog::on_timer, this, m_timer->GetId());
    m_timer->Stop();
    m_timer.reset();
}

void PluginProgressDialog::close()
{
    if (!m_open)
        return;

    m_open = false;
    stop_pulse();
    Destroy();
}

void PluginProgressDialog::on_timer(wxTimerEvent& /*event*/)
{
    if (m_open)
        Pulse(m_pulse_message);
}

}} // namespace Slic3r::GUI
