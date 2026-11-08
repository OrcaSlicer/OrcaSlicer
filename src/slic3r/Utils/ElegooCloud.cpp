#include "ElegooCloud.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <openssl/evp.h>

#include <wx/string.h>

#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r_version.h"
#include "slic3r/GUI/I18N.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* API_GLOBAL      = "https://matrix.elegoo.com";
constexpr const char* API_CHINA       = "https://matrix.elegoo.com.cn";
constexpr const char* LOGIN_GLOBAL    = "https://www.nexprint.com/account/slicer-login";
constexpr const char* AGORA_APP_ID    = "d035320941e34cd5bc4ec106eff05580"; // public, from elegoo-link
constexpr const char* ELEGOO_LINK_RAW = "https://raw.githubusercontent.com/ELEGOO-3D/elegoo-link/main/thirdparty/agora";

constexpr int M_START_PRINT       = 1020;
constexpr int M_DOWNLOAD_FILE     = 1057;
constexpr int M_CANCEL_DOWNLOAD   = 1058;
constexpr int M_SET_AUTO_REFILL   = 2004;
constexpr int M_CANVAS_STATUS     = 2005;
constexpr int M_DOWNLOAD_PROGRESS = 6006;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string mask(const std::string& s) { return s.size() <= 4 ? std::string("****") : "****" + s.substr(s.size() - 4); }

std::string printer_error_text(int code)
{
    switch (code) {
    case 1000: return "printer rejected the token";
    case 1003: return "invalid parameter";
    case 1009: return "printer is busy";
    case 1021: return "file not found on printer";
    case 1026: return "bed leveling data missing";
    case 109: return "filament runout";
    default: return "error";
    }
}

// MD5 of a file: raw 16-byte digest.
std::string file_md5_raw(const fs::path& file)
{
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    if (!in)
        throw ElegooCloudError("Cannot read " + file.string());
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    std::vector<char> buf(1024 * 1024);
    while (in) {
        in.read(buf.data(), buf.size());
        if (in.gcount() > 0)
            EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(in.gcount()));
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    return std::string(reinterpret_cast<const char*>(digest), len);
}

std::string to_hex(const std::string& raw)
{
    static const char* digits = "0123456789abcdef";
    std::string        out;
    for (unsigned char c : raw) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string to_base64(const std::string& raw)
{
    std::string out(boost::beast::detail::base64::encoded_size(raw.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), raw.data(), raw.size()));
    return out;
}

// Depth-first search for the first non-empty value under any of the names (case-insensitive).
const json* find_key(const json& obj, const std::vector<std::string>& names)
{
    auto non_empty = [](const json& v) {
        return !(v.is_null() || (v.is_string() && v.get<std::string>().empty()) || (v.is_number() && v.get<double>() == 0));
    };
    if (obj.is_object()) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            std::string k = it.key();
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            for (const auto& n : names) {
                std::string ln = n;
                std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                if (k == ln && non_empty(*it))
                    return &*it;
            }
        }
        for (const auto& v : obj)
            if (const json* r = find_key(v, names))
                return r;
    } else if (obj.is_array()) {
        for (const auto& v : obj)
            if (const json* r = find_key(v, names))
                return r;
    }
    return nullptr;
}

std::string json_str(const json* v)
{
    if (v == nullptr)
        return {};
    if (v->is_string())
        return v->get<std::string>();
    if (v->is_number_integer())
        return std::to_string(v->get<int64_t>());
    return v->dump();
}

int64_t json_int(const json* v)
{
    if (v == nullptr)
        return 0;
    if (v->is_number())
        return v->get<int64_t>();
    if (v->is_string()) {
        try {
            return std::stoll(v->get<std::string>());
        } catch (...) {}
    }
    return 0;
}

#ifdef _WIN32
std::string dpapi(const std::string& in, bool protect)
{
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
    DATA_BLOB dst{0, nullptr};
    const BOOL ok = protect ? CryptProtectData(&src, L"Elegoo cloud", nullptr, nullptr, nullptr, 0, &dst) :
                              CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, 0, &dst);
    if (!ok)
        throw ElegooCloudError("Windows could not " + std::string(protect ? "encrypt" : "decrypt") + " the saved sign-in (DPAPI error " +
                               std::to_string(GetLastError()) + ")");
    std::string out(reinterpret_cast<const char*>(dst.pbData), dst.cbData);
    LocalFree(dst.pbData);
    return out;
}
#endif

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    std::stringstream       ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    const fs::path tmp = p.string() + ".part";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out.write(data.data(), data.size());
    }
#ifndef _WIN32
    ::chmod(tmp.string().c_str(), 0600);
#endif
    fs::rename(tmp, p);
}

// ---------------------------------------------------------------------------------------------
// Agora RTM 2.x, called through its C++ ABI with no SDK headers.
// Struct layouts and vtable order from IAgoraRtmClient.h / IAgoraRtmEventHandler.h, v2.2.8.
// ---------------------------------------------------------------------------------------------

struct RtmLogConfig { const char* filePath; uint32_t fileSizeInKB; int32_t level; };
struct RtmProxyConfig { int32_t proxyType; const char* server; uint16_t port; const char* account; const char* password; };
struct RtmEncryptionConfig { int32_t encryptionMode; const char* encryptionKey; uint8_t encryptionSalt[32]; };
struct RtmPrivateConfig { int32_t serviceType; const char** accessPointHosts; size_t accessPointHostsCount; };
struct RtmConfig
{
    const char*         appId;
    const char*         userId;
    uint32_t            areaCode;
    int32_t             protocolType;
    uint32_t            presenceTimeout;
    uint32_t            heartbeatInterval;
    void*               context;
    uint32_t            reconnectTimeout;
    bool                useStringUserId;
    bool                multipath;
    bool                ispPolicyEnabled;
    void*               eventHandler;
    RtmLogConfig        logConfig;
    RtmProxyConfig      proxyConfig;
    RtmEncryptionConfig encryptionConfig;
    RtmPrivateConfig    privateConfig;
};
struct RtmPublishOptions { int32_t channelType; int32_t messageType; const char* customType; bool storeInHistory; };
struct RtmSubscribeOptions { bool withMessage, withMetadata, withPresence, withLock, beQuiet; };
struct RtmLinkStateEvent
{
    int32_t     currentState, previousState, serviceType, operation, reasonCode;
    const char* reason;
    void*       affectedChannels;
    size_t      affectedChannelCount;
    void*       unrestoredChannels;
    size_t      unrestoredChannelCount;
    bool        isResumed;
    uint64_t    timestamp;
};
struct RtmMessageEvent
{
    int32_t     channelType, messageType;
    const char* channelName;
    const char* channelTopic;
    const char* message;
    size_t      messageLength;
    const char* publisher;
    const char* customType;
    uint64_t    timestamp;
};

