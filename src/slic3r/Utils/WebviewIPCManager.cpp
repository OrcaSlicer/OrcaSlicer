#include "WebviewIPCManager.h"
#include "JsonUtils.hpp"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <wx/log.h>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r {
namespace webviewIpc {

// ThreadPool implementation
ThreadPool::ThreadPool(size_t numThreads) : m_stop(false) {
    
    if( numThreads == 0 )
    {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 4;
    }

    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool() {
    m_stop = true;
    m_condition.notify_all();
    
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_tasks.push(std::move(task));
    }
    m_condition.notify_one();
}

size_t ThreadPool::pendingTasks() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_queueMutex));
    return m_tasks.size();
}

void ThreadPool::workerThread() {
    while (!m_stop) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_condition.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
            
            if (m_stop && m_tasks.empty()) {
                return;
            }
            
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
        }
        
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                wxLogError("ThreadPool: Task execution failed: %s", e.what());
            } catch (...) {
                wxLogError("ThreadPool: Task execution failed with unknown exception");
            }
        }
    }
}

// WebviewIPCManager implementation
std::vector<WebviewIPCManager*>WebviewIPCManager::s_instances;
std::mutex WebviewIPCManager::s_instancesMutex;

WebviewIPCManager::WebviewIPCManager(wxWebView* webView, size_t threadPoolSize)
    : m_webView(webView), m_requestIdCounter(1000), m_running(false) {
    initialize(threadPoolSize);
    std::lock_guard<std::mutex> lock(s_instancesMutex);
    s_instances.push_back(this);
}

WebviewIPCManager::~WebviewIPCManager() {
    {
        std::lock_guard<std::mutex> lock(s_instancesMutex);
        s_instances.erase(std::remove(s_instances.begin(), s_instances.end(), this), s_instances.end());
    }
    cleanup();
}

void WebviewIPCManager::initialize(size_t threadPoolSize) {
    if (m_running) {
        return;
    }
    
    // Initialize thread pool
    m_threadPool = std::make_unique<ThreadPool>(threadPoolSize);
    
    if(m_webView){
        m_webView->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebviewIPCManager::onScriptMessage, this);
        m_webView->Bind(wxEVT_DESTROY, &WebviewIPCManager::onWebviewDestroy, this);
    }
    m_running = true;
    startTimeoutChecker();
    
    wxLogMessage("IPC: C++ side initialization complete with %zu worker threads", threadPoolSize);
}

void WebviewIPCManager::cleanup() {
    if (!m_running) {
        return;
    }
    m_running = false;
    stopTimeoutChecker();
    if(m_webView){
        m_webView->Unbind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebviewIPCManager::onScriptMessage, this);
        m_webView->Unbind(wxEVT_DESTROY, &WebviewIPCManager::onWebviewDestroy, this);
    }
    m_webView = nullptr;
    
    // Destroy thread pool (will wait for all tasks to complete)
    m_threadPool.reset();
    
    // Clear pending requests
    std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
    m_pendingRequests.clear();
    
    wxLogMessage("IPC: C++ side cleanup complete");
}
void WebviewIPCManager::onWebviewDestroy(wxEvent& event){
    cleanup();
}
void WebviewIPCManager::onScriptMessage(wxWebViewEvent& event){
    wxString message = event.GetString();
    
    // Throttle frequent request_printer_list messages: log every 100th request
    if (message.Find("\"method\":\"request_printer_list\"") != wxNOT_FOUND) {
        if (m_printerListRequestCount % 100 == 0) {
            wxLogMessage("Received message: %s", message);
        }
        m_printerListRequestCount++;
    } else {
        wxLogMessage("Received message: %s", message);
    }
    
    onMessageReceived(message.ToUTF8().data());
}
void WebviewIPCManager::onMessageReceived(const std::string& message) {
    json jsonMessage = parseIPCMessage(message);
    if (jsonMessage.is_null()) {
        return;
    }
    
    try {
        handleMessage(jsonMessage);
    } catch (const std::exception& e) {
        wxLogError("IPC: Failed to handle message: %s", e.what());
    }
}

