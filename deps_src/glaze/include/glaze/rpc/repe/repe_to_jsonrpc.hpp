// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/ext/jsonrpc.hpp"
#include "glaze/glaze.hpp"
#include "glaze/rpc/repe/header.hpp"

namespace glz::repe
{
   // Convert REPE error codes to JSON-RPC error codes
   inline rpc::error_e repe_error_to_jsonrpc(error_code ec)
   {
      switch (ec) {
      case error_code::none:
         return rpc::error_e::no_error;
      case error_code::parse_error:
      case error_code::syntax_error:
         return rpc::error_e::parse_error;
      case error_code::invalid_header:
      case error_code::version_mismatch:
         return rpc::error_e::invalid_request;
      case error_code::method_not_found:
         return rpc::error_e::method_not_found;
      default:
         // Map other errors to internal error
         return rpc::error_e::internal;
      }
   }

   // Convert JSON-RPC error codes to REPE error codes
   inline error_code jsonrpc_error_to_repe(rpc::error_e err)
   {
      switch (err) {
      case rpc::error_e::no_error:
         return error_code::none;
      case rpc::error_e::parse_error:
         return error_code::parse_error;
      case rpc::error_e::invalid_request:
         return error_code::invalid_header;
      case rpc::error_e::method_not_found:
         return error_code::method_not_found;
      case rpc::error_e::invalid_params:
      case rpc::error_e::internal:
      case rpc::error_e::server_error_lower:
      case rpc::error_e::server_error_upper:
      default:
         return error_code::parse_error;
      }
   }

   // Convert REPE message to JSON-RPC request
   // The query should be a method name (with or without leading slash)
   // The body should be JSON formatted params
   inline std::string to_jsonrpc_request(const message& msg)
   {
      if (msg.header.body_format != body_format::JSON) {
         return R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid request","data":"REPE body must be JSON format"},"id":null})";
      }

      // Extract method name from query (remove leading slash if present)
      std::string_view method = msg.query;
      if (!method.empty() && method[0] == '/') {
         method = method.substr(1);
      }

      // Build JSON-RPC request
      std::string result = R"({"jsonrpc":"2.0","method":")";
      result += method;
      result += R"(","params":)";

      // Add params from body (or empty object if no body)
      if (!msg.body.empty()) {
         result += msg.body;
      }
      else {
         result += "{}";
      }

      // Add id if not a notification
      if (!msg.header.notify) {
         result += R"(,"id":)";
         result += std::to_string(msg.header.id);
      }
      else {
         result += R"(,"id":null)";
      }

      result += "}";
      return result;
   }

   // Convert REPE message to JSON-RPC response
   inline std::string to_jsonrpc_response(const message& msg)
   {
      if (msg.header.body_format != body_format::JSON && msg.header.body_format != body_format::UTF8) {
         return R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Internal error","data":"REPE body must be JSON or UTF8 format"},"id":null})";
      }

      std::string result = R"({"jsonrpc":"2.0",)";

      // Check if there's an error
      if (bool(msg.header.ec)) {
         auto jsonrpc_error = repe_error_to_jsonrpc(msg.header.ec);
         result += R"("error":{"code":)";
         result += std::to_string(static_cast<int>(jsonrpc_error));
         result += R"(,"message":")";
         result += rpc::code_as_sv(jsonrpc_error);
         result += R"(")";

         // Add error data if available
         if (!msg.body.empty()) {
            result += R"(,"data":")";
            // Escape the body content for JSON string
            for (char c : msg.body) {
               if (c == '"')
                  result += "\\\"";
               else if (c == '\\')
                  result += "\\\\";
               else if (c == '\n')
                  result += "\\n";
               else if (c == '\r')
                  result += "\\r";
               else if (c == '\t')
                  result += "\\t";
               else
                  result += c;
            }
            result += R"(")";
         }

         result += "}";
      }
      else {
         // Success response with result
         result += R"("result":)";
         if (!msg.body.empty()) {
            result += msg.body;
         }
         else {
            result += "null";
         }
      }

      // Add id
      result += R"(,"id":)";
      result += std::to_string(msg.header.id);
      result += "}";

      return result;
   }

   // Convert JSON-RPC request to REPE message
   inline expected<message, std::string> from_jsonrpc_request(std::string_view json_request)
   {
      auto req = read_json<rpc::generic_request_t>(json_request);
      if (!req) {
         return unexpected<std::string>("Failed to parse JSON-RPC request");
      }

      message msg{};

      // Set query as method name with leading slash
      msg.query = "/";
      msg.query += req->method;

      // Set body as params (if not empty)
      if (!req->params.str.empty() && req->params.str != "null") {
         msg.body = std::string(req->params.str);
      }

      // Set id
      if (std::holds_alternative<std::int64_t>(req->id)) {
         msg.header.id = static_cast<uint64_t>(std::get<std::int64_t>(req->id));
         msg.header.notify = false;
      }
      else if (std::holds_alternative<glz::generic::null_t>(req->id)) {
         msg.header.notify = true;
      }
      else if (std::holds_alternative<std::string_view>(req->id)) {
         // Try to parse string as number, otherwise use hash
         auto sv = std::get<std::string_view>(req->id);
         try {
            msg.header.id = std::stoull(std::string(sv));
         }
         catch (...) {
            // Use hash of string as id
            msg.header.id = std::hash<std::string_view>{}(sv);
         }
         msg.header.notify = false;
      }

      // Set format fields
      msg.header.query_format = query_format::JSON_POINTER;
      msg.header.body_format = body_format::JSON;
      msg.header.query_length = msg.query.size();
      msg.header.body_length = msg.body.size();
      msg.header.length = sizeof(header) + msg.query.size() + msg.body.size();
      msg.header.version = 1;
      msg.header.spec = 0x1507;

      return msg;
   }

   // Convert JSON-RPC response to REPE message
   inline expected<message, std::string> from_jsonrpc_response(std::string_view json_response)
   {
      auto resp = read_json<rpc::generic_response_t>(json_response);
      if (!resp) {
         return unexpected<std::string>("Failed to parse JSON-RPC response");
      }

      message msg{};

      // Set id
      if (std::holds_alternative<std::int64_t>(resp->id)) {
         msg.header.id = static_cast<uint64_t>(std::get<std::int64_t>(resp->id));
      }
      else if (std::holds_alternative<std::string_view>(resp->id)) {
         // Try to parse string as number, otherwise use hash
         auto sv = std::get<std::string_view>(resp->id);
         try {
            msg.header.id = std::stoull(std::string(sv));
         }
         catch (...) {
            // Use hash of string as id
            msg.header.id = std::hash<std::string_view>{}(sv);
         }
      }

      // Check for error
      if (resp->error.has_value()) {
         msg.header.ec = jsonrpc_error_to_repe(resp->error->code);
         msg.header.body_format = body_format::UTF8;

         // Combine error message and data
         if (resp->error->data.has_value()) {
            msg.body = resp->error->data.value();
         }
         else {
            msg.body = resp->error->message;
         }
      }
      else if (resp->result.has_value()) {
         msg.header.body_format = body_format::JSON;
         msg.body = std::string(resp->result->str);
      }

      // Set format fields
      msg.header.query_format = query_format::JSON_POINTER;
      msg.header.query_length = msg.query.size();
      msg.header.body_length = msg.body.size();
      msg.header.length = sizeof(header) + msg.query.size() + msg.body.size();
      msg.header.version = 1;
      msg.header.spec = 0x1507;

      return msg;
   }
}
