#include "SimplyPrint.hpp"

#include <openssl/sha.h>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>

#include "nlohmann/json.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"


namespace Slic3r {

// to make testing easier
//#define SIMPLYPRINT_TEST

#ifdef SIMPLYPRINT_TEST
#define URL_BASE_HOME "https://test.simplyprint.io"
#define URL_BASE_API "https://testapi.simplyprint.io"
#else
#define URL_BASE_HOME "https://simplyprint.io"
#define URL_BASE_API "https://api.simplyprint.io"
#endif

static constexpr boost::asio::ip::port_type CALLBACK_PORT = 21328;
static const std::string CALLBACK_URL = "http://localhost:21328/callback";
static const std::string RESPONSE_TYPE  = "code";
static const std::string CLIENT_ID = "simplyprintorcaslicer";
static const std::string CLIENT_SCOPES = "user.read files.temp_upload";
static const std::string OAUTH_CREDENTIAL_PATH = "simplyprint_oauth.json";
static const std::string TOKEN_URL = URL_BASE_API"/oauth2/Token";
#ifdef SIMPLYPRINT_TEST
static constexpr uint64_t MAX_SINGLE_UPLOAD_FILE_SIZE = 100000ull; // Max file size that can be uploaded in a single http request
#else
static constexpr uint64_t MAX_SINGLE_UPLOAD_FILE_SIZE = 100000000ull; // Max file size that can be uploaded in a single http request
#endif
static const std::string CHUNCK_RECEIVE_URL = URL_BASE_API"/0/files/ChunkReceive";

static std::string generate_verification_code(int code_length = 32)
{
    std::stringstream ss;
    for (auto i = 0; i < code_length; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (rand() % 0x100);
    }

    return ss.str();
}

static std::string sha256b64(const std::string& inputStr)
{
    unsigned char        hash[SHA256_DIGEST_LENGTH];
    const unsigned char* data = (const unsigned char*) inputStr.c_str();
    SHA256(data, inputStr.size(), hash);

    std::string b64;
    b64.resize(boost::beast::detail::base64::encoded_size(sizeof(hash)));
    b64.resize(boost::beast::detail::base64::encode(&b64[0], hash, sizeof(hash)));

    // uses '-' instead of '+' and '_' instead of '/' for url-safe
    std::replace(b64.begin(), b64.end(), '+', '-');
    std::replace(b64.begin(), b64.end(), '/', '_');

    // Stripping "=" is for RFC 7636 compliance
    b64.erase(std::remove(b64.begin(), b64.end(), '='), b64.end());

    return b64;
}

static std::string url_encode(const std::vector<std::pair<std::string, std::string>> query)
{
    std::vector<std::string> q;
    q.reserve(query.size());

    std::transform(query.begin(), query.end(), std::back_inserter(q), [](const auto& kv) {
        if (kv.second.empty()) {
            return Http::url_encode(kv.first);
        }
        return Http::url_encode(kv.first) + "=" + Http::url_encode(kv.second);
    });

    return boost::algorithm::join(q, "&");
}

static void set_auth(Http& http, const std::string& access_token) { http.header("Authorization", "Bearer " + access_token); }

static bool should_open_in_external_browser()
{
    const auto& app = wxGetApp();

    if (app.preset_bundle->use_bbl_device_tab()) {
        // When using bbl device tab, we always need to open external browser
        return true;
    }

    // Otherwise, if user choose to switch to device tab, then don't bother opening external browser
    return !app.app_config->get_bool("open_device_tab_post_upload");
}

SimplyPrint::SimplyPrint(DynamicPrintConfig* config)
{
    cred_file = (boost::filesystem::path(data_dir()) / OAUTH_CREDENTIAL_PATH).make_preferred().string();
    load_oauth_credential();
}

GUI::OAuthParams SimplyPrint::get_oauth_params() const
{
    const auto verification_code = generate_verification_code();
    // SimplyPrint uses S256 for PKCE
    const auto code_challenge = sha256b64(verification_code);
    const auto state          = generate_verification_code();

    const std::vector<std::pair<std::string, std::string>> query_parameters{
        {"client_id", CLIENT_ID},
        {"redirect_uri", CALLBACK_URL},
        {"scope", CLIENT_SCOPES},
        {"response_type", RESPONSE_TYPE},
        {"state", state},
        {"code_challenge", code_challenge},
        {"code_challenge_method", "S256"},
    };
    const auto login_url = (boost::format(URL_BASE_HOME"/panel/oauth2/authorize?%s") % url_encode(query_parameters)).str();

    return GUI::OAuthParams{
        login_url,
        CLIENT_ID,
        CALLBACK_PORT,
        CALLBACK_URL,
        CLIENT_SCOPES,
        RESPONSE_TYPE,
        URL_BASE_HOME"/login-success",
        URL_BASE_HOME"/login-success",
        TOKEN_URL,
        verification_code,
        state,
    };
}

void SimplyPrint::load_oauth_credential()
{
    cred.clear();
    if (boost::filesystem::exists(cred_file)) {
        nlohmann::json j;
        try {
            boost::nowide::ifstream ifs(cred_file);
            ifs >> j;
            ifs.close();

            cred["access_token"] = j["access_token"];
            cred["refresh_token"] = j["refresh_token"];
        } catch (std::exception& err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << cred_file << " failed, reason = " << err.what();
            cred.clear();
        }
    }
}

void SimplyPrint::save_oauth_credential(const GUI::OAuthResult& cred) const
{
    nlohmann::json j;
    j["access_token"]  = cred.access_token;
    j["refresh_token"] = cred.refresh_token;
    
    boost::nowide::ofstream c;
    c.open(cred_file, std::ios::out | std::ios::trunc);
    c << std::setw(4) << j << std::endl;
    c.close();
}

wxString SimplyPrint::get_test_ok_msg() const { return _(L("Connected to SimplyPrint successfully!")); }

wxString SimplyPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to SimplyPrint"), msg.Truncate(256));
}