constexpr uint32_t RTM_AREA_CODE_GLOB      = 0xFFFFFFFF;
constexpr int32_t  RTM_CHANNEL_TYPE_USER   = 3;
constexpr int32_t  RTM_MESSAGE_TYPE_STRING = 1;
constexpr int32_t  RTM_LOG_LEVEL_WARN      = 2;

// IRtmClient vtable (its destructor is declared last, so the order is the same on MSVC and Itanium).
constexpr int VT_RELEASE = 0, VT_LOGIN = 1, VT_LOGOUT = 2, VT_PUBLISH = 8, VT_SUBSCRIBE = 9;
// IRtmEventHandler: virtual destructor first (1 slot on MSVC, 2 on Itanium), then 47 methods.
#ifdef _WIN32
constexpr int EH_DTOR_SLOTS = 1;
#else
constexpr int EH_DTOR_SLOTS = 2;
#endif
constexpr int EH_METHODS = 47;
constexpr int EH_LINK_STATE = 0, EH_MESSAGE = 1, EH_SUBSCRIBE_RESULT = 17, EH_PUBLISH_RESULT = 19, EH_LOGIN_RESULT = 20;

struct AgoraLib
{
    void* rtm{nullptr};
    using CreateFn = void* (*) (const RtmConfig*, int*);
    using VersionFn = const char* (*) ();
    using ReasonFn  = const char* (*) (int);
    CreateFn  create{nullptr};
    VersionFn version{nullptr};
    ReasonFn  reason{nullptr};
};

std::vector<fs::path> agora_candidate_dirs()
{
    std::vector<fs::path> dirs{ElegooCloud::data_folder() / "agora"};
#ifdef _WIN32
    for (const char* env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const char* root = std::getenv(env);
        if (root == nullptr)
            continue;
        for (const char* sub : {"ElegooSlicer", "ELEGOO\\ElegooSlicer", "Programs\\ElegooSlicer", "Elegoo\\ElegooSlicer"})
            dirs.push_back(fs::path(root) / sub);
    }
#elif defined(__APPLE__)
    dirs.emplace_back("/Applications/ElegooSlicer.app/Contents/Frameworks");
#endif
    return dirs;
}

#ifdef _WIN32
constexpr const char* AGORA_MAIN = "agora_rtm_sdk.dll";
constexpr const char* AGORA_AOSL = "libaosl.dll";
#elif defined(__APPLE__)
constexpr const char* AGORA_MAIN = "AgoraRtmKit.framework/AgoraRtmKit";
constexpr const char* AGORA_AOSL = "aosl.framework/aosl";
#else
constexpr const char* AGORA_MAIN = "libagora_rtm_sdk.so";
constexpr const char* AGORA_AOSL = "libaosl.so";
#endif

fs::path find_agora_dir()
{
    boost::system::error_code ec;
    for (const fs::path& d : agora_candidate_dirs()) {
        if (fs::exists(d / AGORA_MAIN, ec))
            return d;
        if (!fs::is_directory(d, ec))
            continue;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) // one level deep, e.g. ...\ElegooSlicer\bin
            if (fs::exists(it->path() / AGORA_MAIN, ec))
                return it->path();
    }
    return {};
}

fs::path download_agora_libs(const ElegooProgressFn& progress)
{
    std::vector<std::pair<std::string, std::string>> files;
#ifdef _WIN32
    files = {{"agora_rtm_sdk.dll", "windows/lib/x86_64/agora_rtm_sdk.dll"}, {"libaosl.dll", "windows/lib/x86_64/libaosl.dll"}};
#elif defined(__linux__)
    files = {{"libagora_rtm_sdk.so", "linux/libagora_rtm_sdk.so"}, {"libaosl.so", "linux/libaosl.so"}};
#else
    throw ElegooCloudError(_u8L("The Elegoo messaging library was not found. Install ElegooSlicer once so it can be loaded from there."));
#endif
    const fs::path target = ElegooCloud::data_folder() / "agora";
    fs::create_directories(target);
    for (const auto& [name, rel] : files) {
        if (progress)
            progress(50, _u8L("Downloading the Elegoo messaging library…"));
        std::string error;
        std::string body;
        Http::get(std::string(ELEGOO_LINK_RAW) + "/" + rel)
            .header("User-Agent", ElegooCloud::user_agent())
            .timeout_max(300)
            .on_complete([&](std::string b, unsigned) { body = std::move(b); })
            .on_error([&](std::string, std::string err, unsigned status) { error = err + " (HTTP " + std::to_string(status) + ")"; })
            .perform_sync();
        if (!error.empty() || body.empty())
            throw ElegooCloudError("Downloading " + name + " failed: " + error);
        write_file(target / name, body);
    }
    return target;
}

