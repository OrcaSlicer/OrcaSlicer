// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "glaze/json/generic.hpp"
#include "glaze/net/http.hpp"
#include "glaze/net/url.hpp"
#include "glaze/util/key_transformers.hpp"

namespace glz
{
   // Request context object
   struct request
   {
      http_method method{};
      std::string target{}; // Full request target (path + query string)
      std::string path{}; // Path component only (without query string)
      std::unordered_map<std::string, std::string> params{}; // Path parameters (e.g., :id)
      std::unordered_map<std::string, std::string> query{}; // Query parameters (e.g., ?limit=10)
      std::unordered_map<std::string, std::string> headers{};
      std::string body{};
      std::string remote_ip{};
      uint16_t remote_port{};
   };

   // Response builder
   struct response
   {
      int status_code = 200;
      std::unordered_map<std::string, std::string> response_headers{};
      std::string response_body{};

      inline response& status(int code)
      {
         status_code = code;
         return *this;
      }

      inline response& header(std::string_view name, std::string_view value)
      {
         // Convert header name to lowercase for case-insensitive lookups (RFC 7230)
         response_headers[to_lower_case(name)] = std::string(value);
         return *this;
      }

      inline response& body(std::string_view content)
      {
         response_body = std::string(content);
         return *this;
      }

      // Use glz::opts for format deduction and serialization
      // my_response.res<Opts>(value);
      template <auto Opts, class T>
      response& body(T&& value)
      {
         if constexpr (Opts.format == JSON) {
            content_type("application/json");
         }
         else if constexpr (Opts.format == BEVE) {
            content_type("application/beve");
         }
         auto ec = glz::write<Opts>(std::forward<T>(value), response_body);
         if (ec) {
            // TODO serialize a struct into proper format for errors
            response_body = R"({"error":"glz::write_json error"})"; // rare that this would ever happen
         }
         return *this;
      }

      inline response& content_type(std::string_view type) { return header("content-type", type); }

      // JSON response helper using Glaze
      template <class T = glz::generic>
      response& json(T&& value)
      {
         content_type("application/json");
         auto ec = glz::write_json(std::forward<T>(value), response_body);
         if (ec) {
            response_body = R"({"error":"glz::write_json error"})"; // rare that this would ever happen
         }
         return *this;
      }
   };

   using handler = std::function<void(const request&, response&)>;
   using async_handler = std::function<std::future<void>(const request&, response&)>;
   using error_handler = std::function<void(std::error_code, std::source_location)>;

   /**
    * @brief Parameter constraint for route validation
    *
    * Defines validation rules for route parameters using a validation function.
    */
   struct param_constraint
   {
      /**
       * @brief Human-readable description of the constraint
       *
       * Used for error reporting and debugging.
       */
      std::string description{};

      /**
       * @brief Validation function for parameter values
       *
       * This function is used to validate parameter values. It should return true if the parameter value is valid,
       * false otherwise.
       */
      std::function<bool(std::string_view)> validation = [](std::string_view) { return true; };
   };

   /**
    * @brief An entry for a registered route.
    */
   struct route_spec
   {
      std::string description{};
      std::vector<std::string> tags{};
      std::unordered_map<std::string, param_constraint> constraints{};

      // Type information for schema generation
      std::optional<std::string> request_body_schema{};
      std::optional<std::string> response_schema{};
      std::optional<std::string> request_body_type_name{};
      std::optional<std::string> response_type_name{};
   };

   /**
    * @brief HTTP router based on a radix tree for efficient path matching
    *
    * @tparam Handler The type of the handler that gets invoked upon a route match.
    *                 Must be invocable with (const request&, response&).
    *                 Note: Middleware registered via use() must also be this type.
    *
    * The basic_http_router class provides fast route matching for HTTP requests using a radix tree
    * data structure. It supports static routes, parameterized routes (e.g., "/users/:id"),
    * wildcard routes, and parameter validation via constraints.
    *
    * Note: http_server::mount() only accepts the default http_router (basic_http_router<>).
    * Custom handler routers are intended for standalone use or with custom server implementations.
    */
   template <class Handler = std::function<void(const request&, response&)>>
      requires std::invocable<Handler, const request&, response&>
   struct basic_http_router
   {
      /**
       * @brief Function type for request handlers
       *
       * Handlers are called when a route matches the incoming request.
       */
      using handler = Handler;

