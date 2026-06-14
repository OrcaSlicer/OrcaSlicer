#include "Router.hpp"

#include <stdexcept>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>

#include "Handlers.hpp"

namespace Slic3r {
namespace Server {

namespace http = boost::beast::http;

Router::Router(std::shared_ptr<IAuthenticator> authenticator,
               ServerContext ctx, JobQueue &queue)
    : m_auth(std::move(authenticator))
    , m_ctx(std::move(ctx))
    , m_queue(queue)
{
    // The authenticator is a hard dependency; a null pointer would mean no
    // auth decision could be made.  Fail loud rather than silently allow all.
    if (!m_auth)
        throw std::invalid_argument("Router: authenticator must not be null");
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

Response Router::dispatch(const Request &req) const
{
    const bool keep_alive = req.keep_alive();
    const std::string target(req.target());

    // -----------------------------------------------------------------------
    // Open endpoint — no auth required
    // -----------------------------------------------------------------------
    if (req.method() == http::verb::get && target == "/healthz") {
        return make_json(200, R"({"ok":true})", keep_alive);
    }

    // -----------------------------------------------------------------------
    // All /v1/* go through the injected authenticator.
    //
    // In anonymous mode the authenticator simply allows every request; in
    // bearer mode it validates the token.  /healthz (handled above) is always
    // open regardless of the configured scheme.
    // -----------------------------------------------------------------------
    if (target.substr(0, 3) == "/v1") {
        // req is-a http::fields (the request derives from it), so it binds
        // directly to the authenticator's httpHeadersType parameter.
        const AuthResult auth = m_auth->authenticate(req);
        if (!auth.allowed) {
            const std::string msg = auth.error.empty()
                                        ? "Unauthorized"
                                        : auth.error;
            return make_unauthorized(msg, m_auth->challenge(), keep_alive);
        }
    }

    // GET /v1/profiles
    if (req.method() == http::verb::get && target.substr(0, 12) == "/v1/profiles") {
        return handle_profiles(req, m_ctx, m_profile_cache, keep_alive);
    }

    // POST /v1/jobs
    if (req.method() == http::verb::post && target == "/v1/jobs") {
        return handle_job_submit(req, m_ctx, m_queue, keep_alive);
    }

    // Routes that require a job id: /v1/jobs/{id}[/result|/preview]
    // We check the prefix then extract the id segment.
    const std::string jobs_prefix = "/v1/jobs/";
    if (target.substr(0, jobs_prefix.size()) == jobs_prefix) {
        // Strip query string for suffix matching; the full target (with query)
        // is forwarded to handlers so they can read ?plate=N themselves.
        const std::string rest_with_query = target.substr(jobs_prefix.size()); // id[/result|/preview][?...]
        const auto qmark = rest_with_query.find('?');
        const std::string rest = (qmark == std::string::npos)
                                     ? rest_with_query
                                     : rest_with_query.substr(0, qmark);

        const std::string result_suffix  = "/result";
        const std::string preview_suffix = "/preview";

        bool is_result  = (rest.size() > result_suffix.size() &&
                           rest.substr(rest.size() - result_suffix.size()) == result_suffix);
        bool is_preview = (!is_result &&
                           rest.size() > preview_suffix.size() &&
                           rest.substr(rest.size() - preview_suffix.size()) == preview_suffix);

        std::string job_id;
        if (is_result)
            job_id = rest.substr(0, rest.size() - result_suffix.size());
        else if (is_preview)
            job_id = rest.substr(0, rest.size() - preview_suffix.size());
        else
            job_id = rest;

        // Reject empty or obviously-invalid ids (no slashes allowed in the id).
        if (job_id.empty() || job_id.find('/') != std::string::npos)
            return make_error(400, "Invalid job id", keep_alive);

        if (is_result && req.method() == http::verb::get)
            return handle_job_result(req, job_id, m_queue, keep_alive);

        if (is_preview && req.method() == http::verb::get)
            return handle_job_preview(req, job_id, m_queue, keep_alive);

        if (!is_result && !is_preview) {
            if (req.method() == http::verb::get)
                return handle_job_status(req, job_id, m_queue, keep_alive);
            if (req.method() == http::verb::delete_)
                return handle_job_cancel(req, job_id, m_queue, keep_alive);
        }
    }

    // Fall-through: no route matched.
    return make_error(404, "Not found", keep_alive);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Router::Response Router::make_error(unsigned status, const std::string &msg,
                                    bool keep_alive)
{
    std::string body = "{\"error\":\"" + msg + "\"}";
    return make_json(status, body, keep_alive);
}

Router::Response Router::make_unauthorized(const std::string &msg,
                                           const std::string &challenge,
                                           bool keep_alive)
{
    Response res = make_error(401, msg, keep_alive);
    // RFC 7235: a 401 should carry a WWW-Authenticate challenge when the
    // scheme defines one (e.g. "Bearer").  Anonymous / API-key schemes return
    // an empty challenge, in which case we omit the header.
    if (!challenge.empty())
        res.set(http::field::www_authenticate, challenge);
    return res;
}

Router::Response Router::make_json(unsigned status, const std::string &body,
                                   bool keep_alive)
{
    Response res{http::status{status}, 11};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = body;
    res.prepare_payload();
    return res;
}

} // namespace Server
} // namespace Slic3r