AgoraLib& load_agora(const ElegooProgressFn& progress)
{
    static AgoraLib   lib;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    if (lib.create != nullptr)
        return lib;
    fs::path dir = find_agora_dir();
    if (dir.empty())
        dir = download_agora_libs(progress);
#ifdef _WIN32
    const DWORD flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
    LoadLibraryExW((dir / AGORA_AOSL).wstring().c_str(), nullptr, flags);
    HMODULE h = LoadLibraryExW((dir / AGORA_MAIN).wstring().c_str(), nullptr, flags);
    if (h == nullptr)
        throw ElegooCloudError("Could not load " + (dir / AGORA_MAIN).string() + " (error " + std::to_string(GetLastError()) + ")");
    lib.rtm     = h;
    lib.create  = reinterpret_cast<AgoraLib::CreateFn>(GetProcAddress(h, "createAgoraRtmClient"));
    lib.version = reinterpret_cast<AgoraLib::VersionFn>(GetProcAddress(h, "getVersion"));
    lib.reason  = reinterpret_cast<AgoraLib::ReasonFn>(GetProcAddress(h, "getErrorReason"));
#else
    dlopen((dir / AGORA_AOSL).string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    void* h = dlopen((dir / AGORA_MAIN).string().c_str(), RTLD_NOW);
    if (h == nullptr) {
        const char* err = dlerror();
        throw ElegooCloudError("Could not load " + (dir / AGORA_MAIN).string() + ": " + (err ? err : "unknown error"));
    }
    lib.rtm     = h;
    lib.create  = reinterpret_cast<AgoraLib::CreateFn>(dlsym(h, "createAgoraRtmClient"));
    lib.version = reinterpret_cast<AgoraLib::VersionFn>(dlsym(h, "getVersion"));
    lib.reason  = reinterpret_cast<AgoraLib::ReasonFn>(dlsym(h, "getErrorReason"));
#endif
    if (lib.create == nullptr)
        throw ElegooCloudError("The Agora RTM library in " + dir.string() + " has no createAgoraRtmClient.");
    BOOST_LOG_TRIVIAL(info) << "ElegooCloud: Agora RTM " << (lib.version ? lib.version() : "?") << " loaded from " << dir.string();
    return lib;
}

// Minimal RTM client: login, subscribe, publish to a user channel, receive messages.
class AgoraRtm
{
public:
    using MessageFn = std::function<void(const std::string& publisher, const std::string& body)>;

    AgoraRtm(AgoraLib& lib, const std::string& user_id, MessageFn on_message) : m_lib(lib), m_on_message(std::move(on_message))
    {
        for (auto& slot : m_vtable)
            slot = reinterpret_cast<void*>(&AgoraRtm::noop);
        m_vtable[EH_DTOR_SLOTS + EH_LINK_STATE]       = reinterpret_cast<void*>(&AgoraRtm::on_link_state);
        m_vtable[EH_DTOR_SLOTS + EH_MESSAGE]          = reinterpret_cast<void*>(&AgoraRtm::on_message);
        m_vtable[EH_DTOR_SLOTS + EH_SUBSCRIBE_RESULT] = reinterpret_cast<void*>(&AgoraRtm::on_str_result);
        m_vtable[EH_DTOR_SLOTS + EH_PUBLISH_RESULT]   = reinterpret_cast<void*>(&AgoraRtm::on_result);
        m_vtable[EH_DTOR_SLOTS + EH_LOGIN_RESULT]     = reinterpret_cast<void*>(&AgoraRtm::on_result);
        m_handler.vtbl  = m_vtable;
        m_handler.owner = this;

        m_user_id  = user_id;
        m_log_path = (ElegooCloud::data_folder() / "agora_rtm.log").string();
        RtmConfig cfg{};
        cfg.appId                = AGORA_APP_ID;
        cfg.userId               = m_user_id.c_str();
        cfg.areaCode             = RTM_AREA_CODE_GLOB;
        cfg.presenceTimeout      = 300;
        cfg.heartbeatInterval    = 30;
        cfg.reconnectTimeout     = 15;
        cfg.useStringUserId      = true;
        cfg.eventHandler         = &m_handler;
        cfg.logConfig.filePath   = m_log_path.c_str();
        cfg.logConfig.fileSizeInKB = 1024;
        cfg.logConfig.level      = RTM_LOG_LEVEL_WARN;
        int err                  = 0;
        m_client                 = m_lib.create(&cfg, &err);
        if (m_client == nullptr || err != 0)
            throw ElegooCloudError("Agora RTM client could not be created (error " + std::to_string(err) + ")");
    }

    ~AgoraRtm()
    {
        if (m_client == nullptr)
            return;
        uint64_t rid = 0;
        vcall<int, uint64_t*>(VT_LOGOUT, &rid);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        vcall<int>(VT_RELEASE);
        m_client = nullptr;
    }

    void login(const std::string& token)
    {
        uint64_t rid = 0;
        vcall<int, const char*, uint64_t*>(VT_LOGIN, token.c_str(), &rid);
        wait_result(rid, 20, "login");
    }

    void subscribe(const std::string& channel)
    {
        RtmSubscribeOptions opts{true, false, true, false, false};
        uint64_t            rid = 0;
        vcall<int, const char*, const RtmSubscribeOptions*, uint64_t*>(VT_SUBSCRIBE, channel.c_str(), &opts, &rid);
        wait_result(rid, 10, "subscribe");
    }

    void publish_to_user(const std::string& user, const std::string& message)
    {
        RtmPublishOptions opts{RTM_CHANNEL_TYPE_USER, RTM_MESSAGE_TYPE_STRING, "PlainText", false};
        uint64_t          rid = 0;
        vcall<int, const char*, const char*, size_t, const RtmPublishOptions*, uint64_t*>(VT_PUBLISH, user.c_str(), message.data(),
                                                                                          message.size(), &opts, &rid);
        wait_result(rid, 10, "publish");
    }

private:
    struct Handler
    {
        void**    vtbl;
        AgoraRtm* owner;
    };

    template<typename R, typename... Args> R vcall(int index, Args... args)
    {
        void** vtbl = *reinterpret_cast<void***>(m_client);
        auto   fn   = reinterpret_cast<R (*)(void*, Args...)>(vtbl[index]);
        return fn(m_client, args...);
    }

    static AgoraRtm* self(void* handler) { return static_cast<Handler*>(handler)->owner; }
    static void      noop(void*) {}
    static void      on_link_state(void* h, const RtmLinkStateEvent* ev)
    {
        if (ev != nullptr)
            BOOST_LOG_TRIVIAL(info) << "ElegooCloud: RTM link state " << ev->previousState << " -> " << ev->currentState << " ("
                                    << (ev->reason ? ev->reason : "") << ")";
        (void) h;
    }
    static void on_message(void* h, const RtmMessageEvent* ev)
    {
        if (ev == nullptr)
            return;
        const std::string body = ev->message ? std::string(ev->message, ev->messageLength) : std::string();
        const std::string pub  = ev->publisher ? ev->publisher : "";
        try {
            self(h)->m_on_message(pub, body);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ElegooCloud: RTM message handler: " << e.what();
        }
    }
    static void on_result(void* h, uint64_t request_id, int32_t error)
    {
        AgoraRtm*                   s = self(h);
        std::lock_guard<std::mutex> lock(s->m_mutex);
        s->m_results[request_id] = error;
        s->m_cond.notify_all();
    }
    static void on_str_result(void* h, uint64_t request_id, const char*, int32_t error) { on_result(h, request_id, error); }

    void wait_result(uint64_t request_id, int timeout_s, const char* what)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cond.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return m_results.count(request_id) > 0; }))
            throw ElegooCloudError(std::string("Agora RTM ") + what + " timed out");
        const int err = m_results[request_id];
        m_results.erase(request_id);
        if (err != 0) {
            const char* reason = m_lib.reason ? m_lib.reason(err) : nullptr;
            throw ElegooCloudError(std::string("Agora RTM ") + what + " failed: error " + std::to_string(err) + " " + (reason ? reason : ""));
        }
    }

    AgoraLib&                      m_lib;
    MessageFn                      m_on_message;
    void*                          m_vtable[EH_DTOR_SLOTS + EH_METHODS + 8]; // spare slots for newer runtimes
    Handler                        m_handler{};
    void*                          m_client{nullptr};
    std::string                    m_user_id;
    std::string                    m_log_path;
    std::mutex                     m_mutex;
    std::condition_variable        m_cond;
    std::map<uint64_t, int32_t>    m_results;
};

