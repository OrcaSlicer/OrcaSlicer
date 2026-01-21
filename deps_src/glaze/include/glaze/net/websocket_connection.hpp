// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Optional OpenSSL support - detected at compile time
#if defined(GLZ_ENABLE_OPENSSL) && __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#define GLZ_HAS_OPENSSL

// To deconflict Windows.h
#ifdef DELETE
#undef DELETE
#endif
#endif

#include "glaze/base64/base64.hpp"
#include "glaze/ext/glaze_asio.hpp"
#include "glaze/net/http_router.hpp"
#include "glaze/util/parse.hpp"

#ifdef GLZ_ENABLE_SSL
#include <asio/ssl.hpp>
#endif

namespace glz
{
   // WebSocket opcode constants
   enum class ws_opcode : uint8_t { continuation = 0x0, text = 0x1, binary = 0x2, close = 0x8, ping = 0x9, pong = 0xa };

   // WebSocket close codes
   enum class ws_close_code : uint16_t {
      normal = 1000,
      going_away = 1001,
      protocol_error = 1002,
      unsupported_data = 1003,
      invalid_payload = 1007,
      policy_violation = 1008,
      message_too_big = 1009,
      mandatory_extension = 1010,
      internal_error = 1011
   };

   // WebSocket frame header helper
   struct ws_frame_header
   {
      uint8_t data[2];

      ws_frame_header() { reset(); }

      void reset()
      {
         data[0] = 0;
         data[1] = 0;
      }

      void fin(bool v) { data[0] = (data[0] & ~0x80) | (v ? 0x80 : 0); }
      void opcode(ws_opcode v) { data[0] = (data[0] & ~0x0F) | (static_cast<uint8_t>(v) & 0x0F); }
      void mask(bool v) { data[1] = (data[1] & ~0x80) | (v ? 0x80 : 0); }
      void payload_len(uint8_t v) { data[1] = (data[1] & ~0x7F) | (v & 0x7F); }

      bool fin() const { return (data[0] & 0x80) != 0; }
      ws_opcode opcode() const { return static_cast<ws_opcode>(data[0] & 0x0F); }
      bool mask() const { return (data[1] & 0x80) != 0; }
      uint8_t payload_len() const { return data[1] & 0x7F; }
   };

   // WebSocket utilities
   namespace ws_util
   {
#if !defined(GLZ_ENABLE_OPENSSL)
      // Fallback SHA-1 implementation when OpenSSL is not available
      namespace fallback_sha1
      {
         struct sha1_context
         {
            uint32_t state[5];
            uint32_t count[2];
            uint8_t buffer[64];
         };

         inline void sha1_init(sha1_context* context)
         {
            context->state[0] = 0x67452301;
            context->state[1] = 0xEFCDAB89;
            context->state[2] = 0x98BADCFE;
            context->state[3] = 0x10325476;
            context->state[4] = 0xC3D2E1F0;
            context->count[0] = context->count[1] = 0;
         }

         inline void sha1_process(sha1_context* context, const uint8_t data[64])
         {
            uint32_t w[80], a, b, c, d, e, temp;

            for (int i = 0; i < 16; i++) {
               w[i] = (data[i * 4] << 24) | (data[i * 4 + 1] << 16) | (data[i * 4 + 2] << 8) | data[i * 4 + 3];
            }

            for (int i = 16; i < 80; i++) {
               w[i] = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
               w[i] = (w[i] << 1) | (w[i] >> 31);
            }

            a = context->state[0];
            b = context->state[1];
            c = context->state[2];
            d = context->state[3];
            e = context->state[4];

            for (int i = 0; i < 80; i++) {
               if (i < 20) {
                  temp = ((a << 5) | (a >> 27)) + ((b & c) | (~b & d)) + e + w[i] + 0x5A827999;
               }
               else if (i < 40) {
                  temp = ((a << 5) | (a >> 27)) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
               }
               else if (i < 60) {
                  temp = ((a << 5) | (a >> 27)) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDC;
               }
               else {
                  temp = ((a << 5) | (a >> 27)) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
               }

               e = d;
               d = c;
               c = (b << 30) | (b >> 2);
               b = a;
               a = temp;
            }

            context->state[0] += a;
            context->state[1] += b;
            context->state[2] += c;
            context->state[3] += d;
            context->state[4] += e;
         }

         inline void sha1_update(sha1_context* context, const uint8_t* data, size_t len)
         {
            size_t i = 0;
            size_t j = (context->count[0] >> 3) & 63;

            if ((context->count[0] += uint32_t(len << 3)) < (len << 3)) context->count[1]++;
            context->count[1] += (len >> 29);

            if ((j + len) > 63) {
               std::memcpy(&context->buffer[j], data, (i = 64 - j));
               sha1_process(context, context->buffer);
               for (; i + 63 < len; i += 64) {
                  sha1_process(context, &data[i]);
               }
               j = 0;
            }

            std::memcpy(&context->buffer[j], &data[i], len - i);
         }

         inline void sha1_final(sha1_context* context, uint8_t digest[20])
         {
            uint8_t finalcount[8];

            for (int i = 0; i < 8; i++) {
               finalcount[i] = (uint8_t)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
            }

            sha1_update(context, (uint8_t*)"\200", 1);
            while ((context->count[0] & 504) != 448) {
               sha1_update(context, (uint8_t*)"\0", 1);
            }

            sha1_update(context, finalcount, 8);

            for (int i = 0; i < 20; i++) {
               digest[i] = (uint8_t)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
            }
         }
      }
#endif

