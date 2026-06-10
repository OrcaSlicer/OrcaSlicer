#pragma once

#include <string>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include "JobQueue.hpp"
#include "ServerContext.hpp"

namespace Slic3r {
namespace Server {

using Request  = boost::beast::http::request<boost::beast::http::string_body>;
using Response = boost::beast::http::response<boost::beast::http::string_body>;

// GET /v1/profiles?type=printer|process|filament
// Enumerates preset names from a PresetBundle loaded from ctx.datadir, served
// from a mutex-guarded cache.  Returns 200 {"type":..,"names":[...]}; 400 on a
// bad/missing type; 500 if the bundle cannot be loaded.
Response handle_profiles(const Request &req, const ServerContext &ctx,
                          ProfileCache &cache, bool keep_alive);

// POST /v1/jobs
// Accepts either:
//   (a) multipart/form-data with a "model" file part and a "config" text part, or
//   (b) application/json with a "model_b64" base64-encoded model bytes field.
//
// Builds a SliceRequest (filling req.datadir from ctx.datadir) and enqueues it
// via JobQueue::submit().
// Returns 201 {"job_id": "..."} on success; 400/422 on parse error.
Response handle_job_submit(const Request &req, const ServerContext &ctx,
                            JobQueue &queue, bool keep_alive);

// GET /v1/jobs/{id}
// Returns 200 {state, progress, message, plates} or 404.
Response handle_job_status(const Request &req, const std::string &job_id,
                            JobQueue &queue, bool keep_alive);

// GET /v1/jobs/{id}/result
// Streams the gcode from the first completed plate.
// Returns 200 with Content-Disposition: attachment, or 404/409.
Response handle_job_result(const Request &req, const std::string &job_id,
                            JobQueue &queue, bool keep_alive);

// DELETE /v1/jobs/{id}
// Cancels a job.  Returns 204 on success; 404 if not found.
Response handle_job_cancel(const Request &req, const std::string &job_id,
                            JobQueue &queue, bool keep_alive);

} // namespace Server
} // namespace Slic3r