// Request/response over RTM to one printer, plus download progress (6006).
class PrinterChannel
{
public:
    PrinterChannel(const json& cred, const std::string& serial, const ElegooProgressFn& progress)
    {
        AgoraLib& lib  = load_agora(progress);
        m_printer_user = cred.value("user_id", "") + serial;
        m_next_id      = static_cast<int>(now_ms() / 1000 % 100000) * 10;
        m_rtm          = std::make_unique<AgoraRtm>(lib, cred.value("rtm_user_id", ""),
                                           [this](const std::string& pub, const std::string& body) { on_message(pub, body); });
        if (progress)
            progress(-1, _u8L("Connecting to Elegoo messaging…"));
        m_rtm->login(cred.value("rtm_token", ""));
        try {
            m_rtm->subscribe(cred.value("user_id", ""));
        } catch (const ElegooCloudError& e) { // replies come as user-channel messages anyway
            BOOST_LOG_TRIVIAL(info) << "ElegooCloud: subscribe skipped: " << e.what();
        }
    }

    json request(int method, const json& params, int timeout_s = 10, bool need_reply = true)
    {
        int rid;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            rid = ++m_next_id;
        }
        const std::string msg = json{{"id", rid}, {"method", method}, {"params", params}}.dump();
        BOOST_LOG_TRIVIAL(info) << "ElegooCloud: RTM -> printer: " << msg.substr(0, 300);
        m_rtm->publish_to_user(m_printer_user, msg);
        if (!need_reply)
            return {};
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cond.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return m_responses.count(rid) > 0; }))
            throw ElegooCloudError("The printer did not answer method " + std::to_string(method) + " within " + std::to_string(timeout_s) +
                                   " s (is it online?)");
        json reply = std::move(m_responses[rid]);
        m_responses.erase(rid);
        const int code = reply.contains("result") && reply["result"].is_object() ? reply["result"].value("error_code", 0) : 0;
        if (code != 0)
            throw ElegooCloudError("The printer refused method " + std::to_string(method) + ": " + printer_error_text(code) + " (error " +
                                   std::to_string(code) + ")");
        return reply;
    }

    json download_state()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_download;
    }
    void reset_download()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_download = json();
    }

private:
    void on_message(const std::string& publisher, const std::string& body)
    {
        BOOST_LOG_TRIVIAL(debug) << "ElegooCloud: RTM <- " << mask(publisher) << ": " << body.substr(0, 300);
        if (!publisher.empty() && publisher != m_printer_user)
            return;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object())
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (j.value("method", 0) == M_DOWNLOAD_PROGRESS)
            m_download = j.value("result", json::object());
        else if (j.contains("id") && j["id"].is_number_integer())
            m_responses[j["id"].get<int>()] = j;
        m_cond.notify_all();
    }

    std::string               m_printer_user;
    int                       m_next_id{0};
    std::mutex                m_mutex;
    std::condition_variable   m_cond;
    std::map<int, json>       m_responses;
    json                      m_download;
    // Last, so it is released first: its callbacks use the members above.
    std::unique_ptr<AgoraRtm> m_rtm;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// ElegooCloud
// ---------------------------------------------------------------------------------------------

ElegooCloud& ElegooCloud::instance()
{
    static ElegooCloud cloud;
    return cloud;
}

ElegooCloud::ElegooCloud() { load(); }

fs::path ElegooCloud::data_folder() { return fs::path(data_dir()) / "elegoo_cloud"; }

std::string ElegooCloud::user_agent()
{
#ifdef _WIN32
    const char* os = "Windows";
#elif defined(__APPLE__)
    const char* os = "macOS";
#else
    const char* os = "Linux";
#endif
    return std::string(SLIC3R_APP_KEY) + "/" + SLIC3R_VERSION + " (" + os + ")";
}