void WebviewIPCManager::handleMessage(const json& message) {
    if (!message.is_object()) {
        wxLogError("IPC: Message is not a valid JSON object");
        return;
    }
    
    std::string type = JsonUtils::safeGetString(message, "type", "request");
    
    if (type == "request") {
        handleRequest(message);
    } else if (type == "response") {
        handleResponse(message);
    } else if (type == "event") {
        handleEvent(message);
    } else {
        wxLogWarning("IPC: Unknown message type: %s", type.c_str());
    }
}

void WebviewIPCManager::handleRequest(const json& message) {
    if (!message.is_object()) {
        wxLogError("IPC: Request message is not a valid JSON object");
        return;
    }

    std::string id = JsonUtils::safeGetString(message, "id", "");
    std::string method = JsonUtils::safeGetString(message, "method", "");
    
    if (id.empty()) {
        wxLogError("IPC: Request message missing or invalid id field");
        return;
    }
    
    if (method.empty()) {
        wxLogError("IPC: Request message missing or invalid method field");
        IPCResponse errorResponse = IPCResponse::error(id, "", 400, "Missing method field");
        sendResponse(errorResponse);
        return;
    }
    
    json params = JsonUtils::safeGetJson(message, "params", json::object());
    
    IPCRequest request(id, method, params);
    
    std::lock_guard<std::mutex> lock(m_requestHandlersMutex);
    
    // Check asynchronous handlers with event sending - execute in thread pool
    auto asyncWithEventsIt = m_asyncRequestHandlersWithEvents.find(method);
    if (asyncWithEventsIt != m_asyncRequestHandlersWithEvents.end()) {
        // Capture handler and request data
        IPCAsyncRequestHandlerWithEvents handler = asyncWithEventsIt->second;
        
        // Submit to thread pool for async execution
        m_threadPool->enqueue([this, handler, request, id, method]() {
            try {
                handler(request, 
                    [this, id, method](const IPCResult& result) {
                        // Convert IPCResult to IPCResponse for sending
                        IPCResponse response(id, method, result.code, result.message, result.data);
                        sendResponse(response);
                    },
                    [this, id](const std::string& eventMethod, const json& eventData) {
                        sendEvent(eventMethod, eventData, id);
                    });
            } catch (const std::exception& e) {
                wxLogError("IPC: Async handler with events execution failed for method '%s': %s", method.c_str(), e.what());
                IPCResponse errorResponse = IPCResponse::error(id, method, 500, "Handler execution failed");
                sendResponse(errorResponse);
            }
        });
        return;
    }
    
    // Check regular asynchronous handlers - execute in thread pool
    auto asyncIt = m_asyncRequestHandlers.find(method);
    if (asyncIt != m_asyncRequestHandlers.end()) {
        // Capture handler and request data
        IPCAsyncRequestHandler handler = asyncIt->second;
        
        // Submit to thread pool for async execution
        m_threadPool->enqueue([this, handler, request, id, method]() {
            try {
                handler(request, [this, id, method](const IPCResult& result) {
                    // Convert IPCResult to IPCResponse for sending
                    IPCResponse response(id, method, result.code, result.message, result.data);
                    sendResponse(response);
                });
            } catch (const std::exception& e) {
                wxLogError("IPC: Async handler execution failed for method '%s': %s", method.c_str(), e.what());
                IPCResponse errorResponse = IPCResponse::error(id, method, 500, "Handler execution failed");
                sendResponse(errorResponse);
            }
        });
        return;
    }
    
    // Check synchronous handlers - execute in thread pool
    auto syncIt = m_requestHandlers.find(method);
    if (syncIt != m_requestHandlers.end()) {
        // Capture handler and request data
        IPCRequestHandler handler = syncIt->second;
        
        // Submit to thread pool for async execution
        m_threadPool->enqueue([this, handler, request, id, method]() {
            try {
                IPCResult result = handler(request);
                // Convert IPCResult to IPCResponse for sending
                IPCResponse response(request.id, request.method, result.code, result.message, result.data);
                sendResponse(response);
            } catch (const std::exception& e) {
                wxLogError("IPC: Sync handler execution failed for method '%s': %s", method.c_str(), e.what());
                IPCResponse errorResponse = IPCResponse::error(id, method, 500, "Handler execution failed");
                sendResponse(errorResponse);
            }
        });
        return;
    }
    
    // Handler not found
    IPCResponse errorResponse = IPCResponse::error(id, method, 404, "Method not found");
    sendResponse(errorResponse);
}

