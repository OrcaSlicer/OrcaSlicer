#include "SendMultiMachinePage.hpp"
#include "TaskManager.hpp"
#include "I18N.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/RadioBox.hpp"
#include <wx/listimpl.cpp>

#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevStorage.h"

namespace Slic3r {
namespace GUI {

#define MATERIAL_ITEM_SIZE wxSize(FromDIP(64), FromDIP(34))
#define MATERIAL_ITEM_REAL_SIZE wxSize(FromDIP(62), FromDIP(32))
#define MAPPING_ITEM_REAL_SIZE wxSize(FromDIP(48), FromDIP(45))
WX_DEFINE_LIST(AmsRadioSelectorList);

class ScrolledWindow : public wxScrolledWindow {
public:
    ScrolledWindow(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxVSCROLL) : wxScrolledWindow(parent, id, pos, size, style) {}

    bool ShouldScrollToChildOnFocus(wxWindow* child) override { return false; }
};

SendDeviceItem::SendDeviceItem(wxWindow* parent,  MachineObject* obj)
    : DeviceItem(parent, obj)
{
    SetBackgroundColour(*wxWHITE);
    m_bitmap_check_disable = ScalableBitmap(this, "check_off_disabled", 18);
    m_bitmap_check_off = ScalableBitmap(this, "check_off_focused", 18);
    m_bitmap_check_on = ScalableBitmap(this, "check_on", 18);


    SetMinSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    SetMaxSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), FromDIP(SEND_ITEM_MAX_HEIGHT)));

    Bind(wxEVT_PAINT, &SendDeviceItem::paintEvent, this);
    Bind(wxEVT_ENTER_WINDOW, &SendDeviceItem::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &SendDeviceItem::OnLeaveWindow, this);
    Bind(wxEVT_LEFT_DOWN, &SendDeviceItem::OnLeftDown, this);
    Bind(wxEVT_MOTION, &SendDeviceItem::OnMove, this);
    Bind(EVT_MULTI_DEVICE_SELECTED, &SendDeviceItem::OnSelectedDevice, this);
    wxGetApp().UpdateDarkUIWin(this);
}

void SendDeviceItem::DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top /*= 0*/)
{
    wxSize size = GetSize();
    wxFont font = dc.GetFont();

    wxSize textSize = dc.GetTextExtent(text);
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(50, 58, 61)));
    int textWidth = textSize.GetWidth();

    if (textWidth > maxWidth) {
        wxString truncatedText = text;
        int ellipsisWidth = dc.GetTextExtent("...").GetWidth();
        int numChars = text.length();

        for (int i = numChars - 1; i >= 0; --i) {
            truncatedText = text.substr(0, i) + "...";
            int truncatedWidth = dc.GetTextExtent(truncatedText).GetWidth();

            if (truncatedWidth <= maxWidth - ellipsisWidth) {
                break;
            }
        }

        if (top == 0) {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2 - top);
        }

    }
    else {
        if (top == 0) {
            dc.DrawText(text, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(text, left, (size.y - textSize.y) / 2 - top);
        }
    }
}

void SendDeviceItem::OnEnterWindow(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh(false);
}

void SendDeviceItem::OnLeaveWindow(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh(false);
}

void SendDeviceItem::OnSelectedDevice(wxCommandEvent& evt)
{
    auto dev_id = evt.GetString();
    auto state = evt.GetInt();
    if (state == 0) {
        state_selected = 1;
    }
    else if (state == 1) {
        state_selected = 0;
    }
    Refresh(false);
}

void SendDeviceItem::OnLeftDown(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {

        if (state_printable <= 2 && state_local_task > 1) {
             post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
        }
    }
}

void SendDeviceItem::OnMove(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {
        SetCursor(wxCURSOR_HAND);
    }
    else {
        SetCursor(wxCURSOR_ARROW);
    }
}

void SendDeviceItem::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void SendDeviceItem::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void SendDeviceItem::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    dc.SetPen(wxPen(*wxBLACK));

    int left = FromDIP(SEND_LEFT_PADDING_LEFT);


    //checkbox
    if (state_printable > 2) {
        dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2 ));
    }
    else {
        if (state_selected == 0) {
            dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2 ));
        }
        else if(state_selected == 1) {
            dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2 ));
        }
    }

    //task status
    if (state_local_task <= 1) {
        dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2 ));
    }

    left += FromDIP(SEND_LEFT_PRINTABLE);

    //dev names
    DrawTextWithEllipsis(dc, wxString::FromUTF8(get_obj()->get_dev_name()),  FromDIP(SEND_LEFT_DEV_NAME), left);
    left += FromDIP(SEND_LEFT_DEV_NAME);

    //device state
    if (state_printable <= 2) {
        dc.SetTextForeground(wxColour(0, 150, 136));
    }
    else {
        dc.SetTextForeground(wxColour(208, 27, 27));
    }

    DrawTextWithEllipsis(dc, get_state_printable(), FromDIP(SEND_LEFT_DEV_NAME), left);
    left += FromDIP(SEND_LEFT_DEV_STATUS);

    dc.SetTextForeground(*wxBLACK);

    //task state
    //DrawTextWithEllipsis(dc, get_local_state_task(), FromDIP(SEND_LEFT_DEV_NAME), left);
    //left += FromDIP(SEND_LEFT_DEV_STATUS);


    //AMS
    if (!obj_->HasAms()) {
        DrawTextWithEllipsis(dc, _L("No AMS"), FromDIP(SEND_LEFT_DEV_NAME), left);
    }
    else {
        DrawTextWithEllipsis(dc, _L("AMS"), FromDIP(SEND_LEFT_DEV_NAME), left);
    }

    if (m_hover) {
        dc.SetPen(wxPen(wxColour(0, 150, 136)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, 3);
    }
}
void SendDeviceItem::post_event(wxCommandEvent&& event)
{
    event.SetEventObject(this);
    event.SetString(obj_->get_dev_id());
    event.SetInt(state_selected);
    wxPostEvent(this, event);
}

void SendDeviceItem::DoSetSize(int x, int y, int width, int height, int sizeFlags /*= wxSIZE_AUTO*/)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}

SendMultiMachinePage::SendMultiMachinePage(Plater* plater)
	: DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
		_L("Send to Multi-device"),
		wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    ,m_plater(plater)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    app_config = get_app_config();

    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(line_top, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(FromDIP(10));

    m_main_scroll = new ScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_main_scroll->SetBackgroundColour(*wxWHITE);
    m_main_scroll->SetScrollRate(5, 5);

    m_sizer_body = new wxBoxSizer(wxVERTICAL);
    m_main_page = create_page();
    m_sizer_body->Add(m_main_page, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
    m_main_scroll->SetSizerAndFit(m_sizer_body);
    m_main_scroll->Layout();
    m_main_scroll->Fit();
    m_main_scroll->Centre(wxBOTH);

    main_sizer->Add(m_main_scroll, 1, wxEXPAND);

    SetSizer(main_sizer);
    Layout();
    Fit();
    Centre(wxBOTH);

    m_mapping_popup = new AmsMapingPopup(m_main_page);
    Bind(EVT_SET_FINISH_MAPPING, &SendMultiMachinePage::on_set_finish_mapping, this);
    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this); e.Skip(); });
    m_main_page->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this); e.Skip(); });
    m_main_scroll->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this); e.Skip(); });

    init_timer();
    Bind(wxEVT_TIMER, &SendMultiMachinePage::on_timer, this);
    wxGetApp().UpdateDlgDarkUI(this);
}

