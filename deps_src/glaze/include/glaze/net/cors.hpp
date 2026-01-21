// CORS Support for Glaze Library
// Add this to a new file: glaze/net/cors.hpp

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "glaze/net/http_router.hpp"

namespace glz
{
   /**
    * @brief Configuration for CORS (Cross-Origin Resource Sharing) support
    */
   struct cors_config
   {
      /**
       * @brief List of allowed origins
       *
       * Use "*" to allow all origins, or specify specific origins like "https://example.com"
       * For credentials to work, you cannot use "*" - must specify exact origins
       */
      std::vector<std::string> allowed_origins = {"*"};

      /**
       * @brief Optional callback to dynamically validate origins
       */
      std::function<bool(std::string_view)> allowed_origins_validator{};

      /**
       * @brief List of allowed HTTP methods
       */
      std::vector<std::string> allowed_methods = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};

      /**
       * @brief List of allowed request headers
       */
      std::vector<std::string> allowed_headers = {"Content-Type", "Authorization", "X-Requested-With"};

      /**
       * @brief List of headers to expose to the client
       */
      std::vector<std::string> exposed_headers = {};

      /**
       * @brief Whether to allow credentials (cookies, authorization headers)
       *
       * When true, allowed_origins cannot contain "*"
       */
      bool allow_credentials = false;

      /**
       * @brief Maximum age for preflight cache (in seconds)
       *
       * Default is 24 hours (86400 seconds)
       */
      int max_age = 86400;

      /**
       * @brief Whether to automatically handle preflight OPTIONS requests
       *
       * When true, the middleware will respond to OPTIONS requests with appropriate headers
       */
      bool handle_preflight = true;

      /**
       * @brief HTTP status code to return for successful OPTIONS preflight responses
       */
      int options_success_status = 204;
   };

   /**
    * @brief Check if an origin is allowed based on the CORS configuration
    *
    * @param config The CORS configuration
    * @param origin The origin to check
    * @return true if the origin is allowed, false otherwise
    */
   inline bool is_origin_allowed(const cors_config& config, std::string_view origin)
   {
      if (origin.empty()) {
         return false;
      }

      if (config.allowed_origins_validator) {
         try {
            if (config.allowed_origins_validator(origin)) {
               return true;
            }
         }
         catch (...) {
            // If the validator throws, treat as not allowed.
         }
      }

      // If no specific origins are configured or "*" is present, allow all
      if ((config.allowed_origins.empty() && !config.allowed_origins_validator) ||
          std::find(config.allowed_origins.begin(), config.allowed_origins.end(), "*") !=
             config.allowed_origins.end()) {
         return true;
      }

      // Check if the origin is in the allowed list
      return std::find(config.allowed_origins.begin(), config.allowed_origins.end(), origin) !=
             config.allowed_origins.end();
   }

   /**
    * @brief Join a vector of strings with a delimiter
    *
    * @param vec The vector of strings to join
    * @param delimiter The delimiter to use
    * @return The joined string
    */
   inline std::string join_strings(const std::vector<std::string>& vec, const std::string& delimiter)
   {
      if (vec.empty()) return "";

      std::string result = vec[0];
      for (size_t i = 1; i < vec.size(); ++i) {
         result += delimiter + vec[i];
      }
      return result;
   }

   /**
    * @brief Create a CORS middleware handler
    *
    * @param config The CORS configuration to use
    * @return A middleware handler function
    */
   inline handler create_cors_middleware(const cors_config& config = {})
   {
      return [config](const request& req, response& res) {
         // Get the origin from the request headers
         std::string origin;
         auto origin_it = req.headers.find("origin");
         if (origin_it != req.headers.end()) {
            origin = origin_it->second;
         }

         // Check if this is a preflight request (OPTIONS method with specific headers)
         bool is_preflight = (req.method == http_method::OPTIONS) &&
                             (req.headers.find("access-control-request-method") != req.headers.end());

         if (is_preflight && config.handle_preflight) {
            // Origin not allowed, but it's a preflight request - respond with 403
            res.status(403).body("CORS: Origin not allowed");
         }

         // Always add CORS headers if origin is allowed
         if (!origin.empty() && is_origin_allowed(config, origin)) {
            // Determine which origin to send back
            std::string allowed_origin = "*";
            const bool contains_wildcard = std::find(config.allowed_origins.begin(), config.allowed_origins.end(),
                                                     "*") != config.allowed_origins.end();
            if (config.allow_credentials || !contains_wildcard || config.allowed_origins_validator) {
               allowed_origin = origin;
            }

            res.header("Access-Control-Allow-Origin", allowed_origin);

            // Add credentials header if enabled
            if (config.allow_credentials) {
               res.header("Access-Control-Allow-Credentials", "true");
            }

            // Add exposed headers if any
            if (!config.exposed_headers.empty()) {
               res.header("Access-Control-Expose-Headers", join_strings(config.exposed_headers, ", "));
            }

            // Handle preflight requests
            if (is_preflight && config.handle_preflight) {
               // Add allowed methods
               res.header("Access-Control-Allow-Methods", join_strings(config.allowed_methods, ", "));

               // Add allowed headers
               if (!config.allowed_headers.empty()) {
                  res.header("Access-Control-Allow-Headers", join_strings(config.allowed_headers, ", "));
               }
               else if (auto requested_headers = req.headers.find("access-control-request-headers");
                        requested_headers != req.headers.end()) {
                  res.header("Access-Control-Allow-Headers", requested_headers->second);
               }

               // Add max age
               res.header("Access-Control-Max-Age", std::to_string(config.max_age));

               // Set status to 204 (No Content) for preflight response
               res.status(config.options_success_status);
            }
         }
      };
   }

   /**
    * @brief Simple CORS middleware with default configuration
    *
    * Allows all origins, methods, and headers - suitable for development
    */
   inline handler simple_cors() { return create_cors_middleware(); }

   /**
    * @brief Restrictive CORS middleware
    *
    * Only allows specific origins - suitable for production
    *
    * @param origins Vector of allowed origins
    * @param allow_credentials Whether to allow credentials
    * @return CORS middleware handler
    */
   inline handler restrictive_cors(const std::vector<std::string>& origins, bool allow_credentials = false)
   {
      cors_config config;
      config.allowed_origins = origins;
      config.allow_credentials = allow_credentials;
      return create_cors_middleware(config);
   }
}