void WebviewIPCManager::handleResponse(const json& message) {
    if (!message.is_object()) {
        wxLogError("IPC: Response message is not a valid JSON object");
        return;
    }
    
    std::string id = JsonUtils::safeGetString(message, "id", "");
    if (id.empty()) {
        wxLogError("IPC: Response message missing or invalid id field");
        return;
    }

    std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
    auto it = m_pendingRequests.find(id);
    if (it != m_pendingRequests.end()) {
        auto& pendingRequest = it->second;

        // Create IPCResult from response message with safe extraction
        int code = JsonUtils::safeGetInt(message, "code", 0);
        std::string msg = JsonUtils::safeGetString(message, "message", "");
        json data = JsonUtils::safeGetJson(message, "data", json::object());

        IPCResult result(code, msg, data);
        
        // Execute callback
        if (pendingRequest->callback) {
            try {
                pendingRequest->callback(result);
            } catch (const std::exception& e) {
                wxLogError("IPC: Response callback execution failed for request '%s': %s", id.c_str(), e.what());
            }
        }
        
        // Remove request
        m_pendingRequests.erase(it);
    } else {
        wxLogWarning("IPC: Received response for unknown request id: %s", id.c_str());
    }
}

void WebviewIPCManager::handleEvent(const json& message) {
    if (!message.is_object()) {
        wxLogError("IPC: Event message is not a valid JSON object");
        return;
    }
    
    std::string method = JsonUtils::safeGetString(message, "method", "");
    if (method.empty()) {
        wxLogError("IPC: Event message missing or invalid method field");
        return;
    }
    
    std::string id = JsonUtils::safeGetString(message, "id", "");
    json data = JsonUtils::safeGetJson(message, "data", json::object());

    IPCEvent event(method, data, id);
    
    // First check if there's an associated request event callback
    if (!id.empty()) {
        std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
        auto it = m_pendingRequests.find(id);
        if (it != m_pendingRequests.end() && it->second->hasEventCallback) {
            IPCRequestEventHandler eventCallback = it->second->eventCallback;
            
            // Execute in thread pool
            m_threadPool->enqueue([eventCallback, event, method, id]() {
                try {
                    eventCallback(event);
                } catch (const std::exception& e) {
                    wxLogError("IPC: Request event callback execution failed for method '%s', id '%s': %s", 
                              method.c_str(), id.c_str(), e.what());
                }
            });
        }
    }
    
    // Then handle global event handlers - execute in thread pool
    std::vector<IPCEventHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(m_eventHandlersMutex);
        auto it = m_eventHandlers.find(method);
        if (it != m_eventHandlers.end()) {
            handlers = it->second;  // Copy handlers
        }
    }
    
    // Execute each handler in thread pool
    for (const auto& handler : handlers) {
        m_threadPool->enqueue([handler, event, method]() {
            try {
                handler(event);
            } catch (const std::exception& e) {
                wxLogError("IPC: Global event handler execution failed for method '%s': %s", 
                          method.c_str(), e.what());
            }
        });
    }
}

