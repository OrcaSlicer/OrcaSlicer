#include "slic3r/UpdateHelper/HttpDownloader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

#include <curl/curl.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "libslic3r_version.h" // SoftFever_VERSION

namespace fs = std::filesystem;

namespace Slic3r {
namespace Updater {

namespace {

constexpr long kConnectTimeoutSeconds = 15;
constexpr long kLowSpeedSeconds = 60;
constexpr long kLowSpeedBytesPerSecond = 1024;
constexpr long kMaxRedirects = 10;
// Report at most once per this many new bytes, so the event loop is not flooded.
constexpr curl_off_t kReportEveryBytes = 64 * 1024;

bool truncate_stream(std::FILE* file)
{
    if (std::fflush(file) != 0)
        return false;
#if defined(_WIN32)
    return ::_chsize_s(::_fileno(file), 0) == 0;
#else
    return ::ftruncate(::fileno(file), 0) == 0;
#endif
}

struct TransferCtx
{
    CURL*                     curl = nullptr;
    std::FILE*                file = nullptr;
    curl_off_t                resume = 0; // bytes already in the .part file
    curl_off_t                done = 0;   // absolute bytes written so far
    bool                      first_write = true;
    bool                      write_failed = false;
    const std::atomic<bool>*  cancel = nullptr;
    bool                      saw_cancel = false;
    const HttpDownloader::ProgressFn* progress = nullptr;
    curl_off_t                last_reported = -1;
    curl_off_t                total = 0;
};

void report_progress(TransferCtx& ctx, curl_off_t dltotal, curl_off_t dlnow)
{
    if (!ctx.progress || !*ctx.progress)
        return;
    const curl_off_t total = dltotal > 0 ? ctx.resume + dltotal : 0;
    const curl_off_t done = ctx.resume + std::max<curl_off_t>(dlnow, 0);
    ctx.total = total;
    if (ctx.last_reported >= 0 && done - ctx.last_reported < kReportEveryBytes && (total <= 0 || done < total))
        return;
    ctx.last_reported = done;
    (*ctx.progress)(done, total);
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    TransferCtx& ctx = *static_cast<TransferCtx*>(userdata);
    const size_t bytes = size * nmemb;
    if (ctx.first_write) {
        ctx.first_write = false;
        // The server may ignore our Range request. Writing on top of the old
        // bytes would corrupt the file, so start it over when that happens.
        if (ctx.resume > 0) {
            long code = 0;
            if (ctx.curl)
                curl_easy_getinfo(ctx.curl, CURLINFO_RESPONSE_CODE, &code);
            if (code == 200) {
                if (!truncate_stream(ctx.file) || std::fseek(ctx.file, 0, SEEK_SET) != 0) {
                    ctx.write_failed = true;
                    return 0;
                }
                ctx.resume = 0;
                ctx.done = 0;
                ctx.last_reported = -1;
            }
        }
    }
    const size_t written = bytes > 0 ? std::fwrite(ptr, 1, bytes, ctx.file) : 0;
    if (written != bytes) {
        ctx.write_failed = true;
        return 0; // makes libcurl stop with CURLE_WRITE_ERROR
    }
    ctx.done += static_cast<curl_off_t>(bytes);
    return bytes;
}

int progress_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    TransferCtx& ctx = *static_cast<TransferCtx*>(userdata);
    if (ctx.cancel && ctx.cancel->load()) {
        ctx.saw_cancel = true;
        return 1; // aborts with CURLE_ABORTED_BY_CALLBACK
    }
    report_progress(ctx, dltotal, dlnow);
    return 0;
}

std::string respond_error(CURLcode result, long http_code, const char* error_buffer)
{
    std::string message;
    if (error_buffer && error_buffer[0])
        message = error_buffer;
    else
        message = curl_easy_strerror(result);
    if (http_code >= 400)
        message += " (HTTP " + std::to_string(http_code) + ")";
    return message;
}

} // namespace

std::string HttpDownloader::user_agent()
{
    return std::string("OrcaSlicer-Updater/") + SoftFever_VERSION;
}

DownloadResult HttpDownloader::download(const std::string&       url,
                                        const std::string&       part_path,
                                        const ProgressFn&        progress,
                                        const std::atomic<bool>& cancel) const
{
    DownloadResult out;

    if (url.empty() || part_path.empty()) {
        out.error = "The download address is missing.";
        return out;
    }

    if (cancel.load()) {
        out.cancelled = true;
        return out;
    }

    {
        std::error_code ec;
        const fs::path parent = fs::path(part_path).parent_path();
        if (!parent.empty())
            fs::create_directories(parent, ec);
        if (ec) {
            out.error = "Could not create the download folder: " + ec.message();
            return out;
        }
    }

    curl_off_t resume = 0;
    {
        std::error_code ec;
        const auto size = fs::file_size(part_path, ec);
        if (!ec)
            resume = static_cast<curl_off_t>(size);
    }
    out.resumed_from = static_cast<int64_t>(resume);

    std::FILE* file = std::fopen(part_path.c_str(), resume > 0 ? "ab" : "wb");
    if (!file) {
        out.error = "Could not open the download file: " + part_path;
        return out;
    }

    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        out.error = "Could not start the download.";
        return out;
    }

    TransferCtx ctx;
    ctx.curl = curl;
    ctx.file = file;
    ctx.resume = resume;
    ctx.done = resume;
    ctx.cancel = &cancel;
    ctx.progress = &progress;

    char error_buffer[CURL_ERROR_SIZE] = { 0 };
    const std::string agent = user_agent();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (resume > 0) {
        // libcurl turns this into "Range: bytes=<resume>-".
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume);
    }

    report_progress(ctx, 0, 0);
    const CURLcode result = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    out.http_code = http_code;

    curl_easy_cleanup(curl);
    if (std::fflush(file) != 0)
        ctx.write_failed = true;
    std::fclose(file);

    {
        std::error_code ec;
        const auto size = fs::file_size(part_path, ec);
        if (!ec)
            out.done = static_cast<int64_t>(size);
        out.total = std::max<int64_t>(out.done, static_cast<int64_t>(ctx.total));
    }

    if (ctx.saw_cancel) {
        out.cancelled = true;
        return out;
    }
    if (ctx.write_failed) {
        out.error = "Could not write the download to disk.";
        return out;
    }
    if (result == CURLE_ABORTED_BY_CALLBACK) {
        out.cancelled = true;
        return out;
    }
    // A resumed request that the server cannot satisfy: the .part already holds
    // at least the whole file, so let verification decide.
    if (http_code == 416 && resume > 0) {
        out.ok = true;
        return out;
    }
    if (result != CURLE_OK) {
        out.error = respond_error(result, http_code, error_buffer);
        return out;
    }
    if (http_code >= 400) {
        out.error = "The server refused the download (HTTP " + std::to_string(http_code) + ").";
        return out;
    }

    out.ok = true;
    return out;
}

} // namespace Updater
} // namespace Slic3r
