#include "KobraXSession.hpp"

#include "KobraX.hpp"
#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/system_error.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace fs  = boost::filesystem;
using tcp     = boost::asio::ip::tcp;
using json    = nlohmann::json;

namespace Slic3r { namespace KobraX {

namespace {

constexpr const char* CERT_FILE    = "anycubic_slicer.pem";
constexpr const char* KEY_FILE     = "anycubic_slicer_key.pem";
constexpr const char* EXPECTED_CN  = "AnycubicSlicer";
constexpr uint64_t    MAX_SCAN     = 16 * 1024 * 1024; // the slicer library is about 3 MB
constexpr size_t      MAX_ANSWER   = 1024 * 1024;       // /info, /ctrl and upload answers are a few hundred bytes
constexpr uint16_t    KEEPALIVE_S  = 60;
const auto            LOGIN_TIMEOUT = std::chrono::seconds(15);
const auto            PING_EVERY    = std::chrono::seconds(30);
const auto            SILENT_LIMIT  = std::chrono::seconds(KEEPALIVE_S * 3 / 2);

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string random_text(size_t size)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device                 device;
    std::uniform_int_distribution<int> pick(0, int(sizeof(alphabet)) - 2);
    std::string                        out;
    for (size_t i = 0; i < size; ++i)
        out += alphabet[pick(device)];
    return out;
}

template<typename T, void (*Free)(T*)> using Owned = std::unique_ptr<T, decltype(Free)>;

// Every "-----BEGIN <label>-----" ... "-----END <label>-----" block in `text`
std::vector<std::pair<std::string, std::string>> pem_blocks(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> out;
    const std::string begin = "-----BEGIN ";
    for (size_t pos = text.find(begin); pos != std::string::npos; pos = text.find(begin, pos + 1)) {
        const size_t label_end = text.find("-----", pos + begin.size());
        if (label_end == std::string::npos || label_end - pos > 64)
            continue;
        const std::string label = text.substr(pos + begin.size(), label_end - pos - begin.size());
        const std::string end   = "-----END " + label + "-----";
        const size_t      stop  = text.find(end, label_end);
        if (stop == std::string::npos || stop - pos > 16 * 1024)
            continue;
        out.emplace_back(label, text.substr(pos, stop + end.size() - pos));
    }
    return out;
}

bool identity_from_text(const std::string& text, ClientIdentity& identity)
{
    std::vector<std::pair<std::string, X509*>> certs;
    std::vector<std::pair<std::string, EVP_PKEY*>> keys;
    for (const auto& [label, block] : pem_blocks(text)) {
        Owned<BIO, BIO_free_all> bio(BIO_new_mem_buf(block.data(), int(block.size())), BIO_free_all);
        if (label == "CERTIFICATE") {
            X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
            char  cn[256] = {};
            if (cert && X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn)) > 0 &&
                std::string(cn) == EXPECTED_CN)
                certs.emplace_back(block, cert);
            else
                X509_free(cert);
        } else if (label.find("PRIVATE KEY") != std::string::npos) {
            if (EVP_PKEY* key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr))
                keys.emplace_back(block, key);
        }
    }
    bool found = false;
    for (const auto& [cert_pem, cert] : certs)
        for (const auto& [key_pem, key] : keys)
            if (!found && X509_check_private_key(cert, key) == 1) {
                identity = {cert_pem, key_pem};
                found    = true;
            }
    for (auto& cert : certs)
        X509_free(cert.second);
    for (auto& key : keys)
        EVP_PKEY_free(key.second);
    return found;
}

bool read_file(const fs::path& path, std::string& data)
{
    boost::system::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > MAX_SCAN)
        return false;
    fs::ifstream file(path, std::ios::binary);
    data.assign(size_t(size), '\0');
    return file && file.read(&data[0], std::streamsize(size));
}

