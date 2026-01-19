#include "TaskManager.hpp"

#include "libslic3r/Thread.hpp"
#include "nlohmann/json.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"

using namespace nlohmann;

namespace Slic3r {
wxDEFINE_EVENT(EVT_MULTI_SEND_LIMIT, wxCommandEvent);

int TaskManager::MaxSendingAtSameTime = 5;
int TaskManager::SendingInterval = 180;

std::string get_task_state_enum_str(TaskState ts)
{
    switch (ts) {
    case TaskState::TS_PENDING:
        return "task pending";
    case TaskState::TS_SENDING:
        return "task sending";
    case TaskState::TS_SEND_COMPLETED:
        return "task sending completed";
    case TaskState::TS_SEND_CANCELED:
        return "task sending canceled";
    case TaskState::TS_SEND_FAILED:
        return "task sending failed";
    case TaskState::TS_PRINTING:
        return "task printing";
    case TaskState::TS_PRINT_SUCCESS:
        return "task print success";
    case TaskState::TS_PRINT_FAILED:
        return "task print failed";
    case TaskState::TS_IDLE:
        return "task idle";
    case TaskState::TS_REMOVED:
        return "task removed";
    default:
        assert(false);
    }
    return "unknown task state";
}

TaskState parse_task_status(int status)
{
    switch (status)
    {
    case 1:
        return TaskState::TS_PRINTING;
    case 2:
        return TaskState::TS_PRINT_SUCCESS;
    case 3:
        return TaskState::TS_PRINT_FAILED;
    case 4:
        return TaskState::TS_PRINTING;
    default:
        return TaskState::TS_PRINTING;
    }
    return TaskState::TS_PRINTING;
}

int TaskStateInfo::g_task_info_id = 0;

TaskStateInfo::TaskStateInfo(BBL::PrintParams param)
    : m_state(TaskState::TS_PENDING)
    , m_params(param)
    , m_sending_percent(0)
    , m_state_changed_fn(nullptr)
    , m_cancel(false)
{
    task_info_id = ++TaskStateInfo::g_task_info_id;

    this->set_task_name(param.project_name);
    this->set_device_name(param.dev_name);

    cancel_fn = [this]() {
        return m_cancel;
    };
    update_status_fn = [this](int stage, int code, std::string msg) {

        if (stage == PrintingStageLimit)
        {
            //limit
            //wxCommandEvent event(EVT_MULTI_SEND_LIMIT);
            //wxPostEvent(this, event);
            GUI::wxGetApp().mainframe->CallAfter([]() {
                GUI::wxGetApp().show_dialog("The printing task exceeds the limit, supporting a maximum of 6 printers.");
            });
        }

        const int StagePercentPoint[(int)PrintingStageFinished + 1] = {
                10,    // PrintingStageCreate
                25,    // PrintingStageUpload
                70,    // PrintingStageWaiting
                75,    // PrintingStageRecord
                90,    // PrintingStageSending
                95,    // PrintingStageFinished
                100    // PrintingStageFinished
        };
        BOOST_LOG_TRIVIAL(trace) << "task_manager: update task, " << m_params.dev_id << ", stage = " << stage << "code = " << code;
        // update current percnet
        int curr_percent = 0;
        if (stage >= 0 && stage <= (int)PrintingStageFinished) {
            curr_percent = StagePercentPoint[stage];
            if ((stage == BBL::SendingPrintJobStage::PrintingStageUpload
                || stage == BBL::SendingPrintJobStage::PrintingStageRecord)
                && (code > 0 && code <= 100)) {
                curr_percent = (StagePercentPoint[stage + 1] - StagePercentPoint[stage]) * code / 100 + StagePercentPoint[stage];
                BOOST_LOG_TRIVIAL(trace) << "task_manager: percent = " << curr_percent;
            }
        }

        BOOST_LOG_TRIVIAL(trace) << "task_manager: update task, curr_percent = " << curr_percent;
        update_sending_percent(curr_percent);
    };

    wait_fn = [this](int status, std::string job_info) {
        BOOST_LOG_TRIVIAL(info) << "task_manager: get_job_info = " << job_info;
        m_job_id = job_info;
        return true;
    };
}

void TaskStateInfo::cancel()
{
    m_cancel = true;
    if (m_state == TaskState::TS_PENDING)
        m_state = TaskState::TS_REMOVED;
    update();
}

bool TaskGroup::need_schedule(std::chrono::system_clock::time_point last, TaskStateInfo* task)
{
    /* only pending task will be scheduled */
    if (task->state() != TaskState::TS_PENDING)
        return false;
    std::chrono::system_clock::time_point curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last);
    if (diff.count() > TaskManager::SendingInterval * 1000) {
        BOOST_LOG_TRIVIAL(trace) << "task_manager: diff count = " << diff.count() << " milliseconds";
        return true;
    }
    return false;
}