SendMultiMachinePage::~SendMultiMachinePage()
{
	// TODO
    m_radio_group.DeleteContents(true);

    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;
}

void SendMultiMachinePage::prepare(int plate_idx)
{
	// TODO
    m_print_plate_idx = plate_idx;
}

void SendMultiMachinePage::on_dpi_changed(const wxRect& suggested_rect)
{
    m_select_checkbox->Rescale();
    m_printer_name->Rescale();
    m_printer_name->SetMinSize(wxSize(FromDIP(SEND_LEFT_DEV_NAME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(SEND_LEFT_DEV_NAME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_device_status->Rescale();
    m_device_status->SetMinSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_device_status->SetMaxSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_ams->Rescale();
    m_ams->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_ams->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_refresh_button->Rescale();
    m_refresh_button->SetMinSize(wxSize(FromDIP(50), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_refresh_button->SetMaxSize(wxSize(FromDIP(50), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_rename_button->msw_rescale();
    print_time->msw_rescale();
    print_weight->msw_rescale();
    timeimg->SetBitmap(print_time->bmp());
    weightimg->SetBitmap(print_weight->bmp());
    m_button_add->Rescale(); // ORCA no need to re set size
    m_button_send->Rescale(); // ORCA no need to re set size

    for (auto it = m_device_items.begin(); it != m_device_items.end(); ++it) {
        it->second->Refresh();
    }

    if (m_mapping_popup) { m_mapping_popup->msw_rescale();}

    Fit();
    Layout();
    Refresh();
}

void SendMultiMachinePage::on_sys_color_changed()
{

}

void SendMultiMachinePage::refresh_user_device()
{
    sizer_machine_list->Clear(false);
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        return;
    }

    auto all_machine = dev->get_my_cloud_machine_list();
    auto user_machine = std::map<std::string, MachineObject*>();

    //selected machine
    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        auto dev_id = app_config->get("multi_devices", std::to_string(i));

        if (all_machine.count(dev_id) > 0) {
            user_machine[dev_id] = all_machine[dev_id];
        }
    }


    auto task_manager = wxGetApp().getTaskManager();

    std::vector<std::string> subscribe_list;
    std::vector<SendDeviceItem*> dev_temp;

    for (auto it = user_machine.begin(); it != user_machine.end(); ++it) {
        SendDeviceItem* di = new SendDeviceItem(scroll_macine_list, it->second);
        if (m_device_items.find(it->first) != m_device_items.end()) {
            auto item = m_device_items[it->first];
            if (item->state_selected == 1 && di->state_printable <= 2)
                di->state_selected = item->state_selected;
            item->Destroy();
        }
        m_device_items[it->first] = di;

        //update state
        if (task_manager) {
            m_device_items[it->first]->state_local_task = task_manager->query_task_state(it->first);
        }

        dev_temp.push_back(m_device_items[it->first]);
        subscribe_list.push_back(it->first);
    }

    dev->subscribe_device_list(subscribe_list);

    if (m_sort.rule == SortItem::SortRule::SR_None) {
        this->device_printable_big = false;
        m_sort.set_role(SortItem::SR_DEV_STATE, device_printable_big);
    }
    std::sort(dev_temp.begin(), dev_temp.end(), m_sort.get_call_back());

    for (auto i = 0; i < dev_temp.size(); ++i) {
        sizer_machine_list->Add(dev_temp[i], 0, wxALL | wxEXPAND, 0);
    }

    // maintenance dev_items
    auto it = m_device_items.begin();
    while (it != m_device_items.end()) {
        if (user_machine.find(it->first) != user_machine.end()) {
            ++it;
        }
        else {
            it->second->Destroy();
            it = m_device_items.erase(it);
        }
    }
    m_tip_text->Show(m_device_items.empty());
    m_button_add->Show(m_device_items.empty());
    sizer_machine_list->Layout();
    Layout();
    Fit();
}

BBL::PrintParams SendMultiMachinePage::request_params(MachineObject* obj)
{
    BBL::PrintParams params;

    //get all setting
    bool bed_leveling = app_config->get("print", "bed_leveling") == "1" ? true : false;
    bool flow_cali = app_config->get("print", "flow_cali") == "1" ? true : false;
    bool timelapse = app_config->get("print", "timelapse") == "1" ? true : false;
    auto use_ams = false;

    AmsRadioSelectorList::Node* node = m_radio_group.GetFirst();
    auto                     groupid = 0;


    while (node) {
        AmsRadioSelector* rs = node->GetData();
        if (rs->m_param_name == "use_ams" && rs->m_radiobox->GetValue()) {
            use_ams = true;
        }

        if (rs->m_param_name == "use_extra" && rs->m_radiobox->GetValue()) {
            use_ams = false;
        }

        node = node->GetNext();
    }

    //use ams


    PrintPrepareData job_data;
    m_plater->get_print_job_data(&job_data);

    std::string temp_file = Slic3r::resources_dir() + "/check_access_code.txt";
    auto check_access_code_path = temp_file.c_str();
    BOOST_LOG_TRIVIAL(trace) << "sned_job: check_access_code_path = " << check_access_code_path;
    job_data._temp_path = fs::path(check_access_code_path);

    int curr_plate_idx;
    if (job_data.plate_idx >= 0)
        curr_plate_idx = job_data.plate_idx + 1;
    else if (job_data.plate_idx == PLATE_CURRENT_IDX)
        curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;
    else if (job_data.plate_idx == PLATE_ALL_IDX)
        curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;
    else
        curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;

    params.dev_ip = obj->get_dev_ip();
    params.dev_id = obj->get_dev_id();
    params.dev_name = obj->get_dev_name();
    params.ftp_folder = obj->get_ftp_folder();
    params.connection_type = obj->connection_type();
    params.print_type = "from_normal";
    params.filename =  job_data._3mf_path.string();
    params.config_filename = job_data._3mf_config_path.string();
    params.plate_index = curr_plate_idx;
    params.task_bed_leveling = bed_leveling;
    params.task_flow_cali = flow_cali;
    params.task_vibration_cali = false;
    params.task_layer_inspect = true;
    params.task_record_timelapse = timelapse;

    if (use_ams) {
        std::string ams_array;
        std::string ams_array2;
        std::string mapping_info;
        get_ams_mapping_result(ams_array, ams_array2, mapping_info);
        params.ams_mapping = ams_array;
        params.ams_mapping2 = ams_array2;
        params.ams_mapping_info = mapping_info;
    }
    else {
        std::string temp;
        std::string ams_array;
        std::string ams_array2;
        std::string mapping_info;

        // change to old version
        for(auto &info : m_ams_mapping_result){
            info.tray_id = VIRTUAL_TRAY_DEPUTY_ID;
            info.ams_id = VIRTUAL_AMS_DEPUTY_ID_STR;
            info.slot_id = "0";
        }
        get_ams_mapping_result(ams_array, temp, mapping_info);

        // change to new version
        for(auto &info : m_ams_mapping_result){
            info.tray_id = VIRTUAL_TRAY_DEPUTY_ID;
            info.ams_id = VIRTUAL_AMS_MAIN_ID_STR;
            info.slot_id = "0";
        }
        temp.clear();
        mapping_info.clear();
        get_ams_mapping_result(temp, ams_array2, mapping_info);

        // restore
        for(auto &info : m_ams_mapping_result){
            info.tray_id = 0;
            info.ams_id = "";
            info.slot_id = "";
        }

        params.ams_mapping = ams_array;
        params.ams_mapping2 = ams_array2;
        params.ams_mapping_info = mapping_info;
    }

    params.connection_type = obj->connection_type();
    params.task_use_ams = use_ams;

    PartPlate* curr_plate = m_plater->get_partplate_list().get_curr_plate();
    if (curr_plate) {
        params.task_bed_type = bed_type_to_gcode_string( curr_plate->get_bed_type(true));
    }

    wxString filename;
    if (m_current_project_name.IsEmpty()) {
        filename = m_plater->get_export_gcode_filename("", true, m_print_plate_idx == PLATE_ALL_IDX ? true : false);
    }
    else {
        filename = m_current_project_name;
    }

    if (m_print_plate_idx == PLATE_ALL_IDX && filename.empty()) {
        filename = _L("Untitled");
    }

    if (filename.empty()) {
        filename = m_plater->get_export_gcode_filename("", true);
        if (filename.empty()) filename = _L("Untitled");
    }

    if (params.preset_name.empty()) { params.preset_name = wxString::Format("%s_plate_%d", filename, m_print_plate_idx).ToStdString(); }
    if (params.project_name.empty()) { params.project_name = filename.ToUTF8(); }



    // check access code and ip address
    if (obj->connection_type() == "lan") {
        /*params.dev_id = m_dev_id;
        params.project_name = "verify_job";
        params.filename = job_data._temp_path.string();
        params.connection_type = this->connection_type;

        result = m_agent->start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr);
        if (result != 0) {
            BOOST_LOG_TRIVIAL(error) << "access code is invalid";
            m_enter_ip_address_fun_fail();
            m_job_finished = true;
            return;
        }

        params.project_name = "";
        params.filename = "";*/
    }
    else {
        if (params.dev_ip.empty())
            params.comments = "no_ip";
        else if (obj->is_support_cloud_print_only)
            params.comments = "low_version";
        else if (obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD)
            params.comments = "no_sdcard";
        else if (params.password.empty())
            params.comments = "no_password";
    }

    return params;
}

bool SendMultiMachinePage::get_ams_mapping_result(std::string &mapping_array_str, std::string &mapping_array_str2, std::string &ams_mapping_info)
{
    if (m_ams_mapping_result.empty()) return false;

    bool valid_mapping_result = true;
    int  invalid_count        = 0;
    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        if (m_ams_mapping_result[i].tray_id == -1) {
            valid_mapping_result = false;
            invalid_count++;
        }
    }

    if (invalid_count == m_ams_mapping_result.size()) {
        return false;
    } else {
        json mapping_v0_json   = json::array();
        json mapping_v1_json   = json::array();
        json mapping_info_json = json::array();

        /* get filament maps */
        std::vector<int> filament_maps;
        Plater *         plater = wxGetApp().plater();
        if (plater) {
            PartPlate *curr_plate = plater->get_partplate_list().get_curr_plate();
            if (curr_plate) {
                filament_maps = curr_plate->get_filament_maps();
            } else {
                BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, curr_plate is nullptr";
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, plater is nullptr";
        }

        for (int i = 0; i < wxGetApp().preset_bundle->filament_presets.size(); i++) {
            int  tray_id = -1;
            json mapping_item_v1;
            mapping_item_v1["ams_id"]  = 0xff;
            mapping_item_v1["slot_id"] = 0xff;
            json mapping_item;
            mapping_item["ams"]          = tray_id;
            mapping_item["targetColor"]  = "";
            mapping_item["filamentId"]   = "";
            mapping_item["filamentType"] = "";
            for (int k = 0; k < m_ams_mapping_result.size(); k++) {
                if (m_ams_mapping_result[k].id == i) {
                    tray_id                      = m_ams_mapping_result[k].tray_id;
                    mapping_item["ams"]          = tray_id;
                    mapping_item["filamentType"] = m_filaments[k].type;
                    if (i >= 0 && i < wxGetApp().preset_bundle->filament_presets.size()) {
                        auto it = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[i]);
                        if (it != nullptr) { mapping_item["filamentId"] = it->filament_id; }
                    }
                    /* nozzle id */
                    mapping_item["nozzleId"] = 0;

                    // convert #RRGGBB to RRGGBBAA
                    mapping_item["sourceColor"] = m_filaments[k].color;
                    mapping_item["targetColor"] = m_ams_mapping_result[k].color;
                    if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID) { tray_id = -1; }

                    /*new ams mapping data*/
                    try {
                        if (m_ams_mapping_result[k].ams_id.empty() || m_ams_mapping_result[k].slot_id.empty()) { // invalid case
                            mapping_item_v1["ams_id"]  = VIRTUAL_TRAY_MAIN_ID;
                            mapping_item_v1["slot_id"] = VIRTUAL_TRAY_MAIN_ID;
                        } else {
                            mapping_item_v1["ams_id"]  = std::stoi(m_ams_mapping_result[k].ams_id);
                            mapping_item_v1["slot_id"] = std::stoi(m_ams_mapping_result[k].slot_id);
                        }
                    } catch (...) {}
                }
            }
            mapping_v0_json.push_back(tray_id);
            mapping_v1_json.push_back(mapping_item_v1);
            mapping_info_json.push_back(mapping_item);
        }

        mapping_array_str  = mapping_v0_json.dump();
        mapping_array_str2 = mapping_v1_json.dump();
        ams_mapping_info   = mapping_info_json.dump();
        return valid_mapping_result;
    }
    return true;
}

void SendMultiMachinePage::on_send(wxCommandEvent& event)
{
    event.Skip();
    BOOST_LOG_TRIVIAL(info) << "SendMultiMachinePage: on_send";

    int result = m_plater->send_gcode(m_print_plate_idx, [this](int export_stage, int current, int total, bool& cancel) {
        if (m_is_canceled) return;
        bool     cancelled = false;
        wxString msg = _L("Preparing print job");
        //m_status_bar->update_status(msg, cancelled, 10, true);
        //m_export_3mf_cancel = cancel = cancelled;
        });

    if (m_is_canceled || m_export_3mf_cancel) {
        BOOST_LOG_TRIVIAL(info) << "print_job: m_export_3mf_cancel or m_is_canceled";
        //m_status_bar->set_status_text(task_canceled_text);
        return;
    }

    if (result < 0) {
        wxString msg = _L("Abnormal print file data. Please slice again");
        //m_status_bar->set_status_text(msg);
        return;
    }

    // export config 3mf if needed
    result = m_plater->export_config_3mf(m_print_plate_idx);
    if (result < 0) {
        BOOST_LOG_TRIVIAL(info) << "export_config_3mf failed, result = " << result;
        return;
    }

    if (m_is_canceled || m_export_3mf_cancel) {
        BOOST_LOG_TRIVIAL(info) << "print_job: m_export_3mf_cancel or m_is_canceled";
        //m_status_bar->set_status_text(task_canceled_text);
        return;
    }


    std::vector<BBL::PrintParams> print_params;

    for (auto it = m_device_items.begin(); it != m_device_items.end(); ++it) {
        auto obj = it->second->get_obj();

        if (obj && obj->is_online() && !obj->can_abort() && !obj->is_in_upgrading() && it->second->get_state_selected() == 1 && it->second->state_printable <= 2) {

            if (!it->second->is_blocking_printing(obj)) {
                BBL::PrintParams params = request_params(obj);
                print_params.push_back(params);
            }
        }
    }


    if (print_params.size() <= 0) {
        MessageDialog msg_wingow(nullptr, _L("There is no device available to send printing."), "", wxICON_WARNING | wxOK);
        msg_wingow.ShowModal();
        return;
    }

    if (wxGetApp().getTaskManager()) {
       TaskSettings settings;

       try
       {
           if (app_config->get("sending_interval").empty()) {
               app_config->set("sending_interval", "1");
               app_config->save();
           }

           if ( app_config->get("max_send").empty()) {
               app_config->set("max_send", "10");
               app_config->save();
           }


           settings.sending_interval = std::stoi(app_config->get("sending_interval")) * 60;
           settings.max_sending_at_same_time = std::stoi(app_config->get("max_send"));

           if (settings.max_sending_at_same_time <= 0) {
               MessageDialog msg_wingow(nullptr, _L("The number of printers in use simultaneously cannot be equal to 0."), "", wxICON_WARNING | wxOK);
               msg_wingow.ShowModal();
               return;
           }



           wxGetApp().getTaskManager()->start_print(print_params, &settings);
       }
       catch (...)
       {}
    }
    //jump to info
    EndModal(wxCLOSE);
    wxGetApp().mainframe->jump_to_multipage();
}

bool SendMultiMachinePage::Show(bool show)
{
    if (show) {
        refresh_user_device();
        set_default();

        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(4000);
        wxPostEvent(this, wxTimerEvent());
    }
    else {
        m_refresh_timer->Stop();
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
           dev->subscribe_device_list(std::vector<std::string>());
        }
    }
    return wxDialog::Show(show);
}

wxBoxSizer* SendMultiMachinePage::create_item_title(wxString title, wxWindow* parent, wxString tooltip)
{
    wxBoxSizer* m_sizer_title = new wxBoxSizer(wxHORIZONTAL);

    auto m_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    m_title->SetForegroundColour(DESIGN_GRAY800_COLOR);
    m_title->SetFont(::Label::Head_13);
    m_title->Wrap(-1);
    m_title->SetToolTip(tooltip);

    auto m_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line->SetBackgroundColour(DESIGN_GRAY400_COLOR);

    m_sizer_title->Add(m_title, 0, wxALIGN_CENTER | wxALL, 3);
    m_sizer_title->Add(0, 0, 0, wxLEFT, 9);
    wxBoxSizer* sizer_line = new wxBoxSizer(wxVERTICAL);
    sizer_line->Add(m_line, 0, wxEXPAND, 0);
    m_sizer_title->Add(sizer_line, 1, wxALIGN_CENTER, 0);

    return m_sizer_title;
}

wxBoxSizer* SendMultiMachinePage::create_item_checkbox(wxString title, wxWindow* parent, wxString tooltip, int padding_left, std::string param)
{
    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    auto checkbox = new ::CheckBox(parent);

    checkbox->SetValue((app_config->get("print", param) == "1") ? true : false);

    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_13);

    auto size = checkbox_title->GetTextExtent(title);
    checkbox_title->SetMinSize(wxSize(size.x + FromDIP(5), -1));
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    // save
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent& e) {
        app_config->set_str("print", param, checkbox->GetValue() ? std::string("1") : std::string("0"));
        app_config->save();
        e.Skip();
    });

    checkbox->SetToolTip(tooltip);
    m_checkbox_map.emplace(param, checkbox);
    return m_sizer_checkbox;
}

