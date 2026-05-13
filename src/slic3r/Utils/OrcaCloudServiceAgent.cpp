#include "OrcaCloudServiceAgent.hpp"
#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <iostream>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <string>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/secretstore.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#if defined(__APPLE__)
#include <unistd.h>
#include <uuid/uuid.h>
#endif

using json = nlohmann::json;

namespace Slic3r {

namespace {
constexpr const char* ORCA_DEFAULT_API_URL = "api.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_AUTH_URL = "https://auth.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_CLOUD_URL = "https://cloud.orcaslicer.com";
// Orca: This is a public key with no secret, used to identify the client application to the backend.
constexpr const char* ORCA_DEFAULT_PUB_KEY = "sb_publishable_lvVe_whOi80SU9BPSxM1kA_tbt9AbR_";

constexpr const char* ORCA_HEALTH_PATH = "/api/v1/health";
constexpr const char* ORCA_SYNC_PULL_PATH = "/api/v1/sync/pull";
constexpr const char* ORCA_SYNC_PUSH_PATH = "/api/v1/sync/push";
constexpr const char* ORCA_SYNC_DELETE_PATH = "/api/v1/sync/delete";
constexpr const char* ORCA_PROFILES_PATH = "/api/v1/sync/profiles";
constexpr const char* ORCA_SUBSCRIPTIONS_PATH = "/api/v1/subscriptions";
constexpr const char* ORCA_SYNC_STATE_FILE = "sync_state";
constexpr const char* ORCA_SYNC_PROFILE_TABLE = "profiles";
constexpr size_t ORCA_SYNC_MAX_PAYLOAD_SIZE = 1048576; // 1MB size limit

constexpr const char* ORCA_CLOUD_LOGIN_PATH = "/orcaslicer-login";

constexpr const char* CONFIG_ORCA_API_URL = "orca_api_url";
constexpr const char* CONFIG_ORCA_AUTH_URL = "orca_auth_url";
constexpr const char* CONFIG_ORCA_CLOUD_URL = "orca_cloud_url";
constexpr const char* CONFIG_ORCA_PUB_KEY = "orca_pub_key";

constexpr const char* SECRET_STORE_SERVICE = "OrcaSlicer/Auth";
constexpr const char* SECRET_STORE_USER    = "orca_refresh_token";
constexpr std::chrono::seconds TOKEN_REFRESH_SKEW{900}; // 15 minutes

std::string generate_uuid_for_setting_id(const std::string& name, const std::string& user_id = "")
{
    if (name.empty()) {
        return "";
    }

    // Mix user_id into the hashed input so two different users generating a setting_id
    // for an identically-named preset get distinct UUIDs. Without this, the cloud's ID
    // space collides across accounts and the second user's create gets HTTP 409 with
    // server_profile=null on every sync (the foreign owner's record is not exposed).
    static const boost::uuids::uuid orca_namespace =
        boost::uuids::string_generator()("f47ac10b-58cc-4372-a567-0e02b2c3d479");

    boost::uuids::name_generator_sha1 gen(orca_namespace);
    boost::uuids::uuid id = user_id.empty() ? gen(name) : gen(user_id + "/" + name);
    return boost::uuids::to_string(id);
}

std::string base64url_encode(const std::vector<unsigned char>& data)
{
    std::string out;
    out.resize(boost::beast::detail::base64::encoded_size(data.size()));
    out.resize(boost::beast::detail::base64::encode(out.data(), data.data(), data.size()));

    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    return out;
}

bool base64url_decode(const std::string& input, std::vector<unsigned char>& out)
{
    std::string padded = input;
    while (padded.size() % 4 != 0) padded.push_back('=');
    std::string normalized = padded;
    std::replace(normalized.begin(), normalized.end(), '-', '+');
    std::replace(normalized.begin(), normalized.end(), '_', '/');

    out.resize(boost::beast::detail::base64::decoded_size(normalized.size()));
    auto res = boost::beast::detail::base64::decode(out.data(), normalized.data(), normalized.size());
    if (!res.second) return false;
    out.resize(res.first);
    return true;
}

std::vector<unsigned char> random_bytes(size_t len)
{
    std::vector<unsigned char> bytes(len);
    if (RAND_bytes(bytes.data(), static_cast<int>(len)) != 1) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));
    }
    return bytes;
}

std::string generate_code_verifier()
{
    constexpr int PKCE_VERIFIER_BYTES = 32;
    auto bytes = random_bytes(PKCE_VERIFIER_BYTES);
    return base64url_encode(bytes);
}

std::string generate_state_token()
{
    auto bytes = random_bytes(16);
    std::stringstream ss;
    for (auto b : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

std::string sha256_base64url(const std::string& input)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    std::vector<unsigned char> hash_vec(hash, hash + sizeof(hash));
    return base64url_encode(hash_vec);
}

std::string os_machine_id()
{
    // Orca: OS-level identifiers that live outside data_dir, so a copied data_dir
    // on another machine yields a different key and the stored refresh
    // token silently fails to decrypt (forcing a normal sign-in).
#if defined(__linux__)
    std::ifstream f("/etc/machine-id");
    std::string id;
    if (f.good()) {
        std::getline(f, id);
    }
    if (!id.empty()) return id;
#elif defined(_WIN32)
    // HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid is a per-install
    // identifier that isn't copied along with the user's data_dir.
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Cryptography",
                      0,
                      KEY_READ | KEY_WOW64_64KEY,
                      &hkey) == ERROR_SUCCESS) {
        wchar_t buffer[128] = {0};
        DWORD buffer_bytes = sizeof(buffer);
        DWORD type = 0;
        LONG status = RegQueryValueExW(hkey, L"MachineGuid", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buffer), &buffer_bytes);
        RegCloseKey(hkey);
        if (status == ERROR_SUCCESS && type == REG_SZ && buffer_bytes > sizeof(wchar_t)) {
            const size_t wlen = (buffer_bytes / sizeof(wchar_t)) - 1; // strip trailing NUL
            return wxString(buffer, wlen).ToStdString();
        }
    }
#elif defined(__APPLE__)
    // gethostuuid() returns the hardware-tied host UUID (same value as
    // "Hardware UUID" in System Information). It persists across OS
    // reinstalls and kernel updates. Do NOT use sysctl kern.uuid — that
    // is the running kernel image's build UUID (from kernel_uuid_string
    // in XNU), which rotates on every macOS/kernel update and would
    // sign users out on every OS update.
    uuid_t host_id;
    struct timespec wait = {0, 0};
    if (gethostuuid(host_id, &wait) == 0) {
        uuid_string_t str;
        uuid_unparse(host_id, str);
        return std::string(str);
    }
#endif

    // Last resort: OS hostname.
    return wxGetHostName().ToStdString();
}

std::string get_encryption_key()
{
    // Bind the encryption key to both the machine and the OS user. A data_dir
    // copied to another machine changes the machine half; a data_dir shared
    // between OS users on the same machine changes the user half. Either
    // difference makes the stored token fail to decrypt and forces a sign-in.
    return os_machine_id() + ":" + wxGetUserId().ToStdString();
}

std::vector<unsigned char> sha256_bytes(const std::string& input)
{
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH, 0);
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), out.data());
    return out;
}

std::string hmac_sha256_hex(const std::string& data, const std::vector<unsigned char>& key)
{
    unsigned int len = 0;
    unsigned char result[EVP_MAX_MD_SIZE];
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), result, &len) == nullptr) {
        return {};
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(result[i]);
    }
    return oss.str();
}

bool is_port_available(int port)
{
    if (port <= 0 || port > 65535) return false;

    using boost::asio::ip::tcp;
    boost::asio::io_context ctx;
    boost::system::error_code ec;

    tcp::acceptor acceptor(ctx);
    tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));

    acceptor.open(endpoint.protocol(), ec);
    if (ec) return false;
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) return false;
    acceptor.bind(endpoint, ec);
    if (ec) return false;
    acceptor.close(ec);
    return true;
}

int choose_loopback_port()
{
    int base_port = auth_constants::LOOPBACK_PORT;

    if (const char* env_port = std::getenv("ORCA_LOOPBACK_PORT")) {
        try {
            int parsed = std::stoi(env_port);
            if (parsed > 0 && parsed <= 65535) {
                base_port = parsed;
            }
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: invalid ORCA_LOOPBACK_PORT value, falling back to default";
        }
    }

    std::vector<int> candidates = {base_port, base_port + 1, base_port + 2};
    for (int port : candidates) {
        if (is_port_available(port)) return port;
    }

    return base_port;
}

