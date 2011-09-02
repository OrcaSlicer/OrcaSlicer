#include "libslic3r.h"
#include "Time.hpp"
#include "Thread.hpp"
#include "ProjectTask.hpp"


#include <thread>
#include <mutex>
#include <codecvt>

#include <boost/random.hpp>
#include <boost/log/trivial.hpp>
#include <boost/generator_iterator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include "nlohmann/json.hpp"

using namespace nlohmann;

namespace pt = boost::property_tree;

namespace Slic3r {

    BBLProfile::BBLProfile(BBLProject* project)
    {
        project_ = nullptr;
        if (project) {
            project_ = project;
            project_id = project_->project_id;
        }

        profile_name = "N/A";
    }

    BBLSliceInfo* BBLProfile::get_slice_info(std::string plate_idx)
    {
        std::map<std::string, BBLSliceInfo*>::iterator it = slice_info.find(plate_idx);
        if (it == slice_info.end())
            return nullptr;
        return it->second;
    }

    BBLTask::BBLTask(BBLProfile* profile)
    {
        profile_ = nullptr;
        if (profile) {
            profile_ = profile;
            task_profile_id = profile->profile_id;
            task_project_id = profile->project_id;
        }
    }

    BBLSubTask::BBLSubTask(BBLTask* task)
    {
        parent_task_ = task;
        if (task) {
            parent_id = task->task_id;
            task_project_id = task->task_project_id;
            task_profile_id = task->task_profile_id;
        }
        task_progress  = 0;
        task_record_timelapse = false;
        task_bed_type  = "auto";
    }

    int BBLSubTask::parse_content_json(std::string json_str)
    {
        try {
            json j = json::parse(json_str);

            if (j.contains("info") && !j["info"].is_null()) {
                if (j["info"].contains("name") && !j["info"]["name"].is_null())
                    task_name = j["info"]["name"].get<std::string>();
                if (j["info"].contains("plate_idx") && !j["info"]["plate_idx"].is_null()) {
                    if (j["info"]["plate_idx"].is_number())
                        task_partplate_idx = std::to_string(j["info"]["plate_idx"].get<int>());
                    else
                        task_partplate_idx = j["info"]["plate_idx"].get<std::string>();
                }
                if (j["info"].contains("printer") && !j["info"]["printer"].is_null())
                    task_printer_dev_id = j["info"]["printer"].get<std::string>();
                return 0;
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(trace) << "parse_content_json failed! json=" << json_str;
            return -1;
        }
        BOOST_LOG_TRIVIAL(trace) << "parse_content_json failed! json=" << json_str;
        return -1;
    }

    BBLSubTask::SubTaskStatus BBLSubTask::parse_status(std::string status)
    {
        if (status.compare("CREATED") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_CREATED;
        }
        else if (status.compare("READY") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_READY;
        }
        else if (status.compare("RUNNING") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_RUNNING;
        }
        else if (status.compare("PAUSE") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_PAUSE;
        }
        else if (status.compare("FAILED") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_FAILED;
        }
        else if (status.compare("FINISHED") == 0) {
            return BBLSubTask::SubTaskStatus::TASK_FINISHED;
        }
        else {
            return BBLSubTask::SubTaskStatus::TASK_CREATED;
        }
    }

    BBLSubTask::SubTaskStatus BBLSubTask::parse_user_service_task_status(int status)
    {
        if (status == 1)
            return BBLSubTask::SubTaskStatus::TASK_RUNNING;
        else if (status == 2)
            return BBLSubTask::SubTaskStatus::TASK_FINISHED;
        else if (status == 3)
            return BBLSubTask::SubTaskStatus::TASK_FAILED;
        return BBLSubTask::SubTaskStatus::TASK_UNKNOWN;
    }

    int BBLTask::parse_content_json(std::string json)
    {
        try {
            std::stringstream ss(json);
            pt::ptree root;
            pt::read_json(ss, root);

            for (int i = 0; i < subtasks.size(); i++) {
                delete subtasks[i];
            }
            subtasks.clear();
            if (root.get_child_optional("subtasks")!= boost::none) {
                pt::ptree subtask_list = root.get_child("subtasks");
                for (auto subtask = subtask_list.begin(); subtask != subtask_list.end(); ++subtask) {
                    BBLSubTask* new_subtask = new BBLSubTask(this);
                    /* create subtasks */
                    boost::optional<std::string> subtask_id = subtask->second.get_optional<std::string>("id");
                    if (subtask_id.has_value()) new_subtask->task_id = subtask_id.value();

                    boost::optional<std::string> subtask_name = subtask->second.get_optional<std::string>("name");
                    if (subtask_name.has_value()) new_subtask->task_name = subtask_name.value();

                    boost::optional<std::string> subtask_create_time = subtask->second.get_optional<std::string>("create_time");
                    if (subtask_create_time.has_value()) new_subtask->task_create_time = subtask_create_time.value();

                    boost::optional<std::string> subtask_plate_idx = subtask->second.get_optional<std::string>("plate_idx");
                    if (subtask_plate_idx.has_value()) new_subtask->task_partplate_idx = subtask_plate_idx.value();

                    boost::optional<std::string> subtask_printer = subtask->second.get_optional<std::string>("printer");
                    if (subtask_printer.has_value()) new_subtask->task_printer_dev_id = subtask_printer.value();

                    boost::optional<std::string> subtask_weight = subtask->second.get_optional<std::string>("weight");
                    if (subtask_weight.has_value()) new_subtask->task_weight = subtask_weight.value();
                    subtasks.push_back(new_subtask);
                }
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(trace) << "parse_content_json failed! json=" << json;
        }
        return 0;
    }

    void BBLProject::reset()
    {
        project_model_id.clear();
        project_name.clear();
        project_id.clear();
        project_design_id.clear();
        project_status.clear();
        project_create_time.clear();
        project_url.clear();
        project_url_md5.clear();
        project_3mf_file.clear();
        project_path.clear();
    }

    BBLModelTask::BBLModelTask()
    {
        job_id      = -1;
        design_id   = -1;
        profile_id  = -1;
    }

} // namespace Slic3r
