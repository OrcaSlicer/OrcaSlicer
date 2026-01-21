// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <glaze/glaze.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <thread>
#include <unordered_map>
#include <vector>

#include "glaze/net/http_router.hpp"
#include "glaze/util/key_transformers.hpp"

namespace glz
{
   inline int strncasecmp(const char* s1, const char* s2, size_t n)
   {
      for (size_t i = 0; i < n; ++i) {
         unsigned char c1 = static_cast<unsigned char>(s1[i]);
         unsigned char c2 = static_cast<unsigned char>(s2[i]);
         if (c1 == '\0' || c2 == '\0') {
            return c1 - c2;
         }
         int diff = std::tolower(c1) - std::tolower(c2);
         if (diff != 0) {
            return diff;
         }
      }
      return 0;
   }

   // Streaming strategy options
   enum class stream_read_strategy {
      bulk_transfer, // Deliver larger chunks, better throughput (default)
      immediate_delivery // Deliver smaller chunks immediately, lower latency
   };

   struct url_parts
   {
      std::string protocol;
      std::string host;
      uint16_t port;
      std::string path;
   };

   inline std::expected<url_parts, std::error_code> parse_url(std::string_view url)
   {
      // Check minimum length
      if (url.size() < 8) { // Minimum for "http://" + 1 char host
         return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      }

      // Check for control characters to prevent CRLF injection
      for (unsigned char c : url) {
         if (c < 32 || c == 127) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
         }
      }

      // Find protocol
      size_t protocol_end = url.find("://");
      if (protocol_end == std::string_view::npos) {
         return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      }

      std::string protocol(url.substr(0, protocol_end));
      if (protocol != "http" && protocol != "https" && protocol != "ws" && protocol != "wss") {
         return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      }

      // Process host, port and path
      size_t host_start = protocol_end + 3;
      if (host_start >= url.size()) {
         return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      }

      size_t host_end = url.find_first_of("/:", host_start);
      std::string host;
      std::string port_str;
      std::string path = "/";

      if (host_end == std::string_view::npos) {
         host = std::string(url.substr(host_start));
      }
      else if (url[host_end] == ':') {
         host = std::string(url.substr(host_start, host_end - host_start));
         size_t port_start = host_end + 1;
         size_t port_end = url.find('/', port_start);

         if (port_end == std::string_view::npos) {
            port_str = std::string(url.substr(port_start));
         }
         else {
            port_str = std::string(url.substr(port_start, port_end - port_start));
            path = std::string(url.substr(port_end));
         }

         if (!port_str.empty() && !std::all_of(port_str.begin(), port_str.end(), ::isdigit)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
         }
      }
      else if (url[host_end] == '/') {
         host = std::string(url.substr(host_start, host_end - host_start));
         path = std::string(url.substr(host_end));
      }

      if (host.empty() || host.find_first_of(":/") != std::string::npos) {
         return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      }

      uint16_t port = 0;
      if (port_str.empty()) {
         port = (protocol == "https" || protocol == "wss") ? 443 : 80;
      }
      else {
         try {
            long port_long = std::stol(port_str);
            if (port_long <= 0 || port_long > 65535) {
               return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
            port = static_cast<uint16_t>(port_long);
         }
         catch (const std::exception&) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
         }
      }

