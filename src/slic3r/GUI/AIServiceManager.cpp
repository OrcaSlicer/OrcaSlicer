#include "AIServiceManager.hpp"

#include "AISidecarClient.hpp"
#include "AIModelOutputDirectory.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/nowide/convert.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif
#include <nlohmann/json.hpp>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct AIServiceManager::SidecarProcess
{
    std::unique_ptr<boost::process::child> child;
};

namespace {

constexpr const char* DEFAULT_LOCAL_ENDPOINT = "http://127.0.0.1:18764";
constexpr const char* EXPECTED_SIDECAR_VERSION = "orcaslicer-ai-sidecar-v9";
constexpr int EXPECTED_PROTOCOL_VERSION = 2;

bool environment_flag(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
        return false;
    const std::string normalized(value);
    return normalized == "1" || normalized == "true" || normalized == "TRUE" || normalized == "yes" || normalized == "YES";
}

bool has_explicit_sidecar_endpoint()
{
    const char* value = std::getenv("ORCASLICER_AI_SIDECAR_URL");
    return value != nullptr && value[0] != '\0';
}

std::string health_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/health";
    return endpoint + "/health";
}

} // namespace

AIServiceAvailability AIServiceAvailability::from_health_response(const std::string& body, bool require_session_protection)
{
    AIServiceAvailability result;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        result.error = "AI sidecar returned an invalid health response.";
        return result;
    }

    const auto ok = parsed.find("ok");
    if (ok == parsed.end() || !ok->is_boolean() || !ok->get<bool>() ||
        !parsed.contains("protocol_version") || !parsed["protocol_version"].is_number_integer() ||
        parsed["protocol_version"].get<int>() != EXPECTED_PROTOCOL_VERSION ||
        !parsed.contains("sidecar_version") || !parsed["sidecar_version"].is_string() ||
        parsed["sidecar_version"].get<std::string>() != EXPECTED_SIDECAR_VERSION ||
        !parsed.contains("capabilities") || !parsed["capabilities"].is_object()) {
        result.error = "AI sidecar returned an incompatible health response.";
        return result;
    }
    if (require_session_protection) {
        if (!parsed.contains("runtime") || !parsed["runtime"].is_object()) {
            result.error = "AI sidecar returned incomplete runtime identity.";
            return result;
        }
        const auto& runtime = parsed["runtime"];
        const auto protected_session = runtime.find("session_protected");
        if (!runtime.contains("health_schema_version") || !runtime["health_schema_version"].is_number_integer() ||
            runtime["health_schema_version"].get<int>() != 2 ||
            protected_session == runtime.end() || !protected_session->is_boolean() ||
            !protected_session->get<bool>() || !runtime.contains("build") || !runtime["build"].is_object()) {
            result.error = "AI sidecar port is occupied by an untrusted or outdated service.";
            return result;
        }
    }

    const auto& capabilities = parsed["capabilities"];
    const auto config_proposal = capabilities.find("config_proposal");
    const auto model_generation = capabilities.find("model_generation");
    const auto expected_sources = nlohmann::json::array({ "text", "image" });
    if (config_proposal == capabilities.end() || !config_proposal->is_object() || !config_proposal->contains("available") ||
        !(*config_proposal)["available"].is_boolean() || model_generation == capabilities.end() || !model_generation->is_object() ||
        !model_generation->contains("available") || !(*model_generation)["available"].is_boolean() ||
        !model_generation->contains("sources") || (*model_generation)["sources"] != expected_sources ||
        !model_generation->contains("artifact_formats")) {
        result.error = "AI sidecar returned incomplete capability data.";
        return result;
    }
    const auto& artifact_formats = (*model_generation)["artifact_formats"];
    if (!artifact_formats.is_array() || artifact_formats.empty() ||
        !std::all_of(artifact_formats.begin(), artifact_formats.end(), [](const auto& format) {
            return format.is_string() && (format == "glb" || format == "obj");
        })) {
        result.error = "AI sidecar returned unsupported model artifact formats.";
        return result;
    }

    result.compatible = true;
    result.sidecar_version = parsed["sidecar_version"].get<std::string>();
    result.config_proposal_available = (*config_proposal)["available"].get<bool>();
    result.model_generation_available = (*model_generation)["available"].get<bool>();
    return result;
}

