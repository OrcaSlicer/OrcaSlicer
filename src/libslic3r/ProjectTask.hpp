#ifndef slic3r_ProjectTask_hpp_
#define slic3r_ProjectTask_hpp_

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <boost/thread.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

namespace Slic3r {

class BBLProject;
class BBLProfile;
class BBLTask;
class BBLModelTask;


enum MachineBedType {
    //BED_TYPE_AUTO = 0,
    BED_TYPE_PC = 0,
    BED_TYPE_PE,
    BED_TYPE_PEI,
    BED_TYPE_PTE,
    BED_TYPE_COUNT,
};

enum MappingResult {
    MAPPING_RESULT_DEFAULT = 0,
    MAPPING_RESULT_TYPE_MISMATCH = 1,
    MAPPING_RESULT_EXCEED = 2
};

struct FilamentInfo
{
    int         id{0};         // filament id = extruder id, start with 0.
    std::string type;
    std::string color;
    std::string filament_id;
    std::string brand;
    float       used_m{0.f};
    float       used_g{0.f};
    int         tray_id{0}; // start with 0
    float       distance{0.f};
    int         ctype = 0;
    std::vector<std::string> colors = std::vector<std::string>();
    int         mapping_result = 0;

    /*for new ams mapping*/
    std::string ams_id;
    std::string slot_id;

public:
    int get_ams_id() const
    {
        if (ams_id.empty()) { return -1; };

        try
        {
            return stoi(ams_id);
        }
        catch (...) {};

        return -1;
    };

    int get_slot_id() const
    {
        if (slot_id.empty()) { return -1; };

        try {
            return stoi(slot_id);
        } catch (...) {};

        return -1;
    };

    /*copied from AmsTray::get_display_filament_type()*/
    std::string get_display_filament_type() const
    {
        if (type == "PLA-S")
            return "Sup.PLA";
        else if (type == "PA-S")
            return "Sup.PA";
        else if (type == "ABS-S")
            return "Sup.ABS";
        else
            return type;
        return type;
    }
};

class BBLSliceInfo {
public:
    BBLSliceInfo(BBLProfile* profile = nullptr)
    {
        profile_ = profile;
        prediction = 0;
        weight = 0.0f;
    }

    BBLSliceInfo(const BBLSliceInfo& obj) {
        this->index = obj.index;
        this->title = obj.title;
        this->thumbnail_dir = obj.thumbnail_dir;
        this->thumbnail_name = obj.thumbnail_name;
        this->thumbnail_url = obj.thumbnail_url;
        this->gcode_name = obj.gcode_name;
        this->gcode_dir = obj.gcode_dir;
        this->gcode_url = obj.gcode_url;
        this->weight = obj.weight;
        this->prediction = obj.prediction;
        this->profile_ = obj.profile_;
        this->filaments_info = obj.filaments_info;
    }

    std::vector<FilamentInfo> filaments_info;

    std::string     index;              // plate index, start 1, 2, 3, etc.
    std::string     title;
    std::string     thumbnail_dir;
    std::string     thumbnail_name;
    std::string     thumbnail_url;
    std::string     gcode_name;
    std::string     gcode_url;
    std::string     gcode_dir;
    std::string     config_url;
    float           weight;
    int             prediction;
    BBLProfile*     profile_;
};

enum TaskUserOptions {
    OPTIONS_BED_LEVELING   = 0,
    OPTIONS_VIBRATION_CALI = 1,
    OPTIONS_FLOW_CALI      = 2,
    OPTIONS_LAYER_INSPECT  = 3,
    OPTIONS_RECORD_TIMELAPSE = 4
};

class BBLModelTask {
public:
    BBLModelTask();
    ~BBLModelTask() {}

    int                         job_id;
    int                         design_id;
    int                         profile_id;
    int                         instance_id;
    std::string                 task_id;
    std::string                 model_id;
    std::string                 model_name;
    std::string                 profile_name;
};

class BBLSubTask {
public:
    enum SubTaskStatus {
        TASK_CREATED = 0,
        TASK_READY = 1,
        TASK_RUNNING = 2,
        TASK_PAUSE = 3,
        TASK_FAILED = 4,
        TASK_FINISHED = 5,
        TASK_UNKNOWN = 6
    };

    BBLSubTask(BBLTask* task = nullptr);

    BBLSubTask(const BBLSubTask& obj) {
        task_id             = obj.task_id;
        parent_id           = obj.parent_id;
        task_model_id       = obj.task_model_id;
        task_project_id     = obj.task_project_id;
        task_profile_id     = obj.task_profile_id;
        task_name           = obj.task_name;
        task_partplate_idx  = obj.task_partplate_idx;
        task_printer_dev_id = obj.task_printer_dev_id;
        task_create_time    = obj.task_create_time;
        task_url            = obj.task_url;
        task_url_md5        = obj.task_url_md5;
        task_gcode_in_3mf   = obj.task_gcode_in_3mf;
        task_record_timelapse = obj.task_record_timelapse;
        task_bed_type       = obj.task_bed_type;
        task_bed_leveling   = obj.task_bed_leveling;
        task_flow_cali      = obj.task_flow_cali;
        task_vibration_cali = obj.task_vibration_cali;
        task_layer_inspect  = obj.task_layer_inspect;

        job_id              = obj.job_id;
        origin_model_name   = obj.origin_model_name;
        origin_profile_name = obj.origin_profile_name;
    }