      return url_parts{std::move(protocol), std::move(host), port, std::move(path)};
   }

   // HTTP connection pool for reusing sockets
   struct http_connection_pool
   {
      struct connection_key
      {
         std::string host;
         uint16_t port;

         bool operator==(const connection_key& other) const { return host == other.host && port == other.port; }
      };

      struct connection_key_hash
      {
         size_t operator()(const connection_key& key) const
         {
            return std::hash<std::string>{}(key.host) ^ (std::hash<uint16_t>{}(key.port) << 1);
         }
      };

      std::mutex mtx;
      std::unordered_map<connection_key, std::vector<std::shared_ptr<asio::ip::tcp::socket>>, connection_key_hash>
         available_connections;
      std::shared_ptr<asio::io_context> io_context;

      http_connection_pool(std::shared_ptr<asio::io_context> ctx) : io_context(ctx) {}

      std::shared_ptr<asio::ip::tcp::socket> get_connection(const std::string& host, uint16_t port)
      {
         connection_key key{host, port};

         {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = available_connections.find(key);
            if (it != available_connections.end() && !it->second.empty()) {
               auto socket = it->second.back();
               it->second.pop_back();

               // Check if socket is still connected
               if (socket && socket->is_open()) {
                  // A simple check might not be enough for stale connections.
                  // A more robust implementation might send a probe or check for readability.
                  // For now, we assume is_open() is sufficient.
                  return socket;
               }
            }
         }

         // Create new connection if none are available or they are closed
         return std::make_shared<asio::ip::tcp::socket>(*io_context);
      }

      void return_connection(const std::string& host, uint16_t port, std::shared_ptr<asio::ip::tcp::socket> socket)
      {
         if (!socket || !socket->is_open()) {
            return;
         }

         connection_key key{host, port};
         std::lock_guard<std::mutex> lock(mtx);

         auto& connections = available_connections[key];
         if (connections.size() < 10) { // Limit pool size per host
            connections.push_back(socket);
         }
         // else, the socket is just closed when the shared_ptr goes out of scope.
      }
   };

   // Forward declaration for streaming connection
   struct http_stream_connection;

   // Handler function types for streaming
   using http_data_handler = std::function<void(std::string_view data)>;
   using http_error_handler =
      std::function<void(std::error_code ec)>; // May carry HTTP statuses via http_status_category()
   using http_connect_handler = std::function<void(const response& headers)>;
   using http_disconnect_handler = std::function<void()>;

   // Streaming HTTP connection handle
   struct http_stream_connection
   {
      std::shared_ptr<asio::ip::tcp::socket> socket;
      std::shared_ptr<asio::steady_timer> timer;
      std::shared_ptr<asio::streambuf> buffer; // Use unified streambuf for all reads
      bool is_connected{false};
      std::atomic<bool> should_stop{false};
      stream_read_strategy strategy{stream_read_strategy::bulk_transfer}; // Default strategy
      std::function<bool(int)> status_is_error{}; // Evaluated before treating status as failure

      // Constructor with optional buffer size limit and strategy
      http_stream_connection(size_t max_buffer_size = 1024 * 1024,
                             stream_read_strategy read_strategy = stream_read_strategy::bulk_transfer)
         : buffer(std::make_shared<asio::streambuf>(max_buffer_size)), strategy(read_strategy)
      {}

      // User-facing disconnect. Signals the internal loops to stop.
      // The actual socket closing/pooling is handled by the internal disconnect handler.
      void disconnect()
      {
         bool expected = false;
         if (should_stop.compare_exchange_strong(expected, true)) {
            if (socket && socket->is_open()) {
               std::error_code ec;
               // This cancels pending async operations on the socket, triggering their handlers
               // with asio::error::operation_aborted.
               socket->cancel(ec);
            }
            if (timer) {
               timer->cancel();
            }
         }
      }

      ~http_stream_connection() { disconnect(); }
   };

   // Stream request parameters struct
   struct stream_request_params
   {
      std::string url;
      http_data_handler on_data;
      http_error_handler on_error;
      std::string method{};
      std::string body{};
      std::unordered_map<std::string, std::string> headers{};
      http_connect_handler on_connect{};
      http_disconnect_handler on_disconnect{};
      std::chrono::seconds timeout{std::chrono::seconds{30}};
      stream_read_strategy strategy{stream_read_strategy::bulk_transfer};
      std::function<bool(int)> status_is_error{}; // Custom predicate to decide whether a status code should fail
   };

   struct http_client
   {
      http_client()
         : async_io_context(std::make_shared<asio::io_context>()),
           connection_pool(std::make_shared<http_connection_pool>(async_io_context))
      {
         start_workers();
      }

      ~http_client() { stop_workers(); }

      // Synchronous GET request - truly synchronous, no promises/futures
      std::expected<response, std::error_code> get(std::string_view url,
                                                   const std::unordered_map<std::string, std::string>& headers = {})
      {
         auto url_result = parse_url(url);
         if (!url_result) {
            return std::unexpected(url_result.error());
         }

         return perform_sync_request("GET", *url_result, "", headers);
      }

      // Synchronous POST request - truly synchronous, no promises/futures
      std::expected<response, std::error_code> post(std::string_view url, const std::string& body,
                                                    const std::unordered_map<std::string, std::string>& headers = {})
      {
         auto url_result = parse_url(url);
         if (!url_result) {
            return std::unexpected(url_result.error());
         }

         return perform_sync_request("POST", *url_result, body, headers);
      }

      // Synchronous PUT request - truly synchronous, no promises/futures
      std::expected<response, std::error_code> put(std::string_view url, const std::string& body,
                                                   const std::unordered_map<std::string, std::string>& headers = {})
      {
         auto url_result = parse_url(url);
         if (!url_result) {
            return std::unexpected(url_result.error());
         }

         return perform_sync_request("PUT", *url_result, body, headers);
      }

      // Synchronous JSON POST request
      template <class T>
      std::expected<response, std::error_code> post_json(
         std::string_view url, const T& data, const std::unordered_map<std::string, std::string>& headers = {})
      {
         std::string json_str;
         auto ec = glz::write_json(data, json_str);
         if (ec) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
         }

         auto merged_headers = headers;
         merged_headers["content-type"] = "application/json";

         return post(url, json_str, merged_headers);
      }

      // Synchronous JSON PUT request
      template <class T>
      std::expected<response, std::error_code> put_json(
         std::string_view url, const T& data, const std::unordered_map<std::string, std::string>& headers = {})
      {
         std::string json_str;
         auto ec = glz::write_json(data, json_str);
         if (ec) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
         }

         auto merged_headers = headers;
         merged_headers["content-type"] = "application/json";

         return put(url, json_str, merged_headers);
      }

      // New unified streaming request method
      std::shared_ptr<http_stream_connection> stream_request(const stream_request_params& params)
      {
         auto url_result = parse_url(params.url);
         if (!url_result) {
            asio::post(*async_io_context,
                       [on_error = params.on_error, error = url_result.error()]() { on_error(error); });
            return nullptr;
         }

         std::string method = params.method.empty() ? "GET" : params.method;

         return perform_stream_request(method, *url_result, params.body, params.headers, params.timeout,
                                       params.strategy, params.status_is_error, params.on_data, params.on_error,
                                       params.on_connect, params.on_disconnect);
      }

      // Asynchronous GET request
      template <typename CompletionHandler>
      void get_async(std::string_view url, const std::unordered_map<std::string, std::string>& headers,
                     CompletionHandler&& handler)
      {
         auto url_result = parse_url(url);
         if (!url_result) {
            asio::post(*async_io_context, [handler = std::forward<CompletionHandler>(handler),
                                           error = url_result.error()] mutable { handler(std::unexpected(error)); });
            return;
         }

         perform_request_async("GET", *url_result, "", headers, std::forward<CompletionHandler>(handler));
      }

      // Overload for get_async without completion handler (returns future)
      std::future<std::expected<response, std::error_code>> get_async(
         std::string_view url, const std::unordered_map<std::string, std::string>& headers = {})
      {
         std::promise<std::expected<response, std::error_code>> promise;
         auto future = promise.get_future();

         get_async(url, headers,
                   [promise = std::move(promise)](std::expected<response, std::error_code> result) mutable {
                      promise.set_value(std::move(result));
                   });

         return future;
      }

      // Asynchronous POST request
      template <typename CompletionHandler>
      void post_async(std::string_view url, const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers, CompletionHandler&& handler)
      {
         auto url_result = parse_url(url);
         if (!url_result) {
            asio::post(*async_io_context, [handler = std::forward<CompletionHandler>(handler),
                                           error = url_result.error()] mutable { handler(std::unexpected(error)); });
            return;
         }

         perform_request_async("POST", *url_result, body, headers, std::forward<CompletionHandler>(handler));
      }

      // Overload for post_async without completion handler (returns future)
      std::future<std::expected<response, std::error_code>> post_async(
         std::string_view url, const std::string& body,
         const std::unordered_map<std::string, std::string>& headers = {})
      {
         std::promise<std::expected<response, std::error_code>> promise;
         auto future = promise.get_future();

         post_async(url, body, headers,
                    [promise = std::move(promise)](std::expected<response, std::error_code> result) mutable {
                       promise.set_value(std::move(result));
                    });

         return future;
      }

      // Async JSON POST request
      template <class T, typename CompletionHandler>
      void post_json_async(std::string_view url, const T& data,
                           const std::unordered_map<std::string, std::string>& headers, CompletionHandler&& handler)
      {
         std::string json_str;
         auto ec = glz::write_json(data, json_str);
         if (ec) {
            asio::post(*async_io_context, [handler = std::forward<CompletionHandler>(handler)]() {
               handler(std::unexpected(std::make_error_code(std::errc::invalid_argument)));
            });
            return;
         }

         auto merged_headers = headers;
         merged_headers["content-type"] = "application/json";

         post_async(url, json_str, merged_headers, std::forward<CompletionHandler>(handler));
      }

      // Overload for post_json_async without completion handler (returns future)
      template <class T>
      std::future<std::expected<response, std::error_code>> post_json_async(
         std::string_view url, const T& data, const std::unordered_map<std::string, std::string>& headers = {})
      {
         std::promise<std::expected<response, std::error_code>> promise;
         auto future = promise.get_future();

         post_json_async(url, data, headers,
                         [promise = std::move(promise)](std::expected<response, std::error_code> result) mutable {
                            promise.set_value(std::move(result));
                         });

         return future;
      }

     private:
      std::shared_ptr<asio::io_context> async_io_context; // For async operations only
      std::shared_ptr<http_connection_pool> connection_pool;
      std::vector<std::thread> worker_threads;
      std::atomic<bool> running{true};

      void start_workers()
      {
         size_t num_threads = std::max(2u, std::thread::hardware_concurrency());
         worker_threads.reserve(num_threads);

         for (size_t i = 0; i < num_threads; ++i) {
            worker_threads.emplace_back([this]() {
               while (running) {
                  try {
                     async_io_context->run();
                     // Reset context to allow it to be run again after being stopped
                     if (async_io_context->stopped()) {
                        async_io_context->restart();
                     }
                  }
                  catch (const std::exception& e) {
                     std::cerr << "HTTP client worker error: " << e.what() << std::endl;
                  }
               }
            });
         }
      }

      void stop_workers()
      {
         running = false;
         async_io_context->stop();

         for (auto& thread : worker_threads) {
            if (thread.joinable()) {
               thread.join();
            }
         }
      }

      std::shared_ptr<http_stream_connection> perform_stream_request(
         const std::string& method, const url_parts& url, const std::string& body,
         const std::unordered_map<std::string, std::string>& headers, std::chrono::seconds timeout,
         stream_read_strategy strategy, std::function<bool(int)> status_is_error, http_data_handler on_data,
         http_error_handler on_error, http_connect_handler on_connect, http_disconnect_handler on_disconnect)
      {
         auto connection = std::make_shared<http_stream_connection>(1024 * 1024, strategy);
         connection->socket = connection_pool->get_connection(url.host, url.port);
         connection->timer = std::make_shared<asio::steady_timer>(*async_io_context);

         connection->status_is_error = std::move(status_is_error);
         if (!connection->status_is_error) {
            connection->status_is_error = [](int status) { return status >= 400; };
         }

         // Wrap the disconnect handler to return the socket to the pool
         auto internal_on_disconnect = [this, user_on_disconnect = std::move(on_disconnect), connection, url]() {
            connection->is_connected = false;
            // Call the user's handler if provided
            if (user_on_disconnect) {
               user_on_disconnect();
            }
            // Return the connection to the pool for reuse
            connection_pool->return_connection(url.host, url.port, connection->socket);
         };

         // Set connection timeout
         connection->timer->expires_after(timeout);
         connection->timer->async_wait([connection, on_error, internal_on_disconnect](std::error_code ec) {
            if (!ec && !connection->is_connected && !connection->should_stop) {
               // Mark for stop to prevent race conditions
               connection->disconnect();
               on_error(std::make_error_code(std::errc::timed_out));
               internal_on_disconnect();
            }
         });

         // Check if the socket is already open
         if (connection->socket->is_open()) {
            // If already connected, skip resolve and connect, and just send the request
            asio::post(*async_io_context, [this, url, method, body, headers, connection, on_data = std::move(on_data),
                                           on_error = std::move(on_error), on_connect = std::move(on_connect),
                                           internal_on_disconnect = std::move(internal_on_disconnect)]() mutable {
               send_stream_request(url, method, body, headers, connection, std::move(on_data), std::move(on_error),
                                   std::move(on_connect), std::move(internal_on_disconnect));
            });
         }
         else {
            // If not connected, resolve and connect as before
            auto resolver = std::make_shared<asio::ip::tcp::resolver>(*async_io_context);
            resolver->async_resolve(
               url.host, std::to_string(url.port),
               [this, url, method, body, headers, connection, resolver, on_data = std::move(on_data),
                on_error = std::move(on_error), on_connect = std::move(on_connect),
                internal_on_disconnect = std::move(internal_on_disconnect)](
                  std::error_code ec, asio::ip::tcp::resolver::results_type results) mutable {
                  if (ec || connection->should_stop) {
                     on_error(ec);
                     internal_on_disconnect(); // Ensure cleanup on resolve error
                     return;
                  }

                  asio::async_connect(*connection->socket, results,
                                      [this, url, method, body, headers, connection, on_data = std::move(on_data),
                                       on_error = std::move(on_error), on_connect = std::move(on_connect),
                                       internal_on_disconnect = std::move(internal_on_disconnect)](
                                         std::error_code ec, const asio::ip::tcp::endpoint&) mutable {
                                         if (ec || connection->should_stop) {
                                            on_error(ec);
                                            internal_on_disconnect(); // Ensure cleanup on connect error
                                            return;
                                         }

                                         send_stream_request(url, method, body, headers, connection, std::move(on_data),
                                                             std::move(on_error), std::move(on_connect),
                                                             std::move(internal_on_disconnect));
                                      });
               });
         }

         return connection;
      }

      // Needs to take the wrapped disconnect handler and use keep-alive
      void send_stream_request(const url_parts& url, const std::string& method, const std::string& body,
                               const std::unordered_map<std::string, std::string>& headers,
                               std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                               http_error_handler on_error, http_connect_handler on_connect,
                               http_disconnect_handler on_disconnect)
      {
         std::string request_str;
         request_str.reserve(512 + body.size());

         request_str.append(method);
         request_str.append(" ");
         request_str.append(url.path);
         request_str.append(" HTTP/1.1\r\n");
         request_str.append("Host: ");
         request_str.append(url.host);
         request_str.append("\r\n");
         // Use keep-alive to allow the connection to be pooled
         request_str.append("Connection: keep-alive\r\n");

         if (!body.empty()) {
            request_str.append("Content-Length: ");
            request_str.append(std::to_string(body.size()));
            request_str.append("\r\n");
         }

         for (const auto& [name, value] : headers) {
            request_str.append(name);
            request_str.append(": ");
            request_str.append(value);
            request_str.append("\r\n");
         }
         request_str.append("\r\n");
         request_str.append(body);

         auto request_buffer = std::make_shared<std::string>(std::move(request_str));

         asio::async_write(*connection->socket, asio::buffer(*request_buffer),
                           [this, connection, request_buffer, on_data = std::move(on_data),
                            on_error = std::move(on_error), on_connect = std::move(on_connect),
                            on_disconnect = std::move(on_disconnect)](std::error_code ec, std::size_t) mutable {
                              if (ec || connection->should_stop) {
                                 on_error(ec);
                                 if (on_disconnect) on_disconnect();
                                 return;
                              }

                              read_stream_response(connection, std::move(on_data), std::move(on_error),
                                                   std::move(on_connect), std::move(on_disconnect));
                           });
      }

      void read_stream_response(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                                http_error_handler on_error, http_connect_handler on_connect,
                                http_disconnect_handler on_disconnect)
      {
         // Use the connection's unified buffer
         asio::async_read_until(
            *connection->socket, *connection->buffer, "\r\n\r\n",
            [this, connection, on_data = std::move(on_data), on_error = std::move(on_error),
             on_connect = std::move(on_connect),
             on_disconnect = std::move(on_disconnect)](std::error_code ec, std::size_t bytes_transferred) mutable {
               if (ec || connection->should_stop) {
                  on_error(ec);
                  if (on_disconnect) on_disconnect();
                  return;
               }

               // Create a zero-copy string_view of the received headers
               std::string_view header_data{static_cast<const char*>(connection->buffer->data().data()),
                                            bytes_transferred};

               // Parse status line
               auto line_end = header_data.find("\r\n");
               if (line_end == std::string_view::npos) {
                  on_error(std::make_error_code(std::errc::protocol_error));
                  if (on_disconnect) on_disconnect();
                  return;
               }
               std::string_view status_line = header_data.substr(0, line_end);
               header_data.remove_prefix(line_end + 2);

               auto parsed_status = parse_http_status_line(status_line);
               if (!parsed_status) {
                  on_error(parsed_status.error());
                  if (on_disconnect) on_disconnect();
                  return;
               }

               response response_headers;
               response_headers.status_code = parsed_status->status_code;

               // Parse header fields
               while (!header_data.starts_with("\r\n")) {
                  line_end = header_data.find("\r\n");
                  if (line_end == std::string_view::npos) {
                     on_error(std::make_error_code(std::errc::protocol_error));
                     if (on_disconnect) on_disconnect();
                     return;
                  }

                  std::string_view header_line = header_data.substr(0, line_end);
                  header_data.remove_prefix(line_end + 2);

                  auto colon_pos = header_line.find(':');
                  if (colon_pos != std::string::npos) {
                     std::string_view name = header_line.substr(0, colon_pos);
                     // Skip past ':' and any whitespace
                     size_t value_start = header_line.find_first_not_of(" \t", colon_pos + 1);
                     std::string_view value = (value_start != std::string::npos) ? header_line.substr(value_start) : "";

                     // Convert header name to lowercase for case-insensitive lookups (RFC 7230)
                     response_headers.response_headers[to_lower_case(name)] = std::string(value);
                  }
               }

               // Consume all processed header data from the streambuf
               connection->buffer->consume(bytes_transferred);

               connection->is_connected = true;
               connection->timer->cancel();

               if (on_connect) {
                  on_connect(response_headers);
               }

               const bool status_is_error = connection->status_is_error
                                               ? connection->status_is_error(parsed_status->status_code)
                                               : parsed_status->status_code >= 400;

               if (status_is_error) {
                  // Propagate the precise HTTP status via a dedicated error category.
                  on_error(make_http_status_error(parsed_status->status_code));
                  if (on_disconnect) on_disconnect();
                  return;
               }

               bool is_chunked = false;
               auto it = response_headers.response_headers.find("transfer-encoding");
               if (it != response_headers.response_headers.end()) {
                  if (it->second.find("chunked") != std::string::npos) {
                     is_chunked = true;
                  }
               }

               if (is_chunked) {
                  start_chunked_reading(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
               }
               else {
                  start_stream_reading(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
               }
            });
      }

      void start_chunked_reading(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                                 http_error_handler on_error, http_disconnect_handler on_disconnect)
      {
         // Begin the chunk parsing loop.
         // The buffer may already contain the first chunk size line or more data.
         read_chunk_size(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
      }

      void read_chunk_size(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                           http_error_handler on_error, http_disconnect_handler on_disconnect)
      {
         asio::async_read_until(
            *connection->socket, *connection->buffer, "\r\n",
            [this, connection, on_data = std::move(on_data), on_error = std::move(on_error),
             on_disconnect = std::move(on_disconnect)](std::error_code ec, std::size_t bytes_transferred) mutable {
               if (ec || connection->should_stop) {
                  if (ec != asio::error::eof && ec != asio::error::operation_aborted && !connection->should_stop)
                     on_error(ec);
                  if (on_disconnect) on_disconnect();
                  return;
               }

               std::string_view line_view{static_cast<const char*>(connection->buffer->data().data()),
                                          bytes_transferred - 2}; // -2 to exclude CRLF

               // Ignore chunk extensions
               auto semi_pos = line_view.find(';');
               if (semi_pos != std::string_view::npos) {
                  line_view = line_view.substr(0, semi_pos);
               }

               size_t chunk_size;
               auto [ptr, parse_ec] =
                  std::from_chars(line_view.data(), line_view.data() + line_view.size(), chunk_size, 16);

               if (parse_ec != std::errc{}) {
                  on_error(std::make_error_code(std::errc::protocol_error));
                  if (on_disconnect) on_disconnect();
                  return;
               }

               // Consume the size line and CRLF from the buffer
               connection->buffer->consume(bytes_transferred);

               if (chunk_size == 0) {
                  // Last chunk
                  if (on_disconnect) on_disconnect();
                  return;
               }

               read_chunk_body(connection, chunk_size, std::move(on_data), std::move(on_error),
                               std::move(on_disconnect));
            });
      }

      void read_chunk_body(std::shared_ptr<http_stream_connection> connection, size_t chunk_size,
                           http_data_handler on_data, http_error_handler on_error,
                           http_disconnect_handler on_disconnect)
      {
         // We need to read 'chunk_size' bytes of data, plus 2 bytes for the trailing CRLF.
         size_t total_to_read = chunk_size + 2;

         // Check if we have enough data in the buffer already.
         if (connection->buffer->size() >= total_to_read) {
            std::string_view data{static_cast<const char*>(connection->buffer->data().data()), chunk_size};
            on_data(data);
            connection->buffer->consume(total_to_read);

            // Post the next read to avoid deep recursion
            asio::post(connection->socket->get_executor(), [this, connection, on_data = std::move(on_data),
                                                            on_error = std::move(on_error),
                                                            on_disconnect = std::move(on_disconnect)]() mutable {
               read_chunk_size(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
            });
            return;
         }

         // We need to read more from the socket.
         asio::async_read(
            *connection->socket, *connection->buffer,
            asio::transfer_exactly(total_to_read - connection->buffer->size()),
            [this, connection, chunk_size, on_data = std::move(on_data), on_error = std::move(on_error),
             on_disconnect = std::move(on_disconnect)](std::error_code ec, std::size_t) mutable {
               if (ec || connection->should_stop) {
                  if (ec != asio::error::eof && ec != asio::error::operation_aborted && !connection->should_stop)
                     on_error(ec);
                  if (on_disconnect) on_disconnect();
                  return;
               }

               std::string_view data{static_cast<const char*>(connection->buffer->data().data()), chunk_size};
               on_data(data);
               connection->buffer->consume(chunk_size + 2); // Consume data + trailing CRLF

               read_chunk_size(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
            });
      }

      void start_stream_reading(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                                http_error_handler on_error, http_disconnect_handler on_disconnect)
      {
         // Dispatch to the appropriate strategy
         switch (connection->strategy) {
         case stream_read_strategy::bulk_transfer:
            start_stream_reading_bulk(connection, std::move(on_data), std::move(on_error), std::move(on_disconnect));
            break;
         case stream_read_strategy::immediate_delivery:
            start_stream_reading_immediate(connection, std::move(on_data), std::move(on_error),
                                           std::move(on_disconnect));
            break;
         }
      }

      void start_stream_reading_bulk(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                                     http_error_handler on_error, http_disconnect_handler on_disconnect)
      {
         // Process any existing data in buffer first
         if (connection->buffer->size() > 0) {
            std::string_view data{static_cast<const char*>(connection->buffer->data().data()),
                                  connection->buffer->size()};
            on_data(data);
            connection->buffer->consume(connection->buffer->size());
         }

         if (connection->should_stop) {
            if (on_disconnect) on_disconnect();
            return;
         }

         // Use async_read with transfer_at_least(1) - may read more data for efficiency
         asio::async_read(
            *connection->socket, *connection->buffer, asio::transfer_at_least(1),
            [this, connection, on_data, on_error, on_disconnect](std::error_code ec,
                                                                 std::size_t /*bytes_transferred*/) {
               if (ec || connection->should_stop) {
                  if (ec != asio::error::eof && ec != asio::error::operation_aborted && !connection->should_stop) {
                     on_error(ec);
                  }
                  if (on_disconnect) on_disconnect();
                  return;
               }

               // Recurse to process the new data
               start_stream_reading_bulk(connection, on_data, on_error, on_disconnect);
            });
      }

      void start_stream_reading_immediate(std::shared_ptr<http_stream_connection> connection, http_data_handler on_data,
                                          http_error_handler on_error, http_disconnect_handler on_disconnect)
      {
         // Process existing buffer content first
         if (connection->buffer->size() > 0) {
            std::string_view data{static_cast<const char*>(connection->buffer->data().data()),
                                  connection->buffer->size()};
            on_data(data);
            connection->buffer->consume(connection->buffer->size());
         }

         if (connection->should_stop) {
            if (on_disconnect) on_disconnect();
            return;
         }

         // Use async_read_some for immediate delivery of available data
         constexpr size_t read_size = 8192;
         connection->socket->async_read_some(
            connection->buffer->prepare(read_size),
            [this, connection, on_data, on_error, on_disconnect](std::error_code ec, std::size_t bytes_transferred) {
               if (ec || connection->should_stop) {
                  if (ec != asio::error::eof && ec != asio::error::operation_aborted && !connection->should_stop) {
                     on_error(ec);
                  }
                  if (on_disconnect) on_disconnect();
                  return;
               }

               // Commit the received data and deliver immediately
               connection->buffer->commit(bytes_transferred);

               std::string_view data{static_cast<const char*>(connection->buffer->data().data()), bytes_transferred};
               on_data(data);
               connection->buffer->consume(bytes_transferred);

               // Continue reading
               start_stream_reading_immediate(connection, on_data, on_error, on_disconnect);
            });
      }

      std::expected<response, std::error_code> perform_sync_request(
         const std::string& method, const url_parts& url, const std::string& body,
         const std::unordered_map<std::string, std::string>& headers)
      {
         auto socket = connection_pool->get_connection(url.host, url.port);

         try {
            // If socket is not connected, connect it synchronously
            if (!socket->is_open()) {
               asio::ip::tcp::resolver resolver(*async_io_context);
               auto endpoints = resolver.resolve(url.host, std::to_string(url.port));
               asio::connect(*socket, endpoints);
            }

            // Build HTTP request
            std::string request_str;
            request_str.append(method);
            request_str.append(" ");
            request_str.append(url.path);
            request_str.append(" HTTP/1.1\r\n");
            request_str.append("Host: ");
            request_str.append(url.host);
            request_str.append("\r\n");
            request_str.append("Connection: keep-alive\r\n"); // Keep connection alive for reuse

            if (!body.empty()) {
               request_str.append("Content-Length: ");
               request_str.append(std::to_string(body.size()));
               request_str.append("\r\n");
            }

            for (const auto& [name, value] : headers) {
               request_str.append(name);
               request_str.append(": ");
               request_str.append(value);
               request_str.append("\r\n");
            }

            request_str.append("\r\n");
            request_str.append(body);

            // Send request synchronously
            asio::write(*socket, asio::buffer(request_str));

            // Read response headers synchronously
            asio::streambuf response_buffer;
            std::error_code ec;
            size_t header_bytes = asio::read_until(*socket, response_buffer, "\r\n\r\n", ec);
            if (ec) {
               socket->close();
               return std::unexpected(ec);
            }

            // Create a zero-copy view of the header data
            std::string_view header_data{static_cast<const char*>(response_buffer.data().data()), header_bytes};

            // Parse status line from the view
            auto line_end = header_data.find("\r\n");
            if (line_end == std::string_view::npos) {
               return std::unexpected(std::make_error_code(std::errc::protocol_error));
            }
            std::string_view status_line = header_data.substr(0, line_end);
            header_data.remove_prefix(line_end + 2); // Advance past status line

            auto parsed_status = parse_http_status_line(status_line);
            if (!parsed_status) {
               return std::unexpected(parsed_status.error());
            }

            // Parse headers from the view
            std::unordered_map<std::string, std::string> response_headers;
            size_t content_length = 0;
            bool connection_close = false;

            while (!header_data.starts_with("\r\n")) {
               line_end = header_data.find("\r\n");
               if (line_end == std::string_view::npos) {
                  return std::unexpected(std::make_error_code(std::errc::protocol_error));
               }
               std::string_view header_line = header_data.substr(0, line_end);
               header_data.remove_prefix(line_end + 2);

               auto colon_pos = header_line.find(':');
               if (colon_pos != std::string::npos) {
                  std::string_view name = header_line.substr(0, colon_pos);
                  size_t value_start = header_line.find_first_not_of(" \t", colon_pos + 1);
                  std::string_view value = (value_start != std::string::npos) ? header_line.substr(value_start) : "";

                  if (name.length() == 14 && strncasecmp(name.data(), "Content-Length", 14) == 0) {
                     std::from_chars(value.data(), value.data() + value.size(), content_length);
                  }
                  else if (name.length() == 10 && strncasecmp(name.data(), "Connection", 10) == 0) {
                     if (value.find("close") != std::string_view::npos) {
                        connection_close = true;
                     }
                  }

                  // Convert header name to lowercase for case-insensitive lookups (RFC 7230)
                  response_headers.emplace(to_lower_case(name), value);
               }
            }

            // Consume header data, leaving only the over-read body part.
            response_buffer.consume(header_bytes);

            // Read the rest of the body if necessary
            size_t body_in_buffer = response_buffer.size();
            if (content_length > body_in_buffer) {
               size_t remaining_to_read = content_length - body_in_buffer;
               std::error_code read_ec;
               asio::read(*socket, response_buffer, asio::transfer_exactly(remaining_to_read), read_ec);
               if (read_ec) {
                  socket->close(); // Don't reuse a failed connection
                  return std::unexpected(read_ec);
               }
            }

            // Create the body string from the buffer, respecting content_length.
            std::string response_body(static_cast<const char*>(response_buffer.data().data()),
                                      std::min(content_length, response_buffer.size()));

            response resp;
            resp.status_code = parsed_status->status_code;
            resp.response_headers = std::move(response_headers);
            resp.response_body = std::move(response_body);

            if (!connection_close) {
               connection_pool->return_connection(url.host, url.port, socket);
            }
            // else, the server will close the connection, so we don't return it to the pool.

            return resp;
         }
         catch (const std::system_error& e) {
            socket->close(); // Ensure socket is closed on error
            return std::unexpected(e.code());
         }
         catch (...) {
            socket->close(); // Ensure socket is closed on error
            return std::unexpected(std::make_error_code(std::errc::connection_refused));
         }
      }

      template <typename CompletionHandler>
      void perform_request_async(const std::string& method, const url_parts& url, const std::string& body,
                                 const std::unordered_map<std::string, std::string>& headers,
                                 CompletionHandler&& handler)
      {
         auto socket = connection_pool->get_connection(url.host, url.port);

         if (socket->is_open()) {
            send_request(socket, method, url, body, headers, std::forward<CompletionHandler>(handler));
         }
         else {
            auto resolver = std::make_shared<asio::ip::tcp::resolver>(*async_io_context);
            resolver->async_resolve(
               url.host, std::to_string(url.port),
               [this, socket, resolver, method, url, body, headers, handler = std::forward<CompletionHandler>(handler)](
                  std::error_code ec, asio::ip::tcp::resolver::results_type results) mutable {
                  if (ec) {
                     handler(std::unexpected(ec));
                     return;
                  }

                  connect_and_send(socket, results, method, url, body, headers, std::move(handler));
               });
         }
      }

      template <typename CompletionHandler>
      void connect_and_send(std::shared_ptr<asio::ip::tcp::socket> socket,
                            asio::ip::tcp::resolver::results_type results, const std::string& method,
                            const url_parts& url, const std::string& body,
                            const std::unordered_map<std::string, std::string>& headers, CompletionHandler&& handler)
      {
         asio::async_connect(
            *socket, results,
            [this, socket, method, url, body, headers, handler = std::forward<CompletionHandler>(handler)](
               std::error_code ec, const asio::ip::tcp::endpoint&) mutable {
               if (ec) {
                  handler(std::unexpected(ec));
                  return;
               }

               send_request(socket, method, url, body, headers, std::move(handler));
            });
      }

      template <typename CompletionHandler>
      void send_request(std::shared_ptr<asio::ip::tcp::socket> socket, const std::string& method, const url_parts& url,
                        const std::string& body, const std::unordered_map<std::string, std::string>& headers,
                        CompletionHandler&& handler)
      {
         // Build HTTP request
         std::string request_str;
         request_str.append(method);
         request_str.append(" ");
         request_str.append(url.path);
         request_str.append(" HTTP/1.1\r\n");
         request_str.append("Host: ");
         request_str.append(url.host);
         request_str.append("\r\n");
         request_str.append("Connection: keep-alive\r\n");

         if (!body.empty()) {
            request_str.append("Content-Length: ");
            request_str.append(std::to_string(body.size()));
            request_str.append("\r\n");
         }

         for (const auto& [name, value] : headers) {
            request_str.append(name);
            request_str.append(": ");
            request_str.append(value);
            request_str.append("\r\n");
         }

         request_str.append("\r\n");
         request_str.append(body);

         // Use shared_ptr to keep request string alive during async operation
         auto request_str_ptr = std::make_shared<std::string>(std::move(request_str));
         asio::async_write(*socket, asio::buffer(*request_str_ptr),
                           [this, socket, request_str_ptr, url, handler = std::forward<CompletionHandler>(handler)](
                              std::error_code ec, std::size_t) mutable {
                              if (ec) {
                                 handler(std::unexpected(ec));
                                 return;
                              }

                              read_response(socket, url, std::move(handler));
                           });
      }

      template <typename CompletionHandler>
      void read_response(std::shared_ptr<asio::ip::tcp::socket> socket, const url_parts& url,
                         CompletionHandler&& handler)
      {
         auto buffer = std::make_shared<asio::streambuf>();

         asio::async_read_until(*socket, *buffer, "\r\n\r\n",
                                [this, socket, buffer, url, handler = std::forward<CompletionHandler>(handler)](
                                   std::error_code ec, std::size_t bytes_transferred) mutable {
                                   if (ec) {
                                      handler(std::unexpected(ec));
                                      return;
                                   }
                                   // Pass the known header size to the parsing function
                                   parse_and_read_body(socket, buffer, bytes_transferred, url, std::move(handler));
                                });
      }

      template <typename CompletionHandler>
      void parse_and_read_body(std::shared_ptr<asio::ip::tcp::socket> socket, std::shared_ptr<asio::streambuf> buffer,
                               size_t header_size, // <-- NEW: The size of the header block from async_read_until
                               const url_parts& url, CompletionHandler&& handler)
      {
         std::string_view header_section{static_cast<const char*>(buffer->data().data()), header_size};

         // Parse the status line from the view.
         auto line_end = header_section.find("\r\n");
         if (line_end == std::string_view::npos) {
            handler(std::unexpected(std::make_error_code(std::errc::protocol_error)));
            return;
         }
         std::string_view status_line = header_section.substr(0, line_end);
         header_section.remove_prefix(line_end + 2); // Move the view past the status line and its CRLF

         auto parsed_status = parse_http_status_line(status_line);
         if (!parsed_status) {
            handler(std::unexpected(parsed_status.error()));
            return;
         }

         // Parse all header fields from the view.
         std::unordered_map<std::string, std::string> response_headers;
         size_t content_length = 0;
         // The header section ends with an empty line ("\r\n"), which means our view will start with it.
         while (!header_section.starts_with("\r\n")) {
            line_end = header_section.find("\r\n");
            if (line_end == std::string_view::npos) {
               handler(std::unexpected(std::make_error_code(std::errc::protocol_error)));
               return;
            }
            std::string_view header_line = header_section.substr(0, line_end);
            header_section.remove_prefix(line_end + 2);

            auto colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
               std::string_view name = header_line.substr(0, colon_pos);
               // Skip past ':' and any leading whitespace on the value.
               size_t value_start = header_line.find_first_not_of(" \t", colon_pos + 1);
               std::string_view value = (value_start != std::string::npos) ? header_line.substr(value_start) : "";

               // A case-insensitive comparison is more robust for header names.
               if (name.size() == 14 && (name[0] == 'C' || name[0] == 'c') &&
                   glz::strncasecmp(name.data(), "Content-Length", 14) == 0) {
                  std::from_chars(value.data(), value.data() + value.size(), content_length);
               }
               // Convert header name to lowercase for case-insensitive lookups (RFC 7230)
               response_headers.emplace(to_lower_case(name), value);
            }
         }

         // Consume the entire header block from the streambuf.
         // This efficiently discards the header data we've just parsed, leaving only body data.
         buffer->consume(header_size);

         // Read the rest of the body, if any is still needed.
         size_t body_already_in_buffer = buffer->size();
         size_t remaining_to_read =
            (content_length > body_already_in_buffer) ? (content_length - body_already_in_buffer) : 0;

         asio::async_read(
            *socket, *buffer, asio::transfer_exactly(remaining_to_read),
            [this, socket, buffer, url, status_code = parsed_status->status_code,
             response_headers = std::move(response_headers),
             handler = std::forward<CompletionHandler>(handler)](std::error_code ec, std::size_t) mutable {
               // EOF is expected if the server closes the connection.
               if (ec && ec != asio::error::eof) {
                  handler(std::unexpected(ec));
                  return;
               }

               // Directly construct the string from the buffer's contiguous memory.
               std::string body(static_cast<const char*>(buffer->data().data()), buffer->size());

               response resp;
               resp.status_code = status_code;
               resp.response_headers = std::move(response_headers);
               resp.response_body = std::move(body);

               // Return connection to pool if it's still usable
               auto connection_header = resp.response_headers.find("connection");
               if (connection_header == resp.response_headers.end() ||
                   connection_header->second.find("close") == std::string::npos) {
                  connection_pool->return_connection(url.host, url.port, socket);
               }

               handler(std::move(resp));
            });
      }
   };
}