void TaskManager::set_max_send_at_same_time(int count)
{
    TaskManager::MaxSendingAtSameTime = count;
}

TaskManager::TaskManager(NetworkAgent* agent)
    :m_agent(agent)
{
    ;
}


int TaskManager::start_print(const std::vector<BBL::PrintParams>& params, TaskSettings* settings)
{
    BOOST_LOG_TRIVIAL(info) << "task_manager: start_print size = " << params.size();
    TaskManager::MaxSendingAtSameTime = settings->max_sending_at_same_time;
    TaskManager::SendingInterval = settings->sending_interval;
    m_map_mutex.lock();
    TaskGroup task_group(*settings);
    task_group.tasks.reserve(params.size());
    for (auto it = params.begin(); it != params.end(); it++) {
        TaskStateInfo* new_item = new TaskStateInfo(*it);
        task_group.append(new_item);
    }
    m_cache_map.push_back(task_group);
    m_map_mutex.unlock();
    return 0;
}

static int start_print_test(BBL::PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    int tick = 2;
    for (int i = 0; i < 100 * tick; i++) {
        boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
        if (cancel_fn) {
            if (cancel_fn()) {
                return -1;
            }
        }
        if (i == tick) {
            if (update_fn) update_fn(PrintingStageCreate, 0, "");
        }
        if (i >= 20 * tick && i <= 70 * tick) {
            int percent = (i - 20 * tick) * 2 / tick;
            if (update_fn) update_fn(PrintingStageUpload, percent, "");
        }

        if (i == 80 * tick)
            if (update_fn) update_fn(PrintingStageSending, 0, "");
        if (i == 99 * tick)
            if (update_fn) update_fn(PrintingStageFinished, 0, "");
    }
    return 0;
}

int TaskManager::schedule(TaskStateInfo* task)
{
    if (!m_agent) {
        assert(false);
        return -1;
    }
    if (task->state() != TaskState::TS_PENDING)
        return 0;
    assert(task->state() == TaskState::TS_PENDING);
    task->set_state(TaskState::TS_SENDING);

    BOOST_LOG_TRIVIAL(trace) << "task_manager: schedule a task to dev_id = " << task->params().dev_id;
    boost::thread* new_sending_thread = new boost::thread();
    *new_sending_thread = Slic3r::create_thread(
        [this, task] {
            if (!m_agent) {
                BOOST_LOG_TRIVIAL(trace) << "task_manager: NetworkAgent is nullptr";
                return;
            }
            assert(m_agent);
// DEBUG FOR TEST
#if 0
            int result = start_print_test(task->get_params(), task->update_status_fn, task->cancel_fn, task->wait_fn);
#else
            int result = m_agent->start_print(task->get_params(), task->update_status_fn, task->cancel_fn, task->wait_fn);
#endif
            if (result == 0) {
                last_sent_timestamp = std::chrono::system_clock::now();
                task->set_sent_time(last_sent_timestamp);
                task->set_state(TaskState::TS_SEND_COMPLETED);
            }
            else {
                if (!task->is_canceled()) {
                    task->set_state(TaskState::TS_SEND_FAILED);
                } else {
                    task->set_state(TaskState::TS_SEND_CANCELED);
                }
            }
     
            /* remove from sending task list */
            m_scedule_mutex.lock();
            auto it = std::find(m_scedule_list.begin(), m_scedule_list.end(), task);
            if (it != m_scedule_list.end()) {
                BOOST_LOG_TRIVIAL(trace) << "task_manager: schedule, scedule task has removed from list";
                m_scedule_list.erase(it);
            }
            else {
                /*assert(false);*/
            }
            m_scedule_mutex.unlock();
        }
    );
    m_sending_thread_list.push_back(new_sending_thread);
    return 0;
}

void TaskManager::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_scedule_thread = Slic3r::create_thread(
        [this] {
        BOOST_LOG_TRIVIAL(trace) << "task_manager: thread start()";
        while (m_started) {
            m_map_mutex.lock();
            for (auto it = m_cache_map.begin(); it != m_cache_map.end(); it++) {
                for (auto iter = it->tasks.begin(); iter != it->tasks.end(); iter++) {
                    m_scedule_mutex.lock();
                    if (m_scedule_list.size() < TaskManager::MaxSendingAtSameTime
                        && it->need_schedule(last_sent_timestamp, *iter)) {
                        m_scedule_list.push_back(*iter);
                    }
                    m_scedule_mutex.unlock();
                }
            }
            m_map_mutex.unlock();
            if (!m_scedule_list.empty()) {
                //BOOST_LOG_TRIVIAL(trace) << "task_manager: need scedule task count = " << m_scedule_list.size();
                m_scedule_mutex.lock();
                for (auto it = m_scedule_list.begin(); it != m_scedule_list.end(); it++) {
                    this->schedule(*it);
                }
                m_scedule_mutex.unlock();
            }
            boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
        }
        BOOST_LOG_TRIVIAL(trace) << "task_manager: thread exit()";
    });
}

