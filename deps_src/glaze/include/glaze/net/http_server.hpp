// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <expected>
#include <functional>
#include <future>
#include <glaze/glaze.hpp>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <thread>
#include <unordered_map>

#include "glaze/ext/glaze_asio.hpp"
#include "glaze/net/cors.hpp"
#include "glaze/net/http_router.hpp"
#include "glaze/net/openapi.hpp"
#include "glaze/net/websocket_connection.hpp"
#include "glaze/util/key_transformers.hpp"

// Conditionally include SSL headers only when needed
#ifdef GLZ_ENABLE_SSL
#include <asio/ssl.hpp>
#endif

// To deconflict Windows.h
#ifdef DELETE
#undef DELETE
#endif

namespace glz
{
   namespace detail
   {
      // Type trait to detect SSL streams
      template <typename T>
      inline constexpr bool is_ssl_stream = false;

#ifdef GLZ_ENABLE_SSL
      template <typename T>
      inline constexpr bool is_ssl_stream<asio::ssl::stream<T>> = true;
#endif
   }

   // Interface for streaming connections (type-erased for HTTP/HTTPS compatibility)
   struct streaming_connection_interface
   {
      using data_sent_handler = std::function<void(std::error_code)>;
      using disconnect_handler = std::function<void()>;

      virtual ~streaming_connection_interface() = default;

      // Send initial headers for streaming response
      virtual void send_headers(int status_code, const std::unordered_map<std::string, std::string>& headers = {},
                                data_sent_handler handler = {}) = 0;

      // Send a chunk of data
      virtual void send_chunk(std::string_view data, data_sent_handler handler = {}) = 0;

      // Send Server-Sent Event
      virtual void send_event(std::string_view event_type, std::string_view data, std::string_view id = {},
                              data_sent_handler handler = {}) = 0;

      // Close the streaming connection
      virtual void close(disconnect_handler handler = {}) = 0;

      // Set disconnect handler for client disconnection
      virtual void on_disconnect(disconnect_handler handler) = 0;

      // Check if connection is still alive
      virtual bool is_open() const = 0;

      // Get remote endpoint info
      virtual std::string remote_address() const = 0;
      virtual uint16_t remote_port() const = 0;

      // Check if headers have been sent
      virtual bool is_headers_sent() const = 0;

      // Get executor for async operations (timers, etc.)
      virtual asio::any_io_executor get_executor() const = 0;

      // Send JSON as Server-Sent Event (convenience method using send_event)
      template <class T>
      void send_json_event(const T& data, std::string_view event_type = "message", std::string_view id = {},
                           data_sent_handler handler = {})
      {
         std::string json_str;
         auto ec = glz::write_json(data, json_str);
         if (!ec) {
            send_event(event_type, json_str, id, handler);
         }
         else if (handler) {
            handler(std::make_error_code(std::errc::invalid_argument));
         }
      }
   };

   // Streaming connection handle for server-side streaming
   template <typename SocketType = asio::ip::tcp::socket>
   struct streaming_connection : public streaming_connection_interface,
                                 public std::enable_shared_from_this<streaming_connection<SocketType>>
   {
      using data_sent_handler = std::function<void(std::error_code)>;
      using disconnect_handler = std::function<void()>;

      streaming_connection(std::shared_ptr<SocketType> socket)
         : socket_(socket), is_headers_sent_(false), is_closed_(false)
      {}

      // Send initial headers for streaming response
      void send_headers(int status_code, const std::unordered_map<std::string, std::string>& headers = {},
                        data_sent_handler handler = {}) override
      {
         if (is_headers_sent_) return;
         is_headers_sent_ = true;

         std::string response_str;
         response_str.reserve(512);

         response_str.append("HTTP/1.1 ");
         response_str.append(std::to_string(status_code));
         response_str.append(" ");
         response_str.append(get_status_message(status_code));
         response_str.append("\r\n");

         // Add custom headers
         for (const auto& [name, value] : headers) {
            response_str.append(name);
            response_str.append(": ");
            response_str.append(value);
            response_str.append("\r\n");
         }

         // Default headers for streaming
         if (headers.find("transfer-encoding") == headers.end()) {
            response_str.append("Transfer-Encoding: chunked\r\n");
            chunked_encoding_ = true;
         }
         if (headers.find("connection") == headers.end()) {
            response_str.append("Connection: keep-alive\r\n");
         }
         if (headers.find("cache-control") == headers.end()) {
            response_str.append("Cache-Control: no-cache\r\n");
         }

         response_str.append("\r\n");

         auto buffer = std::make_shared<std::string>(std::move(response_str));
         auto self = this->shared_from_this();

         asio::async_write(*socket_, asio::buffer(*buffer), [self, buffer, handler](std::error_code ec, std::size_t) {
            if (handler) handler(ec);
         });
      }

      // Send a chunk of data
      void send_chunk(std::string_view data, data_sent_handler handler = {}) override
      {
         if (is_closed_) return;

         auto self = this->shared_from_this();

         if (chunked_encoding_) {
            // Format as HTTP chunk
            std::string chunk;
            chunk.reserve(data.size() + 20); // 20 is a good estimate for hex size + CRLFs

            // Chunk size in hex, converted efficiently with std::to_chars
            std::array<char, 16> size_buf{}; // 64-bit size_t in hex is at most 16 chars
            if (auto [ptr, ec] = std::to_chars(size_buf.data(), size_buf.data() + size_buf.size(), data.size(), 16);
                ec == std::errc()) {
               chunk.append(std::string_view(size_buf.data(), ptr - size_buf.data()));
            }
            chunk.append("\r\n");
            chunk.append(data);
            chunk.append("\r\n");

            auto buffer = std::make_shared<std::string>(std::move(chunk));
            asio::async_write(*socket_, asio::buffer(*buffer),
                              [self, buffer, handler](std::error_code ec, std::size_t) {
                                 if (handler) handler(ec);
                              });
         }
         else {
            // Send raw data
            auto buffer = std::make_shared<std::string>(data);
            asio::async_write(*socket_, asio::buffer(*buffer),
                              [self, buffer, handler](std::error_code ec, std::size_t) {
                                 if (handler) handler(ec);
                              });
         }
      }

      // Send Server-Sent Event
      void send_event(std::string_view event_type, std::string_view data, std::string_view id = {},
                      data_sent_handler handler = {}) override
      {
         std::string sse_data;
         sse_data.reserve(data.size() + 50);

         if (!id.empty()) {
            sse_data.append("id: ");
            sse_data.append(id);
            sse_data.append("\n");
         }

         if (!event_type.empty()) {
            sse_data.append("event: ");
            sse_data.append(event_type);
            sse_data.append("\n");
         }

         sse_data.append("data: ");
         sse_data.append(data);
         sse_data.append("\n\n");

         send_chunk(sse_data, handler);
      }

      // Close the streaming connection
      void close(disconnect_handler handler = {}) override
      {
         if (is_closed_) return;
         is_closed_ = true;

         auto self = this->shared_from_this();

         if (chunked_encoding_) {
            // Send final chunk
            std::string final_chunk = "0\r\n\r\n";
            auto buffer = std::make_shared<std::string>(std::move(final_chunk));

            asio::async_write(*socket_, asio::buffer(*buffer), [self, buffer, handler](asio::error_code, std::size_t) {
               if (handler) handler();
               asio::error_code close_ec;
               self->socket_->lowest_layer().close(close_ec);
            });
         }
         else {
            if (handler) handler();
            asio::error_code ec;
            socket_->lowest_layer().close(ec);
         }
      }

      // Set disconnect handler for client disconnection
      void on_disconnect(disconnect_handler handler) override
      {
         disconnect_handler_ = handler;
         start_disconnect_detection();
      }

      // Check if connection is still alive
      bool is_open() const override { return socket_ && socket_->lowest_layer().is_open() && !is_closed_; }

      // Get remote endpoint info
      std::string remote_address() const override
      {
         if (socket_) {
            try {
               return socket_->lowest_layer().remote_endpoint().address().to_string();
            }
            catch (...) {
            }
         }
         return "";
      }

      uint16_t remote_port() const override
      {
         if (socket_) {
            try {
               return socket_->lowest_layer().remote_endpoint().port();
            }
            catch (...) {
            }
         }
         return 0;
      }

      bool is_headers_sent() const override { return is_headers_sent_; }

      asio::any_io_executor get_executor() const override { return socket_->lowest_layer().get_executor(); }

      std::shared_ptr<SocketType> socket_;

     private:
      disconnect_handler disconnect_handler_;
      bool is_headers_sent_;
      bool is_closed_;
      bool chunked_encoding_ = false;

      void start_disconnect_detection()
      {
         if (!socket_ || is_closed_) return;

         auto self = this->shared_from_this();

         // For SSL streams, we can't use async_receive with message_peek on lowest_layer()
         // because it returns basic_socket which doesn't support async_receive.
         // Use a timer-based check instead that verifies the socket is still open.
         // Trade-off: SSL disconnect detection has up to 1-second latency vs TCP's immediate
         // detection. This is acceptable for most streaming use cases where cleanup timing
         // is not critical. For applications requiring faster detection, consider implementing
         // application-level heartbeats.
         if constexpr (detail::is_ssl_stream<SocketType>) {
            auto timer = std::make_shared<asio::steady_timer>(socket_->lowest_layer().get_executor());
            timer->expires_after(std::chrono::seconds(1));
            timer->async_wait([self, timer](std::error_code ec) {
               if (ec) return; // Timer cancelled
               if (!self->socket_->lowest_layer().is_open()) {
                  if (self->disconnect_handler_) {
                     self->is_closed_ = true;
                     self->disconnect_handler_();
                  }
               }
               else if (!self->is_closed_) {
                  self->start_disconnect_detection();
               }
            });
         }
         else {
            // For TCP sockets, use async_receive with message_peek for more responsive detection
            auto buffer = std::make_shared<std::array<uint8_t, 1>>();
            socket_->async_receive(
               asio::buffer(*buffer), asio::socket_base::message_peek, [self, buffer](std::error_code ec, std::size_t) {
                  if (ec && self->disconnect_handler_) {
                     self->is_closed_ = true;
                     self->disconnect_handler_();
                  }
                  else if (!ec && !self->is_closed_) {
                     // Continue monitoring
                     auto timer = std::make_shared<asio::steady_timer>(self->socket_->lowest_layer().get_executor());
                     timer->expires_after(std::chrono::seconds(1));
                     timer->async_wait([self, timer](std::error_code) { self->start_disconnect_detection(); });
                  }
               });
         }
      }

      std::string_view get_status_message(int status_code)
      {
         switch (status_code) {
         case 200:
            return "OK";
         case 201:
            return "Created";
         case 204:
            return "No Content";
         case 400:
            return "Bad Request";
         case 401:
            return "Unauthorized";
         case 403:
            return "Forbidden";
         case 404:
            return "Not Found";
         case 500:
            return "Internal Server Error";
         default:
            return "Unknown";
         }
      }
   };

