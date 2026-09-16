#ifndef slic3r_PrinterNetworkInfo_hpp_
#define slic3r_PrinterNetworkInfo_hpp_


#include <string>
#include <functional>
#include "../nlohmann/json.hpp"
#include <vector>
#include <map>
#include "PrinterNetworkResult.hpp"

namespace Slic3r {

enum PrinterConnectStatus {
    PRINTER_CONNECT_STATUS_DISCONNECTED = 0,
    PRINTER_CONNECT_STATUS_CONNECTED    = 1,
};
enum PrinterStatus {
    PRINTER_STATUS_OFFLINE            = -1,
    PRINTER_STATUS_IDLE               = 0,
    PRINTER_STATUS_PRINTING           = 1,
    PRINTER_STATUS_PAUSED             = 2,
    PRINTER_STATUS_PAUSING            = 3,
    PRINTER_STATUS_CANCELED           = 4,
    PRINTER_STATUS_SELF_CHECKING      = 5,  // Self-checking
    PRINTER_STATUS_AUTO_LEVELING      = 6,  // Auto-leveling
    PRINTER_STATUS_PID_CALIBRATING    = 7,  // PID calibrating
    PRINTER_STATUS_RESONANCE_TESTING  = 8,  // Resonance testing
    PRINTER_STATUS_UPDATING           = 9,  // Updating
    PRINTER_STATUS_FILE_COPYING       = 10, // File copying
    PRINTER_STATUS_FILE_TRANSFERRING  = 11, // File transferring
    PRINTER_STATUS_HOMING             = 12, // Homing
    PRINTER_STATUS_PREHEATING         = 13, // Preheating
    PRINTER_STATUS_FILAMENT_OPERATING = 14, // Filament operating
    PRINTER_STATUS_EXTRUDER_OPERATING = 15, // Extruder operating
    PRINTER_STATUS_PRINT_COMPLETED    = 16, // Print completed
    PRINTER_STATUS_RFID_RECOGNIZING   = 17, // RFID recognizing
    PRINTER_STATUS_VIDEO_COMPOSING     = 18, // Video composing
    PRINTER_STATUS_EMERGENCY_STOP      = 19, // Emergency stop
    PRINTER_STATUS_POWER_LOSS_RECOVERY = 20, // Power loss recover
    PRINTER_STATUS_INITIALIZING        = 21, // Initializing
    PRINTER_STATUS_BUSY   = 998,  // Device busy
    PRINTER_STATUS_ERROR   = 999,  // Device exception status
    PRINTER_STATUS_ID_NOT_MATCH = 1000, // local mainboard or serial number not match with the remote printer
    PRINTER_STATUS_AUTH_ERROR = 1001, // local auth mode not match with the remote printer
    PRINTER_STATUS_UNKNOWN = 10000, // Unknown status
};

enum TrayStatus {
    TRAY_STATUS_UNKNOWN = -1,
    TRAY_STATUS_DISCONNECTED = 0, // disconnected
    TRAY_STATUS_PRELOADED = 1, // preloaded
    TRAY_STATUS_PRELOADED_UNKNOWN_FILAMENT = 2, // preloaded unknown filament info
    TRAY_STATUS_LOADED = 3, // loaded
    TRAY_STATUS_LOADED_UNKNOWN_FILAMENT = 4, // loaded unknown filament info
    TRAY_STATUS_ERROR = 999, // error
};

#define PRINTER_NETWORK_EXTRA_INFO_KEY_PORT "httpsCaFile"
#define PRINTER_NETWORK_EXTRA_INFO_KEY_IGNORE_CERT_REVOCATION "ignoreCertRevocation"

struct PrinterMmsTray
{
    std::string trayId;
    std::string mmsId;
    std::string trayName;
    std::string settingId;
    std::string filamentId;
    std::string from;
    std::string vendor;
    int         serialNumber;
    std::string filamentType;
    std::string filamentName;
    std::string filamentColor;
    std::string filamentDiameter;
    std::string filamentPresetName; // preset filament name
    std::string filamentPresetAlias; // preset alias (name before @), for cross-printer matching
    double      minNozzleTemp;
    double      maxNozzleTemp;
    double      minBedTemp;
    double      maxBedTemp;
    TrayStatus  status{TRAY_STATUS_UNKNOWN};
};

struct PrinterMms
{
    std::string                 mmsId;
    std::string                 mmsName;
    std::string                 mmsModel;
    double                      temperature;
    int                         humidity;
    bool                        connected;
    std::vector<PrinterMmsTray> trayList;
};

// Multi-Material System
struct PrinterMmsGroup
{
    int                     connectNum;
    bool                    connected;
    std::string             activeMmsId;
    std::string             activeTrayId;
    bool                    autoRefill;
    std::string             mmsSystemName;
    std::vector<PrinterMms> mmsList;
    PrinterMmsTray          vtTray;
};

struct PrintFilamentMmsMapping
{
    int index;
    std::string filamentId;
    std::string settingId;
    std::string vendor;
    std::string filamentName;
    std::string filamentAlias;
    std::string filamentColor;
    std::string filamentType;
    float       filamentWeight;
    float       filamentDensity;
    //print mapping mms filament
    PrinterMmsTray mappedMmsFilament;
};

struct PrintCapabilities
{
    bool supportsAutoBedLeveling = false;    // Supports auto bed leveling
    bool supportsTimeLapse = false;          // Supports time-lapse printing
    bool supportsHeatedBedSwitching = false; // Supports heated bed switching
    bool supportsFilamentMapping = false;    // Supports filament mapping
    bool supportsAutoRefill = false;         // Supports auto refill
};

struct SystemCapabilities
{
    bool canSetPrinterName = false;     // Supports setting machine name
    bool canGetDiskInfo = false;        // Supports getting disk information
    bool supportsMultiFilament = false; // Supports multi-filament printing
};

enum PrinterAuthMode {
    PRINTER_AUTH_MODE_NONE = 0,
    PRINTER_AUTH_MODE_PASSWORD = 1,
    PRINTER_AUTH_MODE_ACCESS_CODE = 2,
    PRINTER_AUTH_MODE_PIN_CODE = 3,
};

enum NetworkType {
    NETWORK_TYPE_LAN = 0,
    NETWORK_TYPE_WAN = 1,
};

enum class UploadTaskStatus {
    UPLOADING = 0,      // Uploading file
    SUCCESS = 1,        // Upload and send print both succeeded (if uploadAndStartPrint is true)
    FAILED = -1,        // Upload or send print failed
    CANCELLED = -2      // Task cancelled
};

struct UploadTaskInfo
{
    std::string taskId;
    std::string printerId;
    std::string fileName;
    uint64_t    uploadedBytes{0};
    uint64_t    totalBytes{0};
    int         progress{0};
    UploadTaskStatus status{UploadTaskStatus::UPLOADING};
    PrinterNetworkErrorCode code{PrinterNetworkErrorCode::SUCCESS};
    std::string message;
    int64_t     beginTime{0};
    int64_t     endTime{0};
}; 

struct PrinterPrintFile
{
    std::string fileName;                       // File name
    int64_t     printTime{0};                   // Print time (seconds)
    int         layer{0};                       // Total layers
    double      layerHeight{0};                 // Layer height (millimeters)
    std::string thumbnail;                      // Thumbnail URL
    int64_t     size{0};                        // File size (bytes)
    int64_t     createTime{0};                  // File creation time (timestamp/seconds)
    double      totalFilamentUsed{0.0f};        // Estimated total filament consumption weight (grams)
    double      totalFilamentUsedLength{0.0f};  // Estimated total filament consumption length (millimeters)
    int         totalPrintTimes{0};             // Total print times
    int64_t     lastPrintTime{0};               // Last print time (timestamp/seconds)
    std::vector<PrintFilamentMmsMapping> filamentMmsMappingList; // Filament color mapping for multi-color printing
};
struct PrinterPrintTask
{
    std::string taskId;
    std::string thumbnail;     // Thumbnail URL
    std::string taskName;      // Task name
    std::string fileName;
    int64_t     totalTime{0};    // Total time (seconds)
    int64_t     currentTime{0};  // Current time (seconds)
    int64_t     estimatedTime{0};// Estimated time (seconds)
    int64_t     beginTime{0};    // Start time (timestamp/seconds)
    int64_t     endTime{0};      // End time (timestamp/seconds)
    int         progress{0};     // Progress (0-100)
    int         taskStatus{0};   // Task status
};

struct PrinterPrintTaskResponse
{
    int totalTasks{0};
    std::vector<PrinterPrintTask> taskList;
};

struct PrinterPrintFileResponse
{
    int totalFiles{0};
    std::vector<PrinterPrintFile> fileList;
};

struct PrinterExceptionDetail
{
    std::string id;
    std::string refId;
    std::string code;
    int         level{0};
    int64_t     time{0};
};

struct PrinterExceptionResponse
{
    int total{0};
    std::vector<PrinterExceptionDetail> exceptionList;
};


struct PrinterNetworkInfo
{
    std::string printerName;
    std::string printerId;
    std::string host;
    int         port{0};
    std::string vendor;
    std::string printerModel;
    std::string protocolVersion;
    std::string firmwareVersion;
    std::string hostType;
    std::string mainboardId;
    std::string serialNumber;
    std::string username;
    std::string password;
    PrinterAuthMode authMode{PRINTER_AUTH_MODE_NONE};
    std::string token;
    std::string accessCode;
    std::string pinCode;
    std::string webUrl;
    NetworkType networkType{NETWORK_TYPE_LAN};
    bool        isPhysicalPrinter{false};
    uint64_t    addTime{0};
    uint64_t    modifyTime{0};
    uint64_t    lastActiveTime{0};
    PrintCapabilities  printCapabilities;
    SystemCapabilities   systemCapabilities;
    std::string extraInfo{"{}"}; // json string
    // not save to file
    PrinterPrintTask     printTask;
    PrinterConnectStatus connectStatus{PRINTER_CONNECT_STATUS_DISCONNECTED};
    PrinterStatus        printerStatus{PRINTER_STATUS_IDLE};
    std::vector<PrinterExceptionDetail> exceptions;
    int                  deviceAssistantStatus{0};
    bool                 isAdded{false}; //only used for frontend to show the printer is added or not when discover printers
};

enum LoginStatus {
    LOGIN_STATUS_NO_USER                              = -1, // No user
    LOGIN_STATUS_NOT_LOGIN                            = 0,  // Not login
    LOGIN_STATUS_LOGIN_SUCCESS                        = 1,  // Login success and online
    LOGIN_STATUS_OFFLINE                              = 2,  // Network error, user is offline , don't need to re-login and refresh token
    LOGIN_STATUS_OFFLINE_INVALID_TOKEN                = 3,  // Invalid token and need to re-login
    LOGIN_STATUS_OFFLINE_INVALID_USER                 = 4,  // Invalid user  and need to re-login
    LOGIN_STATUS_OFFLINE_TOKEN_EXPIRED                = 5,  // Token expired and need to refresh
    LOGIN_STATUS_OTHER_NETWORK_ERROR                  = 100, // Other network error, user is online but network error, don't need to re-login and refresh token, don't need to update user info and notify frontend
};

struct UserNetworkInfo
{
    std::string userId;
    std::string username;
    std::string token;
    std::string refreshToken;
    std::string hostType;
    uint64_t    accessTokenExpireTime{0};
    uint64_t    refreshTokenExpireTime{0};
    std::string rtcToken; // video rtc token
    uint64_t    rtcTokenExpireTime{0};
    LoginStatus loginStatus{LOGIN_STATUS_NO_USER};
    std::string nickname;     // User display name
    std::string email;        // User email
    std::string avatar;       // User avatar URL
    std::string openid;       // OpenID for third-party login
    std::string phone;        // User phone number
    std::string region;      // User region
    std::string language;     // User preferred language
    uint64_t    createTime{0}; // Account creation time
    uint64_t    loginTime{0}; //  login time
    uint64_t    lastTokenRefreshTime{0}; // Last token refresh time
    std::string extraInfo{"{}"}; // json string
    std::string loginErrorMessage; // login error message

};

struct PluginNetworkInfo
{
    std::string pluginPath;
    std::string hostType;
    std::string version;
    std::string latestVersion;
    std::string downloadUrl;
    bool        installed{false};
};



struct LicenseExpiredDevice
{
    std::string serialNumber; // Device serial number
    int status = 0;           // License status: 1=VALID, 2=EXPIRED, 3=need device confirm, 9=not found
};

struct PrinterNetworkParams
{
    std::string printerId;
    std::string filePath;
    std::string fileName;
    int         bedType{0};
    bool        timeLapse{false};
    bool        heatedBedLeveling{false};
    bool        autoRefill{false};
    bool        uploadAndStartPrint{false};
    bool        hasMms{false};
    std::vector<PrintFilamentMmsMapping> filamentMmsMappingList;