// Where Anycubic Slicer Next keeps the identity: the library holding it on macOS, its install folder elsewhere
std::vector<fs::path> slicer_locations()
{
    std::vector<fs::path> out;
#ifdef __APPLE__
    const std::string library = "AnycubicSlicerNext.app/Contents/Frameworks/libcloud_mqtt.dylib";
    out.push_back(fs::path("/Applications") / library);
    if (const char* home = std::getenv("HOME"))
        out.push_back(fs::path(home) / "Applications" / library);
#elif defined(_WIN32)
    for (const char* root : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"})
        if (const char* dir = std::getenv(root))
            out.push_back(fs::path(dir) / "AnycubicSlicerNext" / "cloud_mqtt.dll");
    if (const char* local = std::getenv("LOCALAPPDATA"))
        out.push_back(fs::path(local) / "Programs" / "AnycubicSlicerNext" / "cloud_mqtt.dll");
#endif
    return out;
}

// anycubic_slicer.pem and anycubic_slicer_key.pem in `folder`
bool read_identity_folder(const std::string& folder, ClientIdentity& identity)
{
    std::string cert, key;
    return !folder.empty() && read_file(fs::path(folder) / CERT_FILE, cert) && read_file(fs::path(folder) / KEY_FILE, key) &&
           identity_from_text(cert + "\n" + key, identity);
}

bool http_json(Http http, json& answer, std::string& error)
{
    bool ok = false;
    http.timeout_connect(5)
        .timeout_max(15)
        .size_limit(MAX_ANSWER)
        .on_complete([&](std::string body, unsigned) {
            answer = json::parse(body, nullptr, false);
            ok     = answer.is_object();
            if (!ok)
                error = _u8L("the printer's answer is not JSON");
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = status ? _u8L("the printer answered HTTP") + " " + std::to_string(status) : err;
        })
        .perform_sync();
    return ok;
}

} // namespace

std::string identity_folder() { return (fs::path(data_dir()) / "anycubic").string(); }

bool read_client_identity(const std::string& path, ClientIdentity& identity)
{
    std::string data;
    return read_file(path, data) && identity_from_text(data, identity);
}

bool find_client_identity(const std::string& user_folder,
                          const std::string& bundled_folder,
                          ClientIdentity&    identity,
                          std::string&       error)
{
    if (read_identity_folder(user_folder, identity))
        return true;
    std::string searched = (fs::path(user_folder) / CERT_FILE).string();
    // An installed Anycubic Slicer Next is read once per run, not on every connect
    static std::mutex                    slicer_mutex;
    static std::optional<ClientIdentity> slicer_identity;
    static bool                          slicer_scanned = false;
    {
        std::lock_guard<std::mutex> lock(slicer_mutex);
        if (!slicer_scanned) {
            for (const fs::path& path : slicer_locations()) {
                ClientIdentity found;
                if (read_client_identity(path.string(), found)) {
                    slicer_identity = found;
                    break;
                }
            }
            slicer_scanned = true;
        }
        if (slicer_identity) {
            identity = *slicer_identity;
            return true;
        }
    }
    for (const fs::path& path : slicer_locations())
        searched += ", " + path.string();
    if (read_identity_folder(bundled_folder, identity))
        return true;
    if (!bundled_folder.empty())
        searched += ", " + (fs::path(bundled_folder) / CERT_FILE).string();
    error = _u8L("No Anycubic Slicer client certificate found. Looked in:") + " " + searched;
    return false;
}

struct Session::Impl
{
    Options  options;
    ReportFn on_report;
    ClosedFn on_closed;
    Credentials creds;

    net::io_context                                                       io;
    std::optional<net::executor_work_guard<net::io_context::executor_type>> work;
    ssl::context                                                          tls{ssl::context::tls_client};
    std::unique_ptr<ssl::stream<tcp::socket>>                             stream;
    tcp::resolver                                                         resolver{io};
    net::steady_timer                                                     ping{io};
    std::thread                                                           thread;

    // io thread only
    std::string                            rx;
    std::array<char, 16 * 1024>            chunk{};
    std::deque<std::string>                tx;
    std::chrono::steady_clock::time_point  last_rx;
    bool                                   ended = false;
    bool                                   close_after_write = false;
    std::optional<std::promise<std::string>> login; // "" once subscribed, else why the login failed

    std::future<void> stopped; // ready once the io thread has returned
    std::atomic<bool> up{false};
    std::atomic<bool> closing{false};
    bool              closed = false; // close() ran: the io_context is stopped and cannot be opened again

    struct Waiter
    {
        MatchFn match;
        json    reply;
        bool    done = false;
    };
    std::mutex              wait_mutex;
    std::condition_variable wait_cv;
    std::list<Waiter*>      waiters;