void WebviewIPCManager::request(const std::string& method, const json& params, 
                        IPCResponseHandler callback, int timeout) {
    std::string id = generateRequestId();
    
    if(callback)
    // Store pending request
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
        m_pendingRequests[id] = std::make_unique<PendingRequest>(method, std::move(callback), timeout);
    }
    
    // Build request message
    json message = {
        {"id", id},
        {"method", method},
        {"type", "request"},
        {"params", params}
    };
    
    sendMessage(message);
}

void WebviewIPCManager::requestWithEvents(const std::string& method, const json& params,
                                  IPCResponseHandler responseCallback, IPCRequestEventHandler eventCallback,
                                  int timeout) {
    std::string id = generateRequestId();

    if(eventCallback!=nullptr||responseCallback!=nullptr)
    // Store pending request with event callback
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
        m_pendingRequests[id] = std::make_unique<PendingRequest>(method, std::move(responseCallback), 
                                                                std::move(eventCallback), timeout);
    }
    
    // Build request message
    json message = {
        {"id", id},
        {"method", method},
        {"type", "request"},
        {"params", params}
    };
    
    sendMessage(message);
}

IPCResult WebviewIPCManager::requestSync(const std::string& method, const json& params, int timeout) {
    std::mutex responseMutex;
    std::condition_variable responseCondition;
    IPCResponse response;
    bool responseReceived = false;
    
    request(method, params, [&](const IPCResult& resp) {
        std::lock_guard<std::mutex> lock(responseMutex);
        // Convert IPCResult to IPCResponse for internal processing
        response = IPCResponse("", method, resp.code, resp.message, resp.data);
        responseReceived = true;
        responseCondition.notify_one();
    }, timeout);
    
    // Wait for response
    std::unique_lock<std::mutex> lock(responseMutex);
    if (!responseCondition.wait_for(lock, std::chrono::milliseconds(timeout), 
                                   [&]() { return responseReceived; })) {
        // Timeout
        return IPCResult::error(-1, "Request timeout");
    }
    
    // Convert back to IPCResult for return
    return IPCResult(response.code, response.message, response.data);
}

void WebviewIPCManager::sendResponse(const IPCResponse& response) {
    json message = {
        {"id", response.id},
        {"method", response.method},
        {"type", "response"},
        {"code", response.code},
        {"message", response.message},
        {"data", response.data}
    };
    
    sendMessage(message);
}

void WebviewIPCManager::sendEvent(const IPCEvent& event) {
    json message = {
        {"method", event.method},
        {"type", "event"},
        {"data", event.data}
    };
    
    if (!event.id.empty()) {
        message["id"] = event.id;
    }
    
    sendMessage(message);
}

void WebviewIPCManager::sendEvent(const std::string& method, const json& data, const std::string& requestId) {
    IPCEvent event(method, data, requestId);
    sendEvent(event);
}

void WebviewIPCManager::onRequest(const std::string& method, IPCRequestHandler handler) {
    std::lock_guard<std::mutex> lock(m_requestHandlersMutex);
    m_requestHandlers[method] = std::move(handler);
}

void WebviewIPCManager::onRequestAsync(const std::string& method, IPCAsyncRequestHandler handler) {
    std::lock_guard<std::mutex> lock(m_requestHandlersMutex);
    m_asyncRequestHandlers[method] = std::move(handler);
}

void WebviewIPCManager::onRequestAsyncWithEvents(const std::string& method, IPCAsyncRequestHandlerWithEvents handler) {
    std::lock_guard<std::mutex> lock(m_requestHandlersMutex);
    m_asyncRequestHandlersWithEvents[method] = std::move(handler);
}

void WebviewIPCManager::offRequest(const std::string& method) {
    std::lock_guard<std::mutex> lock(m_requestHandlersMutex);
    m_requestHandlers.erase(method);
    m_asyncRequestHandlers.erase(method);
    m_asyncRequestHandlersWithEvents.erase(method);
}