namespace {

std::string shutdown_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/v1/orcaslicer/shutdown";
    return endpoint + "/v1/orcaslicer/shutdown";
}

void inherit_sidecar_environment(boost::process::environment& destination)
{
    // Start from an empty block so unrelated desktop/cloud credentials cannot
    // flow into Python. Keep only Windows process essentials, proxy/CA policy,
    // and the provider settings explicitly supported by this Sidecar.
    static constexpr std::array<const char*, 29> allowed {
        "SystemRoot", "WINDIR", "COMSPEC", "PATH", "PATHEXT",
        "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "PROGRAMDATA",
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY",
        "SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE", "REQUESTS_CA_BUNDLE",
        "OPENAI_PRO_API", "OPENAI_PRO_URL", "OPENAI_API_KEY", "OPENAI_BASE_URL",
        "OPENAI_IMAGE_MODEL", "OPENAI_IMAGE_QUALITY",
        "OPENAI_TEXT_MODEL", "TRIPO_API_BASE", "TRIPO_API_KEY", "TRIPO_MODEL",
    };
    for (const char* name : allowed) {
        if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0')
            destination[name] = value;
    }
}

} // namespace

AIServiceManager::AIServiceManager(std::string endpoint)
    : m_endpoint(std::move(endpoint))
    , m_lifetime(std::make_shared<int>(0))
{
    (void)ai_model_output_directory();
    if (m_endpoint == DEFAULT_LOCAL_ENDPOINT && !has_explicit_sidecar_endpoint() &&
        !AISidecarClient::initialize_local_session())
        BOOST_LOG_TRIVIAL(error) << "Unable to initialize local AI sidecar session protection; autostart will remain disabled.";
}

std::string challenge_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/v1/orcaslicer/session-challenge";
    return endpoint + "/v1/orcaslicer/session-challenge";
}

AIServiceManager::~AIServiceManager()
{
    shutdown();
}

void AIServiceManager::discover_async(wxWindow* target, CompleteFn on_complete)
{
    cancel_discovery();
    m_lifetime = std::make_shared<int>(0);
    const std::weak_ptr<int> lifetime = m_lifetime;
    const wxWeakRef<wxWindow> weak_target(target);

    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        AIServiceAvailability result;
        result.error = "AI sidecar discovery requires a loopback endpoint.";
        if (weak_target) {
            weak_target->CallAfter([weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                if (weak_target && !lifetime.expired() && on_complete)
                    on_complete(std::move(result));
            });
        }
        return;
    }

    if (!AISidecarClient::session_protection_enabled()) {
        probe_health(weak_target, lifetime, std::move(on_complete));
        return;
    }

    const std::string client_nonce = AISidecarClient::create_session_nonce();
    if (client_nonce.empty()) {
        AIServiceAvailability result;
        result.error = "Unable to initialize AI sidecar authentication.";
        if (weak_target) {
            weak_target->CallAfter([weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                if (weak_target && !lifetime.expired() && on_complete)
                    on_complete(std::move(result));
            });
        }
        return;
    }

    auto http = Http::get(challenge_url(m_endpoint));
    http.local_only()
        .header("X-OrcaSlicer-Client-Nonce", client_nonce)
        .timeout_connect(2).timeout_max(5).size_limit(8 * 1024);
    http.on_complete([this, weak_target, lifetime, on_complete, client_nonce](std::string body, unsigned) mutable {
        const auto parsed = nlohmann::json::parse(body, nullptr, false);
        bool valid = parsed.is_object();
        if (valid) {
            const auto ok = parsed.find("ok");
            const auto protected_session = parsed.find("session_protected");
            valid = ok != parsed.end() && ok->is_boolean() && ok->get<bool>() &&
                    parsed.contains("protocol_version") && parsed["protocol_version"].is_number_integer() &&
                    parsed["protocol_version"].get<int>() == EXPECTED_PROTOCOL_VERSION &&
                    parsed.contains("sidecar_version") && parsed["sidecar_version"].is_string() &&
                    parsed["sidecar_version"].get<std::string>() == EXPECTED_SIDECAR_VERSION &&
                    protected_session != parsed.end() && protected_session->is_boolean() && protected_session->get<bool>() &&
                    parsed.contains("server_nonce") && parsed["server_nonce"].is_string() &&
                    parsed.contains("server_proof") && parsed["server_proof"].is_string() &&
                    AISidecarClient::accept_session_challenge(client_nonce,
                                                               parsed["server_nonce"].get<std::string>(),
                                                               parsed["server_proof"].get<std::string>());
        }
        if (!valid) {
            AIServiceAvailability result;
            result.error = "AI sidecar port is occupied by an untrusted or incompatible service.";
            if (weak_target) {
                weak_target->CallAfter([weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                    if (weak_target && !lifetime.expired() && on_complete)
                        on_complete(std::move(result));
                });
            }
            return;
        }
        if (!weak_target || lifetime.expired())
            return;
        // Chain the next request on the wx event thread. AIServiceManager is
        // owned and destroyed there, so the lifetime token can be checked
        // without racing its destructor from the HTTP worker thread.
        weak_target->CallAfter([this, weak_target, lifetime, on_complete = std::move(on_complete)]() mutable {
            if (weak_target && !lifetime.expired())
                probe_health(weak_target, lifetime, std::move(on_complete));
        });
    });
    http.on_error([this, weak_target, lifetime, on_complete](std::string, std::string error, unsigned status) mutable {
        BOOST_LOG_TRIVIAL(warning) << "AI sidecar challenge failed, status=" << status << ", error=" << error;
        AIServiceAvailability result;
        result.transient = status == 0;
        result.error = result.transient ? "AI sidecar is not reachable."
                                        : "AI sidecar port belongs to another or incompatible service.";
        if (weak_target) {
            weak_target->CallAfter([this, weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                if (weak_target && !lifetime.expired() && on_complete) {
                    if (result.transient)
                        try_autostart_local_sidecar();
                    on_complete(std::move(result));
                }
            });
        }
    });
    m_active_request = http.perform();
}