std::string ElegooCloud::api_base() const { return region() == "CN" ? API_CHINA : API_GLOBAL; }

bool ElegooCloud::signed_in() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return !m_account.access_token.empty();
}

ElegooCloudAccount ElegooCloud::account() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_account;
}

std::string ElegooCloud::account_label() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_account.nickname.empty())
        return m_account.nickname;
    if (!m_account.email.empty())
        return m_account.email;
    return m_account.user_id.empty() ? std::string() : mask(m_account.user_id);
}

void ElegooCloud::set_account(const ElegooCloudAccount& account)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_account = account;
    save();
}

void ElegooCloud::sign_out()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const std::string region = m_account.region;
    m_account                = ElegooCloudAccount();
    m_account.region         = region;
    save();
}

std::string ElegooCloud::region() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_account.region;
}

void ElegooCloud::set_region(const std::string& region)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_account.region = region == "CN" ? "CN" : "global";
    save();
}

std::string ElegooCloud::login_url(bool dark) const
{
    // The China login page is not published in elegoo-link; it is set by ElegooSlicer's private build
    // environment. Global accounts sign in on nexprint.com.
    std::string url = LOGIN_GLOBAL;
    url += "?language=en&region=" + std::string(region() == "CN" ? "CN" : "US") + "&theme=" + (dark ? "dark" : "light");
    return url;
}

bool ElegooCloud::accept_login_report(const json& params, std::string& error)
{
    ElegooCloudAccount a;
    a.region           = region();
    a.user_id          = json_str(find_key(params, {"userId", "accountId"}));
    a.access_token     = json_str(find_key(params, {"accessToken", "token"}));
    a.refresh_token    = json_str(find_key(params, {"refreshToken"}));
    a.access_expire_ms = json_int(find_key(params, {"accessTokenExpireTime"}));
    a.nickname         = json_str(find_key(params, {"nickname"}));
    a.email            = json_str(find_key(params, {"email"}));
    a.avatar           = json_str(find_key(params, {"avatar"}));
    if (a.access_token.empty() || a.user_id.empty()) {
        error = _u8L("The Elegoo sign-in page did not return an account.");
        return false;
    }
    set_account(a);
    BOOST_LOG_TRIVIAL(info) << "ElegooCloud: signed in as " << mask(a.user_id);
    return true;
}

#ifdef _WIN32
bool ElegooCloud::import_from_elegooslicer(std::string& error)
{
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr) {
        error = "APPDATA is not set.";
        return false;
    }
    const fs::path dir = fs::path(appdata) / "ElegooSlicer" / "user";
    try {
        std::string text;
        if (fs::exists(dir / "user_info"))
            text = dpapi(read_file(dir / "user_info"), false);
        else if (fs::exists(dir / "user_info.json"))
            text = read_file(dir / "user_info.json");
        else {
            error = _u8L("No ElegooSlicer sign-in was found on this computer.");
            return false;
        }
        const json data = json::parse(text, nullptr, false);
        ElegooCloudAccount a;
        a.access_token     = json_str(find_key(data, {"token", "accessToken", "access_token"}));
        a.refresh_token    = json_str(find_key(data, {"refreshToken", "refresh_token"}));
        a.user_id          = json_str(find_key(data, {"userId", "user_id", "accountId"}));
        a.access_expire_ms = json_int(find_key(data, {"accessTokenExpireTime", "access_token_expire_time"}));
        a.nickname         = json_str(find_key(data, {"nickname"}));
        a.email            = json_str(find_key(data, {"email"}));
        std::string reg    = json_str(find_key(data, {"region"}));
        std::transform(reg.begin(), reg.end(), reg.begin(), ::toupper);
        a.region = (reg == "CN" || reg == "CHINA") ? "CN" : "global";
        if (a.access_token.empty()) {
            error = _u8L("ElegooSlicer is not signed in to an Elegoo account.");
            return false;
        }
        set_account(a);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
#endif

void ElegooCloud::load()
{
    const fs::path p = data_folder() / "account.dat";
    try {
        if (!fs::exists(p))
            return;
        std::string text = read_file(p);
#ifdef _WIN32
        text = dpapi(text, false);
#endif
        const json j = json::parse(text, nullptr, false);
        if (!j.is_object())
            return;
        m_account.region           = j.value("region", "global");
        m_account.access_token     = j.value("access_token", "");
        m_account.refresh_token    = j.value("refresh_token", "");
        m_account.access_expire_ms = j.value("access_expire_ms", int64_t(0));
        m_account.user_id          = j.value("user_id", "");
        m_account.nickname         = j.value("nickname", "");
        m_account.email            = j.value("email", "");
        m_account.avatar           = j.value("avatar", "");
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "ElegooCloud: could not read the saved sign-in: " << e.what();
    }
}

void ElegooCloud::save() const
{
    const json j = {{"region", m_account.region},           {"access_token", m_account.access_token},
                    {"refresh_token", m_account.refresh_token}, {"access_expire_ms", m_account.access_expire_ms},
                    {"user_id", m_account.user_id},         {"nickname", m_account.nickname},
                    {"email", m_account.email},             {"avatar", m_account.avatar}};
    try {
        std::string text = j.dump();
#ifdef _WIN32
        text = dpapi(text, true);
#endif
        write_file(data_folder() / "account.dat", text);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ElegooCloud: could not save the sign-in: " << e.what();
    }
}

json ElegooCloud::request(const std::string& method, const std::string& path, const json* body, bool auth)
{
    const std::string url = api_base() + path;
    Http http = method == "POST" ? Http::post(url) : Http::get(url);
    http.header("User-Agent", user_agent()).header("Accept", "application/json").timeout_connect(15).timeout_max(30);
    if (auth)
        http.header("Authorization", "Bearer " + account().access_token);
    if (body != nullptr) {
        http.header("Content-Type", "application/json");
        http.set_post_body(body->dump());
    }
    std::string reply;
    std::string error;
    unsigned    status = 0;
    http.on_complete([&](std::string b, unsigned s) {
            reply  = std::move(b);
            status = s;
        })
        .on_error([&](std::string b, std::string err, unsigned s) {
            error  = err;
            status = s;
            reply  = std::move(b);
        })
        .perform_sync();
    const std::string short_path = path.substr(0, path.find('?'));
    if (status == 401 || status == 403)
        throw ElegooCloudError(_u8L("Elegoo rejected the sign-in. Sign in to your Elegoo account again."));
    if (!error.empty())
        throw ElegooCloudError("Elegoo API " + method + " " + short_path + " failed: " + error + " " + reply.substr(0, 200));
    const json j = json::parse(reply, nullptr, false);
    if (!j.is_object())
        throw ElegooCloudError("Unexpected reply from the Elegoo API for " + short_path);
    const int code = j.value("code", -1);
    if (code == 401 || code == 403)
        throw ElegooCloudError(_u8L("Elegoo rejected the sign-in. Sign in to your Elegoo account again."));
    if (code != 0)
        throw ElegooCloudError("Elegoo API " + short_path + ": code " + std::to_string(code) + " " + j.value("message", ""));
    return j;
}

json ElegooCloud::call(const std::string& method, const std::string& path, const json* body)
{
    ensure_token();
    try {
        return request(method, path, body, true);
    } catch (const ElegooCloudError&) {
        // One retry with a refreshed token: the access token may have been revoked early.
        if (account().refresh_token.empty())
            throw;
        refresh();
        return request(method, path, body, true);
    }
}

void ElegooCloud::ensure_token()
{
    const ElegooCloudAccount a = account();
    if (a.access_token.empty())
        throw ElegooCloudError(_u8L("Not signed in to an Elegoo account."));
    int64_t exp = a.access_expire_ms;
    if (exp < 1000000000LL) // not an absolute time: rely on a 401 instead
        exp = 0;
    else if (exp < 10000000000LL) // seconds, not ms
        exp *= 1000;
    if (exp != 0 && exp - 60000 < now_ms() && !a.refresh_token.empty())
        refresh();
}

void ElegooCloud::refresh()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    BOOST_LOG_TRIVIAL(info) << "ElegooCloud: refreshing the access token";
    const json body = {{"refreshToken", m_account.refresh_token}, {"clientId", "Slicer"}};
    const json j    = request("POST", "/api/v1/account-center-server/account-auth/token/refresh", &body, false);
    const json d    = j.value("data", json::object());
    if (d.value("accessToken", "").empty())
        throw ElegooCloudError(_u8L("Elegoo did not renew the sign-in. Sign in to your Elegoo account again."));
    m_account.access_token     = d.value("accessToken", "");
    m_account.refresh_token    = d.value("refreshToken", m_account.refresh_token);
    m_account.access_expire_ms = json_int(d.contains("expiresTime") ? &d["expiresTime"] : nullptr);
    if (const json* id = find_key(d, {"accountId"}))
        m_account.user_id = json_str(id);
    save();
}