bool aes256gcm_encrypt(const std::string& plaintext, const std::vector<unsigned char>& key, std::string& out_b64)
{
    const int iv_len = 12;
    auto iv = random_bytes(iv_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = true;
    int len = 0;
    std::vector<unsigned char> ciphertext(plaintext.size());
    std::vector<unsigned char> tag(16);

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, nullptr) != 1) ok = false;
    if (ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) ok = false;
    if (ok && EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                                reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()) != 1) ok = false;
    int ciphertext_len = len;
    if (ok && EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) ok = false;
    ciphertext_len += len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) ok = false;

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return false;
    ciphertext.resize(ciphertext_len);

    std::vector<unsigned char> payload;
    payload.reserve(iv.size() + tag.size() + ciphertext.size());
    payload.insert(payload.end(), iv.begin(), iv.end());
    payload.insert(payload.end(), tag.begin(), tag.end());
    payload.insert(payload.end(), ciphertext.begin(), ciphertext.end());

    out_b64 = base64url_encode(payload);
    return true;
}

bool aes256gcm_decrypt(const std::string& b64_payload, const std::vector<unsigned char>& key, std::string& plaintext)
{
    std::vector<unsigned char> payload;
    if (!base64url_decode(b64_payload, payload)) return false;
    if (payload.size() < 12 + 16) return false;

    const size_t iv_len = 12;
    const size_t tag_len = 16;
    std::vector<unsigned char> iv(payload.begin(), payload.begin() + iv_len);
    std::vector<unsigned char> tag(payload.begin() + iv_len, payload.begin() + iv_len + tag_len);
    std::vector<unsigned char> ciphertext(payload.begin() + iv_len + tag_len, payload.end());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = true;
    int len = 0;
    std::vector<unsigned char> plain(ciphertext.size());

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, nullptr) != 1) ok = false;
    if (ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) ok = false;
    if (ok && EVP_DecryptUpdate(ctx, plain.data(), &len, ciphertext.data(), ciphertext.size()) != 1) ok = false;
    int plain_len = len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1) ok = false;
    if (ok && EVP_DecryptFinal_ex(ctx, plain.data() + len, &len) != 1) ok = false;
    plain_len += len;

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return false;
    plain.resize(plain_len);
    plaintext.assign(reinterpret_cast<char*>(plain.data()), plain.size());
    return true;
}

} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

OrcaCloudServiceAgent::OrcaCloudServiceAgent(std::string log_dir)
    : log_dir(std::move(log_dir))
    , api_base_url(ORCA_DEFAULT_API_URL)
    , auth_base_url(ORCA_DEFAULT_AUTH_URL)
    , cloud_base_url(ORCA_DEFAULT_CLOUD_URL)
{
    auth_headers["apikey"] = ORCA_DEFAULT_PUB_KEY;
    pkce_bundle.loopback_port = choose_loopback_port();
    update_redirect_uri();
    regenerate_pkce();
    compute_fallback_path();
}

OrcaCloudServiceAgent::~OrcaCloudServiceAgent()
{
    if (refresh_thread.joinable()) {
        refresh_thread.join();
    }
}

void OrcaCloudServiceAgent::configure_urls(AppConfig* app_config)
{
    if (!app_config) return;

    // Read token storage preference
    m_use_encrypted_token_file = app_config->get_bool(SETTING_USE_ENCRYPTED_TOKEN_FILE);

    std::string api_url = app_config->get(CONFIG_ORCA_API_URL);
    if (!api_url.empty()) {
        api_base_url = api_url;
    }

    std::string auth_url = app_config->get(CONFIG_ORCA_AUTH_URL);
    if (!auth_url.empty()) {
        auth_base_url = auth_url;
    }

    std::string cloud_url = app_config->get(CONFIG_ORCA_CLOUD_URL);
    if (!cloud_url.empty()) {
        cloud_base_url = cloud_url;
    }

    std::string pub_key = app_config->get(CONFIG_ORCA_PUB_KEY);
    if (!pub_key.empty()) {
        auth_headers["apikey"] = pub_key;
    }
}

void OrcaCloudServiceAgent::set_api_base_url(const std::string& url)
{
    api_base_url = url;
}

void OrcaCloudServiceAgent::set_auth_base_url(const std::string& url)
{
    auth_base_url = url;
}

void OrcaCloudServiceAgent::set_cloud_base_url(const std::string& url)
{
    cloud_base_url = url;
}

void OrcaCloudServiceAgent::set_use_encrypted_token_file(bool use)
{
    m_use_encrypted_token_file = use;
}

bool OrcaCloudServiceAgent::get_use_encrypted_token_file() const
{
    return m_use_encrypted_token_file;
}

// ============================================================================
// ICloudServiceAgent - Lifecycle Methods
// ============================================================================

int OrcaCloudServiceAgent::init_log()
{
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_config_dir(std::string cfg_dir)
{
    config_dir = cfg_dir;
    wxFileName fallback(wxString::FromUTF8(cfg_dir.c_str()), "orca_refresh_token.sec");
    fallback.Normalize();
    refresh_fallback_path = fallback.GetFullPath().ToStdString();
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_cert_file(std::string folder, std::string filename)
{
    // Not used by OrcaCloudServiceAgent (OAuth doesn't need client certs)
    (void) folder;
    (void) filename;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_country_code(std::string code)
{
    country_code = code;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::start()
{
    regenerate_pkce();

    // Attempt silent sign-in from stored refresh token
    std::string stored_refresh;
    if (load_refresh_token(stored_refresh) && !stored_refresh.empty()) {
        refresh_now(stored_refresh, "refresh token", false);
    }

    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// ICloudServiceAgent - User Session Management
// ============================================================================

bool OrcaCloudServiceAgent::exchange_auth_code(const std::string& auth_code, const std::string& state, std::string& session_payload)
{
    const auto expected_state = pkce_bundle.state;
    if (expected_state.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "[auth] event=code_exchange result=failure reason=no_expected_state";
        return false;
    }
    if (state != expected_state) {
        BOOST_LOG_TRIVIAL(warning) << "[auth] event=code_exchange result=failure reason=state_mismatch";
        return false;
    }

    std::string code_verifier;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        code_verifier = pkce_bundle.verifier;
    }

    // Exchange the Supabase PKCE code with the token endpoint.
    // With redirect_to, GoTrue handles Google OAuth internally and redirects to localhost
    // with a Supabase-generated auth_code (not a raw Google code). The grant_type goes
    // in the URL query string (same pattern as the refresh_token flow).
    const std::string token_url = auth_base_url + auth_constants::TOKEN_PATH + "?grant_type=pkce";
    json body_j;
    body_j["auth_code"] = auth_code;
    body_j["code_verifier"] = code_verifier;

    std::string response;
    unsigned int http_code = 0;
    const bool success = http_post_token(body_j.dump(), &response, &http_code, token_url);

    if (!success || http_code >= 400) {
        BOOST_LOG_TRIVIAL(error) << "[auth] event=code_exchange result=failure http_code=" << http_code << " body=" << response;
        return false;
    }

    session_payload = response;
    BOOST_LOG_TRIVIAL(info) << "[auth] event=code_exchange result=success";
    return true;
}

int OrcaCloudServiceAgent::change_user(std::string user_info)
{
    try {
        auto tree = json::parse(user_info);

        auto safe_str = [](const json& j, const std::string& key) -> std::string {
            if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
            return "";
        };

        // Check if this is a WebView login message (PKCE flow completion)
        std::string command = safe_str(tree, "command");
        if (command == "user_login") {
            if (!tree.contains("data") || !tree["data"].is_object()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: WebView login payload missing data field";
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }
            const auto& data = tree["data"];
            std::string state = safe_str(data, "state");

            // Check for auth code (PKCE authorization code flow)
            std::string auth_code = safe_str(data, "code");
            if (!auth_code.empty()) {
                std::string session_payload;
                if (!exchange_auth_code(auth_code, state, session_payload)) {
                    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
                }
                // Recursively process the session payload (contains access_token, user, etc.)
                return change_user(session_payload);
            }

            // Validate PKCE state
            const auto expected_state = pkce_bundle.state;
            if (!expected_state.empty() && state != expected_state) {
                BOOST_LOG_TRIVIAL(warning) << "[auth] event=login result=failure reason=state_mismatch";
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            // Direct token flow (tokens already obtained by WebView)
            bool success = set_user_session(data);
            BOOST_LOG_TRIVIAL(info) << "[auth] event=login result=" << (success ? "success" : "failure")
                                    << " source=webview";
            return success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }

        // Orca cloud session payload (default flow)
        const json* session_node = nullptr;
        if (tree.contains("data") && tree["data"].is_object()) {
            const auto& data = tree["data"];
            if (data.contains("session") && data["session"].is_object()) {
                session_node = &data["session"];
            } else if (data.contains("access_token") || data.contains("token")) {
                session_node = &data;
            }
        }
        if (!session_node) {
            if (tree.contains("session") && tree["session"].is_object()) {
                session_node = &tree["session"];
            } else if (tree.contains("access_token") || tree.contains("token")) {
                session_node = &tree;
            }
        }

        if (session_node) {
            return set_user_session(*session_node)
                ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }

        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: Username/password login is disabled. Use the Orca cloud PKCE flow.";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: change_user exception - " << e.what();
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }
}

bool OrcaCloudServiceAgent::is_user_login()
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.logged_in;
}

int OrcaCloudServiceAgent::user_logout(bool request)
{

    // Send logout request to backend if requested
    if (request) {
        std::string token;
        std::string refresh_copy;
        {
            std::lock_guard<std::mutex> lock(session_mutex);
            token = session.access_token;
            refresh_copy = session.refresh_token;
        }

        if (!token.empty()) {
            std::string response;
            unsigned int http_code = 0;
            json logout_req = json::object();
            if (!refresh_copy.empty()) {
                logout_req["refresh_token"] = refresh_copy;
            }

            int result = http_post_auth(auth_constants::LOGOUT_PATH, logout_req.dump(), &response, &http_code) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
            if (result != BAMBU_NETWORK_SUCCESS || http_code >= 400) {
                BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: Orca cloud logout request failed - http_code=" << http_code;
            }
        }
    }

    clear_session();
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaCloudServiceAgent::get_user_id()
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_id;
}

std::string OrcaCloudServiceAgent::get_user_name()
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_name;
}

std::string OrcaCloudServiceAgent::get_user_avatar()
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_avatar;
}

std::string OrcaCloudServiceAgent::get_user_nickname()
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_nickname;
}

// ============================================================================
// ICloudServiceAgent - Login UI Support
// ============================================================================

std::string OrcaCloudServiceAgent::build_login_cmd()
{
    // When already signed in, emit the homepage payload so the web UI
    // can flip to the logged-in state without re-opening the login flow.
    if (is_user_login()) {
        std::string display_name = get_user_nickname();
        if (display_name.empty()) {
            display_name = "unknown name";
        }
        json cmd;
        cmd["command"] = "orca_userlogin";
        cmd["data"]["name"] = display_name;
        cmd["data"]["avatar"] = get_user_avatar();
        return cmd.dump();
    }

    update_redirect_uri();
    regenerate_pkce();
    const auto bundle = pkce();

    json tree;
    tree["action"] = "login_config";
    tree["backend_url"] = auth_base_url;

    // Include API key for direct Supabase calls from JavaScript
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        auto it = auth_headers.find("apikey");
        if (it != auth_headers.end()) {
            tree["apikey"] = it->second;
        }
    }

    tree["pkce"]["code_challenge"] = bundle.challenge;
    tree["pkce"]["code_challenge_method"] = "S256";
    tree["pkce"]["state"] = bundle.state;
    tree["pkce"]["redirect_uri"] = bundle.redirect;
    tree["pkce"]["code_verifier"] = bundle.verifier;
    tree["pkce"]["loopback_port"] = bundle.loopback_port;

    return tree.dump();
}

