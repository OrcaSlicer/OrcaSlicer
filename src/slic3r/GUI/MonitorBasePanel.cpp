///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.1-0-g8feb16b3)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#include "MonitorBasePanel.h"
#include "Printer/PrinterFileSystem.h"
#include "Widgets/Label.hpp"

///////////////////////////////////////////////////////////////////////////
using namespace Slic3r::GUI;

MonitorBasePanel::MonitorBasePanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name) : wxPanel(parent, id, pos, size, style, name)
{
	this->SetMinSize(wxSize(600, 400));

	wxBoxSizer* bSizer_top;
	bSizer_top = new wxBoxSizer(wxVERTICAL);

	m_splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_3D | wxSP_BORDER);
	m_splitter->SetSashGravity(0);
	m_splitter->Connect(wxEVT_IDLE, wxIdleEventHandler(MonitorBasePanel::m_splitterOnIdle), NULL, this);
	m_splitter->SetMinimumPaneSize(182);

	m_panel_splitter_left = new wxPanel(m_splitter, wxID_ANY, wxDefaultPosition, wxSize(182, -1), wxTAB_TRAVERSAL);
	m_panel_splitter_left->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_left_top;
	bSizer_left_top = new wxBoxSizer(wxHORIZONTAL);

	wxBoxSizer* bSizerleft;
	bSizerleft = new wxBoxSizer(wxVERTICAL);

	bSizerleft->SetMinSize(wxSize(182, 833));
	m_panel_printer = new wxPanel(m_panel_splitter_left, wxID_ANY, wxDefaultPosition, wxSize(182, 87), wxTAB_TRAVERSAL);
	m_panel_printer->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_printer_top;
	bSizer_printer_top = new wxBoxSizer(wxVERTICAL);

	bSizer_printer_top->AddStretchSpacer();

	wxBoxSizer* bSizer_printer;
	bSizer_printer = new wxBoxSizer(wxHORIZONTAL);

	bSizer_printer->SetMinSize(wxSize(-1, 36));

	bSizer_printer->Add(23, 0, 0, wxEXPAND, 0);

	m_bitmap_printer = new wxStaticBitmap(m_panel_printer, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_printer->Add(m_bitmap_printer, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);


	bSizer_printer->Add(3, 0, 0, wxEXPAND, 0);

	m_bitmap_arrow1 = new wxStaticBitmap(m_panel_printer, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_printer->Add(m_bitmap_arrow1, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);


	bSizer_printer->Add(8, 0, 0, 0, 0);

	wxBoxSizer* bSizer_printer_info;
	bSizer_printer_info = new wxBoxSizer(wxVERTICAL);

	bSizer_printer_info->SetMinSize(wxSize(-1, 27));

	bSizer_printer_info->Add(0, 14, 0, wxEXPAND, 0);

	m_staticText_machine_name = new wxStaticText(m_panel_printer, wxID_ANY, wxT("BBL-Printer001"), wxDefaultPosition, wxSize(-1, -1), wxST_ELLIPSIZE_END | wxST_ELLIPSIZE_MIDDLE | wxST_ELLIPSIZE_START);
	m_staticText_machine_name->Wrap(-1);
	m_staticText_machine_name->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("@HarmonyOS Sans SC")));
	m_staticText_machine_name->SetMinSize(wxSize(100, -1));

	bSizer_printer_info->Add(m_staticText_machine_name, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);

	m_staticText_capacity_val = new wxStaticText(m_panel_printer, wxID_ANY, wxT("N/A"), wxDefaultPosition, wxSize(-1, -1), 0);
	m_staticText_capacity_val->Wrap(-1);
	m_staticText_capacity_val->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxEmptyString));

	bSizer_printer_info->Add(m_staticText_capacity_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);


	bSizer_printer->Add(bSizer_printer_info, 1, wxEXPAND, 0);


	bSizer_printer_top->Add(bSizer_printer, 0, wxEXPAND, 0);

	bSizer_printer_top->AddStretchSpacer();

	m_panel_printer->SetSizer(bSizer_printer_top);
	m_panel_printer->Layout();
	bSizer_printer_top->Fit(m_panel_printer);
	bSizerleft->Add(m_panel_printer, 0, wxALL | wxEXPAND, 0);

	m_staticline1 = new StaticLine(m_panel_splitter_left);
	m_staticline1->SetLineColour(wxColour(0xEEEEEE));
	bSizerleft->Add(m_staticline1, 0, wxEXPAND | wxRIGHT | wxLEFT, 14);

	m_panel_status_tab = new wxPanel(m_panel_splitter_left, wxID_ANY, wxDefaultPosition, wxSize(182, 52), wxTAB_TRAVERSAL);
	m_panel_status_tab->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_status_caption;
	bSizer_status_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_status_caption->Add(28, 0, 0, 0, 0);

	m_staticText_status = new wxStaticText(m_panel_status_tab, wxID_ANY, wxT("Status"), wxDefaultPosition, wxSize(-1, -1), wxST_ELLIPSIZE_END);
	m_staticText_status->Wrap(-1);
	m_staticText_status->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("@HarmonyOS Sans SC")));
	m_staticText_status->SetMinSize(wxSize(65, -1));

	bSizer_status_caption->Add(m_staticText_status, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM | wxLEFT, 0);

	m_bitmap_signal = new wxStaticBitmap(m_panel_status_tab, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_status_caption->Add(m_bitmap_signal, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);


	bSizer_status_caption->AddStretchSpacer();

	m_bitmap_arrow2 = new wxStaticBitmap(m_panel_status_tab, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_status_caption->Add(m_bitmap_arrow2, 0, wxALIGN_CENTER_VERTICAL, 26);

	bSizer_status_caption->Add(16, 0, 0, wxEXPAND, 0);

	m_panel_status_tab->SetSizer(bSizer_status_caption);
	m_panel_status_tab->Layout();
	bSizer_status_caption->Fit(m_panel_status_tab);
	bSizerleft->Add(m_panel_status_tab, 0, wxALL | wxEXPAND, 0);

	m_staticline2 = new StaticLine(m_panel_splitter_left);
	m_staticline2->SetLineColour(wxColour(0xEEEEEE));
	bSizerleft->Add(m_staticline2, 0, wxEXPAND | wxRIGHT | wxLEFT, 14);

	m_panel_time_lapse_tab = new wxPanel(m_panel_splitter_left, wxID_ANY, wxDefaultPosition, wxSize(182, 52), wxTAB_TRAVERSAL);
	m_panel_time_lapse_tab->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_time_lapse_caption;
	bSizer_time_lapse_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_time_lapse_caption->Add(28, 0, 0, wxALL, 0);

	m_staticText_time_lapse = new wxStaticText(m_panel_time_lapse_tab, wxID_ANY, wxT("Time Lapse"), wxDefaultPosition, wxSize(-1, -1), wxALIGN_LEFT | wxST_ELLIPSIZE_END);
	m_staticText_time_lapse->Wrap(-1);
	m_staticText_time_lapse->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("@HarmonyOS Sans SC")));
	m_staticText_time_lapse->SetMinSize(wxSize(122, -1));

	bSizer_time_lapse_caption->Add(m_staticText_time_lapse, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);


	bSizer_time_lapse_caption->AddStretchSpacer();

	m_bitmap_arrow3 = new wxStaticBitmap(m_panel_time_lapse_tab, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_time_lapse_caption->Add(m_bitmap_arrow3, 0, wxALIGN_CENTER_VERTICAL, 26);

	bSizer_time_lapse_caption->Add(16, 0, 0, wxEXPAND, 0);

	m_panel_time_lapse_tab->SetSizer(bSizer_time_lapse_caption);
	m_panel_time_lapse_tab->Layout();
	bSizer_time_lapse_caption->Fit(m_panel_time_lapse_tab);
	bSizerleft->Add(m_panel_time_lapse_tab, 0, wxALL | wxEXPAND, 0);

	m_staticline3 = new StaticLine(m_panel_splitter_left);
	m_staticline3->SetLineColour(wxColour(0xEEEEEE));
	bSizerleft->Add(m_staticline3, 0, wxEXPAND | wxRIGHT | wxLEFT, 14);

	m_panel_video_tab = new wxPanel(m_panel_splitter_left, wxID_ANY, wxDefaultPosition, wxSize(182, 52), wxTAB_TRAVERSAL);
	m_panel_video_tab->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_video_monitoring_caption;
	bSizer_video_monitoring_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_video_monitoring_caption->Add(28, 0, 0, wxALL, 0);

	m_staticText_video_monitoring = new wxStaticText(m_panel_video_tab, wxID_ANY, wxT("Video"), wxDefaultPosition, wxSize(-1, -1), wxALIGN_LEFT | wxST_ELLIPSIZE_END);
	m_staticText_video_monitoring->Wrap(-1);
	m_staticText_video_monitoring->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("@HarmonyOS Sans SC")));
	m_staticText_video_monitoring->SetMinSize(wxSize(122, -1));

	bSizer_video_monitoring_caption->Add(m_staticText_video_monitoring, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);


	bSizer_video_monitoring_caption->AddStretchSpacer();

	m_bitmap_arrow4 = new wxStaticBitmap(m_panel_video_tab, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_video_monitoring_caption->Add(m_bitmap_arrow4, 0, wxALIGN_CENTER_VERTICAL, 26);

	bSizer_video_monitoring_caption->Add(16, 0, 0, wxEXPAND, 0);

	m_panel_video_tab->SetSizer(bSizer_video_monitoring_caption);
	m_panel_video_tab->Layout();
	bSizer_video_monitoring_caption->Fit(m_panel_video_tab);
	bSizerleft->Add(m_panel_video_tab, 0, wxALL | wxEXPAND, 0);

	m_staticline4 = new StaticLine(m_panel_splitter_left);
	m_staticline4->SetLineColour(wxColour(0xEEEEEE));

	bSizerleft->Add(m_staticline4, 0, wxEXPAND | wxRIGHT | wxLEFT, 14);

	m_panel_task_list_tab = new wxPanel(m_panel_splitter_left, wxID_ANY, wxDefaultPosition, wxSize(182, 52), wxTAB_TRAVERSAL);
	m_panel_task_list_tab->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_tasklist_caption;
	bSizer_tasklist_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_tasklist_caption->Add(28, 0, 0, wxALL, 0);

	m_staticText_subtask_list = new wxStaticText(m_panel_task_list_tab, wxID_ANY, wxT("Task List"), wxDefaultPosition, wxSize(-1, -1), wxALIGN_LEFT | wxST_ELLIPSIZE_END);
	m_staticText_subtask_list->Wrap(-1);
	m_staticText_subtask_list->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("@HarmonyOS Sans SC")));
	m_staticText_subtask_list->SetMinSize(wxSize(122, -1));

	bSizer_tasklist_caption->Add(m_staticText_subtask_list, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);


	bSizer_tasklist_caption->AddStretchSpacer();

	m_bitmap_arrow5 = new wxStaticBitmap(m_panel_task_list_tab, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(-1, -1), 0);

	bSizer_tasklist_caption->Add(m_bitmap_arrow5, 0, wxALIGN_CENTER_VERTICAL, 26);

	bSizer_tasklist_caption->Add(16, 0, 0, wxEXPAND, 0);

	m_panel_task_list_tab->SetSizer(bSizer_tasklist_caption);
	m_panel_task_list_tab->Layout();
	bSizer_tasklist_caption->Fit(m_panel_task_list_tab);
	bSizerleft->Add(m_panel_task_list_tab, 0, wxALL | wxEXPAND, 0);

	m_staticline5 = new StaticLine(m_panel_splitter_left);
	m_staticline5->SetLineColour(wxColour(0xEEEEEE));
	bSizerleft->Add(m_staticline5, 0, wxEXPAND | wxRIGHT | wxLEFT, 14);


	bSizer_left_top->Add(bSizerleft, 0, wxEXPAND, 0);


	m_panel_splitter_left->SetSizer(bSizer_left_top);
	m_panel_splitter_left->Layout();
	bSizer_left_top->Fit(m_panel_splitter_left);
	m_panel_splitter_right = new wxPanel(m_splitter, wxID_ANY, wxDefaultPosition, wxSize(1258, 900), wxTAB_TRAVERSAL);
	m_panel_splitter_right->SetBackgroundColour(wxColour(255, 255, 255));

	m_splitter->SplitVertically(m_panel_splitter_left, m_panel_splitter_right, 182);
	bSizer_top->Add(m_splitter, 1, wxALL | wxEXPAND, 0);


	this->SetSizerAndFit(bSizer_top);
	this->Layout();

	// Connect Events

	//make splitter immovable
	m_splitter->Connect(wxEVT_COMMAND_SPLITTER_SASH_POS_CHANGING, wxSplitterEventHandler(MonitorBasePanel::m_splitterOnSplitterSashPosChanging), NULL, this);
}

