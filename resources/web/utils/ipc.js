/**
 * wxWebView IPC Communication Library - JavaScript Side
 * Provides asynchronous request/response and event handling functionality
 * 
 * Dependencies:
 * - This file requires locales/index.js to be loaded before it
 * - Ensure <script src="locales/index.js"></script> is placed before this script in HTML
 */

// Helper function to get translated text
function getI18nText(key, fallback) {
    try {
        if (typeof i18n !== 'undefined' && i18n && i18n.global) {
            return i18n.global.t(key);
        }
    } catch (error) {
        console.warn('IPC: Failed to get translation for key:', key, error);
    }
    return fallback;
}

class IPCManager {
    constructor() {
        this.requestId = 1000;
        this.pendingRequests = new Map(); // Store pending requests
        this.eventHandlers = new Map();   // Store event handlers
        this.requestHandlers = new Map(); // Store request handlers (sync)
        this.asyncRequestHandlers = new Map(); // Store async request handlers
        this.asyncRequestHandlersWithEvents = new Map(); // Store async request handlers with events

        // Initialize message listening
        this.initMessageListener();
    }

    /**
     * Initialize message listener
     */
    initMessageListener() {
        window.HandleStudio = (data) => {
            try {
                console.log('IPC: Received message', data);
                // const message = JSON.parse(data);
                this.handleMessage(data);
            } catch (error) {
                console.error('IPC: Failed to parse message', error);
            }
        };
    }

    /**
     * Handle received messages
     */
    handleMessage(message) {
        const { id, method, type, code, data, params } = message;

        switch (type) {
            case 'response':
                this.handleResponse(id, code, message.message, data);
                break;
            case 'event':
                this.handleEvent(method, data, id); // Pass id parameter
                break;
            case 'request':
                this.handleRequest(id, method, params);
                break;
            default:
                console.warn('IPC: Unknown message type', type);
        }
    }

    /**
     * Handle response messages
     */
    handleResponse(id, code, message, data) {
        const pendingRequest = this.pendingRequests.get(id);
        if (pendingRequest) {
            this.pendingRequests.delete(id);

            if (code === 0) {
                pendingRequest.resolve(data);
            } else {
                const error = new Error(message || 'Request failed');
                error.code = code;
                error.data = data;
                pendingRequest.reject(error);
            }
        }
    }

    /**
     * Handle event messages
     */
    handleEvent(method, data, eventId = null) {
        // First check if there's an associated request event callback
        if (eventId) {
            const pendingRequest = this.pendingRequests.get(eventId);
            if (pendingRequest && pendingRequest.eventCallback) {
                try {
                    pendingRequest.eventCallback(method, data);
                } catch (error) {
                    console.error(`IPC: Request event callback execution failed ${method}`, error);
                }
            }
        }

        // Then handle global event listeners
        const handlers = this.eventHandlers.get(method);
        if (handlers) {
            handlers.forEach(handler => {
                try {
                    handler(data);
                } catch (error) {
                    console.error(`IPC: Event handler execution failed ${method}`, error);
                }
            });
        }
    }

    /**
     * Handle request messages
     */
    async handleRequest(id, method, params) {
        const handler = this.requestHandlers.get(method);
        const asyncHandler = this.asyncRequestHandlers.get(method);
        const asyncHandlerWithEvents = this.asyncRequestHandlersWithEvents.get(method);

        if (asyncHandlerWithEvents) {
            try {
                // Create enhanced response helper
                const respond = this.createResponseHelper(id, method);

                // Create event callback
                const sendEvent = (eventMethod, eventData) => {
                    this.sendEvent(eventMethod, eventData, id);
                };

                // Call async handler with events
                await asyncHandlerWithEvents(params, respond, sendEvent);
            } catch (error) {
                this.sendResponse(id, method, 1, error.message || 'Processing failed', null);
            }
        } else if (asyncHandler) {
            try {
                // Create enhanced response helper
                const respond = this.createResponseHelper(id, method);

                // Call async handler
                await asyncHandler(params, respond);
            } catch (error) {
                this.sendResponse(id, method, 1, error.message || 'Processing failed', null);
            }
        } else if (handler) {
            try {
                const result = await handler(params);
                this.sendResponse(id, method, 0, 'success', result);
            } catch (error) {
                this.sendResponse(id, method, 1, error.message || 'Processing failed', null);
            }
        } else {
            this.sendResponse(id, method, 404, 'Method not found', null);
        }
    }

