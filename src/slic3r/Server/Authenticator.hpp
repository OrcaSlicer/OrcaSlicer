#pragma once

#include <memory>
#include <string>

#include <boost/beast/http/fields.hpp>

namespace Slic3r {
namespace Server {

// The header map type passed to authenticators.  This is the base class of
// every beast http::request/response, so a Router can hand an authenticator
// the request's full set of headers without coupling it to the body type.
//
// An authenticator may read ANY header it needs:
//   - Authorization               (Bearer, Basic, …)
//   - X-Api-Key                   (API-key schemes)
//   - X-Client-Cert / forwarded   (mTLS via a terminating proxy)
//   - custom HMAC signature headers
// New schemes therefore require NO change to this interface.
using httpHeadersType = boost::beast::http::fields;

// Parsed view of the Authorization header, provided as a convenience to
// authenticators that key off the standard scheme/credential split.
// (Authenticators are free to ignore this and read raw headers instead.)
struct AuthContext {
    std::string scheme;      // e.g. "Bearer", "Basic" (empty if no Authorization header)
    std::string credential;  // the token / encoded credential following the scheme
};

// Outcome of an authentication attempt.
struct AuthResult {
    bool        allowed = false;  // true → request may proceed
    std::string principal;        // identity granted (e.g. "anonymous", token id)
    std::string error;            // human-readable reason when allowed == false
};

// Strategy interface for request authentication.
//
// Implementations are stateless after construction and MUST be thread-safe:
// a single instance is shared (via shared_ptr) across all connection threads
// and invoked concurrently with no external locking.
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;

    // Inspect the incoming request headers and decide allow/deny.
    // Implementations should be const and side-effect free.
    virtual AuthResult authenticate(const httpHeadersType &headers) const = 0;

    // Stable identifier for logging / diagnostics (e.g. "anonymous", "bearer").
    virtual const char *name() const = 0;

    // Optional: the value to emit in a WWW-Authenticate header on 401.
    // Returns an empty string when the scheme has no challenge to advertise
    // (e.g. anonymous, or API-key schemes that do not use WWW-Authenticate).
    virtual std::string challenge() const { return {}; }
};

// ---------------------------------------------------------------------------
// Helper: parse the Authorization header into an AuthContext.
// Declared here so both authenticators and tests can reuse it.
// ---------------------------------------------------------------------------
AuthContext parse_auth_context(const httpHeadersType &headers);

// ===========================================================================
// Concrete authenticators
// ===========================================================================

// ANONYMOUS mode: always allows, principal "anonymous".
class AnonymousAuthenticator final : public IAuthenticator {
public:
    AuthResult  authenticate(const httpHeadersType &headers) const override;
    const char *name() const override { return "anonymous"; }
};

// BEARER mode: requires  Authorization: Bearer <token>  matching a fixed token.
// The token is compared in constant time to avoid leaking length/prefix via
// timing.  An empty configured token is rejected at construction time.
class BearerTokenAuthenticator final : public IAuthenticator {
public:
    // Throws std::invalid_argument if token is empty — fail loud, never
    // silently allow all.
    explicit BearerTokenAuthenticator(std::string token);

    AuthResult  authenticate(const httpHeadersType &headers) const override;
    const char *name() const override { return "bearer"; }
    std::string challenge() const override { return "Bearer"; }

private:
    std::string m_token;
};

// future authenticators (one new class each, no interface change):
//   class ApiKeyAuthenticator  final : public IAuthenticator { ... };  // X-Api-Key header
//   class JwtAuthenticator     final : public IAuthenticator { ... };  // verify signed JWT
//   class HmacAuthenticator    final : public IAuthenticator { ... };  // signed-request HMAC
//   class MtlsAuthenticator    final : public IAuthenticator { ... };  // client-cert via proxy header

// ===========================================================================
// Factory
// ===========================================================================

// Selected auth scheme plus its parameters.  Populated from CLI flags / env.
struct ServerAuthConfig {
    std::string scheme = "anonymous";  // "anonymous" | "bearer" | (future: "apikey" | "jwt" | …)
    std::string token;                 // used by "bearer"
};

// Map a ServerAuthConfig to a concrete authenticator instance.
//
// Adding a new scheme is exactly:
//   1. one new IAuthenticator subclass (above), and
//   2. one new `else if (cfg.scheme == "<name>")` line in this factory.
// No other code changes are needed anywhere in the server.
//
// Throws std::invalid_argument on an unknown scheme or invalid parameters
// (e.g. bearer with an empty token) so misconfiguration fails loudly at
// startup rather than silently allowing all traffic.
std::shared_ptr<IAuthenticator> make_authenticator(const ServerAuthConfig &cfg);

} // namespace Server
} // namespace Slic3r
