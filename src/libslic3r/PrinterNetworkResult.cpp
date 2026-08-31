#include "PrinterNetworkResult.hpp"
#include "I18N.hpp"
namespace Slic3r {

// Get localized error message based on error code
std::string getErrorMessage(PrinterNetworkErrorCode error)
{
    switch (error) {
        case PrinterNetworkErrorCode::SUCCESS:
            return _u8L("Success");            
        case PrinterNetworkErrorCode::UNKNOWN_ERROR:
            return _u8L("An unknown error occurred. Please try again later.");
        case PrinterNetworkErrorCode::NOT_INITIALIZED:
            return _u8L("Not initialized. Please try again later.");
        case PrinterNetworkErrorCode::INVALID_PARAMETER:
            return _u8L("Invalid parameter. Please try again.");
        case PrinterNetworkErrorCode::OPERATION_TIMEOUT:
            return _u8L("Operation timed out. Please try again.");
        case PrinterNetworkErrorCode::OPERATION_CANCELLED:
            return _u8L("Operation cancelled");
        case PrinterNetworkErrorCode::OPERATION_IN_PROGRESS:
            return _u8L("Operation is in progress. Please wait.");
        case PrinterNetworkErrorCode::PRINTER_ACCESS_DENIED:
            return _u8L("Access denied. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_MISSING_BED_LEVELING_DATA:
            return _u8L("Missing bed leveling data. Please level the bed.");
        case PrinterNetworkErrorCode::PRINTER_PRINT_FILE_NOT_FOUND:
            return _u8L("Print file not found. Please troubleshoot.");
        case PrinterNetworkErrorCode::PRINTER_CONNECTION_ERROR:
            return _u8L("Connection failed. Please check the network of your computer and the printer, then try again.");
        case PrinterNetworkErrorCode::NETWORK_ERROR:
            return _u8L("Network error occurred. Please check the network of your computer and the printer, then try again.");
        case PrinterNetworkErrorCode::NOT_CONNECTED_TO_SUBSERVICE:
            return _u8L("Not connected to subservice. Please try again later.");
        case PrinterNetworkErrorCode::INVALID_USERNAME_OR_PASSWORD:
            return _u8L("Unauthorized access, please check your account login status and try again.");
        case PrinterNetworkErrorCode::INVALID_TOKEN:
            return _u8L("Account info has expired, please log in again.");
        case PrinterNetworkErrorCode::INVALID_ACCESS_CODE:
            return _u8L("Invalid access code. Please check and try again.");
        case PrinterNetworkErrorCode::INVALID_PIN_CODE:
            return _u8L("Invalid PIN: Please check the printer's region matches your account region, or check that the PIN was entered correctly.");
        case PrinterNetworkErrorCode::PRINTER_CONNECTION_LIMIT_EXCEEDED:
            return _u8L("Connection limit reached. Please check and try again.");
        case PrinterNetworkErrorCode::FILE_TRANSFER_FAILED:
            return _u8L("File upload failed. Please try again later.");
        case PrinterNetworkErrorCode::FILE_NOT_FOUND:
            return _u8L("File not found. Please troubleshoot.");
        case PrinterNetworkErrorCode::FILE_ALREADY_EXISTS:
            return _u8L("File already exists. Please check and try again.");
        case PrinterNetworkErrorCode::FILE_ACCESS_DENIED:
            return _u8L("File access denied. Please check permissions and try again.");
        case PrinterNetworkErrorCode::PRINTER_BUSY:
            return _u8L("Printer is busy. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_COMMAND_FAILED:
            return _u8L("Operation failed. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_ALREADY_CONNECTED:
            return _u8L("Printer already connected");
        case PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR:
            return _u8L("Printer error. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_INVALID_PARAMETER:
            return _u8L("Invalid printer parameter. Please check and try again.");
        case PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION:
            return _u8L("Printer network error. Please try again later or restart the printer.");
        case PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA:
            return _u8L("Invalid printer data received. Please try again later or restart the printer.");
        case PrinterNetworkErrorCode::PRINTER_MMS_NOT_CONNECTED:
            return _u8L("Printer MMS connection failed");
        case PrinterNetworkErrorCode::PRINTER_NOT_FOUND:
            return _u8L("Printer not found. Please refresh and try again.");
        case PrinterNetworkErrorCode::PRINTER_ALREADY_EXISTS:
            return _u8L("Printer already exists, please check.");
        case PrinterNetworkErrorCode::PRINTER_TYPE_NOT_SUPPORTED:
            return _u8L("Printer type not supported");
        case PrinterNetworkErrorCode::HOST_TYPE_NOT_SUPPORTED:
            return _u8L("Host type not supported");
        case PrinterNetworkErrorCode::CREATE_NETWORK_FOR_HOST_TYPE_FAILED:
            return _u8L("Failed to establish network connection");
        case PrinterNetworkErrorCode::PRINTER_NOT_SELECTED:
            return _u8L("Printer not selected. Please try again.");
        case PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED:
            return _u8L("Unmapped filament detected, printing cannot be started.");
        case PrinterNetworkErrorCode::PRINTER_HOST_NOT_MATCH:
            return _u8L("The IP address does not match the printer's information. Please remove the printer and add it again.");
        case PrinterNetworkErrorCode::PRINTER_OFFLINE:
            return _u8L("Printer is offline. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_FILAMENT_RUNOUT:
            return _u8L("No filament detected. Please load filament before printing.");
        case PrinterNetworkErrorCode::PRINTER_NETWORK_NOT_INITIALIZED:
            return _u8L("Printer network not initialized. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_PLUGIN_NOT_INSTALLED:
            return _u8L("Printer plugin not installed. Please install the plugin and try again.");
        case PrinterNetworkErrorCode::SERVER_UNKNOWN_ERROR:
            return _u8L("Server error. Please try again later.");
        case PrinterNetworkErrorCode::SERVER_INVALID_RESPONSE:
            return _u8L("Server response invalid data. Please try again later.");
        case PrinterNetworkErrorCode::SERVER_TOO_MANY_REQUESTS:
            return _u8L("Too many requests. Please try again later.");
        case PrinterNetworkErrorCode::SERVER_RTM_NOT_CONNECTED:
            return _u8L("RTM client not connected or not logged in. Please try again later.");
        case PrinterNetworkErrorCode::SERVER_UNAUTHORIZED:
            return _u8L("Unauthorized access, please check your account login status and try again.");
        case PrinterNetworkErrorCode::SERVER_FORBIDDEN:
            return _u8L("Forbidden access. Please try again later.");
        case PrinterNetworkErrorCode::SERVER_PIN_CODE_MISMATCH:
            return _u8L("Pairing failed. Please select the correct printer model.");
        case PrinterNetworkErrorCode::INSUFFICIENT_MEMORY:
            return _u8L("Insufficient memory. Please try again later.");
        case PrinterNetworkErrorCode::PRINTER_SERIAL_NUMBER_EMPTY:
            return _u8L("Printer serial number is empty. Please enter the serial number.");
        case PrinterNetworkErrorCode::PRINTER_NOT_CONNECTED_TO_UNBIND:
            return _u8L("Printer not connected to unbind.");
        case PrinterNetworkErrorCode::OPERATION_NOT_IMPLEMENTED:
            return _u8L("Operation not implemented. Please try again later.");
        case PrinterNetworkErrorCode::FILE_TOO_LARGE:
            return _u8L("The file is too large to upload. Please try another way to start the transfer or print.");
        case PrinterNetworkErrorCode::IPC_NOT_CONNECTED:
            return _u8L("Unable to connect to main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_CONNECTION_FAILED:
            return _u8L("Failed to connect to main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_CONNECTION_LOST:
            return _u8L("Connection to main process lost. Please try again.");
        case PrinterNetworkErrorCode::IPC_CONNECTION_CLOSED:
            return _u8L("Connection to main process closed. Please try again.");
        case PrinterNetworkErrorCode::IPC_IO_ERROR:
            return _u8L("Communication error with main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_SEND_FAILED:
            return _u8L("Failed to send request to main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_INVALID_MESSAGE:
            return _u8L("Invalid data received from main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_PROTOCOL_ERROR:
            return _u8L("Communication protocol error with main process. Please try again.");
        case PrinterNetworkErrorCode::IPC_TOO_MANY_PENDING:
            return _u8L("Too many requests to main process. Please try again later.");
        case PrinterNetworkErrorCode::IPC_DEADLOCK_PREVENTED:
            return _u8L("Internal communication error. Please try again.");
        default:
            return _u8L("An unknown error occurred. Please try again later.");
    }
}

} // namespace Slic3r 