#include "Authenticator.hpp"

#include <cstddef>
#include <stdexcept>

#include <boost/beast/http/field.hpp>

namespace Slic3r {
namespace Server {

namespace http = boost::beast::http;

// ---------------------------------------------------------------------------
// parse_auth_context
//
// Splits the Authorization header value "<scheme> <credential>" into parts.
// If the header is absent or malformed, returns an AuthContext with empty
// fields (authenticators treat that as "no credential presented").
// ---------------------------------------------------------------------------

AuthContext parse_auth_context(const httpHeadersType &headers)
{
    AuthContext ctx;

    auto it = headers.find(http::field::authorization);
    if (it == headers.end())
        return ctx;

    const std::string value(it->value());

    // Split on the first space:  "<scheme> <credential>".
    const auto sp = value.find(' ');
    if (sp == std::string::npos) {
        // No space → treat the whole value as the scheme with no credential.
        ctx.scheme = value;
        return ctx;
    }

    ctx.scheme     = value.substr(0, sp);
    ctx.credential = value.substr(sp + 1);
    return ctx;
}

// ---------------------------------------------------------------------------
// Constant-time string comparison.
//
// Avoids leaking how many leading characters matched via early exit.  Length
// is folded into the accumulator so unequal-length inputs also differ.
// ---------------------------------------------------------------------------

namespace {

bool constant_time_equals(const std::string &a, const std::string &b)
{
    // Mixing the length difference into the result keeps the comparison
    // constant-time with respect to content even when sizes differ.
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());

    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);

    return diff == 0 && a.size() == b.size();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AnonymousAuthenticator
// ---------------------------------------------------------------------------

AuthResult AnonymousAuthenticator::authenticate(const httpHeadersType & /*headers*/) const
{
    AuthResult r;
    r.allowed   = true;
    r.principal = "anonymous";
    return r;
}

// ---------------------------------------------------------------------------
// BearerTokenAuthenticator
// ---------------------------------------------------------------------------

BearerTokenAuthenticator::BearerTokenAuthenticator(std::string token)
    : m_token(std::move(token))
{
    // Fail loud: an empty token would otherwise compare-equal to a client
    // sending "Bearer " with no token, effectively allowing all.
    if (m_token.empty())
        throw std::invalid_argument(
            "BearerTokenAuthenticator: token must not be empty");
}

AuthResult BearerTokenAuthenticator::authenticate(const httpHeadersType &headers) const
{
    AuthResult r;

    const AuthContext ctx = parse_auth_context(headers);

    if (ctx.scheme.empty()) {
        r.error = "Missing Authorization header";
        return r;
    }
    if (ctx.scheme != "Bearer") {
        r.error = "Unsupported authorization scheme (expected Bearer)";
        return r;
    }
    if (!constant_time_equals(ctx.credential, m_token)) {
        r.error = "Invalid bearer token";
        return r;
    }

    r.allowed   = true;
    r.principal = "bearer";
    return r;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::shared_ptr<IAuthenticator> make_authenticator(const ServerAuthConfig &cfg)
{
    if (cfg.scheme == "anonymous") {
        return std::make_shared<AnonymousAuthenticator>();
    } else if (cfg.scheme == "bearer") {
        // Constructor throws std::invalid_argument on empty token.
        return std::make_shared<BearerTokenAuthenticator>(cfg.token);
    }
    // future: else if (cfg.scheme == "apikey") return std::make_shared<ApiKeyAuthenticator>(cfg.token);
    // future: else if (cfg.scheme == "jwt")    return std::make_shared<JwtAuthenticator>(cfg.token);

    throw std::invalid_argument("Unknown auth scheme: '" + cfg.scheme + "'");
}

} // namespace Server
} // namespace Slic3r
