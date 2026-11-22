#include "KobraXHost.hpp"

#include "Http.hpp"
#include "KobraX.hpp"
#include "KobraXPrinterAgent.hpp"
#include "KobraXSession.hpp"
#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <wx/string.h>

using json = nlohmann::json;

namespace Slic3r {

KobraXHost::KobraXHost(DynamicPrintConfig* config) : m_http_port(KobraX::HTTP_PORT)
{
    // An address that is not a plain host stays empty, and test and upload say so
    if (!KobraX::split_address(config->opt_string("print_host"), m_host, m_http_port))
        m_host.clear();
}

const char* KobraXHost::get_name() const { return "Anycubic Kobra X"; }

bool KobraXHost::test(wxString& msg) const
{
    // Only the LAN token: the MQTT login needs the client certificate, which the upload reports when missing
    if (m_host.empty()) {
        msg = _L("The printer address is not a valid host name or IP address.");
        return false;
    }
    bool ok = false;
    Http::get("http://" + KobraX::url_host(m_host) + ":" + std::to_string(m_http_port) + "/info")
        .timeout_connect(5)
        .timeout_max(10)
        .size_limit(1024 * 1024)
        .on_complete([&](std::string body, unsigned) {
            const json info = json::parse(body, nullptr, false);
            ok = info.is_object() && info.contains("token") && info["token"].is_string() && info["token"].get<std::string>().size() >= 32;
            if (!ok)
                msg = _L("The printer answered without a LAN token. Is LAN mode enabled on the printer?");
        })
        .on_error([&](std::string body, std::string error, unsigned status) { msg = format_error(body, error, status); })
        .perform_sync();
    return ok;
}

wxString KobraXHost::get_test_ok_msg() const { return _L("Connection to the Anycubic Kobra X is working correctly."); }

wxString KobraXHost::get_test_failed_msg(wxString& msg) const
{
    return wxString::Format("%s: %s", _L("Could not connect to the Anycubic Kobra X"), msg);
}

bool KobraXHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    auto fail = [&](const std::string& error) {
        BOOST_LOG_TRIVIAL(error) << "KobraXHost: " << error;
        error_fn(GUI::from_u8(error));
        return false;
    };

    if (m_host.empty())
        return fail(_u8L("The printer address is not a valid host name or IP address."));
    info_fn(L"status", _L("Connecting to the printer..."));
    KobraX::Session::Options options{m_host, m_http_port, KobraX::MQTT_PORT, {}};
    std::string              error;
    if (!KobraX::find_client_identity(KobraX::identity_folder(), KobraX::bundled_identity_folder(), options.identity, error))
        return fail(error);
    std::mutex    mutex;
    KobraX::State state;
    KobraX::Session session(
        options,
        [&](const std::string& suffix, const json& payload) {
            std::lock_guard<std::mutex> lock(mutex);
            state.apply(suffix, payload);
        },
        {});
    if (!session.open(error))
        return fail(error);

    const auto answers = [](const char* suffix) {
        return [suffix](const std::string& got, const json&) { return got == suffix; };
    };
    json reply;
    if (!session.request("info", "query", json(), answers("info/report"), std::chrono::seconds(10), reply) ||
        !session.request("multiColorBox", "getInfo", json(), answers("multiColorBox/report"), std::chrono::seconds(10), reply))
        return fail(_u8L("The printer did not report its state."));

    std::string            upload_url, view;
    json                   mapping;
    std::string            mapping_error;
    {
        std::lock_guard<std::mutex> lock(mutex);
        upload_url = state.upload_url;
        view       = state.view_state();
        // The print host dialog chooses no slots: filament N prints from loaded slot N
        if (!KobraX::box_mapping("", state.slots(), mapping, mapping_error))
            mapping = json::array();
    }
    const bool start = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    if (start && (view == "printing" || view == "paused" || view == "busy"))
        return fail(view == "paused" ? _u8L("The printer is paused; nothing was sent.") :
                    view == "busy"   ? _u8L("The printer is busy; nothing was sent.") :
                                       _u8L("The printer is printing; nothing was sent."));

    const std::string name = KobraXPrinterAgent::upload_name(upload_data.upload_path.filename().string(), upload_data.source_path.string());
    bool              cancelled = false;
    const bool        uploaded  = session.upload(
        upload_data.source_path.string(), name, upload_url,
        [&](int percent) {
            const std::string none;
            progress_fn(Http::Progress(0, 0, 100, size_t(percent), none), cancelled);
        },
        [&] { return cancelled; }, error);
    if (cancelled)
        return false;
    if (!uploaded)
        return fail(error);
    if (!start)
        return true;

    info_fn(L"status", _L("Starting print..."));
    boost::system::error_code ec;
    const uint64_t size = boost::filesystem::file_size(upload_data.source_path, ec);
    const json     data = KobraX::print_start_data(name, size, mapping, true, false, false);
    if (!session.request("print", "start", data,
                         [](const std::string& suffix, const json& report) {
                             return KobraXPrinterAgent::start_outcome(suffix, report) >= 0 ||
                                    (suffix == "print/report" && report.is_object() && report.contains("action") && report["action"] == "start");
                         },
                         std::chrono::seconds(15), reply))
        return fail(_u8L("The printer did not answer the print start; the file stays on the printer."));
    if (KobraXPrinterAgent::start_outcome("print/report", reply) < 0)
        session.wait([](const std::string& suffix, const json& report) { return KobraXPrinterAgent::start_outcome(suffix, report) >= 0; },
                     std::chrono::seconds(5), reply);
    if (KobraXPrinterAgent::start_outcome("print/report", reply) == 0 || (reply.contains("state") && reply["state"] == "failed"))
        return fail(_u8L("The printer ended the print at once") + ": " + reply.value("msg", json("")).dump());
    return true;
}

} // namespace Slic3r