wxBoxSizer* SendMultiMachinePage::create_item_input(wxString str_before, wxString str_after, wxWindow* parent, wxString tooltip, std::string param)
{
    wxBoxSizer* sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto input_title = new wxStaticText(parent, wxID_ANY, str_before);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);

    auto input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->GetTextCtrl()->SetValidator(validator);

    auto second_title = new wxStaticText(parent, wxID_ANY, str_after, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_13);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);
    sizer_input->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input](wxCommandEvent& e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        app_config->save();
        e.Skip();
        });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input](wxFocusEvent& e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        app_config->save();
        e.Skip();
        });

    m_input_map.emplace(param, input);
    return sizer_input;
}

wxBoxSizer* SendMultiMachinePage::create_item_radiobox(wxString title, wxWindow* parent, wxString tooltip, int groupid, std::string param)
{
    wxBoxSizer* radiobox_sizer = new wxBoxSizer(wxHORIZONTAL);

    RadioBox* radiobox = new RadioBox(parent);
    radiobox->SetBackgroundColour(wxColour(248, 248, 248));
    radiobox->Bind(wxEVT_LEFT_DOWN, &SendMultiMachinePage::OnSelectRadio, this);

    AmsRadioSelector* rs = new AmsRadioSelector;
    rs->m_groupid = groupid;
    rs->m_param_name = param;
    rs->m_radiobox = radiobox;
    rs->m_selected = false;
    m_radio_group.Append(rs);

    wxStaticText* text = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    radiobox_sizer->Add(radiobox, 0, wxLEFT, FromDIP(23));
    radiobox_sizer->Add(text, 0, wxLEFT, FromDIP(10));
    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return radiobox_sizer;
}