std::string OrcaCloudServiceAgent::build_logout_cmd()
{
    return json{{"command", "orca_useroffline"}}.dump();
}

std::string OrcaCloudServiceAgent::build_login_info()
{
    json tree;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        tree["user_id"] = session.user_id;
        tree["user_name"] = session.user_name;
        tree["nickname"] = session.user_nickname;
        tree["avatar"] = session.user_avatar;
        tree["logged_in"] = session.logged_in;
    }
    // Do not expose tokens to the WebView
    tree["access_token"] = "";
    tree["refresh_token"] = "";
    tree["backend_url"] = api_base_url;
    tree["auth_url"] = auth_base_url;

    return tree.dump();
}

// ============================================================================
// ICloudServiceAgent - Token Access
// ============================================================================

std::string OrcaCloudServiceAgent::get_access_token() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.access_token;
}

std::string OrcaCloudServiceAgent::get_refresh_token() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.refresh_token;
}

bool OrcaCloudServiceAgent::ensure_token_fresh(const std::string& reason)
{
    return refresh_if_expiring(TOKEN_REFRESH_SKEW, reason);
}

// ============================================================================
// ICloudServiceAgent - Server Connectivity
// ============================================================================

int OrcaCloudServiceAgent::connect_server()
{
    std::string response;
    unsigned int http_code = 0;
    int result = http_get(ORCA_HEALTH_PATH, &response, &http_code);

    bool connected = (result == BAMBU_NETWORK_SUCCESS && http_code >= 200 && http_code < 300);
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        is_connected = connected;
    }

    invoke_server_connected_callback(connected ? 0 : -1, http_code);
    return connected ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED;
}

bool OrcaCloudServiceAgent::is_server_connected()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return is_connected;
}

int OrcaCloudServiceAgent::refresh_connection()
{
    return connect_server();
}

int OrcaCloudServiceAgent::start_subscribe(std::string module)
{
    (void) module;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::stop_subscribe(std::string module)
{
    (void) module;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::add_subscribe(std::vector<std::string> dev_list)
{
    (void) dev_list;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::del_subscribe(std::vector<std::string> dev_list)
{
    (void) dev_list;
    return BAMBU_NETWORK_SUCCESS;
}

void OrcaCloudServiceAgent::enable_multi_machine(bool enable)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    multi_machine_enabled = enable;
}

// ============================================================================
// ICloudServiceAgent - Settings Synchronization
// ============================================================================

int OrcaCloudServiceAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    if (!user_presets) return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: Not logged in";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    bool success = false;
    std::string error_msg;
    int http_code_out = 0;
    SyncState saved_state;

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        saved_state = sync_state;
        sync_state = SyncState{};
    }

    auto on_success = [&](const SyncPullResponse& resp) {
        // Get current user_id for all presets (required by load_user_preset)
        std::string current_user_id = get_user_id();

        for (const auto& upsert : resp.upserts) {
            // Parse JSON content into key-value pairs using helper
            std::map<std::string, std::string> value_map;
            json_to_map(upsert.content.dump(), value_map);

            // Add metadata from top-level sync response if not already in content
            // These are required by PresetCollection::load_user_preset
            if (value_map.find(BBL_JSON_KEY_SETTING_ID) == value_map.end()) {
                value_map[BBL_JSON_KEY_SETTING_ID] = upsert.id;
            }
            if (value_map.find(BBL_JSON_KEY_USER_ID) == value_map.end()) {
                value_map[BBL_JSON_KEY_USER_ID] = current_user_id;
            }
            if (value_map.find(ORCA_JSON_KEY_UPDATE_TIME) == value_map.end()) {
                value_map[ORCA_JSON_KEY_UPDATE_TIME] = std::to_string(upsert.updated_time);
            }

            // Use preset name from content or fallback to upsert.name or upsert.id
            std::string preset_name = upsert.content.value(BBL_JSON_KEY_NAME, "");
            if (preset_name.empty()) {
                preset_name = upsert.name.empty() ? upsert.id : upsert.name;
            }

            // Store as: user_presets[preset_name][key] = value
            // This matches the format expected by PresetBundle::load_user_presets
            (*user_presets)[preset_name] = value_map;
        }

        if (resp.next_cursor != 0) {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            sync_state.last_sync_timestamp = resp.next_cursor;
            save_sync_state();
        } else {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            sync_state = saved_state;
        }
        success = true;
    };

    auto on_error = [&](int code, const std::string& err) {
        http_code_out = code;
        error_msg = err;
        success = false;
    };

    sync_pull(on_success, on_error);

    if (!success) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        sync_state = saved_state;
    }

    return success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
}

std::string OrcaCloudServiceAgent::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    std::string new_id = generate_uuid_for_setting_id(name, get_user_id());
    if (new_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: request_setting_id failed - name is empty";
        return "";
    }

    nlohmann::json content;
    content["name"] = name;
    content["type"] = IOT_PRINT_TYPE_STRING; // Default type

    if (values_map && !values_map->empty()) {
        for (const auto& pair : *values_map) {
            // Skip updated_time - it's metadata, not content
            if (pair.first == IOT_JSON_KEY_UPDATED_TIME) continue;
            content[pair.first] = pair.second;
        }
    }

    // Use sync_push to create the profile (no original_updated_time for new profiles per spec)
    auto result = sync_push(new_id, name, content, "");
    if (http_code) *http_code = result.http_code;

    if (result.success) {
        if (values_map && result.new_updated_time != 0) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = std::to_string(result.new_updated_time);
        }
        return new_id;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: request_setting_id failed - " << result.error_message << " - http code: " << result.http_code;
    return "";
}

int OrcaCloudServiceAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    // Extract original_updated_time for Optimistic Concurrency Control
    // If present, server will verify version before update. If absent, treated as insert.
    std::string original_updated_time;
    if (values_map) {
        auto it = values_map->find(IOT_JSON_KEY_UPDATED_TIME);
        if (it != values_map->end()) {
            original_updated_time = it->second;
        }
    }

    // Build content JSON
    nlohmann::json content;
    content["name"] = name;

    if (values_map && !values_map->empty()) {
        for (const auto& pair : *values_map) {
            // Skip updated_time - it's used for OCC, not as content
            if (pair.first == IOT_JSON_KEY_UPDATED_TIME) continue;
            content[pair.first] = pair.second;
        }
    }

    auto result = sync_push(setting_id, name, content, original_updated_time);
    if (http_code) *http_code = result.http_code;

    if (result.success) {
        if (values_map && result.new_updated_time != 0) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = std::to_string(result.new_updated_time);
        }
        return BAMBU_NETWORK_SUCCESS;
    }

    if (result.http_code == 409) {
        // Conflict - update values_map with server version
        if (values_map && result.server_version.updated_time != 0) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = std::to_string(result.server_version.updated_time);
        }
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: put_setting conflict - server_updated_time="
                                   << result.server_version.updated_time;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: put_setting failed - " << result.error_message;
    return BAMBU_NETWORK_ERR_PUT_SETTING_FAILED;
}

int OrcaCloudServiceAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    return get_setting_list2(bundle_version, nullptr, pro_fn, cancel_fn);
}

int OrcaCloudServiceAgent::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    (void) bundle_version;
    bool success = false;
    int error_code = 0;

    auto on_success = [&](const SyncPullResponse& resp) {
        int total = static_cast<int>(resp.upserts.size() + resp.deletes.size());
        int processed = 0;
        bool cancelled = false;

        for (const auto& upsert : resp.upserts) {
            if (cancel_fn && cancel_fn()) {
                cancelled = true;
                break;
            }

            if (chk_fn) {
                std::map<std::string, std::string> info;
                info[IOT_JSON_KEY_SETTING_ID] = upsert.id;
                info[IOT_JSON_KEY_UPDATED_TIME] = std::to_string(upsert.updated_time);

                if (upsert.content.is_object()) {
                    for (auto& [key, value] : upsert.content.items()) {
                        if (value.is_string()) {
                            info[key] = value.get<std::string>();
                        } else {
                            info[key] = value.dump();
                        }
                    }
                }

                if (!info.count(IOT_JSON_KEY_NAME) && !upsert.name.empty()) {
                    info[IOT_JSON_KEY_NAME] = upsert.name;
                }
                if (!info.count(IOT_JSON_KEY_TYPE)) {
                    info[IOT_JSON_KEY_TYPE] = IOT_PRINT_TYPE_STRING;
                }

                chk_fn(info);
            }

            if (pro_fn) {
                int progress = total > 0 ? (processed * 100 / total) : 100;
                pro_fn(progress);
            }

            processed++;
        }

        if (!cancelled) {
            for (const auto& deleted_id : resp.deletes) {
                if (cancel_fn && cancel_fn()) {
                    cancelled = true;
                    break;
                }

                if (chk_fn) {
                    std::map<std::string, std::string> info;
                    info[IOT_JSON_KEY_SETTING_ID] = deleted_id;
                    info["deleted"] = "true";
                    chk_fn(info);
                }

                if (pro_fn) {
                    int progress = total > 0 ? (processed * 100 / total) : 100;
                    pro_fn(progress);
                }

                processed++;
            }
        }

        if (!cancelled && resp.next_cursor != 0) {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            sync_state.last_sync_timestamp = resp.next_cursor;
            save_sync_state();
        }

        if (pro_fn) {
            pro_fn(100);
        }

        success = !cancelled;
    };

    auto on_error = [&](int code, const std::string& err) {
        error_code = code;
        success = false;
    };

    sync_pull(on_success, on_error);

    return success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
}

int OrcaCloudServiceAgent::delete_setting(std::string setting_id)
{
    std::string path = std::string(ORCA_SYNC_DELETE_PATH) + "?resource=" + ORCA_SYNC_PROFILE_TABLE + "&id=" + setting_id;
    std::string response;
    unsigned int http_code = 0;

    int result = http_delete(path, &response, &http_code);

    // Treat 204 as success: the setting is already gone from cloud, which is our goal.
    if (http_code == 204) {
        return BAMBU_NETWORK_SUCCESS;
    }

    if (result != BAMBU_NETWORK_SUCCESS || http_code >= 400) {
        return BAMBU_NETWORK_ERR_DEL_SETTING_FAILED;
    }

    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Sync Protocol Implementation
// ============================================================================

int OrcaCloudServiceAgent::sync_pull(
    std::function<void(const SyncPullResponse&)> on_success,
    std::function<void(int http_code, const std::string& error)> on_error)
{
    std::string path = ORCA_SYNC_PULL_PATH;
    if (sync_state.last_sync_timestamp != 0) {
        path += "?cursor=" + std::to_string(sync_state.last_sync_timestamp);
    }

    std::string response;
    unsigned int http_code = 0;
    int result = http_get(path, &response, &http_code);

    // Handle 410 Gone - cursor too old, need full resync
    if (http_code == 410) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: sync_pull returned 410 Gone - cursor too old, triggering full resync";
        clear_sync_state();
        // Retry without cursor
        path = ORCA_SYNC_PULL_PATH;
        result = http_get(path, &response, &http_code);
    }

    if (result != BAMBU_NETWORK_SUCCESS || (http_code != 200 && http_code != 304)) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: sync_pull failed - http_code=" << http_code << " - path=" << path;
        if (on_error) on_error(http_code, response);
        return BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
    }

    if (http_code == 304) {
        if (on_success) {
            SyncPullResponse empty_response;
            on_success(empty_response);
        }
        return BAMBU_NETWORK_SUCCESS;
    }

    try {
        auto json = nlohmann::json::parse(response);
        SyncPullResponse resp;
        resp.next_cursor = json.value("next_cursor", 0);

        if (json.contains("upserts") && json["upserts"].is_array()) {
            for (const auto& item : json["upserts"]) {
                ProfileUpsert upsert;
                upsert.id = item.value("id", "");
                upsert.name = item.value("name", "");
                upsert.updated_time = item.value(ORCA_JSON_KEY_UPDATE_TIME, 0);
                upsert.created_time = item.value(ORCA_JSON_KEY_CREATED_TIME, 0);
                if (item.contains("content")) {
                    upsert.content = item["content"];
                }
                resp.upserts.push_back(upsert);
            }
        }

        if (json.contains("deletes") && json["deletes"].is_array()) {
            for (const auto& item : json["deletes"]) {
                resp.deletes.push_back(item.get<std::string>());
            }
        }

        if (on_success) on_success(resp);
        return BAMBU_NETWORK_SUCCESS;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: sync_pull parse error - " << e.what();
        if (on_error) on_error(http_code, e.what());
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }
}

