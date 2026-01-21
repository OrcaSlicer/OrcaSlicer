// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <charconv>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// To deconflict Windows.h
#ifdef DELETE
#undef DELETE
#endif

namespace glz
{
   enum struct http_method { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS };

   namespace detail
   {
      inline std::string_view http_status_reason_phrase(int status_code)
      {
         switch (status_code) {
         case 100:
            return "Continue";
         case 101:
            return "Switching Protocols";
         case 102:
            return "Processing";
         case 103:
            return "Early Hints";
         case 200:
            return "OK";
         case 201:
            return "Created";
         case 202:
            return "Accepted";
         case 203:
            return "Non-Authoritative Information";
         case 204:
            return "No Content";
         case 205:
            return "Reset Content";
         case 206:
            return "Partial Content";
         case 207:
            return "Multi-Status";
         case 208:
            return "Already Reported";
         case 226:
            return "IM Used";
         case 300:
            return "Multiple Choices";
         case 301:
            return "Moved Permanently";
         case 302:
            return "Found";
         case 303:
            return "See Other";
         case 304:
            return "Not Modified";
         case 305:
            return "Use Proxy";
         case 307:
            return "Temporary Redirect";
         case 308:
            return "Permanent Redirect";
         case 400:
            return "Bad Request";
         case 401:
            return "Unauthorized";
         case 402:
            return "Payment Required";
         case 403:
            return "Forbidden";
         case 404:
            return "Not Found";
         case 405:
            return "Method Not Allowed";
         case 406:
            return "Not Acceptable";
         case 407:
            return "Proxy Authentication Required";
         case 408:
            return "Request Timeout";
         case 409:
            return "Conflict";
         case 410:
            return "Gone";
         case 411:
            return "Length Required";
         case 412:
            return "Precondition Failed";
         case 413:
            return "Payload Too Large";
         case 414:
            return "URI Too Long";
         case 415:
            return "Unsupported Media Type";
         case 416:
            return "Range Not Satisfiable";
         case 417:
            return "Expectation Failed";
         case 418:
            return "I'm a teapot";
         case 421:
            return "Misdirected Request";
         case 422:
            return "Unprocessable Content";
         case 423:
            return "Locked";
         case 424:
            return "Failed Dependency";
         case 425:
            return "Too Early";
         case 426:
            return "Upgrade Required";
         case 428:
            return "Precondition Required";
         case 429:
            return "Too Many Requests";
         case 431:
            return "Request Header Fields Too Large";
         case 451:
            return "Unavailable For Legal Reasons";
         case 500:
            return "Internal Server Error";
         case 501:
            return "Not Implemented";
         case 502:
            return "Bad Gateway";
         case 503:
            return "Service Unavailable";
         case 504:
            return "Gateway Timeout";
         case 505:
            return "HTTP Version Not Supported";
         case 506:
            return "Variant Also Negotiates";
         case 507:
            return "Insufficient Storage";
         case 508:
            return "Loop Detected";
         case 510:
            return "Not Extended";
         case 511:
            return "Network Authentication Required";
         default:
            return "";
         }
      }

      class http_status_category_impl final : public std::error_category
      {
        public:
         const char* name() const noexcept override { return "glz.http_status"; }

         std::string message(int condition) const override
         {
            if (condition < 100 || condition > 599) {
               return "Invalid HTTP status (" + std::to_string(condition) + ")";
            }

            std::string_view phrase = http_status_reason_phrase(condition);
            if (!phrase.empty()) {
               return "HTTP status " + std::to_string(condition) + " (" + std::string(phrase) + ")";
            }

            return "HTTP status " + std::to_string(condition);
         }
      };
   } // namespace detail

   inline const std::error_category& http_status_category()
   {
      static detail::http_status_category_impl instance;
      return instance;
   }

   inline std::error_code make_http_status_error(int status_code) { return {status_code, http_status_category()}; }

   inline std::optional<int> http_status_from(std::error_code ec)
   {
      if (ec.category() == http_status_category()) {
         return ec.value();
      }
      return {};
   }