    void fail(const std::string& reason)
    {
        if (ended)
            return;
        ended = true;
        up    = false;
        boost::system::error_code ec;
        if (stream)
            stream->lowest_layer().close(ec);
        ping.cancel();
        resolver.cancel();
        {
            std::lock_guard<std::mutex> lock(wait_mutex);
            wait_cv.notify_all();
        }
        if (login) {
            login->set_value(reason);
            login.reset();
        } else if (!closing && on_closed) {
            on_closed(reason);
        }
    }

    void write(std::string packet)
    {
        tx.push_back(std::move(packet));
        if (tx.size() == 1)
            write_next();
    }

    void write_next()
    {
        net::async_write(*stream, net::buffer(tx.front()), [this](boost::system::error_code ec, size_t) {
            if (ec)
                return fail(_u8L("connection lost") + " (" + ec.message() + ")");
            tx.pop_front();
            if (tx.empty() && close_after_write)
                fail("closed");
            else if (!tx.empty() && !ended)
                write_next();
        });
    }

    void schedule_ping()
    {
        ping.expires_after(PING_EVERY);
        ping.async_wait([this](boost::system::error_code ec) {
            if (ec || ended)
                return;
            if (std::chrono::steady_clock::now() - last_rx > SILENT_LIMIT)
                return fail(_u8L("the printer stopped answering"));
            write(Mqtt::pingreq());
            schedule_ping();
        });
    }

    void read()
    {
        stream->async_read_some(net::buffer(chunk), [this](boost::system::error_code ec, size_t size) {
            if (ec)
                return fail((login ? _u8L("the printer closed the connection during the TLS/MQTT login") : _u8L("connection lost")) +
                            " (" + ec.message() + ")");
            last_rx = std::chrono::steady_clock::now();
            rx.append(chunk.data(), size);
            std::vector<Mqtt::Packet> packets;
            if (!Mqtt::split(rx, packets))
                return fail(_u8L("MQTT protocol error"));
            for (const Mqtt::Packet& packet : packets)
                if (!ended)
                    handle(packet);
            if (!ended)
                read();
        });
    }

    void handle(const Mqtt::Packet& packet)
    {
        if (packet.type == Mqtt::CONNACK) {
            const int code = packet.body.size() >= 2 ? uint8_t(packet.body[1]) : -1;
            if (code == 4 || code == 5)
                return fail(_u8L("the printer refused the MQTT login") + " (" + std::to_string(code) + ")");
            if (code != 0)
                return fail(_u8L("the printer refused the MQTT connection") + " (" + std::to_string(code) + ")");
            write(Mqtt::subscribe(1, subscribe_topic(creds)));
        } else if (packet.type == Mqtt::SUBACK) {
            if (packet.body.size() < 3 || (uint8_t(packet.body[2]) & 0x80))
                return fail(_u8L("the printer refused the subscription"));
            up = true;
            if (login) {
                login->set_value("");
                login.reset();
            }
            schedule_ping();
        } else if (packet.type == Mqtt::PUBLISH) {
            std::string topic, payload;
            if (!Mqtt::parse_publish(packet, topic, payload))
                return;
            // .../<deviceId>/response only acknowledges that a request was received
            if (topic.size() >= 9 && topic.compare(topic.size() - 9, 9, "/response") == 0)
                return;
            const json message = json::parse(payload, nullptr, false);
            if (!message.is_object())
                return;
            const std::string suffix = report_suffix(topic);
            {
                std::lock_guard<std::mutex> lock(wait_mutex);
                for (Waiter* waiter : waiters)
                    if (!waiter->done && waiter->match(suffix, message)) {
                        waiter->reply = message;
                        waiter->done  = true;
                    }
                wait_cv.notify_all();
            }
            if (on_report)
                on_report(suffix, message);
        }
    }

    void start(const std::string& client_id)
    {
        resolver.async_resolve(options.host, std::to_string(options.mqtt_port),
                               [this, client_id](boost::system::error_code ec, tcp::resolver::results_type results) {
            if (ec)
                return fail(_u8L("cannot resolve the printer's address") + " " + options.host + " (" + ec.message() + ")");
            net::async_connect(stream->lowest_layer(), results, [this, client_id](boost::system::error_code ec, const tcp::endpoint&) {
                if (ec)
                    return fail(_u8L("no answer on the printer's MQTT port") + " " + std::to_string(options.mqtt_port) + " (" +
                                ec.message() + ")");
                stream->async_handshake(ssl::stream_base::client, [this, client_id](boost::system::error_code ec) {
                    if (ec)
                        return fail(_u8L("the TLS handshake with the printer failed, most likely because of the client certificate") +
                                    " (" + ec.message() + ")");
                    last_rx = std::chrono::steady_clock::now();
                    write(Mqtt::connect(client_id, creds.username, creds.password, KEEPALIVE_S));
                    read();
                });
            });
        });
    }

