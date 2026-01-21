// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/glaze.hpp"
#include "glaze/rpc/repe/buffer.hpp"
#include "glaze/rpc/repe/repe.hpp"

namespace glz
{
   namespace detail
   {
      static constexpr std::string_view empty_path = "";

      // Single shared thread_local buffer for error messages (safe for synchronous operations)
      inline std::string& error_buffer()
      {
         thread_local std::string buffer;
         buffer.clear();
         return buffer;
      }

      inline std::string_view build_registry_error(std::string_view query, std::string_view what)
      {
         auto& buf = error_buffer();
         buf = "registry error for `";
         buf.append(query);
         buf.append("`: ");
         buf.append(what);
         return buf;
      }

      inline std::string_view build_invalid_query_error(std::string_view query)
      {
         auto& buf = error_buffer();
         buf = "invalid_query: ";
         buf.append(query);
         return buf;
      }

      inline std::string_view build_version_error(uint8_t version)
      {
         auto& buf = error_buffer();
         buf = "REPE version mismatch: expected 1, got ";
         const auto n = buf.size();
         buf.resize(n + 8);
         auto* end = glz::to_chars(buf.data() + n, uint32_t(version));
         buf.resize(size_t(end - buf.data()));
         return buf;
      }

      inline std::string_view build_length_error(uint64_t expected, uint64_t actual)
      {
         auto& buf = error_buffer();
         buf = "REPE length mismatch: expected ";
         auto n = buf.size();
         buf.resize(n + 24);
         auto* end = glz::to_chars(buf.data() + n, expected);
         buf.resize(size_t(end - buf.data()));
         buf.append(", got ");
         n = buf.size();
         buf.resize(n + 24);
         end = glz::to_chars(buf.data() + n, actual);
         buf.resize(size_t(end - buf.data()));
         return buf;
      }

      inline std::string_view build_magic_error(uint16_t spec)
      {
         auto& buf = error_buffer();
         buf = "REPE magic number mismatch: expected 0x1507, got 0x";
         constexpr char hex_chars[] = "0123456789abcdef";
         char hex[4];
         hex[0] = hex_chars[(spec >> 12) & 0xF];
         hex[1] = hex_chars[(spec >> 8) & 0xF];
         hex[2] = hex_chars[(spec >> 4) & 0xF];
         hex[3] = hex_chars[spec & 0xF];
         buf.append(hex, 4);
         return buf;
      }
   }

   // Forward declaration of implementation template
   template <auto Opts, uint32_t Protocol>
   struct registry_impl;
}

// Include implementation files
#include "glaze/net/rest_registry_impl.hpp"
#include "glaze/rpc/jsonrpc_registry_impl.hpp"
#include "glaze/rpc/repe/repe_registry_impl.hpp"

namespace glz
{
   // This registry does not support adding methods from RPC calls or adding methods once RPC calls can be made.
   template <auto Opts = opts{}, uint32_t Proto = REPE>
   struct registry
   {
      // procedure for REPE protocol (zero-copy state_view)
      using procedure = std::function<void(repe::state_view&)>; // RPC method

      static constexpr auto protocol = Proto;

      typename protocol_storage<Proto>::type endpoints{};

      void clear() { endpoints.clear(); }

     private:
      // Helper to register all members of a type without registering the root endpoint
      template <const std::string_view& root, class T, const std::string_view& parent>
         requires(glaze_object_t<T> || reflectable<T>)
      void register_members(T& value)
      {
         using namespace glz::detail;
         static constexpr auto N = reflect<T>::size;

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T> && requires { to_tie(value); }) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         using impl = registry_impl<Opts, Proto>;