      /**
       * @brief A compile-time boolean indicating whether asynchronous request handlers are enabled.
       *
       * Async methods wrap the async handler in a capturing lambda and store it as a regular handler.
       * This requires Handler to be constructible from such a wrapper. std::function and similar
       * type-erasing wrappers support this; function pointers and fixed-signature handlers do not.
       *
       * Note: This heuristic checks constructibility from std::function as a proxy for "can accept
       * a type-erased callable." A Handler constructible from lambdas but not std::function would
       * incorrectly get is_async_enabled = false, but this is an unusual corner case.
       */
      static constexpr bool is_async_enabled =
         std::is_constructible_v<Handler, std::function<void(const request&, response&)>>;

      /**
       * @brief Function type for asynchronous request handlers
       *
       * Async handlers return a std::future that completes when the request is processed.
       * The async methods (get_async, post_async, etc.) wrap these by calling .get() on
       * the returned future, converting them to synchronous handlers.
       *
       * Note: This type is fixed to std::function<std::future<void>...>. Users who need
       * custom async patterns (coroutines, different future types, callbacks) should use
       * their async-capable type directly as the Handler template parameter instead of
       * using the built-in async methods.
       *
       * Async methods are only available when is_async_enabled is true.
       */
      using async_handler = std::function<std::future<void>(const request&, response&)>;

      /**
       * @brief An entry for a registered route.
       */
      struct route_entry
      {
         handler handle{};
         route_spec spec{};
      };

      /**
       * @brief Node in the radix tree routing structure
       *
       * Each node represents a segment of a path, which can be a static string,
       * a parameter (prefixed with ":"), or a wildcard (prefixed with "*").
       */
      struct RadixNode
      {
         /**
          * @brief The path segment this node represents
          */
         std::string segment;

         /**
          * @brief Whether this node represents a parameter (e.g., ":id")
          */
         bool is_parameter = false;

         /**
          * @brief Whether this node represents a wildcard (e.g., "*action")
          */
         bool is_wildcard = false;

         /**
          * @brief Name of the parameter (if is_parameter or is_wildcard is true)
          */
         std::string parameter_name;

         /**
          * @brief Map of static child nodes indexed by segment
          */
         std::unordered_map<std::string, std::unique_ptr<RadixNode>> static_children;

         /**
          * @brief Parameter child node (only one parameter child per node is allowed)
          */
         std::unique_ptr<RadixNode> parameter_child;

         /**
          * @brief Wildcard child node (only one wildcard child per node is allowed)
          */
         std::unique_ptr<RadixNode> wildcard_child;

         /**
          * @brief Map of handlers for different HTTP methods
          *
          * Only present if this node represents an endpoint.
          */
         std::unordered_map<http_method, handler> handlers;

         /**
          * @brief Map of parameter constraints for different HTTP methods
          */
         std::unordered_map<http_method, std::unordered_map<std::string, param_constraint>> constraints;

         /**
          * @brief Whether this node represents an endpoint (route handler)
          */
         bool is_endpoint = false;

         /**
          * @brief Full path to this node (for debugging and conflict detection)
          */
         std::string full_path;

         /**
          * @brief Return a human-readable representation of this node
          *
          * @return String representation of the node
          */
         std::string to_string() const
         {
            std::string result;
            result.reserve(80 + segment.size() + full_path.size());

            result.append("Node[");
            result.append(is_parameter ? "PARAM:" : (is_wildcard ? "WILD:" : ""));
            result.append(segment);
            result.append(", endpoint=");
            result.append(is_endpoint ? "true" : "false");
            result.append(", children=");
            result.append(std::to_string(static_children.size()));
            result.append(parameter_child ? "+param" : "");
            result.append(wildcard_child ? "+wild" : "");
            result.append(", full_path=");
            result.append(full_path);
            result.append("]");
            return result;
         }
      };