void SendMultiMachinePage::OnSelectRadio(wxMouseEvent& event)
{
    AmsRadioSelectorList::Node* node = m_radio_group.GetFirst();
    auto                     groupid = 0;

    //while (node) {
    //    AmsRadioSelector* rs = node->GetData();
    //    if (rs->m_radiobox->GetId() == event.GetId()) groupid = rs->m_groupid;
    //    node = node->GetNext();
    //}

    node = m_radio_group.GetFirst();
    while (node) {
        AmsRadioSelector *rs = node->GetData();
        if (rs->m_groupid == groupid && rs->m_radiobox->GetId() == event.GetId()) {
            rs->m_radiobox->SetValue(true);
            if (rs->m_param_name == "use_external") {
                MaterialHash::iterator iter = m_material_list.begin();
                while (iter != m_material_list.end()) {
                    Material *    item = iter->second;
                    MaterialItem *m    = item->item;
                    if (item->id == m_current_filament_id) { m->set_ams_info(wxColour("#CECECE"), "Ext", 0, std::vector<wxColour>()); }
                    iter++;
                }
            } else if (rs->m_param_name == "use_ams") {
                m_current_filament_id = 1;
                wxCommandEvent event(EVT_SET_FINISH_MAPPING);
                event.SetInt(0);
                event.SetString("206|206|206|255|A1|1|0|0");
                wxPostEvent(this, event);
            }
        }
        if (rs->m_groupid == groupid && rs->m_radiobox->GetId() != event.GetId()) rs->m_radiobox->SetValue(false);
        node = node->GetNext();
    }
}

void SendMultiMachinePage::on_select_radio(std::string param)
{
    AmsRadioSelectorList::Node* node = m_radio_group.GetFirst();
    auto                     groupid = 0;

    while (node) {
        AmsRadioSelector* rs = node->GetData();
        if (rs->m_param_name == param) groupid = rs->m_groupid;
        node = node->GetNext();
    }

    node = m_radio_group.GetFirst();
    while (node) {
        AmsRadioSelector* rs = node->GetData();
        if (rs->m_groupid == groupid && rs->m_param_name == param) rs->m_radiobox->SetValue(true);
        if (rs->m_groupid == groupid && rs->m_param_name != param) rs->m_radiobox->SetValue(false);
        node = node->GetNext();
    }
}

