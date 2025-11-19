#ifndef __BAMBU_NETWORKING_HPP__
#define __BAMBU_NETWORKING_HPP__

#include <string>
#include <functional>
#include <map>

extern std::string g_log_folder;
extern std::string g_log_start_time;

namespace BBL {

#define BAMBU_NETWORK_SUCCESS                           0
#define BAMBU_NETWORK_ERR_INVALID_HANDLE                -1
#define BAMBU_NETWORK_ERR_CONNECT_FAILED                -2
#define BAMBU_NETWORK_ERR_DISCONNECT_FAILED             -3
#define BAMBU_NETWORK_ERR_SEND_MSG_FAILED               -4
#define BAMBU_NETWORK_ERR_BIND_FAILED                   -5
#define BAMBU_NETWORK_ERR_UNBIND_FAILED                 -6
#define BAMBU_NETWORK_ERR_REQUEST_SETTING_FAILED        -7
#define BAMBU_NETWORK_ERR_PUT_SETTING_FAILED            -8
#define BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED       -9
#define BAMBU_NETWORK_ERR_DEL_SETTING_FAILED            -10
#define BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED     -11
#define BAMBU_NETWORK_ERR_QUERY_BIND_INFO_FAILED        -12
#define BAMBU_NETWORK_ERR_MODIFY_PRINTER_NAME_FAILED    -13
#define BAMBU_NETWORK_ERR_FILE_NOT_EXIST                -14
#define BAMBU_NETWORK_ERR_FILE_OVER_SIZE                -15
#define BAMBU_NETWORK_ERR_CHECK_MD5_FAILED              -16
#define BAMBU_NETWORK_ERR_TIMEOUT                       -17
#define BAMBU_NETWORK_ERR_CANCELED                      -18
#define BAMBU_NETWORK_ERR_INVALID_RESULT                -19
#define BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED             -20
#define BAMBU_NETWORK_ERR_GET_RATING_ID_FAILED          -21
#define BAMBU_NETWORK_ERR_OPEN_FILE_FAILED              -22
#define BAMBU_NETWORK_ERR_PARSE_CONFIG_FAILED           -23
#define BAMBU_NETWORK_ERR_NO_CORRESPONDING_BUCKET       -24
#define BAMBU_NETWORK_ERR_GET_INSTANCE_ID_FAILED        -25
#define BAMBU_NETWORK_SIGNED_ERROR                      -26

//bind error
#define BAMBU_NETWORK_ERR_BIND_CREATE_SOCKET_FAILED          -1010 //failed to create socket
#define BAMBU_NETWORK_ERR_BIND_SOCKET_CONNECT_FAILED         -1020 //failed to socket connect
#define BAMBU_NETWORK_ERR_BIND_PUBLISH_LOGIN_REQUEST         -1030 //failed to publish login request
#define BAMBU_NETWORK_ERR_BIND_GET_PRINTER_TICKET_TIMEOUT    -1040 //timeout to get ticket from printer
#define BAMBU_NETWORK_ERR_BIND_GET_CLOUD_TICKET_TIMEOUT      -1050 //timeout to get ticket from cloud server
#define BAMBU_NETWORK_ERR_BIND_POST_TICKET_TO_CLOUD_FAILED   -1060 //failed to post ticket to cloud server
#define BAMBU_NETWORK_ERR_BIND_PARSE_LOGIN_REPORT_FAILED     -1070 //failed to parse login report reason no error code
#define BAMBU_NETWORK_ERR_BIND_ECODE_LOGIN_REPORT_FAILED     -1080 //failed to parse login report reason has error code
#define BAMBU_NETWORK_ERR_BIND_RECEIVE_LOGIN_REPORT_TIMEOUT  -1090 //timeout to receive login report

//start_local_print_with_record  error
#define BAMBU_NETWORK_ERR_PRINT_WR_REQUEST_PROJECT_ID_FAILED        -2010 //failed to request project id
#define BAMBU_NETWORK_ERR_PRINT_WR_CHECK_MD5_FAILED                 -2020 //failed to check md5 for upload 3mf to oss
#define BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_3MF_CONFIG_TO_OSS_FAILED  -2030 //failed to  upload 3mf config to oss
#define BAMBU_NETWORK_ERR_PRINT_WR_FILE_OVER_SIZE                   -2040 //the size of the uploaded file cannot exceed 1 GB
#define BAMBU_NETWORK_ERR_PRINT_WR_PUT_NOTIFICATION_FAILED          -2050 //timeout to get notification
#define BAMBU_NETWORK_ERR_PRINT_WR_GET_NOTIFICATION_TIMEOUT         -2060 //timeout to get notification
#define BAMBU_NETWORK_ERR_PRINT_WR_GET_NOTIFICATION_FAILED          -2070 //failed to get notification
#define BAMBU_NETWORK_ERR_PRINT_WR_PATCH_PROJECT_FAILED             -2080 //failed to patch project
#define BAMBU_NETWORK_ERR_PRINT_WR_GET_MY_SETTING_FAILED            -2090 //failed to get my setting
#define BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST                   -2100 //3mf file is not exists
#define BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_3MF_TO_OSS_FAILED         -2110 //failed to  upload 3mf to oss
#define BAMBU_NETWORK_ERR_PRINT_WR_POST_TASK_FAILED                 -2120 //failed to post task
#define BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED                -2130 //failed to upload to ftp
#define BAMBU_NETWORK_ERR_PRINT_WR_GET_USER_UPLOAD_FAILED           -2140 //failed to get_user_upload

//start_print  error
#define BAMBU_NETWORK_ERR_PRINT_SP_REQUEST_PROJECT_ID_FAILED        -3010 //failed to request project id
#define BAMBU_NETWORK_ERR_PRINT_SP_CHECK_MD5_FAILED                 -3020 //failed to check md5 for upload 3mf to oss
#define BAMBU_NETWORK_ERR_PRINT_SP_UPLOAD_3MF_CONFIG_TO_OSS_FAILED  -3030 //failed to upload 3mf config to oss
#define BAMBU_NETWORK_ERR_PRINT_SP_PUT_NOTIFICATION_FAILED          -3040 //failed to put notification
#define BAMBU_NETWORK_ERR_PRINT_SP_GET_NOTIFICATION_TIMEOUT         -3050 //timeout to get notification
#define BAMBU_NETWORK_ERR_PRINT_SP_GET_NOTIFICATION_FAILED          -3060 //failed to get notification
#define BAMBU_NETWORK_ERR_PRINT_SP_FILE_NOT_EXIST                   -3070 //3mf file is not exists
#define BAMBU_NETWORK_ERR_PRINT_SP_GET_USER_UPLOAD_FAILED           -3080 //failed to get_user_upload
#define BAMBU_NETWORK_ERR_PRINT_SP_FILE_OVER_SIZE                   -3090 //the size of the uploaded file cannot exceed 1 GB
#define BAMBU_NETWORK_ERR_PRINT_SP_UPLOAD_3MF_TO_OSS_FAILED         -3100 //failed to  upload 3mf to oss
#define BAMBU_NETWORK_ERR_PRINT_SP_PATCH_PROJECT_FAILED             -3110 //failed to patch project
#define BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED                 -3120 //failed to post task
#define BAMBU_NETWORK_ERR_PRINT_SP_WAIT_PRINTER_FAILED              -3130 //failed to wait the ack from printer
#define BAMBU_NETOWRK_ERR_PRINT_SP_ENC_FLAG_NOT_READY               -3140 //failed to get flag info

//start_local_print   error
#define BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE                   -4010 //the size of the uploaded file cannot exceed 1 GB
#define BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED                -4020 //failed to upload ftp
#define BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED               -4030 //failed to send mqtt message to  device

//start_send_gcode_to_sdcard error
#define BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED                -5010 //failed to upload ftp

//connection to printer failed
#define BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED              -6010 //Connection to printer failed
#define BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED               -6020 //Connection to server failed


#define BAMBU_NETWORK_LIBRARY               "bambu_networking"
#define BAMBU_NETWORK_AGENT_NAME            "bambu_network_agent"

#define BAMBU_NETWORK_AGENT_VERSION_LEGACY  "01.10.01.01"
#define BAMBU_NETWORK_AGENT_VERSION         "02.03.00.62"

//iot preset type strings
#define IOT_PRINTER_TYPE_STRING     "printer"
#define IOT_FILAMENT_STRING         "filament"
#define IOT_PRINT_TYPE_STRING       "print"

#define IOT_JSON_KEY_VERSION            "version"
#define IOT_JSON_KEY_NAME               "name"
#define IOT_JSON_KEY_TYPE               "type"
#define IOT_JSON_KEY_UPDATE_TIME        "update_time"
#define IOT_JSON_KEY_UPDATED_TIME        "updated_time"
#define IOT_JSON_KEY_BASE_ID            "base_id"
#define IOT_JSON_KEY_SETTING_ID         "setting_id"
#define IOT_JSON_KEY_FILAMENT_ID        "filament_id"
#define IOT_JSON_KEY_USER_ID            "user_id"

// user callbacks
typedef std::function<void(int online_login, bool login)> OnUserLoginFn;
// printer callbacks
typedef std::function<void(std::string topic_str)>  OnPrinterConnectedFn;
typedef std::function<void(int status, std::string dev_id, std::string msg)> OnLocalConnectedFn;
typedef std::function<void(int return_code, int reason_code)>                OnServerConnectedFn;
typedef std::function<void(std::string dev_id, std::string msg)> OnMessageFn;
// http callbacks
typedef std::function<void(unsigned http_code, std::string http_body)> OnHttpErrorFn;
typedef std::function<std::string()>                GetCountryCodeFn;
typedef std::function<void(std::string topic)>      GetSubscribeFailureFn;
// print callbacks
typedef std::function<void(int status, int code, std::string msg)> OnUpdateStatusFn;
typedef std::function<bool()>                       WasCancelledFn;
typedef std::function<bool(int status, std::string job_info)> OnWaitFn;
// local callbacks
typedef std::function<void(std::string dev_info_json_str)> OnMsgArrivedFn;
// queue call to main thread
typedef std::function<void(std::function<void()>)> QueueOnMainFn;

typedef std::function<void(int progress)> ProgressFn;
typedef std::function<void(int retcode, std::string info)> LoginFn;
typedef std::function<void(int result, std::string info)> ResultFn;
typedef std::function<bool()> CancelFn;
typedef std::function<bool(std::map<std::string, std::string> info)> CheckFn;

//err callbacks
typedef std::function<void(std::string url, int status)> OnServerErrFn;

enum SendingPrintJobStage {
    PrintingStageCreate = 0,
    PrintingStageUpload = 1,
    PrintingStageWaiting = 2,
    PrintingStageSending = 3,
    PrintingStageRecord  = 4,
    PrintingStageWaitPrinter = 5,
    PrintingStageFinished = 6,
    PrintingStageERROR = 7,
    PrintingStageLimit = 8,
};

enum PublishingStage {
    PublishingCreate    = 0,
    PublishingUpload    = 1,
    PublishingWaiting   = 2,
    PublishingJumpUrl   = 3,
};

enum BindJobStage {
    LoginStageConnect = 0,
    LoginStageLogin = 1,
    LoginStageWaitForLogin = 2,
    LoginStageGetIdentify = 3,
    LoginStageWaitAuth = 4,
    LoginStageFinished = 5,
};

enum ConnectStatus {
    ConnectStatusOk = 0,
    ConnectStatusFailed = 1,
    ConnectStatusLost = 2,
};

struct detectResult {
    std::string    result_msg;
    std::string    command;
    std::string    dev_id;
    std::string    model_id;
    std::string    dev_name;
    std::string    version;
    std::string    bind_state;
    std::string    connect_type;
};

/* print job*/
struct PrintParams_Legacy {
    /* basic info */
    std::string     dev_id;
    std::string     task_name;
    std::string     project_name;
    std::string     preset_name;
    std::string     filename;
    std::string     config_filename;
    int             plate_index;
    std::string     ftp_folder;
    std::string     ftp_file;
    std::string     ftp_file_md5;
    std::string     ams_mapping;
    std::string     ams_mapping_info;
    std::string     connection_type;
    std::string     comments;
    int             origin_profile_id = 0;
    int             stl_design_id = 0;
    std::string     origin_model_id;
    std::string     print_type;
    std::string     dst_file;
    std::string     dev_name;