    // Registers a waiter before `start` runs, so a fast answer is not missed
    bool await(const MatchFn& match, std::chrono::milliseconds timeout, json& reply, const std::function<bool()>& start)
    {
        Waiter waiter{match, json(), false};
        {
            std::lock_guard<std::mutex> lock(wait_mutex);
            waiters.push_back(&waiter);
        }
        bool done = false;
        if (start()) {
            std::unique_lock<std::mutex> lock(wait_mutex);
            wait_cv.wait_for(lock, timeout, [&] { return waiter.done || !up; });
            done = waiter.done;
        }
        std::lock_guard<std::mutex> lock(wait_mutex);
        waiters.remove(&waiter);
        if (done)
            reply = waiter.reply;
        return done;
    }

    bool fetch_credentials(std::string& error)
    {
        const std::string base = "http://" + url_host(options.host) + ":" + std::to_string(options.http_port);
        json              info;
        if (!http_json(Http::get(base + "/info"), info, error)) {
            error = _u8L("the printer did not answer on its HTTP port") + " " + std::to_string(options.http_port) + ": " + error;
            return false;
        }
        const std::string token = info.contains("token") && info["token"].is_string() ? info["token"].get<std::string>() : "";
        if (token.size() < 32) {
            error = _u8L("the printer gave no LAN token. Is LAN mode enabled on the printer?");
            return false;
        }
        const std::string ts = std::to_string(now_ms()), nonce = random_text(6);
        json              ctrl;
        Http              post = Http::post(base + "/ctrl?ts=" + ts + "&nonce=" + nonce + "&sign=" + ctrl_sign(token, ts, nonce) + "&did=random");
        post.set_post_body(std::string());
        return http_json(std::move(post), ctrl, error) && parse_ctrl_answer(token, ctrl.dump(), options.host, creds, error);
    }
};

Session::Session(Options options, ReportFn on_report, ClosedFn on_closed) : p(std::make_unique<Impl>())
{
    p->options   = std::move(options);
    p->on_report = std::move(on_report);
    p->on_closed = std::move(on_closed);
}

Session::~Session() { close(); }

bool Session::open(std::string& error)
{
    if (p->thread.joinable() || p->closed) {
        error = "the session was already opened";
        return false;
    }
    if (!p->fetch_credentials(error))
        return false;

    try {
        SSL_CTX* ctx = p->tls.native_handle();
        // The printer presents a self-signed certificate; the client identity is what its broker checks.
        // Security level 0 is what Anycubic Slicer Next and Kobra Control connect with. The client certificate
        // (SHA-1 signed) is sent at higher levels too; the printer's own key, signature algorithms and ciphers
        // have not been recorded, so a higher level is not known to complete the handshake. This context
        // serves this one printer connection only.
        p->tls.set_verify_mode(ssl::verify_none);
        SSL_CTX_set_security_level(ctx, 0);
        SSL_CTX_set_cipher_list(ctx, "DEFAULT:@SECLEVEL=0");
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        p->tls.use_certificate(net::buffer(p->options.identity.cert_pem), ssl::context::pem);
        p->tls.use_private_key(net::buffer(p->options.identity.key_pem), ssl::context::pem);
    } catch (const boost::system::system_error& e) {
        error = _u8L("the Anycubic Slicer client certificate cannot be used") + " (" + e.what() + ")";
        return false;
    }

    p->stream = std::make_unique<ssl::stream<tcp::socket>>(p->io, p->tls);
    p->login.emplace();
    std::future<std::string> login = p->login->get_future();
    p->work.emplace(net::make_work_guard(p->io));
    const std::string client_id = "orca_" + random_text(8);
    net::post(p->io, [this, client_id] { p->start(client_id); });
    std::promise<void> stopped;
    p->stopped = stopped.get_future();
    p->thread  = std::thread([this, stopped = std::move(stopped)]() mutable {
        p->io.run();
        stopped.set_value();
    });

    if (login.wait_for(LOGIN_TIMEOUT) != std::future_status::ready)
        net::post(p->io, [this] { p->fail(_u8L("the printer did not finish the MQTT login in time")); });
    error = login.get();
    if (!error.empty()) {
        close();
        return false;
    }
    return true;
}

