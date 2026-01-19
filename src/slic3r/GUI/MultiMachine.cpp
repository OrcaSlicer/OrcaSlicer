#include "MultiMachine.hpp"
#include "I18N.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"

namespace Slic3r {
namespace GUI {


wxDEFINE_EVENT(EVT_MULTI_CLOUD_TASK_SELECTED, wxCommandEvent);
wxDEFINE_EVENT(EVT_MULTI_LOCAL_TASK_SELECTED, wxCommandEvent);
wxDEFINE_EVENT(EVT_MULTI_DEVICE_SELECTED, wxCommandEvent);
wxDEFINE_EVENT(EVT_MULTI_DEVICE_SELECTED_FINHSH, wxCommandEvent);
wxDEFINE_EVENT(EVT_MULTI_DEVICE_VIEW, wxCommandEvent);
wxDEFINE_EVENT(EVT_MULTI_REFRESH, wxCommandEvent);

DeviceItem::DeviceItem(wxWindow* parent,  MachineObject* obj)
    : wxWindow(parent, wxID_ANY)
    , obj_(obj)
{
    sync_state();
    Bind(EVT_MULTI_REFRESH, &DeviceItem::on_refresh, this);
}

void DeviceItem::on_refresh(wxCommandEvent& evt)
{
    Refresh();
}

void DeviceItem::sync_state()
{
    if (obj_) {
        state_online = obj_->is_online();
        state_dev_name = obj_->get_dev_name();

        //printable
        if (obj_->print_status == "IDLE") {
            state_printable = 0;
        }
        else if (obj_->print_status == "FINISH") {
            state_printable = 1;
        }
        else if (obj_->print_status == "FAILED") {
            state_printable = 2;
        }
        else if (obj_->is_in_printing()) {
            state_printable = 3;
        }
        else {
            state_printable = 6;
        }

        if (is_blocking_printing(obj_)) {
            state_printable = 5;
        }

        if (obj_->is_in_upgrading()) {
            state_printable = 4;
        }

        state_enable_ams = obj_->ams_exist_bits;


        //device
        if (obj_->print_status == "IDLE") {
            state_device = 0;
        }
        else if (obj_->print_status == "FINISH") {
            state_device = 1;
        }
        else if (obj_->print_status == "FAILED") {
            state_device = 2;
        }
        else if (obj_->print_status == "RUNNING") {
            state_device = 3;
        }
        else if (obj_->print_status == "PAUSE") {
            state_device = 4;
        }
        else if (obj_->print_status == "PREPARE") {
            state_device = 5;
        }
        else if (obj_->print_status == "SLICING") {
            state_device = 6;
        }
        else {
            state_device = 7;
        }
    }
}

void DeviceItem::selected()
{
    if (state_selected != 2) {
        state_selected = 1;
    }
}

void DeviceItem::unselected()
{
    if (state_selected != 2) {
        state_selected = 0;
    }
}

bool DeviceItem::is_blocking_printing(MachineObject* obj_)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;
    auto target_model = obj_->printer_type;
    std::string source_model = "";

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    source_model = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    if (source_model != target_model) {
        std::vector<std::string> compatible_machine = obj_->get_compatible_machine();
        vector<std::string>::iterator it = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) {
            return true;
        }
    }

    return false;
}

void DeviceItem::update_item(const DeviceItem* item)
{
    // Except for the selected status, everything else is updated
    if (this == item)
        return;
    this->state_online = item->state_online;
    this->state_printable = item->state_printable;
    this->state_enable_ams = item->state_enable_ams;
    this->state_device = item->state_device;
    this->state_local_task = item->state_local_task;
}

wxString DeviceItem::get_state_printable()
{
    //0-idle 1-finish 2-printing 3-upgrading 4-preset incompatible  5-unknown
    std::vector<wxString> str_state_printable;
    str_state_printable.push_back(_L("Idle"));
    str_state_printable.push_back(_L("Idle"));
    str_state_printable.push_back(_L("Idle"));
    str_state_printable.push_back(_L("Printing"));
    str_state_printable.push_back(_L("Upgrading"));
    str_state_printable.push_back(_L("Incompatible"));
    str_state_printable.push_back(_L("syncing"));

    return str_state_printable[state_printable];
}