      // Generate WebSocket accept key from client key
      inline std::string generate_accept_key(std::string_view client_key)
      {
         std::string combined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

         unsigned char hash[20];

#if defined(GLZ_ENABLE_OPENSSL) && defined(GLZ_HAS_OPENSSL)
         // Use OpenSSL when available
#if OPENSSL_VERSION_NUMBER <= 0x030000000L
         SHA_CTX sha1;
         SHA1_Init(&sha1);
         SHA1_Update(&sha1, combined.data(), combined.size());
         SHA1_Final(hash, &sha1);
#else
         EVP_MD_CTX* sha1 = EVP_MD_CTX_new();
         EVP_DigestInit_ex(sha1, EVP_sha1(), NULL);
         EVP_DigestUpdate(sha1, combined.data(), combined.size());
         EVP_DigestFinal_ex(sha1, hash, NULL);
         EVP_MD_CTX_free(sha1);
#endif
#else
         // Use fallback implementation when OpenSSL is not available
         fallback_sha1::sha1_context ctx;
         fallback_sha1::sha1_init(&ctx);
         fallback_sha1::sha1_update(&ctx, reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
         fallback_sha1::sha1_final(&ctx, hash);
#endif

         return glz::write_base64(std::string_view{reinterpret_cast<char*>(hash), sizeof(hash)});
      }

      // Check if a string contains a value (case-insensitive, comma-separated)
      inline bool header_contains(std::string_view header, std::string_view value)
      {
         while (!header.empty()) {
            // Skip whitespace
            while (!header.empty() && (header.front() == ' ' || header.front() == '\t')) {
               header.remove_prefix(1);
            }

            if (header.empty()) break;

            // Find the end of this token
            auto comma_pos = header.find(',');
            std::string_view token = header.substr(0, comma_pos);

            // Remove trailing whitespace from token
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
               token.remove_suffix(1);
            }

            // Case-insensitive comparison
            if (token.size() == value.size() &&
                std::equal(token.begin(), token.end(), value.begin(), value.end(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
               return true;
            }

            if (comma_pos == std::string_view::npos) break;
            header.remove_prefix(comma_pos + 1);
         }

         return false;
      }
   }

   // Forward declarations
   template <typename SocketType = asio::ip::tcp::socket>
   struct websocket_connection;

   // Base class for type-erased connection closing
   struct closeable_connection
   {
      virtual ~closeable_connection() = default;
      virtual void force_close() = 0;
   };

   // Type-erased interface for WebSocket connections.
   // Allows handlers to work with both TCP (ws://) and SSL (wss://) WebSocket connections.
   //
   // NOTE: Handler types now use websocket_connection_interface instead of the concrete
   // websocket_connection<tcp::socket>. Using `auto` in lambdas still works:
   //   ws_server->on_message([](auto conn, std::string_view msg, ws_opcode op) {
   //      conn->send_text("reply");  // interface methods work fine
   //   });
   // Only code that explicitly typed the concrete type or accessed internal members
   // (like socket_) needs to be updated.
   struct websocket_connection_interface : public closeable_connection
   {
      // Send operations
      virtual void send_text(std::string_view message) = 0;
      virtual void send_binary(std::string_view message) = 0;
      virtual void send_ping(std::string_view payload = {}) = 0;
      virtual void send_pong(std::string_view payload = {}) = 0;

      // Close the connection
      virtual void close(ws_close_code code = ws_close_code::normal, std::string_view reason = {}) = 0;

      // Connection info
      virtual std::string remote_address() const = 0;
      virtual uint16_t remote_port() const = 0;

      // User data
      virtual void set_user_data(std::shared_ptr<void> data) = 0;
      virtual std::shared_ptr<void> get_user_data() const = 0;

      // Close info (valid after connection is closed)
      virtual ws_close_code get_close_code() const = 0;
      virtual std::string_view get_close_reason() const = 0;
   };

   // WebSocket server class
   //
   // Message Handler Lifetime:
   // The std::string_view passed to message handlers is only valid for the duration
   // of the callback. The view points directly into internal receive buffers which
   // are reused after the handler returns. If you need to retain the message data
   // beyond the callback (e.g., for async processing), you must copy it:
   //
   //   ws_server->on_message([](auto conn, std::string_view msg, ws_opcode op) {
   //      std::string retained(msg);  // Copy if needed beyond this callback
   //      // ... use msg directly for synchronous processing ...
   //   });
   //
   struct websocket_server
   {
      // Handler types use websocket_connection_interface for type erasure.
      // This allows handlers to work with both TCP (ws://) and SSL (wss://) connections.
      using message_handler =
         std::function<void(std::shared_ptr<websocket_connection_interface>, std::string_view, ws_opcode)>;
      using close_handler =
         std::function<void(std::shared_ptr<websocket_connection_interface>, ws_close_code, std::string_view)>;
      using error_handler = std::function<void(std::shared_ptr<websocket_connection_interface>, std::error_code)>;
      using open_handler = std::function<void(std::shared_ptr<websocket_connection_interface>, const request&)>;
      using validate_handler = std::function<bool(const request&)>;

      websocket_server() = default;

      ~websocket_server() { close_all_connections(); }

      // Close all active connections - called on server shutdown
      void close_all_connections()
      {
         std::lock_guard<std::mutex> lock(connections_mutex_);
         for (auto& weak_conn : connections_) {
            if (auto conn = weak_conn.lock()) {
               conn->force_close();
            }
         }
         connections_.clear();
      }

      // Register a connection for tracking (called by websocket_connection)
      void register_connection(std::shared_ptr<closeable_connection> conn)
      {
         std::lock_guard<std::mutex> lock(connections_mutex_);
         connections_.push_back(conn);
      }

      // Remove expired weak_ptrs (optional cleanup, called periodically or on unregister)
      void cleanup_expired_connections()
      {
         std::lock_guard<std::mutex> lock(connections_mutex_);
         connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                           [](const std::weak_ptr<closeable_connection>& w) { return w.expired(); }),
                            connections_.end());
      }

