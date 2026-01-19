///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.1-0-g8feb16b3)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#pragma once

#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/statbmp.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/bmpbuttn.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/gbsizer.h>
#include <wx/splitter.h>
#include "Widgets/Button.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/AxisCtrlButton.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/StaticLine.hpp"
#include "wxMediaCtrl2.h"
#include "MediaPlayCtrl.h"

///////////////////////////////////////////////////////////////////////////

namespace Slic3r
{
	namespace GUI
	{
		///////////////////////////////////////////////////////////////////////////////
		/// Class MonitorBasePanel
		///////////////////////////////////////////////////////////////////////////////
		class MonitorBasePanel : public wxPanel
		{
		private:

		protected:
			wxSplitterWindow* m_splitter;
			wxPanel* m_panel_splitter_left;
			wxPanel* m_panel_printer;
			wxStaticBitmap* m_bitmap_printer;
			wxStaticBitmap* m_bitmap_arrow1;
			wxStaticText* m_staticText_machine_name;
			wxStaticText* m_staticText_capacity_val;
			StaticLine* m_staticline1;
			wxPanel* m_panel_status_tab;
			wxStaticText* m_staticText_status;
			wxStaticBitmap* m_bitmap_signal;
			wxStaticBitmap* m_bitmap_arrow2;
			StaticLine* m_staticline2;
			wxPanel* m_panel_time_lapse_tab;
			wxStaticText* m_staticText_time_lapse;
			wxStaticBitmap* m_bitmap_arrow3;
			StaticLine* m_staticline3;
			wxPanel* m_panel_video_tab;
			wxStaticText* m_staticText_video_monitoring;
			wxStaticBitmap* m_bitmap_arrow4;
			StaticLine* m_staticline4;
			wxPanel* m_panel_task_list_tab;
			wxStaticText* m_staticText_subtask_list;
			wxStaticBitmap* m_bitmap_arrow5;
			StaticLine* m_staticline5;
			wxPanel* m_panel_splitter_right;

			// Virtual event handlers, override them in your derived class
			virtual void m_splitterOnSplitterSashPosChanging(wxSplitterEvent& event) { event.Veto(); }
			virtual void on_printer_clicked(wxMouseEvent& event) { event.Skip(); }
			virtual void on_status(wxMouseEvent& event) { event.Skip(); }
			virtual void on_timelapse(wxMouseEvent& event) { event.Skip(); }
			virtual void on_video(wxMouseEvent& event) { event.Skip(); }
			virtual void on_tasklist(wxMouseEvent& event) { event.Skip(); }

		public:

			MonitorBasePanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(1440, 900), long style = wxTAB_TRAVERSAL, const wxString& name = wxEmptyString);

			~MonitorBasePanel();

			void m_splitterOnIdle(wxIdleEvent&)
			{
				m_splitter->SetSashPosition(182);
				m_splitter->Disconnect(wxEVT_IDLE, wxIdleEventHandler(MonitorBasePanel::m_splitterOnIdle), NULL, this);
			}

		};


		///////////////////////////////////////////////////////////////////////////////
		/// Class MediaFilePanel
		///////////////////////////////////////////////////////////////////////////////
		class MediaFilePanel;

		///////////////////////////////////////////////////////////////////////////////
		/// Class VideoMonitoringBasePanel
		///////////////////////////////////////////////////////////////////////////////
		class VideoMonitoringBasePanel : public wxPanel
		{
		private:

		protected:

		public:

			VideoMonitoringBasePanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(1258, 834), long style = wxTAB_TRAVERSAL, const wxString& name = wxEmptyString);

			~VideoMonitoringBasePanel();

		};

		///////////////////////////////////////////////////////////////////////////////
		/// Class TaskListBasePanel
		///////////////////////////////////////////////////////////////////////////////
		class TaskListBasePanel : public wxPanel
		{
		private:

		protected:
			wxPanel* m_panel_model_name_caption;
			wxStaticText* m_staticText_model_name;
			wxPanel* m_panel_model_name_content;
			wxStaticBitmap* m_bitmap_task;
			wxStaticText* m_staticText_task_desc;
			wxStaticText* m_staticText_ceation_time_title;
			wxStaticText* m_staticText_creation_time;
			wxPanel* m_panel_plater_caption;
			wxStaticText* m_staticText_plater;
			wxPanel* m_panel_plater_content;
			wxFlexGridSizer* fgSizer_subtask;

		public:

			TaskListBasePanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(1258, 834), long style = wxTAB_TRAVERSAL, const wxString& name = wxEmptyString);

			~TaskListBasePanel();

		};

	} // namespace GUI
} // namespace Slic3r