      /**
       * @brief Default constructor
       */
      basic_http_router() = default;

      /**
       * @brief Match a value against a pattern with advanced pattern matching features
       *
       * Supports:
       * - Wildcards (*) for matching any number of characters
       * - Question marks (?) for matching a single character
       * - Character classes ([a-z], [^0-9])
       * - Anchors (^ for start of string, $ for end of string)
       * - Escape sequences with backslash
       *
       * @param value The string to match
       * @param pattern The pattern to match against
       * @return true if the value matches the pattern, false otherwise
       */
      static bool match_pattern(std::string_view value, std::string_view pattern)
      {
         enum struct State { Literal, Escape, CharClass };

         if (pattern.empty()) return true; // Empty pattern matches anything

         size_t v_pos = 0;
         size_t p_pos = 0;

         // For backtracking when we encounter *
         std::optional<size_t> backtrack_pattern;
         std::optional<size_t> backtrack_value;

         // For character classes
         State state = State::Literal;
         bool negate_class = false;
         bool char_class_match = false;

         while (v_pos < value.size() || p_pos < pattern.size()) {
            // Pattern exhausted but value remains
            if (p_pos >= pattern.size()) {
               // Can we backtrack for wildcard?
               if (backtrack_pattern) {
                  p_pos = *backtrack_pattern;
                  v_pos = ++(*backtrack_value);
                  continue;
               }
               return false;
            }

            // Value exhausted but pattern remains
            if (v_pos >= value.size()) {
               // If remaining pattern is just * wildcard, it's a match
               if (p_pos < pattern.size() && pattern[p_pos] == '*' && p_pos == pattern.size() - 1) return true;

               // Can we backtrack?
               if (backtrack_pattern) {
                  p_pos = *backtrack_pattern;
                  v_pos = ++(*backtrack_value);
                  continue;
               }
               return false;
            }

            switch (state) {
            case State::Literal:
               if (pattern[p_pos] == '\\') {
                  // Escape sequence
                  state = State::Escape;
                  p_pos++;
                  continue;
               }
               else if (pattern[p_pos] == '[') {
                  // Begin character class
                  state = State::CharClass;
                  char_class_match = false;
                  p_pos++;

                  // Check for negation
                  if (p_pos < pattern.size() && pattern[p_pos] == '^') {
                     negate_class = true;
                     p_pos++;
                  }
                  else {
                     negate_class = false;
                  }
                  continue;
               }
               else if (pattern[p_pos] == '*') {
                  // Wildcard - match zero or more chars
                  backtrack_pattern = p_pos;
                  backtrack_value = v_pos;
                  p_pos++;
                  continue;
               }
               else if (pattern[p_pos] == '?') {
                  // Match any single character
                  p_pos++;
                  v_pos++;
                  continue;
               }
               else if (pattern[p_pos] == '^' && p_pos == 0) {
                  // Beginning of string anchor
                  p_pos++;
                  continue;
               }
               else if (pattern[p_pos] == '$' && p_pos == pattern.size() - 1) {
                  // End of string anchor - only matches if we're at the end
                  return v_pos == value.size();
               }
               else {
                  // Literal character
                  if (pattern[p_pos] != value[v_pos]) {
                     // Can we backtrack?
                     if (backtrack_pattern) {
                        p_pos = *backtrack_pattern;
                        v_pos = ++(*backtrack_value);
                        continue;
                     }
                     return false;
                  }
                  p_pos++;
                  v_pos++;
               }
               break;

            case State::Escape:
               // Escaped character - match literally
               if (p_pos >= pattern.size() || pattern[p_pos] != value[v_pos]) {
                  // Can we backtrack?
                  if (backtrack_pattern) {
                     p_pos = *backtrack_pattern;
                     v_pos = ++(*backtrack_value);
                     state = State::Literal;
                     continue;
                  }
                  return false;
               }
               p_pos++;
               v_pos++;
               state = State::Literal;
               break;

            case State::CharClass:
               if (pattern[p_pos] == ']') {
                  // End of character class
                  p_pos++;
                  if (negate_class) {
                     if (char_class_match) {
                        // Negated and found a match = fail
                        return false;
                     }
                     v_pos++; // Character not in class
                  }
                  else {
                     if (!char_class_match) {
                        // Not negated and no match = fail
                        return false;
                     }
                     v_pos++; // Character in class
                  }
                  state = State::Literal;
                  continue;
               }
               else if (p_pos + 2 < pattern.size() && pattern[p_pos + 1] == '-') {
                  // Character range
                  char start = pattern[p_pos];
                  char end = pattern[p_pos + 2];
                  if (value[v_pos] >= start && value[v_pos] <= end) {
                     char_class_match = true;
                  }
                  p_pos += 3; // Skip the range
               }
               else {
                  // Single character in class
                  if (pattern[p_pos] == value[v_pos]) {
                     char_class_match = true;
                  }
                  p_pos++;
               }
               break;
            }
         }

         return v_pos == value.size() && p_pos == pattern.size();
      }