      // Configuration
      void set_max_message_size(size_t size) { max_message_size_ = size; }
      size_t get_max_message_size() const { return max_message_size_; }

      // Set message handler
      inline void on_message(message_handler handler) { message_handler_ = std::move(handler); }

      // Set connection handler
      inline void on_open(open_handler handler) { open_handler_ = std::move(handler); }

      // Set close handler
      inline void on_close(close_handler handler) { close_handler_ = std::move(handler); }

      // Set error handler
      inline void on_error(error_handler handler) { error_handler_ = std::move(handler); }

      // Set connection validator
      inline void on_validate(validate_handler handler) { validate_handler_ = std::move(handler); }

      // Internal methods called by websocket_connection (templated to accept any socket type)
      template <typename SocketType>
      inline void notify_open(std::shared_ptr<websocket_connection<SocketType>> conn, const request& req)
      {
         if (open_handler_) {
            open_handler_(conn, req);
         }
      }

      template <typename SocketType>
      inline void notify_message(std::shared_ptr<websocket_connection<SocketType>> conn, std::string_view message,
                                 ws_opcode opcode)
      {
         if (message_handler_) {
            message_handler_(conn, message, opcode);
         }
      }

      template <typename SocketType>
      inline void notify_close(std::shared_ptr<websocket_connection<SocketType>> conn, ws_close_code code,
                               std::string_view reason)
      {
         if (close_handler_) {
            close_handler_(conn, code, reason);
         }
      }

      template <typename SocketType>
      inline void notify_error(std::shared_ptr<websocket_connection<SocketType>> conn, std::error_code ec)
      {
         if (error_handler_) {
            error_handler_(conn, ec);
         }
      }

      inline bool validate_connection(const request& req)
      {
         if (validate_handler_) {
            return validate_handler_(req);
         }
         return true;
      }

     private:
      size_t max_message_size_{1024 * 1024 * 16};
      open_handler open_handler_;
      message_handler message_handler_;
      close_handler close_handler_;
      error_handler error_handler_;
      validate_handler validate_handler_;

      // Connection tracking for clean shutdown
      std::mutex connections_mutex_;
      std::vector<std::weak_ptr<closeable_connection>> connections_;
   };