    /**
     * Create response helper with convenient methods
     * @param {string} id Request ID
     * @param {string} method Method name
     * @returns {object} Response helper object with convenient methods
     */
    createResponseHelper(id, method) {
        const self = this;

        // Create response helper object with convenient methods
        const respond = {
            // Success response with data
            success: (data = null, message = 'success') => {
                self.sendResponse(id, method, 0, message, data);
            },

            // Error response with message
            error: (message = "error", code = 1, data = null) => {
                self.sendResponse(id, method, code, message, data);
            },

            // Not found error
            notFound: (message = 'Resource not found') => {
                self.sendResponse(id, method, 404, message, null);
            },

            // Bad request error
            badRequest: (message = 'Bad request', validationErrors = null) => {
                const data = validationErrors ? { validationErrors } : null;
                self.sendResponse(id, method, 400, message, data);
            },

            // Unauthorized error
            unauthorized: (message = 'Unauthorized') => {
                self.sendResponse(id, method, 401, message, null);
            },

            // Forbidden error
            forbidden: (message = 'Forbidden') => {
                self.sendResponse(id, method, 403, message, null);
            },

            // Internal server error
            serverError: (message = 'Internal server error', error = null) => {
                const data = error ? { error: error.toString() } : null;
                self.sendResponse(id, method, 500, message, data);
            },

            // Custom response (for backward compatibility)
            custom: (code, message, data = null) => {
                self.sendResponse(id, method, code, message, data);
            },

            // Raw response method (original behavior)
            raw: (code, message, data) => {
                self.sendResponse(id, method, code, message, data);
            }
        };

        // Make it callable for backward compatibility
        // respond(code, message, data) === respond.custom(code, message, data)
        const callableRespond = function (code, message, data) {
            return respond.custom(code, message, data);
        };

        // Copy all methods to the callable function
        Object.assign(callableRespond, respond);

        return callableRespond;
    }
    /**
     * Generate unique request ID
     * @returns {string} Unique request ID
     * 
     * @example
     * // Generated IDs follow the pattern: "req-{incrementing_number}"
     * const id1 = ipcManager.generateRequestId(); // "req-1001"
     * const id2 = ipcManager.generateRequestId(); // "req-1002" 
     * const id3 = ipcManager.generateRequestId(); // "req-1003"
     * 
     * @example
     * // Used internally but can be called manually if needed
     * const customId = ipcManager.generateRequestId();
     * console.log('Generated request ID:', customId);
     */
    generateRequestId() {
        return `req-${++this.requestId}`;
    }

    /**
     * Send message to C++ side
     * @param {object} message JSON message object to send
     * 
     * @example
     * // Send a custom message (typically used internally)
     * ipcManager.sendMessage({
     *     id: 'req-1001',
     *     method: 'customMethod',
     *     type: 'request',
     *     params: { key: 'value' }
     * });
     * 
     * @example
     * // Send response message (used in request handlers)
     * ipcManager.sendMessage({
     *     id: 'req-1001',
     *     method: 'getData',
     *     type: 'response',
     *     code: 0,
     *     message: 'success',
     *     data: { result: 'some data' }
     * });
     * 
     * @example
     * // Send event message
     * ipcManager.sendMessage({
     *     id: 'req-1001', // optional, for request-specific events
     *     method: 'onStatusUpdate',
     *     type: 'event',
     *     data: { status: 'processing', progress: 50 }
     * });
     */
    sendMessage(message) {
        console.log('IPC: Sending message', message);
        
        const trySend = (attempt = 0) => {
            if (window.wx && window.wx.postMessage) {
                window.wx.postMessage(JSON.stringify(message));
                return;
            }
            
            if (attempt < 5) {
                setTimeout(() => trySend(attempt + 1), 1000);
            } else {
                console.error('IPC: Unable to send message, no valid communication interface found');
                throw new Error('IPC: No valid communication interface found');
            }
        };
        
        trySend();
    }