      /**
       * @brief Split a path into segments
       *
       * Splits a path like "/users/:id/profile" into ["users", ":id", "profile"]
       *
       * @param path The path to split
       * @return Vector of path segments
       */
      static std::vector<std::string> split_path(std::string_view path)
      {
         std::vector<std::string> segments;
         segments.reserve(std::count(path.begin(), path.end(), '/') + 1);

         size_t start = 0;
         while (start < path.size()) {
            if (path[start] == '/') {
               start++;
               continue;
            }

            size_t end = path.find('/', start);
            if (end == std::string::npos) end = path.size();

            segments.push_back(std::string(path.substr(start, end - start)));
            start = end;
         }

         return segments;
      }

      /**
       * @brief Register a route with the router
       *
       * @param method The HTTP method (GET, POST, etc.)
       * @param path The route path, can include parameters (":param") and wildcards ("*param")
       * @param handle The handler function to call when this route matches
       * @param spec Optional spec for the route.
       * @return Reference to this router for method chaining
       * @throws std::runtime_error if there's a route conflict
       */
      inline basic_http_router& route(http_method method, std::string_view path, handler handle,
                                      const route_spec& spec = {})
      {
         std::string path_str(path);
         try {
            // Store in the routes map
            auto& entry = routes[path_str][method];
            entry.handle = std::move(handle);
            entry.spec = spec;

            // Also add to the radix tree
            add_route(method, path, entry.handle, spec.constraints);
         }
         catch (const std::exception& e) {
            // Log the error instead of propagating it
            std::fprintf(stderr, "Error adding route '%.*s': %s\n", static_cast<int>(path.length()), path.data(),
                         e.what());
         }
         return *this;
      }

      /**
       * @brief Register a GET route
       */
      inline basic_http_router& get(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return route(http_method::GET, path, std::move(handle), spec);
      }

      /**
       * @brief Register a POST route
       */
      inline basic_http_router& post(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return route(http_method::POST, path, std::move(handle), spec);
      }

      /**
       * @brief Register a PUT route
       */
      inline basic_http_router& put(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return route(http_method::PUT, path, std::move(handle), spec);
      }

      /**
       * @brief Register a DELETE route
       */
      inline basic_http_router& del(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return route(http_method::DELETE, path, std::move(handle), spec);
      }

      /**
       * @brief Register a PATCH route
       */
      inline basic_http_router& patch(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return route(http_method::PATCH, path, std::move(handle), spec);
      }

      /**
       * @brief Register an asynchronous route
       */
      inline basic_http_router& route_async(http_method method, std::string_view path, async_handler handle,
                                            const route_spec& spec = {})
         requires is_async_enabled
      {
         // Convert async handle to sync handle (capture by move for efficiency)
         return route(
            method, path,
            [handle = std::move(handle)](const request& req, response& res) {
               // Create a future and get the result, which will propagate any exceptions
               auto future = handle(req, res);
               future.get(); // This will throw if the async operation threw
            },
            spec);
      }

      /**
       * @brief Register an asynchronous GET route
       */
      inline basic_http_router& get_async(std::string_view path, async_handler handle, const route_spec& spec = {})
         requires is_async_enabled
      {
         return route_async(http_method::GET, path, std::move(handle), spec);
      }

