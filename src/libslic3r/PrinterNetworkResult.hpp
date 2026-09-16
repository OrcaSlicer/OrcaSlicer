#ifndef slic3r_PrinterNetworkResult_hpp_
#define slic3r_PrinterNetworkResult_hpp_

#include <string>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Slic3r {

/**
 * Printer network error codes for the slicer software
 * Aligned with ElegooError design
 */
enum class PrinterNetworkErrorCode
{
    // Success status
    SUCCESS = 0, // Operation successful

    // General errors (1-99)
    UNKNOWN_ERROR             = 1, // Unknown error
    NOT_INITIALIZED           = 2, // Not initialized
    INVALID_PARAMETER         = 3, // Invalid parameter (value error, type error, etc.)
    OPERATION_TIMEOUT         = 4, // Operation timeout (including connection, file transfer, etc.)
    OPERATION_CANCELLED       = 5, // Operation canceled
    OPERATION_IN_PROGRESS     = 6, // Operation in progress
    OPERATION_NOT_IMPLEMENTED = 7, // Operation not implemented
    NETWORK_ERROR             = 8, // Network error
    INSUFFICIENT_MEMORY       = 9, // Insufficient memory
    NOT_CONNECTED_TO_SUBSERVICE = 10, // Not connected to subservice

    // Authentication-related errors (200-299)
    INVALID_USERNAME_OR_PASSWORD = 201, // Username or password invalid
    INVALID_TOKEN                = 202, // Token invalid
    INVALID_ACCESS_CODE          = 203, // Access code invalid
    INVALID_PIN_CODE             = 204, // PIN code invalid

    // File transfer-related errors (300-399)
    FILE_TRANSFER_FAILED = 300, // File transfer failed (including upload, download)
    FILE_NOT_FOUND       = 301, // File not found
    FILE_ALREADY_EXISTS  = 302, // File already exists
    FILE_ACCESS_DENIED   = 303, // File access denied

    // 1000-1999 Printer business errors
    PRINTER_NOT_FOUND                 = 1000, // Printer not found
    PRINTER_CONNECTION_ERROR          = 1001, // Printer connection error
    PRINTER_CONNECTION_LIMIT_EXCEEDED = 1002, // Printer connection limit exceeded
    PRINTER_ALREADY_CONNECTED         = 1003, // Printer already connected or connecting
    PRINTER_BUSY                      = 1004, // Printer busy
    PRINTER_COMMAND_FAILED            = 1005, // Printer command execution failed
    PRINTER_UNKNOWN_ERROR            = 1006, // Printer internal error
    PRINTER_INVALID_PARAMETER         = 1007, // send data format error
    PRINTER_INVALID_RESPONSE          = 1008, // Printer response invalid data
    PRINTER_ACCESS_DENIED             = 1009, // Printer access denied
    PRINTER_MISSING_BED_LEVELING_DATA = 1010, // Printer missing bed leveling data
    PRINTER_PRINT_FILE_NOT_FOUND = 1011,       // Printer print file not found
    PRINTER_OFFLINE = 1012,                    // Printer offline
    PRINTER_FILAMENT_RUNOUT = 1013,            // Printer filament runout

    // 2000-2999 Server business errors
    SERVER_UNKNOWN_ERROR = 2000,     // Server error
    SERVER_INVALID_RESPONSE = 2001,  // Server response invalid data
    SERVER_TOO_MANY_REQUESTS = 2002, // Too many requests
    SERVER_RTM_NOT_CONNECTED = 2049, // RTM client not connected or not logged in
  
    SERVER_UNAUTHORIZED = 2050, // Unauthorized access
    SERVER_FORBIDDEN = 2051,    // Forbidden access
    SERVER_PIN_CODE_MISMATCH = 2052,// PIN code does not match, e.g. SN or model does not match