SyncPushResult OrcaCloudServiceAgent::sync_push(
    const std::string& profile_id,
    const std::string& name,
    const nlohmann::json& content,
    const std::string& original_updated_time)
{
    SyncPushResult result;
    result.success = false;
    result.http_code = 0;
    result.server_deleted = false;

    nlohmann::json body;
    body["id"] = profile_id;
    body["name"] = name;
    body["content"] = content;
    if (!original_updated_time.empty()) {
        body["original_updated_time"] = original_updated_time;
    }

    // Validate payload size before upload
    std::string body_str = body.dump();
    if (body_str.size() > ORCA_SYNC_MAX_PAYLOAD_SIZE) {
        result.http_code = 413; // HTTP 413 Payload Too Large
        result.success = false;
        result.error_message = "Preset content exceeds 1MB size limit (actual: " +
                              std::to_string(body_str.size()) + " bytes)";
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: sync_push payload too large - "
                                 << "size=" << body_str.size() << " bytes, "
                                 << "limit=" << ORCA_SYNC_MAX_PAYLOAD_SIZE << " bytes, "
                                 << "profile_id=" << profile_id;
        return result;
    }

    std::string response;
    unsigned int http_code = 0;
    int http_result = http_post(ORCA_SYNC_PUSH_PATH, body_str, &response, &http_code);

    result.http_code = http_code;

    if (http_code == 409) {
        // Conflict - parse server version
        try {
            auto json = nlohmann::json::parse(response);
            if (json.is_null()) {
                result.server_deleted = true;
            } else {
                auto& profile_data = json["server_profile"];
                result.server_version.id = profile_data.value("id", "");
                result.server_version.name = profile_data.value("name", "");
                result.server_version.updated_time = profile_data.value(ORCA_JSON_KEY_UPDATE_TIME, 0);
            }
        } catch (...) {}
        result.error_message = response;
        return result;
    }

    if (http_code != 200) {
        result.error_message = response;
        return result;
    }

    // Success
    try {
        auto json = nlohmann::json::parse(response);
        result.new_updated_time = json.value(ORCA_JSON_KEY_UPDATE_TIME, 0);
        if (result.new_updated_time != 0) {
            result.success = true;
        } else {
            result.error_message = "Server response missing required updated_time timestamp";
        }
    } catch (const std::exception& e) {
        result.error_message = e.what();
    }

    return result;
}

// ============================================================================
// Sync State Management
// ============================================================================

void OrcaCloudServiceAgent::load_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    if (sync_state_path.empty()) return;

    try {
        std::ifstream ifs(sync_state_path);
        if (ifs.good()) {
            std::string line;
            if (std::getline(ifs, line)) {
                sync_state.last_sync_timestamp = std::stoll(line);
            }
        }
    } catch (...) {
        sync_state.last_sync_timestamp = 0;
    }
}

void OrcaCloudServiceAgent::save_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    if (sync_state_path.empty()) return;

    try {
        std::string tmp_path = sync_state_path + ".tmp";
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (ofs.good()) {
            ofs << std::to_string(sync_state.last_sync_timestamp);
            ofs.close();
            boost::filesystem::rename(tmp_path, sync_state_path);
        }
    } catch (...) {}
}

void OrcaCloudServiceAgent::clear_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    sync_state = SyncState{};
    if (!sync_state_path.empty() && boost::filesystem::exists(sync_state_path)) {
        boost::filesystem::remove(sync_state_path);
    }
}

// ============================================================================
// Auth - PKCE and Session Management
// ============================================================================

void OrcaCloudServiceAgent::set_session_handler(SessionHandler handler)
{
    session_handler = std::move(handler);
}

void OrcaCloudServiceAgent::set_on_login_complete_handler(OnLoginCompleteHandler handler)
{
    on_login_complete_handler = std::move(handler);
}

const OrcaCloudServiceAgent::PkceBundle& OrcaCloudServiceAgent::pkce()
{
    if (pkce_bundle.verifier.empty() || pkce_bundle.challenge.empty() || pkce_bundle.state.empty()) {
        regenerate_pkce();
    }
    return pkce_bundle;
}

void OrcaCloudServiceAgent::regenerate_pkce()
{
    pkce_bundle.verifier = generate_code_verifier();
    pkce_bundle.challenge = sha256_base64url(pkce_bundle.verifier);
    pkce_bundle.state = generate_state_token();
    if (pkce_bundle.redirect.empty()) {
        pkce_bundle.redirect = "http://localhost:" + std::to_string(pkce_bundle.loopback_port) + auth_constants::LOOPBACK_PATH;
    }
}

void OrcaCloudServiceAgent::update_redirect_uri()
{
    int selected_port = choose_loopback_port();
    pkce_bundle.loopback_port = selected_port;
    pkce_bundle.redirect = "http://localhost:" + std::to_string(selected_port) + auth_constants::LOOPBACK_PATH;
}

// ============================================================================
// Auth - Token Persistence
// ============================================================================