std::vector<ElegooCloudPrinter> ElegooCloud::printers()
{
    const json                      j = call("GET", "/api/v1/device-management-server/device/list");
    std::vector<ElegooCloudPrinter> out;
    for (const json& it : j.value("data", json::array())) {
        ElegooCloudPrinter p;
        p.serial = it.value("serialNo", "");
        p.model  = it.value("pcode", "");
        p.name   = it.value("deviceName", "");
        if (p.name.empty())
            p.name = p.model.empty() ? p.serial : p.model;
        if (!p.serial.empty())
            out.push_back(std::move(p));
    }
    return out;
}

int ElegooCloud::printer_state(const std::string& serial)
{
    try {
        const json j = call("GET", "/api/v1/device-management-server/device/report-data/list?deviceCode=" + Http::url_encode(serial));
        const json data = j.value("data", json::object());
        if (!data.contains("machine_status") || !data["machine_status"].is_array())
            return -1;
        for (const json& item : data["machine_status"])
            if (item.value("reportLinkKey", "") == "status") {
                const json& v = item["reportValue"];
                return static_cast<int>(json_int(&v));
            }
    } catch (const ElegooCloudError& e) {
        BOOST_LOG_TRIVIAL(info) << "ElegooCloud: status of " << mask(serial) << " unavailable: " << e.what();
    }
    return -1;
}

json ElegooCloud::file_detail(const std::string& serial, const std::string& filename)
{
    try {
        const json j = call("GET", "/api/v1/device-management-server/local-file/filename?serialNo=" + Http::url_encode(serial) +
                                       "&filename=" + Http::url_encode(filename));
        return j.value("data", json());
    } catch (const ElegooCloudError&) {
        return json();
    }
}

static json agora_credential(ElegooCloud& cloud, const std::function<json(const std::string&)>& get)
{
    (void) cloud;
    const json j = get("/api/v1/device-management-server/device/list/agora-token?source=slicer");
    const json t = j.value("data", json::object()).value("agoraToken", json::object());
    json cred = {{"user_id", json_str(t.contains("userId") ? &t["userId"] : nullptr)},
                 {"rtm_user_id", json_str(t.contains("rtmUserId") ? &t["rtmUserId"] : nullptr)},
                 {"rtm_token", t.value("rtmToken", "")}};
    if (cred["rtm_token"].get<std::string>().empty() || cred["rtm_user_id"].get<std::string>().empty())
        throw ElegooCloudError("Elegoo did not return a messaging token for this account.");
    return cred;
}