    /**
     * Send asynchronous request
     * @param {string} method Method name
     * @param {object} params Parameters
     * @param {number} timeout Timeout in milliseconds, default 10 seconds
     * @returns {Promise} Returns Promise object
     * 
     * @example
     * // Basic request
     * try {
     *     const result = await ipcManager.request('getSystemInfo', {});
     *     console.log('System info:', result);
     * } catch (error) {
     *     console.error('Request failed:', error.message);
     * }
     * 
     * @example
     * // Request with parameters and custom timeout
     * const result = await ipcManager.request('calculateSum', {
     *     numbers: [1, 2, 3, 4, 5]
     * }, 5000);
     * console.log('Sum result:', result.sum);
     */
    request(method, params = {}, timeout = 10000) {
        return new Promise((resolve, reject) => {
            const id = this.generateRequestId();

            // Set timeout
            const timeoutId = setTimeout(() => {
                this.pendingRequests.delete(id);
                console.error('IPC: Request timeout:', method);
                reject(new Error(getI18nText('requestTimeout', 'Request timeout, please check your network and try again.')));
            }, timeout);

            // Store request information
            this.pendingRequests.set(id, {
                resolve: (data) => {
                    clearTimeout(timeoutId);
                    resolve(data);
                },
                reject: (error) => {
                    clearTimeout(timeoutId);
                    reject(error);
                }
            });

            // Send request
            const message = {
                id,
                method,
                type: 'request',
                params
            };
            try {
                this.sendMessage(message);
            } catch (error) {
                console.error('Failed to send IPC message:', error);
                this.pendingRequests.delete(id);
                reject(error);
            }
        });
    }

    /**
     * Send asynchronous request with event callback
     * @param {string} method Method name
     * @param {object} params Parameters
     * @param {function} eventCallback Event callback function, receives event data
     * @param {number} timeout Timeout in milliseconds, default 10 seconds
     * @returns {Promise} Returns Promise object, resolves with {response, cleanup}
     * 
     * @example
     * // Request with event monitoring
     * try {
     *     const result = await ipcManager.requestWithEvents(
     *         'startPrint',
     *         { filename: 'model.gcode' },
     *         (eventMethod, eventData) => {
     *             if (eventMethod === 'onPrintProgress') {
     *                 console.log(`Print progress: ${eventData.progress}%`);
     *             } else if (eventMethod === 'onPrintStarted') {
     *                 console.log('Print job started:', eventData.jobId);
     *             }
     *         },
     *         15000
     *     );
     *     
     *     console.log('Print completed:', result.response);
     *     
     *     // Clean up resources
     *     if (result.cleanup) {
     *         result.cleanup();
     *     }
     * } catch (error) {
     *     console.error('Print request failed:', error.message);
     * }
     * 
     * @example
     * // File download with progress tracking
     * const downloadResult = await ipcManager.requestWithEvents(
     *     'downloadFile',
     *     { url: 'https://example.com/file.zip', destination: './downloads/' },
     *     (method, data) => {
     *         if (method === 'onDownloadProgress') {
     *             const percent = Math.round((data.downloaded / data.totalSize) * 100);
     *             console.log(`Download progress: ${percent}% (${data.speed})`);
     *         }
     *     }
     * );
     */
    requestWithEvents(method, params = {}, eventCallback = null, timeout = 10000) {
        return new Promise((resolve, reject) => {
            const id = this.generateRequestId();
            let isCompleted = false;

            // Set timeout
            const timeoutId = setTimeout(() => {
                if (!isCompleted) {
                    isCompleted = true;
                    this.pendingRequests.delete(id);
                    console.error('IPC: Request timeout:', method);
                    reject(new Error(getI18nText('requestTimeout', 'Request timeout, please check your network and try again.')));
                }
            }, timeout);

            // Cleanup function
            const cleanup = () => {
                if (!isCompleted) {
                    isCompleted = true;
                    clearTimeout(timeoutId);
                    this.pendingRequests.delete(id);
                }
            };

            // Store request information, including event callback
            this.pendingRequests.set(id, {
                resolve: (data) => {
                    if (!isCompleted) {
                        isCompleted = true;
                        clearTimeout(timeoutId);
                        // Return response data and cleanup function
                        resolve({ response: data, cleanup });
                    }
                },
                reject: (error) => {
                    if (!isCompleted) {
                        isCompleted = true;
                        clearTimeout(timeoutId);
                        reject(error);
                    }
                },
                eventCallback: eventCallback, // Store event callback
                requestId: id
            });

            // Send request
            const message = {
                id,
                method,
                type: 'request',
                params
            };
            try {
                this.sendMessage(message);
            } catch (error) {
                console.error('Failed to send IPC message:', error);
                this.pendingRequests.delete(id);
                reject(error);
            }
        });
    }

    /**
     * Send response
     * @param {string} id Request ID
     * @param {string} method Method name
     * @param {number} code Response code (0 = success, other = error)
     * @param {string} message Response message
     * @param {object} data Response data
     * 
     * @example
     * // Send success response
     * ipcManager.sendResponse('req-1001', 'getData', 0, 'success', {
     *     users: [
     *         { id: 1, name: 'John Doe' },
     *         { id: 2, name: 'Jane Smith' }
     *     ],
     *     total: 2
     * });
     * 
     * @example
     * // Send error response
     * ipcManager.sendResponse('req-1002', 'deleteUser', 404, 'User not found', null);
     * 
     * @example
     * // Send response with validation error
     * ipcManager.sendResponse('req-1003', 'createUser', 400, 'Email is required', {
     *     validationErrors: ['email is required', 'name must be at least 2 characters']
     * });
     */
    sendResponse(id, method, code, message, data) {
        const response = {
            id,
            method,
            type: 'response',
            code,
            message,
            data
        };

        this.sendMessage(response);
    }