void OrcaCloudServiceAgent::persist_refresh_token(const std::string& token)
{
    if (token.empty()) {
        clear_refresh_token();
        return;
    }

    bool stored = false;

    if (m_use_encrypted_token_file) {
        // Use encrypted file only
        auto key = sha256_bytes(get_encryption_key());
        if (key.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: cannot derive key for refresh-token file storage";
            return;
        }

        std::string payload;
        if (!aes256gcm_encrypt(token, key, payload)) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: failed to encrypt refresh token for file storage";
            return;
        }

        std::string signed_payload = payload;
        if (auto mac = hmac_sha256_hex(payload, key); !mac.empty()) {
            signed_payload = "v2:" + mac + ":" + payload;
        }

        compute_fallback_path();
        wxFileName path(wxString::FromUTF8(refresh_fallback_path.c_str()));
        path.Normalize();
        if (!wxFileName::DirExists(path.GetPath())) {
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }

        const std::string tmp_path = refresh_fallback_path + ".tmp";
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (ofs.good()) {
            ofs << signed_payload;
            ofs.flush();
            ofs.close();

            if (wxRenameFile(wxString::FromUTF8(tmp_path.c_str()), wxString::FromUTF8(refresh_fallback_path.c_str()), true)) {
                stored = true;
            } else {
                wxRemoveFile(wxString::FromUTF8(tmp_path.c_str()));
                BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: failed to atomically replace refresh-token file";
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: cannot open refresh-token file for write - " << refresh_fallback_path;
        }
    } else {
        // Use wxSecretStore only
        wxSecretStore store = wxSecretStore::GetDefault();
        if (store.IsOk()) {
            wxSecretValue secret(wxString::FromUTF8(token.c_str()));
            if (store.Save(SECRET_STORE_SERVICE, SECRET_STORE_USER, secret)) {
                stored = true;
            } else {
                BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: System Keychain save failed";
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: System Keychain not available";
        }
    }

    (void) stored;
}

bool OrcaCloudServiceAgent::load_refresh_token(std::string& out_token)
{
    out_token.clear();

    if (m_use_encrypted_token_file) {
        // Load from encrypted file only
        compute_fallback_path();
        if (wxFileExists(wxString::FromUTF8(refresh_fallback_path.c_str()))) {
            std::ifstream ifs(refresh_fallback_path, std::ios::binary);
            std::string payload((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            auto key = sha256_bytes(get_encryption_key());
            std::string plain;
            if (!key.empty()) {
                std::string encoded_payload = payload;
                bool integrity_ok = true;

                if (payload.rfind("v2:", 0) == 0) {
                    auto delim = payload.find(':', 3);
                    if (delim == std::string::npos) {
                        integrity_ok = false;
                    } else {
                        std::string stored_hmac = payload.substr(3, delim - 3);
                        std::string lower_stored = stored_hmac;
                        std::transform(lower_stored.begin(), lower_stored.end(), lower_stored.begin(), ::tolower);
                        encoded_payload = payload.substr(delim + 1);

                        std::string computed_hmac = hmac_sha256_hex(encoded_payload, key);
                        std::transform(computed_hmac.begin(), computed_hmac.end(), computed_hmac.begin(), ::tolower);
                        if (computed_hmac.empty() || computed_hmac != lower_stored) {
                            integrity_ok = false;
                            BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: refresh token integrity check failed (HMAC mismatch)";
                        }
                    }
                }

                if (integrity_ok && aes256gcm_decrypt(encoded_payload, key, plain) && !plain.empty()) {
                    out_token = plain;
                    // Upgrade legacy payloads to signed format
                    if (payload.rfind("v2:", 0) != 0) {
                        persist_refresh_token(out_token);
                    }
                    return true;
                }
            }
        }
    } else {
        wxSecretStore store = wxSecretStore::GetDefault();
        if (store.IsOk()) {
            wxString username;
            wxSecretValue secret;
            if (store.Load(SECRET_STORE_SERVICE, username, secret) && secret.IsOk()) {
                out_token.assign(static_cast<const char*>(secret.GetData()), secret.GetSize());
                if (!out_token.empty()) {
                    return true;
                }
            }
        }
    }

    return false;
}

void OrcaCloudServiceAgent::clear_refresh_token()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        store.Delete(SECRET_STORE_SERVICE);
    }

    compute_fallback_path();
    if (!refresh_fallback_path.empty() && wxFileExists(wxString::FromUTF8(refresh_fallback_path.c_str()))) {
        wxRemoveFile(wxString::FromUTF8(refresh_fallback_path.c_str()));
    }
}

// ============================================================================
// Auth - Token Refresh
// ============================================================================

bool OrcaCloudServiceAgent::should_refresh_locked(std::chrono::seconds skew) const
{
    if (!session.logged_in) return false;
    if (session.expires_at.time_since_epoch().count() == 0) return true;

    auto now = std::chrono::system_clock::now();
    return (session.expires_at - now) <= skew;
}

bool OrcaCloudServiceAgent::decode_jwt_expiry(const std::string& token, std::chrono::system_clock::time_point& out_tp)
{
    out_tp = {};
    if (token.empty()) return false;

    auto first = token.find('.');
    auto second = token.find('.', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;

    std::string payload_b64 = token.substr(first + 1, second - first - 1);
    std::vector<unsigned char> payload_bytes;
    if (!base64url_decode(payload_b64, payload_bytes)) return false;

    std::string payload_str(payload_bytes.begin(), payload_bytes.end());
    try {
        auto payload = json::parse(payload_str);
        if (payload.contains("exp") && payload["exp"].is_number()) {
            out_tp = std::chrono::system_clock::time_point{
                std::chrono::seconds(payload["exp"].get<long long>())
            };
            return true;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: failed to decode JWT exp - " << e.what();
    }
    return false;
}

bool OrcaCloudServiceAgent::refresh_now(const std::string& refresh_token, const std::string& reason, bool async)
{
    if (refresh_token.empty()) return false;

    bool expected = false;
    if (!refresh_running.compare_exchange_strong(expected, true)) {
        BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: refresh already running, skip (reason=" << reason << ")";
        return false;
    }

    auto worker = [this, refresh_token, reason]() {
        (void) reason;
        bool ok = refresh_session_with_token(refresh_token);
        refresh_running.store(false);
        return ok;
    };

    if (async) {
        if (refresh_thread.joinable()) {
            refresh_thread.join();
        }
        refresh_thread = std::thread([worker]() { worker(); });
        return true;
    }

    return worker();
}

bool OrcaCloudServiceAgent::refresh_from_storage(const std::string& reason, bool async)
{
    std::string refresh_token = get_refresh_token();
    if (refresh_token.empty()) {
        load_refresh_token(refresh_token);
    }
    if (refresh_token.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: no refresh token available for refresh (reason=" << reason << ")";
        return false;
    }

    return refresh_now(refresh_token, reason, async);
}

bool OrcaCloudServiceAgent::refresh_if_expiring(std::chrono::seconds skew, const std::string& reason)
{
    bool needs_refresh = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        needs_refresh = should_refresh_locked(skew);
    }

    if (!needs_refresh) return true;

    if (refresh_from_storage(reason, false)) return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    return refresh_from_storage(reason + "_retry", false);
}

bool OrcaCloudServiceAgent::refresh_session_with_token(const std::string& refresh_token)
{
    std::string body = "{\"refresh_token\":\"" + refresh_token + "\"}";
    std::string url = auth_base_url + auth_constants::TOKEN_PATH + "?grant_type=refresh_token";
    std::string  response;
    unsigned int http_code = 0;
    if (!http_post_token(body, &response, &http_code, url) || http_code >= 400) {
        std::string truncated_response = response.size() > 200 ? response.substr(0, 200) + "..." : response;
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: token refresh failed - http_code=" << http_code
                                   << ", response_body=" << truncated_response;
        return false;
    }

    if (session_handler) {
        return session_handler(response);
    }

    // No session handler set - parse the token response directly and establish session
    // This makes OrcaCloudServiceAgent self-contained without requiring external setup
    try {
        return set_user_session(json::parse(response));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: token refresh parse exception - " << e.what();
        return false;
    }
}

// ============================================================================
// Auth - Session State Helpers
// ============================================================================

bool OrcaCloudServiceAgent::set_user_session(const std::string& token,
                                     const std::string& user_id,
                                     const std::string& username,
                                     const std::string& nickname,
                                     const std::string& avatar,
                                     const std::string& refresh_token)
{
    std::chrono::system_clock::time_point exp_tp{};
    decode_jwt_expiry(token, exp_tp);

    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session.access_token = token;
        session.refresh_token = refresh_token;
        session.user_id = user_id;
        session.user_name = username;
        session.user_nickname = nickname;
        session.user_avatar = avatar;
        session.expires_at = exp_tp;
        session.logged_in = true;
    }

    if (!refresh_token.empty()) {
        persist_refresh_token(refresh_token);
    }

    // Set per-user sync state path
    if (!config_dir.empty() && !user_id.empty()) {
        boost::filesystem::path user_dir = boost::filesystem::path(config_dir) / "user" / user_id;
        if (!boost::filesystem::exists(user_dir)) {
            boost::filesystem::create_directories(user_dir);
        }
        sync_state_path = (user_dir / ORCA_SYNC_STATE_FILE).string();
        load_sync_state();
    }

    BOOST_LOG_TRIVIAL(info) << "OrcaCloudServiceAgent: set_user_session - user_id=" << user_id << ", username=" << username;
    return true;
}

bool OrcaCloudServiceAgent::set_user_session(const json& session_json, bool notify_login)
{
    auto safe_str = [](const json& j, const std::string& key) -> std::string {
        if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
        return "";
    };

    std::string access_token = safe_str(session_json, "access_token");
    if (access_token.empty()) {
        access_token = safe_str(session_json, "token");
    }
    std::string refresh_token = safe_str(session_json, "refresh_token");

    std::string user_id, username, nickname, avatar;
    if (session_json.contains("user") && session_json["user"].is_object()) {
        // Nested format (Orca cloud / GoTrue response)
        const auto& user = session_json["user"];
        user_id = safe_str(user, "id");
        if (user.contains("user_metadata") && user["user_metadata"].is_object()) {

            const auto& meta = user["user_metadata"];
            username = safe_str(meta, "username"); // Orca Cloud's unique username 

            nickname = safe_str(meta, "display_name"); // Orca Cloud's primary display name field
            // Fallback to different name from different providers if display_name is not set
            if (nickname.empty())
                nickname = safe_str(meta, "full_name");
            if (nickname.empty())
                nickname = safe_str(meta, "name");
            if (nickname.empty())
                nickname = username;

            avatar = safe_str(meta, "avatar_url");
        }
    } else {
        // Flat format (WebView direct token flow)
        user_id = safe_str(session_json, "user_id");
        username = safe_str(session_json, "username");
        nickname = safe_str(session_json, "display_name");
        if(nickname.empty())
            nickname = safe_str(session_json, "nickname");
            
        avatar = safe_str(session_json, "avatar");
    }

    if (access_token.empty() || user_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: session payload missing access_token or user id";
        return false;
    }

    bool success = set_user_session(access_token, user_id, username, nickname, avatar, refresh_token);
    if (success && notify_login && on_login_complete_handler) {
        on_login_complete_handler(true, user_id);
    }
    return success;
}

void OrcaCloudServiceAgent::clear_session()
{
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session = SessionInfo{};
    }
    clear_refresh_token();
}

// ============================================================================
// HTTP Helpers
// ============================================================================

bool OrcaCloudServiceAgent::attempt_refresh_after_unauthorized(const std::string& reason)
{
    if (refresh_from_storage(reason, false)) return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (refresh_from_storage(reason + "_retry", false)) return true;

    BOOST_LOG_TRIVIAL(warning) << "[auth] event=refresh result=failure source=" << reason << " action=logout";
    return false;
}

std::map<std::string, std::string> OrcaCloudServiceAgent::data_headers()
{
    std::scoped_lock lock(state_mutex, headers_mutex);
    auto headers = auth_headers;
    for (const auto& pair : extra_headers) {
        headers[pair.first] = pair.second;
    }
    return headers;
}