ElegooCanvasInfo ElegooCloud::canvas_info(const std::string& serial)
{
    std::lock_guard<std::mutex> rtm_lock(m_rtm_mutex);
    const json      cred = agora_credential(*this, [this](const std::string& p) { return call("GET", p); });
    PrinterChannel  ch(cred, serial, nullptr);
    const json      reply = ch.request(M_CANVAS_STATUS, json::object(), 10);
    const json      ci    = reply.value("result", json::object()).value("canvas_info", json::object());
    ElegooCanvasInfo info;
    info.auto_refill = ci.value("auto_refill", false);
    for (const json& canvas : ci.value("canvas_list", json::array())) {
        const int id = canvas.value("canvas_id", 0);
        if (canvas.value("connected", 0) != 0)
            info.connected = true;
        for (const json& tray : canvas.value("tray_list", json::array())) {
            ElegooCanvasTray t;
            t.canvas_id     = id;
            t.tray_id       = tray.value("tray_id", 0);
            t.filament_type = tray.value("filament_type", "");
            t.filament_name = tray.value("filament_name", "");
            t.brand         = tray.value("brand", "");
            t.color         = tray.value("filament_color", "");
            t.status        = tray.value("status", 0);
            if (!t.color.empty() && t.color[0] != '#')
                t.color = "#" + t.color;
            info.trays.push_back(std::move(t));
        }
    }
    return info;
}

std::string ElegooCloud::upload_to_storage(const std::string& server_name, const fs::path& file, const std::string& md5_base64,
                                           const ElegooProgressFn& progress)
{
    const json j = call("GET", "/api/v1/device-management-server/oss/biz-entrypoint?filename=" + Http::url_encode(server_name) +
                                   "&bucketAlias=iot-private&module=gcode&fileMd5=" + Http::url_encode(md5_base64));
    const json        d      = j.value("data", json::object());
    const std::string entry  = d.value("entrypoint", "");
    const std::string access = d.value("accessUrl", "");
    if (entry.empty() || access.empty())
        throw ElegooCloudError("Elegoo did not provide an upload slot.");
    const uintmax_t size = fs::file_size(file);
    if (size >= 500ull * 1024 * 1024)
        throw ElegooCloudError(_u8L("Files of 500 MB or more cannot be sent through the Elegoo cloud yet."));

    std::string error;
    unsigned    status    = 0;
    bool        cancelled = false;
    Http::put(entry)
        .header("Content-Type", "application/octet-stream")
        .header("Content-MD5", md5_base64)
        // Cloudflare in front of the bucket rejects requests without a client identity.
        .header("User-Agent", user_agent())
        .timeout_max(0)
        .set_put_body(file)
        .on_progress([&](Http::Progress p, bool& cancel) {
            if (progress && p.ultotal > 0 && progress(static_cast<int>(p.ulnow * 50 / p.ultotal), std::string()))
                cancel = cancelled = true;
        })
        .on_complete([&](std::string, unsigned s) { status = s; })
        .on_error([&](std::string body, std::string err, unsigned s) {
            status = s;
            error  = err + " " + body.substr(0, 200);
        })
        .perform_sync();
    if (cancelled)
        throw ElegooCloudError(_u8L("Cancelled"));
    if (!error.empty() || status < 200 || status >= 300)
        throw ElegooCloudError("Cloud storage upload failed: HTTP " + std::to_string(status) + " " + error);
    return access;
}

