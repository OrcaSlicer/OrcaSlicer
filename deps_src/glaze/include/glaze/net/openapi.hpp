// Glaze Library - For the license information refer to glaze.hpp
#pragma once

#include <unordered_map>

#include "glaze/json.hpp"
#include "glaze/json/schema.hpp"
#include "glaze/net/http_router.hpp"

namespace glz
{
   struct openapi_info
   {
      std::string title = "API";
      std::string version = "1.0.0";
      std::optional<std::string> description{};
   };

   struct openapi_schema_ref
   {
      std::string ref{}; // Reference to schema like "#/components/schemas/TypeName"
   };

   struct openapi_media_type
   {
      std::optional<detail::schematic> schema{};
      std::optional<std::string> example{};
   };

   struct openapi_request_body
   {
      std::optional<std::string> description{};
      std::unordered_map<std::string, openapi_media_type> content{};
      bool required = false;
   };

   struct openapi_parameter
   {
      std::string name{};
      std::string in{}; // "path", "query", "header", or "cookie"
      std::optional<std::string> description{};
      bool required = false;
      std::optional<detail::schematic> schema{};
   };

   struct openapi_response
   {
      std::string description{};
      std::optional<std::unordered_map<std::string, openapi_media_type>> content{};
   };

   struct openapi_components
   {
      std::optional<std::unordered_map<std::string, detail::schematic>> schemas{};
   };

   struct openapi_operation
   {
      std::optional<std::vector<std::string>> tags{};
      std::optional<std::string> summary{};
      std::optional<std::string> operationId{};
      std::optional<std::vector<openapi_parameter>> parameters{};
      std::optional<openapi_request_body> requestBody{};
      std::unordered_map<std::string, openapi_response> responses{};
   };

   struct openapi_path_item
   {
      std::optional<openapi_operation> get{};
      std::optional<openapi_operation> put{};
      std::optional<openapi_operation> post{};
      std::optional<openapi_operation> del{}; // Mapped to "delete" in JSON
      std::optional<openapi_operation> patch{};
   };

   struct open_api
   {
      std::string openapi = "3.0.3";
      openapi_info info{};
      std::optional<openapi_components> components{};
      std::unordered_map<std::string, openapi_path_item> paths{};
   };
}

template <>
struct glz::meta<glz::openapi_path_item>
{
   using T = glz::openapi_path_item;
   static constexpr auto value = glz::object(&T::get, &T::put, &T::post, "delete", &T::del, &T::patch);
};