wxString DeviceItem::get_state_device()
{
    //0-idle 1-finish 2-running 3-pause 4-failed  5-prepare 
    std::vector<wxString> str_state_device;
    str_state_device.push_back(_L("Idle"));
    str_state_device.push_back(_L("Printing Finish"));
    str_state_device.push_back(_L("Printing Failed"));
    str_state_device.push_back(_L("Printing"));
    str_state_device.push_back(_L("Printing Pause"));
    str_state_device.push_back(_L("Prepare"));
    str_state_device.push_back(_L("Slicing"));
    str_state_device.push_back(_L("syncing"));

    return str_state_device[state_device];
}

wxString DeviceItem::get_local_state_task()
{
    //0-padding  1-sending 2-sending finish  3-sending cancel  4-sending failed 5-Removed
    std::vector<wxString> str_state_task;
    str_state_task.push_back(_L("Pending"));
    str_state_task.push_back(_L("Sending"));
    str_state_task.push_back(_L("Sending Finish"));
    str_state_task.push_back(_L("Sending Cancel"));
    str_state_task.push_back(_L("Sending Failed"));
    str_state_task.push_back(_L("Printing"));
    str_state_task.push_back(_L("Print Success"));
    str_state_task.push_back(_L("Print Failed"));
    str_state_task.push_back(_L("Removed"));
    str_state_task.push_back(_L("Idle"));   
    return str_state_task[state_local_task];
}

wxString DeviceItem::get_cloud_state_task()
{
    //0-printing 1-printing finish 2-printing failed
    std::vector<wxString> str_state_task;
    str_state_task.push_back(_L("Printing"));
    str_state_task.push_back(_L("Printing Finish"));
    str_state_task.push_back(_L("Printing Failed"));

    return str_state_task[state_cloud_task];
}


std::vector<DeviceItem*> selected_machines(const std::vector<DeviceItem*>& dev_item_list, std::string search_text)
{
    std::vector<DeviceItem*> res;
    for (const auto& item : dev_item_list) {
        const MachineObject* dev = item->get_obj();
        const std::string& dev_name = dev->get_dev_name();
        const std::string& dev_ip = dev->get_dev_ip();

        auto name_it = dev_name.find(search_text);
        auto ip_it = dev_ip.find(search_text);

        if (name_it != std::string::npos || ip_it != std::string::npos)
            res.emplace_back(item);
    }

    return res;
}

SortItem::SortItem()
{
    sort_map.emplace(std::make_pair(SortRule::SR_None, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return d1->state_dev_name > d2->state_dev_name;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_DEV_NAME, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_dev_name > d2->state_dev_name : d1->state_dev_name < d2->state_dev_name;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_ONLINE, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_online > d2->state_online : d1->state_online < d2->state_online;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_PRINTABLE, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_printable > d2->state_printable : d1->state_printable < d2->state_printable;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_EN_AMS, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_enable_ams > d2->state_enable_ams : d1->state_enable_ams < d2->state_enable_ams;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_DEV_STATE, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_device > d2->state_device : d1->state_device < d2->state_device;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_LOCAL_TASK_STATE, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_local_task > d2->state_local_task : d1->state_local_task < d2->state_local_task;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_CLOUD_TASK_STATE, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->state_cloud_task > d2->state_cloud_task : d1->state_cloud_task < d2->state_cloud_task;
    }));
    sort_map.emplace(std::make_pair(SortRule::SR_SEND_TIME, [this](const DeviceItem* d1, const DeviceItem* d2) {
        return this->big ? d1->m_send_time > d2->m_send_time : d1->m_send_time < d2->m_send_time;
    }));
}

SortItem::SortCallBack SortItem::get_call_back()
{
    return sort_map[rule];
}

void SortItem::set_role(SortRule rule, bool big)
{
    this->rule = rule;
    this->big = big;
}

void SortItem::set_role(SortMultiMachineCB cb, SortRule rl, bool big)
{
    this->cb = cb;
    this->rule = rl;
    this->big = big;
}

} // namespace GUI
} // namespace Slic3r