    /* access options */
    std::string     dev_ip;
    bool            use_ssl_for_ftp;
    bool            use_ssl_for_mqtt;
    std::string     username;
    std::string     password;

    /*user options */
    bool            task_bed_leveling;      /* bed leveling of task */
    bool            task_flow_cali;         /* flow calibration of task */
    bool            task_vibration_cali;    /* vibration calibration of task */
    bool            task_layer_inspect;     /* first layer inspection of task */
    bool            task_record_timelapse;  /* record timelapse of task */
    bool            task_use_ams;
    std::string     task_bed_type;
    std::string     extra_options;
};

/* print job*/
struct PrintParams {
    /* basic info */
    std::string     dev_id;
    std::string     task_name;
    std::string     project_name;
    std::string     preset_name;
    std::string     filename;
    std::string     config_filename;
    int             plate_index;
    std::string     ftp_folder;
    std::string     ftp_file;
    std::string     ftp_file_md5;
    std::string     nozzle_mapping;
    std::string     ams_mapping;
    std::string     ams_mapping2;
    std::string     ams_mapping_info;
    std::string     nozzles_info;
    std::string     connection_type;
    std::string     comments;
    int             origin_profile_id = 0;
    int             stl_design_id = 0;
    std::string     origin_model_id;
    std::string     print_type;
    std::string     dst_file;
    std::string     dev_name;