   // WebSocket connection class - implementations come after websocket_server
   //
   // Thread-safety: send_text(), send_binary(), send_ping(), send_pong(), and close()
   // are thread-safe and may be called concurrently from multiple threads.
   // Outgoing frames are serialized via an internal write queue.
   //
   // Message Handler Lifetime: The std::string_view passed to on_message callbacks
   // is only valid for the duration of the callback. Copy the data if you need to
   // retain it beyond the callback scope.
   template <typename SocketType>
   struct websocket_connection : public websocket_connection_interface,
                                 public std::enable_shared_from_this<websocket_connection<SocketType>>
   {
     public:
      inline websocket_connection(std::shared_ptr<SocketType> socket,
                                  std::weak_ptr<websocket_server> server = std::weak_ptr<websocket_server>{})
         : socket_(socket), server_(std::move(server))
      {
         // Try to get remote endpoint, but don't fail if it's not available yet
         try {
            remote_endpoint_ = socket_->lowest_layer().remote_endpoint();
         }
         catch (...) {
            // Endpoint not available yet (e.g., for client mode before connection)
            // Will be set later if needed
         }
         if (auto srv = server_.lock()) {
            max_message_size_ = srv->get_max_message_size();
         }
      }

      ~websocket_connection()
      {
         // Clear handlers first to prevent any callbacks during destruction.
         // Don't close the socket here - let it close naturally when the shared_ptr
         // is destroyed. Closing during destruction can cause issues if this destructor
         // runs during io_context destruction (cancelled operations try to interact
         // with a destroying io_context).
         client_message_handler_ = nullptr;
         client_close_handler_ = nullptr;
         client_error_handler_ = nullptr;
      }

      // Configuration
      void set_max_message_size(size_t size) { max_message_size_ = size; }

      // Start the WebSocket connection (performs handshake)
      inline void start(const request& req) { perform_handshake(req); }

      // Send a text message
      void send_text(std::string_view message) override { send_frame(ws_opcode::text, message); }

      // Send a binary message
      void send_binary(std::string_view message) override { send_frame(ws_opcode::binary, message); }

      // Send a ping frame
      void send_ping(std::string_view payload = {}) override { send_frame(ws_opcode::ping, payload); }

      // Send a pong frame
      void send_pong(std::string_view payload = {}) override { send_frame(ws_opcode::pong, payload); }

      // Close the connection
      // RFC 6455 Section 7.1.2: "To Start the WebSocket Closing Handshake with a status code,
      // an endpoint MUST send a Close control frame"
      // https://datatracker.ietf.org/doc/html/rfc6455#section-7.1.2
      void close(ws_close_code code = ws_close_code::normal, std::string_view reason = {}) override
      {
         // Atomically check and set is_closing_ to prevent race conditions
         bool expected = false;
         if (!is_closing_.compare_exchange_strong(expected, true)) {
            return; // Another thread is already closing
         }

         // Store close code for the callback
         close_code_ = code;

         // Send close frame but DON'T close socket immediately.
         // RFC 6455 Section 7.1.1: "Once an endpoint has both sent and received a Close control
         // frame, that endpoint SHOULD Close the WebSocket Connection"
         // The socket will be closed when we receive the close response in handle_frame().
         send_close_frame(code, reason, false);
      }

      // Force close the connection immediately (closes socket without handshake).
      // Used during server shutdown to ensure clean destruction.
      void force_close() override
      {
         is_closing_ = true;
         // ALWAYS clear handlers under mutex to prevent callbacks with dangling references.
         // This races with do_close() which also holds the mutex while calling handlers.
         // Mutual exclusion ensures either handlers are called OR cleared, never both.
         {
            std::lock_guard<std::mutex> lock(close_handler_mutex_);
            client_message_handler_ = nullptr;
            client_close_handler_ = nullptr;
            client_error_handler_ = nullptr;
         }
         closed_ = true;
         // Hold socket_op_mutex_ while closing to prevent race with async_read_some/async_write
         // setup in do_start_read/do_write. ASIO socket operations are NOT thread-safe.
         {
            std::lock_guard<std::mutex> lock(socket_op_mutex_);
            if (socket_) {
               asio::error_code ec;
               socket_->lowest_layer().cancel(ec);
               socket_->lowest_layer().close(ec);
               socket_.reset();
            }
         }
      }

      // Get remote endpoint information
      std::string remote_address() const override { return remote_endpoint_.address().to_string(); }

      uint16_t remote_port() const override { return remote_endpoint_.port(); }

      // Set user data
      void set_user_data(std::shared_ptr<void> data) override { user_data_ = data; }
      std::shared_ptr<void> get_user_data() const override { return user_data_; }

      // Get close code and reason (valid after connection is closed)
      ws_close_code get_close_code() const override { return close_code_; }
      std::string_view get_close_reason() const override { return close_reason_; }

      // Inject initial data read during handshake
      inline void set_initial_data(std::string_view data)
      {
         frame_buffer_.insert(frame_buffer_.end(), data.begin(), data.end());
         size_t consumed = process_frames(frame_buffer_.data(), frame_buffer_.size());
         if (consumed > 0) {
            frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + consumed);
         }
      }

     private:
      size_t max_message_size_{1024 * 1024 * 16}; // 16 MB limit