    std::function<void(const uint64_t uploadedBytes, const uint64_t totalBytes, bool& cancel)> uploadProgressFn;
    std::function<void(const std::string& errorMsg)>                                           errorFn;
};

using PrinterConnectStatusFn = std::function<void(const std::string& printerId, const PrinterConnectStatus& status)>;
using PrinterStatusFn        = std::function<void(const std::string& printerId, const PrinterStatus& status)>;
using PrinterPrintTaskFn     = std::function<void(const std::string& printerId, const PrinterPrintTask& printTask)>;
using PrinterAttributesFn    = std::function<void(const std::string& printerId, const PrinterNetworkInfo& printerInfo)>;

PrinterNetworkInfo convertJsonToPrinterNetworkInfo(const nlohmann::json& json);
nlohmann::json convertPrinterNetworkInfoToJson(const PrinterNetworkInfo& printerNetworkInfo);
nlohmann::json convertPrinterMmsTrayToJson(const PrinterMmsTray& tray);
PrinterMmsTray convertJsonToPrinterMmsTray(const nlohmann::json& json);
nlohmann::json convertPrinterMmsToJson(const PrinterMms& mms);
PrinterMms convertJsonToPrinterMms(const nlohmann::json& json);
nlohmann::json convertPrinterMmsGroupToJson(const PrinterMmsGroup& mmsGroup);
PrinterMmsGroup convertJsonToPrinterMmsGroup(const nlohmann::json& json);
nlohmann::json convertPrintFilamentMmsMappingToJson(const PrintFilamentMmsMapping& printFilamentMmsMapping);
PrintFilamentMmsMapping convertJsonToPrintFilamentMmsMapping(const nlohmann::json& json);
nlohmann::json convertUserNetworkInfoToJson(const UserNetworkInfo& userNetworkInfo);
UserNetworkInfo convertJsonToUserNetworkInfo(const nlohmann::json& json);

nlohmann::json convertPrinterPrintTaskToJson(const PrinterPrintTask& task);
PrinterPrintTask convertJsonToPrinterPrintTask(const nlohmann::json& json);
nlohmann::json convertPrinterPrintTaskResponseToJson(const PrinterPrintTaskResponse& response);
PrinterPrintTaskResponse convertJsonToPrinterPrintTaskResponse(const nlohmann::json& json);

nlohmann::json convertPrinterExceptionDetailToJson(const PrinterExceptionDetail& detail);
PrinterExceptionDetail convertJsonToPrinterExceptionDetail(const nlohmann::json& json);
nlohmann::json convertPrinterExceptionResponseToJson(const PrinterExceptionResponse& response);
PrinterExceptionResponse convertJsonToPrinterExceptionResponse(const nlohmann::json& json);

nlohmann::json convertUploadTaskInfoToJson(const UploadTaskInfo& task);
UploadTaskInfo convertJsonToUploadTaskInfo(const nlohmann::json& json);

nlohmann::json convertPrinterPrintFileToJson(const PrinterPrintFile& file);
PrinterPrintFile convertJsonToPrinterPrintFile(const nlohmann::json& json);
nlohmann::json convertPrinterPrintFileResponseToJson(const PrinterPrintFileResponse& response);
PrinterPrintFileResponse convertJsonToPrinterPrintFileResponse(const nlohmann::json& json);

nlohmann::json convertPrinterNetworkParamsToJson(const PrinterNetworkParams& params);
PrinterNetworkParams convertJsonToPrinterNetworkParams(const nlohmann::json& json);

LoginStatus parseLoginStatusByErrorCode(PrinterNetworkErrorCode resultCode);

} // namespace Slic3r

#endif
