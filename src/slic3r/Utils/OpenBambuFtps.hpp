#pragma once

// OpenBambuFtps - FTPS upload for BambuLab printers using libcurl.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// BambuLab printers accept FTPS file uploads on port 990 (implicit TLS).
// Username is "bblp", password is the printer's access code.
// Files are uploaded to /sdcard/ or / depending on firmware.

#include <functional>
#include <string>
#include <cstdint>

namespace Slic3r {

/// Progress callback: (bytes_sent, bytes_total) -> return false to cancel
using FtpsProgressCb = std::function<bool(uint64_t sent, uint64_t total)>;

struct FtpsUploadConfig {
    std::string host;
    int         port      = 990;
    std::string username  = "bblp";
    std::string password;
    bool        use_tls   = true;
    bool        tls_insecure = true; // Skip cert verification for LAN mode
};

/// Upload a local file to a BambuLab printer via FTPS.
/// Returns empty string on success, error message on failure.
std::string open_bambu_ftps_upload(const FtpsUploadConfig& cfg,
                                   const std::string& local_path,
                                   const std::string& remote_path,
                                   FtpsProgressCb progress_cb = nullptr);

} // namespace Slic3r