MonitorBasePanel::~MonitorBasePanel()
{
	m_splitter->Disconnect(wxEVT_COMMAND_SPLITTER_SASH_POS_CHANGING, wxSplitterEventHandler(MonitorBasePanel::m_splitterOnSplitterSashPosChanging), NULL, this);
}

VideoMonitoringBasePanel::VideoMonitoringBasePanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name) : wxPanel(parent, id, pos, size, style, name)
{
}

VideoMonitoringBasePanel::~VideoMonitoringBasePanel()
{
}

///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.1-0-g8feb16b3)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#include "MonitorBasePanel.h"

///////////////////////////////////////////////////////////////////////////
using namespace Slic3r::GUI;

TaskListBasePanel::TaskListBasePanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name) : wxPanel(parent, id, pos, size, style, name)
{
	this->SetBackgroundColour(wxColour(238, 238, 238));
	this->SetMinSize(wxSize(600, 400));

	wxFlexGridSizer* fgSizer_tasklist_top;
	fgSizer_tasklist_top = new wxFlexGridSizer(3, 1, 24, 0);
	fgSizer_tasklist_top->SetFlexibleDirection(wxBOTH);
	fgSizer_tasklist_top->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);


	fgSizer_tasklist_top->Add(0, 6, 0, wxEXPAND, 0);

	wxBoxSizer* bSizer_model_name;
	bSizer_model_name = new wxBoxSizer(wxVERTICAL);

	bSizer_model_name->SetMinSize(wxSize(496, 245));
	m_panel_model_name_caption = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(496, 48), wxTAB_TRAVERSAL);
	m_panel_model_name_caption->SetBackgroundColour(wxColour(248, 248, 248));

	wxBoxSizer* bSizer_model_name_caption;
	bSizer_model_name_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_model_name_caption->Add(23, 0, 0, wxEXPAND, 0);

	m_staticText_model_name = new wxStaticText(m_panel_model_name_caption, wxID_ANY, wxT("Model Name"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText_model_name->Wrap(-1);
	m_staticText_model_name->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("HarmonyOS Sans SC")));

	bSizer_model_name_caption->Add(m_staticText_model_name, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);


	m_panel_model_name_caption->SetSizer(bSizer_model_name_caption);
	m_panel_model_name_caption->Layout();
	bSizer_model_name_caption->Fit(m_panel_model_name_caption);
	bSizer_model_name->Add(m_panel_model_name_caption, 0, wxALL | wxEXPAND, 0);

	m_panel_model_name_content = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(496, 197), wxTAB_TRAVERSAL);
	m_panel_model_name_content->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_model_name_content;
	bSizer_model_name_content = new wxBoxSizer(wxVERTICAL);


	bSizer_model_name_content->Add(0, 30, 0, wxEXPAND, 0);

	wxBoxSizer* bSizer11;
	bSizer11 = new wxBoxSizer(wxHORIZONTAL);

	m_bitmap_task = new wxStaticBitmap(m_panel_model_name_content, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);

	bSizer11->Add(m_bitmap_task, 0, wxALL, 5);

	wxBoxSizer* bSizer12;
	bSizer12 = new wxBoxSizer(wxVERTICAL);

	m_staticText_task_desc = new wxStaticText(m_panel_model_name_content, wxID_ANY, wxT("Robort expose task dao movie with smart part \ndesigned for new year\n"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText_task_desc->Wrap(-1);
	bSizer12->Add(m_staticText_task_desc, 0, wxALL, 5);

	wxBoxSizer* bSizer13;
	bSizer13 = new wxBoxSizer(wxHORIZONTAL);

	m_staticText_ceation_time_title = new wxStaticText(m_panel_model_name_content, wxID_ANY, wxT("CreationTime:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText_ceation_time_title->Wrap(-1);
	bSizer13->Add(m_staticText_ceation_time_title, 0, wxALL, 5);

	m_staticText_creation_time = new wxStaticText(m_panel_model_name_content, wxID_ANY, wxT("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText_creation_time->Wrap(-1);
	bSizer13->Add(m_staticText_creation_time, 0, wxALL, 5);


	bSizer12->Add(bSizer13, 1, wxEXPAND, 5);


	bSizer11->Add(bSizer12, 1, 0, 5);


	bSizer_model_name_content->Add(bSizer11, 1, wxLEFT | wxRIGHT | wxEXPAND, 46);


	m_panel_model_name_content->SetSizer(bSizer_model_name_content);
	m_panel_model_name_content->Layout();
	bSizer_model_name_content->Fit(m_panel_model_name_content);
	bSizer_model_name->Add(m_panel_model_name_content, 1, wxALL | wxEXPAND, 0);


	fgSizer_tasklist_top->Add(bSizer_model_name, 0, wxLEFT, 38);

	wxBoxSizer* bSizer_plater;
	bSizer_plater = new wxBoxSizer(wxVERTICAL);

	bSizer_plater->SetMinSize(wxSize(496, -1));
	m_panel_plater_caption = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(496, 48), wxTAB_TRAVERSAL);
	m_panel_plater_caption->SetBackgroundColour(wxColour(248, 248, 248));

	wxBoxSizer* bSizer_plater_caption;
	bSizer_plater_caption = new wxBoxSizer(wxHORIZONTAL);

	bSizer_plater_caption->Add(23, 0, 0, wxEXPAND, 0);

	m_staticText_plater = new wxStaticText(m_panel_plater_caption, wxID_ANY, wxT("Plater"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText_plater->Wrap(-1);
	m_staticText_plater->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("HarmonyOS Sans SC")));

	bSizer_plater_caption->Add(m_staticText_plater, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);


	m_panel_plater_caption->SetSizer(bSizer_plater_caption);
	m_panel_plater_caption->Layout();
	bSizer_plater_caption->Fit(m_panel_plater_caption);
	bSizer_plater->Add(m_panel_plater_caption, 0, wxEXPAND | wxALL, 0);

	m_panel_plater_content = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(496, 439), wxTAB_TRAVERSAL);
	m_panel_plater_content->SetBackgroundColour(wxColour(255, 255, 255));

	wxBoxSizer* bSizer_tasklist;
	bSizer_tasklist = new wxBoxSizer(wxVERTICAL);

	fgSizer_subtask = new wxFlexGridSizer(100, 1, 10, 0);
	fgSizer_subtask->SetFlexibleDirection(wxBOTH);
	fgSizer_subtask->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

	bSizer_tasklist->Add(fgSizer_subtask, 0, wxALIGN_CENTER_HORIZONTAL, 0);

	m_panel_plater_content->SetSizer(bSizer_tasklist);
	m_panel_plater_content->Layout();
	bSizer_tasklist->Fit(m_panel_plater_content);
	bSizer_plater->Add(m_panel_plater_content, 1, wxEXPAND | wxALL, 0);


	fgSizer_tasklist_top->Add(bSizer_plater, 0, wxLEFT, 38);


	this->SetSizer(fgSizer_tasklist_top);
	this->Layout();
}

TaskListBasePanel::~TaskListBasePanel()
{
}


