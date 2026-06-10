//
// orca-server — headless REST server for remote slicing jobs.
//
// Startup sequence:
//   1. Parse command-line flags.
//   2. wxInitialize()  — needed because libslic3r internals use wxString /
//      wx temp-dir helpers.  NO event loop or wxApp is created; we just
//      initialize wx's low-level runtime.
//   3. Construct JobQueue and HttpServer.
//   4. Install SIGINT/SIGTERM handler that calls HttpServer::stop().
//   5. HttpServer::run() blocks until stop() is called.
//   6. wxUninitialize() at exit.
//

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

// wxWidgets headless init — provides wx runtime (wxString, file utils) without
// a display or event loop.  Must be included before any other wx header that
// might pull in wx/app.h indirectly.
#include <wx/init.h>

// POSIX signal handling (SIGINT / SIGTERM).
// On Windows, signal() from <csignal> handles Ctrl-C (SIGINT); SIGTERM is
// also raised by some shutdown mechanisms.
#include <csignal>

#include "Authenticator.hpp"
#include "HttpServer.hpp"
#include "JobQueue.hpp"
#include "ServerContext.hpp"

// ---------------------------------------------------------------------------
// Global stop flag — touched by signal handler then by HttpServer::stop()
// ---------------------------------------------------------------------------
namespace {
// Raw pointer to the live server so the signal handler can call stop().
// Set before signal() is installed; never modified after that.
Slic3r::Server::HttpServer *g_server = nullptr;

extern "C" void signal_handler(int /*sig*/)
{
    if (g_server)
        g_server->stop();
}

void install_signal_handlers()
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
}

// ---------------------------------------------------------------------------
// Minimal argument parser — avoids pulling in Boost.ProgramOptions
// ---------------------------------------------------------------------------

struct Config {
    std::string    addr      = "0.0.0.0";
    unsigned short port      = 8080;
    std::string    auth;        // "", "none", "bearer", (future: "apikey", "jwt", …)
    std::string    token;
    std::string    datadir;
    std::string    resources;
    int            workers   = 1;
};

void usage(const char *argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --addr <ip>        Bind address  (default: 0.0.0.0)\n"
        << "  --port <n>         TCP port      (default: 8080)\n"
        << "  --auth <scheme>    Auth scheme: 'none' (anonymous) or 'bearer'.\n"
        << "                     If omitted: 'bearer' when a token is provided,\n"
        << "                     otherwise 'none' (anonymous).\n"
        << "  --token <str>      Bearer token (or set ORCA_SERVER_TOKEN).\n"
        << "                     Required when --auth bearer.\n"
        << "  --datadir <path>   OrcaSlicer data directory\n"
        << "  --resources <path> OrcaSlicer resources directory\n"
        << "  --workers <n>      Job worker threads (default: 1)\n"
        << "\n"
        << "  future: --auth apikey|jwt|hmac|mtls (one new class + one factory line)\n"
        << "\n";
}