      /**
       * @brief Register an asynchronous POST route
       */
      inline basic_http_router& post_async(std::string_view path, async_handler handle, const route_spec& spec = {})
         requires is_async_enabled
      {
         return route_async(http_method::POST, path, std::move(handle), spec);
      }

      /**
       * @brief Register an asynchronous PUT route
       */
      inline basic_http_router& put_async(std::string_view path, async_handler handle, const route_spec& spec = {})
         requires is_async_enabled
      {
         return route_async(http_method::PUT, path, std::move(handle), spec);
      }

      /**
       * @brief Register an asynchronous DELETE route
       */
      inline basic_http_router& del_async(std::string_view path, async_handler handle, const route_spec& spec = {})
         requires is_async_enabled
      {
         return route_async(http_method::DELETE, path, std::move(handle), spec);
      }

      /**
       * @brief Register an asynchronous PATCH route
       */
      inline basic_http_router& patch_async(std::string_view path, async_handler handle, const route_spec& spec = {})
         requires is_async_enabled
      {
         return route_async(http_method::PATCH, path, std::move(handle), spec);
      }

      /**
       * @brief Register middleware to be executed before route handlers
       *
       * Middleware functions are executed in the order they are registered.
       *
       * @param middleware The middleware function
       * @return Reference to this router for method chaining
       */
      inline basic_http_router& use(handler middleware)
      {
         middlewares.push_back(std::move(middleware));
         return *this;
      }

      /**
       * @brief Match a request against registered routes
       *
       * @param method The HTTP method of the request
       * @param target The target path of the request (may include query string)
       * @return A pair containing the matched handler and extracted parameters
       */
      inline std::pair<handler, std::unordered_map<std::string, std::string>> match(http_method method,
                                                                                    const std::string& target) const
      {
         std::unordered_map<std::string, std::string> params;
         handler result = nullptr;

         // Strip query string from target for matching
         const auto [path, query_string] = split_target(target);

         // First try direct lookup for non-parameterized routes (optimization)
         auto direct_it = direct_routes.find(std::string(path));
         if (direct_it != direct_routes.end()) {
            auto method_it = direct_it->second.find(method);
            if (method_it != direct_it->second.end()) {
               return {method_it->second, params}; // No params for direct match
            }
         }

         // Split the target path into segments
         std::vector<std::string> segments = split_path(path);

         // Use recursive matching function
         match_node(&root, segments, 0, method, params, result);

         return {result, params};
      }

      /**
       * @brief Print the entire router tree structure for debugging
       */
      void print_tree() const
      {
         std::cout << "Radix Tree Structure:\n";
         print_node(&root, 0);
      }

      /**
       * @brief Map of routes registered with this router
       *
       * Used for compatibility with mount functionality.
       */
      std::unordered_map<std::string, std::unordered_map<http_method, route_entry>> routes;

      /**
       * @brief Vector of middleware handlers
       */
      std::vector<handler> middlewares;

     private:
      /**
       * @brief Root node of the radix tree
       */
      mutable RadixNode root;

      /**
       * @brief Direct lookup table for non-parameterized routes (optimization)
       */
      std::unordered_map<std::string, std::unordered_map<http_method, handler>> direct_routes;