void WebviewIPCManager::onEvent(const std::string& method, IPCEventHandler handler) {
    std::lock_guard<std::mutex> lock(m_eventHandlersMutex);
    m_eventHandlers[method].push_back(std::move(handler));
}

void WebviewIPCManager::offEvent(const std::string& method, const IPCEventHandler& handler) {
    std::lock_guard<std::mutex> lock(m_eventHandlersMutex);
    auto it = m_eventHandlers.find(method);
    if (it != m_eventHandlers.end()) {
        auto& handlers = it->second;
        // Note: This requires function object comparison, may need other identification methods in actual use
        handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
            [&handler](const IPCEventHandler& h) {
                // Comparison implementation needed based on actual situation
                return false; // Simplified implementation
            }), handlers.end());
    }
}

std::string WebviewIPCManager::generateRequestId() {
    return "req-" + std::to_string(++m_requestIdCounter);
}

size_t WebviewIPCManager::getThreadPoolSize() const {
    return m_threadPool ? m_threadPool->size() : 0;
}

size_t WebviewIPCManager::getPendingTaskCount() const {
    return m_threadPool ? m_threadPool->pendingTasks() : 0;
}

void WebviewIPCManager::sendMessage(const json& message) {
    if (!m_webView) {
        wxLogError("IPC: WebView not initialized");
        return;
    }
    
    if (!m_running) {
        wxLogWarning("IPC: Manager is not running, message not sent");
        return;
    }
    
    try {
        // Validate message before sending
        if (message.is_null() || !message.is_object()) {
            wxLogError("IPC: Invalid message format - not a JSON object");
            return;
        }
        
        // Use compact dump for transmission efficiency, with error handling
        std::string jsonStr = message.dump(-1, ' ', true);
        if (jsonStr.empty()) {
            wxLogError("IPC: Failed to serialize message to JSON");
            return;
        }
        
        wxString strJS = wxString::Format("HandleStudio(%s)", jsonStr);
        WebviewIPCManager *_this = this;
        Slic3r::GUI::wxGetApp().CallAfter([_this, strJS] { 
            try {
                {
                    std::lock_guard<std::mutex> lock(s_instancesMutex);
                    if(std::find(s_instances.begin(), s_instances.end(), _this) == s_instances.end()){
                        // Instance has been destroyed
                        return;
                    }
                }
                if(!_this->m_webView) return;
                WebView::RunScript(_this->m_webView, strJS); 
            } catch (const std::exception& e) {
                wxLogError("IPC: Failed to execute script: %s", e.what());
            }
        });
    } catch (const json::type_error& e) {
        wxLogError("IPC: JSON type error when sending message: %s", e.what());
    } catch (const std::exception& e) {
        wxLogError("IPC: Unexpected error when sending message: %s", e.what());
    }
}

void WebviewIPCManager::checkTimeouts() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> expiredIds;
        
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
            for (const auto& pair : m_pendingRequests) {
                const auto& request = pair.second;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - request->timestamp).count();
                
                if (elapsed >= request->timeout) {
                    expiredIds.push_back(pair.first);
                }
            }
        }
        
        // Handle timeout requests
        for (const std::string& id : expiredIds) {
            std::lock_guard<std::mutex> lock(m_pendingRequestsMutex);
            auto it = m_pendingRequests.find(id);
            if (it != m_pendingRequests.end()) {
                IPCResult timeoutResult = IPCResult::error(-1, "Request timeout");
                if (it->second->callback) {
                    it->second->callback(timeoutResult);
                }
                m_pendingRequests.erase(it);
            }
        }
    }
}

void WebviewIPCManager::startTimeoutChecker() {
    m_timeoutThread = std::thread(&WebviewIPCManager::checkTimeouts, this);
}

void WebviewIPCManager::stopTimeoutChecker() {
    if (m_timeoutThread.joinable()) {
        m_timeoutThread.join();
    }
}

} // namespace webviewIpc
} // namespace Slic3r