   // Enhanced response class with streaming support
   struct streaming_response
   {
      std::shared_ptr<streaming_connection_interface> stream;

      streaming_response(std::shared_ptr<streaming_connection_interface> conn) : stream(std::move(conn)) {}

      // Send headers and start streaming
      streaming_response& start_stream(int status_code = 200,
                                       const std::unordered_map<std::string, std::string>& headers = {})
      {
         if (stream) {
            stream->send_headers(status_code, headers);
         }
         return *this;
      }

      // Send a chunk of data
      streaming_response& send(std::string_view data)
      {
         if (stream) {
            stream->send_chunk(data);
         }
         return *this;
      }

      // Send JSON data
      template <class T>
      streaming_response& send_json(const T& data)
      {
         if (stream) {
            std::string json_str;
            auto ec = glz::write_json(data, json_str);
            if (!ec) {
               stream->send_chunk(json_str);
            }
         }
         return *this;
      }

      // Send Server-Sent Event
      streaming_response& send_event(std::string_view event_type, std::string_view data, std::string_view id = {})
      {
         if (stream) {
            stream->send_event(event_type, data, id);
         }
         return *this;
      }

      // Helper for SSE setup
      streaming_response& as_event_stream()
      {
         return start_stream(200, {{"Content-Type", "text/event-stream"},
                                   {"Cache-Control", "no-cache"},
                                   {"Access-Control-Allow-Origin", "*"}});
      }

      // Close the stream
      void close()
      {
         if (stream) {
            stream->close();
         }
      }
   };

   // Handler types for streaming
   using streaming_handler = std::function<void(request&, streaming_response&)>;

   // Wrapping middleware types
   // Non-copyable, non-movable wrapper to enforce synchronous execution
   // This prevents accidental storage and async invocation which would cause dangling references
   struct next_handler final
   {
      explicit next_handler(std::function<void()> fn) : fn_(std::move(fn)) {}

      // Prevent copying and moving to enforce synchronous usage
      next_handler(const next_handler&) = delete;
      next_handler(next_handler&&) = delete;
      next_handler& operator=(const next_handler&) = delete;
      next_handler& operator=(next_handler&&) = delete;

      void operator()() const { fn_(); }

     private:
      std::function<void()> fn_;
   };

   // C++20 concept for wrapping middleware callables
   template <typename F>
   concept wrapping_middleware_callable = requires(F&& f, const request& req, response& res, const next_handler& next) {
      { std::forward<F>(f)(req, res, next) } -> std::same_as<void>;
   };

   using wrapping_middleware = std::function<void(const request&, response&, const next_handler&)>;

   // Request/Response lifecycle hooks for metrics and logging
   using request_hook = std::function<void(const request&, response&)>;
   using response_hook = std::function<void(const request&, const response&)>;

   /**
    * @brief Configuration for HTTP connection behavior
    *
    * Controls keep-alive (persistent connection) settings for the HTTP server.
    * HTTP/1.1 defaults to keep-alive; this configuration allows tuning or disabling it.
    */
   struct connection_config
   {
      /**
       * @brief Enable HTTP/1.1 persistent connections (keep-alive)
       *
       * When enabled, the server will reuse connections for multiple requests.
       * When disabled, each request/response closes the connection.
       * Default: true (HTTP/1.1 compliant behavior)
       */
      bool keep_alive = true;

      /**
       * @brief Timeout for idle connections in seconds
       *
       * After sending a response, if no new request arrives within this timeout,
       * the connection is closed. Set to 0 to disable timeout (not recommended).
       * Default: 60 seconds
       */
      uint32_t keep_alive_timeout = 60;

      /**
       * @brief Maximum requests allowed per connection
       *
       * After this many requests on a single connection, the server sends
       * Connection: close and closes the connection. Set to 0 for unlimited.
       * Default: 0 (unlimited)
       */
      uint32_t max_requests_per_connection = 0;
   };

   // Server implementation using non-blocking asio with WebSocket support
   template <bool EnableTLS = false>
   struct http_server
   {
      // Socket type abstraction
      using socket_type = std::conditional_t<EnableTLS,
#ifdef GLZ_ENABLE_SSL
                                             asio::ssl::stream<asio::ip::tcp::socket>,
#else
                                             asio::ip::tcp::socket,
#endif
                                             asio::ip::tcp::socket>;

      /**
       * @brief Construct an HTTP server
       *
       * @param context Optional external io_context to use. If nullptr, creates its own.
       *                Using an external io_context allows sharing the event loop with
       *                other asio-based components or managing the io_context lifecycle
       *                externally.
       * @param custom_error_handler Optional error handler for server errors.
       *                             If not provided, uses a default handler that prints
       *                             to stderr.
       *
       * Example with internal io_context (default):
       * @code
       * glz::http_server server;
       * server.bind(8080);
       * server.start();  // Creates worker threads using hardware_concurrency()
       * @endcode
       *
       * Example with external io_context:
       * @code
       * auto io_ctx = std::make_shared<asio::io_context>();
       * glz::http_server server(io_ctx);
       * server.bind(8080);
       * server.start(0);  // Pass 0 to skip creating worker threads
       *
       * // Run io_context in your own thread(s)
       * std::thread io_thread([io_ctx]() {
       *    io_ctx->run();
       * });
       *
       * // ... later ...
       * server.stop();
       * io_ctx->stop();
       * io_thread.join();
       * @endcode
       *
       * Example with custom error handler:
       * @code
       * auto error_handler = [](std::error_code ec, std::source_location loc) {
       *    my_logger.error("Server error at {}:{}: {}",
       *                    loc.file_name(), loc.line(), ec.message());
       * };
       * glz::http_server server(nullptr, error_handler);
       * @endcode
       */
      inline http_server(std::shared_ptr<asio::io_context> context = nullptr,
                         glz::error_handler custom_error_handler = {})
         : io_context(context ? context : std::make_shared<asio::io_context>())
      {
         if (custom_error_handler) {
            error_handler = std::move(custom_error_handler);
         }
         else {
            error_handler = [](std::error_code ec, std::source_location loc) {
               std::fprintf(stderr, "Error at %s:%d: %s\n", loc.file_name(), static_cast<int>(loc.line()),
                            ec.message().c_str());
            };
         }

         // Initialize SSL context for TLS-enabled servers
         if constexpr (EnableTLS) {
#ifdef GLZ_ENABLE_SSL
            ssl_context = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12);
            ssl_context->set_default_verify_paths();
#else
            static_assert(!EnableTLS, "TLS support requires GLZ_ENABLE_SSL to be defined and OpenSSL to be available");
#endif
         }
      }

      inline ~http_server()
      {
         if (running) {
            stop();
         }
         // Clean up any remaining threads
         for (auto& thread : threads) {
            if (thread.joinable()) {
               thread.join();
            }
         }
         threads.clear();

         // websocket_handlers_ destruction will call close_all_connections() on each
         // websocket_server, which force-closes all sockets. This ensures sockets are
         // deregistered from the reactor BEFORE io_context is destroyed.
      }

      inline http_server& bind(std::string_view address, uint16_t port)
      {
         try {
            asio::ip::tcp::endpoint endpoint(asio::ip::make_address(address), port);
            acceptor = std::make_unique<asio::ip::tcp::acceptor>(*io_context, endpoint);
         }
         catch (...) {
            error_handler(std::make_error_code(std::errc::address_in_use), std::source_location::current());
         }
         return *this;
      }

      inline http_server& bind(uint16_t port) { return bind("0.0.0.0", port); }

      inline uint16_t port(asio::error_code& ec) const
      {
         if (!acceptor) {
            ec = asio::error::not_connected;
            return 0;
         }
         auto endpoint = acceptor->local_endpoint(ec);
         if (ec) {
            return 0;
         }
         return endpoint.port();
      }