void AIServiceManager::probe_health(const wxWeakRef<wxWindow>& weak_target,
                                    const std::weak_ptr<int>& lifetime,
                                    CompleteFn on_complete)
{
    auto http = Http::get(health_url(m_endpoint));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(2).timeout_max(5).size_limit(16 * 1024);

    http.on_complete([this, weak_target, lifetime, on_complete](std::string body, unsigned) mutable {
        AIServiceAvailability result;
        try {
            result = AIServiceAvailability::from_health_response(body, AISidecarClient::session_protection_enabled());
        } catch (const std::exception&) {
            result.error = "AI sidecar returned a malformed health response.";
        }
        if (weak_target) {
            weak_target->CallAfter([this, weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                if (weak_target && !lifetime.expired() && on_complete) {
                    if (result.compatible)
                        m_autostart_attempts = 0;
                    on_complete(std::move(result));
                }
            });
        }
    });
    http.on_error([this, weak_target, lifetime, on_complete](std::string, std::string error, unsigned status) mutable {
        BOOST_LOG_TRIVIAL(warning) << "AI sidecar health check failed, status=" << status << ", error=" << error;
        AIServiceAvailability result;
        result.transient = status == 0;
        result.error = result.transient ? "AI sidecar is not reachable."
                                        : "AI sidecar rejected the authenticated session.";
        if (weak_target) {
            weak_target->CallAfter([this, weak_target, lifetime, on_complete, result = std::move(result)]() mutable {
                if (weak_target && !lifetime.expired() && on_complete) {
                    if (result.transient)
                        try_autostart_local_sidecar();
                    on_complete(std::move(result));
                }
            });
        }
    });
    m_active_request = http.perform();
}

void AIServiceManager::shutdown()
{
    cancel_discovery();
    stop_owned_sidecar();
}