         for_each<N>([&]<auto I>() {
            decltype(auto) func = [&]() -> decltype(auto) {
               if constexpr (reflectable<T>) {
                  return get_member(value, get<I>(t));
               }
               else {
                  return get_member(value, get<I>(reflect<T>::values));
               }
            }();

            static constexpr auto key = reflect<T>::keys[I];

            static constexpr std::string_view full_key = [&] {
               if constexpr (parent == detail::empty_path) {
                  return join_v<chars<"/">, key>;
               }
               else {
                  return join_v<parent, chars<"/">, key>;
               }
            }();

            // This logic chain should match glz::cli_menu
            using Func = decltype(func);
            if constexpr (std::is_invocable_v<Func>) {
               using Result = std::decay_t<std::invoke_result_t<Func>>;
               impl::template register_function_endpoint<Func, Result>(full_key, func, *this);
            }
            else if constexpr (is_invocable_concrete<std::remove_cvref_t<Func>>) {
               using Tuple = invocable_args_t<std::remove_cvref_t<Func>>;
               constexpr auto N = glz::tuple_size_v<Tuple>;
               static_assert(N == 1, "Only one input is allowed for your function");

               using Params = glz::tuple_element_t<0, Tuple>;

               impl::template register_param_function_endpoint<Func, Params>(full_key, func, *this);
            }
            else if constexpr (is_function_ptr_invocable<std::remove_cvref_t<Func>>) {
               // Handle function pointers with arguments (e.g., void(*)(int))
               using Tuple = function_ptr_args_t<std::remove_cvref_t<Func>>;
               constexpr auto N = glz::tuple_size_v<Tuple>;
               static_assert(N == 1, "Only one input is allowed for your function pointer");

               using Params = glz::tuple_element_t<0, Tuple>;

               impl::template register_param_function_endpoint<Func, Params>(full_key, func, *this);
            }
            else if constexpr (std::is_pointer_v<std::remove_cvref_t<Func>> &&
                               (glaze_object_t<std::remove_pointer_t<std::remove_cvref_t<Func>>> ||
                                reflectable<std::remove_pointer_t<std::remove_cvref_t<Func>>>)) {
               // Handle pointer members explicitly for RPC traversal
               if (func) { // Only traverse if pointer is valid
                  on<root, std::remove_pointer_t<std::remove_cvref_t<Func>>, full_key>(*func);
                  impl::template register_object_endpoint<std::remove_pointer_t<std::remove_cvref_t<Func>>>(
                     full_key, *func, *this);
               }
               // else: skip registration for null pointers - no endpoints created
            }
            else if constexpr (glaze_object_t<std::remove_cvref_t<Func>> || reflectable<std::remove_cvref_t<Func>>) {
               on<root, std::remove_cvref_t<Func>, full_key>(func);

               impl::template register_object_endpoint<std::remove_cvref_t<Func>>(full_key, func, *this);
            }
            else if constexpr (not std::is_lvalue_reference_v<Func>) {
               // For glz::custom, glz::manage, etc.
               impl::template register_value_endpoint<std::remove_cvref_t<Func>>(full_key, func, *this);
            }
            else {
               static_assert(std::is_lvalue_reference_v<Func>);

               if constexpr (std::is_member_function_pointer_v<std::decay_t<Func>>) {
                  using F = std::decay_t<Func>;
                  using Ret = typename return_type<F>::type;
                  using Tuple = typename inputs_as_tuple<F>::type;
                  constexpr auto n_args = glz::tuple_size_v<Tuple>;
                  if constexpr (std::is_void_v<Ret>) {
                     if constexpr (n_args == 0) {
                        impl::template register_member_function_endpoint<T, F, void>(full_key, value, func, *this);
                     }
                     else if constexpr (n_args == 1) {
                        using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                        impl::template register_member_function_with_params_endpoint<T, F, Input, void>(full_key, value,
                                                                                                        func, *this);
                     }
                     else {
                        static_assert(false_v<Func>, "function cannot have more than one input");
                     }
                  }
                  else {
                     // Member function pointers
                     if constexpr (n_args == 0) {
                        impl::template register_member_function_endpoint<T, F, Ret>(full_key, value, func, *this);
                     }
                     else if constexpr (n_args == 1) {
                        using Input = std::decay_t<glz::tuple_element_t<0, Tuple>>;
                        impl::template register_member_function_with_params_endpoint<T, F, Input, Ret>(full_key, value,
                                                                                                       func, *this);
                     }
                     else {
                        static_assert(false_v<Func>, "function cannot have more than one input");
                     }
                  }
               }
               else {
                  // this is a variable and not a function, so we build RPC read/write calls
                  // We can't remove const here, because const fields need to be able to be written
                  impl::template register_variable_endpoint<std::remove_reference_t<Func>>(full_key, func, *this);
               }
            }
         });
      }