      inline uint16_t port() const
      {
         if (!acceptor) {
            throw std::runtime_error("Server not bound");
         }
         return acceptor->local_endpoint().port();
      }

      /**
       * @brief Start the HTTP server
       *
       * @param num_threads Number of worker threads to create:
       *                    - std::nullopt (default): Uses std::thread::hardware_concurrency(),
       *                      with a minimum of 1 thread if hardware_concurrency() returns 0
       *                    - 0: No worker threads created (caller manages io_context::run())
       *                    - N: Creates exactly N worker threads
       *
       * When using an external io_context, pass 0 to avoid creating threads,
       * then call io_context::run() in your own thread(s).
       *
       * Example with default behavior:
       * @code
       * glz::http_server server;
       * server.bind(8080);
       * server.start();  // Uses hardware_concurrency() threads
       * @endcode
       *
       * Example with specific thread count:
       * @code
       * glz::http_server server;
       * server.bind(8080);
       * server.start(4);  // Creates exactly 4 worker threads
       * @endcode
       *
       * Example with external io_context:
       * @code
       * auto io_ctx = std::make_shared<asio::io_context>();
       * glz::http_server server(io_ctx);
       * server.bind(8080);
       * server.start(0);  // No worker threads - caller manages io_context
       *
       * // Run io_context in your own thread
       * std::thread io_thread([io_ctx]() {
       *    io_ctx->run();
       * });
       *
       * // ... later ...
       * server.stop();
       * io_ctx->stop();
       * io_thread.join();
       * @endcode
       */
      inline void start(std::optional<size_t> num_threads = std::nullopt)
      {
         if (running || !acceptor) {
            return;
         }

         running = true;

         // Determine number of threads
         size_t actual_threads;
         if (num_threads.has_value()) {
            // Use the explicitly specified value (including 0)
            actual_threads = *num_threads;
         }
         else {
            // Use hardware concurrency, but ensure at least 1 thread
            actual_threads = std::thread::hardware_concurrency();
            if (actual_threads == 0) {
               actual_threads = 1;
            }
         }

         // Start the acceptor
         do_accept();

         // Start worker threads (unless explicitly set to 0)
         if (actual_threads > 0) {
            threads.reserve(actual_threads);
            for (size_t i = 0; i < actual_threads; ++i) {
               threads.emplace_back([this] {
                  io_context->run();
                  // Don't report errors during shutdown
               });
            }
         }
      }

      inline void stop()
      {
         {
            std::lock_guard<std::mutex> lock(shutdown_mutex);
            if (!running) {
               return;
            }
            running = false;
         }

         // Stop accepting new connections
         if (acceptor) {
            try {
               acceptor->close();
            }
            catch (...) {
               // Ignore errors on close
            }
         }

         // Cancel the signal handler
         if (signals_) {
            try {
               signals_->cancel();
            }
            catch (...) {
               // Ignore errors on cancel
            }
         }

         // Stop the io_context
         io_context->stop();

         // Only join threads if we're not in one of the worker threads
         auto current_thread_id = std::this_thread::get_id();
         bool is_worker_thread = false;
         for (const auto& thread : threads) {
            if (thread.get_id() == current_thread_id) {
               is_worker_thread = true;
               break;
            }
         }

         if (!is_worker_thread) {
            for (auto& thread : threads) {
               if (thread.joinable()) {
                  thread.join();
               }
            }
            threads.clear();
         }

         // Notify any threads waiting for shutdown
         shutdown_cv.notify_all();
      }

      inline http_server& mount(std::string_view base_path, const http_router& router)
      {
         // Mount all routes from the router at the specified base path
         for (const auto& [path, method_handlers] : router.routes) {
            std::string full_path = std::string(base_path);
            if (!full_path.empty() && full_path.back() == '/') {
               full_path.pop_back();
            }
            full_path += path;

            for (const auto& [method, route_entry] : method_handlers) {
               root_router.route(method, full_path, route_entry.handle, route_entry.spec);
            }
         }

         // Add middleware
         for (const auto& middleware : router.middlewares) {
            root_router.use(middleware);
         }

         return *this;
      }

      inline http_server& use(handler middleware)
      {
         root_router.use(std::move(middleware));
         return *this;
      }

      /**
       * @brief Register wrapping middleware that executes around route handlers
       *
       * Wrapping middleware can execute code before AND after the handler by wrapping
       * the next() function. This enables natural timing measurements, response
       * transformation, and comprehensive error handling.
       *
       * Use cases:
       * - Request/response timing and metrics
       * - Logging with full context (including response status and timing)
       * - Response transformation (compression, encryption, header injection)
       * - Error handling around handlers (try-catch wrapping)
       * - Any cross-cutting concerns that need both before and after logic
       *
       * CRITICAL SAFETY REQUIREMENT:
       * ============================
       * The next() handler MUST be called synchronously within the middleware function.
       * The next_handler type is intentionally non-copyable and non-movable to prevent
       * accidental storage and asynchronous invocation.
       *
       * Storing next() or calling it asynchronously will result in:
       * - Compile-time errors (if you try to copy/move it)
       * - Undefined behavior due to dangling references (if you work around the safety)
       *
       * The request, response, and handler objects are only valid during the synchronous
       * execution of the middleware chain.
       *
       * CORRECT Usage:
       * ```cpp
       * server.wrap([](const request& req, response& res, auto next) {
       *     auto start = std::chrono::steady_clock::now();
       *     next(); // ✓ Synchronous call - correct
       *     auto duration = std::chrono::steady_clock::now() - start;
       *     std::cout << "Request took " << duration.count() << "ns\n";
       * });
       * ```
       *
       * INCORRECT Usage (will not compile):
       * ```cpp
       * server.wrap([](const request& req, response& res, auto next) {
       *     // ✗ COMPILE ERROR: next_handler is non-copyable
       *     std::thread([next]() {
       *         std::this_thread::sleep_for(std::chrono::seconds(1));
       *         next(); // Would cause dangling references
       *     }).detach();
       * });
       * ```
       *
       * For asynchronous operations, complete them BEFORE or AFTER calling next():
       * ```cpp
       * server.wrap([](const request& req, response& res, auto next) {
       *     // Async work BEFORE handler
       *     auto data = fetch_from_cache_async().get();
       *
       *     next(); // Synchronous handler execution
       *
       *     // Async work AFTER handler (fire and forget logging)
       *     std::async(std::launch::async, [status = res.status_code]() {
       *         log_to_remote_server(status);
       *     });
       * });
       * ```
       *
       * Execution order with multiple middleware:
       * ```cpp
       * server.wrap([](auto&, auto&, auto next) {
       *     std::cout << "A before\n";
       *     next();
       *     std::cout << "A after\n";
       * });
       * server.wrap([](auto&, auto&, auto next) {
       *     std::cout << "B before\n";
       *     next();
       *     std::cout << "B after\n";
       * });
       * // Output: A before -> B before -> handler -> B after -> A after
       * ```
       *
       * @tparam F Callable type (must satisfy wrapping_middleware_callable concept)
       * @param middleware The wrapping middleware function
       * @return Reference to this server for method chaining
       */
      template <wrapping_middleware_callable F>
      inline http_server& wrap(F&& middleware)
      {
         wrapping_middlewares_.push_back(std::forward<F>(middleware));
         return *this;
      }

      inline http_router& route(http_method method, std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.route(method, path, handle, spec);
      }

      inline http_router& get(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.get(path, handle, spec);
      }

      inline http_router& post(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.post(path, handle, spec);
      }

      inline http_router& put(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.put(path, handle, spec);
      }

      inline http_router& del(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.del(path, handle, spec);
      }

      inline http_router& patch(std::string_view path, handler handle, const route_spec& spec = {})
      {
         return root_router.patch(path, handle, spec);
      }

      // Register streaming route
      inline http_server& stream(http_method method, std::string_view path, streaming_handler handle)
      {
         streaming_handlers_[std::string(path)][method] = std::move(handle);
         return *this;
      }

      // Convenience methods for streaming
      inline http_server& stream_get(std::string_view path, streaming_handler handle)
      {
         return stream(http_method::GET, path, std::move(handle));
      }

      inline http_server& stream_post(std::string_view path, streaming_handler handle)
      {
         return stream(http_method::POST, path, std::move(handle));
      }

      inline http_server& on_error(error_handler handle)
      {
         this->error_handler = std::move(handle);
         return *this;
      }