void SimplyPrint::log_out() const
{
    boost::nowide::remove(cred_file.c_str());
}

bool SimplyPrint::do_api_call(std::function<Http(bool)>                               build_request,
                              std::function<bool(std::string, unsigned)>              on_complete,
                              std::function<bool(std::string, std::string, unsigned)> on_error) const
{
    if (cred.find("access_token") == cred.end()) {
        return false;
    }

    bool res = true;

    const auto create_request = [this, &build_request, &res, &on_complete](const std::string& access_token, bool is_retry) {
        auto http = build_request(is_retry);
        set_auth(http, access_token);
        http.header("User-Agent", "SimplyPrint Orca Plugin")
            .on_complete([&](std::string body, unsigned http_status) {
                res = on_complete(body, http_status);
            });

        return http;
    };

    create_request(cred.at("access_token"), false)
        .on_error([&res, &on_error, this, &create_request](std::string body, std::string error, unsigned http_status) {
            if (http_status == 401) {
                // Refresh token
                BOOST_LOG_TRIVIAL(warning) << boost::format("SimplyPrint: Access token invalid: %1%, HTTP %2%, body: `%3%`") % error %
                                                  http_status % body;
                BOOST_LOG_TRIVIAL(info) << "SimplyPrint: Attempt to refresh access token";

                auto http = Http::post(TOKEN_URL);
                http.timeout_connect(5)
                    .timeout_max(5)
                    .form_add("grant_type", "refresh_token")
                    .form_add("client_id", CLIENT_ID)
                    .form_add("refresh_token", cred.at("refresh_token"))
                    .on_complete([this, &res, &on_error, &create_request](std::string body, unsigned http_status) {
                        GUI::OAuthResult r;
                        GUI::OAuthJob::parse_token_response(body, false, r);
                        if (r.success) {
                            BOOST_LOG_TRIVIAL(info) << "SimplyPrint: Successfully refreshed access token";
                            this->save_oauth_credential(r);

                            // Run the api call again
                            create_request(r.access_token, true)
                                .on_error([&res, &on_error](std::string body, std::string error, unsigned http_status) {
                                    res = on_error(body, error, http_status);
                                })
                                .perform_sync();
                        } else {
                            BOOST_LOG_TRIVIAL(error)
                                << boost::format("SimplyPrint: Failed to refresh access token: %1%, body: `%2%`") % r.error_message % body;
                            res = on_error(body, r.error_message, http_status);
                        }
                    })
                    .on_error([&res, &on_error](std::string body, std::string error, unsigned http_status) {
                        BOOST_LOG_TRIVIAL(error)
                            << boost::format("SimplyPrint: Failed to refresh access token: %1%, HTTP %2%, body: `%3%`") % error %
                                   http_status % body;
                        res = on_error(body, error, http_status);
                    })
                    .perform_sync();
            } else {
                res = on_error(body, error, http_status);
            }
        })
        .perform_sync();

    return res;
}


