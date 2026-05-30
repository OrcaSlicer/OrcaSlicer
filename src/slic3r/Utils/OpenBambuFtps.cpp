// OpenBambuFtps - FTPS upload for BambuLab printers using libcurl.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// Uses libcurl (already a project dependency) for FTPS implicit-TLS uploads
// to BambuLab printers on port 990. The printer expects files in /sdcard/
// or at the FTPS root depending on firmware version.

#include "OpenBambuFtps.hpp"

#include <boost/log/trivial.hpp>

#include <curl/curl.h>

#include <cstdio>
#include <string>

namespace Slic3r {

namespace {

struct UploadContext {
    FILE*          file      = nullptr;
    uint64_t       total     = 0;
    uint64_t       sent      = 0;
    FtpsProgressCb progress;
    bool           cancelled = false;
};

size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* ctx = static_cast<UploadContext*>(userdata);
    if (ctx->cancelled) return CURL_READFUNC_ABORT;
    size_t bytes = fread(buffer, size, nitems, ctx->file);
    ctx->sent += bytes;
    return bytes;
}

int progress_callback(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow)
{
    auto* ctx = static_cast<UploadContext*>(userdata);
    if (ctx->progress) {
        uint64_t total = ctx->total > 0 ? ctx->total : static_cast<uint64_t>(ultotal);
        if (!ctx->progress(static_cast<uint64_t>(ulnow), total)) {
            ctx->cancelled = true;
            return 1; // Non-zero aborts the transfer
        }
    }
    return 0;
}

} // namespace

std::string open_bambu_ftps_upload(const FtpsUploadConfig& cfg,
                                   const std::string& local_path,
                                   const std::string& remote_path,
                                   FtpsProgressCb progress_cb)
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuFtps: uploading " << local_path
                            << " to ftps://" << cfg.host << ":" << cfg.port << remote_path;

    FILE* file = fopen(local_path.c_str(), "rb");
    if (!file) {
        return "Failed to open file: " + local_path;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        return "File is empty or unreadable: " + local_path;
    }

    UploadContext ctx;
    ctx.file     = file;
    ctx.total    = static_cast<uint64_t>(file_size);
    ctx.progress = std::move(progress_cb);

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        return "curl_easy_init() failed";
    }

    // Build FTP URL
    std::string url = "ftps://" + cfg.host + ":" + std::to_string(cfg.port) + remote_path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Authentication
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());

    // Upload mode
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));

    // TLS settings - implicit FTPS
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    if (cfg.tls_insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // Progress
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    // FTP options
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR);

    // Perform the upload
    CURLcode res = curl_easy_perform(curl);

    std::string error;
    if (res != CURLE_OK) {
        if (ctx.cancelled) {
            error = "upload cancelled";
        } else {
            error = std::string("FTPS upload failed: ") + curl_easy_strerror(res);
        }
        BOOST_LOG_TRIVIAL(error) << "OpenBambuFtps: " << error;
    } else {
        BOOST_LOG_TRIVIAL(info) << "OpenBambuFtps: upload complete (" << ctx.sent << " bytes)";
    }

    curl_easy_cleanup(curl);
    fclose(file);
    return error;
}

} // namespace Slic3r