    std::string     task_id;            /* plate id */
    std::string     task_model_id;      /* model id */
    std::string     task_project_id;    /* project id */
    std::string     task_profile_id;    /* profile id*/
    std::string     task_name;          /* task name, generally filename as task name */
    std::string     task_file;          /* local full file path of 3mf or gcode */
    fs::path        task_path;          /* local path of 3mf or gcode */
    std::string     task_gcode_in_3mf;  /* gcode in 3mf */
    std::string     task_create_time;   /* time created by cloud */
    std::string     task_thumbnail_url; /* url of task thumbnail */
    /* user options */
    std::string     task_bed_type;      /* bed_type of task, enum "auto" "pe", "pc", "pei" */
    bool            task_bed_leveling;  /* bed leveling of task */
    bool            task_flow_cali;     /* flow calibration of task */
    bool            task_vibration_cali; /* vibration calibration of task */
    bool            task_layer_inspect {true}; /* first layer inspection of task */
    bool            task_record_timelapse; /* record timelapse of task */

    // task of plate info
    std::string     task_weight;        /* weight create by slicer */
    float           task_weightF;       /* weight in task */
    BBLSliceInfo    slice_info;         /* slice info of subtask */
    std::string     task_partplate_idx; /* partplate_idx, start at 1, 2, etc. */

    SubTaskStatus   task_status;
    std::string     task_printer_dev_id;/* dev_id of machine */
    int             task_progress;      /* task running progress, update by machine */
    std::string     printing_status;    /* task status, update by machine */
    std::string     task_url;           /* post task to this url */
    std::string     task_url_md5;       /* md5 of task file */
    BBLTask*        parent_task_;
    std::string     parent_id;

    int             job_id;
    std::string     origin_model_name;
    std::string     origin_profile_name;

    int parse_content_json(std::string json_str);
    static BBLSubTask::SubTaskStatus parse_status(std::string status);
    static BBLSubTask::SubTaskStatus parse_user_service_task_status(int status);
};

typedef std::function<void(BBLModelTask* subtask)> OnGetSubTaskFn;

class BBLTask {
public:
    enum TaskStatus {
        TASK_ACTIVE = 0,
        TASK_INACTIVE = 1,
    };

    BBLTask(BBLProfile* profile = nullptr);

    /* properties */
    std::string                 task_id;
    std::string                 task_name;
    std::string                 task_create_time;
    TaskStatus                  task_status;
    std::wstring                task_file;          /* local task file */
    std::string                 task_url;           /* cloud task url */
    std::string                 task_url_md5;       /* md5 of cloud task url file */
    std::wstring                task_dst_url;       /* put task to dest url in machine */
    BBLProfile*                 profile_;
    std::string                 task_project_id;
    std::string                 task_model_id;
    std::string                 task_profile_id;
    std::vector<BBLSubTask*>    subtasks;
    std::map<std::string, BBLSliceInfo*> slice_info; /* slice info of subtasks, key: plate idx, 1, 2, 3, etc... */

    std::string task_status_str() {
        if (task_status == TaskStatus::TASK_ACTIVE) {
            return "active";
        }
        else if (task_status == TaskStatus::TASK_INACTIVE) {
            return "inactive";
        }
        else {
            return "inactive";
        }
    }

    int parse_content_json(std::string json);
};

class BBLProfile {
public:
    BBLProfile(BBLProject* project = nullptr);
    ~BBLProfile() {}

    std::vector<BBLTask*>   tasks;
    std::string             profile_id;
    std::string             profile_name;
    std::string             profile_content;
    std::string             project_id;         /* parent project_id */
    std::string             model_id;           /* parent model_id */
    std::string             upload_url;         /* url for upload 3mf */
    std::string             upload_ticket;      /* ticket for notification */
    std::string             url;                /* 3mf url */
    std::string             md5;                /* 3mf md5 */
    std::string             filename;           /* 3mf filename */
    BBLProject*             project_;
    std::map<std::string, BBLSliceInfo*>    slice_info; /* key: plate_idx, start at 1, 2, 3, etc. */
    BBLSliceInfo* get_slice_info(std::string plate_idx);
};

class BBLProject {
public:
    BBLProject() {
        /* give a default project name */
        project_name = "Untitled";
    }
    BBLProject(std::string name) {
        project_name = name;
    }

    std::string     project_id;
    std::string     project_model_id;       /* model id */
    std::string     project_design_id;      /* design_id */
    std::string     project_status;
    std::string     project_create_time;    /* created by cloud */
    std::string     project_url;            /* url storage on cloud */
    std::string     project_url_md5;        /* md5 of project url file */
    std::string     project_name;
    std::string     project_3mf_file;
    fs::path        project_path;
    std::string     project_content;
    std::string     project_country_code;


    std::vector<BBLProfile*>   profiles;

    /* deprecated apis */
    void set_name(std::string name) { project_name = name; }

    void reset();
};

} // namespace Slic3r

#endif //  slic3r_ProjectTask_hpp_