    /* access options */
    std::string     dev_ip;
    bool            use_ssl_for_ftp;
    bool            use_ssl_for_mqtt;
    std::string     username;
    std::string     password;

    /*user options */
    bool            task_bed_leveling;      /* bed leveling of task */
    bool            task_flow_cali;         /* flow calibration of task */
    bool            task_vibration_cali;    /* vibration calibration of task */
    bool            task_layer_inspect;     /* first layer inspection of task */
    bool            task_record_timelapse;  /* record timelapse of task */
    bool            task_use_ams;
    std::string     task_bed_type;
    std::string     extra_options;
    int             auto_bed_leveling{ 0 };
    int             auto_flow_cali{ 0 };
    int             auto_offset_cali{ 0 };
    bool            task_ext_change_assist;
    bool            try_emmc_print;
};

struct TaskQueryParams
{
    std::string dev_id;
    int status = 0;
    int offset = 0;
    int limit = 20;
};

struct PublishParams {
    std::string     project_name;
    std::string     project_3mf_file;
    std::string     preset_name;
    std::string     project_model_id;
    std::string     design_id;
    std::string     config_filename;
};

struct CertificateInformation {
    std::string     issuer;
    std::string     sub_name;
    std::string     start_date;
    std::string     end_date;
    std::string     serial_number;
};


enum class MessageFlag : int
{
    MSG_FLAG_NONE = 0,
    MSG_SIGN      = 1 << 0,
    MSG_ENCRYPT   = 1 << 1,
};

}

#endif