bool SendMultiMachinePage::get_value_radio(std::string param)
{
    AmsRadioSelectorList::Node* node = m_radio_group.GetFirst();
    auto                     groupid = 0;
    while (node) {
        AmsRadioSelector* rs = node->GetData();
        if (rs->m_groupid == groupid && rs->m_param_name == param)
            return rs->m_radiobox->GetValue();
        node = node->GetNext();
    }
    return false;
}

void SendMultiMachinePage::on_set_finish_mapping(wxCommandEvent& evt)
{
    auto selection_data = evt.GetString();
    auto selection_data_arr = wxSplit(selection_data.ToStdString(), '|');

    BOOST_LOG_TRIVIAL(info) << "The ams mapping selection result: data is " << selection_data;

    if (selection_data_arr.size() == 8) {
        auto ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
        int  old_filament_id = (int)wxAtoi(selection_data_arr[5]);

        int ctype = 0;
        std::vector<wxColour> material_cols;
        std::vector<std::string> tray_cols;
        for (auto mapping_item : m_mapping_popup->m_mapping_item_list) {
            if (mapping_item->m_tray_data.id == evt.GetInt()) {
                ctype = mapping_item->m_tray_data.ctype;
                material_cols = mapping_item->m_tray_data.material_cols;
                for (auto col : mapping_item->m_tray_data.material_cols) {
                    wxString color = wxString::Format("#%02X%02X%02X%02X", col.Red(), col.Green(), col.Blue(), col.Alpha());
                    tray_cols.push_back(color.ToStdString());
                }
                break;
            }
        }

        for (auto i = 0; i < m_ams_mapping_result.size(); i++) {
            if (m_ams_mapping_result[i].id == wxAtoi(selection_data_arr[5])) {
                m_ams_mapping_result[i].tray_id = evt.GetInt();
                auto     ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                wxString color      = wxString::Format("#%02X%02X%02X%02X", ams_colour.Red(), ams_colour.Green(), ams_colour.Blue(), ams_colour.Alpha());
                m_ams_mapping_result[i].color  = color.ToStdString();
                m_ams_mapping_result[i].ctype  = ctype;
                m_ams_mapping_result[i].colors = tray_cols;

                m_ams_mapping_result[i].ams_id  = selection_data_arr[6].ToStdString();
                m_ams_mapping_result[i].slot_id = selection_data_arr[7].ToStdString();
            }
            BOOST_LOG_TRIVIAL(trace) << "The ams mapping result: id is " << m_ams_mapping_result[i].id << "tray_id is " << m_ams_mapping_result[i].tray_id;
        }

        MaterialHash::iterator iter = m_material_list.begin();
        while (iter != m_material_list.end()) {
            Material* item = iter->second;
            MaterialItem* m = item->item;
            if (item->id == m_current_filament_id) {
                auto ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                m->set_ams_info(ams_colour, selection_data_arr[4], ctype, material_cols);
            }
            iter++;
        }

    }
}