      /**
       * @brief Enable API inspection by exposing an OpenAPI 3.0 specification.
       *
       * This provides a standard, machine-readable way for clients to discover the API.
       *
       * @param path The path to expose the OpenAPI JSON specification on.
       * @param title The title of the API.
       * @param version The version of the API.
       * @return Reference to this server for method chaining.
       */
      http_server& enable_openapi_spec(std::string_view path = "/openapi.json",
                                       std::string_view title = "API Specification", std::string_view version = "1.0.0")
      {
         get(path, [this, title = std::string(title), version = std::string(version)](const request&, response& res) {
            open_api spec{};
            spec.info.title = title;
            spec.info.version = version;

            for (const auto& [route_path, method_handlers] : root_router.routes) {
               // Convert router path /:param to OpenAPI path /{param}
               std::string openapi_path = route_path;
               size_t pos = 0;
               while ((pos = openapi_path.find(':', pos)) != std::string::npos) {
                  openapi_path.replace(pos, 1, "{");
                  size_t end_pos = openapi_path.find('/', pos);
                  if (end_pos == std::string::npos) {
                     openapi_path.push_back('}');
                  }
                  else {
                     openapi_path.insert(end_pos, "}");
                  }
               }

               auto& path_item = spec.paths[openapi_path];

               for (const auto& [method, route_entry] : method_handlers) {
                  openapi_operation op;
                  op.summary = route_entry.spec.description;
                  if (!route_entry.spec.tags.empty()) {
                     op.tags = route_entry.spec.tags;
                  }
                  op.operationId = std::string(to_string(method)) + route_path;
                  op.responses["200"].description = "OK";

                  // Add request body schema
                  if (route_entry.spec.request_body_schema) {
                     openapi_request_body req_body;
                     req_body.required = true;
                     if (auto schema_val =
                            glz::read_json<glz::detail::schematic>(*route_entry.spec.request_body_schema)) {
                        req_body.content["application/json"].schema = *schema_val;
                     }
                     op.requestBody = req_body;

                     // Add schema to components
                     if (!spec.components) spec.components.emplace();
                     if (!spec.components->schemas) spec.components->schemas.emplace();
                     if (auto schema_val =
                            glz::read_json<glz::detail::schematic>(*route_entry.spec.request_body_schema)) {
                        spec.components->schemas->operator[](*route_entry.spec.request_body_type_name) = *schema_val;
                     }
                  }

                  // Add response schema
                  if (route_entry.spec.response_schema) {
                     openapi_response res_obj;
                     res_obj.description = "Successful response";
                     res_obj.content.emplace(); // Emplace the unordered_map
                     if (auto schema_val = glz::read_json<glz::detail::schematic>(*route_entry.spec.response_schema)) {
                        res_obj.content.value()["application/json"].schema = *schema_val;
                     }
                     op.responses["200"] = res_obj;

                     // Add schema to components
                     if (!spec.components) spec.components.emplace();
                     if (!spec.components->schemas) spec.components->schemas.emplace();
                     if (auto schema_val = glz::read_json<glz::detail::schematic>(*route_entry.spec.response_schema)) {
                        spec.components->schemas->operator[](*route_entry.spec.response_type_name) = *schema_val;
                     }
                  }

                  // Extract path parameters
                  auto segments = http_router::split_path(route_path);
                  for (const auto& segment : segments) {
                     if (!segment.empty() && segment.front() == ':') {
                        if (!op.parameters) op.parameters.emplace();
                        auto& param = op.parameters->emplace_back();
                        param.name = segment.substr(1);
                        param.in = "path";
                        param.required = true;
                        if (auto it = route_entry.spec.constraints.find(param.name);
                            it != route_entry.spec.constraints.end()) {
                           param.description = it->second.description;
                        }
                     }
                  }

                  switch (method) {
                  case http_method::GET:
                     path_item.get = op;
                     break;
                  case http_method::POST:
                     path_item.post = op;
                     break;
                  case http_method::PUT:
                     path_item.put = op;
                     break;
                  case http_method::DELETE:
                     path_item.del = op;
                     break;
                  case http_method::PATCH:
                     path_item.patch = op;
                     break;
                  default:
                     break;
                  }
               }
            }
            res.json(spec);
         });
         return *this;
      }

      /**
       * @brief Enable CORS with default configuration (allows all origins)
       *
       * This is suitable for development environments
       *
       * @return Reference to this server for method chaining
       */
      http_server& enable_cors()
      {
         root_router.use(glz::simple_cors());
         return *this;
      }

      /**
       * @brief Enable CORS with custom configuration
       *
       * @param config The CORS configuration to use
       * @return Reference to this server for method chaining
       */
      http_server& enable_cors(const glz::cors_config& config)
      {
         root_router.use(glz::create_cors_middleware(config));
         return *this;
      }

      /**
       * @brief Enable CORS for specific origins
       *
       * This is suitable for production environments where you want to restrict
       * which origins can access your API
       *
       * @param origins Vector of allowed origins (e.g., {"https://example.com", "https://app.example.com"})
       * @param allow_credentials Whether to allow credentials (cookies, auth headers)
       * @return Reference to this server for method chaining
       */
      http_server& enable_cors(const std::vector<std::string>& origins, bool allow_credentials = false)
      {
         root_router.use(glz::restrictive_cors(origins, allow_credentials));
         return *this;
      }

      /**
       * @brief Register a WebSocket handler for a specific path
       *
       * @param path The path to handle WebSocket connections on
       * @param server The WebSocket server instance to handle connections
       * @return Reference to this http_server for method chaining
       */
      inline http_server& websocket(std::string_view path, std::shared_ptr<websocket_server> server)
      {
         websocket_handlers_[std::string(path)] = server;
         return *this;
      }

      /**
       * @brief Register a hook to be called when a request is received
       *
       * Request hooks are called after the request is fully parsed, before middleware
       * and route handlers are executed. This is useful for metrics tracking, logging,
       * or request preprocessing.
       *
       * For timing measurements, consider using wrapping middleware instead, which can
       * naturally measure duration by wrapping the next() function.
       *
       * @param hook The function to call for each request
       * @return Reference to this server for method chaining
       */
      inline http_server& on_request(request_hook hook)
      {
         request_hooks_.push_back(std::move(hook));
         return *this;
      }

      /**
       * @brief Register a hook to be called before a response is sent
       *
       * Response hooks are called after the route handler completes, but before the
       * response is sent to the client. This is useful for metrics collection,
       * response logging, or adding final response headers.
       *
       * For timing measurements, consider using wrapping middleware instead, which can
       * naturally measure duration by wrapping the next() function.
       *
       * @param hook The function to call before each response
       * @return Reference to this server for method chaining
       */
      inline http_server& on_response(response_hook hook)
      {
         response_hooks_.push_back(std::move(hook));
         return *this;
      }

      /**
       * @brief Load SSL certificate and private key for HTTPS servers
       *
       * @param cert_file Path to the certificate file (PEM format)
       * @param key_file Path to the private key file (PEM format)
       * @return Reference to this server for method chaining
       */
      inline http_server& load_certificate(const std::string& cert_file, const std::string& key_file)
      {
         if constexpr (EnableTLS) {
#ifdef GLZ_ENABLE_SSL
            ssl_context->use_certificate_chain_file(cert_file);
            ssl_context->use_private_key_file(key_file, asio::ssl::context::pem);
#endif
         }
         else {
            (void)cert_file;
            (void)key_file;
         }
         return *this;
      }

      /**
       * @brief Set SSL verification mode
       *
       * @param mode SSL verification mode
       * @return Reference to this server for method chaining
       */
      inline http_server& set_ssl_verify_mode([[maybe_unused]] int mode)
      {
         if constexpr (EnableTLS) {
#ifdef GLZ_ENABLE_SSL
            ssl_context->set_verify_mode(mode);
#endif
         }
         return *this;
      }

      /**
       * @brief Enable signal handling for graceful shutdown
       *
       * Registers signal handlers for SIGINT (Ctrl+C) and SIGTERM.
       * When these signals are received, the server will stop gracefully.
       *
       * @return Reference to this server for method chaining
       */
      inline http_server& with_signals()
      {
         signal_handling_enabled = true;

         // Create signal_set that will handle SIGINT and SIGTERM
         signals_ = std::make_unique<asio::signal_set>(*io_context, SIGINT, SIGTERM);

         // Set up async handler - this properly captures 'this' and integrates with ASIO
         signals_->async_wait([this](std::error_code ec, int signal_number) {
            if (!ec) {
               std::cout << "\nReceived signal " << signal_number << ", shutting down..." << std::endl;
               std::cout.flush();
               this->stop();
            }
         });

         return *this;
      }

      /**
       * @brief Configure connection behavior (keep-alive settings)
       *
       * @param config The connection configuration to use
       * @return Reference to this server for method chaining
       *
       * Example:
       * @code
       * server.connection_settings({
       *    .keep_alive = true,           // Enable persistent connections
       *    .keep_alive_timeout = 30,     // 30 second idle timeout
       *    .max_requests_per_connection = 100  // Close after 100 requests
       * });
       * @endcode
       */
      inline http_server& connection_settings(const connection_config& config)
      {
         conn_config_ = config;
         return *this;
      }

      /**
       * @brief Enable or disable HTTP keep-alive (persistent connections)
       *
       * When disabled, the server sends "Connection: close" with every response
       * and closes the connection after each request/response cycle.
       *
       * @param enable true to enable keep-alive (default), false to disable
       * @return Reference to this server for method chaining
       */
      inline http_server& keep_alive(bool enable)
      {
         conn_config_.keep_alive = enable;
         return *this;
      }

      /**
       * @brief Set the keep-alive timeout for idle connections
       *
       * @param timeout_seconds Seconds to wait for a new request before closing
       * @return Reference to this server for method chaining
       */
      inline http_server& keep_alive_timeout(uint32_t timeout_seconds)
      {
         conn_config_.keep_alive_timeout = timeout_seconds;
         return *this;
      }

      /**
       * @brief Set maximum requests per connection
       *
       * @param max_requests Maximum requests before closing (0 = unlimited)
       * @return Reference to this server for method chaining
       */
      inline http_server& max_requests_per_connection(uint32_t max_requests)
      {
         conn_config_.max_requests_per_connection = max_requests;
         return *this;
      }

