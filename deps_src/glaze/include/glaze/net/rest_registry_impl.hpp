// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/opts.hpp" // For REST constant
#include "glaze/glaze.hpp"
#include "glaze/json/schema.hpp"
#include "glaze/net/http_router.hpp"
#include "glaze/rpc/repe/repe.hpp" // For protocol_storage template

namespace glz
{
   // Forward declaration of the registry template
   template <auto Opts, uint32_t Proto>
   struct registry;

   template <>
   struct protocol_storage<REST>
   {
      using type = http_router;
   };

   // Implementation for REST protocol
   template <auto Opts>
   struct registry_impl<Opts, REST>
   {
      using enum http_method;

      // Helper method to convert a JSON pointer path to a REST path
      static std::string convert_to_rest_path(sv json_pointer_path)
      {
         // Make a copy of the path
         std::string rest_path(json_pointer_path);

         // Remove trailing slashes
         if (!rest_path.empty() && rest_path.back() == '/') {
            rest_path.pop_back();
         }

         return rest_path;
      }

      // Helper to generate JSON schema for a type
      template <class T>
      static std::string generate_schema_for_type()
      {
         std::string schema_str;
         auto ec = write_json_schema<T>(schema_str);
         if (ec) {
            return "{}";
         }
         return schema_str;
      }

      template <class T>
      static std::string get_type_name()
      {
         if constexpr (string_t<T>) {
            return "string";
         }
         else if constexpr (num_t<T>) {
            return "number";
         }
         else if constexpr (readable_array_t<T> || tuple_t<T> || is_std_tuple<T>) {
            return "array";
         }
         else if constexpr (boolean_like<T>) {
            return "boolean";
         }
         else if constexpr (glaze_object_t<T> || reflectable<T> || writable_map_t<T>) {
            return "object";
         }
         else {
            return "";
         }
      }

      // Helper to create route spec with type information
      template <class RequestType = void, class ResponseType = void>
      static route_spec create_route_spec_with_types(const std::string& description = "",
                                                     const std::vector<std::string>& tags = {})
      {
         route_spec spec;
         spec.description = description;
         spec.tags = tags;

         if constexpr (!std::same_as<RequestType, void>) {
            spec.request_body_schema = generate_schema_for_type<RequestType>();
            spec.request_body_type_name = get_type_name<RequestType>();
         }

         if constexpr (!std::same_as<ResponseType, void>) {
            spec.response_schema = generate_schema_for_type<ResponseType>();
            spec.response_type_name = get_type_name<ResponseType>();
         }

         return spec;
      }

      template <class T, class RegistryType>
      static void register_endpoint(const sv path, T& value, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         // GET handler for the entire object
         auto get_spec = create_route_spec_with_types<void, T>("Get " + get_type_name<T>(), {"data"});
         reg.endpoints.route(
            GET, rest_path, [&value](const request& /*req*/, response& res) { res.template body<Opts>(value); },
            get_spec);

         // PUT handler for updating the entire object
         auto put_spec = create_route_spec_with_types<T, void>("Update " + get_type_name<T>(), {"data"});
         reg.endpoints.route(
            PUT, rest_path,
            [&value](const request& req, response& res) {
               // Parse the JSON request body
               auto ec = read<Opts>(value, req.body);
               if (ec) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               res.status(204); // No Content
            },
            put_spec);
      }

      template <class Func, class Result, class RegistryType>
      static void register_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto get_spec = create_route_spec_with_types<void, Result>("Get " + get_type_name<Result>(), {"data"});
         // GET handler for functions
         reg.endpoints.route(
            GET, rest_path,
            [&func](const request& /*req*/, response& res) {
               if constexpr (std::same_as<Result, void>) {
                  func();
                  res.status(204); // No Content
               }
               else {
                  auto result = func();
                  res.body<Opts>(result);
               }
            },
            get_spec);
      }

      template <class Func, class Params, class RegistryType>
      static void register_param_function_endpoint(const sv path, Func& func, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         using Result = std::invoke_result_t<decltype(func), Params>;
         auto post_spec = create_route_spec_with_types<Params, Result>("Create " + get_type_name<Result>(), {"data"});
         // POST handler for functions with parameters
         reg.endpoints.route(
            POST, rest_path,
            [&func](const request& req, response& res) {
               // Parse the JSON request body
               Params params_result{};
               auto ec = read<Opts>(params_result, req.body);
               if (bool(ec)) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               if constexpr (std::same_as<Result, void>) {
                  func(std::move(params_result));
                  res.status(204); // No Content
               }
               else {
                  auto result = func(std::move(params_result));
                  res.body<Opts>(result);
               }
            },
            post_spec);
      }

