// OutputTarget.cpp — deliver() implementation for SliceCore's headless output routing.
//
// Dispatch structure:
//   deliver()              — public API; try/catch wrapper; switches on OutputMode
//     deliver_file()       — File mode: no-op if already in outputdir, else copy
//     deliver_stdout()     — Stdout mode: stream raw G-code bytes to std::cout
//     deliver_printhost()  — PrintHost mode: synchronous upload via PrintHost::upload()
//
// Adding a new mode (e.g. S3, Webhook):
//   1. Add the new enum value to OutputMode in SliceTypes.hpp.
//   2. Write a deliver_<mode>() helper in the anonymous namespace below.
//   3. Add the corresponding case to the switch in deliver().

#include "OutputTargetDeliver.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

// wx base — provides wxApp::GetInstance() to detect whether a full GUI_App
// is live without assuming a cast is valid.
#include <wx/app.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"        // ConfigOptionString / opt<>()
#include "slic3r/GUI/GUI_App.hpp"      // GUI_App (has public app_config member)
#include "slic3r/Utils/PrintHost.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace SliceCore {

// ─────────────────────────────────────────────────────────────────────────────
// Anonymous-namespace helpers — one per OutputMode
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// ---------------------------------------------------------------------------
// File mode
// ---------------------------------------------------------------------------
// If gcode_path is already inside outputdir (same canonical path as the
// would-be destination), this is a no-op — SliceService may have placed the
// file there directly.  Otherwise the file is copied into outputdir, which is
// created if it does not exist.
bool deliver_file(const OutputTarget &out,
                  const std::string  &gcode_path,
                  std::string        &err)
{
    fs::path src(gcode_path);
    fs::path dir(out.outputdir);

    boost::system::error_code ec;

    // Canonicalise the source so the no-op comparison is reliable.
    fs::path src_canonical = fs::canonical(src, ec);
    if (ec) {
        err = "File mode: cannot resolve source path '" + gcode_path + "': " + ec.message();
        return false;
    }

    // Create destination directory if absent.
    if (!fs::exists(dir)) {
        fs::create_directories(dir, ec);
        if (ec) {
            err = "File mode: cannot create output directory '"
                  + out.outputdir + "': " + ec.message();
            return false;
        }
    }

    fs::path dst        = dir / src.filename();
    fs::path dst_weakly = fs::weakly_canonical(dst, ec);

    // No-op: source is already the destination (SliceService placed it there).
    if (!ec && src_canonical == dst_weakly)
        return true;

    // Copy, overwriting any stale file at the destination.
    fs::copy_file(src, dst, fs::copy_option::overwrite_if_exists, ec);
    if (ec) {
        err = "File mode: failed to copy '" + gcode_path + "' to '"
              + dst.string() + "': " + ec.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stdout mode
// ---------------------------------------------------------------------------
// Streams raw G-code bytes to std::cout.  std::cout.write() is used for
// binary safety (no implicit newline translation on Windows).
bool deliver_stdout(const std::string &gcode_path, std::string &err)
{
    std::ifstream in(gcode_path, std::ios::binary);
    if (!in) {
        err = "Stdout mode: cannot open G-code file '" + gcode_path + "' for reading";
        return false;
    }

    constexpr std::size_t kChunk = 65536;
    char buf[kChunk];
    while (in.read(buf, kChunk) || in.gcount() > 0)
        std::cout.write(buf, in.gcount());

    std::cout.flush();

    if (in.bad()) {
        err = "Stdout mode: I/O error while reading '" + gcode_path + "'";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Headless safety — allow_ip_resolve / IPListDialog crash on Windows
// ---------------------------------------------------------------------------
// PROBLEM
// ─────────────────────────────────────────────────────────────────────────────
// On Windows, OctoPrint.cpp (and ElegooLink.cpp) call
//
//     GUI::get_app_config()->get_bool("allow_ip_resolve")
//
// inside an #ifdef WIN32 block during upload().  GUI::get_app_config() is
// implemented as:
//
//     AppConfig* get_app_config() { return wxGetApp().app_config; }  // GUI.cpp:494
//
// where wxGetApp() is defined by DECLARE_APP(GUI_App) as:
//
//     GUI_App& wxGetApp() { return *static_cast<GUI_App*>(wxApp::GetInstance()); }
//
// When the slicer server runs headless (wxInitialize() only, no GUI_App
// subclass), wxApp::GetInstance() returns a plain wxApp, not a GUI_App.
// The static_cast is then UB and app_config reads garbage — or, if wx is not
// initialised at all, GetInstance() returns null and the dereference crashes.
//
// Furthermore, when allow_ip_resolve is true AND the host is a .local mDNS
// name with 3+ resolved addresses, the code shows an IPListDialog (wxDialog)
// to let the user pick — which also crashes without a live GUI event loop.
//
// THE ACTUAL MITIGATION (there is no RAII guard)
// ─────────────────────────────────────────────────────────────────────────────
// There is no clean way to redirect GUI::get_app_config() from this layer
// without a real GUI_App, so we do NOT attempt to install a temporary
// AppConfig.  The real, sufficient protection is the .local PRE-FLIGHT
// REJECTION in deliver_printhost(): on Windows in headless mode we reject any
// host ending in ".local" before calling upload(), which is the only code
// path that reaches the crashing get_app_config() call.
//
// Why that is sufficient: the problematic branch in OctoPrint.cpp:277 /
// ElegooLink.cpp:657 is guarded by `... && ends_with(host, ".local")`.  For a
// direct IP, the earlier `!ec` branch handles it and get_app_config() is never
// reached.  For a plain DNS name on Windows the ".local" guard excludes it.
// On Linux/macOS the entire #ifdef WIN32 block is skipped.  So once .local
// hosts are rejected in headless mode, get_app_config() is never called.
//
// REMAINING LIMITATION
// ─────────────────────────────────────────────────────────────────────────────
// A .local printer hostname that is ONLY discoverable via mDNS Bonjour (not
// registered in system DNS) cannot be used for headless PrintHost delivery on
// Windows — it is rejected up front.  Configure a direct IP address or a
// fully-qualified DNS name instead.

// Returns the GUI_App* if a real GUI_App is live, nullptr otherwise.
// dynamic_cast on a wrong-type or null wxApp pointer safely returns nullptr;
// it does not crash, which is exactly what lets us detect headless mode.
GUI::GUI_App *detect_gui_app()
{
    return dynamic_cast<GUI::GUI_App *>(wxApp::GetInstance());
}

// ---------------------------------------------------------------------------
// PrintHost mode
// ---------------------------------------------------------------------------
// Builds a PrintHostUpload, then calls host->upload() synchronously on this
// thread.  PrintHostJobQueue is NOT used — it requires a live GUI event loop.
// The headless allow_ip_resolve/IPListDialog crash is neutralised by the
// .local pre-flight rejection below (see "Headless safety" note above) — there
// is deliberately no RAII guard, because none can safely redirect
// get_app_config() from this layer.
//
// wxString → std::string bridge: error.ToUTF8().data()
// (same pattern used in PrinterWebViewHandler.cpp:237).
bool deliver_printhost(const OutputTarget &out,
                       const std::string  &gcode_path,
                       std::string        &err)
{
    // PrintHost::get_print_host() (PrintHost.hpp:81) takes a non-const ptr.
    // Use a local copy so we do not mutate the caller's config.
    DynamicPrintConfig cfg = out.host_config;

#ifdef WIN32
    // ── Headless pre-flight for .local hosts ────────────────────────────────
    // In headless mode (no GUI_App), the Windows mDNS resolution path in
    // OctoPrint/ElegooLink calls GUI::get_app_config() which dereferences a
    // non-GUI_App pointer — UB that typically crashes.  Direct-IP and plain-DNS
    // hosts never enter that path, so they are safe.  Reject .local hosts
    // early with a clear diagnostic rather than crashing silently.
    if (detect_gui_app() == nullptr) {
        const auto *host_opt = cfg.opt<ConfigOptionString>("print_host");
        const std::string host_str = host_opt ? host_opt->value : std::string();
        if (boost::algorithm::iends_with(host_str, ".local")) {
            err = "PrintHost mode: headless upload to '.local' mDNS hosts is "
                  "not supported on Windows — configure a direct IP address or "
                  "a fully-qualified DNS hostname instead.";
            return false;
        }
    }
#endif

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&cfg));
    if (!host) {
        err = "PrintHost mode: host_config does not specify a valid print host "
              "(missing or unrecognised host_type / print_host)";
        return false;
    }

    // Build the upload descriptor.
    // PrintHostUpload fields (PrintHost.hpp ~line 30):
    //   bool                              use_3mf
    //   boost::filesystem::path           source_path   — path to local file
    //   boost::filesystem::path           upload_path   — display name on printer
    //   std::string                       group
    //   std::string                       storage
    //   PrintHostPostUploadAction         post_action
    //   std::map<std::string,std::string> extended_info
    PrintHostUpload upload;
    upload.use_3mf     = false;
    upload.source_path = fs::path(gcode_path);
    upload.upload_path = fs::path(gcode_path).filename();  // display name only
    upload.post_action = out.start_print
                            ? PrintHostPostUploadAction::StartPrint
                            : PrintHostPostUploadAction::None;
    // group, storage, extended_info left empty — use printer defaults.

    // (No RAII guard here: the headless allow_ip_resolve/IPListDialog crash is
    // already neutralised by the .local pre-flight rejection above.  For safe
    // hosts — direct IP, plain DNS — upload() never reaches get_app_config().)

    // Capture upload outcome via callbacks.
    std::string upload_err;

    // Progress callback — no visible UI in headless context; cancel=false.
    auto progress_fn = [](Http::Progress /*p*/, bool & /*cancel*/) {};

    // Error callback.  wxString → std::string: .ToUTF8().data()
    // (PrinterWebViewHandler.cpp:237 uses the identical pattern).
    auto error_fn = [&upload_err](wxString error) {
        upload_err = error.ToUTF8().data();
    };

    // Info callback — informational only, discard.
    auto info_fn = [](wxString /*tag*/, wxString /*status*/) {};

    // Synchronous upload on this thread.
    // Signature (PrintHost.hpp:59):
    //   virtual bool upload(PrintHostUpload upload_data,
    //                       ProgressFn prorgess_fn,
    //                       ErrorFn error_fn,
    //                       InfoFn info_fn) const = 0;
    const bool ok = host->upload(std::move(upload), progress_fn, error_fn, info_fn);

    if (!ok) {
        if (upload_err.empty())
            upload_err = "upload() returned false with no additional detail";
        err = "PrintHost mode: upload failed: " + upload_err;
        return false;
    }
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool deliver(const OutputTarget &out,
             const std::string  &gcode_path,
             std::string        &err)
{
    try {
        switch (out.mode) {
        case OutputMode::File:
            return deliver_file(out, gcode_path, err);

        case OutputMode::Stdout:
            return deliver_stdout(gcode_path, err);

        case OutputMode::PrintHost:
            return deliver_printhost(out, gcode_path, err);

        default:
            err = "deliver(): unhandled OutputMode value "
                  + std::to_string(static_cast<int>(out.mode));
            return false;
        }
    } catch (const std::exception &e) {
        err = std::string("deliver(): unhandled exception: ") + e.what();
        return false;
    } catch (...) {
        err = "deliver(): unknown exception";
        return false;
    }
}

} // namespace SliceCore
} // namespace Slic3r