int OrcaCloudServiceAgent::http_get(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: GET " << url;

    ensure_token_fresh("http_get_" + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::get(url);
            http.tls_verify(true);

            std::string token = get_access_token();

            auto headers = data_headers();
            for (const auto& pair : headers) {
                http.header(pair.first, pair.second);
            }

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            http.on_complete([&](std::string body, unsigned resp_status) {
                    result.success = true;
                    result.status = resp_status;
                    result.body = body;
                })
                .on_error([&](std::string body, std::string error, unsigned resp_status) {
                    result.success = false;
                    result.status = resp_status;
                    result.body = body;
                })
                .timeout_max(30)
                .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_get exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();

    // Single retry on 401 - no recursion
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_get_" + path)) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (!res.success || res.status >= 400) {
        invoke_http_error_callback(res.status, res.body);
    }

    return res.success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaCloudServiceAgent::http_post(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: POST " << url;

    ensure_token_fresh("http_post_" + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::post(url);
            http.tls_verify(true);

            std::string token = get_access_token();

            auto headers = data_headers();
            for (const auto& pair : headers) {
                http.header(pair.first, pair.second);
            }

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            http.header("Content-Type", "application/json");
            http.set_post_body(body);

            http.on_complete([&](std::string resp_body, unsigned resp_status) {
                    result.success = true;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .on_error([&](std::string resp_body, std::string error, unsigned resp_status) {
                    result.success = false;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .timeout_max(30)
                .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_post exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();

    // Single retry on 401 - no recursion
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_post_" + path)) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (!res.success || res.status >= 400) {
        invoke_http_error_callback(res.status, res.body);
    }

    return res.success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaCloudServiceAgent::http_put(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: PUT " << url;

    ensure_token_fresh("http_put_" + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::put(url);
            http.tls_verify(true);

            std::string token = get_access_token();

            auto headers = data_headers();
            for (const auto& pair : headers) {
                http.header(pair.first, pair.second);
            }

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            http.header("Content-Type", "application/json");
            http.set_post_body(body);

            http.on_complete([&](std::string resp_body, unsigned resp_status) {
                    result.success = true;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .on_error([&](std::string resp_body, std::string error, unsigned resp_status) {
                    result.success = false;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .timeout_max(30)
                .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_put exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();

    // Single retry on 401 - no recursion
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_put_" + path)) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (!res.success || res.status >= 400) {
        invoke_http_error_callback(res.status, res.body);
    }

    return res.success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaCloudServiceAgent::http_delete(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: DELETE " << url;

    ensure_token_fresh("http_delete_" + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::del(url);
            http.tls_verify(true);

            std::string token = get_access_token();

            auto headers = data_headers();
            for (const auto& pair : headers) {
                http.header(pair.first, pair.second);
            }

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            http.on_complete([&](std::string resp_body, unsigned resp_status) {
                    result.success = true;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .on_error([&](std::string resp_body, std::string error, unsigned resp_status) {
                    result.success = false;
                    result.status = resp_status;
                    result.body = resp_body;
                })
                .timeout_max(30)
                .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_delete exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();

    // Single retry on 401 - no recursion
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_delete_" + path)) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (!res.success || res.status >= 400) {
        invoke_http_error_callback(res.status, res.body);
    }

    return res.success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

bool OrcaCloudServiceAgent::http_post_token(const std::string& body, std::string* response_body, unsigned int* http_code, const std::string& custom_url)
{
    std::map<std::string, std::string> headers_copy;
    std::string                        url;
    {
        std::lock_guard<std::mutex> lock(headers_mutex);
        url          = custom_url.empty() ? (auth_base_url + auth_constants::TOKEN_PATH) : custom_url;
        headers_copy = extra_headers;
    }

    // Add auth headers
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        for (const auto& pair : auth_headers) {
            headers_copy[pair.first] = pair.second;
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: POST " << url;

    bool has_apikey = false;
    for (const auto& pair : headers_copy) {
        if (pair.first == "apikey")
            has_apikey = true;
    }
    if (!has_apikey) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: http_post_token - apikey header MISSING! Token request will likely fail.";
    }

    try {
        auto http = Http::post(url);
        http.tls_verify(true);

        for (const auto& pair : headers_copy) {
            http.header(pair.first, pair.second);
        }

        http.remove_header("Authorization");
        http.remove_header("Content-Type");
        http.header("Content-Type", "application/json");
        http.set_post_body(body);

        bool         success = false;
        unsigned int status  = 0;
        std::string  resp_body;

        http.on_complete([&](std::string body, unsigned resp_status) {
                success   = true;
                status    = resp_status;
                resp_body = body;
            })
            .on_error([&](std::string body, std::string error, unsigned resp_status) {
                success   = false;
                status    = resp_status;
                resp_body = body;
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        if (response_body)
            *response_body = resp_body;
        if (http_code)
            *http_code = status;
        return success;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_post_token exception - " << e.what();
        if (http_code)
            *http_code = 0;
        return false;
    }
}

bool OrcaCloudServiceAgent::http_post_auth(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = auth_base_url + path + "?scope=local";
    std::string token;
    std::map<std::string, std::string> headers_copy;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        token = session.access_token;
    }
    {
        std::lock_guard<std::mutex> lock(headers_mutex);
        headers_copy = extra_headers;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        for (const auto& pair : auth_headers) {
            headers_copy[pair.first] = pair.second;
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "OrcaCloudServiceAgent: POST (auth) " << url;

    try {
        auto http = Http::post(url);
        http.tls_verify(true);

        for (const auto& pair : headers_copy) {
            http.header(pair.first, pair.second);
        }

        if (!token.empty()) {
            http.header("Authorization", "Bearer " + token);
        }

        http.remove_header("Content-Type");
        http.header("Content-Type", "application/json");
        http.set_post_body(body);

        bool         success = false;
        unsigned int status  = 0;
        std::string  resp_body;

        http.on_complete([&](std::string body, unsigned resp_status) {
                success   = true;
                status    = resp_status;
                resp_body = body;
            })
            .on_error([&](std::string body, std::string error, unsigned resp_status) {
                success   = false;
                status    = resp_status;
                resp_body = body;
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: HTTP (auth) error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        if (response_body)
            *response_body = resp_body;
        if (http_code)
            *http_code = status;
        return success;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: http_post_auth exception - " << e.what();
        if (http_code)
            *http_code = 0;
        return false;
    }
}

void OrcaCloudServiceAgent::compute_fallback_path()
{
    if (!refresh_fallback_path.empty()) return;
    wxFileName fallback(wxStandardPaths::Get().GetUserDataDir(), "orca_refresh_token.sec");
    fallback.Normalize();
    refresh_fallback_path = fallback.GetFullPath().ToStdString();
}

// ============================================================================
// JSON Helpers
// ============================================================================

std::string OrcaCloudServiceAgent::map_to_json(const std::map<std::string, std::string>& map)
{
    nlohmann::json j;
    for (const auto& pair : map) {
        j[pair.first] = pair.second;
    }
    return j.dump();
}

void OrcaCloudServiceAgent::json_to_map(const std::string& json, std::map<std::string, std::string>& map)
{
    try {
        auto j = nlohmann::json::parse(json);
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                map[it.key()] = it.value().get<std::string>();
            } else {
                map[it.key()] = it.value().dump();
            }
        }
    } catch (...) {}
}

// ============================================================================
// Callback Invocation
// ============================================================================

void OrcaCloudServiceAgent::invoke_server_connected_callback(int return_code, int reason_code)
{
    AppOnServerConnectedFn callback;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_server_connected_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        CloudEvent event{ORCA_CLOUD_PROVIDER};
        if (queue_fn) {
            queue_fn([callback, event, return_code, reason_code]() {
                callback(event, return_code, reason_code);
            });
        } else {
            callback(event, return_code, reason_code);
        }
    }
}

void OrcaCloudServiceAgent::invoke_http_error_callback(unsigned http_code, const std::string& http_body)
{
    AppOnHttpErrorFn callback;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_http_error_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        CloudEvent event{ORCA_CLOUD_PROVIDER};
        if (queue_fn) {
            queue_fn([callback, event, http_code, http_body]() {
                callback(event, http_code, http_body);
            });
        } else {
            callback(event, http_code, http_body);
        }
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

int OrcaCloudServiceAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_server_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_http_error_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    get_country_code_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Stub Implementations (Cloud Services, Model Mall, Analytics, Ratings)
// ============================================================================

int OrcaCloudServiceAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_my_message (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "[]";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::check_user_task_report(int* task_id, bool* printable)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: check_user_task_report (stub)";
    if (task_id) *task_id = 0;
    if (printable) *printable = false;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_user_print_info (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_user_tasks (stub)";
    if (http_body) *http_body = "[]";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_printer_firmware (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_task_plate_index(std::string task_id, int* plate_index)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_task_plate_index (stub)";
    if (plate_index) *plate_index = 0;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_user_info(int* identifier)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_user_info (stub)";
    if (identifier) *identifier = 0;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_subtask_info (stub)";
    if (task_json) *task_json = "{}";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_slice_info (stub)";
    if (slice_json) *slice_json = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: query_bind_status (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::modify_printer_name(std::string dev_id, std::string dev_name)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: modify_printer_name (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_camera_url (stub)";
    if (callback) callback("");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_design_staffpick (stub)";
    if (callback) callback("[]");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: start_publish (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_model_publish_url(std::string* url)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_model_publish_url (stub)";
    if (url) *url = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_subtask (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_model_mall_home_url(std::string* url)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_model_mall_home_url (stub)";
    if (url) *url = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_model_mall_detail_url(std::string* url, std::string id)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_model_mall_detail_url (stub)";
    if (url) *url = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_my_profile (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_my_token (stub) - Orca cloud uses code-based OAuth, not tickets";
    if (http_code) *http_code = 0;
    if (http_body) *http_body = "";
    return -1;
}

int OrcaCloudServiceAgent::track_enable(bool enable)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    enable_track = enable;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::track_remove_files()
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: track_remove_files (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::track_event(std::string evt_key, std::string content)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: track_event (stub) - " << evt_key;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::track_header(std::string header)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: track_header (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::track_update_property(std::string name, std::string value, std::string type)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: track_update_property (stub) - " << name;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::track_get_property(std::string name, std::string& value, std::string type)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: track_get_property (stub) - " << name;
    value = "";
    return BAMBU_NETWORK_SUCCESS;
}

bool OrcaCloudServiceAgent::get_track_enable()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return enable_track;
}

int OrcaCloudServiceAgent::put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: put_model_mall_rating (stub)";
    http_code = 200;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_oss_config (stub)";
    config = "{}";
    http_code = 200;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: put_rating_picture_oss (stub)";
    http_code = 200;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_model_mall_rating_result (stub)";
    rating_result = "{}";
    http_code = 200;
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaCloudServiceAgent::get_cloud_service_host()
{
    return api_base_url;
}

std::string OrcaCloudServiceAgent::get_cloud_login_url(const std::string& language)
{
    std::string url = cloud_base_url + ORCA_CLOUD_LOGIN_PATH;
    if (!language.empty()) {
        url += "?lang=" + language;
    }
    return url;
}

int OrcaCloudServiceAgent::get_mw_user_preference(std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_mw_user_preference (stub)";
    if (callback) callback("{}");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(debug) << "OrcaCloudServiceAgent: get_mw_user_4ulist (stub)";
    if (callback) callback("[]");
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaCloudServiceAgent::get_version()
{
    return "OrcaCloudServiceAgent 1.0.0";
}

// ============================================================================
// Bundle Subscription Implementation
// ============================================================================

bool OrcaCloudServiceAgent::unsubscribe_bundle(const std::string& bundle_id)
{
    std::string path = std::string(ORCA_SUBSCRIPTIONS_PATH) + "/" + bundle_id;
    std::string response;
    unsigned int http_code = 0;

    int result = http_delete(path, &response, &http_code);
    if (http_code >= 400) {
        return false;
    }

    return true;
}

std::string OrcaCloudServiceAgent::get_bundle_url(const std::string& bundle_id) const
{
    return cloud_base_url + "/app/bundles/subscribed-bundles/" + bundle_id;
}

int OrcaCloudServiceAgent::get_subscribed_bundles(std::vector<std::pair<std::string, std::string>>* bundles,std::vector<std::string>& notfound, std::vector<std::string>& unauthorized)
{
    if (!bundles) return -1;

    std::string response_body;
    unsigned int http_code = 0;

    // GET /api/v1/bundles
    int result = http_get("/api/v1/subscriptions", &response_body, &http_code);

    if (result != 0 || http_code != 200) {
        BOOST_LOG_TRIVIAL(error) << "get_subscribed_bundles failed: http_code=" << http_code
                                 << ", response=" << response_body;
        return result != 0 ? result : http_code;
    }

    // Parse JSON response
    try {
        auto json = nlohmann::json::parse(response_body);

        if (!json.contains("data") || !json["data"].is_array()) {
            BOOST_LOG_TRIVIAL(error) << "get_subscribed_bundles: invalid response format";
            return -1;
        }
        if(json.contains("not_found") )
        {
            //not found warning
            for(const auto& not_found : json["not_found"])
            {
                notfound.push_back(not_found);
            }
        }
        if(json.contains("unauthorised") )
        {
            // populate something to be iterated to show warning notifications
            for(const auto& u : json["unauthorised"])
            {
                unauthorized.push_back(u);
            }
        }
        // if(json.contains("privated") && json["privated"].is_array() && !json["privated"].empty())
        // {
        //     // populate something to be iterated to show warning notifications
        //     for(const auto& not_found : json["privated"])
        //     {
                
        //     }
        // }

        for (const auto& bundle_json : json["data"]) {
            bundles->push_back(std::make_pair(bundle_json["id"].get<std::string>(), bundle_json["version"].get<std::string>()));
        }

        BOOST_LOG_TRIVIAL(info) << "get_subscribed_bundles: loaded " << bundles->size() << " bundles";
        return 0;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "get_subscribed_bundles: JSON parse error: " << e.what();
        return -1;
    }
}

int OrcaCloudServiceAgent::get_shared_bundle(const std::string& bundle_id, std::map<std::string, std::map<std::string, std::string>>* presets, BundleMetadata* bundle_metadata)
{
    if (!presets) return -1;

    std::string response_body;
    unsigned int http_code = 0;

    // GET /api/v1/bundles/{id}
    std::string path = "/api/v1/bundles/" + bundle_id;
    int result = http_get(path, &response_body, &http_code);

    if (result != 0 || http_code != 200) {
        BOOST_LOG_TRIVIAL(error) << "get_shared_bundle failed: bundle_id=" << bundle_id
                                 << ", http_code=" << http_code;
        return result != 0 ? result : http_code;
    }

    // Parse JSON response
    try {
        auto json = nlohmann::json::parse(response_body);

        BOOST_LOG_TRIVIAL(info) << "get_shared_bundle: response: " << response_body;
        BOOST_LOG_TRIVIAL(info) << "get_shared_bundle: shared_profile: " << json["shared_profiles"];

        // Parse the bundle metadata
        if (json.contains("id")) bundle_metadata->id = json["id"].get<std::string>();
        if (json.contains("name")) bundle_metadata->name = json["name"].get<std::string>();
        if (json.contains("version")) bundle_metadata->version = json["version"].get<std::string>();
        if (json.contains("description")) bundle_metadata->description = json["description"].get<std::string>();
        if (json.contains("author")) bundle_metadata->author = json["author"].get<std::string>();
        if (json.contains("updated_time")) bundle_metadata->updated_time = json["updated_time"].get<long long>();

        if (!json.contains("shared_profiles") || !json["shared_profiles"].is_array()) {
            BOOST_LOG_TRIVIAL(error) << "get_shared_bundle: invalid response format";
            return -1;
        }

        for (auto& preset_object : json["shared_profiles"]) {
            BOOST_LOG_TRIVIAL(info) << "shared profile object: " << preset_object;

            // Extract preset name and content
            std::string preset_name = preset_object.value("name", "");
            if (preset_name.empty()) {
                BOOST_LOG_TRIVIAL(warning) << "get_shared_bundle: preset has no name, skipping";
                continue;
            }

            // Parse content JSON into key-value pairs using helper (same as get_user_presets)
            std::map<std::string, std::string> value_map;
            if (preset_object.contains("content") && preset_object["content"].is_object()) {
                json_to_map(preset_object["content"].dump(), value_map);
            }

            // Add metadata fields to match get_user_presets format
            // These are required by PresetCollection::load_user_preset
            if (preset_object.contains("id") && value_map.find(BBL_JSON_KEY_SETTING_ID) == value_map.end()) {
                value_map[BBL_JSON_KEY_SETTING_ID] = preset_object["id"];
            }
            if (value_map.find(BBL_JSON_KEY_USER_ID) == value_map.end()) {
                // Bundle presets don't have a user_id in the traditional sense
                // Use bundle_id as a placeholder to indicate source
                value_map[BBL_JSON_KEY_USER_ID] = "bundle:" + bundle_id;
            }
            if (preset_object.contains("updated_time") && value_map.find(ORCA_JSON_KEY_UPDATE_TIME) == value_map.end()) {
                value_map[ORCA_JSON_KEY_UPDATE_TIME] = preset_object["updated_time"].dump();
            }
            if (value_map.find(BBL_JSON_KEY_NAME) == value_map.end()) {
                value_map[BBL_JSON_KEY_NAME] = preset_name;
            }

            // Store as: presets[preset_name][key] = value
            // This matches the format expected by PresetBundle::load_user_presets
            (*presets)[preset_name] = value_map;
        }

        BOOST_LOG_TRIVIAL(info) << "get_shared_bundle: loaded " << presets->size()
                                << " presets for bundle_id=" << bundle_id;
        return 0;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "get_shared_bundle: JSON parse error: " << e.what();
        return -1;
    }
}

} // namespace Slic3r
