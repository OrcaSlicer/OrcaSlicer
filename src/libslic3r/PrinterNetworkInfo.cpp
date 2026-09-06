#include <string>
#include <vector>
#include "PrinterNetworkInfo.hpp"
#include "slic3r/Utils/JsonUtils.hpp"

namespace Slic3r {

PrinterNetworkInfo convertJsonToPrinterNetworkInfo(const nlohmann::json& json)
{
    PrinterNetworkInfo printerNetworkInfo;
    try {
        printerNetworkInfo.printerId = JsonUtils::safeGetString(json, "printerId", "");
        printerNetworkInfo.printerName = JsonUtils::safeGetString(json, "printerName", "");
        printerNetworkInfo.host = JsonUtils::safeGetString(json, "host", "");
        printerNetworkInfo.port = JsonUtils::safeGetInt(json, "port", 0);
        printerNetworkInfo.vendor = JsonUtils::safeGetString(json, "vendor", "");
        printerNetworkInfo.printerModel = JsonUtils::safeGetString(json, "printerModel", "");
        printerNetworkInfo.protocolVersion = JsonUtils::safeGetString(json, "protocolVersion", "");
        printerNetworkInfo.firmwareVersion = JsonUtils::safeGetString(json, "firmwareVersion", "");
        printerNetworkInfo.hostType = JsonUtils::safeGetString(json, "hostType", "");
        printerNetworkInfo.mainboardId = JsonUtils::safeGetString(json, "mainboardId", "");
        printerNetworkInfo.serialNumber = JsonUtils::safeGetString(json, "serialNumber", "");
        printerNetworkInfo.username = JsonUtils::safeGetString(json, "username", "");
        printerNetworkInfo.password = JsonUtils::safeGetString(json, "password", "");
        printerNetworkInfo.authMode = static_cast<PrinterAuthMode>(JsonUtils::safeGetInt(json, "authMode", 0));
        printerNetworkInfo.token = JsonUtils::safeGetString(json, "token", "");
        printerNetworkInfo.accessCode = JsonUtils::safeGetString(json, "accessCode", "");
        printerNetworkInfo.pinCode = JsonUtils::safeGetString(json, "pinCode", "");
        printerNetworkInfo.webUrl = JsonUtils::safeGetString(json, "webUrl", "");
        printerNetworkInfo.networkType = static_cast<NetworkType>(JsonUtils::safeGetInt(json, "networkType", 0));
        printerNetworkInfo.isPhysicalPrinter = JsonUtils::safeGetBool(json, "isPhysicalPrinter", false);
        printerNetworkInfo.addTime = JsonUtils::safeGet<uint64_t>(json, "addTime", 0);
        printerNetworkInfo.modifyTime = JsonUtils::safeGet<uint64_t>(json, "modifyTime", 0);
        printerNetworkInfo.lastActiveTime = JsonUtils::safeGet<uint64_t>(json, "lastActiveTime", 0);
        printerNetworkInfo.connectStatus = static_cast<PrinterConnectStatus>(JsonUtils::safeGetInt(json, "connectStatus", 0));
        printerNetworkInfo.printerStatus = static_cast<PrinterStatus>(JsonUtils::safeGetInt(json, "printerStatus", 0));
        if (json.contains("exceptions") && json["exceptions"].is_array()) {
            for (const auto& exceptionJson : json["exceptions"]) {
                printerNetworkInfo.exceptions.push_back(convertJsonToPrinterExceptionDetail(exceptionJson));
            }
        }
        printerNetworkInfo.deviceAssistantStatus = JsonUtils::safeGetInt(json, "deviceAssistantStatus", 0);
        printerNetworkInfo.isAdded = JsonUtils::safeGetBool(json, "isAdded", false);
        
        if (json.contains("extraInfo")) {
            printerNetworkInfo.extraInfo = json["extraInfo"].dump();
        } else {
            printerNetworkInfo.extraInfo = "{}";
        }
        
        if (json.contains("printTask")) {
            auto& printTask = json["printTask"];
            printerNetworkInfo.printTask.taskId = JsonUtils::safeGetString(printTask, "taskId", "");
            printerNetworkInfo.printTask.fileName = JsonUtils::safeGetString(printTask, "fileName", "");
            printerNetworkInfo.printTask.totalTime = JsonUtils::safeGetInt64(printTask, "totalTime", 0);
            printerNetworkInfo.printTask.currentTime = JsonUtils::safeGetInt64(printTask, "currentTime", 0);
            printerNetworkInfo.printTask.estimatedTime = JsonUtils::safeGetInt64(printTask, "estimatedTime", 0);
            printerNetworkInfo.printTask.progress = JsonUtils::safeGetInt(printTask, "progress", 0);
        }
        
        if (json.contains("printCapabilities")) {
            if (json["printCapabilities"].contains("supportsAutoBedLeveling")) {
                printerNetworkInfo.printCapabilities.supportsAutoBedLeveling = json["printCapabilities"]["supportsAutoBedLeveling"]
                                                                                   .get<bool>();
            }
            if (json["printCapabilities"].contains("supportsTimeLapse")) {
                printerNetworkInfo.printCapabilities.supportsTimeLapse = json["printCapabilities"]["supportsTimeLapse"].get<bool>();
            }
            if (json["printCapabilities"].contains("supportsHeatedBedSwitching")) {
                printerNetworkInfo.printCapabilities.supportsHeatedBedSwitching = json["printCapabilities"]["supportsHeatedBedSwitching"]
                                                                                      .get<bool>();
            }
            if (json["printCapabilities"].contains("supportsFilamentMapping")) {
                printerNetworkInfo.printCapabilities.supportsFilamentMapping = json["printCapabilities"]["supportsFilamentMapping"]
                                                                                   .get<bool>();
            }
            if (json["printCapabilities"].contains("supportsAutoRefill")) {
                printerNetworkInfo.printCapabilities.supportsAutoRefill = json["printCapabilities"]["supportsAutoRefill"].get<bool>();
            }
        }
        if (json.contains("systemCapabilities")) {
            auto& sysCaps = json["systemCapabilities"];
            printerNetworkInfo.systemCapabilities.supportsMultiFilament = JsonUtils::safeGetBool(sysCaps, "supportsMultiFilament", false);
            printerNetworkInfo.systemCapabilities.canGetDiskInfo = JsonUtils::safeGetBool(sysCaps, "canGetDiskInfo", false);
            printerNetworkInfo.systemCapabilities.canSetPrinterName = JsonUtils::safeGetBool(sysCaps, "canSetPrinterName", false);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to convert json to printer network info: " + std::string(e.what()));
    }
    return printerNetworkInfo;
}
nlohmann::json convertPrinterNetworkInfoToJson(const PrinterNetworkInfo& printerNetworkInfo)
{
    nlohmann::json json;
    json["printerId"]       = printerNetworkInfo.printerId;
    json["printerName"]     = printerNetworkInfo.printerName;
    json["host"]            = printerNetworkInfo.host;
    json["port"]            = printerNetworkInfo.port;
    json["vendor"]          = printerNetworkInfo.vendor;
    json["printerModel"]    = printerNetworkInfo.printerModel;
    json["protocolVersion"] = printerNetworkInfo.protocolVersion;
    json["firmwareVersion"] = printerNetworkInfo.firmwareVersion;
    json["hostType"]        = printerNetworkInfo.hostType;
    json["mainboardId"]     = printerNetworkInfo.mainboardId;
    json["serialNumber"]    = printerNetworkInfo.serialNumber;
    json["username"]        = printerNetworkInfo.username;
    json["password"]        = printerNetworkInfo.password;
    json["authMode"]        = printerNetworkInfo.authMode;
    json["token"]           = printerNetworkInfo.token;
    json["accessCode"]      = printerNetworkInfo.accessCode;
    json["pinCode"]         = printerNetworkInfo.pinCode;
    json["webUrl"]          = printerNetworkInfo.webUrl;
    json["networkType"]     = printerNetworkInfo.networkType;
    if (!printerNetworkInfo.extraInfo.empty()) {
        json["extraInfo"] = nlohmann::json::parse(printerNetworkInfo.extraInfo);
    } else {
        json["extraInfo"] = nlohmann::json::object();
    }
    json["isPhysicalPrinter"] = printerNetworkInfo.isPhysicalPrinter;
    json["addTime"]           = printerNetworkInfo.addTime;
    json["modifyTime"]        = printerNetworkInfo.modifyTime;
    json["lastActiveTime"]    = printerNetworkInfo.lastActiveTime;
    json["connectStatus"]     = printerNetworkInfo.connectStatus;
    json["printerStatus"]     = printerNetworkInfo.printerStatus;
    nlohmann::json exceptionsJson = nlohmann::json::array();
    for (const auto& exception : printerNetworkInfo.exceptions) {
        exceptionsJson.push_back(convertPrinterExceptionDetailToJson(exception));
    }
    json["exceptions"]        = exceptionsJson;
    json["deviceAssistantStatus"] = printerNetworkInfo.deviceAssistantStatus;
    nlohmann::json printTaskJson;
    printTaskJson["taskId"]        = printerNetworkInfo.printTask.taskId;
    printTaskJson["fileName"]      = printerNetworkInfo.printTask.fileName;
    printTaskJson["totalTime"]     = printerNetworkInfo.printTask.totalTime;
    printTaskJson["currentTime"]   = printerNetworkInfo.printTask.currentTime;
    printTaskJson["estimatedTime"] = printerNetworkInfo.printTask.estimatedTime;
    printTaskJson["progress"]      = printerNetworkInfo.printTask.progress;
    json["printTask"]              = printTaskJson;
    nlohmann::json printCapabilitiesJson;
    printCapabilitiesJson["supportsAutoBedLeveling"]    = printerNetworkInfo.printCapabilities.supportsAutoBedLeveling;
    printCapabilitiesJson["supportsTimeLapse"]          = printerNetworkInfo.printCapabilities.supportsTimeLapse;
    printCapabilitiesJson["supportsHeatedBedSwitching"] = printerNetworkInfo.printCapabilities.supportsHeatedBedSwitching;
    printCapabilitiesJson["supportsFilamentMapping"]    = printerNetworkInfo.printCapabilities.supportsFilamentMapping;
    printCapabilitiesJson["supportsAutoRefill"]         = printerNetworkInfo.printCapabilities.supportsAutoRefill;
    json["printCapabilities"]                           = printCapabilitiesJson;
    nlohmann::json systemCapabilitiesJson;
    systemCapabilitiesJson["supportsMultiFilament"] = printerNetworkInfo.systemCapabilities.supportsMultiFilament;
    systemCapabilitiesJson["canGetDiskInfo"]        = printerNetworkInfo.systemCapabilities.canGetDiskInfo;
    systemCapabilitiesJson["canSetPrinterName"]     = printerNetworkInfo.systemCapabilities.canSetPrinterName;
    json["systemCapabilities"]                      = systemCapabilitiesJson;
    json["isAdded"]                                 = printerNetworkInfo.isAdded;
    return json;
}

PrinterMms convertJsonToPrinterMms(const nlohmann::json& json)
{
    PrinterMms mms;
    try {
        mms.mmsId = JsonUtils::safeGetString(json, "mmsId", "");
        mms.temperature = JsonUtils::safeGetDouble(json, "temperature", 0.0);
        mms.humidity = JsonUtils::safeGetInt(json, "humidity", 0);
        mms.connected = JsonUtils::safeGetBool(json, "connected", false);
        if (json.contains("trayList") && json["trayList"].is_array()) {
            for (auto& tray : json["trayList"]) {
                mms.trayList.push_back(convertJsonToPrinterMmsTray(tray));
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to convert json to printer mms: " + std::string(e.what()));
    }
    return mms;
}

nlohmann::json convertPrinterMmsToJson(const PrinterMms& mms)
{
    nlohmann::json json     = nlohmann::json::object();
    json["mmsId"]           = mms.mmsId;
    json["temperature"]     = mms.temperature;
    json["humidity"]        = mms.humidity;
    json["connected"]       = mms.connected;
    nlohmann::json trayList = nlohmann::json::array();
    for (auto& tray : mms.trayList) {
        nlohmann::json trayJson = convertPrinterMmsTrayToJson(tray);
        trayList.push_back(trayJson);
    }
    json["trayList"] = trayList;
    return json;
}

nlohmann::json convertPrinterMmsTrayToJson(const PrinterMmsTray& tray)
{
    nlohmann::json json        = nlohmann::json::object();
    json["trayId"]             = tray.trayId;
    json["mmsId"]              = tray.mmsId;
    json["trayName"]           = tray.trayName;
    json["settingId"]          = tray.settingId;
    json["filamentId"]         = tray.filamentId;
    json["from"]               = tray.from;
    json["vendor"]             = tray.vendor;
    json["serialNumber"]       = tray.serialNumber;
    json["filamentType"]       = tray.filamentType;
    json["filamentName"]       = tray.filamentName;
    json["filamentColor"]      = tray.filamentColor;
    json["filamentDiameter"]   = tray.filamentDiameter;
    json["filamentPresetName"] = tray.filamentPresetName;
    json["filamentPresetAlias"] = tray.filamentPresetAlias;
    json["minNozzleTemp"]      = tray.minNozzleTemp;
    json["maxNozzleTemp"]      = tray.maxNozzleTemp;
    json["minBedTemp"]         = tray.minBedTemp;
    json["maxBedTemp"]         = tray.maxBedTemp;
    json["status"]             = tray.status;
    return json;
}

PrinterMmsTray convertJsonToPrinterMmsTray(const nlohmann::json& json)
{
    PrinterMmsTray tray;
    try {
        tray.trayId = JsonUtils::safeGetString(json, "trayId", "");
        tray.mmsId = JsonUtils::safeGetString(json, "mmsId", "");
        tray.trayName = JsonUtils::safeGetString(json, "trayName", "");
        tray.settingId = JsonUtils::safeGetString(json, "settingId", "");
        tray.filamentId = JsonUtils::safeGetString(json, "filamentId", "");
        tray.from = JsonUtils::safeGetString(json, "from", "");
        tray.vendor = JsonUtils::safeGetString(json, "vendor", "");
        tray.serialNumber = JsonUtils::safeGetInt(json, "serialNumber", 0);
        tray.filamentType = JsonUtils::safeGetString(json, "filamentType", "");
        tray.filamentName = JsonUtils::safeGetString(json, "filamentName", "");
        tray.filamentColor = JsonUtils::safeGetString(json, "filamentColor", "");
        tray.filamentDiameter = JsonUtils::safeGetString(json, "filamentDiameter", "");
        tray.filamentPresetName = JsonUtils::safeGetString(json, "filamentPresetName", "");
        tray.filamentPresetAlias = JsonUtils::safeGetString(json, "filamentPresetAlias", "");
        tray.minNozzleTemp = JsonUtils::safeGetDouble(json, "minNozzleTemp", 0.0);
        tray.maxNozzleTemp = JsonUtils::safeGetDouble(json, "maxNozzleTemp", 0.0);
        tray.minBedTemp = JsonUtils::safeGetDouble(json, "minBedTemp", 0.0);
        tray.maxBedTemp = JsonUtils::safeGetDouble(json, "maxBedTemp", 0.0);
        tray.status = static_cast<TrayStatus>(JsonUtils::safeGetInt(json, "status", -1));
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to convert json to printer mms tray: " + std::string(e.what()));
    }
    return tray;
}

nlohmann::json convertPrinterMmsGroupToJson(const PrinterMmsGroup& mmsGroup)
{
    nlohmann::json json    = nlohmann::json::object();
    json["connectNum"]     = mmsGroup.connectNum;
    json["connected"]      = mmsGroup.connected;
    json["activeMmsId"]    = mmsGroup.activeMmsId;
    json["activeTrayId"]   = mmsGroup.activeTrayId;
    json["autoRefill"]     = mmsGroup.autoRefill;
    json["mmsSystemName"]  = mmsGroup.mmsSystemName;
    json["vtTray"]         = convertPrinterMmsTrayToJson(mmsGroup.vtTray);
    nlohmann::json mmsList = nlohmann::json::array();
    for (auto& mms : mmsGroup.mmsList) {
        nlohmann::json mmsJson = convertPrinterMmsToJson(mms);
        mmsList.push_back(mmsJson);
    }
    json["mmsList"] = mmsList;
    return json;
}

PrinterMmsGroup convertJsonToPrinterMmsGroup(const nlohmann::json& json)
{
    PrinterMmsGroup mmsGroup;
    try {
        mmsGroup.connectNum = JsonUtils::safeGetInt(json, "connectNum", 0);
        mmsGroup.connected = JsonUtils::safeGetBool(json, "connected", false);
        mmsGroup.activeMmsId = JsonUtils::safeGetString(json, "activeMmsId", "");
        mmsGroup.activeTrayId = JsonUtils::safeGetString(json, "activeTrayId", "");
        mmsGroup.autoRefill = JsonUtils::safeGetBool(json, "autoRefill", false);
        mmsGroup.mmsSystemName = JsonUtils::safeGetString(json, "mmsSystemName", "");
        if (json.contains("vtTray")) {
            mmsGroup.vtTray = convertJsonToPrinterMmsTray(json["vtTray"]);
        }
        if (json.contains("mmsList") && json["mmsList"].is_array()) {
            for (auto& mms : json["mmsList"]) {
                mmsGroup.mmsList.push_back(convertJsonToPrinterMms(mms));
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to convert json to printer mms group: " + std::string(e.what()));
    }
    return mmsGroup;
}

nlohmann::json convertPrintFilamentMmsMappingToJson(const PrintFilamentMmsMapping& printFilamentMmsMapping)
{
    nlohmann::json json       = nlohmann::json::object();
    json["filamentId"]        = printFilamentMmsMapping.filamentId;
    json["vendor"]            = printFilamentMmsMapping.vendor;
    json["filamentName"]      = printFilamentMmsMapping.filamentName;
    json["filamentAlias"]     = printFilamentMmsMapping.filamentAlias;
    json["filamentColor"]     = printFilamentMmsMapping.filamentColor;
    json["filamentType"]      = printFilamentMmsMapping.filamentType;
    json["filamentWeight"]    = printFilamentMmsMapping.filamentWeight;
    json["filamentDensity"]   = printFilamentMmsMapping.filamentDensity;
    json["index"]             = printFilamentMmsMapping.index;
    json["mappedMmsFilament"] = convertPrinterMmsTrayToJson(printFilamentMmsMapping.mappedMmsFilament);
    return json;
}

PrintFilamentMmsMapping convertJsonToPrintFilamentMmsMapping(const nlohmann::json& json)
{
    PrintFilamentMmsMapping printFilamentMmsMapping;
    try {
        printFilamentMmsMapping.filamentId = JsonUtils::safeGetString(json, "filamentId", "");
        printFilamentMmsMapping.vendor = JsonUtils::safeGetString(json, "vendor", "");
        printFilamentMmsMapping.filamentName = JsonUtils::safeGetString(json, "filamentName", "");
        printFilamentMmsMapping.filamentAlias = JsonUtils::safeGetString(json, "filamentAlias", "");
        printFilamentMmsMapping.filamentColor = JsonUtils::safeGetString(json, "filamentColor", "");
        printFilamentMmsMapping.filamentType = JsonUtils::safeGetString(json, "filamentType", "");
        printFilamentMmsMapping.filamentWeight = JsonUtils::safeGetDouble(json, "filamentWeight", 0.0);
        printFilamentMmsMapping.filamentDensity = JsonUtils::safeGetDouble(json, "filamentDensity", 0.0);
        printFilamentMmsMapping.index = JsonUtils::safeGetInt(json, "index", 0);
        if (json.contains("mappedMmsFilament")) {
            printFilamentMmsMapping.mappedMmsFilament = convertJsonToPrinterMmsTray(json["mappedMmsFilament"]);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to convert json to print filament mms mapping: " + std::string(e.what()));
    }
    return printFilamentMmsMapping;
}

nlohmann::json convertUserNetworkInfoToJson(const UserNetworkInfo& userNetworkInfo)
{
    nlohmann::json json            = nlohmann::json::object();
    json["userId"]                 = userNetworkInfo.userId;
    json["username"]               = userNetworkInfo.username;
    json["token"]                  = userNetworkInfo.token;
    json["refreshToken"]           = userNetworkInfo.refreshToken;
    json["hostType"]               = userNetworkInfo.hostType;
    json["accessTokenExpireTime"]  = userNetworkInfo.accessTokenExpireTime;
    json["refreshTokenExpireTime"] = userNetworkInfo.refreshTokenExpireTime;
    json["rtcToken"]               = userNetworkInfo.rtcToken;
    json["rtcTokenExpireTime"]     = userNetworkInfo.rtcTokenExpireTime;
    json["nickname"]               = userNetworkInfo.nickname;
    json["email"]                  = userNetworkInfo.email;
    json["avatar"]                 = userNetworkInfo.avatar;
    json["openid"]                 = userNetworkInfo.openid;
    json["phone"]                  = userNetworkInfo.phone;
    json["region"]                = userNetworkInfo.region;
    json["language"]               = userNetworkInfo.language;
    json["createTime"]             = userNetworkInfo.createTime;
    json["loginTime"]              = userNetworkInfo.loginTime;
    json["loginStatus"]            = userNetworkInfo.loginStatus;
    json["lastTokenRefreshTime"]   = userNetworkInfo.lastTokenRefreshTime;
    json["extraInfo"]              = userNetworkInfo.extraInfo;
    json["loginErrorMessage"]      = userNetworkInfo.loginErrorMessage;
    return json;
}
UserNetworkInfo convertJsonToUserNetworkInfo(const nlohmann::json& json)
{
    UserNetworkInfo userNetworkInfo;
    userNetworkInfo.userId = JsonUtils::safeGetString(json, "userId", "");
    userNetworkInfo.username = JsonUtils::safeGetString(json, "username", "");
    userNetworkInfo.token = JsonUtils::safeGetString(json, "token", "");
    userNetworkInfo.refreshToken = JsonUtils::safeGetString(json, "refreshToken", "");
    userNetworkInfo.hostType = JsonUtils::safeGetString(json, "hostType", "");
    userNetworkInfo.accessTokenExpireTime = JsonUtils::safeGetInt64(json, "accessTokenExpireTime", 0);
    userNetworkInfo.refreshTokenExpireTime = JsonUtils::safeGetInt64(json, "refreshTokenExpireTime", 0);
    userNetworkInfo.rtcToken = JsonUtils::safeGetString(json, "rtcToken", "");
    userNetworkInfo.rtcTokenExpireTime = JsonUtils::safeGetInt64(json, "rtcTokenExpireTime", 0);
    userNetworkInfo.nickname = JsonUtils::safeGetString(json, "nickname", "");
    userNetworkInfo.email = JsonUtils::safeGetString(json, "email", "");
    userNetworkInfo.avatar = JsonUtils::safeGetString(json, "avatar", "");
    userNetworkInfo.openid = JsonUtils::safeGetString(json, "openid", "");
    userNetworkInfo.phone = JsonUtils::safeGetString(json, "phone", "");
    userNetworkInfo.region = JsonUtils::safeGetString(json, "region", "");
    userNetworkInfo.language = JsonUtils::safeGetString(json, "language", "");
    userNetworkInfo.createTime = JsonUtils::safeGetInt64(json, "createTime", 0);
    userNetworkInfo.loginTime = JsonUtils::safeGetInt64(json, "loginTime", 0);
    userNetworkInfo.loginStatus = static_cast<LoginStatus>(JsonUtils::safeGetInt(json, "loginStatus", 0));
    userNetworkInfo.lastTokenRefreshTime = JsonUtils::safeGetInt64(json, "lastTokenRefreshTime", 0);
    userNetworkInfo.extraInfo = JsonUtils::safeGetString(json, "extraInfo", "");
    userNetworkInfo.loginErrorMessage = JsonUtils::safeGetString(json, "loginErrorMessage", "");
    return userNetworkInfo;
}

LoginStatus parseLoginStatusByErrorCode(PrinterNetworkErrorCode resultCode)
{
    switch (resultCode) {
    case PrinterNetworkErrorCode::SUCCESS:
        return LOGIN_STATUS_LOGIN_SUCCESS;   
    case PrinterNetworkErrorCode::SERVER_UNAUTHORIZED:
        // token expired, need to refresh, if refresh failed, need to re-login
        return LOGIN_STATUS_OFFLINE_TOKEN_EXPIRED;
    case PrinterNetworkErrorCode::NETWORK_ERROR:
         return LOGIN_STATUS_OFFLINE;
    default:
        return LOGIN_STATUS_OTHER_NETWORK_ERROR;
    }
}

nlohmann::json convertPrinterPrintTaskToJson(const PrinterPrintTask& task)
{
    nlohmann::json json = nlohmann::json::object();
    json["taskId"] = task.taskId;
    json["thumbnail"] = task.thumbnail;
    json["taskName"] = task.taskName;
    json["fileName"] = task.fileName;
    json["totalTime"] = task.totalTime;
    json["currentTime"] = task.currentTime;
    json["estimatedTime"] = task.estimatedTime;
    json["beginTime"] = task.beginTime;
    json["endTime"] = task.endTime;
    json["progress"] = task.progress;
    json["taskStatus"] = task.taskStatus;
    return json;
}

PrinterPrintTask convertJsonToPrinterPrintTask(const nlohmann::json& json)
{
    PrinterPrintTask task;
    task.taskId = JsonUtils::safeGetString(json, "taskId", "");
    task.thumbnail = JsonUtils::safeGetString(json, "thumbnail", "");
    task.taskName = JsonUtils::safeGetString(json, "taskName", "");
    task.fileName = JsonUtils::safeGetString(json, "fileName", "");
    task.totalTime = JsonUtils::safeGetInt64(json, "totalTime", 0);
    task.currentTime = JsonUtils::safeGetInt64(json, "currentTime", 0);
    task.estimatedTime = JsonUtils::safeGetInt64(json, "estimatedTime", 0);
    task.beginTime = JsonUtils::safeGetInt64(json, "beginTime", 0);
    task.endTime = JsonUtils::safeGetInt64(json, "endTime", 0);
    task.progress = JsonUtils::safeGetInt(json, "progress", 0);
    task.taskStatus = JsonUtils::safeGetInt(json, "taskStatus", 0);
    return task;
}

nlohmann::json convertPrinterPrintTaskResponseToJson(const PrinterPrintTaskResponse& response)
{
    nlohmann::json json = nlohmann::json::object();
    json["totalTasks"] = response.totalTasks;
    nlohmann::json taskList = nlohmann::json::array();
    for (const auto& task : response.taskList) {
        taskList.push_back(convertPrinterPrintTaskToJson(task));
    }
    json["taskList"] = taskList;
    return json;
}

PrinterPrintTaskResponse convertJsonToPrinterPrintTaskResponse(const nlohmann::json& json)
{
    PrinterPrintTaskResponse response;
    response.totalTasks = JsonUtils::safeGetInt(json, "totalTasks", 0);
    if (json.contains("taskList") && json["taskList"].is_array()) {
        for (const auto& taskJson : json["taskList"]) {
            response.taskList.push_back(convertJsonToPrinterPrintTask(taskJson));
        }
    }
    return response;
}

nlohmann::json convertPrinterExceptionDetailToJson(const PrinterExceptionDetail& detail)
{
    nlohmann::json json = nlohmann::json::object();
    json["id"]    = detail.id;
    json["refId"] = detail.refId;
    json["code"]  = detail.code;
    json["level"] = detail.level;
    json["time"]  = detail.time;
    return json;
}

PrinterExceptionDetail convertJsonToPrinterExceptionDetail(const nlohmann::json& json)
{
    PrinterExceptionDetail detail;
    detail.id    = JsonUtils::safeGetString(json, "id", "");
    detail.refId = JsonUtils::safeGetString(json, "refId", "");
    detail.code  = JsonUtils::safeGetString(json, "code", "");
    detail.level = JsonUtils::safeGetInt(json, "level", 0);
    detail.time  = JsonUtils::safeGetInt64(json, "time", 0);
    return detail;
}

nlohmann::json convertPrinterExceptionResponseToJson(const PrinterExceptionResponse& response)
{
    nlohmann::json json = nlohmann::json::object();
    json["total"]       = response.total;
    nlohmann::json exceptionList = nlohmann::json::array();
    for (const auto& detail : response.exceptionList) {
        exceptionList.push_back(convertPrinterExceptionDetailToJson(detail));
    }
    json["exceptionList"] = exceptionList;
    return json;
}

PrinterExceptionResponse convertJsonToPrinterExceptionResponse(const nlohmann::json& json)
{
    PrinterExceptionResponse response;
    response.total = JsonUtils::safeGetInt(json, "total", 0);
    if (json.contains("exceptionList") && json["exceptionList"].is_array()) {
        for (const auto& detailJson : json["exceptionList"]) {
            response.exceptionList.push_back(convertJsonToPrinterExceptionDetail(detailJson));
        }
    }
    return response;
}

nlohmann::json convertUploadTaskInfoToJson(const UploadTaskInfo& task)
{
    nlohmann::json json;
    json["taskId"] = task.taskId;
    json["printerId"] = task.printerId;
    json["fileName"] = task.fileName;
    json["uploadedBytes"] = task.uploadedBytes;
    json["totalBytes"] = task.totalBytes;
    json["progress"] = task.progress;
    json["status"] = static_cast<int>(task.status);
    json["code"] = static_cast<int>(task.code);
    json["message"] = task.message;
    json["beginTime"] = task.beginTime;
    json["endTime"] = task.endTime;
    return json;
}

UploadTaskInfo convertJsonToUploadTaskInfo(const nlohmann::json& json)
{
    UploadTaskInfo task;
    if (json.is_null()) return task;
    
    task.taskId = JsonUtils::safeGetString(json, "taskId", "");
    task.printerId = JsonUtils::safeGetString(json, "printerId", "");
    task.fileName = JsonUtils::safeGetString(json, "fileName", "");
    task.uploadedBytes = JsonUtils::safeGet<uint64_t>(json, "uploadedBytes", 0);
    task.totalBytes = JsonUtils::safeGet<uint64_t>(json, "totalBytes", 0);
    task.progress = JsonUtils::safeGetInt(json, "progress", 0);
    task.status = static_cast<UploadTaskStatus>(JsonUtils::safeGetInt(json, "status", 0));
    task.code = static_cast<PrinterNetworkErrorCode>(JsonUtils::safeGetInt(json, "code", 0));
    task.message = JsonUtils::safeGetString(json, "message", "");
    task.beginTime = JsonUtils::safeGetInt64(json, "beginTime", 0);
    task.endTime = JsonUtils::safeGetInt64(json, "endTime", 0);
    return task;
}

nlohmann::json convertPrinterPrintFileToJson(const PrinterPrintFile& file)
{
    nlohmann::json json = nlohmann::json::object();
    json["fileName"] = file.fileName;
    json["printTime"] = file.printTime;
    json["layer"] = file.layer;
    json["layerHeight"] = file.layerHeight;
    json["thumbnail"] = file.thumbnail;
    json["size"] = file.size;
    json["createTime"] = file.createTime;
    json["totalFilamentUsed"] = file.totalFilamentUsed;
    json["totalFilamentUsedLength"] = file.totalFilamentUsedLength;
    json["totalPrintTimes"] = file.totalPrintTimes;
    json["lastPrintTime"] = file.lastPrintTime;
    nlohmann::json mappingList = nlohmann::json::array();
    for (const auto& mapping : file.filamentMmsMappingList) {
        mappingList.push_back(convertPrintFilamentMmsMappingToJson(mapping));
    }
    json["filamentMmsMappingList"] = mappingList;
    return json;
}

PrinterPrintFile convertJsonToPrinterPrintFile(const nlohmann::json& json)
{
    PrinterPrintFile file;
    file.fileName = JsonUtils::safeGetString(json, "fileName", "");
    file.printTime = JsonUtils::safeGetInt64(json, "printTime", 0);
    file.layer = JsonUtils::safeGetInt(json, "layer", 0);
    file.layerHeight = JsonUtils::safeGetDouble(json, "layerHeight", 0.0);
    file.thumbnail = JsonUtils::safeGetString(json, "thumbnail", "");
    file.size = JsonUtils::safeGetInt64(json, "size", 0);
    file.createTime = JsonUtils::safeGetInt64(json, "createTime", 0);
    file.totalFilamentUsed = JsonUtils::safeGetDouble(json, "totalFilamentUsed", 0.0);
    file.totalFilamentUsedLength = JsonUtils::safeGetDouble(json, "totalFilamentUsedLength", 0.0);
    file.totalPrintTimes = JsonUtils::safeGetInt(json, "totalPrintTimes", 0);
    file.lastPrintTime = JsonUtils::safeGetInt64(json, "lastPrintTime", 0);
    if (json.contains("filamentMmsMappingList") && json["filamentMmsMappingList"].is_array()) {
        for (const auto& mappingJson : json["filamentMmsMappingList"]) {
            file.filamentMmsMappingList.push_back(convertJsonToPrintFilamentMmsMapping(mappingJson));
        }
    }
    return file;
}

nlohmann::json convertPrinterPrintFileResponseToJson(const PrinterPrintFileResponse& response)
{
    nlohmann::json json = nlohmann::json::object();
    json["totalFiles"] = response.totalFiles;
    nlohmann::json fileList = nlohmann::json::array();
    for (const auto& file : response.fileList) {
        fileList.push_back(convertPrinterPrintFileToJson(file));
    }
    json["fileList"] = fileList;
    return json;
}

PrinterPrintFileResponse convertJsonToPrinterPrintFileResponse(const nlohmann::json& json)
{
    PrinterPrintFileResponse response;
    response.totalFiles = JsonUtils::safeGetInt(json, "totalFiles", 0);
    if (json.contains("fileList") && json["fileList"].is_array()) {
        for (const auto& fileJson : json["fileList"]) {
            response.fileList.push_back(convertJsonToPrinterPrintFile(fileJson));
        }
    }
    return response;
}

nlohmann::json convertPrinterNetworkParamsToJson(const PrinterNetworkParams& params)
{
    nlohmann::json json;
    json["printerId"] = params.printerId;
    json["filePath"] = params.filePath;
    json["fileName"] = params.fileName;
    json["bedType"] = params.bedType;
    json["timeLapse"] = params.timeLapse;
    json["heatedBedLeveling"] = params.heatedBedLeveling;
    json["autoRefill"] = params.autoRefill;
    json["uploadAndStartPrint"] = params.uploadAndStartPrint;
    json["hasMms"] = params.hasMms;
    nlohmann::json mappingList = nlohmann::json::array();
    for (const auto& mapping : params.filamentMmsMappingList) {
        mappingList.push_back(convertPrintFilamentMmsMappingToJson(mapping));
    }
    json["filamentMmsMappingList"] = mappingList;
    return json;
}

PrinterNetworkParams convertJsonToPrinterNetworkParams(const nlohmann::json& json)
{
    PrinterNetworkParams params;
    params.printerId = JsonUtils::safeGetString(json, "printerId", "");
    params.filePath = JsonUtils::safeGetString(json, "filePath", "");
    params.fileName = JsonUtils::safeGetString(json, "fileName", "");
    params.bedType = JsonUtils::safeGetInt(json, "bedType", 0);
    params.timeLapse = JsonUtils::safeGetBool(json, "timeLapse", false);
    params.heatedBedLeveling = JsonUtils::safeGetBool(json, "heatedBedLeveling", false);
    params.autoRefill = JsonUtils::safeGetBool(json, "autoRefill", false);
    params.uploadAndStartPrint = JsonUtils::safeGetBool(json, "uploadAndStartPrint", false);
    params.hasMms = JsonUtils::safeGetBool(json, "hasMms", false);
    if (json.contains("filamentMmsMappingList") && json["filamentMmsMappingList"].is_array()) {
        for (const auto& mappingJson : json["filamentMmsMappingList"]) {
            params.filamentMmsMappingList.push_back(convertJsonToPrintFilamentMmsMapping(mappingJson));
        }
    }
    return params;
}

} // namespace Slic3r