    /**
     * Send event
     * @param {string} method Event name
     * @param {object} data Event data
     * @param {string} requestId Associated request ID (optional)
     * 
     * @example
     * // Send global event
     * ipcManager.sendEvent('onSystemStatusChanged', {
     *     status: 'ready',
     *     timestamp: new Date().toISOString(),
     *     cpu: 45.2,
     *     memory: 67.8
     * });
     * 
     * @example
     * // Send event associated with a specific request
     * ipcManager.sendEvent('onTaskProgress', {
     *     progress: 75,
     *     currentStep: 'processing',
     *     estimatedTimeLeft: 30
     * }, 'req-1001');
     * 
     * @example
     * // Send notification event
     * ipcManager.sendEvent('onNotification', {
     *     type: 'info',
     *     title: 'Task Completed',
     *     message: 'Your file has been processed successfully',
     *     timestamp: Date.now()
     * });
     */
    sendEvent(method, data, requestId = null) {
        const event = {
            id: requestId,
            method,
            type: 'event',
            data
        };

        this.sendMessage(event);
    }

    /**
     * Listen for events
     * @param {string} method Event name
     * @param {function} handler Event handler
     * 
     * @example
     * // Listen for system status updates
     * ipcManager.on('onSystemStatus', (data) => {
     *     console.log('System status updated:', data);
     *     updateStatusDisplay(data.status, data.cpu, data.memory);
     * });
     * 
     * @example
     * // Listen for print progress events
     * ipcManager.on('onPrintProgress', (data) => {
     *     const progressBar = document.getElementById('progress');
     *     progressBar.value = data.progress;
     *     
     *     if (data.progress === 100) {
     *         showNotification('Print completed successfully!');
     *     }
     * });
     * 
     * @example
     * // Listen for error events
     * ipcManager.on('onError', (data) => {
     *     console.error('Error occurred:', data.message);
     *     showErrorDialog(data.message, data.code);
     * });
     */
    on(method, handler) {
        if (!this.eventHandlers.has(method)) {
            this.eventHandlers.set(method, []);
        }
        this.eventHandlers.get(method).push(handler);
    }

    /**
     * Remove event listener
     * @param {string} method Event name
     * @param {function} handler Event handler
     * 
     * @example
     * // Remove specific event handler
     * const statusHandler = (data) => {
     *     console.log('Status:', data);
     * };
     * 
     * // Add handler
     * ipcManager.on('onSystemStatus', statusHandler);
     * 
     * // Later, remove the same handler
     * ipcManager.off('onSystemStatus', statusHandler);
     * 
     * @example
     * // Component cleanup pattern
     * class MyComponent {
     *     constructor() {
     *         this.handleProgress = (data) => {
     *             this.updateProgress(data.progress);
     *         };
     *         ipcManager.on('onProgress', this.handleProgress);
     *     }
     *     
     *     destroy() {
     *         // Clean up event listener when component is destroyed
     *         ipcManager.off('onProgress', this.handleProgress);
     *     }
     * }
     */
    off(method, handler) {
        const handlers = this.eventHandlers.get(method);
        if (handlers) {
            const index = handlers.indexOf(handler);
            if (index > -1) {
                handlers.splice(index, 1);
            }
        }
    }