     public:
      // Register a C++ type that stores pointers to the value, so be sure to keep the registered value alive
      template <const std::string_view& root = detail::empty_path, class T, const std::string_view& parent = root>
         requires(glaze_object_t<T> || reflectable<T>)
      void on(T& value)
      {
         using impl = registry_impl<Opts, Proto>;

         if constexpr (parent == root && (glaze_object_t<T> || reflectable<T>)) {
            impl::register_endpoint(root, value, *this);
         }

         register_members<root, T, parent>(value);
      }

      // Register multiple C++ types merged together, allowing the root endpoint to return a combined view
      template <const std::string_view& root = detail::empty_path, class... Ts>
         requires(sizeof...(Ts) > 0 && (... && (glaze_object_t<Ts> || reflectable<Ts>)))
      void on(glz::merge<Ts...>& merged)
      {
         using impl = registry_impl<Opts, Proto>;

         // Register root endpoint with the merged object - this handles ""
         impl::register_merge_endpoint(root, merged, *this);

         // Register each merged object's member paths
         for_each<sizeof...(Ts)>([&]<size_t I>() {
            auto& obj = glz::get<I>(merged.value);
            using T = std::decay_t<decltype(obj)>;
            register_members<root, T, root>(obj);
         });
      }

      /// Message-based call for REPE protocol (deprecated)
      /// @deprecated Use the zero-copy span-based overload instead:
      ///             `call(std::span<const char>, std::string&)`
      template <class In = repe::message, class Out = repe::message>
         requires(Proto == REPE)
      [[deprecated("Use call(std::span<const char>, std::string&) for zero-copy performance")]]
      void call(In&& in, Out&& out)
      {
         auto write_error = [&](std::string_view body) {
            out.body = body;
            out.header.body_length = body.size();
            out.header.body_format = repe::body_format::UTF8; // Error messages are UTF-8
            out.header.length = sizeof(repe::header) + out.query.size() + out.body.size();
         };

         // REPE Header Validation

         // Version validation - REPE spec requires version 1
         if (in.header.version != 1) {
            out.header.ec = error_code::version_mismatch;
            out.header.id = in.header.id; // Echo back the original ID
            write_error(detail::build_version_error(in.header.version));
            return;
         }

         // Length validation - REPE spec requires length = 48 + query_length + body_length
         const uint64_t expected_length = sizeof(repe::header) + in.header.query_length + in.header.body_length;
         if (in.header.length != expected_length) {
            out.header.ec = error_code::invalid_header;
            out.header.id = in.header.id; // Echo back the original ID
            write_error(detail::build_length_error(expected_length, in.header.length));
            return;
         }

         // Magic number validation - REPE spec requires 0x1507
         if (in.header.spec != 0x1507) {
            out.header.ec = error_code::invalid_header;
            out.header.id = in.header.id; // Echo back the original ID
            write_error(detail::build_magic_error(in.header.spec));
            return;
         }

         if (auto it = endpoints.find(in.query); it != endpoints.end()) {
            if (bool(in.header.ec)) {
               out = in;
            }
            else {
               // Create view into input message
               repe::request_view req_view{};
               req_view.hdr = in.header;
               req_view.query = in.query;
               req_view.body = in.body;

               // Response builder writes directly to output message (no intermediate buffer)
               repe::response_builder resp{out};
               resp.reset(req_view);
               repe::state_view state{req_view, resp};

               try {
                  it->second(state);
               }
               catch (const std::exception& e) {
                  resp.reset(req_view);
                  resp.set_error(error_code::parse_error, detail::build_registry_error(in.query, e.what()));
               }
            }
         }
         else {
            out.header.ec = error_code::method_not_found;
            out.header.id = in.header.id; // Preserve the ID from the input message
            write_error(detail::build_invalid_query_error(in.query));
         }
      }

