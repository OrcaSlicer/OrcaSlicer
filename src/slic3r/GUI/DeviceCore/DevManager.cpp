#include <nlohmann/json.hpp>
#include "DevManager.h"
#include "DevUtil.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Time.hpp"

using namespace nlohmann;

namespace Slic3r
{
    DeviceManager::DeviceManager(NetworkAgent* agent)
    {
        m_agent = agent;
        m_refresher = new DeviceManagerRefresher(this);

        DevPrinterConfigUtil::InitFilePath(resources_dir());

        // Load saved local machines
        if (agent) {
            AppConfig*  config         = GUI::wxGetApp().app_config;
            const auto local_machines = config->get_local_machines();
            for (auto& it : local_machines) {
                const auto&    m         = it.second;
                MachineObject* obj       = new MachineObject(this, m_agent, m.dev_name, m.dev_id, m.dev_ip);
                obj->printer_type        = m.printer_type;
                obj->dev_connection_type = "lan";
                obj->bind_state          = "free";
                obj->bind_sec_link       = "secure";
                obj->m_is_online         = true;
                obj->last_alive          = Slic3r::Utils::get_current_time_utc();
                obj->set_access_code(config->get("access_code", m.dev_id), false);
                obj->set_user_access_code(config->get("user_access_code", m.dev_id), false);
                if (obj->has_access_right()) {
                    localMachineList.insert(std::make_pair(m.dev_id, obj));
                } else {
                    config->erase_local_machine(m.dev_id);
                    delete obj;
                }
            }
        }
    }

    void DeviceManager::update_local_machine(const MachineObject& m)
    {
        AppConfig* config = GUI::wxGetApp().app_config;
        if (config) {
            if (m.is_lan_mode_printer()) {
                if (m.has_access_right()) {
                    BBLocalMachine local_machine;
                    local_machine.dev_id       = m.get_dev_id();
                    local_machine.dev_name     = m.get_dev_name();
                    local_machine.dev_ip       = m.get_dev_ip();
                    local_machine.printer_type = m.printer_type;
                    config->update_local_machine(local_machine);
                }
            } else {
                config->erase_local_machine(m.get_dev_id());
            }
        }
    }