      template <class Obj, class RegistryType>
      static void register_object_endpoint(const sv path, Obj& obj, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto get_spec = create_route_spec_with_types<void, Obj>("Get " + get_type_name<Obj>(), {"data"});
         // GET handler for nested objects
         reg.endpoints.route(
            GET, rest_path, [&obj](const request& /*req*/, response& res) { res.body<Opts>(obj); }, get_spec);

         auto put_spec = create_route_spec_with_types<Obj, void>("Update " + get_type_name<Obj>(), {"data"});
         // PUT handler for updating nested objects
         reg.endpoints.route(
            PUT, rest_path,
            [&obj](const request& req, response& res) {
               // Parse the JSON request body
               auto ec = read<Opts>(obj, req.body);
               if (ec) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               res.status(204); // No Content
            },
            put_spec);
      }

      template <class Value, class RegistryType>
      static void register_value_endpoint(const sv path, Value& value, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto get_spec = create_route_spec_with_types<void, Value>("Get " + get_type_name<Value>(), {"data"});
         // GET handler for values
         reg.endpoints.route(
            GET, rest_path, [&value](const request& /*req*/, response& res) { res.body<Opts>(value); }, get_spec);

         auto put_spec = create_route_spec_with_types<Value, void>("Update " + get_type_name<Value>(), {"data"});
         // PUT handler for updating values
         reg.endpoints.route(
            PUT, rest_path,
            [&value](const request& req, response& res) {
               // Parse the JSON request body
               auto ec = read<Opts>(value, req.body);
               if (ec) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               res.status(204); // No Content
            },
            put_spec);
      }

      template <class Var, class RegistryType>
      static void register_variable_endpoint(const sv path, Var& var, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto get_spec = create_route_spec_with_types<void, Var>("Get " + get_type_name<Var>(), {"data"});
         // GET handler for variables
         reg.endpoints.route(
            GET, rest_path, [&var](const request& /*req*/, response& res) { res.body<Opts>(var); }, get_spec);

         auto put_spec = create_route_spec_with_types<Var, void>("Update " + get_type_name<Var>(), {"data"});
         // PUT handler for updating variables
         reg.endpoints.route(
            PUT, rest_path,
            [&var](const request& req, response& res) {
               // Parse the JSON request body
               auto ec = read<Opts>(var, req.body);
               if (ec) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               res.status(204); // No Content
            },
            put_spec);
      }

      template <class T, class F, class Ret, class RegistryType>
      static void register_member_function_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto get_spec = create_route_spec_with_types<void, Ret>("Get " + get_type_name<Ret>(), {"data"});
         // GET handler for member functions with no args
         reg.endpoints.route(
            GET, rest_path,
            [&value, func](const request& /*req*/, response& res) {
               if constexpr (std::same_as<Ret, void>) {
                  (value.*func)();
                  res.status(204); // No Content
               }
               else {
                  auto result = (value.*func)();
                  res.body<Opts>(result);
               }
            },
            get_spec);
      }

      template <class T, class F, class Input, class Ret, class RegistryType>
      static void register_member_function_with_params_endpoint(const sv path, T& value, F func, RegistryType& reg)
      {
         std::string rest_path = convert_to_rest_path(path);

         auto post_spec = create_route_spec_with_types<Input, Ret>("Create " + get_type_name<Ret>(), {"data"});
         // POST handler for member functions with args
         reg.endpoints.route(
            POST, rest_path,
            [&value, func](const request& req, response& res) {
               // Parse the JSON request body
               Input params_result{};
               auto ec = read<Opts>(params_result, req.body);
               if (bool(ec)) {
                  res.status(400).body("Invalid request body: " + format_error(ec, req.body));
                  return;
               }

               if constexpr (std::same_as<Ret, void>) {
                  (value.*func)(std::move(params_result));
                  res.status(204); // No Content
               }
               else {
                  auto result = (value.*func)(std::move(params_result));
                  res.body<Opts>(result);
               }
            },
            post_spec);
      }
   };
}