      /**
       * @brief Wait for a shutdown signal
       *
       * This method blocks until the server is stopped, either by calling stop()
       * or by receiving a signal if signal handling is enabled with with_signals().
       *
       * @return void
       */
      inline void wait_for_signal()
      {
         std::unique_lock<std::mutex> lock(shutdown_mutex);
         shutdown_cv.wait(lock, [this] { return !running; });

         // After shutdown is signaled, wait for all threads to finish
         lock.unlock();
         for (auto& thread : threads) {
            if (thread.joinable()) {
               thread.join();
            }
         }
         threads.clear();
      }

     private:
      std::shared_ptr<asio::io_context> io_context;
      std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
      std::vector<std::thread> threads;
      http_router root_router;
      bool running = false;
      glz::error_handler error_handler;
      std::unordered_map<std::string, std::shared_ptr<websocket_server>> websocket_handlers_;
      std::unordered_map<std::string, std::unordered_map<http_method, streaming_handler>> streaming_handlers_;

      // Wrapping middleware (executes around handlers)
      std::vector<wrapping_middleware> wrapping_middlewares_;

      // Lifecycle hooks for metrics and logging
      std::vector<request_hook> request_hooks_;
      std::vector<response_hook> response_hooks_;

      // Connection configuration
      connection_config conn_config_;

      // Connection state for managing keep-alive connections
      // Uses socket_type which is either tcp::socket (HTTP) or ssl::stream<tcp::socket> (HTTPS)
      struct connection_state : public std::enable_shared_from_this<connection_state>
      {
         std::shared_ptr<socket_type> socket;
         std::shared_ptr<asio::streambuf> buffer;
         std::string body;
         asio::ip::tcp::endpoint remote_endpoint;
         std::shared_ptr<asio::steady_timer> idle_timer;
         uint32_t request_count = 0;
         bool should_close = false;

         connection_state(std::shared_ptr<socket_type> sock, asio::ip::tcp::endpoint endpoint)
            : socket(std::move(sock)),
              buffer(std::make_shared<asio::streambuf>()),
              remote_endpoint(std::move(endpoint)),
              idle_timer(std::make_shared<asio::steady_timer>(socket->lowest_layer().get_executor()))
         {}
      };

      // Helper for executing wrapping middleware chain
      void execute_middleware_chain(size_t index, request& req, response& res, const handler& h)
      {
         if (index >= wrapping_middlewares_.size()) {
            h(req, res);
            return;
         }

         // Wrap the continuation in next_handler to enforce synchronous execution
         next_handler next([this, index, &req, &res, &h]() { execute_middleware_chain(index + 1, req, res, h); });

         wrapping_middlewares_[index](req, res, next);
      }

      // Signal handling members
      bool signal_handling_enabled = false;
      std::unique_ptr<asio::signal_set> signals_;
      std::condition_variable shutdown_cv;
      std::mutex shutdown_mutex;

#ifdef GLZ_ENABLE_SSL
      std::conditional_t<EnableTLS, std::unique_ptr<asio::ssl::context>, std::monostate> ssl_context;
#endif

      inline void do_accept()
      {
         acceptor->async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
               asio::error_code endpoint_ec;
               auto remote_endpoint = socket.remote_endpoint(endpoint_ec);
               if (endpoint_ec) {
                  error_handler(endpoint_ec, std::source_location::current());
               }
               else {
                  if constexpr (EnableTLS) {
#ifdef GLZ_ENABLE_SSL
                     // For HTTPS: wrap socket in SSL stream and perform handshake
                     auto ssl_socket = std::make_shared<socket_type>(std::move(socket), *ssl_context);
                     ssl_socket->async_handshake(asio::ssl::stream_base::server, [this, ssl_socket, remote_endpoint](
                                                                                    std::error_code handshake_ec) {
                        if (!handshake_ec) {
                           auto conn = std::make_shared<connection_state>(ssl_socket, remote_endpoint);
                           start_connection(conn);
                        }
                        else {
                           // SSL handshake failed - explicitly close the socket
                           error_handler(handshake_ec, std::source_location::current());
                           asio::error_code close_ec;
                           ssl_socket->lowest_layer().close(close_ec);
                        }
                     });
#endif
                  }
                  else {
                     // For HTTP: use TCP socket directly
                     auto socket_ptr = std::make_shared<socket_type>(std::move(socket));
                     auto conn = std::make_shared<connection_state>(std::move(socket_ptr), remote_endpoint);
                     start_connection(conn);
                  }
               }
            }
            else if (running) {
               // Only report errors if we're still running (not shutting down)
               error_handler(ec, std::source_location::current());
            }