    /**
     * Register request handler (synchronous)
     * @param {string} method Method name
     * @param {function} handler Handler function
     * 
     * Note: For synchronous handlers, errors are thrown as exceptions.
     * The system will automatically convert them to error responses with code 1.
     * 
     * @example
     * // Simple synchronous handler
     * ipcManager.onRequest('getVersion', (params) => {
     *     return {
     *         version: '1.0.0',
     *         buildDate: '2025-08-18',
     *         platform: navigator.platform
     *     };
     * });
     * 
     * @example
     * // Handler with parameter validation
     * ipcManager.onRequest('calculateArea', (params) => {
     *     const { width, height } = params;
     *     
     *     if (!width || !height) {
     *         throw new Error('Width and height are required');
     *     }
     *     
     *     if (width <= 0 || height <= 0) {
     *         throw new Error('Width and height must be positive numbers');
     *     }
     *     
     *     return {
     *         area: width * height,
     *         perimeter: 2 * (width + height)
     *     };
     * });
     * 
     * @example
     * // Handler that accesses local storage
     * ipcManager.onRequest('getUserPreferences', (params) => {
     *     const prefs = localStorage.getItem('userPrefs');
     *     return prefs ? JSON.parse(prefs) : {
     *         theme: 'light',
     *         language: 'en',
     *         notifications: true
     *     };
     * });
     * 
     * @example
     * // Error handling with custom error messages
     * ipcManager.onRequest('getUserById', (params) => {
     *     const { userId } = params;
     *     
     *     // Validate required parameters
     *     if (!userId) {
     *         throw new Error('User ID is required');
     *     }
     *     
     *     // Validate parameter format
     *     if (typeof userId !== 'string' && typeof userId !== 'number') {
     *         throw new Error('User ID must be a string or number');
     *     }
     *     
     *     // Mock database lookup
     *     const users = {
     *         '1': { id: 1, name: 'John Doe', email: 'john@example.com' },
     *         '2': { id: 2, name: 'Jane Smith', email: 'jane@example.com' }
     *     };
     *     
     *     const user = users[userId.toString()];
     *     
     *     // Return error if user not found
     *     if (!user) {
     *         throw new Error(`User with ID ${userId} not found`);
     *     }
     *     
     *     return user;
     * });
     * 
     * @example
     * // Permission-based error handling
     * ipcManager.onRequest('getSecretData', (params) => {
     *     const { token, dataType } = params;
     *     
     *     // Check authentication
     *     if (!token) {
     *         throw new Error('Authentication token is required');
     *     }
     *     
     *     // Mock token validation
     *     const validTokens = ['admin-token', 'user-token'];
     *     if (!validTokens.includes(token)) {
     *         throw new Error('Invalid or expired authentication token');
     *     }
     *     
     *     // Check permissions
     *     if (dataType === 'admin' && token !== 'admin-token') {
     *         throw new Error('Insufficient permissions to access admin data');
     *     }
     *     
     *     // Return mock data based on permissions
     *     const data = {
     *         user: { message: 'User level data', timestamp: new Date().toISOString() },
     *         admin: { message: 'Admin level data', users: 150, revenue: 25000 }
     *     };
     *     
     *     return data[dataType] || data.user;
     * });
     * 
     * @example
     * // Business logic validation errors
     * ipcManager.onRequest('validateOrder', (params) => {
     *     const { items, couponCode, customerAge } = params;
     *     
     *     // Validate order items
     *     if (!items || !Array.isArray(items) || items.length === 0) {
     *         throw new Error('Order must contain at least one item');
     *     }
     *     
     *     // Check item availability
     *     const unavailableItems = items.filter(item => item.stock === 0);
     *     if (unavailableItems.length > 0) {
     *         throw new Error(`Items out of stock: ${unavailableItems.map(i => i.name).join(', ')}`);
     *     }
     *     
     *     // Validate age-restricted items
     *     const ageRestrictedItems = items.filter(item => item.ageRestriction && item.ageRestriction > customerAge);
     *     if (ageRestrictedItems.length > 0) {
     *         throw new Error(`Age restriction violation: Customer must be at least ${Math.max(...ageRestrictedItems.map(i => i.ageRestriction))} years old`);
     *     }
     *     
     *     // Validate coupon
     *     if (couponCode) {
     *         const validCoupons = ['SAVE10', 'WELCOME20'];
     *         if (!validCoupons.includes(couponCode)) {
     *             throw new Error('Invalid coupon code');
     *         }
     *     }
     *     
     *     // Calculate total
     *     const subtotal = items.reduce((sum, item) => sum + item.price * item.quantity, 0);
     *     const discount = couponCode === 'SAVE10' ? 0.1 : (couponCode === 'WELCOME20' ? 0.2 : 0);
     *     const total = subtotal * (1 - discount);
     *     
     *     return {
     *         valid: true,
     *         subtotal: subtotal,
     *         discount: discount * 100 + '%',
     *         total: total,
     *         message: 'Order validation successful'
     *     };
     * });
     */
    onRequest(method, handler) {
        this.requestHandlers.set(method, handler);
    }