      /// Span-based call for zero-copy processing (REPE protocol only)
      /// Request is parsed in-place (query/body are views into the buffer).
      /// Response is written directly to response_buffer.
      /// @param request Raw REPE message bytes
      /// @param response_buffer Buffer for response (will be resized, empty if no response)
      void call(std::span<const char> request, std::string& response_buffer)
         requires(Proto == REPE)
      {
         response_buffer.clear(); // Empty buffer means no response
         repe::response_builder resp{response_buffer};

         // Parse request with zero-copy (header copied to stack, query/body are views)
         auto result = repe::parse_request(request);
         if (!result) {
            // Use the specific error code from parse_result
            // Note: parse_request copies the header before validation, so we can access it
            resp.reset(result.request.hdr.id);

            // Build appropriate error message based on error code
            if (result.ec == error_code::version_mismatch) {
               resp.set_error(result.ec, detail::build_version_error(result.request.hdr.version));
            }
            else if (result.ec == error_code::invalid_header) {
               // Could be magic mismatch, length mismatch, or buffer too small
               if (request.size() >= sizeof(repe::header)) {
                  const auto& hdr = result.request.hdr;
                  if (hdr.spec != repe::repe_magic) {
                     resp.set_error(result.ec, detail::build_magic_error(hdr.spec));
                  }
                  else {
                     // Length mismatch
                     const uint64_t expected = sizeof(repe::header) + hdr.query_length + hdr.body_length;
                     resp.set_error(result.ec, detail::build_length_error(expected, hdr.length));
                  }
               }
               else {
                  resp.set_error(result.ec, "Invalid header");
               }
            }
            else {
               resp.set_error(result.ec, "Failed to parse request");
            }
            return;
         }

         const auto& req = result.request;

         // Look up endpoint (transparent comparison avoids allocation)
         auto it = endpoints.find(req.query);
         if (it == endpoints.end()) {
            if (req.is_notify()) {
               return; // Silent ignore for unknown notifications (buffer stays empty)
            }
            resp.reset(req);
            resp.set_error(error_code::method_not_found, detail::build_invalid_query_error(req.query));
            return;
         }

         // If request has an error, just echo it back
         if (bool(req.hdr.ec)) {
            resp.reset(req);
            resp.set_error(req.hdr.ec);
            return;
         }

         // Zero-copy call: state_view references the parsed request and response builder directly
         repe::state_view state{req, resp};

         try {
            it->second(state);
         }
         catch (const std::exception& e) {
            resp.reset(req);
            resp.set_error(error_code::parse_error, detail::build_registry_error(req.query, e.what()));
            return;
         }
         catch (...) {
            resp.reset(req);
            resp.set_error(error_code::parse_error, "Unknown error");
            return;
         }

         // For notifications, response buffer stays empty (no response sent)
      }

      // Function to call methods - only available for JSONRPC protocol
      // Returns a JSON RPC response string
      // Supports single requests, batch requests, and notifications
      std::string call(std::string_view json_request)
         requires(Proto == JSONRPC)
      {
         // Find first non-whitespace to determine batch vs single request
         auto it = std::find_if(json_request.begin(), json_request.end(),
                                [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });

         if (it != json_request.end() && *it == '[') {
            // Batch request
            auto batch_requests = glz::read_json<std::vector<glz::raw_json_view>>(json_request);
            if (!batch_requests) {
               return R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error","data":)" +
                      write_json(format_error(batch_requests.error(), json_request)).value_or("null") +
                      R"(},"id":null})";
            }
            if (batch_requests->empty()) {
               return R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid Request","data":"Empty batch"},"id":null})";
            }
            return process_batch(*batch_requests);
         }

         // Single request
         auto response = process_single_request(json_request);
         return response.value_or("");
      }