wxPanel* SendMultiMachinePage::create_page()
{
    auto main_page = new wxPanel(m_main_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    main_page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    // add title
    m_title_panel = new wxPanel(main_page, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_title_panel->SetBackgroundColour(*wxWHITE);
    m_title_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_rename_switch_panel = new wxSimplebook(m_title_panel);
    m_rename_switch_panel->SetMinSize(wxSize(FromDIP(240), FromDIP(25)));
    m_rename_switch_panel->SetMaxSize(wxSize(FromDIP(240), FromDIP(25)));

    m_rename_normal_panel = new wxPanel(m_rename_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_rename_normal_panel->SetBackgroundColour(*wxWHITE);
    rename_sizer_v = new wxBoxSizer(wxVERTICAL);
    rename_sizer_h = new wxBoxSizer(wxHORIZONTAL);

    m_task_name = new wxStaticText(m_rename_normal_panel, wxID_ANY, wxT("MyLabel"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxALIGN_CENTRE);
    m_task_name->SetFont(::Label::Body_13);
    m_task_name->SetMinSize(wxSize(FromDIP(200), -1));
    m_task_name->SetMaxSize(wxSize(FromDIP(200), -1));
    m_rename_button = new ScalableButton(m_rename_normal_panel, wxID_ANY, "ams_editable");
    m_rename_button->SetBackgroundColour(*wxWHITE);
    rename_sizer_h->Add(m_task_name, 0, wxALIGN_CENTER, 0);
    rename_sizer_h->Add(m_rename_button, 0, wxALIGN_CENTER, 0);
    rename_sizer_v->Add(rename_sizer_h, 1, wxALIGN_CENTER, 0);
    m_rename_normal_panel->SetSizer(rename_sizer_v);
    m_rename_normal_panel->Layout();
    rename_sizer_v->Fit(m_rename_normal_panel);

    //rename edit
    m_rename_edit_panel = new wxPanel(m_rename_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_rename_edit_panel->SetBackgroundColour(*wxWHITE);
    auto rename_edit_sizer_v = new wxBoxSizer(wxVERTICAL);

    m_rename_input = new ::TextInput(m_rename_edit_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_rename_input->GetTextCtrl()->SetFont(::Label::Body_13);
    m_rename_input->SetSize(wxSize(FromDIP(220), FromDIP(24)));
    m_rename_input->SetMinSize(wxSize(FromDIP(220), FromDIP(24)));
    m_rename_input->SetMaxSize(wxSize(FromDIP(220), FromDIP(24)));
    m_rename_input->Bind(wxEVT_TEXT_ENTER, [this](auto& e) {on_rename_enter(); });
    m_rename_input->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        if (!m_rename_input->HasFocus() && !m_task_name->HasFocus())
            on_rename_enter();
        else
            e.Skip(); });
    rename_edit_sizer_v->Add(m_rename_input, 1, wxALIGN_CENTER, 0);

    m_rename_edit_panel->SetSizer(rename_edit_sizer_v);
    m_rename_edit_panel->Layout();
    rename_edit_sizer_v->Fit(m_rename_edit_panel);

    m_rename_button->Bind(wxEVT_BUTTON, &SendMultiMachinePage::on_rename_click, this);
    m_rename_switch_panel->AddPage(m_rename_normal_panel, wxEmptyString, true);
    m_rename_switch_panel->AddPage(m_rename_edit_panel, wxEmptyString, false);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (m_rename_switch_panel->GetSelection() == 0) {
                e.Skip();
            }
            else {
                m_rename_switch_panel->SetSelection(0);
                m_task_name->SetLabel(m_current_project_name);
                m_rename_normal_panel->Layout();
            }
        }
        else {
            e.Skip();
        }
    });

    m_text_sizer = new wxBoxSizer(wxVERTICAL);
    m_text_sizer->Add(m_rename_switch_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    m_panel_image = new wxPanel(m_title_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_image_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_thumbnail_panel = new ThumbnailPanel(m_panel_image);
    m_thumbnail_panel->SetSize(wxSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
    m_thumbnail_panel->SetMinSize(wxSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
    m_thumbnail_panel->SetMaxSize(wxSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
    m_thumbnail_panel->SetBackgroundColour(*wxRED);
    m_image_sizer->Add(m_thumbnail_panel, 0, wxALIGN_CENTER, 0);
    m_panel_image->SetSizer(m_image_sizer);
    m_panel_image->Layout();
    m_title_sizer->Add(m_panel_image, 0, wxLEFT, 0);

    wxBoxSizer* m_sizer_basic = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* m_sizer_basic_time = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* m_sizer_basic_weight = new wxBoxSizer(wxHORIZONTAL);

    print_time = new ScalableBitmap(m_title_panel, "print-time", 18);
    timeimg = new wxStaticBitmap(m_title_panel, wxID_ANY, print_time->bmp(), wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), 0);
    m_sizer_basic_time->Add(timeimg, 1, wxEXPAND | wxALL, FromDIP(5));
    m_stext_time = new wxStaticText(m_title_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
    m_sizer_basic_time->Add(m_stext_time, 0, wxALL, FromDIP(5));
    m_sizer_basic->Add(m_sizer_basic_time, 0, wxALIGN_CENTER, 0);
    m_sizer_basic->Add(0, 0, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

    print_weight = new ScalableBitmap(m_title_panel, "print-weight", 18);
    weightimg = new wxStaticBitmap(m_title_panel, wxID_ANY, print_weight->bmp(), wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), 0);
    m_sizer_basic_weight->Add(weightimg, 1, wxEXPAND | wxALL, FromDIP(5));
    m_stext_weight = new wxStaticText(m_title_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_sizer_basic_weight->Add(m_stext_weight, 0, wxALL, FromDIP(5));
    m_sizer_basic->Add(m_sizer_basic_weight, 0, wxALIGN_CENTER, 0);

    m_text_sizer->Add(m_sizer_basic, wxALIGN_CENTER, 0);
    m_title_sizer->Add(m_text_sizer, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_title_panel->SetSizer(m_title_sizer);
    m_title_panel->Layout();
    sizer->Add(m_title_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    // add filament
    wxBoxSizer* title_filament = create_item_title(_L("Filament"), main_page, "");
    wxBoxSizer* radio_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* use_external_sizer = create_item_radiobox(_L("Use External Spool"), main_page, "", 0, "use_external");
    wxBoxSizer* use_ams_sizer = create_item_radiobox(_L("Use AMS"), main_page, "", 0, "use_ams");
    radio_sizer->Add(use_external_sizer, 0, wxLeft, FromDIP(20));
    radio_sizer->Add(use_ams_sizer, 0, wxLeft, FromDIP(5));
    sizer->Add(title_filament, 0, wxEXPAND, 0);
    sizer->Add(radio_sizer, 0, wxLEFT, FromDIP(20));
    sizer->AddSpacer(FromDIP(5));
    on_select_radio("use_external");

    // add ams item
    m_ams_list_sizer = new wxGridSizer(0, 4, 0, FromDIP(5));

    sizer->Add(m_ams_list_sizer, 0, wxLEFT, FromDIP(25));
    sizer->AddSpacer(FromDIP(10));

    // select printer
    wxBoxSizer* title_select_printer = create_item_title(_L("Select Printers"), main_page, "");

    // add table head
    StateColor head_bg(
        std::pair<wxColour, int>(TABLE_HEAD_PRESSED_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(TABLE_HEAR_NORMAL_COLOUR, StateColor::Normal)
    );

    m_table_head_panel = new wxPanel(main_page, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_table_head_panel->SetMinSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetMaxSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetBackgroundColour(TABLE_HEAR_NORMAL_COLOUR);
    m_table_head_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_select_checkbox = new CheckBox(m_table_head_panel, wxID_ANY);
    m_table_head_sizer->AddSpacer(FromDIP(SEND_LEFT_PADDING_LEFT));
    m_table_head_sizer->Add(m_select_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_select_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        if (m_select_checkbox->GetValue()) {
            for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {

                if (it->second->state_printable <= 2) {
                    it->second->selected();
                }
            }
        }
        else {
            for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
                it->second->unselected();
            }
        }
        Refresh(false);
        e.Skip();
    });

    m_printer_name = new Button(m_table_head_panel, _L("Device Name"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_printer_name->SetBackgroundColor(head_bg);
    m_printer_name->SetCornerRadius(0);
    m_printer_name->SetFont(TABLE_HEAD_FONT);
    m_printer_name->SetMinSize(wxSize(FromDIP(SEND_LEFT_DEV_NAME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(SEND_LEFT_DEV_NAME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_printer_name->SetCenter(false);
    m_printer_name->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_printer_name->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_printer_name->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_name_big = !device_name_big;
        this->m_sort.set_role(SortItem::SortRule::SR_DEV_NAME, device_name_big);
        this->refresh_user_device();
    });

    m_table_head_sizer->Add( 0, 0, 0, wxLEFT, FromDIP(10) );
    m_table_head_sizer->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_device_status = new Button(m_table_head_panel, _L("Device Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_device_status->SetBackgroundColor(head_bg);
    m_device_status->SetFont(TABLE_HEAD_FONT);
    m_device_status->SetCornerRadius(0);
    m_device_status->SetMinSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_device_status->SetMaxSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_device_status->SetCenter(false);
    m_device_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_device_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_device_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_printable_big = !device_printable_big;
        this->m_sort.set_role(SortItem::SortRule::SR_PRINTABLE, device_printable_big);
        this->refresh_user_device();
        evt.Skip();
    });
    m_table_head_sizer->Add(m_device_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    /*m_task_status = new Button(m_table_head_panel, _L("Task Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SIZE);
    m_task_status->SetBackgroundColor(head_bg);
    m_task_status->SetFont(TABLE_HEAD_FONT);
    m_task_status->SetCornerRadius(0);
    m_task_status->SetMinSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_status->SetMaxSize(wxSize(FromDIP(SEND_LEFT_DEV_STATUS), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_status->SetCenter(false);
    m_task_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
        });
    m_task_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
        });
    m_task_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_printable_big = !device_printable_big;
        this->m_sort.set_role(SortItem::SortRule::SR_PRINTABLE, device_printable_big);
        this->refresh_user_device();
        evt.Skip();
    });*/

    //m_table_head_sizer->Add(m_task_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_ams = new Button(m_table_head_panel, _L("AMS Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_ams->SetBackgroundColor(head_bg);
    m_ams->SetCornerRadius(0);
    m_ams->SetFont(TABLE_HEAD_FONT);
    m_ams->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_ams->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_ams->SetCenter(false);
    m_ams->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_ams->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_ams->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_en_ams_big = !device_en_ams_big;
        this->m_sort.set_role(SortItem::SortRule::SR_EN_AMS, device_en_ams_big);
        this->refresh_user_device();
        evt.Skip();
    });
    m_table_head_sizer->Add(m_ams, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_refresh_button = new Button(m_table_head_panel, "", "mall_control_refresh", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_refresh_button->SetBackgroundColor(head_bg);
    m_refresh_button->SetCornerRadius(0);
    m_refresh_button->SetFont(TABLE_HEAD_FONT);
    m_refresh_button->SetMinSize(wxSize(FromDIP(50), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_refresh_button->SetMaxSize(wxSize(FromDIP(50), FromDIP(SEND_ITEM_MAX_HEIGHT)));
    m_refresh_button->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_refresh_button->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_refresh_button->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        this->refresh_user_device();
        evt.Skip();
    });
    m_table_head_sizer->Add(m_refresh_button, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_table_head_panel->SetSizer(m_table_head_sizer);
    m_table_head_panel->Layout();

    m_tip_text = new wxStaticText(main_page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tip_text->SetMinSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetMaxSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetLabel(_L("Please select the devices you would like to manage here (up to 6 devices)"));
    m_tip_text->SetForegroundColour(DESIGN_GRAY800_COLOR);
    m_tip_text->SetFont(::Label::Head_20);
    m_tip_text->Wrap(-1);

    m_button_add = new Button(main_page, _L("Add"));
    m_button_add->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_button_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        MultiMachinePickPage dlg;
        dlg.ShowModal();
        refresh_user_device();
        evt.Skip();
    });

    scroll_macine_list = new wxScrolledWindow(main_page, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(800), FromDIP(300)), wxHSCROLL | wxVSCROLL);
    scroll_macine_list->SetBackgroundColour(*wxWHITE);
    scroll_macine_list->SetScrollRate(5, 5);
    scroll_macine_list->SetMinSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), 10 * FromDIP(SEND_ITEM_MAX_HEIGHT)));
    scroll_macine_list->SetMaxSize(wxSize(FromDIP(DEVICE_ITEM_MAX_WIDTH), 10 * FromDIP(SEND_ITEM_MAX_HEIGHT)));

    sizer_machine_list = new wxBoxSizer(wxVERTICAL);
    scroll_macine_list->SetSizer(sizer_machine_list);
    scroll_macine_list->Layout();

    sizer->Add(title_select_printer, 0, wxEXPAND, 0);
    sizer->Add(m_table_head_panel, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(40));
    sizer->Add(m_tip_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(100));
    sizer->Add(m_button_add, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(20));
    sizer->Add(scroll_macine_list, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(40));
    sizer->AddSpacer(FromDIP(10));

    // add printing options
    wxBoxSizer* title_print_option = create_item_title(_L("Printing Options"), main_page, "");
    wxBoxSizer* item_bed_level = create_item_checkbox(_L("Bed Leveling"), main_page, "", 50, "bed_leveling");
    wxBoxSizer* item_timelapse = create_item_checkbox(_L("Timelapse"), main_page, "", 50, "timelapse");
    wxBoxSizer* item_flow_dy_ca = create_item_checkbox(_L("Flow Dynamic Calibration"), main_page, "", 50, "flow_cali");
    sizer->Add(title_print_option, 0, wxEXPAND, 0);
    wxBoxSizer* options_sizer_v = new wxBoxSizer(wxHORIZONTAL);
    options_sizer_v->Add(item_bed_level, 0, wxLEFT, 0);
    options_sizer_v->Add(item_timelapse, 0, wxLEFT, FromDIP(100));
    sizer->Add(options_sizer_v, 0, wxLEFT, FromDIP(20));
    sizer->Add(item_flow_dy_ca, 0, wxLEFT, FromDIP(20));
    sizer->AddSpacer(FromDIP(10));

    // add send option
    wxBoxSizer* title_send_option = create_item_title(_L("Send Options"), main_page, "");
    wxBoxSizer* max_printer_send = create_item_input(_L("Send to"), _L("printers at the same time. (It depends on how many devices can undergo heating at the same time.)"), main_page, "", "max_send");
    wxBoxSizer* delay_time = create_item_input(_L("Wait"), _L("minute each batch. (It depends on how long it takes to complete the heating.)"), main_page, "", "sending_interval");
    sizer->Add(title_send_option, 0, wxEXPAND, 0);
    sizer->Add(max_printer_send, 0, wxLEFT, FromDIP(20));
    sizer->AddSpacer(FromDIP(3));
    sizer->Add(delay_time, 0, wxLEFT, FromDIP(20));
    sizer->AddSpacer(FromDIP(10));

    // add send button
    m_button_send = new Button(main_page, _L("Send"));
    m_button_send->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_send->Bind(wxEVT_BUTTON, &SendMultiMachinePage::on_send, this);
    //m_button_send->Disable();
    //m_button_send->SetBackgroundColor(wxColour(0x90, 0x90, 0x90));
    //m_button_send->SetBorderColor(wxColour(0x90, 0x90, 0x90));
    sizer->Add(m_button_send, 0, wxALIGN_CENTER, 0);

    main_page->SetSizer(sizer);
    main_page->Layout();
    main_page->Fit();
    return main_page;
}

void SendMultiMachinePage::sync_ams_list()
{
    // for black list
    std::vector<std::string> materials;
    std::vector<std::string> brands;
    std::vector<std::string> display_materials;
    std::vector<std::string> m_filaments_id;
    auto                     preset_bundle = wxGetApp().preset_bundle;

    for (auto filament_name : preset_bundle->filament_presets) {
        for (int f_index = 0; f_index < preset_bundle->filaments.size(); f_index++) {
            PresetCollection* filament_presets = &wxGetApp().preset_bundle->filaments;
            Preset* preset = &filament_presets->preset(f_index);

            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                std::string m_filament_id = preset->filament_id;
                display_materials.push_back(display_filament_type);
                materials.push_back(filament_type);
                m_filaments_id.push_back(m_filament_id);

                std::string m_vendor_name = "";
                auto        vendor = dynamic_cast<ConfigOptionStrings*>(preset->config.option("filament_vendor"));
                if (vendor && (vendor->values.size() > 0)) {
                    std::string vendor_name = vendor->values[0];
                    m_vendor_name = vendor_name;
                }
                brands.push_back(m_vendor_name);
            }
        }
    }

    auto           extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();
    BitmapCache    bmcache;
    MaterialHash::iterator iter = m_material_list.begin();
    while (iter != m_material_list.end()) {
        int       id = iter->first;
        Material* item = iter->second;
        item->item->Destroy();
        delete item;
        iter++;
    }

    m_ams_list_sizer->Clear();
    m_material_list.clear();
    m_filaments.clear();
    m_ams_mapping_result.clear();

    for (auto i = 0; i < extruders.size(); i++) {
        auto          extruder = extruders[i] - 1;
        auto          colour = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int)extruder);
        unsigned char rgb[4];
        bmcache.parse_color4(colour, rgb);

        auto colour_rgb = wxColour((int)rgb[0], (int)rgb[1], (int)rgb[2], (int)rgb[3]);
        if (extruder >= materials.size() || extruder < 0 || extruder >= display_materials.size()) continue;

        MaterialItem* item = new MaterialItem(m_main_page, colour_rgb, _L(display_materials[extruder]));
        //item->set_ams_info(wxColour("#CECECE"), "A1", 0, std::vector<wxColour>());
        item->set_ams_info(wxColour("#CECECE"), "Ext", 0, std::vector<wxColour>());
        m_ams_list_sizer->Add(item, 0, wxALL, FromDIP(4));

        item->Bind(wxEVT_LEFT_UP, [this, item, materials, extruder](wxMouseEvent& e) {});
        item->Bind(wxEVT_LEFT_DOWN, [this, item, materials, extruder](wxMouseEvent& e) {
            MaterialHash::iterator iter = m_material_list.begin();
            while (iter != m_material_list.end()) {
                int           id = iter->first;
                Material* item = iter->second;
                MaterialItem* m = item->item;
                m->on_normal();
                iter++;
            }

            m_current_filament_id = extruder;
            item->on_selected();

            auto    mouse_pos = ClientToScreen(e.GetPosition());
            wxPoint rect = item->ClientToScreen(wxPoint(0, 0));

            // update ams data
            if (get_value_radio("use_ams")) {
                if (m_mapping_popup->IsShown()) return;
                wxPoint pos = item->ClientToScreen(wxPoint(0, 0));
                pos.y += item->GetRect().height;
                m_mapping_popup->Move(pos);
                m_mapping_popup->set_send_win(this);
                m_mapping_popup->set_parent_item(item);
                m_mapping_popup->set_current_filament_id(extruder);
                m_mapping_popup->set_tag_texture(materials[extruder]);
                m_mapping_popup->update_ams_data_multi_machines();
                m_mapping_popup->Popup();
            }
            });

        Material* material_item = new Material();
        material_item->id = extruder;
        material_item->item = item;
        m_material_list[i] = material_item;

        // build for ams mapping
        if (extruder < materials.size() && extruder >= 0) {
            FilamentInfo info;
            info.id = extruder;
            info.tray_id = 0;
            info.type = materials[extruder];
            info.brand = brands[extruder];
            info.filament_id = m_filaments_id[extruder];
            //info.color = wxString::Format("#%02X%02X%02X%02X", colour_rgb.Red(), colour_rgb.Green(), colour_rgb.Blue(), colour_rgb.Alpha()).ToStdString();
            info.color = "#CECECEFF";
            m_filaments.push_back(info);
            m_ams_mapping_result.push_back(info);
        }
    }

    if (extruders.size() <= 8) {
        m_ams_list_sizer->SetCols(extruders.size());
    }
    else {
        m_ams_list_sizer->SetCols(8);
    }
}

void SendMultiMachinePage::set_default_normal(const ThumbnailData& data)
{
    if (data.is_valid()) {
        wxImage image(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char* px = (unsigned char*)data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
                image.SetAlpha((int)c, (int)r, px[3]);
            }
        }
        image = image.Rescale(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
        m_thumbnail_panel->set_thumbnail(image);
    }

    m_main_scroll->Layout();
    m_main_scroll->Fit();

    // basic info
    auto aprint_stats = m_plater->get_partplate_list().get_current_fff_print().print_statistics();
    wxString   time;
    PartPlate* plate = m_plater->get_partplate_list().get_curr_plate();
    if (plate) {
        if (plate->get_slice_result()) { time = wxString::Format("%s", short_time(get_time_dhms(plate->get_slice_result()->print_statistics.modes[0].time))); }
    }

    char weight[64];
    if (wxGetApp().app_config->get("use_inches") == "1") {
        ::sprintf(weight, "  %.2f oz", aprint_stats.total_weight * 0.035274);
    }
    else {
        ::sprintf(weight, "  %.2f g", aprint_stats.total_weight);
    }

    m_stext_time->SetLabel(time);
    m_stext_weight->SetLabel(weight);
}

void SendMultiMachinePage::set_default()
{
    wxString filename = m_plater->get_export_gcode_filename("", true, m_print_plate_idx == PLATE_ALL_IDX ? true : false);
    if (m_print_plate_idx == PLATE_ALL_IDX && filename.empty()) {
        filename = _L("Untitled");
    }

    if (filename.empty()) {
        filename = m_plater->get_export_gcode_filename("", true);
        if (filename.empty()) filename = _L("Untitled");
    }

    fs::path filename_path(filename.c_str());
    std::string file_name = filename_path.filename().string();
    if (from_u8(file_name).find(_L("Untitled")) != wxString::npos) {
        PartPlate* part_plate = m_plater->get_partplate_list().get_plate(m_print_plate_idx);
        if (part_plate) {
            if (std::vector<ModelObject*> objects = part_plate->get_objects_on_this_plate(); objects.size() > 0) {
                file_name = objects[0]->name;
                for (int i = 1; i < objects.size(); i++) {
                    file_name += (" + " + objects[i]->name);
                }
            }
            if (file_name.size() > 100) {
                file_name = file_name.substr(0, 97) + "...";
            }
        }
    }

    m_current_project_name = wxString::FromUTF8(file_name);
    //unsupported character filter
    m_current_project_name = from_u8(filter_characters(m_current_project_name.ToUTF8().data(), "<>[]:/\\|?*\""));

    m_task_name->SetLabel(m_current_project_name);

    sync_ams_list();
    set_default_normal(m_plater->get_partplate_list().get_curr_plate()->thumbnail_data);
}

void SendMultiMachinePage::on_rename_enter()
{
    if (m_is_rename_mode == false) {
        return;
    }
    else {
        m_is_rename_mode = false;
    }

    auto     new_file_name = m_rename_input->GetTextCtrl()->GetValue();
    wxString temp;
    int      num = 0;
    for (auto t : new_file_name) {
        if (t == wxString::FromUTF8("\x20")) {
            num++;
            if (num == 1) temp += t;
        }
        else {
            num = 0;
            temp += t;
        }
    }
    new_file_name = temp;
    auto     m_valid_type = Valid;
    wxString info_line;

    const char* unusable_symbols = "<>[]:/\\|?*\"";

    const std::string unusable_suffix = PresetCollection::get_suffix_modified(); //"(modified)";
    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (new_file_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            m_valid_type = NoValid;
            break;
        }
    }

    if (m_valid_type == Valid && new_file_name.find(unusable_suffix) != std::string::npos) {
        info_line = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.empty()) {
        info_line = _L("The name is not allowed to be empty.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_first_of(' ') == 0) {
        info_line = _L("The name is not allowed to start with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_last_of(' ') == new_file_name.length() - 1) {
        info_line = _L("The name is not allowed to end with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.size() >= 100) {
        info_line = _L("The name length exceeds the limit.");
        m_valid_type = NoValid;
    }

    if (m_valid_type != Valid) {
        MessageDialog msg_wingow(nullptr, info_line, "", wxICON_WARNING | wxOK);
        if (msg_wingow.ShowModal() == wxID_OK) {
            m_rename_switch_panel->SetSelection(0);
            m_task_name->SetLabel(m_current_project_name);
            m_rename_normal_panel->Layout();
            return;
        }
    }

    m_current_project_name = new_file_name;
    m_rename_switch_panel->SetSelection(0);
    m_task_name->SetLabelText(m_current_project_name);
    m_rename_normal_panel->Layout();
}

void SendMultiMachinePage::check_fcous_state(wxWindow* window)
{
    check_focus(window);
    auto children = window->GetChildren();
    for (auto child : children) {
        check_fcous_state(child);
    }
}

void SendMultiMachinePage::check_focus(wxWindow* window)
{
    if (window == m_rename_input || window == m_rename_input->GetTextCtrl()) {
        on_rename_enter();
    }
}

void SendMultiMachinePage::on_rename_click(wxCommandEvent& event)
{
    m_is_rename_mode = true;
    m_rename_input->GetTextCtrl()->SetValue(m_current_project_name);
    m_rename_switch_panel->SetSelection(1);
    m_rename_input->GetTextCtrl()->SetFocus();
    m_rename_input->GetTextCtrl()->SetInsertionPointEnd();
}

void SendMultiMachinePage::init_timer()
{
    m_refresh_timer = new wxTimer();
}

void SendMultiMachinePage::on_timer(wxTimerEvent& event)
{
    for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
        it->second->sync_state();
        it->second->Refresh();
    }
}

} // namespace GUI
} // namespace Slic3r
