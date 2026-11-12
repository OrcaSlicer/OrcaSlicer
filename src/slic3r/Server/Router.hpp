#pragma once

#include <memory>
#include <string>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include "Authenticator.hpp"
#include "JobQueue.hpp"
#include "ServerContext.hpp"

namespace Slic3r {
namespace Server {

// Router owns the routing logic and dispatches beast HTTP requests to the
// correct handler function.  Authentication is delegated to an injected
// IAuthenticator (dependency injection) so the routing layer is decoupled
// from any particular auth scheme.
//
// Thread-safety: all public methods are safe to call from multiple threads
// simultaneously because JobQueue and ProfileCache are internally
// mutex-guarded, the injected authenticator is required to be thread-safe, and
// Router itself carries no mutable state beyond the references/pointers it
// holds (the ProfileCache member is the one mutable, internally-locked piece).
class Router {
public:
    using Request  = boost::beast::http::request<boost::beast::http::string_body>;
    using Response = boost::beast::http::response<boost::beast::http::string_body>;

    // authenticator — injected auth strategy (must be non-null; shared across
    //                  all connection threads).
    // ctx           — server-wide config (datadir/resources); copied in.
    // queue         — job queue reference; must outlive Router.
    Router(std::shared_ptr<IAuthenticator> authenticator,
           ServerContext ctx, JobQueue &queue);

    // Dispatch a request and produce a fully-formed response.
    Response dispatch(const Request &req) const;

private:
    std::shared_ptr<IAuthenticator> m_auth;
    ServerContext                   m_ctx;
    JobQueue                       &m_queue;
    // Lazily-loaded preset-name cache (internally mutex-guarded). Mutable so
    // dispatch() can remain const while still populating the cache on demand.
    mutable ProfileCache            m_profile_cache;

    // Build a JSON error response.
    static Response make_error(unsigned status, const std::string &msg,
                               bool keep_alive = false);

    // Build a JSON error response that also advertises a WWW-Authenticate
    // challenge (used for 401 responses when the authenticator supplies one).
    static Response make_unauthorized(const std::string &msg,
                                      const std::string &challenge,
                                      bool keep_alive);

    // Build a JSON response body.
    static Response make_json(unsigned status, const std::string &body,
                              bool keep_alive = false);
};

} // namespace Server
} // namespace Slic3r