void AIServiceManager::cancel_discovery()
{
    m_lifetime.reset();
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

void AIServiceManager::try_autostart_local_sidecar()
{
#ifdef _WIN32
    if (m_endpoint != DEFAULT_LOCAL_ENDPOINT || has_explicit_sidecar_endpoint() ||
        environment_flag("ORCASLICER_AI_DISABLE_AUTOSTART") || !AISidecarClient::session_protection_enabled())
        return;

    if (m_sidecar_process && m_sidecar_process->child) {
        std::error_code ec;
        if (m_sidecar_process->child->running(ec))
            return;
        m_sidecar_process->child->wait(ec);
        m_sidecar_process.reset();
    }

    // Bound repeated startup failures during the existing health-check retry window.
    if (m_autostart_attempts >= 3)
        return;

    const wxFileName application(wxStandardPaths::Get().GetExecutablePath());
    const wxString executable_dir = application.GetPath();
    const wxFileName python(executable_dir + wxFILE_SEP_PATH + "python", "pythonw.exe");
    const wxFileName bootstrap(executable_dir + wxFILE_SEP_PATH + "resources" + wxFILE_SEP_PATH + "tools" +
                                   wxFILE_SEP_PATH + "ai",
                               "orca_ai_installed_bootstrap.py");
    if (!python.FileExists() || !bootstrap.FileExists()) {
        BOOST_LOG_TRIVIAL(info) << "Packaged AI sidecar runtime is unavailable; python=" << python.FileExists()
                                << ", bootstrap=" << bootstrap.FileExists() << "; keeping AI features disabled.";
        m_autostart_attempts = 3;
        return;
    }

    ++m_autostart_attempts;
    try {
        BOOST_LOG_TRIVIAL(info) << "Starting packaged AI sidecar, attempt=" << m_autostart_attempts
                                << ", diagnostics=" << Slic3r::data_dir() << "/log/orca-ai-sidecar.log";
        namespace process = boost::process;
        const std::string session_token = AISidecarClient::session_token_for_child();
        if (session_token.empty()) {
            BOOST_LOG_TRIVIAL(error) << "AI sidecar child capability is unavailable.";
            return;
        }
        process::environment child_environment;
        inherit_sidecar_environment(child_environment);
        child_environment["ORCASLICER_AI_SESSION_TOKEN"] = session_token;
        child_environment["ORCASLICER_AI_REQUIRE_SESSION"] = "1";
        child_environment["ORCASLICER_AI_PARENT_PID"] = std::to_string(boost::this_process::get_id());
        child_environment["PYTHONNOUSERSITE"] = "1";
        process::wenvironment launch_environment(child_environment);
        ai_model_output_directory().configure_child_environment(launch_environment);
        auto owned = std::make_unique<SidecarProcess>();
        owned->child = std::make_unique<process::child>(
            python.GetFullPath().ToStdWstring(),
            process::args(std::vector<std::wstring>{ L"-I", bootstrap.GetFullPath().ToStdWstring(),
                                                     boost::nowide::widen(Slic3r::data_dir()) }),
            launch_environment,
            process::start_dir(bootstrap.GetPath().ToStdWstring()),
            process::std_out > process::null,
            process::std_err > process::null,
            process::windows::create_no_window);
        BOOST_LOG_TRIVIAL(info) << "Started packaged AI sidecar, pid=" << owned->child->id();
        m_sidecar_process = std::move(owned);
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Failed to start packaged AI sidecar: " << error.what();
        m_sidecar_process.reset();
    }
#endif
}

void AIServiceManager::stop_owned_sidecar()
{
#ifdef _WIN32
    if (!m_sidecar_process || !m_sidecar_process->child)
        return;

    std::error_code ec;
    if (m_sidecar_process->child->running(ec)) {
        bool graceful_requested = false;
        auto http = Http::post(shutdown_url(m_endpoint));
        AISidecarClient::configure_native_request(http);
        // Http's empty-body POST path uses its file read callback. Send a JSON
        // body explicitly so shutdown never reads an uninitialized upload.
        http.header("Content-Type", "application/json").set_post_body(std::string("{}"));
        http.timeout_connect(1).timeout_max(2).size_limit(8 * 1024);
        http.on_complete([&graceful_requested](std::string, unsigned) { graceful_requested = true; });
        http.on_error([](std::string, std::string, unsigned) {});
        http.perform_sync();

        if (graceful_requested) {
            for (int attempt = 0; attempt < 40 && m_sidecar_process->child->running(ec); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (m_sidecar_process->child->running(ec)) {
            BOOST_LOG_TRIVIAL(warning) << "AI sidecar did not stop gracefully; terminating owned process.";
            m_sidecar_process->child->terminate(ec);
        }
    }
    m_sidecar_process->child->wait(ec);
    m_sidecar_process.reset();
#endif
}

} // namespace Slic3r::GUI