void ElegooCloud::send(const fs::path& file, const std::string& display_name, const std::string& serial, const ElegooPrintOptions& opt,
                       const ElegooProgressFn& progress_in)
{
    auto report = [&](int pct, const std::string& status) { return progress_in ? progress_in(pct, status) : false; };
    auto check  = [&](int pct, const std::string& status) {
        if (report(pct, status))
            throw ElegooCloudError(_u8L("Cancelled"));
    };
    if (!fs::exists(file))
        throw ElegooCloudError("File not found: " + file.string());
    if (serial.empty())
        throw ElegooCloudError(_u8L("Choose an Elegoo printer first."));
    std::string name = fs::path(display_name.empty() ? file.filename().string() : display_name).filename().string();
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto ends = [&](const char* s) { return lower.size() >= strlen(s) && lower.compare(lower.size() - strlen(s), strlen(s), s) == 0; };
        if (!ends(".gcode") && !ends(".3mf"))
            name += ".gcode";
    }

    check(0, _u8L("Checking sign-in…"));
    ensure_token();
    if (opt.start_print && printer_state(serial) == 2)
        throw ElegooCloudError(_u8L("The printer reports it is printing. Wait for it to finish, or send without printing."));

    const std::string md5_raw = file_md5_raw(file);
    const std::string hex_md5 = to_hex(md5_raw);
    const std::string uid     = account().user_id;
    std::string       ext     = fs::path(name).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    const std::string server_name = (uid.size() > 6 ? uid.substr(uid.size() - 6) : uid) + "_" +
                                    (serial.size() > 6 ? serial.substr(serial.size() - 6) : serial) + "_" + hex_md5 + ext;
    const json before = file_detail(serial, name);

    check(0, _u8L("Uploading to the Elegoo cloud…"));
    const std::string url = upload_to_storage(server_name, file, to_base64(md5_raw), progress_in);
    BOOST_LOG_TRIVIAL(info) << "ElegooCloud: uploaded " << name << " (" << fs::file_size(file) << " bytes)";

    std::lock_guard<std::mutex> rtm_lock(m_rtm_mutex);
    const json     cred = agora_credential(*this, [this](const std::string& p) { return call("GET", p); });
    PrinterChannel ch(cred, serial, progress_in);
    const std::string task_id = serial;
    try {
        ch.request(M_CANCEL_DOWNLOAD, {{"taskID", task_id}}, 5);
    } catch (const ElegooCloudError&) {} // nothing to cancel
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ch.reset_download();
    check(50, _u8L("The printer is downloading the file…"));
    ch.request(M_DOWNLOAD_FILE, {{"filename", name}, {"url", url}, {"md5", hex_md5}, {"taskID", task_id}}, 15);

    auto last_change = std::chrono::steady_clock::now();
    int  last_pct    = -1;
    for (bool done = false; !done;) {
        if (report(-1, std::string())) {
            ch.request(M_CANCEL_DOWNLOAD, {{"taskID", task_id}}, 5, false);
            throw ElegooCloudError(_u8L("Cancelled"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const json d = ch.download_state();
        if (d.is_object()) {
            const int pct = d.value("progress", 0);
            if (pct != last_pct) {
                last_pct    = pct;
                last_change = std::chrono::steady_clock::now();
                report(50 + pct / 2, std::string());
            }
            switch (d.value("status", 0)) {
            case 1: done = true; break;
            case 2: throw ElegooCloudError(_u8L("The printer cancelled the download."));
            case 3: throw ElegooCloudError(_u8L("The printer failed to download the file."));
            default: break;
            }
        }
        if (!done && std::chrono::steady_clock::now() - last_change > std::chrono::seconds(60)) {
            // No progress for a minute: check whether the file landed anyway.
            const json detail = file_detail(serial, name);
            if (detail.is_object() && (!before.is_object() || detail.value("createTime", json()) != before.value("createTime", json())))
                done = true;
            else
                throw ElegooCloudError(_u8L("The printer reported no download progress for 60 seconds."));
        }
    }
    report(100, std::string());
    if (!opt.start_print)
        return;

    if (opt.set_auto_refill) {
        try {
            ch.request(M_SET_AUTO_REFILL, {{"auto_refill", opt.auto_refill}}, 10);
        } catch (const ElegooCloudError& e) {
            BOOST_LOG_TRIVIAL(warning) << "ElegooCloud: auto refill not set: " << e.what();
        }
    }

    report(100, _u8L("Starting the print…"));
    json slot_map = json::array();
    for (const ElegooSlotMapEntry& s : opt.slot_map)
        slot_map.push_back({{"t", s.t}, {"canvas_id", s.canvas_id}, {"tray_id", s.tray_id}});
    json params = {{"storage_media", "local"},
                   {"filename", name},
                   {"config",
                    {{"delay_video", opt.timelapse},
                     {"printer_check", opt.bed_leveling},
                     {"print_layout", opt.plate_side == 'B' ? "B" : "A"},
                     {"bedlevel_force", false},
                     {"slot_map", slot_map}}}};
    try {
        ch.request(M_START_PRINT, params, 10);
    } catch (const ElegooCloudError& e) {
        // 1026: no bed mesh saved yet; force leveling when the user asked for it.
        if (std::string(e.what()).find("error 1026") == std::string::npos || !opt.bed_leveling)
            throw;
        params["config"]["bedlevel_force"] = true;
        ch.request(M_START_PRINT, params, 10);
    }
}

// ---------------------------------------------------------------------------------------------
// ElegooCloudHost
// ---------------------------------------------------------------------------------------------

bool ElegooCloudHost::test(wxString& msg) const
{
    try {
        ElegooCloud::instance().printers();
        return true;
    } catch (const std::exception& e) {
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

wxString ElegooCloudHost::get_test_ok_msg() const { return _L("Connected to the Elegoo cloud."); }

wxString ElegooCloudHost::get_test_failed_msg(wxString& msg) const
{
    return wxString::Format("%s: %s", _L("Could not connect to the Elegoo cloud"), msg);
}

void ElegooCloudHost::to_extended_info(const std::string& serial, const ElegooPrintOptions& o, std::map<std::string, std::string>& info)
{
    json slot_map = json::array();
    for (const ElegooSlotMapEntry& s : o.slot_map)
        slot_map.push_back({{"t", s.t}, {"canvas_id", s.canvas_id}, {"tray_id", s.tray_id}});
    info["elegoo_serial"]          = serial;
    info["elegoo_bed_leveling"]    = o.bed_leveling ? "1" : "0";
    info["elegoo_timelapse"]       = o.timelapse ? "1" : "0";
    info["elegoo_auto_refill"]     = o.auto_refill ? "1" : "0";
    info["elegoo_set_auto_refill"] = o.set_auto_refill ? "1" : "0";
    info["elegoo_plate_side"]      = std::string(1, o.plate_side == 'B' ? 'B' : 'A');
    info["elegoo_slot_map"]        = slot_map.dump();
}

ElegooPrintOptions ElegooCloudHost::from_extended_info(const PrintHostUpload& u, std::string& serial)
{
    ElegooPrintOptions o;
    serial            = u.extended("elegoo_serial");
    o.start_print     = u.post_action == PrintHostPostUploadAction::StartPrint;
    o.bed_leveling    = u.extended("elegoo_bed_leveling", "1") == "1";
    o.timelapse       = u.extended("elegoo_timelapse") == "1";
    o.auto_refill     = u.extended("elegoo_auto_refill") == "1";
    o.set_auto_refill = u.extended("elegoo_set_auto_refill") == "1";
    o.plate_side      = u.extended("elegoo_plate_side", "A") == "B" ? 'B' : 'A';
    const json slots  = json::parse(u.extended("elegoo_slot_map", "[]"), nullptr, false);
    if (slots.is_array())
        for (const json& s : slots)
            o.slot_map.push_back({s.value("t", 0), s.value("canvas_id", 0), s.value("tray_id", 0)});
    return o;
}

bool ElegooCloudHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    std::string              serial;
    const ElegooPrintOptions opt = from_extended_info(upload_data, serial);
    const std::string        dummy;
    try {
        ElegooCloud::instance().send(upload_data.source_path, upload_data.upload_path.filename().string(), serial, opt,
                                     [&](int pct, const std::string& status) {
                                         if (!status.empty() && info_fn)
                                             info_fn(L"set_status", wxString::FromUTF8(status));
                                         bool cancel = false;
                                         if (pct >= 0 && progress_fn)
                                             progress_fn(Http::Progress(0, 0, 100, static_cast<size_t>(pct), dummy), cancel);
                                         else if (progress_fn) // poll for cancel without moving the bar
                                             progress_fn(Http::Progress(0, 0, 0, 0, dummy), cancel);
                                         return cancel;
                                     });
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ElegooCloud: send failed: " << e.what();
        if (error_fn)
            error_fn(wxString::FromUTF8(e.what()));
        return false;
    }
}

} // namespace Slic3r