    HOST_TYPE_NOT_SUPPORTED = 9998,          // Host type not supported
    PRINTER_NETWORK_EXCEPTION = 9999,          // External error
    PRINTER_NETWORK_INVALID_DATA = 10000,    // Printer network invalid data
    //PRINTER_NOT_FOUND = 10001,                // Printer not found
    PRINTER_ALREADY_EXISTS = 10002,           // Printer already exists
    PRINTER_TYPE_NOT_SUPPORTED = 10003,       // Printer type not supported
    PRINTER_MMS_NOT_CONNECTED = 10004,        // Printer MMS not connected
    CREATE_NETWORK_FOR_HOST_TYPE_FAILED = 10005, // Failed to create network for host type
    PRINTER_NOT_SELECTED = 10006,                // Printer not selected
    PRINTER_MMS_FILAMENT_NOT_MAPPED = 10007,     // Printer MMS filament not mapped
    PRINTER_HOST_NOT_MATCH = 10008,      // host not match with the local printer
    PRINTER_NETWORK_NOT_INITIALIZED = 10009, // Printer network not initialized
    PRINTER_PLUGIN_NOT_INSTALLED = 10010, // Printer plugin not installed
    USER_NETWORK_BUSY = 10011, // User network busy
    PRINTER_SERIAL_NUMBER_EMPTY = 10012, // Printer serial number empty
    PRINTER_NOT_CONNECTED_TO_UNBIND = 10013, // Printer not connected to unbind
    FILE_TOO_LARGE = 10014, // File too large

    // IPC (Inter-Process Communication) errors (100000-100099)
    IPC_NOT_CONNECTED        = 100000, // IPC connection not established
    IPC_CONNECTION_FAILED    = 100001, // Failed to connect to master process
    IPC_CONNECTION_LOST      = 100002, // Connection lost during operation
    IPC_CONNECTION_CLOSED    = 100003, // Connection closed by peer
    IPC_IO_ERROR             = 100004, // I/O error during communication
    IPC_SEND_FAILED          = 100005, // Failed to send request to main process
    IPC_INVALID_MESSAGE      = 100006, // Invalid message format
    IPC_PROTOCOL_ERROR       = 100007, // Protocol violation
    IPC_TOO_MANY_PENDING     = 100008, // Too many pending requests
    IPC_DEADLOCK_PREVENTED   = 100009, // Deadlock prevented (called from I/O thread)
};


std::string getErrorMessage(PrinterNetworkErrorCode error);

template<typename T = std::monostate>
struct PrinterNetworkResult
{
    PrinterNetworkErrorCode code = PrinterNetworkErrorCode::SUCCESS;
    std::string message = "ok";
    std::optional<T> data;

    // Default constructor
    PrinterNetworkResult() = default;

    // Perfect forwarding constructor, supports results with data
    template<typename U> PrinterNetworkResult(PrinterNetworkErrorCode resultCode, U&& dataVal, std::string_view msg = "") :
    code(resultCode), data(std::forward<U>(dataVal)), message(msg.empty() ? getErrorMessage(resultCode) : msg) {}

    // Check if successful
    bool isSuccess() const noexcept
    {
        return code == PrinterNetworkErrorCode::SUCCESS;
    }

    // Check if failed
    bool isError() const noexcept
    {
        return code != PrinterNetworkErrorCode::SUCCESS;
    }

    // Check if data exists
    bool hasData() const noexcept
    {
        return data.has_value();
    }

    // Get data reference (only if data exists)
    const T& value() const&
    {
        return data.value();
    }

    T& value() &
    {
        return data.value();
    }

    T&& value() &&
    {
        return std::move(data).value();
    }

    // Functional programming support - map operation
    template <typename F>
    auto map(F&& func) const -> PrinterNetworkResult<std::invoke_result_t<F, T>>
    {
        if (isError() || !hasData()) {
            return PrinterNetworkResult<std::invoke_result_t<F, T>>::Error(code, message);
        }
        return PrinterNetworkResult<std::invoke_result_t<F, T>>::Ok(func(data.value()));
    }

    // Functional programming support - flatMap operation
    template <typename F>
    auto flatMap(F&& func) const -> std::invoke_result_t<F, T>
    {
        if (isError() || !hasData()) {
            using ReturnType = std::invoke_result_t<F, T>;
            return ReturnType::Error(code, message);
        }
        return func(data.value());
    }
};



} // namespace Slic3r

#endif 