    /**
     * Register async request handler
     * @param {string} method Method name
     * @param {function} handler Async handler function (params, respond)
     * 
     * The respond parameter provides convenient methods:
     * - respond.success(data, message) - Send success response
     * - respond.error(message, code, data) - Send error response
     * - respond.notFound(message) - Send 404 error
     * - respond.badRequest(message, validationErrors) - Send 400 error
     * - respond.unauthorized(message) - Send 401 error
     * - respond.forbidden(message) - Send 403 error
     * - respond.serverError(message, error) - Send 500 error
     * - respond.custom(code, message, data) - Send custom response
     * - respond(code, message, data) - Original format (backward compatibility)
     * 
     * @example
     * // Using convenient response methods
     * ipcManager.onRequestAsync('saveFile', async (params, respond) => {
     *     try {
     *         const { filename, content } = params;
     *         
     *         // Validation
     *         if (!filename) {
     *             respond.badRequest('Filename is required');
     *             return;
     *         }
     *         
     *         if (!content) {
     *             respond.badRequest('Content is required', ['content cannot be empty']);
     *             return;
     *         }
     *         
     *         // Simulate async file save operation
     *         await new Promise(resolve => setTimeout(resolve, 1000));
     *         
     *         // Success response
     *         respond.success({
     *             filename: filename,
     *             size: new Blob([content]).size,
     *             savedAt: new Date().toISOString()
     *         }, 'File saved successfully');
     *         
     *     } catch (error) {
     *         respond.serverError('Failed to save file', error);
     *     }
     * });
     * 
     * @example
     * // User authentication handler
     * ipcManager.onRequestAsync('login', async (params, respond) => {
     *     const { username, password } = params;
     *     
     *     if (!username || !password) {
     *         respond.badRequest('Username and password are required');
     *         return;
     *     }
     *     
     *     try {
     *         const user = await authenticateUser(username, password);
     *         
     *         if (!user) {
     *             respond.unauthorized('Invalid username or password');
     *             return;
     *         }
     *         
     *         respond.success({
     *             user: { id: user.id, name: user.name, role: user.role },
     *             token: generateToken(user),
     *             expiresIn: '24h'
     *         });
     *         
     *     } catch (error) {
     *         respond.serverError('Authentication service unavailable');
     *     }
     * });
     * 
     * @example
     * // API data fetching with error handling
     * ipcManager.onRequestAsync('fetchUserData', async (params, respond) => {
     *     const { userId } = params;
     *     
     *     if (!userId) {
     *         respond.badRequest('User ID is required');
     *         return;
     *     }
     *     
     *     try {
     *         const response = await fetch(`/api/users/${userId}`);
     *         
     *         if (response.status === 404) {
     *             respond.notFound('User not found');
     *             return;
     *         }
     *         
     *         if (response.status === 403) {
     *             respond.forbidden('Access denied');
     *             return;
     *         }
     *         
     *         if (!response.ok) {
     *             respond.error('Failed to fetch user data', response.status);
     *             return;
     *         }
     *         
     *         const userData = await response.json();
     *         respond.success(userData);
     *         
     *     } catch (error) {
     *         respond.serverError('Network error occurred', error);
     *     }
     * });
     * 
     * @example
     * // Backward compatibility - original response format still works
     * ipcManager.onRequestAsync('legacyMethod', async (params, respond) => {
     *     try {
     *         const result = await processData(params);
     *         respond(0, 'success', result); // Original format
     *     } catch (error) {
     *         respond(1, error.message, null); // Original format
     *     }
     * });
     */
    onRequestAsync(method, handler) {
        this.asyncRequestHandlers.set(method, handler);
    }

