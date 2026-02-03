#include "OrcaCloudServiceAgent.hpp"
#include "Http.hpp"
#include "slic3r/Utils/InstanceID.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

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

#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/secretstore.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace pt = boost::property_tree;

namespace Slic3r {

namespace {
constexpr const char* ORCA_DEFAULT_API_URL = "https://xxx.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_AUTH_URL = "https://xxx.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_PUB_KEY = "xxxxxxxxxxxxx";
constexpr const char* ORCA_HEALTH_PATH = "/api/v1/health";
constexpr const char* ORCA_SYNC_PULL_PATH = "/api/v1/sync/pull";
constexpr const char* ORCA_SYNC_PUSH_PATH = "/api/v1/sync/push";
constexpr const char* ORCA_PROFILES_PATH = "/api/v1/profiles";
constexpr const char* ORCA_SYNC_STATE_FILE = "sync_state";

constexpr const char* CONFIG_ORCA_API_URL = "orca_api_url";
constexpr const char* CONFIG_ORCA_AUTH_URL = "orca_auth_url";
constexpr const char* CONFIG_ORCA_PUB_KEY = "orca_pub_key";

constexpr const char* SECRET_STORE_SERVICE = "OrcaSlicer/Auth";
constexpr const char* SECRET_STORE_USER    = "orca_refresh_token";
constexpr std::chrono::seconds TOKEN_REFRESH_SKEW{900}; // 15 minutes

std::string generate_uuid(const std::string& name = "")
{
    if (name.empty()) {
        return "";
    }

    // Use a fixed namespace UUID for OrcaSlicer profiles
    // This ensures the same name always generates the same UUID
    static const boost::uuids::uuid orca_namespace =
        boost::uuids::string_generator()("f47ac10b-58cc-4372-a567-0e02b2c3d479");

    boost::uuids::name_generator_sha1 gen(orca_namespace);
    boost::uuids::uuid id = gen(name);
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

std::string machine_identifier()
{
    if (auto* cfg = Slic3r::GUI::wxGetApp().app_config) {
        const auto iid = Slic3r::instance_id::ensure(*cfg);
        if (!iid.empty()) return iid;
    }

#if defined(__linux__)
    std::ifstream f("/etc/machine-id");
    std::string id;
    if (f.good()) {
        std::getline(f, id);
    }
    if (!id.empty()) return id;
#elif defined(_WIN32)
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(buffer, &size)) {
        return std::string(buffer, size);
    }
#elif defined(__APPLE__)
    char uuid_str[128] = {0};
    size_t len = sizeof(uuid_str);
    if (sysctlbyname("kern.uuid", uuid_str, &len, nullptr, 0) == 0 && len > 0) {
        return std::string(uuid_str, len - 1);
    }
#endif
    return wxGetUserId().ToStdString() + "@" + wxGetHostName().ToStdString();
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
    // Validate PKCE state
    const auto expected_state = pkce_bundle.state;
    if (!expected_state.empty() && state != expected_state) {
        BOOST_LOG_TRIVIAL(warning) << "[auth] event=code_exchange result=failure reason=state_mismatch";
        return false;
    }

    std::string url = auth_base_url + auth_constants::TOKEN_PATH;

    std::string redirect_uri;
    std::string code_verifier;
    std::map<std::string, std::string> headers_copy;
    {
        std::lock_guard<std::mutex> lock(headers_mutex);
        headers_copy = extra_headers;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        redirect_uri = pkce_bundle.redirect;
        code_verifier = pkce_bundle.verifier;
        for (const auto& pair : auth_headers) {
            headers_copy[pair.first] = pair.second;
        }
    }

    bool has_apikey = false;
    for (const auto& pair : headers_copy) {
        if (pair.first == "apikey") {
            has_apikey = true;
            break;
        }
    }
    if (!has_apikey) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: exchange_auth_code - apikey header MISSING! Token request may fail.";
    }

    std::string response;
    unsigned int http_code = 0;
    bool success = false;