      inline void perform_handshake(const request& req)
      {
         // Validate WebSocket upgrade request
         auto it = req.headers.find("upgrade");
         constexpr std::string_view websocket_str = "websocket";
         if (it == req.headers.end() ||
             !std::equal(it->second.begin(), it->second.end(), websocket_str.begin(), websocket_str.end(),
                         [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            do_close();
            return;
         }

         it = req.headers.find("connection");
         if (it == req.headers.end() || !ws_util::header_contains(it->second, "upgrade")) {
            do_close();
            return;
         }

         it = req.headers.find("sec-websocket-version");
         if (it == req.headers.end() || it->second != "13") {
            do_close();
            return;
         }

         it = req.headers.find("sec-websocket-key");
         if (it == req.headers.end()) {
            do_close();
            return;
         }

         // Check if server wants to validate this connection
         if (auto srv = server_.lock()) {
            if (!srv->validate_connection(req)) {
               do_close();
               return;
            }
         }

         // Generate accept key
         std::string accept_key = ws_util::generate_accept_key(it->second);

         // Send handshake response
         std::string response_str =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
         response_str.append(accept_key);
         response_str.append("\r\n\r\n");

         auto self = this->shared_from_this();

         // Hold socket_op_mutex_ while setting up the async write operation.
         // This prevents close() from being called while async_write() is setting up.
         std::lock_guard<std::mutex> sock_lock(socket_op_mutex_);

         // Capture socket locally to prevent race condition
         auto socket = socket_;
         // Check both pointer validity AND that socket is still open
         if (!socket || !socket->lowest_layer().is_open()) {
            return;
         }

         // Use shared_ptr to keep response string alive during async operation
         auto response_buffer = std::make_shared<std::string>(std::move(response_str));
         asio::async_write(*socket, asio::buffer(*response_buffer),
                           [self, req, response_buffer, socket](std::error_code ec, std::size_t) {
                              if (ec) {
                                 self->is_closing_ = true;
                                 if (auto srv = self->server_.lock()) {
                                    srv->notify_error(self, ec);
                                 }
                                 self->do_close();
                                 return;
                              }

                              self->handshake_complete_ = true;

                              // Register with server for clean shutdown tracking
                              if (auto srv = self->server_.lock()) {
                                 srv->register_connection(self);
                                 srv->notify_open(self, req);
                              }

                              // Start reading frames
                              self->start_read();
                           });
      }

      inline void on_read(std::error_code ec, std::size_t bytes_transferred)
      {
         if (ec) {
            is_closing_ = true;
            // Only call error handlers if not already closed (prevents calling handlers
            // after force_close() when user's captured references may be invalid)
            if (!closed_.load()) {
               dispatch_error(ec);
            }
            // Close the connection after a read error (connection is likely broken)
            do_close();
            return;
         }

         // Add received data to frame buffer
         frame_buffer_.insert(frame_buffer_.end(), read_buffer_.begin(), read_buffer_.begin() + bytes_transferred);

         // Check buffer size limit (simple DoS protection)
         if (frame_buffer_.size() > max_message_size_ + 1024) { // Allow some header overhead
            close(ws_close_code::message_too_big, "Buffer limit exceeded");
            return;
         }

         // Process complete frames
         size_t consumed = process_frames(frame_buffer_.data(), frame_buffer_.size());
         if (consumed > 0) {
            frame_buffer_.erase(frame_buffer_.begin(), frame_buffer_.begin() + consumed);
         }

         // Continue reading if connection is still open
         // RFC 6455 Section 7.1.1: The initiator must continue reading to receive the peer's
         // Close frame response before closing the TCP connection.
         // Use closed_ (not is_closing_) to allow receiving close response during handshake.
         // https://datatracker.ietf.org/doc/html/rfc6455#section-7.1.1
         if (!closed_.load() && handshake_complete_) {
            start_read();
         }
      }

      inline size_t process_frames(uint8_t* data, std::size_t length)
      {
         std::size_t offset = 0;

         while (offset < length) {
            // Need at least 2 bytes for basic header
            if (length - offset < 2) {
               break;
            }

            ws_frame_header header;
            header.data[0] = data[offset];
            header.data[1] = data[offset + 1];
            size_t header_size = 2;

            // Check reserved bits
            if ((header.data[0] & 0x70) != 0) {
               close(ws_close_code::protocol_error, "Reserved bits set");
               return length; // Consume all
            }

            // Get payload length
            uint64_t payload_length = header.payload_len();

            if (payload_length == 126) {
               if (length - offset < 4) break;
               payload_length = (static_cast<uint64_t>(data[offset + 2]) << 8) | data[offset + 3];
               header_size += 2;
            }
            else if (payload_length == 127) {
               if (length - offset < 10) break;
               payload_length = 0;
               for (int i = 0; i < 8; ++i) {
                  payload_length = (payload_length << 8) | data[offset + 2 + i];
               }
               header_size += 8;
            }

            if (payload_length > max_message_size_) {
               close(ws_close_code::message_too_big, "Message too big");
               return length;
            }

            // Read mask key if present
            std::array<uint8_t, 4> mask_key{};
            if (header.mask()) {
               if (length - offset < header_size + 4) break;
               std::copy(data + offset + header_size, data + offset + header_size + 4, mask_key.begin());
               header_size += 4;
            }

            // Check if we have the complete payload
            if (length - offset < header_size + payload_length) {
               break;
            }

            // Get pointer to payload in frame buffer and unmask in-place
            uint8_t* payload_ptr = data + offset + header_size;
            const auto payload_size = static_cast<size_t>(payload_length);

            if (header.mask() && payload_size > 0) {
               for (std::size_t i = 0; i < payload_size; ++i) {
                  payload_ptr[i] ^= mask_key[i % 4];
               }
            }

            // Handle the frame
            handle_frame(header.opcode(), payload_ptr, payload_size, header.fin());

            offset += header_size + static_cast<size_t>(payload_length);
         }
         return offset;
      }

      inline void handle_frame(ws_opcode opcode, const uint8_t* payload, std::size_t length, bool fin)
      {
         switch (opcode) {
         case ws_opcode::text:
         case ws_opcode::binary:
            if (is_reading_frame_) {
               close(ws_close_code::protocol_error, "Unexpected data frame");
               return;
            }

            if (length > max_message_size_) {
               close(ws_close_code::message_too_big, "Message too big");
               return;
            }

            if (fin) {
               // Complete single-frame message - deliver directly (zero-copy)
               if (opcode == ws_opcode::text) {
                  if (!glz::validate_utf8(payload, length)) {
                     close(ws_close_code::invalid_payload, "Invalid UTF-8");
                     return;
                  }
               }

               std::string_view message_view(reinterpret_cast<const char*>(payload), length);
               // Only call message handlers if not closed (prevents calling handlers
               // after force_close() when user's captured references may be invalid)
               if (!closed_.load()) {
                  dispatch_message(message_view, opcode);
               }
            }
            else {
               // Fragmented message - must buffer for reassembly
               current_opcode_ = opcode;
               message_buffer_.assign(payload, payload + length);
               is_reading_frame_ = true;
            }
            break;

         case ws_opcode::continuation:
            if (!is_reading_frame_) {
               close(ws_close_code::protocol_error, "Unexpected continuation frame");
               return;
            }

            if (message_buffer_.size() + length > max_message_size_) {
               close(ws_close_code::message_too_big, "Message too big");
               return;
            }

            message_buffer_.insert(message_buffer_.end(), payload, payload + length);

            if (fin) {
               is_reading_frame_ = false;

               // Validate UTF-8 for text frames
               if (current_opcode_ == ws_opcode::text) {
                  if (!glz::validate_utf8(message_buffer_.data(), message_buffer_.size())) {
                     close(ws_close_code::invalid_payload, "Invalid UTF-8");
                     return;
                  }
               }

               std::string_view message_view(reinterpret_cast<const char*>(message_buffer_.data()),
                                             message_buffer_.size());
               // Only call message handlers if not closed (prevents calling handlers
               // after force_close() when user's captured references may be invalid)
               if (!closed_.load()) {
                  dispatch_message(message_view, current_opcode_);
               }
               message_buffer_.clear();
               current_opcode_ = ws_opcode::continuation;
            }
            break;

         // RFC 6455 Section 5.5.1: "If an endpoint receives a Close frame and did not previously
         // send a Close frame, the endpoint MUST send a Close frame in response."
         // https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.1
         case ws_opcode::close: {
            ws_close_code code = ws_close_code::normal;
            std::string reason;

            if (length >= 2) {
               code = static_cast<ws_close_code>((payload[0] << 8) | payload[1]);
               if (length > 2) {
                  reason = std::string(reinterpret_cast<const char*>(payload + 2), length - 2);
               }
            }

            // Store close code and reason for callback
            close_code_ = code;
            close_reason_ = std::move(reason);

            // Atomically check and set is_closing_ to handle race with close() method
            bool expected = false;
            if (is_closing_.compare_exchange_strong(expected, true)) {
               // Peer initiated close - we successfully marked as closing, send response
               // RFC 6455: "the endpoint MUST send a Close frame in response"
               // Send close frame and schedule socket close after write completes
               send_close_frame(code, {}, true);
            }
            else {
               // We initiated close, peer responded - close socket now
               // RFC 6455 Section 7.1.1: "Once an endpoint has both sent and received a Close
               // control frame, that endpoint SHOULD Close the WebSocket Connection"
               do_close();
            }
         } break;

         case ws_opcode::ping:
            send_pong(std::string_view(reinterpret_cast<const char*>(payload), length));
            break;

         case ws_opcode::pong:
            // Pong frames are just ignored
            break;

         default:
            close(ws_close_code::protocol_error, "Unknown opcode");
            break;
         }
      }

      inline void send_frame(ws_opcode opcode, std::string_view payload, bool fin = true, bool close_after = false)
      {
         // Allow close frames to be sent even when closing, but block other frame types
         if (is_closing_ && opcode != ws_opcode::close) {
            return;
         }

         // Capture socket locally to prevent race condition
         auto socket = socket_;
         if (!socket) {
            // Socket already closed - if close was requested, ensure do_close() is called
            if (close_after) {
               do_close();
            }
            return;
         }

         std::size_t header_size = get_frame_header_size(payload.size(), client_mode_);
         auto frame_buffer = std::make_unique<std::vector<uint8_t>>(header_size + payload.size());

         write_frame_header(opcode, payload.size(), fin, frame_buffer->data(), client_mode_);
         std::copy(payload.begin(), payload.end(), frame_buffer->begin() + header_size);

         // Apply masking if in client mode
         if (client_mode_ && payload.size() > 0) {
            // Get the masking key from the header
            std::size_t mask_key_offset = header_size - 4;
            const uint8_t* mask_key = frame_buffer->data() + mask_key_offset;
            // Mask the payload
            for (std::size_t i = 0; i < payload.size(); ++i) {
               (*frame_buffer)[header_size + i] ^= mask_key[i % 4];
            }
         }

         // Queue the frame and start writing if not already in progress
         {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_queue_.push_back(std::move(frame_buffer));
            if (close_after) {
               close_after_write_ = true; // Set atomically with queuing
            }
            if (write_in_progress_) {
               return; // Another write is in progress, frame will be sent when it completes
            }
            write_in_progress_ = true;
         }

         // Start writing the queued frame
         do_write();
      }

      // Process the write queue - called when a write completes or when starting a new write
      inline void do_write()
      {
         std::unique_ptr<std::vector<uint8_t>> frame_buffer;
         bool should_close_after = false;

         {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (write_queue_.empty()) {
               write_in_progress_ = false;
               should_close_after = close_after_write_;
               close_after_write_ = false;
            }
            else {
               frame_buffer = std::move(write_queue_.front());
               write_queue_.pop_front();
            }
         }

         // Handle close-after-write when queue is empty
         if (!frame_buffer) {
            if (should_close_after) {
               schedule_close();
            }
            return;
         }

         // Hold socket_op_mutex_ while setting up the async write operation.
         // This prevents close() from being called while async_write() is setting up.
         // ASIO socket operations are NOT thread-safe for concurrent calls.
         std::lock_guard<std::mutex> sock_lock(socket_op_mutex_);

         auto socket = socket_;
         // Check both pointer validity AND that socket is still open.
         // The socket could be closed (is_open() == false) while the shared_ptr is still valid.
         // Calling async_write on a closed socket crashes in ASIO's reactor.
         if (!socket || !socket->lowest_layer().is_open()) {
            // Socket was closed externally - ensure close callback fires if pending
            bool had_close_pending = false;
            {
               std::lock_guard<std::mutex> lock(write_mutex_);
               write_in_progress_ = false;
               had_close_pending = close_after_write_;
               write_queue_.clear();
               close_after_write_ = false;
            }
            if (had_close_pending) {
               do_close(); // Ensure callback fires (idempotent via closed_ CAS)
            }
            return;
         }

         auto self = this->shared_from_this();
         // Create buffer view before moving frame_buffer into lambda (C++14 evaluation order safety)
         auto buffer_view = asio::buffer(*frame_buffer);
         asio::async_write(*socket, buffer_view,
                           [self, socket, frame_buffer = std::move(frame_buffer)](std::error_code ec, std::size_t) {
                              if (ec) {
                                 // Mark connection as closing before notifying handlers to avoid re-entrant close
                                 // attempts
                                 self->is_closing_ = true;
                                 {
                                    std::lock_guard<std::mutex> lock(self->write_mutex_);
                                    self->write_in_progress_ = false;
                                    self->write_queue_.clear();
                                    self->close_after_write_ = false;
                                 }
                                 // Only call error handlers if not already closed (prevents calling handlers
                                 // after force_close() when user's captured references may be invalid)
                                 if (!self->closed_.load()) {
                                    self->dispatch_error(ec);
                                 }
                                 // Close the connection after a write error (connection is likely broken)
                                 self->do_close();
                                 return;
                              }

                              // Process next frame in queue
                              self->do_write();
                           });
      }

      // Close the socket after the close frame has been sent (responder side)
      // RFC 6455 Section 7.1.1: "the server MUST close the connection immediately"
      // after sending the Close frame response.
      // https://datatracker.ietf.org/doc/html/rfc6455#section-7.1.1
      inline void schedule_close()
      {
         // The close frame has been written to the kernel buffer via async_write.
         // Use shutdown to signal we're done sending while allowing pending data to drain.
         // This ensures the close frame reaches the peer before we close the socket.
         auto socket = socket_;
         if (socket) {
            asio::error_code ec;
            socket->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ec);
            // Ignore shutdown errors - socket may already be closed
         }
         do_close();
      }

      inline void send_close_frame(ws_close_code code, std::string_view reason, bool schedule_socket_close = false)
      {
         std::vector<uint8_t> payload(2 + reason.size());
         payload[0] = static_cast<uint8_t>(static_cast<uint16_t>(code) >> 8);
         payload[1] = static_cast<uint8_t>(static_cast<uint16_t>(code) & 0xFF);

         if (!reason.empty()) {
            std::copy(reason.begin(), reason.end(), payload.begin() + 2);
         }

         if (schedule_socket_close) {
            // Send close frame and schedule socket closure after write completes
            send_close_frame_with_callback(
               ws_opcode::close, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
         }
         else {
            // Just send the close frame without scheduling closure
            send_frame(ws_opcode::close,
                       std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
         }
      }

      inline void send_close_frame_with_callback(ws_opcode opcode, std::string_view payload)
      {
         // Queue the close frame and set close_after_write_ flag atomically
         send_frame(opcode, payload, true, true);
      }

      inline std::size_t get_frame_header_size(std::size_t payload_length, bool use_masking)
      {
         std::size_t base_size;
         if (payload_length < 126) {
            base_size = 2;
         }
         else if (payload_length <= 0xFFFF) {
            base_size = 4;
         }
         else {
            base_size = 10;
         }
         return base_size + (use_masking ? 4 : 0); // Add 4 bytes for masking key if needed
      }

      inline void write_frame_header(ws_opcode opcode, std::size_t payload_length, bool fin, uint8_t* header,
                                     bool use_masking)
      {
         ws_frame_header frame_header;
         frame_header.fin(fin);
         frame_header.opcode(opcode);
         frame_header.mask(use_masking);

         header[0] = frame_header.data[0];

         std::size_t header_offset = 2;

         if (payload_length < 126) {
            frame_header.payload_len(static_cast<uint8_t>(payload_length));
            header[1] = frame_header.data[1];
         }
         else if (payload_length <= 0xFFFF) {
            frame_header.payload_len(126);
            header[1] = frame_header.data[1];
            header[2] = static_cast<uint8_t>(payload_length >> 8);
            header[3] = static_cast<uint8_t>(payload_length & 0xFF);
            header_offset = 4;
         }
         else {
            frame_header.payload_len(127);
            header[1] = frame_header.data[1];
            for (int i = 0; i < 8; ++i) {
               header[2 + i] = static_cast<uint8_t>(static_cast<uint64_t>(payload_length) >> (8 * (7 - i)));
            }
            header_offset = 10;
         }

         // Write masking key if needed (client mode)
         if (use_masking) {
            // Generate random masking key
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<unsigned int> dist(0, 255);
            for (int i = 0; i < 4; ++i) {
               header[header_offset + i] = static_cast<uint8_t>(dist(gen));
            }
            // Mask the payload (done in-place in the frame buffer after this header)
            // Note: Actual masking of payload should be done by caller if needed
         }
      }

      // Helper to dispatch error notifications to server or client handler
      void dispatch_error(std::error_code ec)
      {
         if (auto srv = server_.lock()) {
            srv->notify_error(this->shared_from_this(), ec);
            return;
         }
         if (client_error_handler_) {
            client_error_handler_(ec);
         }
      }

      // Helper to dispatch message notifications to server or client handler
      void dispatch_message(std::string_view message, ws_opcode opcode)
      {
         if (auto srv = server_.lock()) {
            srv->notify_message(this->shared_from_this(), message, opcode);
            return;
         }
         if (client_message_handler_) {
            client_message_handler_(message, opcode);
         }
      }

      // Helper to dispatch close notifications to server or client handler
      void dispatch_close()
      {
         if (auto srv = server_.lock()) {
            srv->notify_close(this->shared_from_this(), close_code_, close_reason_);
            return;
         }
         if (client_close_handler_) {
            client_close_handler_(close_code_, close_reason_);
         }
      }

      inline void do_close()
      {
         // Ensure we only close once and call on_close once
         bool expected = false;
         if (!closed_.compare_exchange_strong(expected, true)) {
            return; // Already closed
         }

         // Synchronize with force_close() to prevent calling handlers with dangling references
         {
            std::lock_guard<std::mutex> lock(close_handler_mutex_);
            dispatch_close();
         }

         // Hold socket_op_mutex_ while closing to prevent race with async_read_some/async_write
         // setup in do_start_read/do_write. ASIO socket operations are NOT thread-safe.
         {
            std::lock_guard<std::mutex> lock(socket_op_mutex_);
            if (socket_) {
               asio::error_code ec;
               socket_->lowest_layer().cancel(ec);
               socket_->lowest_layer().close(ec);
               socket_.reset(); // Reset pointer so subsequent checks know socket is closed
            }
         }
      }

      std::shared_ptr<SocketType> socket_;
      std::weak_ptr<websocket_server> server_;
      std::array<uint8_t, 16384> read_buffer_;
      std::vector<uint8_t> frame_buffer_;
      std::vector<uint8_t> message_buffer_;
      ws_opcode current_opcode_{ws_opcode::continuation};
      bool is_reading_frame_{false};
      std::atomic<bool> is_closing_{false};
      bool handshake_complete_{false};
      std::atomic<bool> closed_{false};
      std::shared_ptr<void> user_data_;
      asio::ip::tcp::endpoint remote_endpoint_;
      bool client_mode_{false}; // For client-side connections (requires masking)

      // Write queue for serializing outgoing frames (prevents interleaved writes)
      std::mutex write_mutex_;
      std::deque<std::unique_ptr<std::vector<uint8_t>>> write_queue_;
      bool write_in_progress_{false};
      bool close_after_write_{false}; // Close socket after write queue drains

      // Client mode callbacks
      std::function<void(std::string_view, ws_opcode)> client_message_handler_;
      std::function<void(ws_close_code, std::string_view)> client_close_handler_;
      std::function<void(std::error_code)> client_error_handler_;

      // Mutex to synchronize close handler access between do_close() and force_close()
      std::mutex close_handler_mutex_;

      // Mutex to prevent race between starting async operations and closing the socket.
      // ASIO sockets are NOT thread-safe for concurrent operations (e.g., async_read_some
      // racing with close()). This mutex is held briefly while setting up async ops and
      // while closing the socket.
      std::mutex socket_op_mutex_;

      // Close code and reason for callback
      ws_close_code close_code_{ws_close_code::normal};
      std::string close_reason_;

     public:
      // Client mode support
      inline void set_client_mode(bool enabled)
      {
         client_mode_ = enabled;
         handshake_complete_ = true;
      }
      inline void start_read() { do_start_read(); }

      // Client mode callback setters
      inline void on_message(std::function<void(std::string_view, ws_opcode)> handler)
      {
         client_message_handler_ = std::move(handler);
      }
      inline void on_close(std::function<void(ws_close_code, std::string_view)> handler)
      {
         client_close_handler_ = std::move(handler);
      }
      inline void on_error(std::function<void(std::error_code)> handler) { client_error_handler_ = std::move(handler); }

     private:
      inline void do_start_read()
      {
         // Check if already fully closed to avoid operations on closed sockets
         // RFC 6455 Section 7.1.1: Use closed_ (not is_closing_) to allow receiving
         // close response during handshake. The initiator must continue reading.
         // https://datatracker.ietf.org/doc/html/rfc6455#section-7.1.1
         if (closed_.load()) {
            return;
         }

         // Hold socket_op_mutex_ while setting up the async read operation.
         // This prevents close() from being called while async_read_some() is setting up.
         // ASIO socket operations are NOT thread-safe for concurrent calls.
         std::lock_guard<std::mutex> lock(socket_op_mutex_);

         // Re-check under lock since socket may have been closed while waiting for mutex
         auto socket = socket_;
         if (!socket || !socket->lowest_layer().is_open()) {
            return;
         }

         auto self = this->shared_from_this();
         socket->async_read_some(asio::buffer(read_buffer_),
                                 [self, socket](std::error_code ec, std::size_t bytes_transferred) {
                                    self->on_read(ec, bytes_transferred);
                                 });
      }
   };
}