            // Continue accepting if still running
            if (running) {
               do_accept();
            }
         });
      }

      // Start handling a new connection
      inline void start_connection(std::shared_ptr<connection_state> conn) { read_request(conn); }

      // Start or reset the idle timer for keep-alive connections
      inline void start_idle_timer(std::shared_ptr<connection_state> conn)
      {
         if (conn_config_.keep_alive_timeout == 0) {
            return; // No timeout configured
         }

         conn->idle_timer->expires_after(std::chrono::seconds(conn_config_.keep_alive_timeout));
         conn->idle_timer->async_wait([conn](std::error_code ec) {
            if (!ec) {
               // Timer expired - close the connection
               std::error_code close_ec;
               conn->socket->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, close_ec);
               conn->socket->lowest_layer().close(close_ec);
            }
            // If ec is operation_aborted, the timer was cancelled (new request arrived)
         });
      }

      // Cancel the idle timer (called when a new request starts)
      inline void cancel_idle_timer(std::shared_ptr<connection_state> conn) { conn->idle_timer->cancel(); }

      // Read the next HTTP request from the connection
      inline void read_request(std::shared_ptr<connection_state> conn)
      {
         // Start idle timer for keep-alive connections (except for first request)
         if (conn->request_count > 0 && conn_config_.keep_alive) {
            start_idle_timer(conn);
         }

         asio::async_read_until(*conn->socket, *conn->buffer, "\r\n\r\n",
                                [this, conn](asio::error_code ec, std::size_t /*bytes_transferred*/) {
                                   // Cancel idle timer since we received data
                                   cancel_idle_timer(conn);

                                   if (ec) {
                                      // EOF is a normal disconnect, not a server error
                                      if (ec != asio::error::eof && ec != asio::error::operation_aborted) {
                                         error_handler(ec, std::source_location::current());
                                      }
                                      return;
                                   }

                                   conn->request_count++;
                                   process_request_data(conn);
                                });
      }

      // Process data received from the connection
      inline void process_request_data(std::shared_ptr<connection_state> conn)
      {
         const auto data_size = conn->buffer->size();
         const char* data_ptr = static_cast<const char*>(conn->buffer->data().data());
         std::string_view request_view(data_ptr, data_size);

         size_t headers_end_pos = request_view.find("\r\n\r\n");
         if (headers_end_pos == std::string_view::npos) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }
         std::string_view headers_part = request_view.substr(0, headers_end_pos);
         size_t body_start_offset = headers_end_pos + 4;

         // Parse request line
         size_t request_line_end_pos = headers_part.find("\r\n");
         std::string_view request_line;
         if (request_line_end_pos == std::string_view::npos) {
            request_line = headers_part; // Request line is the entire headers_part
            headers_part = ""; // headers_part becomes empty as there are no more headers
         }
         else {
            request_line = headers_part.substr(0, request_line_end_pos);
            headers_part.remove_prefix(request_line_end_pos + 2); // +2 for \r\n
         }

         // Parse method, target, and HTTP version from the request line
         size_t first_space = request_line.find(' ');
         if (first_space == std::string_view::npos) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }
         std::string_view method_sv = request_line.substr(0, first_space);
         if (method_sv.empty() ||
             !std::all_of(method_sv.begin(), method_sv.end(), [](char c) { return std::isalnum(c) || c == '_'; })) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }

         size_t second_space = request_line.find(' ', first_space + 1);
         if (second_space == std::string_view::npos) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }
         std::string_view target_sv = request_line.substr(first_space + 1, second_space - first_space - 1);
         if (target_sv.empty() || target_sv.find(' ') != std::string_view::npos) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }

         std::string_view http_version_part = request_line.substr(second_space + 1);
         if (http_version_part.size() > 0 && http_version_part.back() == '\r') {
            http_version_part.remove_suffix(1);
         }
         if (http_version_part.size() < 7 || http_version_part.rfind("HTTP/", 0) != 0) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }
         std::string_view version_number = http_version_part.substr(5);
         size_t dot_pos = version_number.find('.');
         if (dot_pos == std::string_view::npos || dot_pos == 0 || dot_pos == version_number.length() - 1) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }
         std::string_view major_v = version_number.substr(0, dot_pos);
         std::string_view minor_v = version_number.substr(dot_pos + 1);
         if (major_v.empty() || !std::all_of(major_v.begin(), major_v.end(), ::isdigit) || minor_v.empty() ||
             !std::all_of(minor_v.begin(), minor_v.end(), ::isdigit)) {
            send_error_response_with_close(conn, 400, "Bad Request");
            return;
         }

         // Determine HTTP version for keep-alive default behavior
         // HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close
         bool is_http_11 = (major_v == "1" && minor_v == "1");

         auto method_opt = from_string(std::string(method_sv));
         if (!method_opt) {
            send_error_response_with_close(conn, 501, "Not Implemented");
            return;
         }
         std::string target{target_sv};

         // Parse headers
         std::unordered_map<std::string, std::string> headers;
         while (!headers_part.empty()) {
            size_t line_end = headers_part.find("\r\n");
            std::string_view line = headers_part.substr(0, line_end);

            auto colon_pos = line.find(':');
            if (colon_pos != std::string_view::npos) {
               std::string_view name_sv = line.substr(0, colon_pos);
               std::string_view value_sv = line.substr(colon_pos + 1);
               value_sv.remove_prefix(std::min(value_sv.find_first_not_of(" \t"), value_sv.size()));
               headers[to_lower_case(name_sv)] = std::string(value_sv);
            }

            if (line_end == std::string_view::npos) break;
            headers_part.remove_prefix(line_end + 2);
         }

         // Determine if connection should be kept alive based on:
         // 1. Server configuration
         // 2. HTTP version (1.1 = keep-alive by default, 1.0 = close by default)
         // 3. Client's Connection header
         // 4. Max requests per connection limit
         bool should_keep_alive = determine_keep_alive(headers, is_http_11, conn);
         conn->should_close = !should_keep_alive;

         // Consume the parsed headers from the buffer
         conn->buffer->consume(body_start_offset);

         if (is_websocket_upgrade(headers)) {
            // WebSocket upgrades take over the connection
            handle_websocket_upgrade(conn->socket, *method_opt, target, headers, conn->remote_endpoint);
            return;
         }

         std::size_t content_length = 0;
         if (auto it = headers.find("content-length"); it != headers.end()) {
            try {
               content_length = std::stoul(it->second);
            }
            catch (const std::exception&) {
               send_error_response_with_close(conn, 400, "Bad Request");
               return;
            }
         }

         if (content_length > 0) {
            conn->body.resize(content_length);
            // Copy what's already in the buffer to the body
            const size_t initial_body_size = std::min(content_length, conn->buffer->size());
            const auto missing_bytes = content_length - initial_body_size;
            std::memcpy(conn->body.data(), conn->buffer->data().data(), initial_body_size);
            conn->buffer->consume(initial_body_size);
            if (initial_body_size < content_length) {
               asio::async_read(*conn->socket, asio::buffer(&conn->body[initial_body_size], missing_bytes),
                                asio::transfer_exactly(missing_bytes),
                                [this, conn, method_opt, target, headers](std::error_code ec, size_t) {
                                   if (ec) {
                                      error_handler(ec, std::source_location::current());
                                      return;
                                   }
                                   // Append newly read data
                                   process_full_request_with_conn(conn, *method_opt, target, headers);
                                });
            }
            else {
               process_full_request_with_conn(conn, *method_opt, target, headers);
            }
         }
         else {
            process_full_request_with_conn(conn, *method_opt, target, headers);
         }
      }

      // Determine if the connection should be kept alive
      inline bool determine_keep_alive(const std::unordered_map<std::string, std::string>& headers, bool is_http_11,
                                       std::shared_ptr<connection_state> conn)
      {
         // If server has keep-alive disabled, always close
         if (!conn_config_.keep_alive) {
            return false;
         }

         // Check max requests per connection limit
         if (conn_config_.max_requests_per_connection > 0 &&
             conn->request_count >= conn_config_.max_requests_per_connection) {
            return false;
         }

         // Check client's Connection header
         auto conn_header_it = headers.find("connection");
         if (conn_header_it != headers.end()) {
            std::string conn_value = to_lower_case(conn_header_it->second);
            if (conn_value.find("close") != std::string::npos) {
               return false; // Client requested close
            }
            if (conn_value.find("keep-alive") != std::string::npos) {
               return true; // Client explicitly requested keep-alive
            }
         }

         // Default behavior based on HTTP version
         // HTTP/1.1: keep-alive by default
         // HTTP/1.0: close by default
         return is_http_11;
      }

      // Process a full request using connection state (new keep-alive aware version)
      inline void process_full_request_with_conn(std::shared_ptr<connection_state> conn, http_method method,
                                                 const std::string& target,
                                                 const std::unordered_map<std::string, std::string>& headers)
      {
         std::string body = std::move(conn->body);
         // Check for a streaming handler first
         auto handler_it = streaming_handlers_.find(target);
         if (handler_it != streaming_handlers_.end()) {
            auto method_it = handler_it->second.find(method);
            if (method_it != handler_it->second.end()) {
               // Streaming handlers take over the connection (no keep-alive loop)
               handle_streaming_request(conn->socket, method, target, headers, std::move(body), conn->remote_endpoint,
                                        method_it->second);
               return;
            }
         }

         // Create the request object
         request request;
         request.method = method;
         request.target = target;
         request.remote_ip = conn->remote_endpoint.address().to_string();
         request.remote_port = conn->remote_endpoint.port();

         // Parse path and query string from target
         const auto [path_view, query_string] = split_target(target);
         request.path = std::string(path_view);
         request.query = parse_urlencoded(query_string);
         request.headers = headers;
         request.body = std::move(body);

         // Find a matching route
         auto [handle, params] = root_router.match(method, request.path);

         // Create the response object
         response response;

         // Call request hooks for metrics/logging
         try {
            for (const auto& hook : request_hooks_) {
               hook(request, response);
            }
         }
         catch (const std::exception&) {
            error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
            send_error_response_with_close(conn, 500, "Internal Server Error");
            return;
         }

         if (!handle) {
            if (method == http_method::OPTIONS) {
               std::vector<http_method> allowed_methods;
               std::unordered_map<std::string, std::string> preflight_params;

               auto capture_first_params = [&](std::unordered_map<std::string, std::string>&& candidate_params) {
                  if (preflight_params.empty()) {
                     preflight_params = std::move(candidate_params);
                  }
               };

               auto try_method = [&](http_method m) {
                  if (m == http_method::OPTIONS) return;
                  auto [candidate_handle, candidate_params] = root_router.match(m, target);
                  if (candidate_handle) {
                     allowed_methods.push_back(m);
                     capture_first_params(std::move(candidate_params));
                  }
               };

               try_method(http_method::GET);
               try_method(http_method::POST);
               try_method(http_method::PUT);
               try_method(http_method::DELETE);
               try_method(http_method::PATCH);
               try_method(http_method::HEAD);

               if (!allowed_methods.empty()) {
                  request.params = std::move(preflight_params);

                  bool has_request_method_header = false;
                  bool requested_method_known = false;
                  http_method requested_method{};

                  if (auto request_method_it = request.headers.find("access-control-request-method");
                      request_method_it != request.headers.end()) {
                     has_request_method_header = true;
                     std::string method_token = request_method_it->second;
                     auto trim_pos = method_token.find_last_not_of(" \t\r\n");
                     if (trim_pos != std::string::npos) {
                        method_token.erase(trim_pos + 1);
                     }
                     else {
                        method_token.clear();
                     }

                     if (!method_token.empty()) {
                        std::transform(method_token.begin(), method_token.end(), method_token.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

                        if (auto parsed = from_string(method_token)) {
                           requested_method = *parsed;
                           requested_method_known = true;
                        }
                     }
                  }

                  if (std::none_of(allowed_methods.begin(), allowed_methods.end(),
                                   [](http_method m) { return m == http_method::OPTIONS; })) {
                     allowed_methods.push_back(http_method::OPTIONS);
                  }

                  std::string allow_header;
                  allow_header.reserve(allowed_methods.size() * 8);
                  for (size_t i = 0; i < allowed_methods.size(); ++i) {
                     if (i > 0) allow_header.append(", ");
                     allow_header.append(std::string{to_string(allowed_methods[i])});
                  }
                  response.header("Allow", allow_header);

                  bool requested_method_allowed = true;
                  if (has_request_method_header) {
                     requested_method_allowed =
                        requested_method_known && std::find(allowed_methods.begin(), allowed_methods.end(),
                                                            requested_method) != allowed_methods.end();
                  }

                  try {
                     for (const auto& middleware : root_router.middlewares) {
                        middleware(request, response);
                     }
                  }
                  catch (const std::exception&) {
                     error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
                     send_error_response_with_close(conn, 500, "Internal Server Error");
                     return;
                  }

                  if (!requested_method_allowed) {
                     response.status(405);
                     for (const auto& hook : response_hooks_) {
                        try {
                           hook(request, response);
                        }
                        catch (const std::exception&) {
                           error_handler(std::make_error_code(std::errc::invalid_argument),
                                         std::source_location::current());
                        }
                     }
                     send_response_with_conn(conn, response);
                     return;
                  }

                  for (const auto& hook : response_hooks_) {
                     try {
                        hook(request, response);
                     }
                     catch (const std::exception&) {
                        error_handler(std::make_error_code(std::errc::invalid_argument),
                                      std::source_location::current());
                     }
                  }
                  send_response_with_conn(conn, response);
                  return;
               }
            }

            // No matching route found
            send_error_response_with_conn(conn, 404, "Not Found");
            return;
         }

         request.params = std::move(params);

         try {
            for (const auto& middleware : root_router.middlewares) {
               middleware(request, response);
            }

            if (wrapping_middlewares_.empty()) {
               handle(request, response);
            }
            else {
               execute_middleware_chain(0, request, response, handle);
            }

            for (const auto& hook : response_hooks_) {
               try {
                  hook(request, response);
               }
               catch (const std::exception&) {
                  error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
               }
            }

            send_response_with_conn(conn, response);
         }
         catch (const std::exception&) {
            error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
            send_error_response_with_close(conn, 500, "Internal Server Error");
         }
      }

      // Send response with keep-alive support
      inline void send_response_with_conn(std::shared_ptr<connection_state> conn, const response& response)
      {
         size_t estimated_size = 64;

         for (const auto& [name, value] : response.response_headers) {
            estimated_size += name.size() + value.size() + 4;
         }

         estimated_size += response.response_body.size() + 128;

         std::string response_str;
         response_str.reserve(estimated_size);

         response_str.append("HTTP/1.1 ");
         response_str.append(std::to_string(response.status_code));
         response_str.append(" ");
         response_str.append(get_status_message(response.status_code));
         response_str.append("\r\n");

         for (const auto& [name, value] : response.response_headers) {
            response_str.append(name);
            response_str.append(": ");
            response_str.append(value);
            response_str.append("\r\n");
         }

         if (response.response_headers.find("content-length") == response.response_headers.end()) {
            response_str.append("Content-Length: ");
            response_str.append(std::to_string(response.response_body.size()));
            response_str.append("\r\n");
         }

         if (response.response_headers.find("date") == response.response_headers.end()) {
            response_str.append("Date: ");
            response_str.append(get_current_date());
            response_str.append("\r\n");
         }

         if (response.response_headers.find("server") == response.response_headers.end()) {
            response_str.append("Server: Glaze/1.0\r\n");
         }

         // Add Connection header based on keep-alive decision
         if (response.response_headers.find("connection") == response.response_headers.end()) {
            if (conn->should_close) {
               response_str.append("Connection: close\r\n");
            }
            else {
               response_str.append("Connection: keep-alive\r\n");
               // Add Keep-Alive header with timeout info
               if (conn_config_.keep_alive_timeout > 0) {
                  response_str.append("Keep-Alive: timeout=");
                  response_str.append(std::to_string(conn_config_.keep_alive_timeout));
                  if (conn_config_.max_requests_per_connection > 0) {
                     response_str.append(", max=");
                     response_str.append(std::to_string(conn_config_.max_requests_per_connection));
                  }
                  response_str.append("\r\n");
               }
            }
         }

         response_str.append("\r\n");
         response_str.append(response.response_body);

         auto response_buffer = std::make_shared<std::string>(std::move(response_str));

         asio::async_write(*conn->socket, asio::buffer(*response_buffer),
                           [this, conn, response_buffer](std::error_code ec, std::size_t /*bytes_transferred*/) {
                              if (ec) {
                                 // Write error - connection is dead
                                 return;
                              }

                              if (conn->should_close) {
                                 // Close the connection gracefully
                                 std::error_code close_ec;
                                 conn->socket->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, close_ec);
                                 conn->socket->lowest_layer().close(close_ec);
                              }
                              else {
                                 // Keep-alive: read the next request
                                 read_request(conn);
                              }
                           });
      }

      // Send error response with keep-alive support (for normal errors)
      inline void send_error_response_with_conn(std::shared_ptr<connection_state> conn, int status_code,
                                                const std::string& message)
      {
         response resp;
         resp.status(status_code).content_type("text/plain").body(message);
         send_response_with_conn(conn, resp);
      }

      // Send error response and close connection (for protocol errors)
      inline void send_error_response_with_close(std::shared_ptr<connection_state> conn, int status_code,
                                                 const std::string& message)
      {
         conn->should_close = true; // Force close after error
         send_error_response_with_conn(conn, status_code, message);
      }

      inline bool is_websocket_upgrade(const std::unordered_map<std::string, std::string>& headers)
      {
         auto upgrade_it = headers.find("upgrade");
         if (upgrade_it == headers.end()) return false;

         auto connection_it = headers.find("connection");
         if (connection_it == headers.end()) return false;

         // Check if upgrade header contains "websocket" (case-insensitive)
         auto upgrade_value = to_lower_case(upgrade_it->second);
         if (upgrade_value.find("websocket") == std::string::npos) return false;

         // Check if connection header contains "upgrade" (case-insensitive)
         auto connection_value = to_lower_case(connection_it->second);
         return connection_value.find("upgrade") != std::string::npos;
      }

      inline void handle_websocket_upgrade(std::shared_ptr<socket_type> socket, http_method method,
                                           const std::string& target,
                                           const std::unordered_map<std::string, std::string>& headers,
                                           asio::ip::tcp::endpoint remote_endpoint)
      {
         // Find matching WebSocket handler
         auto ws_it = websocket_handlers_.find(target);
         if (ws_it == websocket_handlers_.end()) {
            send_error_response(socket, 404, "Not Found");
            return;
         }

         // Create request object for WebSocket handler
         request req;
         req.method = method;
         req.target = target;
         req.headers = headers;
         req.remote_ip = remote_endpoint.address().to_string();
         req.remote_port = remote_endpoint.port();

         // Create WebSocket connection and start it
         // Uses socket_type which is either tcp::socket (ws://) or ssl::stream (wss://)
         auto ws_conn = std::make_shared<websocket_connection<socket_type>>(socket, ws_it->second);
         ws_conn->start(req);
      }

      inline void handle_streaming_request(std::shared_ptr<socket_type> socket, http_method method,
                                           const std::string& target,
                                           const std::unordered_map<std::string, std::string>& headers,
                                           std::string body, asio::ip::tcp::endpoint remote_endpoint,
                                           const streaming_handler& handler)
      {
         // Create request object
         request req;
         req.method = method;
         req.target = target;
         req.headers = headers;
         req.body = std::move(body);
         req.remote_ip = remote_endpoint.address().to_string();
         req.remote_port = remote_endpoint.port();

         // Create streaming connection (works for both HTTP and HTTPS via interface)
         auto stream_conn = std::make_shared<streaming_connection<socket_type>>(socket);
         streaming_response stream_res(stream_conn);

         try {
            // Call the streaming handler
            handler(req, stream_res);
         }
         catch (const std::exception&) {
            // If handler throws immediately, try to send an error response.
            if (!stream_conn->is_headers_sent()) {
               stream_conn->send_headers(500, {{"Content-Type", "text/plain"}});
               stream_conn->send_chunk("Internal Server Error");
            }
            stream_conn->close();
            // Log the error
            error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
         }
      }

      inline void process_full_request(std::shared_ptr<socket_type> socket, http_method method,
                                       const std::string& target,
                                       const std::unordered_map<std::string, std::string>& headers, std::string body,
                                       asio::ip::tcp::endpoint remote_endpoint)
      {
         // Check for a streaming handler first. This performs an exact match on the path,
         // so parameterized streaming routes are not supported, which is consistent
         // with the WebSocket implementation.
         auto handler_it = streaming_handlers_.find(target);
         if (handler_it != streaming_handlers_.end()) {
            auto method_it = handler_it->second.find(method);
            if (method_it != handler_it->second.end()) {
               // Found a streaming handler, delegate to it and exit.
               handle_streaming_request(socket, method, target, headers, std::move(body), remote_endpoint,
                                        method_it->second);
               return;
            }
         }

         // Create the request object
         request request;
         request.method = method;
         request.target = target;
         request.remote_ip = remote_endpoint.address().to_string();
         request.remote_port = remote_endpoint.port();

         // Parse path and query string from target
         const auto [path_view, query_string] = split_target(target);
         request.path = std::string(path_view);
         request.query = parse_urlencoded(query_string);
         request.headers = headers;
         request.body = std::move(body);

         // Find a matching route using http_router::match which handles both exact and parameterized routes
         auto [handle, params] = root_router.match(method, request.path);

         // Create the response object up front so we can reuse it in fallback flows
         response response;

         // Call request hooks for metrics/logging
         try {
            for (const auto& hook : request_hooks_) {
               hook(request, response);
            }
         }
         catch (const std::exception&) {
            error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
            send_error_response(socket, 500, "Internal Server Error");
            return;
         }

         if (!handle) {
            if (method == http_method::OPTIONS) {
               std::vector<http_method> allowed_methods;
               std::unordered_map<std::string, std::string> preflight_params;

               auto capture_first_params = [&](std::unordered_map<std::string, std::string>&& candidate_params) {
                  if (preflight_params.empty()) {
                     preflight_params = std::move(candidate_params);
                  }
               };

               auto try_method = [&](http_method method) {
                  if (method == http_method::OPTIONS) return;
                  auto [candidate_handle, candidate_params] = root_router.match(method, target);
                  if (candidate_handle) {
                     allowed_methods.push_back(method);
                     capture_first_params(std::move(candidate_params));
                  }
               };

               try_method(http_method::GET);
               try_method(http_method::POST);
               try_method(http_method::PUT);
               try_method(http_method::DELETE);
               try_method(http_method::PATCH);
               try_method(http_method::HEAD);

               if (!allowed_methods.empty()) {
                  request.params = std::move(preflight_params);

                  bool has_request_method_header = false;
                  bool requested_method_known = false;
                  http_method requested_method{};

                  // Parse and normalize the request method from the Access-Control-Request-Method header
                  if (auto request_method_it = request.headers.find("access-control-request-method");
                      request_method_it != request.headers.end()) {
                     has_request_method_header = true;
                     std::string method_token = request_method_it->second;
                     auto trim_pos = method_token.find_last_not_of(" \t\r\n");
                     if (trim_pos != std::string::npos) {
                        method_token.erase(trim_pos + 1);
                     }
                     else {
                        method_token.clear();
                     }

                     if (!method_token.empty()) {
                        std::transform(method_token.begin(), method_token.end(), method_token.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

                        if (auto parsed = from_string(method_token)) {
                           requested_method = *parsed;
                           requested_method_known = true;
                        }
                     }
                  }

                  if (std::none_of(allowed_methods.begin(), allowed_methods.end(),
                                   [](http_method m) { return m == http_method::OPTIONS; })) {
                     allowed_methods.push_back(http_method::OPTIONS);
                  }

                  // Build the Allow header
                  std::string allow_header;
                  allow_header.reserve(allowed_methods.size() * 8);
                  for (size_t i = 0; i < allowed_methods.size(); ++i) {
                     if (i > 0) allow_header.append(", ");
                     allow_header.append(std::string{to_string(allowed_methods[i])});
                  }
                  response.header("Allow", allow_header);

                  bool requested_method_allowed = true;
                  if (has_request_method_header) {
                     requested_method_allowed =
                        requested_method_known && std::find(allowed_methods.begin(), allowed_methods.end(),
                                                            requested_method) != allowed_methods.end();
                  }

                  try {
                     for (const auto& middleware : root_router.middlewares) {
                        middleware(request, response);
                     }
                  }
                  catch (const std::exception&) {
                     error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
                     send_error_response(socket, 500, "Internal Server Error");
                     return;
                  }

                  if (!requested_method_allowed) {
                     response.status(405);
                     // Call response hooks before sending
                     for (const auto& hook : response_hooks_) {
                        try {
                           hook(request, response);
                        }
                        catch (const std::exception&) {
                           error_handler(std::make_error_code(std::errc::invalid_argument),
                                         std::source_location::current());
                        }
                     }
                     send_response(socket, response);
                     return;
                  }

                  // Call response hooks before sending
                  for (const auto& hook : response_hooks_) {
                     try {
                        hook(request, response);
                     }
                     catch (const std::exception&) {
                        error_handler(std::make_error_code(std::errc::invalid_argument),
                                      std::source_location::current());
                     }
                  }
                  send_response(socket, response);
                  return;
               }
            }

            // No matching route found (path or method)
            send_error_response(socket, 404, "Not Found");
            return;
         }

         // Update the request with any extracted parameters
         request.params = std::move(params);

         try {
            // Apply old-style middleware (runs before handler)
            for (const auto& middleware : root_router.middlewares) {
               middleware(request, response);
            }

            // Execute wrapping middleware chain (index-based, no allocations)
            if (wrapping_middlewares_.empty()) {
               handle(request, response);
            }
            else {
               execute_middleware_chain(0, request, response, handle);
            }

            // Call response hooks for metrics/logging
            for (const auto& hook : response_hooks_) {
               try {
                  hook(request, response);
               }
               catch (const std::exception&) {
                  error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());
               }
            }

            // Send the response
            send_response(socket, response);
         }
         catch (const std::exception& e) {
            // Log the exception details for debugging
            std::string error_message = e.what();
            error_handler(std::make_error_code(std::errc::invalid_argument), std::source_location::current());

            // Send a 500 error response back to the client
            send_error_response(socket, 500, "Internal Server Error");
         }
      }

      inline void send_response(std::shared_ptr<socket_type> socket, const response& response)
      {
         // Note: We can't call response hooks here because we don't have access to the request object
         // Response hooks are called in process_full_request before calling this method

         // Pre-calculate response size to avoid reallocations
         size_t estimated_size = 64; // Base size for status line

         for (const auto& [name, value] : response.response_headers) {
            estimated_size += name.size() + value.size() + 4; // ": " + "\r\n"
         }

         estimated_size += response.response_body.size() + 128; // Extra buffer for default headers

         // Use a single string with reserved capacity
         std::string response_str;
         response_str.reserve(estimated_size);

         // Direct string building without streams
         response_str.append("HTTP/1.1 ");
         response_str.append(std::to_string(response.status_code));
         response_str.append(" ");
         response_str.append(get_status_message(response.status_code));
         response_str.append("\r\n");

         for (const auto& [name, value] : response.response_headers) {
            response_str.append(name);
            response_str.append(": ");
            response_str.append(value);
            response_str.append("\r\n");
         }

         // Add default headers if not present (using find is faster than streams)
         if (response.response_headers.find("content-length") == response.response_headers.end()) {
            response_str.append("Content-Length: ");
            response_str.append(std::to_string(response.response_body.size()));
            response_str.append("\r\n");
         }

         if (response.response_headers.find("date") == response.response_headers.end()) {
            response_str.append("Date: ");
            response_str.append(get_current_date());
            response_str.append("\r\n");
         }

         if (response.response_headers.find("server") == response.response_headers.end()) {
            response_str.append("Server: Glaze/1.0\r\n");
         }

         // End headers and add body
         response_str.append("\r\n");
         response_str.append(response.response_body);

         auto response_buffer = std::make_shared<std::string>(std::move(response_str));

         // Send the response asynchronously
         asio::async_write(*socket, asio::buffer(*response_buffer),
                           [socket, response_buffer](std::error_code /*ec*/, std::size_t /*bytes_transferred*/) {
                              // response_buffer keeps the string alive for the duration of the async operation
                              // Socket cleanup handled by shared_ptr
                           });
      }

      inline void send_error_response(std::shared_ptr<socket_type> socket, int status_code, const std::string& message)
      {
         response response;
         response.status(status_code).content_type("text/plain").body(message);

         send_response(socket, response);
      }

      inline std::string_view get_status_message(int status_code)
      {
         switch (status_code) {
         case 200:
            return "OK";
         case 201:
            return "Created";
         case 204:
            return "No Content";
         case 400:
            return "Bad Request";
         case 401:
            return "Unauthorized";
         case 403:
            return "Forbidden";
         case 404:
            return "Not Found";
         case 405:
            return "Method Not Allowed";
         case 409:
            return "Conflict";
         case 418:
            return "I'm a teapot";
         case 500:
            return "Internal Server Error";
         case 501:
            return "Not Implemented";
         case 503:
            return "Service Unavailable";
         default:
            return "Unknown";
         }
      }

      inline std::string get_current_date()
      {
         auto now = std::chrono::system_clock::now();
         auto time_t_now = std::chrono::system_clock::to_time_t(now);

         char buf[100];
         std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&time_t_now));

         return buf;
      }
   };

   // Utility functions for common streaming patterns
   namespace streaming_utils
   {
      // Create a periodic data sender (works with both HTTP and HTTPS)
      template <typename T>
      void send_periodic_data(std::shared_ptr<streaming_connection_interface> conn, std::function<T()> data_generator,
                              std::chrono::milliseconds interval, size_t max_events = 0)
      {
         auto counter = std::make_shared<size_t>(0);
         if (!conn || !conn->is_open()) return;
         auto timer = std::make_shared<asio::steady_timer>(conn->get_executor());

         // Use shared_ptr to safely handle recursive lambda calls and avoid compiler-specific segfaults
         auto send_next = std::make_shared<std::function<void()>>();
         *send_next = [conn, timer, counter, data_generator, interval, max_events, send_next]() mutable {
            if (!conn->is_open() || (max_events > 0 && *counter >= max_events)) {
               conn->close();
               return;
            }

            try {
               T data = data_generator();
               conn->send_json_event(data, "data", std::to_string(*counter), [=](std::error_code ec) mutable {
                  if (!ec) {
                     (*counter)++;
                     timer->expires_after(interval);
                     timer->async_wait([send_next, timer, counter](std::error_code) { (*send_next)(); });
                  }
                  else {
                     conn->close();
                  }
               });
            }
            catch (const std::exception&) {
               conn->close();
            }
         };

         (*send_next)();
      }

      // Create a data stream from a collection (works with both HTTP and HTTPS)
      template <typename Container>
      void stream_collection(std::shared_ptr<streaming_connection_interface> conn, const Container& data,
                             std::chrono::milliseconds delay_between_items = std::chrono::milliseconds(10))
      {
         auto it = std::make_shared<typename Container::const_iterator>(data.begin());
         auto end_it = data.end();
         if (!conn || !conn->is_open()) return;
         auto timer = std::make_shared<asio::steady_timer>(conn->get_executor());

         // Use shared_ptr to safely handle recursive lambda calls and avoid compiler-specific segfaults
         auto send_next = std::make_shared<std::function<void()>>();
         *send_next = [conn, timer, it, end_it, delay_between_items, send_next]() mutable {
            if (!conn->is_open() || *it == end_it) {
               conn->close();
               return;
            }

            conn->send_json_event(**it, "item", "", [=](std::error_code ec) mutable {
               if (!ec) {
                  ++(*it);
                  timer->expires_after(delay_between_items);
                  timer->async_wait([send_next, timer, it](std::error_code) { (*send_next)(); });
               }
               else {
                  conn->close();
               }
            });
         };

         (*send_next)();
      }
   } // namespace streaming_utils

   // Alias for HTTPS server
   using https_server = http_server<true>;
}