bool parse_args(int argc, char **argv, Config &cfg)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto require_next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--addr") {
            const char *v = require_next("--addr");
            if (!v) return false;
            cfg.addr = v;
        } else if (arg == "--port") {
            const char *v = require_next("--port");
            if (!v) return false;
            cfg.port = static_cast<unsigned short>(std::stoi(v));
        } else if (arg == "--auth") {
            const char *v = require_next("--auth");
            if (!v) return false;
            cfg.auth = v;
        } else if (arg == "--token") {
            const char *v = require_next("--token");
            if (!v) return false;
            cfg.token = v;
        } else if (arg == "--datadir") {
            const char *v = require_next("--datadir");
            if (!v) return false;
            cfg.datadir = v;
        } else if (arg == "--resources") {
            const char *v = require_next("--resources");
            if (!v) return false;
            cfg.resources = v;
        } else if (arg == "--workers") {
            const char *v = require_next("--workers");
            if (!v) return false;
            cfg.workers = std::stoi(v);
        } else if (arg == "--help" || arg == "-h") {
            return false; // Trigger usage print.
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    Config cfg;

    if (!parse_args(argc, argv, cfg)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Token may also come from the environment.
    if (cfg.token.empty()) {
        const char *env = std::getenv("ORCA_SERVER_TOKEN");
        if (env && *env)
            cfg.token = env;
    }

    if (cfg.workers < 1)
        cfg.workers = 1;

    // -----------------------------------------------------------------------
    // Resolve the authentication scheme.
    //
    // DEFAULT-MODE LOGIC (explicit & documented):
    //   --auth given        → use it verbatim ("none" | "bearer" | future…).
    //   --auth omitted:
    //       token present   → "bearer"   (a token clearly signals intent).
    //       token absent    → "none"     (ANONYMOUS — open access).
    //
    // This makes the open/anonymous case the default ONLY when no token was
    // supplied.  Supplying a token without --auth is treated as opting in to
    // bearer auth, never silently ignored.
    // -----------------------------------------------------------------------
    Slic3r::Server::ServerAuthConfig auth_cfg;
    {
        std::string scheme = cfg.auth;
        // Normalise the user-facing alias "none" to the internal "anonymous".
        if (scheme == "none")
            scheme = "anonymous";

        if (scheme.empty())
            scheme = cfg.token.empty() ? "anonymous" : "bearer";

        // Guard rail: requesting bearer with no token is a misconfiguration.
        // make_authenticator() also enforces this, but we check here to emit a
        // friendlier message before construction.
        if (scheme == "bearer" && cfg.token.empty()) {
            std::cerr << "Error: --auth bearer requires --token "
                         "(or ORCA_SERVER_TOKEN)\n";
            usage(argv[0]);
            return EXIT_FAILURE;
        }

        auth_cfg.scheme = scheme;
        auth_cfg.token  = cfg.token;
    }

    // Build the authenticator via the factory.  A bad scheme or empty bearer
    // token throws std::invalid_argument — fail loud at startup, never
    // silently allow all traffic.
    std::shared_ptr<Slic3r::Server::IAuthenticator> authenticator;
    try {
        authenticator = Slic3r::Server::make_authenticator(auth_cfg);
    } catch (const std::exception &ex) {
        std::cerr << "Error: invalid auth configuration — " << ex.what() << "\n";
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // -----------------------------------------------------------------------
    // Headless wxWidgets initialisation.
    //
    // wxInitialize() sets up wx's runtime (locale, file path helpers,
    // wxString encoding, temporary directory) without creating a wxApp
    // subclass, a GUI event loop, or any window.
    //
    // libslic3r uses wxString in a handful of utility paths (e.g. data-dir
    // resolution, log sinks).  wxInitialize() is sufficient for these.
    //
    // wxUninitialize() is called at exit via the guard below.
    // -----------------------------------------------------------------------
    if (!wxInitialize()) {
        std::cerr << "Warning: wxInitialize() failed — wx-dependent paths "
                     "inside SliceService may not work correctly.\n";
        // Continue anyway; slicing may succeed for models that don't hit wx
        // code paths at runtime.
    }

    // RAII guard so wxUninitialize() runs even on early returns.
    struct WxGuard {
        ~WxGuard() { wxUninitialize(); }
    } wx_guard;

    // -----------------------------------------------------------------------
    // Construct the job queue and HTTP server.
    // -----------------------------------------------------------------------
    Slic3r::Server::ServerContext srv_ctx;
    srv_ctx.datadir       = cfg.datadir;
    srv_ctx.resources_dir = cfg.resources;

    Slic3r::Server::JobQueue queue(cfg.resources, cfg.workers);
    Slic3r::Server::HttpServer server(cfg.addr, cfg.port, authenticator,
                                      srv_ctx, queue);

    // Install signal handlers after constructing the server so g_server is
    // valid when a signal fires.
    g_server = &server;
    install_signal_handlers();

    std::cerr << "[orca-server] Starting — addr=" << cfg.addr
              << " port=" << cfg.port
              << " workers=" << cfg.workers
              << " auth=" << authenticator->name()
              << "\n";
    if (auth_cfg.scheme == "anonymous") {
        std::cerr << "[orca-server] WARNING: anonymous auth — all /v1/* "
                     "requests are accepted without credentials.\n";
    }

    // Blocks until stop() is called (via signal or external request).
    server.run();

    g_server = nullptr;
    std::cerr << "[orca-server] Shutdown complete.\n";
    return EXIT_SUCCESS;
}