void TaskManager::stop()
{
    m_started = false;
    if (m_scedule_thread.joinable())
        m_scedule_thread.join();
}

std::map<int, TaskStateInfo*> TaskManager::get_local_task_list()
{
    std::map<int, TaskStateInfo*> out;
    m_map_mutex.lock();
    for (auto it = m_cache_map.begin(); it != m_cache_map.end(); it++) {
        for (auto iter = (*it).tasks.begin(); iter != (*it).tasks.end(); iter++) {
            if ((*iter)->state() == TaskState::TS_PENDING
                || (*iter)->state() == TaskState::TS_SENDING
                || (*iter)->state() == TaskState::TS_SEND_CANCELED
                || (*iter)->state() == TaskState::TS_SEND_COMPLETED
                || (*iter)->state() == TaskState::TS_SEND_FAILED) {
                out.insert(std::make_pair((*iter)->task_info_id, *iter));
            }
        }
    }
    m_map_mutex.unlock();
    return out;
}

std::map<std::string, TaskStateInfo> TaskManager::get_task_list(int curr_page, int page_count, int& total)
{
    std::map<std::string, TaskStateInfo> out;
    if (m_agent) {
        BBL::TaskQueryParams task_query_params;
        task_query_params.limit = page_count;
        task_query_params.offset = curr_page * page_count;
        std::string task_info;
        int result = m_agent->get_user_tasks(task_query_params, &task_info);
        BOOST_LOG_TRIVIAL(trace) << "task_manager: get_task_list task_info=" << task_info;
        if (result == 0) {
            try {
                json j = json::parse(task_info);
                if (j.contains("total")) {
                    total = j["total"].get<int>();
                }
                if (!j.contains("hits")) {
                    return out;
                }
                BOOST_LOG_TRIVIAL(trace) << "task_manager: get_task_list task count =" << j["hits"].size();
                for (auto& hit : j["hits"]) {
                    TaskStateInfo task_info;
                    int64_t design_id = 0;
                    if (hit.contains("designId")) {
                        design_id = hit["designId"].get<int64_t>();
                    }
                    if (design_id > 0 && hit.contains("designTitle")) {
                        task_info.set_task_name(hit["designTitle"].get<std::string>());
                    } else {
                        if (hit.contains("title"))
                            task_info.set_task_name(hit["title"].get<std::string>());
                    }
                    if (hit.contains("deviceName"))
                        task_info.set_device_name(hit["deviceName"].get<std::string>());
                    if (hit.contains("deviceId"))
                        task_info.params().dev_id = hit["deviceId"].get<std::string>();
                    if (hit.contains("id"))
                        task_info.set_job_id(std::to_string(hit["id"].get<int64_t>()));
                    if (hit.contains("status"))
                        task_info.set_state(parse_task_status(hit["status"].get<int>()));
                    if (hit.contains("cover"))
                        task_info.thumbnail_url = hit["cover"].get<std::string>();
                    if (hit.contains("startTime"))
                        task_info.start_time = hit["startTime"].get<std::string>();
                    if (hit.contains("endTime"))
                        task_info.end_time = hit["endTime"].get<std::string>();
                    if (hit.contains("profileId"))
                        task_info.profile_id = std::to_string(hit["profileId"].get<int64_t>());
                    if (!task_info.get_job_id().empty())
                        out.insert(std::make_pair(task_info.get_job_id(), task_info));
                }
            }
            catch(...) {
            }
        }
    }
    return out;
}

TaskState TaskManager::query_task_state(std::string dev_id)
{
    /* priority: TS_SENDING > TS_PENDING > TS_IDLE */
    TaskState ts = TaskState::TS_IDLE;
    m_map_mutex.lock();
    for (auto& task_group : m_cache_map) {
        for (auto it = task_group.tasks.begin(); it != task_group.tasks.end(); it++) {
            if ((*it)->params().dev_id == dev_id) {
                if ((*it)->state() == TS_SENDING) {
                    m_map_mutex.unlock();
                    return TS_SENDING;
                } else if ((*it)->state() == TS_PENDING) {
                    ts = TS_PENDING;
                }
            }
        }
    }
    m_map_mutex.unlock();
    return ts;
}

} // namespace Slic3r