    DeviceManager::~DeviceManager()
    {
        delete m_refresher;

        for (auto it = localMachineList.begin(); it != localMachineList.end(); it++)
        {
            if (it->second)
            {
                delete it->second;
                it->second = nullptr;
            }
        }
        localMachineList.clear();

        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            if (it->second)
            {
                delete it->second;
                it->second = nullptr;
            }
        }
        userMachineList.clear();
    }


    void DeviceManager::EnableMultiMachine(bool enable)
    {
        m_agent->enable_multi_machine(enable);
        m_enable_mutil_machine = enable;
    }

    void DeviceManager::start_refresher() { m_refresher->Start(); }
    void DeviceManager::stop_refresher() { m_refresher->Stop(); }


    void DeviceManager::keep_alive()
    {
        MachineObject* obj = this->get_selected_machine();
        if (obj)
        {
            if (obj->keep_alive_count == 0)
            {
                obj->last_keep_alive = std::chrono::system_clock::now();
            }
            obj->keep_alive_count++;
            std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
            auto internal = std::chrono::duration_cast<std::chrono::milliseconds>(start - obj->last_keep_alive);
            if (internal.count() > TIMEOUT_FOR_KEEPALIVE && (internal.count() < 1000 * 60 * 60 * 300))
            {
                BOOST_LOG_TRIVIAL(info) << "keep alive = " << internal.count() << ", count = " << obj->keep_alive_count;
                obj->command_request_push_all();
                obj->last_keep_alive = start;
            }
            else if (obj->m_push_count == 0)
            {
                BOOST_LOG_TRIVIAL(info) << "keep alive = " << internal.count() << ", push_count = 0, count = " << obj->keep_alive_count;
                obj->command_request_push_all();
                obj->last_keep_alive = start;
            }
        }
    }

    void DeviceManager::check_pushing()
    {
        keep_alive();
        MachineObject* obj = this->get_selected_machine();

        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
        auto internal = std::chrono::duration_cast<std::chrono::milliseconds>(start - obj->last_update_time);

        if (obj && !obj->is_support_mqtt_alive)
        {
            if (internal.count() > TIMEOUT_FOR_STRAT && internal.count() < 1000 * 60 * 60 * 300)
            {
                BOOST_LOG_TRIVIAL(info) << "command_pushing: diff = " << internal.count();
                obj->command_pushing("start");
            }
        }
    }

    void DeviceManager::on_machine_alive(std::string json_str)
    {
        try {
            json j = json::parse(json_str);
            std::string dev_name        = j["dev_name"].get<std::string>();
            std::string dev_id          = j["dev_id"].get<std::string>();
            std::string dev_ip          = j["dev_ip"].get<std::string>();
            std::string printer_type_str= j["dev_type"].get<std::string>();
            std::string printer_signal  = j["dev_signal"].get<std::string>();
            std::string connect_type    = j["connect_type"].get<std::string>();
            std::string bind_state      = j["bind_state"].get<std::string>();

            if (connect_type == "farm") {
                connect_type ="lan";
                bind_state   = "free";
            }

            std::string sec_link = "";
            std::string ssdp_version = "";
            if (j.contains("sec_link")) {
                sec_link = j["sec_link"].get<std::string>();
            }
            if (j.contains("ssdp_version")) {
                ssdp_version = j["ssdp_version"].get<std::string>();
            }
            std::string connection_name = "";
            if (j.contains("connection_name")) {
                connection_name = j["connection_name"].get<std::string>();
            }

            MachineObject* obj;

            /* update userMachineList info */
            auto it = userMachineList.find(dev_id);
            if (it != userMachineList.end()) {
                if (it->second->get_dev_ip() != dev_ip ||
                    it->second->bind_state != bind_state ||
                    it->second->bind_sec_link != sec_link ||
                    it->second->dev_connection_type != connect_type ||
                    it->second->bind_ssdp_version != ssdp_version)
                {
                    if (it->second->bind_state != bind_state) {
                        OnMachineBindStateChanged(it->second, bind_state);
                    }

                    it->second->set_dev_ip(dev_ip);
                    it->second->bind_state          = bind_state;
                    it->second->bind_sec_link       = sec_link;
                    it->second->dev_connection_type = connect_type;
                    it->second->bind_ssdp_version   = ssdp_version;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " UpdateUserMachineInfo"
                        << ", dev_id= " << dev_id
                        << ", ip = "  <<dev_ip
                        << ", printer_name= " << dev_name
                        << ", con_type= " << connect_type << ", signal= " << printer_signal
                        << ", bind_state= " << bind_state;
                }
            }

            /* update localMachineList */
            it = localMachineList.find(dev_id);
            if (it != localMachineList.end()) {
                // update properties
                /* ip changed */
                obj = it->second;

                if (obj->get_dev_ip().compare(dev_ip) != 0) {
                    if ( connection_name.empty() ) {
                        BOOST_LOG_TRIVIAL(info) << "MachineObject IP changed from " << obj->get_dev_ip()
                                                << " to " << dev_ip;
                        obj->set_dev_ip(dev_ip);
                    }
                    else {
                        if ( obj->dev_connection_name.empty() || obj->dev_connection_name.compare(connection_name) == 0) {
                            BOOST_LOG_TRIVIAL(info) << "MachineObject IP changed from " << obj->get_dev_ip()
                                                    << " to " << dev_ip << " connection_name is " << connection_name;
                            if(obj->dev_connection_name.empty()){obj->dev_connection_name = connection_name;}
                            obj->set_dev_ip(dev_ip);
                        }
                    }
                    /* ip changed reconnect mqtt */
                }

                if (obj->wifi_signal != printer_signal ||
                    obj->dev_connection_type != connect_type ||
                    obj->bind_state != bind_state ||
                    obj->bind_sec_link != sec_link ||
                    obj->bind_ssdp_version != ssdp_version ||
                    obj->printer_type != _parse_printer_type(printer_type_str))
                {
                    if (obj->dev_connection_type != connect_type ||
                        obj->bind_state != bind_state ||
                        obj->bind_sec_link != sec_link ||
                        obj->bind_ssdp_version != ssdp_version ||
                        obj->printer_type != _parse_printer_type(printer_type_str))
                    {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " UpdateLocalMachineInfo"
                            << ", dev_id= " << dev_id
                            << ", ip = " << dev_ip
                            << ", printer_name= " << dev_name
                            << ", con_type= " << connect_type << ", signal= " << printer_signal
                            << ", bind_state= " << bind_state;
                    }
                    else
                    {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " UpdateLocalMachineInfo_WIFI"
                            << ", dev_id= " << dev_id
                            << ", ip = "  << dev_ip
                            << ", printer_name= " << dev_name
                            << ", con_type= " << connect_type << ", signal= " << printer_signal
                            << ", bind_state= " << bind_state;
                    }

                    obj->wifi_signal         = printer_signal;
                    obj->dev_connection_type = connect_type;
                    obj->bind_state          = bind_state;
                    obj->bind_sec_link       = sec_link;
                    obj->bind_ssdp_version   = ssdp_version;
                    obj->printer_type        = _parse_printer_type(printer_type_str);
                }

                // U0 firmware
                if (obj->dev_connection_type.empty() && obj->bind_state.empty())
                    obj->bind_state = "free";

                obj->last_alive = Slic3r::Utils::get_current_time_utc();
                obj->m_is_online = true;
                obj->set_dev_name(dev_name);
                /* if (!obj->dev_ip.empty()) {
                Slic3r::GUI::wxGetApp().app_config->set_str("ip_address", obj->dev_id, obj->dev_ip);
                Slic3r::GUI::wxGetApp().app_config->save();
                }*/
            }
            else {
                /* insert a new machine */
                obj = new MachineObject(this, m_agent, dev_name, dev_id, dev_ip);
                obj->printer_type = _parse_printer_type(printer_type_str);
                obj->wifi_signal = printer_signal;
                obj->dev_connection_type = connect_type;
                obj->bind_state     = bind_state;
                obj->bind_sec_link  = sec_link;
                obj->dev_connection_name = connection_name;
                obj->bind_ssdp_version = ssdp_version;
                obj->m_is_online = true;

                //load access code
                AppConfig* config = Slic3r::GUI::wxGetApp().app_config;
                if (config) {
                    obj->set_access_code(Slic3r::GUI::wxGetApp().app_config->get("access_code", dev_id), false);
                    obj->set_user_access_code(Slic3r::GUI::wxGetApp().app_config->get("user_access_code", dev_id), false);
                }
                localMachineList.insert(std::make_pair(dev_id, obj));

                /* if (!obj->dev_ip.empty()) {
                Slic3r::GUI::wxGetApp().app_config->set_str("ip_address", obj->dev_id, obj->dev_ip);
                Slic3r::GUI::wxGetApp().app_config->save();
                }*/
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " New Machine, dev_id= " << dev_id
                    << ", ip = " << dev_ip <<", printer_name = " << dev_name
                    << ", con_type= " << connect_type <<", signal= " << printer_signal << ", bind_state= " << bind_state;
            }
            update_local_machine(*obj);
        }
        catch (...) {
            ;
        }
    }

    MachineObject* DeviceManager::insert_local_device(const BBLocalMachine& machine,
        std::string connection_type, std::string bind_state,
        std::string version, std::string access_code)
    {
        MachineObject* obj;
        auto           it = localMachineList.find(machine.dev_id);
        if (it != localMachineList.end()) {
            obj = it->second;
        } else {
            obj = new MachineObject(this, m_agent, machine.dev_name, machine.dev_id, machine.dev_ip);
            localMachineList.insert(std::make_pair(machine.dev_id, obj));
        }
        if (machine.printer_type.empty())
            obj->printer_type = _parse_printer_type("C11");
        else
            obj->printer_type = _parse_printer_type(machine.printer_type);
        obj->dev_connection_type = connection_type == "farm" ? "lan":connection_type;
        obj->bind_state          = connection_type == "farm" ? "free":bind_state;
        obj->bind_sec_link = "secure";
        obj->bind_ssdp_version = version;
        obj->m_is_online = true;
        obj->last_alive = Slic3r::Utils::get_current_time_utc();
        obj->set_access_code(access_code, false);
        obj->set_user_access_code(access_code, false);

        update_local_machine(*obj);

        return obj;
    }

    int DeviceManager::query_bind_status(std::string& msg)
    {
        if (!m_agent)
        {
            msg = "";
            return -1;
        }

        BOOST_LOG_TRIVIAL(trace) << "DeviceManager::query_bind_status";
        std::map<std::string, MachineObject*>::iterator it;
        std::vector<std::string> query_list;
        for (it = localMachineList.begin(); it != localMachineList.end(); it++)
        {
            query_list.push_back(it->first);
        }

        unsigned int http_code;
        std::string http_body;
        int result = m_agent->query_bind_status(query_list, &http_code, &http_body);

        if (result < 0)
        {
            msg = (boost::format("code=%1%,body=%2") % http_code % http_body).str();
        }
        else
        {
            msg = "";
            try
            {
                json j = json::parse(http_body);
                if (j.contains("bind_list"))
                {

                    for (auto& item : j["bind_list"])
                    {
                        auto it = localMachineList.find(item["dev_id"].get<std::string>());
                        if (it != localMachineList.end())
                        {
                            if (!item["user_id"].is_null())
                                it->second->bind_user_id = item["user_id"].get<std::string>();
                            if (!item["user_name"].is_null())
                                it->second->bind_user_name = item["user_name"].get<std::string>();
                            else
                                it->second->bind_user_name = "Free";
                        }
                    }
                }
            }
            catch (...)
            {
                ;
            }
        }
        return result;
    }

    MachineObject* DeviceManager::get_user_machine(std::string dev_id)
    {
        if (!m_agent || !m_agent->is_user_login())
        {
            return nullptr;
        }

        std::map<std::string, MachineObject*>::iterator it = userMachineList.find(dev_id);
        if (it == userMachineList.end()) return nullptr;
        return it->second;
    }

    MachineObject* DeviceManager::get_my_machine(std::string dev_id)
    {
        auto list = get_my_machine_list();
        auto it = list.find(dev_id);
        if (it != list.end())
        {
            return it->second;
        }
        return nullptr;
    }

    void DeviceManager::clean_user_info()
    {
        BOOST_LOG_TRIVIAL(trace) << "DeviceManager::clean_user_info";
        // reset selected_machine
        selected_machine = "";
        local_selected_machine = "";

        OnSelectedMachineChanged(selected_machine, "");

        // clean user list
        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            if (it->second)
            {
                it->second->set_access_code("");
                delete it->second;
                it->second = nullptr;
            }
        }
        userMachineList.clear();
    }

    bool DeviceManager::set_selected_machine(std::string dev_id)
    {
        BOOST_LOG_TRIVIAL(info) << "set_selected_machine=" << dev_id
            << " cur_selected=" << selected_machine;
        auto my_machine_list = get_my_machine_list();
        auto it = my_machine_list.find(dev_id);

        // disconnect last if dev_id difference from previous one
        auto last_selected = my_machine_list.find(selected_machine);
        if (last_selected != my_machine_list.end() && selected_machine != dev_id)
        {
            if (last_selected->second->connection_type() == "lan")
            {
                m_agent->disconnect_printer();
            }
            else if (last_selected->second->connection_type() == "cloud") {
                m_agent->set_user_selected_machine("");
            }
        }

        // connect curr
        if (it != my_machine_list.end())
        {
            if (selected_machine == dev_id)
            {
                // same dev_id, cloud => reset update time
                if (it->second->connection_type() != "lan")
                {
                    BOOST_LOG_TRIVIAL(info) << "set_selected_machine: same cloud machine, dev_id =" << dev_id
                        << ", just reset update time";

                    // only reset update time
                    it->second->reset_update_time();

                    // check subscribe state
                    Slic3r::GUI::wxGetApp().on_start_subscribe_again(dev_id);

                    return true;
                }
                // same dev_id, lan => disconnect and reconnect
                else
                {
                    BOOST_LOG_TRIVIAL(info) << "set_selected_machine: same lan machine, dev_id =" << dev_id
                        << ", disconnect and reconnect";

                    // lan mode printer reconnect printer
                    if (m_agent)
                    {
                        m_agent->disconnect_printer();
                        it->second->reset();

#if !BBL_RELEASE_TO_PUBLIC
                        it->second->connect(Slic3r::GUI::wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true" ? true : false);
#else
                        it->second->connect(it->second->local_use_ssl_for_mqtt);
#endif
                        it->second->set_lan_mode_connection_state(true);
                    }
                }
            }
            else
            {
                if (m_agent)
                {
                    if (it->second->connection_type() != "lan" || it->second->connection_type().empty())
                    {
                        // diff dev_id, cloud => set_user_selected_machine(new)
                        BOOST_LOG_TRIVIAL(info) << "set_selected_machine: select new cloud machine, dev_id =" << dev_id;
                        m_agent->set_user_selected_machine(dev_id);
                        it->second->reset();
                    }
                    else
                    {
                        BOOST_LOG_TRIVIAL(info) << "set_selected_machine: select new lan machine, dev_id =" << dev_id;
                        it->second->reset();
#if !BBL_RELEASE_TO_PUBLIC
                        it->second->connect(Slic3r::GUI::wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true" ? true : false);
#else
                        it->second->connect(it->second->local_use_ssl_for_mqtt);
#endif
                        it->second->set_lan_mode_connection_state(true);
                    }
                }
            }
            for (auto& data : it->second->m_nozzle_filament_data)
            {
                data.second.checked_filament.clear();
            }
        }

        if (selected_machine != dev_id) {
            OnSelectedMachineChanged(selected_machine, dev_id);
        }

        selected_machine = dev_id;
        return true;
    }

    MachineObject* DeviceManager::get_selected_machine()
    {
        if (selected_machine.empty()) return nullptr;

        MachineObject* obj = get_user_machine(selected_machine);
        if (obj)
            return obj;

        // return local machine has access code
        auto it = localMachineList.find(selected_machine);
        if (it != localMachineList.end())
        {
            if (it->second->has_access_right())
                return it->second;
        }
        return nullptr;
    }

    void DeviceManager::add_user_subscribe()
    {
        /* user machine */
        std::vector<std::string> dev_list;
        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            dev_list.push_back(it->first);
            BOOST_LOG_TRIVIAL(trace) << "add_user_subscribe: " << it->first;
        }
        m_agent->add_subscribe(dev_list);
    }


    void DeviceManager::del_user_subscribe()
    {
        /* user machine */
        std::vector<std::string> dev_list;
        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            dev_list.push_back(it->first);
            BOOST_LOG_TRIVIAL(trace) << "del_user_subscribe: " << it->first;
        }
        m_agent->del_subscribe(dev_list);
    }

    void DeviceManager::subscribe_device_list(std::vector<std::string> dev_list)
    {
        std::vector<std::string> unsub_list;
        subscribe_list_cache.clear();
        for (auto& it : subscribe_list_cache)
        {
            if (it != selected_machine)
            {
                unsub_list.push_back(it);
                BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: unsub dev id = " << it;
            }
        }
        BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: unsub_list size = " << unsub_list.size();

        if (!selected_machine.empty())
        {
            subscribe_list_cache.push_back(selected_machine);
        }
        for (auto& it : dev_list)
        {
            subscribe_list_cache.push_back(it);
            BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: sub dev id = " << it;
        }
        BOOST_LOG_TRIVIAL(trace) << "subscribe_device_list: sub_list size = " << subscribe_list_cache.size();
        if (!unsub_list.empty())
            m_agent->del_subscribe(unsub_list);
        if (!dev_list.empty())
            m_agent->add_subscribe(subscribe_list_cache);
    }

    std::map<std::string, MachineObject*> DeviceManager::get_my_machine_list()
    {
        std::map<std::string, MachineObject*> result;

        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            if (it->second && !it->second->is_lan_mode_printer())
            {
                result.insert(std::make_pair(it->first, it->second));
            }
        }

        for (auto it = localMachineList.begin(); it != localMachineList.end(); it++)
        {
            if (it->second && it->second->has_access_right() && it->second->is_avaliable() && it->second->is_lan_mode_printer())
            {
                // remove redundant in userMachineList
                if (result.find(it->first) == result.end())
                {
                    result.emplace(std::make_pair(it->first, it->second));
                }
            }
        }
        return result;
    }

    std::map<std::string, MachineObject*> DeviceManager::get_my_cloud_machine_list()
    {
        std::map<std::string, MachineObject*> result;
        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            if (it->second && !it->second->is_lan_mode_printer()) { result.emplace(*it); }
        }
        return result;
    }

    std::string DeviceManager::get_first_online_user_machine() const
    {
        for (auto it = userMachineList.begin(); it != userMachineList.end(); it++)
        {
            if (it->second && it->second->is_online())
            {
                return it->second->get_dev_id();
            }
        }
        return "";
    }

    void DeviceManager::modify_device_name(std::string dev_id, std::string dev_name)
    {
        BOOST_LOG_TRIVIAL(trace) << "modify_device_name";
        if (m_agent)
        {
            int result = m_agent->modify_printer_name(dev_id, dev_name);
            if (result == 0)
            {
                update_user_machine_list_info();
            }
        }
    }

    void DeviceManager::parse_user_print_info(std::string body)
    {
        BOOST_LOG_TRIVIAL(trace) << "DeviceManager::parse_user_print_info";
        std::lock_guard<std::mutex> lock(listMutex);

        if (device_subseries.size() <= 0) {
            device_subseries = DevPrinterConfigUtil::get_all_subseries();
            if (device_subseries.size() <= 0) {
                device_subseries.insert(std::pair<std::string, std::vector<std::string>>("", std::vector<std::string>()));
            }
        }

        std::set<std::string> new_list;
        try
        {
            json j = json::parse(body);

#if !BBL_RELEASE_TO_PUBLIC
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": " << j;
#endif

            if (j.contains("devices") && !j["devices"].is_null())
            {
                for (auto& elem : j["devices"])
                {
                    MachineObject* obj = nullptr;
                    std::string dev_id;
                    if (!elem["dev_id"].is_null())
                    {
                        dev_id = elem["dev_id"].get<std::string>();
                        new_list.insert(dev_id);
                    }
                    std::map<std::string, MachineObject*>::iterator iter = userMachineList.find(dev_id);
                    if (iter != userMachineList.end())
                    {
                        /* update field */
                        obj = iter->second;
                        obj->set_dev_id(dev_id);
                    }
                    else
                    {
                        obj = new MachineObject(this, m_agent, "", "", "");
                        if (m_agent)
                        {
                            obj->set_bind_status(m_agent->get_user_name());
                        }

                        if (obj->get_dev_ip().empty())
                        {
                            obj->get_dev_ip() = Slic3r::GUI::wxGetApp().app_config->get("ip_address", dev_id);
                        }
                        userMachineList.insert(std::make_pair(dev_id, obj));
                    }

                    if (!obj) continue;

                    if (!elem["dev_id"].is_null())
                        obj->set_dev_id(elem["dev_id"].get<std::string>());
                    if (!elem["dev_name"].is_null())
                        obj->set_dev_name(elem["dev_name"].get<std::string>());
                    if (!elem["dev_online"].is_null())
                        obj->m_is_online = elem["dev_online"].get<bool>();
                    if (elem.contains("dev_model_name") && !elem["dev_model_name"].is_null()) {
                        auto printer_type = elem["dev_model_name"].get<std::string>();
                        for (const std::pair<std::string, std::vector<std::string>> &pair : device_subseries) {
                            auto it = std::find(pair.second.begin(), pair.second.end(), printer_type);
                            if (it != pair.second.end())
                            {
                                obj->printer_type = Slic3r::_parse_printer_type(pair.first);
                                break;
                            }
                            else
                            {
                                obj->printer_type = Slic3r::_parse_printer_type(printer_type);
                            }
                        }
                    }
                    if (!elem["task_status"].is_null())
                        obj->iot_print_status = elem["task_status"].get<std::string>();
                    if (elem.contains("dev_product_name") && !elem["dev_product_name"].is_null())
                        obj->dev_product_name = elem["dev_product_name"].get<std::string>();
                    if (elem.contains("dev_access_code") && !elem["dev_access_code"].is_null())
                    {
                        std::string acc_code = elem["dev_access_code"].get<std::string>();
                        acc_code.erase(std::remove(acc_code.begin(), acc_code.end(), '\n'), acc_code.end());
                        obj->set_access_code(acc_code);
                    }
                }

                //remove MachineObject from userMachineList
                std::map<std::string, MachineObject*>::iterator iterat;
                for (iterat = userMachineList.begin(); iterat != userMachineList.end(); )
                {
                    if (new_list.find(iterat->first) == new_list.end())
                    {
                        iterat = userMachineList.erase(iterat);
                    }
                    else
                    {
                        iterat++;
                    }
                }
            }
        }
        catch (std::exception& e)
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception=" << e.what();
        }
    }

    void DeviceManager::update_user_machine_list_info()
    {
        if (!m_agent) return;

        BOOST_LOG_TRIVIAL(debug) << "update_user_machine_list_info";
        unsigned int http_code;
        std::string body;
        int result = m_agent->get_user_print_info(&http_code, &body);
        if (result == 0)
        {
            parse_user_print_info(body);
        }
    }

    void DeviceManager::load_last_machine()
    {
        // Get all available machines, include cloud machines and lan machines that have access right
        auto all_machines = get_my_machine_list();
        if (all_machines.empty())
            return;
        
        // Then connect to the machine we last selected if available
        const std::string last_monitor_machine = m_agent ? m_agent->get_user_selected_machine() : "";
        const auto        last_machine         = all_machines.find(last_monitor_machine);
        if (last_machine != all_machines.end()) {
            this->set_selected_machine(last_machine->second->get_dev_id());
        } else {
            // If not, then select the first available one
            this->set_selected_machine(all_machines.begin()->second->get_dev_id());
        }
    }

    void DeviceManager::OnMachineBindStateChanged(MachineObject* obj, const std::string& new_state)
    {
        if (!obj) { return; }
        if (obj->get_dev_id() == selected_machine)
        {
            if (new_state == "free") { OnSelectedMachineLost(); }
        }
    }

    void DeviceManager::OnSelectedMachineLost()
    {
        GUI::wxGetApp().sidebar().update_sync_status(nullptr);
        GUI::wxGetApp().sidebar().load_ams_list(nullptr);
    }

    void DeviceManager::OnSelectedMachineChanged(const std::string& /*pre_dev_id*/,
                                                 const std::string& /*new_dev_id*/)
    {
        if (MachineObject* obj_ = get_selected_machine()) {
            GUI::wxGetApp().sidebar().update_sync_status(obj_);
            GUI::wxGetApp().sidebar().load_ams_list(obj_);
        };
    }

    void DeviceManager::reload_printer_settings()
    {
        for (auto obj : this->userMachineList) { obj.second->reload_printer_settings(); };
    }


    DeviceManagerRefresher::DeviceManagerRefresher(DeviceManager* manger) : wxObject()
    {
        m_manager = manger;
        m_timer = new wxTimer();
        m_timer->Bind(wxEVT_TIMER, &DeviceManagerRefresher::on_timer, this);
    }

    DeviceManagerRefresher::~DeviceManagerRefresher()
    {
        m_timer->Stop();
        delete m_timer;
    }

    void DeviceManagerRefresher::on_timer(wxTimerEvent& event)
    {
        if (!m_manager) { return; }

        NetworkAgent* agent = m_manager->get_agent();
        if (!agent) { return; }

        // reset to active
        Slic3r::GUI::wxGetApp().reset_to_active();

        MachineObject* obj = m_manager->get_selected_machine();
        if (!obj) { return; }

        // check valid machine
        if (obj && m_manager->get_my_machine(obj->get_dev_id()) == nullptr)
        {
            m_manager->set_selected_machine("");
            agent->set_user_selected_machine("");
            return;
        }

        // do some refresh
        if (Slic3r::GUI::wxGetApp().is_user_login())
        {
            m_manager->check_pushing();
            try
            {
                agent->refresh_connection();
            }
            catch (...)
            {
                ;
            }
        }

        // certificate
        agent->install_device_cert(obj->get_dev_id(), obj->is_lan_mode_printer());
    }
}