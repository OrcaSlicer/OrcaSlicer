#pragma once

#include <string>
#include <functional>
#include <nlohmann/json.hpp>

namespace Slic3r {

using json = nlohmann::json;

/**
 * Universal IPC Request
 */
struct IPCRequest {
    std::string id;
    std::string method;
    json params;
    
    IPCRequest() = default;
    IPCRequest(const std::string& id, const std::string& method, const json& params = json::object())
        : id(id), method(method), params(params) {}
};

/**
 * Universal IPC Result (for handler return values)
 */
struct IPCResult {
    int code = 0;
    std::string message;
    json data;

    IPCResult() = default;
    IPCResult(int code, const std::string& message = "", const json& data = json::object())
        : code(code), message(message), data(data) {}
    IPCResult(const json& data) : code(0), message("success"), data(data) {}
    
    static IPCResult success(const json& data = json::object());
    static IPCResult error(int code, const std::string& message);
    static IPCResult error(const std::string& message = "error");
    
    bool hasError() const { return code != 0; }
};

/**
 * Universal IPC Response (for sending responses)
 */
struct IPCResponse {
    std::string id;
    std::string method;
    int code = 0;
    std::string message;
    json data;
    
    IPCResponse() : code(0) {}
    IPCResponse(int code, const std::string& message = "") : code(code), message(message) {}
    IPCResponse(const json& data) : code(0), message("success"), data(data) {}
    IPCResponse(const std::string& id, const std::string& method, int code, 
                const std::string& message, const json& data = json::object())
        : id(id), method(method), code(code), message(message), data(data) {}
    
    static IPCResponse success(const std::string& id, const std::string& method, const json& data = json::object());
    static IPCResponse success(const json& data = json::object());
    static IPCResponse error(const std::string& id, const std::string& method, int code, const std::string& message);
    static IPCResponse error(const std::string& message = "");
};

/**
 * Universal IPC Event
 */
struct IPCEvent {
    std::string id;
    std::string method;
    json data;
    
    IPCEvent() = default;
    IPCEvent(const std::string& method, const json& data, const std::string& id = "")
        : id(id), method(method), data(data) {}
};

/**
 * Handler type definitions
 */
using IPCRequestHandler = std::function<IPCResult(const IPCRequest&)>;
using IPCAsyncRequestHandler = std::function<void(const IPCRequest&, std::function<void(const IPCResult&)>)>;
using IPCAsyncRequestHandlerWithEvents = std::function<void(const IPCRequest&, std::function<void(const IPCResult&)>, std::function<void(const std::string&, const json&)>)>;

using IPCResponseHandler = std::function<void(const IPCResult&)>;
using IPCEventHandler = std::function<void(const IPCEvent&)>;
using IPCRequestEventHandler = std::function<void(const IPCEvent&)>;

/**
 * Message serialization/deserialization utilities
 */
std::string serializeIPCMessage(const json& message);
json parseIPCMessage(const std::string& jsonStr);

} // namespace Slic3r

