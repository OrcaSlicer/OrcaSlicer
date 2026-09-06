#ifndef slic3r_Printago_hpp_
#define slic3r_Printago_hpp_

#include <optional>
#include <string>

namespace Slic3r {

// Printago is integrated as an embedded web experience: OrcaSlicer hosts app.printago.io in a
// webview and never calls the Printago API directly. The only credential OrcaSlicer holds is an
// opaque, long-lived "Orca session token" used to re-establish the gated web session on relaunch.
// The web app hands that token to native by navigating to a custom-scheme callback URL, which the
// webview intercepts (see parse_callback_token).

// All Printago web locations in one place (trivial to point at staging or a mock).
struct PrintagoEndpoints
{
    std::string web_base;    // user-facing web app, e.g. https://app.printago.io
    std::string orca_path;   // first-login entry route, e.g. /orca
    std::string launch_path; // relaunch/restore route, e.g. /orca/launch
    std::string send_path;   // "Save"/"Save & Queue" embedded page route, e.g. /orca/send

    std::string login_url() const;                          // web_base + orca_path
    std::string launch_url(const std::string& token) const; // web_base + launch_path + "#token=" + token
    std::string send_url() const;                           // web_base + send_path + "?intent=save"
    std::string queue_url() const;                          // web_base + send_path + "?intent=queue"
    // Replace-confirmation dialog for an Edit-in-Orca session: web_base + "/orca/replace?partId=<id>".
    std::string replace_url(const std::string& part_id) const;
};

// web_base defaults to production (https://app.printago.io). Override for dev by setting the
// PRINTAGO_URL environment variable (e.g. http://localhost:3000); trailing slashes are trimmed.
std::string printago_web_base();

PrintagoEndpoints default_printago_endpoints();

// Custom URI the embedded web app navigates to in order to hand a session token back to native.
inline constexpr const char* PRINTAGO_CALLBACK_PREFIX = "orcaslicer://auth/callback";

class Printago
{
public:
    explicit Printago(PrintagoEndpoints endpoints = default_printago_endpoints());

    // --- Secure session-token storage --------------------------------------------------------
    // Backed by wxSecretStore (macOS Keychain / Windows Credential Manager / Linux libsecret),
    // mirroring how OrcaCloudServiceAgent stores its refresh token. Never written in plaintext.
    bool               load_session_token(); // refresh the in-memory token from the secure store
    void               save_session_token(const std::string& token);
    void               clear_session_token();
    bool               is_connected() const { return !token_.empty(); }
    const std::string& session_token() const { return token_; }

    // --- Web entry points ---------------------------------------------------------------------
    std::string login_url() const;    // first login (no token yet)
    std::string relaunch_url() const; // restore the session with the stored token (or login_url if none)
    std::string send_url() const;     // "Save to Printago" embedded page (self-authenticating webview)
    std::string queue_url() const;    // "Save & Queue to Printago" embedded page

    // --- Pure helper (no I/O, unit-testable) --------------------------------------------------
    // Extract the token from an orcaslicer://auth/callback?token=... URL. nullopt if not a
    // Printago auth callback or no token present.
    static std::optional<std::string> parse_callback_token(const std::string& url);

    const PrintagoEndpoints& endpoints() const { return ep_; }

private:
    PrintagoEndpoints ep_;
    std::string       token_;
};

} // namespace Slic3r

#endif // slic3r_Printago_hpp_
