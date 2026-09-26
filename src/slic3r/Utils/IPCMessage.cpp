#include "IPCMessage.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

// IPCResult static methods
IPCResult IPCResult::success(const json& data) {
    return IPCResult(0, "success", data);
}

IPCResult IPCResult::error(int code, const std::string& message) {
    return IPCResult(code, message, json::object());
}

IPCResult IPCResult::error(const std::string& message) {
    return IPCResult(1, message, json::object());
}

// IPCResponse static methods
IPCResponse IPCResponse::success(const std::string& id, const std::string& method, const json& data) {
    return IPCResponse(id, method, 0, "success", data);
}

IPCResponse IPCResponse::success(const json& data) {
    return IPCResponse(data);
}

IPCResponse IPCResponse::error(const std::string& id, const std::string& method, int code, const std::string& message) {
    return IPCResponse(id, method, code, message, json::object());
}

IPCResponse IPCResponse::error(const std::string& message) {
    return IPCResponse(1, message);
}

// Message serialization/deserialization
std::string serializeIPCMessage(const json& message) {
    if (message.is_null()) {
        BOOST_LOG_TRIVIAL(warning) << "IPC: attempting to serialize null JSON message";
        return "{}";
    }
    
    try {
        return message.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (const json::type_error& e) {
        BOOST_LOG_TRIVIAL(error) << "IPC: JSON type error during serialization: " << e.what();
        return "{}";
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "IPC: unexpected error during message serialization: " << e.what();
        return "{}";
    }
}

json parseIPCMessage(const std::string& jsonStr) {
    if (jsonStr.empty()) {
        BOOST_LOG_TRIVIAL(error) << "IPC: received empty message";
        return json();
    }
    
    try {
        json parsed = json::parse(jsonStr);
        if (parsed.is_null()) {
            BOOST_LOG_TRIVIAL(error) << "IPC: parsed JSON is null";
            return json();
        }
        return parsed;
    } catch (const json::parse_error& e) {
        BOOST_LOG_TRIVIAL(error) << "IPC: JSON parse error at byte " << e.byte << ": " << e.what();
        return json();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "IPC: unexpected error during JSON parsing: " << e.what();
        return json();
    }
}

} // namespace Slic3r