bool SimplyPrint::test(wxString& curl_msg) const
{
    if (cred.find("access_token") == cred.end()) {
        return false;
    }

    return do_api_call(
        [](bool is_retry) {
            auto http = Http::get(URL_BASE_API"/oauth2/TokenInfo");
            http.header("Accept", "application/json");
            return http;
        },
        [](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: Got token info: %1%") % body;
            return true;
        },
        [](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("SimplyPrint: Error getting token info: %1%, HTTP %2%, body: `%3%`") % error %
                                            status % body;
            return false;
        });
}

bool SimplyPrint::do_temp_upload(const boost::filesystem::path& file_path,
                                 const std::string&             chunk_id,
                                 const std::string&             filename,
                                 ProgressFn                     prorgess_fn,
                                 ErrorFn                        error_fn) const
{
    if (file_path.empty() == chunk_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "SimplyPrint: Invalid arguments: both file_path and chunk_id are set or not provided";
        error_fn(_L("Internal error"));
        return false;
    }

    return do_api_call(
        [&file_path, &chunk_id, &prorgess_fn, &filename](bool is_retry) {
            auto http = Http::post(URL_BASE_HOME"/api/files/TempUpload");
            if (!file_path.empty()) {
                http.form_add_file("file", file_path, filename);
            } else {
                http.form_add("chunkId", chunk_id);
            }
            http.on_progress([&prorgess_fn](Http::Progress progress, bool& cancel) { prorgess_fn(std::move(progress), cancel); });

            return http;
        },
        [&error_fn, &filename, this](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: File uploaded: HTTP %1%: %2%") % status % body;

            // Get file UUID
            const auto j = nlohmann::json::parse(body, nullptr, false, true);
            if (j.is_discarded()) {
                BOOST_LOG_TRIVIAL(error) << "SimplyPrint: Invalid or no JSON data on token response: " << body;
                error_fn(_L("Unknown error"));
                return false;
            }

            if (j.find("uuid") == j.end()) {
                BOOST_LOG_TRIVIAL(error) << "SimplyPrint: Invalid or no JSON data on token response: " << body;
                error_fn(_L("Unknown error"));
                return false;
            }
            const std::string uuid = j["uuid"];

            // Launch external browser for file importing after uploading
            const auto url = URL_BASE_HOME"/panel?" + url_encode({{"import", "tmp:" + uuid}, {"filename", filename}});

            if (should_open_in_external_browser()) {
                wxLaunchDefaultBrowser(url);
            } else {
                const auto mainframe = GUI::wxGetApp().mainframe;
                mainframe->request_select_tab(MainFrame::TabPosition::tpMonitor);
                mainframe->load_printer_url(url);
            }

            return true;
        },
        [this, &error_fn](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("SimplyPrint: Error uploading file : %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            return false;
        });
}

bool SimplyPrint::do_chunk_upload(const boost::filesystem::path& file_path, const std::string& filename, ProgressFn prorgess_fn, ErrorFn error_fn) const
{
    const auto file_size = boost::filesystem::file_size(file_path);
#ifdef SIMPLYPRINT_TEST
    constexpr auto buffer_size = MAX_SINGLE_UPLOAD_FILE_SIZE;
#else
    constexpr auto buffer_size = MAX_SINGLE_UPLOAD_FILE_SIZE - 1000000;
#endif

    const auto chunk_amount = (size_t)ceil((double) file_size / buffer_size);

    std::string chunk_id;
    std::string delete_token;

    // Tell SimplyPrint that the upload has failed and the chunks should be deleted
    // Note: any error happens here won't be notified to the user
    const auto clean_up = [this, &chunk_id, &delete_token]() {
        if (chunk_id.empty()) {
            // The initial upload failed, do nothing
            BOOST_LOG_TRIVIAL(warning) << "SimplyPrint: Initial chunk upload failed, skip delete";
            return;
        }

        assert(!delete_token.empty());

        BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: Deleting file chunk %s...") % chunk_id;
        const std::vector<std::pair<std::string, std::string>> query_parameters{
            {"id", chunk_id},
            {"temp", "true"},
            {"delete", delete_token},
        };
        const auto url = (boost::format("%s?%s") % CHUNCK_RECEIVE_URL % url_encode(query_parameters)).str();
        do_api_call(
            [&url](bool is_retry) {
                auto http = Http::get(url);
                return http;
            },
            [&chunk_id](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: File chunk %1% deleted: HTTP %2%: %3%") % chunk_id % status % body;
                return true;
            },
            [&chunk_id](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("SimplyPrint: Error deleting file chunk %1%: %2%, HTTP %3%, body: `%4%`") %
                                                  chunk_id % error % status % body;
                return false;
            });
    };

    // Do chunk upload
    for (size_t i = 0; i < chunk_amount; i++) {
        std::string url;
        {
            std::vector<std::pair<std::string, std::string>> query_parameters{
                {"i", std::to_string(i)},
                {"temp", "true"},
            };
            if (i == 0) {
                query_parameters.emplace_back("filename", filename);
                query_parameters.emplace_back("chunks", std::to_string(chunk_amount));
                query_parameters.emplace_back("totalsize", std::to_string(file_size));
            } else {
                query_parameters.emplace_back("id", chunk_id);
            }
            url = (boost::format("%s?%s") % CHUNCK_RECEIVE_URL % url_encode(query_parameters)).str();
        }

        // Calculate the offset and length of current chunk
        const boost::filesystem::ifstream::off_type offset = i * buffer_size;
        const size_t length = i == (chunk_amount - 1) ? file_size - offset : buffer_size;

        const bool succ = do_api_call(
            [&url, &file_path, &filename, i, chunk_amount, file_size, offset, length, prorgess_fn](bool is_retry) {
                BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: Start uploading file chunk [%1%/%2%]...") % (i + 1) % chunk_amount;
                auto http = Http::post(url);
                http.form_add_file("file", file_path, filename, offset, length);

                http.on_progress([&prorgess_fn, file_size, offset](Http::Progress progress, bool& cancel) {
                    progress.ultotal = file_size;
                    progress.ulnow += offset;

                    prorgess_fn(std::move(progress), cancel);
                });

                return http;
            },
            [&error_fn, i, chunk_amount, this, &chunk_id, &delete_token](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << boost::format("SimplyPrint: File chunk [%1%/%2%] uploaded: HTTP %3%: %4%") % (i + 1) % chunk_amount % status % body;
                if (i == 0) {
                    // First chunk, parse chunk id
                    const auto j = nlohmann::json::parse(body, nullptr, false, true);
                    if (j.is_discarded()) {
                        BOOST_LOG_TRIVIAL(error) << "SimplyPrint: Invalid or no JSON data on ChunkReceive: " << body;
                        error_fn(_L("Unknown error"));
                        return false;
                    }

                    if (j.find("id") == j.end() || j.find("delete_token") == j.end()) {
                        BOOST_LOG_TRIVIAL(error) << "SimplyPrint: Invalid or no JSON data on ChunkReceive: " << body;
                        error_fn(_L("Unknown error"));
                        return false;
                    }

                    const unsigned long id = j["id"];

                    chunk_id = std::to_string(id);
                    delete_token = j["delete_token"];
                }
                return true;
            },
            [this, &error_fn, i, chunk_amount](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("SimplyPrint: Error uploading file chunk [%1%/%2%]: %3%, HTTP %4%, body: `%5%`") %
                                                (i + 1) % chunk_amount % error % status % body;
                error_fn(format_error(body, error, status));
                return false;
            });

        if (!succ) {
            clean_up();
            return false;
        }
    }

    assert(!chunk_id.empty());

    // Finally, complete the upload using the chunk id
    const bool succ = do_temp_upload({}, chunk_id, filename, prorgess_fn, error_fn);
    if (!succ) {
        clean_up();
    }

    return succ;
}


bool SimplyPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    if (cred.find("access_token") == cred.end()) {
        error_fn(_L("SimplyPrint account not linked. Go to Connect options to set it up."));
        return false;
    }
    const auto filename = upload_data.upload_path.filename().string();

    if (boost::filesystem::file_size(upload_data.source_path) > MAX_SINGLE_UPLOAD_FILE_SIZE) {
        // If file is over 100 MB, do chunk upload
        return do_chunk_upload(upload_data.source_path, filename, prorgess_fn, error_fn);
    } else {
        // Normal upload
        return do_temp_upload(upload_data.source_path, {}, filename, prorgess_fn, error_fn);
    }
}

} // namespace Slic3r