   // Utility functions for HTTP methods
   inline std::string_view to_string(http_method method)
   {
      using enum http_method;
      switch (method) {
      case GET:
         return "GET";
      case POST:
         return "POST";
      case PUT:
         return "PUT";
      case DELETE:
         return "DELETE";
      case PATCH:
         return "PATCH";
      case HEAD:
         return "HEAD";
      case OPTIONS:
         return "OPTIONS";
      default:
         return "UNKNOWN";
      }
   }

   inline std::optional<http_method> from_string(std::string_view method)
   {
      using enum http_method;
      if (method == "GET") return GET;
      if (method == "POST") return POST;
      if (method == "PUT") return PUT;
      if (method == "DELETE") return DELETE;
      if (method == "PATCH") return PATCH;
      if (method == "HEAD") return HEAD;
      if (method == "OPTIONS") return OPTIONS;
      return std::nullopt;
   }

   struct HttpStatusLine
   {
      std::string_view version{};
      int status_code{};
      std::string_view status_message{};
   };

   inline std::expected<HttpStatusLine, std::error_code> parse_http_status_line(std::string_view status_line)
   {
      if (status_line.empty()) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      constexpr std::string_view http_prefix = "HTTP/";
      // Check if status line starts with HTTP/
      if (status_line.size() <= http_prefix.size() || !status_line.starts_with(http_prefix)) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      // Find the positions of the spaces that separate the components
      const size_t first_space = status_line.find(' ', http_prefix.length());
      if (first_space == std::string_view::npos || first_space >= status_line.size() - 1) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      const size_t second_space = status_line.find(' ', first_space + 1);
      const bool has_status_message = second_space != std::string_view::npos;

      // Extract the components as views instead of creating new strings
      std::string_view version = status_line.substr(http_prefix.length(), first_space - http_prefix.length());

      std::string_view status_code_str = status_line.substr(
         first_space + 1, has_status_message ? second_space - first_space - 1 : std::string_view::npos);

      std::string_view status_message;
      if (has_status_message && second_space < status_line.length() - 1) {
         status_message = status_line.substr(second_space + 1);

         // Trim leading and trailing whitespace (adapted for string_view)
         const auto start = std::find_if(status_message.begin(), status_message.end(),
                                         [](unsigned char ch) { return !std::isspace(ch); });

         if (start == status_message.end()) {
            // String is all whitespace
            status_message = std::string_view();
         }
         else {
            const auto end = std::find_if(status_message.rbegin(), status_message.rend(), [](unsigned char ch) {
                                return !std::isspace(ch);
                             }).base();
            status_message = status_message.substr(start - status_message.begin(), end - start);
         }
      }

      // Validate HTTP version (must be digits.digits)
      const size_t dot_pos = version.find('.');
      if (dot_pos == std::string_view::npos || dot_pos == 0 || dot_pos == version.length() - 1) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      // Check that characters before and after dot are digits
      const bool valid_version =
         std::all_of(version.begin(), version.begin() + dot_pos, [](unsigned char ch) { return std::isdigit(ch); }) &&
         std::all_of(version.begin() + dot_pos + 1, version.end(), [](unsigned char ch) { return std::isdigit(ch); });

      if (!valid_version) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      // Validate status code (must be all digits and not too long)
      constexpr size_t max_status_code_length = 3; // HTTP status codes are 3 digits
      if (status_code_str.empty() || status_code_str.length() > max_status_code_length ||
          !std::all_of(status_code_str.begin(), status_code_str.end(),
                       [](unsigned char ch) { return std::isdigit(ch); })) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      // Parse status code using from_chars for better error handling and performance
      int status_code;
      const auto [ptr, ec] =
         std::from_chars(status_code_str.data(), status_code_str.data() + status_code_str.size(), status_code);

      if (ec != std::errc() || status_code < 100 || status_code > 599) {
         return std::unexpected(std::make_error_code(std::errc::protocol_error));
      }

      // Create and return the parsed status line structure
      // No copying of strings is performed here as we're using views into the original string
      return HttpStatusLine{.version = version, .status_code = status_code, .status_message = status_message};
   }
}

// Include URL utilities for convenience (can also be included directly via glaze/net/url.hpp)
#include "glaze/net/url.hpp"