     private:
      // Process a single JSON RPC request, returns nullopt for notifications
      std::optional<std::string> process_single_request(std::string_view json_request)
         requires(Proto == JSONRPC)
      {
         auto request = glz::read_json<rpc::generic_request_t>(json_request);
         if (!request.has_value()) {
            // Check if it's a JSON syntax error vs schema error
            if (glz::validate_json(json_request)) {
               // JSON is syntactically invalid - return Parse error (-32700)
               return R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error","data":)" +
                      write_json(format_error(request.error(), json_request)).value_or("null") + R"(},"id":null})";
            }
            // Valid JSON but invalid request structure - return Invalid Request (-32600)
            auto id = glz::get_as_json<rpc::id_t, "/id">(json_request);
            std::string id_json = id.has_value() ? glz::write_json(id.value()).value_or("null") : "null";
            return R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid Request","data":)" +
                   write_json(format_error(request.error(), json_request)).value_or("null") + R"(},"id":)" + id_json +
                   "}";
         }

         auto& req = request.value();

         // Validate version
         if (req.version != rpc::supported_version) {
            std::string id_json = glz::write_json(req.id).value_or("null");
            return R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid Request","data":)" +
                   write_json("Invalid version: " + std::string(req.version)).value_or("null") + R"(},"id":)" +
                   id_json + "}";
         }

         // Check if this is a notification (id is null)
         bool is_notification = std::holds_alternative<glz::generic::null_t>(req.id);

         // Look up the endpoint - try direct lookup first (handles methods that already start with /)
         auto it = endpoints.find(req.method);
         if (it == endpoints.end()) {
            if (!req.method.empty()) {
               // Try with leading slash using stack buffer for common case
               char buf[256];
               if (req.method.size() < sizeof(buf) - 1) {
                  buf[0] = '/';
                  std::memcpy(buf + 1, req.method.data(), req.method.size());
                  it = endpoints.find(std::string_view{buf, req.method.size() + 1});
               }
               else {
                  // Fallback for very long method names
                  std::string method_path = "/";
                  method_path += req.method;
                  it = endpoints.find(method_path);
               }
            }
            else {
               // Empty method - try root endpoint
               it = endpoints.find("");
            }
            if (it == endpoints.end()) {
               if (is_notification) {
                  return std::nullopt; // No response for notifications
               }
               std::string id_json = glz::write_json(req.id).value_or("null");
               return R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found","data":)" +
                      write_json(req.method).value_or("null") + R"(},"id":)" + id_json + "}";
            }
         }

         // Prepare state
         std::string response;
         bool has_params = !req.params.str.empty() && req.params.str != "null";
         jsonrpc::state state{req.id, response, is_notification, has_params, req.params.str};

         try {
            it->second(std::move(state));
         }
         catch (const std::exception& e) {
            if (is_notification) {
               return std::nullopt;
            }
            std::string id_json = glz::write_json(req.id).value_or("null");
            return R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Internal error","data":)" +
                   write_json(std::string_view{e.what()}).value_or("null") + R"(},"id":)" + id_json + "}";
         }

         if (is_notification) {
            return std::nullopt;
         }

         return response;
      }

      // Process batch requests
      std::string process_batch(const std::vector<glz::raw_json_view>& batch)
         requires(Proto == JSONRPC)
      {
         std::vector<std::string> responses;
         responses.reserve(batch.size());

         size_t total_size = 2; // []
         for (const auto& req : batch) {
            auto response = process_single_request(req.str);
            if (response.has_value()) {
               total_size += response->size() + 1; // +1 for comma
               responses.push_back(std::move(*response));
            }
         }

         // If all were notifications, return empty string
         if (responses.empty()) {
            return "";
         }

         // Build batch response array with pre-reserved capacity
         std::string result;
         result.reserve(total_size);
         result = "[";
         for (size_t i = 0; i < responses.size(); ++i) {
            if (i > 0) {
               result += ",";
            }
            result += responses[i];
         }
         result += "]";
         return result;
      }
   };
}
