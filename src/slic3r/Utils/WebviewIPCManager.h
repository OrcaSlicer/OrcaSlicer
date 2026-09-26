#pragma once

#include "IPCMessage.hpp"
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>
#include <condition_variable>
#include <wx/webview.h>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace webviewIpc {
using json = nlohmann::json;

/**
 * Simple thread pool for handling IPC callbacks asynchronously
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = 4);
    ~ThreadPool();
    
    // Disable copy and move
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    
    // Submit a task to the thread pool
    void enqueue(std::function<void()> task);
    
    // Get the number of threads
    size_t size() const { return m_workers.size(); }
    
    // Get the number of pending tasks
    size_t pendingTasks() const;
    
private:
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_queueMutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop;
    
    void workerThread();
};

/**
 * Pending request information
 */
struct PendingRequest {
    std::string method;
    IPCResponseHandler callback;
    IPCRequestEventHandler eventCallback;
    std::chrono::steady_clock::time_point timestamp;
    int timeout;
    bool hasEventCallback;
    
    PendingRequest(const std::string& method, IPCResponseHandler callback, int timeout)
        : method(method), callback(std::move(callback)), 
          timestamp(std::chrono::steady_clock::now()), timeout(timeout), hasEventCallback(false) {}
    
    PendingRequest(const std::string& method, IPCResponseHandler callback, IPCRequestEventHandler eventCallback, int timeout)
        : method(method), callback(std::move(callback)), eventCallback(std::move(eventCallback)),
          timestamp(std::chrono::steady_clock::now()), timeout(timeout), hasEventCallback(true) {}
};

/**
 * wxWebView IPC Manager
 */
class WebviewIPCManager {
private:
    wxWebView* m_webView;
    std::atomic<int> m_requestIdCounter;
    
    // Store pending requests
    std::map<std::string, std::unique_ptr<PendingRequest>> m_pendingRequests;
    std::mutex m_pendingRequestsMutex;
    
    // Request handlers
    std::map<std::string, IPCRequestHandler> m_requestHandlers;
    std::map<std::string, IPCAsyncRequestHandler> m_asyncRequestHandlers;
    std::map<std::string, IPCAsyncRequestHandlerWithEvents> m_asyncRequestHandlersWithEvents;
    std::mutex m_requestHandlersMutex;
    
    // Event handlers
    std::map<std::string, std::vector<IPCEventHandler>> m_eventHandlers;
    std::mutex m_eventHandlersMutex;
    
    // Thread pool for async execution
    std::unique_ptr<ThreadPool> m_threadPool;
    
    // Timeout check thread
    std::thread m_timeoutThread;
    std::atomic<bool> m_running;
    
    // Log throttling for frequent messages
    std::atomic<uint64_t> m_printerListRequestCount{0};
    
    // Internal processing methods
    void handleMessage(const json& message);
    void handleRequest(const json& message);
    void handleResponse(const json& message);
    void handleEvent(const json& message);
    
    // Timeout checking
    void checkTimeouts();
    void startTimeoutChecker();
    void stopTimeoutChecker();

    void onScriptMessage(wxWebViewEvent& event);
    void onWebviewDestroy(wxEvent& event);
public:
    explicit WebviewIPCManager(wxWebView* webView, size_t threadPoolSize = 4);
    ~WebviewIPCManager();
    
    // Disable copy constructor and assignment
    WebviewIPCManager(const WebviewIPCManager&) = delete;
    WebviewIPCManager& operator=(const WebviewIPCManager&) = delete;
    
    /**
     * Initialize IPC manager
     * @param threadPoolSize Number of threads in the thread pool (default: 4)
     */
    void initialize(size_t threadPoolSize = 4);
    
    /**
     * Clean up resources
     */
    void cleanup();
    
    /**
     * Handle messages from JavaScript
     * @param message JSON format message string
     */
    void onMessageReceived(const std::string& message);
    
    /**
     * Send asynchronous request
     * @param method Method name
     * @param params Parameters
     * @param callback Response callback function
     * @param timeout Timeout in milliseconds, default 10 seconds
     */
    void request(const std::string& method, const json& params, 
                IPCResponseHandler callback, int timeout = 10000);
    
    /**
     * Send asynchronous request with event callback
     * @param method Method name
     * @param params Parameters
     * @param responseCallback Response callback function
     * @param eventCallback Event callback function, receives events related to this request
     * @param timeout Timeout in milliseconds, default 10 seconds
     */
    void requestWithEvents(const std::string& method, const json& params,
                          IPCResponseHandler responseCallback, IPCRequestEventHandler eventCallback,
                          int timeout = 10000);
    
    /**
     * Send synchronous request (blocking version, not recommended for main thread)
     * @param method Method name
     * @param params Parameters
     * @param timeout Timeout in milliseconds, default 10 seconds
     * @return Response result
     */
    IPCResult requestSync(const std::string& method, const json& params, int timeout = 10000);
    
    /**
     * Send response
     * @param response Response object
     */
    void sendResponse(const IPCResponse& response);
    
    /**
     * Send event
     * @param event Event object
     */
    void sendEvent(const IPCEvent& event);
    
    /**
     * Send event (convenience method)
     * @param method Event name
     * @param data Event data
     * @param requestId Associated request ID (optional)
     */
    void sendEvent(const std::string& method, const json& data, const std::string& requestId = "");
    
    /**
     * Register request handler (synchronous)
     * @param method Method name
     * @param handler Handler function
     */
    void onRequest(const std::string& method, IPCRequestHandler handler);
    
    /**
     * Register request handler (asynchronous)
     * @param method Method name
     * @param handler Asynchronous handler function
     */
    void onRequestAsync(const std::string& method, IPCAsyncRequestHandler handler);
    
    /**
     * Register request handler (asynchronous, with event sending callback)
     * @param method Method name
     * @param handler Asynchronous handler function with event sending callback
     */
    void onRequestAsyncWithEvents(const std::string& method, IPCAsyncRequestHandlerWithEvents handler);
    
    /**
     * Remove request handler
     * @param method Method name
     */
    void offRequest(const std::string& method);
    
    /**
     * Register event handler
     * @param method Event name
     * @param handler Handler function
     */
    void onEvent(const std::string& method, IPCEventHandler handler);
    
    /**
     * Remove event handler
     * @param method Event name
     * @param handler Handler function (must be the same function object)
     */
    void offEvent(const std::string& method, const IPCEventHandler& handler);
    
    /**
     * Generate unique request ID
     */
    std::string generateRequestId();
    
    /**
     * Get the number of worker threads in the thread pool
     */
    size_t getThreadPoolSize() const;
    
    /**
     * Get the number of pending tasks in the thread pool
     */
    size_t getPendingTaskCount() const;
    
private:
    /**
     * Send message to JavaScript side
     * @param message JSON message object
     */
    void sendMessage(const json& message);

    static std::vector<WebviewIPCManager*> s_instances;
    static std::mutex s_instancesMutex;
};

} // namespace webviewIpc
} // namespace Slic3r