    try {
        auto http = Http::post(url);

        for (const auto& pair : headers_copy) {
            http.header(pair.first, pair.second);
        }

        http.remove_header("Authorization");
        http.remove_header("Content-Type");
        http.form_add("grant_type", "authorization_code");
        http.form_add("code", auth_code);
        http.form_add("redirect_uri", redirect_uri);
        http.form_add("code_verifier", code_verifier);

        http.on_complete([&](std::string body, unsigned resp_status) {
                success = true;
                http_code = resp_status;
                response = body;
            })
            .on_error([&](std::string body, std::string error, unsigned resp_status) {
                success = false;
                http_code = resp_status;
                response = body;
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: exchange_auth_code exception - " << e.what();
    }

    if (!success || http_code >= 400) {
        BOOST_LOG_TRIVIAL(error) << "[auth] event=code_exchange result=failure http_code=" << http_code;
        return false;
    }

    session_payload = response;
    BOOST_LOG_TRIVIAL(info) << "[auth] event=code_exchange result=success";
    return true;
}

int OrcaCloudServiceAgent::change_user(std::string user_info)
{
    try {
        std::stringstream ss(user_info);
        pt::ptree tree;
        pt::read_json(ss, tree);

        auto read_str = [](const pt::ptree& node, const std::string& path) {
            return node.get<std::string>(path, "");
        };

        // Check if this is a WebView login message (PKCE flow completion)
        std::string command = tree.get<std::string>("command", "");
        if (command == "user_login") {

            auto data_opt = tree.get_child_optional("data");
            if (!data_opt) {
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: WebView login payload missing data field";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            pt::ptree data = *data_opt;
            std::string state = read_str(data, "state");

            // Check for auth code (PKCE authorization code flow)
            std::string auth_code = read_str(data, "code");
            if (!auth_code.empty()) {
                std::string session_payload;
                if (!exchange_auth_code(auth_code, state, session_payload)) {
                    invoke_user_login_callback(0, false);
                    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
                }
                // Recursively process the session payload (contains access_token, user, etc.)
                return change_user(session_payload);
            }

            // Direct token flow (tokens already obtained by WebView)
            std::string token = read_str(data, "token");
            std::string user_id = read_str(data, "user_id");
            std::string username = read_str(data, "username");
            std::string name = read_str(data, "name");
            std::string nickname = read_str(data, "nickname");
            std::string avatar = read_str(data, "avatar");
            std::string refresh_token = read_str(data, "refresh_token");

            // Validate PKCE state
            const auto expected_state = pkce_bundle.state;
            if (!expected_state.empty() && state != expected_state) {
                BOOST_LOG_TRIVIAL(warning) << "[auth] event=login result=failure reason=state_mismatch";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            if (token.empty() || user_id.empty()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: WebView login - token or user_id empty";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            bool success = set_user_session(token, user_id, username, name, nickname, avatar, refresh_token);
            if (success) {
                invoke_user_login_callback(1, true);
                if (on_login_complete_handler) {
                    on_login_complete_handler(true, user_id);
                }
            } else {
                invoke_user_login_callback(0, false);
            }
            BOOST_LOG_TRIVIAL(info) << "[auth] event=login result=" << (success ? "success" : "failure")
                                    << " source=webview user_id=" << user_id;
            return success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }

        // Orca cloud session payload (default flow)
        const pt::ptree* session_node = nullptr;
        auto data_opt = tree.get_child_optional("data");
        if (data_opt) {
            if (data_opt->get_child_optional("session")) {
                session_node = &data_opt->get_child("session");
            } else if (data_opt->get_optional<std::string>("access_token") ||
                       data_opt->get_optional<std::string>("token")) {
                session_node = &*data_opt;
            }
        }
        if (!session_node) {
            if (tree.get_child_optional("session")) {
                session_node = &tree.get_child("session");
            } else if (tree.get_optional<std::string>("access_token") ||
                       tree.get_optional<std::string>("token")) {
                session_node = &tree;
            }
        }

        if (session_node) {
            std::string access_token = read_str(*session_node, "access_token");
            if (access_token.empty()) {
                access_token = read_str(*session_node, "token");
            }
            std::string refresh_token = read_str(*session_node, "refresh_token");
            std::string user_id = read_str(*session_node, "user.id");
            std::string email = read_str(*session_node, "user.email");
            std::string full_name = read_str(*session_node, "user.user_metadata.full_name");
            std::string preferred_username = read_str(*session_node, "user.user_metadata.preferred_username");
            std::string avatar = read_str(*session_node, "user.user_metadata.avatar_url");
            std::string username = !preferred_username.empty() ? preferred_username : email;
            std::string name = !full_name.empty() ? full_name : (!preferred_username.empty() ? preferred_username : email);
            std::string nickname = !preferred_username.empty() ? preferred_username : username;
            if (nickname.empty()) nickname = name;
            if (nickname.empty()) nickname = email;

            if (access_token.empty() || user_id.empty()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: Orca cloud login payload missing access_token or user.id";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            bool success = set_user_session(access_token, user_id, username, name, nickname, avatar, refresh_token);
            if (success) {
                invoke_user_login_callback(1, true);
                if (on_login_complete_handler) {
                    on_login_complete_handler(true, user_id);
                }
            } else {
                invoke_user_login_callback(0, false);
            }
            return success ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }

        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: Username/password login is disabled. Use the Orca cloud PKCE flow.";
        invoke_user_login_callback(0, false);
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: change_user exception - " << e.what();
        invoke_user_login_callback(0, false);
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
            pt::ptree logout_req;
            if (!refresh_copy.empty()) {
                logout_req.put("refresh_token", refresh_copy);
            }
            std::stringstream body_ss;
            if (!logout_req.empty()) {
                pt::write_json(body_ss, logout_req);
            } else {
                body_ss << "{}";
            }

            int result = http_post_auth(auth_constants::LOGOUT_PATH, body_ss.str(), &response, &http_code) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
            if (result != BAMBU_NETWORK_SUCCESS || http_code >= 400) {
                BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: Orca cloud logout request failed - http_code=" << http_code;
            }
        }
    }

    clear_session();
    invoke_user_login_callback(0, false);
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
        pt::ptree cmd;
        cmd.put("command", "studio_userlogin");

        pt::ptree data;
        std::string display_name = get_user_nickname();
        if (display_name.empty()) {
            display_name = get_user_name();
        }
        data.put("name", display_name);
        data.put("avatar", get_user_avatar());
        cmd.add_child("data", data);

        std::stringstream ss;
        pt::write_json(ss, cmd, false);
        return ss.str();
    }

    update_redirect_uri();
    regenerate_pkce();
    const auto bundle = pkce();

    pt::ptree tree;
    tree.put("action", "login_config");
    tree.put("backend_url", auth_base_url);

    // Include API key for direct Supabase calls from JavaScript
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        auto it = auth_headers.find("apikey");
        if (it != auth_headers.end()) {
            tree.put("apikey", it->second);
        }
    }

    pt::ptree pkce_node;
    pkce_node.put("code_challenge", bundle.challenge);
    pkce_node.put("code_challenge_method", "S256");
    pkce_node.put("state", bundle.state);
    pkce_node.put("redirect_uri", bundle.redirect);
    pkce_node.put("code_verifier", bundle.verifier);
    pkce_node.put("loopback_port", bundle.loopback_port);
    tree.add_child("pkce", pkce_node);

    std::stringstream ss;
    pt::write_json(ss, tree, false);
    return ss.str();
}

std::string OrcaCloudServiceAgent::build_logout_cmd()
{
    pt::ptree tree;
    tree.put("action", "logout");
    tree.put("provider", "orca");

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

std::string OrcaCloudServiceAgent::build_login_info()
{
    pt::ptree tree;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        tree.put("user_id", session.user_id);
        tree.put("user_name", session.user_name);
        tree.put("nickname", session.user_nickname);
        tree.put("avatar", session.user_avatar);
        tree.put("logged_in", session.logged_in);
    }
    // Do not expose tokens to the WebView
    tree.put("access_token", "");
    tree.put("refresh_token", "");
    tree.put("backend_url", api_base_url);
    tree.put("auth_url", auth_base_url);

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
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
        for (const auto& upsert : resp.upserts) {
            std::string preset_type = IOT_PRINT_TYPE_STRING;
            if (upsert.content.contains("type") && upsert.content["type"].is_string()) {
                preset_type = upsert.content["type"].get<std::string>();
            }

            (*user_presets)[preset_type][upsert.id] = upsert.content.dump();
        }

        if (!resp.next_cursor.empty()) {
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
    std::string new_id = generate_uuid(name);
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

    // Use sync_push to create the profile (no original_updated_at for new profiles per spec)
    auto result = sync_push(new_id, name, content, "");
    if (http_code) *http_code = result.http_code;

    if (result.success) {
        if (values_map && !result.new_updated_at.empty()) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = result.new_updated_at;
        }
        return new_id;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: request_setting_id failed - " << result.error_message;
    return "";
}

int OrcaCloudServiceAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    // Extract original_updated_at for Optimistic Concurrency Control
    // If present, server will verify version before update. If absent, treated as insert.
    // If present, server will verify version before update. If absent, treated as insert.
    std::string original_updated_at;
    if (values_map) {
        auto it = values_map->find(IOT_JSON_KEY_UPDATED_TIME);
        if (it != values_map->end()) {
            original_updated_at = it->second;
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

    auto result = sync_push(setting_id, name, content, original_updated_at);
    if (http_code) *http_code = result.http_code;

    if (result.success) {
        if (values_map && !result.new_updated_at.empty()) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = result.new_updated_at;
        }
        return BAMBU_NETWORK_SUCCESS;
    }

    if (result.http_code == 409) {
        // Conflict - update values_map with server version
        if (values_map && !result.server_version.updated_at.empty()) {
            (*values_map)[IOT_JSON_KEY_UPDATED_TIME] = result.server_version.updated_at;
        }
        BOOST_LOG_TRIVIAL(warning) << "OrcaCloudServiceAgent: put_setting conflict - server_updated_at="
                                   << result.server_version.updated_at;
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
                info[IOT_JSON_KEY_UPDATED_TIME] = upsert.updated_at;

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

        if (!cancelled && !resp.next_cursor.empty()) {
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
    std::string path = std::string(ORCA_PROFILES_PATH) + "/" + setting_id;
    std::string response;
    unsigned int http_code = 0;

    int result = http_delete(path, &response, &http_code);
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
    if (!sync_state.last_sync_timestamp.empty()) {
        path += "?cursor=" + sync_state.last_sync_timestamp;
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
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: sync_pull failed - http_code=" << http_code;
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
        resp.next_cursor = json.value("next_cursor", "");

        if (json.contains("upserts") && json["upserts"].is_array()) {
            for (const auto& item : json["upserts"]) {
                ProfileUpsert upsert;
                upsert.id = item.value("id", "");
                upsert.name = item.value("name", "");
                upsert.updated_at = item.value(ORCA_JSON_KEY_UPDATE_TIME, "");
                upsert.created_at = item.value(ORCA_JSON_KEY_CREATED_TIME, "");
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
    const std::string& original_updated_at)
{
    SyncPushResult result;
    result.success = false;
    result.http_code = 0;
    result.server_deleted = false;

    nlohmann::json body;
    body["id"] = profile_id;
    body["name"] = name;
    body["content"] = content;
    if (!original_updated_at.empty()) {
        body["original_updated_at"] = original_updated_at;
    }

    std::string response;
    unsigned int http_code = 0;
    int http_result = http_post(ORCA_SYNC_PUSH_PATH, body.dump(), &response, &http_code);

    result.http_code = http_code;

    if (http_result != BAMBU_NETWORK_SUCCESS) {
        result.error_message = "HTTP request failed";
        return result;
    }

    if (http_code == 409) {
        // Conflict - parse server version
        try {
            auto json = nlohmann::json::parse(response);
            if (json.is_null()) {
                result.server_deleted = true;
            } else {
                result.server_version.id = json.value("id", "");
                result.server_version.name = json.value("name", "");
                result.server_version.updated_at = json.value(ORCA_JSON_KEY_UPDATE_TIME, "");
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
        result.new_updated_at = json.value(ORCA_JSON_KEY_UPDATE_TIME, "");
        if (!result.new_updated_at.empty()) {
            result.success = true;
        } else {
            result.error_message = "Server response missing required updated_at timestamp";
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
                sync_state.last_sync_timestamp = line;
            }
        }
    } catch (...) {}
}

void OrcaCloudServiceAgent::save_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    if (sync_state_path.empty()) return;

    try {
        std::string tmp_path = sync_state_path + ".tmp";
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (ofs.good()) {
            ofs << sync_state.last_sync_timestamp;
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
        auto key = sha256_bytes(machine_identifier());
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
            auto key = sha256_bytes(machine_identifier());
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
        pt::ptree payload;
        std::stringstream ss(payload_str);
        pt::read_json(ss, payload);
        auto exp_opt = payload.get_optional<long long>("exp");
        if (exp_opt) {
            out_tp = std::chrono::system_clock::time_point{std::chrono::seconds(*exp_opt)};
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
        std::stringstream ss(response);
        pt::ptree tree;
        pt::read_json(ss, tree);

        auto read_str = [](const pt::ptree& node, const std::string& path) {
            return node.get<std::string>(path, "");
        };

        // Token refresh response has the same structure as login response
        std::string access_token = read_str(tree, "access_token");
        if (access_token.empty()) {
            access_token = read_str(tree, "token");
        }
        std::string new_refresh_token = read_str(tree, "refresh_token");
        std::string user_id = read_str(tree, "user.id");
        std::string email = read_str(tree, "user.email");
        std::string full_name = read_str(tree, "user.user_metadata.full_name");
        std::string preferred_username = read_str(tree, "user.user_metadata.preferred_username");
        std::string avatar = read_str(tree, "user.user_metadata.avatar_url");

        std::string username = !preferred_username.empty() ? preferred_username : email;
        std::string name = !full_name.empty() ? full_name : (!preferred_username.empty() ? preferred_username : email);
        std::string nickname = !preferred_username.empty() ? preferred_username : username;
        if (nickname.empty()) nickname = name;
        if (nickname.empty()) nickname = email;

        if (access_token.empty() || user_id.empty()) {
            BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: token refresh response missing access_token or user.id";
            invoke_user_login_callback(0, false);
            return false;
        }

        bool success = set_user_session(access_token, user_id, username, name, nickname, avatar, new_refresh_token);
        if (success) {
            invoke_user_login_callback(0, true);
            if (on_login_complete_handler) {
                on_login_complete_handler(true, user_id);
            }
        } else {
            invoke_user_login_callback(0, false);
        }
        return success;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaCloudServiceAgent: token refresh parse exception - " << e.what();
        invoke_user_login_callback(0, false);
        return false;
    }
}

// ============================================================================
// Auth - Session State Helpers
// ============================================================================

bool OrcaCloudServiceAgent::set_user_session(const std::string& token,
                                     const std::string& user_id,
                                     const std::string& username,
                                     const std::string& name,
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
        session.user_name = name.empty() ? username : name;
        session.user_nickname = nickname.empty() ? (!username.empty() ? username : name) : nickname;
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
    std::string url = auth_base_url + path;
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

void OrcaCloudServiceAgent::invoke_user_login_callback(int online_login, bool login)
{
    OnUserLoginFn callback;
    QueueOnMainFn queue_fn;

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callback = on_user_login_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, online_login, login]() {
                callback(online_login, login);
            });
        } else {
            callback(online_login, login);
        }
    }
}

void OrcaCloudServiceAgent::invoke_server_connected_callback(int return_code, int reason_code)
{
    OnServerConnectedFn callback;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_server_connected_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, return_code, reason_code]() {
                callback(return_code, reason_code);
            });
        } else {
            callback(return_code, reason_code);
        }
    }
}

void OrcaCloudServiceAgent::invoke_http_error_callback(unsigned http_code, const std::string& http_body)
{
    OnHttpErrorFn callback;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_http_error_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, http_code, http_body]() {
                callback(http_code, http_body);
            });
        } else {
            callback(http_code, http_body);
        }
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

int OrcaCloudServiceAgent::set_on_user_login_fn(OnUserLoginFn fn)
{
    std::lock_guard<std::mutex> lock(callback_mutex);
    on_user_login_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_server_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaCloudServiceAgent::set_on_http_error_fn(OnHttpErrorFn fn)
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

int OrcaCloudServiceAgent::set_extra_http_header(std::map<std::string, std::string> headers)
{
    std::lock_guard<std::mutex> lock(headers_mutex);
    extra_headers = headers;
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaCloudServiceAgent::get_cloud_service_host()
{
    return api_base_url;
}

std::string OrcaCloudServiceAgent::get_cloud_login_url(const std::string& language)
{
    // Orca uses a local HTML file for the login flow
    boost::filesystem::path login_path = boost::filesystem::path(resources_dir()) / "web" / "login" / "orca_login.html";
    return "file://" + login_path.make_preferred().string();
}

std::string OrcaCloudServiceAgent::get_studio_info_url()
{
    return api_base_url + "/studio/info";
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

} // namespace Slic3r