      /**
       * @brief Add a route to the radix tree
       *
       * @param method HTTP method for the route
       * @param path Path pattern for the route
       * @param handle Handler function for the route
       * @param constraints Optional parameter constraints
       * @throws std::runtime_error if there's a route conflict
       */
      void add_route(http_method method, std::string_view path, handler handle,
                     const std::unordered_map<std::string, param_constraint>& constraints = {})
      {
         std::string path_str(path);

         // Optimization: for non-parameterized routes, store them directly
         if (path_str.find(':') == std::string::npos && path_str.find('*') == std::string::npos) {
            // Check for conflicts first
            auto& method_handlers = direct_routes[path_str];
            if (method_handlers.find(method) != method_handlers.end()) {
               throw std::runtime_error("Route conflict: handler already exists for " + std::string(to_string(method)) +
                                        " " + path_str);
            }

            // Store the route directly
            method_handlers[method] = handle;
            return;
         }

         // For parameterized routes, use the radix tree
         std::vector<std::string> segments = split_path(path);

         // Start at the root node
         RadixNode* current = &root;

         // Build the path through the tree
         for (size_t i = 0; i < segments.size(); ++i) {
            const std::string& segment = segments[i];

            if (segment.empty()) continue;

            if (segment[0] == ':') {
               // Parameter segment
               std::string param_name = segment.substr(1);

               if (!current->parameter_child) {
                  current->parameter_child = std::make_unique<RadixNode>();
                  current->parameter_child->is_parameter = true;
                  current->parameter_child->parameter_name = param_name;
                  current->parameter_child->segment = segment;

                  // Build the full path for debugging/conflict detection
                  current->parameter_child->full_path = current->full_path + "/" + segment;
               }
               else if (current->parameter_child->parameter_name != param_name) {
                  // Parameter name conflict
                  throw std::runtime_error("Route conflict: different parameter names at same position: :" +
                                           current->parameter_child->parameter_name + " vs :" + param_name);
               }

               current = current->parameter_child.get();
            }
            else if (segment[0] == '*') {
               // Wildcard segment
               std::string wildcard_name = segment.substr(1);

               // Wildcards must be at the end of the route
               if (i != segments.size() - 1) {
                  throw std::runtime_error("Wildcard must be the last segment in route: " + path_str);
               }

               if (!current->wildcard_child) {
                  current->wildcard_child = std::make_unique<RadixNode>();
                  current->wildcard_child->is_wildcard = true;
                  current->wildcard_child->parameter_name = wildcard_name;
                  current->wildcard_child->segment = segment;

                  // Build the full path for debugging/conflict detection
                  current->wildcard_child->full_path = current->full_path + "/" + segment;
               }
               else if (current->wildcard_child->parameter_name != wildcard_name) {
                  // Wildcard name conflict
                  throw std::runtime_error("Route conflict: different wildcard names at same position: *" +
                                           current->wildcard_child->parameter_name + " vs *" + wildcard_name);
               }

               current = current->wildcard_child.get();
               break; // Wildcard is always the last segment
            }
            else {
               // Static segment
               if (current->static_children.find(segment) == current->static_children.end()) {
                  current->static_children[segment] = std::make_unique<RadixNode>();
                  current->static_children[segment]->segment = segment;

                  // Build the full path for debugging/conflict detection
                  current->static_children[segment]->full_path = current->full_path + "/" + segment;
               }

               current = current->static_children[segment].get();
            }
         }

         // Check for route conflict
         if (current->is_endpoint && current->handlers.find(method) != current->handlers.end()) {
            throw std::runtime_error("Route conflict: handler already exists for " + std::string(to_string(method)) +
                                     " " + path_str);
         }

         // Mark as endpoint and set handler
         current->is_endpoint = true;
         current->handlers[method] = handle;

         // Store constraints (if any)
         if (!constraints.empty()) {
            current->constraints[method] = constraints;
         }
      }