    /**
     * Register async request handler with events
     * @param {string} method Method name
     * @param {function} handler Async handler function (params, respond, sendEvent)
     * 
     * The handler function receives three parameters:
     * - params: Request parameters
     * - respond: Enhanced response helper with convenient methods
     * - sendEvent: Function to send events (eventMethod, eventData)
     * 
     * Response helper methods:
     * - respond.success(data, message) - Send success response
     * - respond.error(message, code, data) - Send error response
     * - respond.notFound(message) - Send 404 error
     * - respond.badRequest(message, validationErrors) - Send 400 error
     * - respond.unauthorized(message) - Send 401 error
     * - respond.forbidden(message) - Send 403 error
     * - respond.serverError(message, error) - Send 500 error
     * - respond.custom(code, message, data) - Send custom response
     * - respond(code, message, data) - Original format (backward compatibility)
     * 
     * @example
     * // Print job handler with convenient response methods
     * ipcManager.onRequestAsyncWithEvents('startPrint', async (params, respond, sendEvent) => {
     *     try {
     *         const { filename, settings } = params;
     *         
     *         // Validation with convenient error responses
     *         if (!filename) {
     *             respond.badRequest('Filename is required');
     *             return;
     *         }
     *         
     *         if (!settings.temperature || settings.temperature < 0) {
     *             respond.badRequest('Invalid temperature setting', ['temperature must be >= 0']);
     *             return;
     *         }
     *         
     *         // Send start event
     *         sendEvent('onPrintStarted', { 
     *             jobId: 'job-' + Date.now(),
     *             filename: filename,
     *             startTime: new Date().toISOString(),
     *             estimatedDuration: '2h 30m'
     *         });
     *         
     *         // Simulate printing with progress updates
     *         for (let progress = 0; progress <= 100; progress += 10) {
     *             await new Promise(resolve => setTimeout(resolve, 500));
     *             
     *             sendEvent('onPrintProgress', {
     *                 progress: progress,
     *                 currentLayer: Math.floor(progress / 2),
     *                 totalLayers: 50,
     *                 timeRemaining: Math.max(0, (100 - progress) * 90) + 's'
     *             });
     *         }
     *         
     *         // Send success response with convenient method
     *         respond.success({ 
     *             jobId: 'job-' + Date.now(),
     *             duration: '2h 25m',
     *             layersCompleted: 50
     *         }, 'Print job completed successfully');
     *         
     *     } catch (error) {
     *         sendEvent('onPrintError', { error: error.message });
     *         respond.serverError('Print job failed', error);
     *     }
     * });
     * 
     * @example
     * // File upload with progress and validation
     * ipcManager.onRequestAsyncWithEvents('uploadFile', async (params, respond, sendEvent) => {
     *     const { file, destination } = params;
     *     
     *     // Validation
     *     if (!file) {
     *         respond.badRequest('File is required');
     *         return;
     *     }
     *     
     *     if (!destination) {
     *         respond.badRequest('Destination path is required');
     *         return;
     *     }
     *     
     *     try {
     *         sendEvent('onUploadStarted', {
     *             filename: file.name,
     *             size: file.size,
     *             destination: destination,
     *             startTime: new Date().toISOString()
     *         });
     *         
     *         // Simulate upload progress
     *         const totalChunks = 20;
     *         for (let chunk = 1; chunk <= totalChunks; chunk++) {
     *             await new Promise(resolve => setTimeout(resolve, 100));
     *             
     *             const progress = Math.round((chunk / totalChunks) * 100);
     *             sendEvent('onUploadProgress', {
     *                 chunk: chunk,
     *                 totalChunks: totalChunks,
     *                 progress: progress,
     *                 uploadedBytes: Math.round((file.size * chunk) / totalChunks),
     *                 totalBytes: file.size
     *             });
     *         }
     *         
     *         sendEvent('onUploadCompleted', {
     *             filename: file.name,
     *             finalPath: destination + '/' + file.name,
     *             uploadTime: (totalChunks * 100) + 'ms'
     *         });
     *         
     *         respond.success({
     *             uploadedFile: destination + '/' + file.name,
     *             size: file.size,
     *             checksum: 'sha256:abc123...'
     *         });
     *         
     *     } catch (error) {
     *         sendEvent('onUploadError', { 
     *             error: error.message,
     *             filename: file.name 
     *         });
     *         respond.serverError('Upload failed', error);
     *     }
     * });
     * 
     * @example
     * // Data processing with multiple stages and error handling
     * ipcManager.onRequestAsyncWithEvents('processDataset', async (params, respond, sendEvent) => {
     *     try {
     *         const { dataFile, analysisType, options } = params;
     *         
     *         // Validation
     *         if (!dataFile) {
     *             respond.badRequest('Data file is required');
     *             return;
     *         }
     *         
     *         if (!['basic', 'advanced', 'statistical'].includes(analysisType)) {
     *             respond.badRequest('Invalid analysis type', [
     *                 'analysisType must be one of: basic, advanced, statistical'
     *             ]);
     *             return;
     *         }
     *         
     *         const stages = ['loading', 'validation', 'processing', 'analysis', 'export'];
     *         
     *         sendEvent('onProcessingStarted', {
     *             dataFile: dataFile,
     *             analysisType: analysisType,
     *             totalStages: stages.length,
     *             estimatedTime: '5 minutes'
     *         });
     *         
     *         for (let i = 0; i < stages.length; i++) {
     *             const stage = stages[i];
     *             const progress = Math.round(((i + 1) / stages.length) * 100);
     *             
     *             sendEvent('onProcessingStage', {
     *                 stage: stage,
     *                 stageIndex: i + 1,
     *                 totalStages: stages.length,
     *                 progress: progress,
     *                 message: `Executing ${stage} stage...`
     *             });
     *             
     *             // Simulate stage processing time
     *             await new Promise(resolve => setTimeout(resolve, 800 + Math.random() * 400));
     *             
     *             // Simulate stage-specific error
     *             if (stage === 'validation' && Math.random() < 0.1) {
     *                 throw new Error('Data validation failed - corrupted records found');
     *             }
     *         }
     *         
     *         respond.success({
     *             resultFile: 'analysis_result_' + Date.now() + '.json',
     *             recordsProcessed: 15420,
     *             analysisResults: {
     *                 mean: 42.7,
     *                 median: 41.2,
     *                 standardDeviation: 8.9
     *             }
     *         }, 'Data processing completed successfully');
     *         
     *     } catch (error) {
     *         sendEvent('onProcessingError', { 
     *             stage: 'unknown',
     *             error: error.message 
     *         });
     *         
     *         if (error.message.includes('validation')) {
     *             respond.badRequest('Data validation failed', [error.message]);
     *         } else if (error.message.includes('permission')) {
     *             respond.forbidden('Insufficient permissions to process data');
     *         } else {
     *             respond.serverError('Processing failed', error);
     *         }
     *     }
     * });
     * 
     * @example
     * // Backward compatibility with original response format
     * ipcManager.onRequestAsyncWithEvents('legacyTaskWithEvents', async (params, respond, sendEvent) => {
     *     try {
     *         sendEvent('onTaskStarted', { taskId: 'task-123' });
     *         
     *         // Process task...
     *         await new Promise(resolve => setTimeout(resolve, 1000));
     *         
     *         sendEvent('onTaskCompleted', { taskId: 'task-123' });
     *         
     *         // Original response format still works
     *         respond(0, 'success', { result: 'completed' });
     *         
     *     } catch (error) {
     *         sendEvent('onTaskError', { error: error.message });
     *         respond(1, error.message, null); // Original format
     *     }
     * });
     */
    onRequestAsyncWithEvents(method, handler) {
        this.asyncRequestHandlersWithEvents.set(method, handler);
    }