void Session::close()
{
    // Once: the destructor and a failed open() both call it
    if (p->closed || !p->thread.joinable())
        return;
    p->closed  = true;
    p->closing = true;
    net::post(p->io, [this] {
        if (p->ended)
            return;
        if (!p->up)
            return p->fail("closed");
        // A polite DISCONNECT; the socket goes once it is written
        p->close_after_write = true;
        p->write(Mqtt::disconnect());
    });
    p->work.reset();
    // A write that never completes must not hang the caller
    if (p->stopped.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
        p->io.stop();
    p->thread.join();
}

bool Session::is_open() const { return p->up; }

const Credentials& Session::credentials() const { return p->creds; }

bool Session::send(const std::string& type, const std::string& action, const json& data, const std::string& kind)
{
    if (!p->up)
        return false;
    const std::string msgid  = boost::uuids::to_string(boost::uuids::random_generator()());
    std::string       packet = Mqtt::publish(publish_topic(p->creds, type, kind), KobraX::request(type, action, data, msgid, now_ms()).dump());
    net::post(p->io, [this, packet = std::move(packet)]() mutable {
        if (p->up)
            p->write(std::move(packet));
    });
    return true;
}

bool Session::request(const std::string&        type,
                      const std::string&        action,
                      const json&               data,
                      const MatchFn&            match,
                      std::chrono::milliseconds timeout,
                      json&                     reply)
{
    return p->await(match, timeout, reply, [&] { return send(type, action, data); });
}

bool Session::wait(const MatchFn& match, std::chrono::milliseconds timeout, json& reply)
{
    return p->await(match, timeout, reply, [] { return true; });
}

bool Session::upload(const std::string& path,
                     const std::string& name,
                     const std::string& upload_url,
                     const ProgressFn&  progress,
                     const CancelFn&    cancelled,
                     std::string&       error)
{
    const size_t at = upload_url.find("?s=");
    if (at == std::string::npos) {
        error = _u8L("the printer has not announced its upload address yet");
        return false;
    }
    // The token goes into the upload URL as it is: only characters that need no escaping
    const std::string token = upload_url.substr(at + 3);
    if (token.empty() ||
        token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        error = _u8L("the printer announced an upload address this version cannot use");
        return false;
    }
    if (name.empty() || name.find_first_of("\"\r\n/\\") != std::string::npos) {
        error = _u8L("unsupported file name for the printer");
        return false;
    }
    boost::system::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        error = _u8L("cannot read the file to upload") + " " + path;
        return false;
    }
    // Only the token is taken from the announced address; the file goes to the printer we are connected to.
    // The headers are those Kobra Control sends, which the printer accepted on firmware 2.0.2.8.
    Http http = Http::post("http://" + url_host(p->options.host) + ":" + std::to_string(p->options.http_port) + "/gcode_upload?s=" +
                           token);
    bool ok = false;
    http.header("X-File-Length", std::to_string(size))
        .header("X-BBL-Client-Type", "slicer")
        .header("Accept", "*/*")
        .form_add("filename", name)
        .form_add_file("gcode", path, name)
        .timeout_connect(5)
        .size_limit(MAX_ANSWER)
        .on_progress([&](Http::Progress pr, bool& cancel) {
            cancel = cancelled && cancelled();
            if (progress && pr.ultotal > 0)
                progress(int(pr.ulnow * 100 / pr.ultotal));
        })
        .on_complete([&](std::string body, unsigned) {
            // {"code": 200, "data": {"gcode": "<name>"}, "message": "success"}
            const json answer = json::parse(body, nullptr, false);
            ok = answer.is_object() && answer.value("code", json()) == 200;
            if (!ok)
                error = _u8L("the printer refused the upload") + ": " + body.substr(0, 200);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = status ? _u8L("the printer refused the upload") + ": HTTP " + std::to_string(status) : _u8L("the upload failed") + ": " + err;
        })
        .perform_sync();
    if (!ok && cancelled && cancelled())
        error = _u8L("upload cancelled");
    return ok;
}

}} // namespace Slic3r::KobraX