      /**
       * @brief Match a path against the radix tree
       *
       * Recursive function to match a path against the tree and extract parameters.
       *
       * @param node Current node in the tree
       * @param segments Path segments to match
       * @param index Current segment index
       * @param method HTTP method to match
       * @param params Map to store extracted parameters
       * @param result Handler if a match is found
       * @return true if a match was found, false otherwise
       */
      bool match_node(RadixNode* node, const std::vector<std::string>& segments, size_t index, http_method method,
                      std::unordered_map<std::string, std::string>& params, handler& result) const
      {
         // End of path
         if (index == segments.size()) {
            if (node->is_endpoint) {
               auto it = node->handlers.find(method);
               if (it != node->handlers.end()) {
                  // Validate constraints if any before setting the handler
                  bool constraints_passed = true;
                  auto constraints_it = node->constraints.find(method);
                  if (constraints_it != node->constraints.end()) {
                     for (const auto& [param_name, constraint] : constraints_it->second) {
                        auto param_it = params.find(param_name);
                        if (param_it != params.end()) {
                           const std::string& value = param_it->second;

                           // Use the validation function
                           if (!constraint.validation(value)) {
                              constraints_passed = false;
                              break; // Validation failed
                           }
                        }
                     }
                  }

                  if (constraints_passed) {
                     // Only set the handler if constraints pass
                     result = it->second;
                     return true;
                  }
                  return false;
               }
            }
            return false;
         }

         const std::string& segment = segments[index];

         // Try static match first (most specific)
         auto static_it = node->static_children.find(segment);
         if (static_it != node->static_children.end()) {
            if (match_node(static_it->second.get(), segments, index + 1, method, params, result)) {
               return true;
            }
         }

         // Try parameter match (less specific than static)
         if (node->parameter_child) {
            // Save parameter value (URL-decoded)
            std::string param_name = node->parameter_child->parameter_name;
            params[param_name] = url_decode(segment);

            if (match_node(node->parameter_child.get(), segments, index + 1, method, params, result)) {
               return true;
            }

            // If this parameter didn't lead to a match, remove it
            params.erase(param_name);
         }

         // Try wildcard match (least specific)
         if (node->wildcard_child) {
            // For wildcards, capture all remaining segments (URL-decoded)
            std::string full_capture;
            for (size_t i = index; i < segments.size(); i++) {
               if (i > index) full_capture += "/";
               full_capture += url_decode(segments[i]);
            }

            params[node->wildcard_child->parameter_name] = full_capture;

            // Wildcards always go to the endpoint of their node
            if (node->wildcard_child->is_endpoint) {
               auto it = node->wildcard_child->handlers.find(method);
               if (it != node->wildcard_child->handlers.end()) {
                  // Validate constraints for the wildcard before setting the handler
                  bool constraints_passed = true;
                  auto constraints_it = node->wildcard_child->constraints.find(method);
                  if (constraints_it != node->wildcard_child->constraints.end()) {
                     for (const auto& [param_name, constraint] : constraints_it->second) {
                        auto param_it = params.find(param_name);
                        if (param_it != params.end()) {
                           const std::string& value = param_it->second;

                           // Use the validation function
                           if (!constraint.validation(value)) {
                              constraints_passed = false;
                              break; // Validation failed
                           }
                        }
                     }
                  }

                  if (constraints_passed) {
                     // Only set the handler if constraints pass
                     result = it->second;
                     return true;
                  }
                  return false;
               }
            }
         }

         return false;
      }

      /**
       * @brief Print a node in the radix tree (for debugging)
       *
       * @param node The node to print
       * @param depth Current depth in the tree (for indentation)
       */
      void print_node(const RadixNode* node, int depth) const
      {
         if (!node) return;

         std::string indent(depth * 2, ' ');
         std::cout << indent << node->to_string() << "\n";

         // Print all handlers for this node
         if (node->is_endpoint) {
            std::cout << indent << "  Handlers: ";
            for (const auto& [method, _] : node->handlers) {
               std::cout << to_string(method) << " ";
            }
            std::cout << "\n";

            // Print constraints if any
            for (const auto& [method, method_constraints] : node->constraints) {
               std::cout << indent << "  Constraints for " << to_string(method) << ":\n";
               for (const auto& [param, constraint] : method_constraints) {
                  std::cout << indent << "    " << param << ": (" << constraint.description << ")\n";
               }
            }
         }

         // Print children
         for (const auto& [segment, child] : node->static_children) {
            print_node(child.get(), depth + 1);
         }

         if (node->parameter_child) {
            print_node(node->parameter_child.get(), depth + 1);
         }

         if (node->wildcard_child) {
            print_node(node->wildcard_child.get(), depth + 1);
         }
      }
   };

   /**
    * @brief Default HTTP router using std::function handlers
    *
    * This is a type alias for backward compatibility. Use basic_http_router<Handler>
    * if you need to customize the handler type for coroutines, different futures
    * implementations, or callback-based architectures.
    */
   using http_router = basic_http_router<>;
}