    /**
     * Remove request handler
     * @param {string} method Method name
     * 
     * @example
     * // Remove all types of handlers for a method
     * ipcManager.offRequest('oldMethod');
     * 
     * @example
     * // Component cleanup pattern
     * class ApiHandler {
     *     constructor() {
     *         // Register handlers
     *         ipcManager.onRequest('getData', this.handleGetData.bind(this));
     *         ipcManager.onRequestAsync('processData', this.handleProcessData.bind(this));
     *         ipcManager.onRequestAsyncWithEvents('longTask', this.handleLongTask.bind(this));
     *     }
     *     
     *     destroy() {
     *         // Clean up all handlers when component is destroyed
     *         ipcManager.offRequest('getData');
     *         ipcManager.offRequest('processData');
     *         ipcManager.offRequest('longTask');
     *     }
     * }
     * 
     * @example
     * // Replace handler with new implementation
     * // Remove old handler first
     * ipcManager.offRequest('calculatePrice');
     * 
     * // Register new improved handler
     * ipcManager.onRequestAsync('calculatePrice', async (params, respond) => {
     *     // New implementation with better logic
     *     const price = calculatePriceV2(params);
     *     respond(0, 'success', { price });
     * });
     */
    offRequest(method) {
        this.requestHandlers.delete(method);
        this.asyncRequestHandlers.delete(method);
        this.asyncRequestHandlersWithEvents.delete(method);
    }
}

// Create global instance
const ipcManager = new IPCManager();
window.nativeIpc = {};
// Export convenience methods
window.nativeIpc.request = (method, params, timeout) => ipcManager.request(method, params, timeout);
window.nativeIpc.requestWithEvents = (method, params, eventCallback, timeout) => ipcManager.requestWithEvents(method, params, eventCallback, timeout);
window.nativeIpc.on = (method, handler) => ipcManager.on(method, handler);
window.nativeIpc.off = (method, handler) => ipcManager.off(method, handler);
window.nativeIpc.onRequest = (method, handler) => ipcManager.onRequest(method, handler);
window.nativeIpc.onRequestAsync = (method, handler) => ipcManager.onRequestAsync(method, handler);
window.nativeIpc.onRequestAsyncWithEvents = (method, handler) => ipcManager.onRequestAsyncWithEvents(method, handler);
window.nativeIpc.offRequest = (method) => ipcManager.offRequest(method);
window.nativeIpc.sendEvent = (method, data, requestId) => ipcManager.sendEvent(method, data, requestId);

// Can also be exported as module
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { IPCManager, ipcManager };
}

console.log('IPC: JavaScript side initialization complete');
