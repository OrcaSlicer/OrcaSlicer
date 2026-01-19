#ifndef slic3r_GUI_AuxiliaryList_hpp_
#define slic3r_GUI_AuxiliaryList_hpp_

#include <map>
#include <vector>
#include <set>

#include <wx/bitmap.h>
#include <wx/dataview.h>
#include <wx/menu.h>
#include <wx/file.h>
#include <wx/dir.h>

#include "AuxiliaryDataViewModel.hpp"

class AuxiliaryList : public wxDataViewCtrl
{
public:
	AuxiliaryList(wxWindow* parent);
	~AuxiliaryList();
	wxSizer* get_top_sizer() { return m_sizer; }
	void init_auxiliary();
	void reload(wxString aux_path);

private:
	void do_import_file(AuxiliaryModelNode* folder);
	void on_create_folder(wxCommandEvent& evt);
	void on_import_file(wxCommandEvent& evt);
	void on_delete(wxCommandEvent& evt);
	void on_context_menu(wxDataViewEvent& evt);
	void on_begin_drag(wxDataViewEvent& evt);
	void on_drop_possible(wxDataViewEvent& evt);
	void on_drop(wxDataViewEvent& evt);
	void on_editing_started(wxDataViewEvent& evt);
	void on_editing_done(wxDataViewEvent& evt);
	void on_left_dclick(wxMouseEvent& evt);

	void create_new_folder();
	void handle_key_event(wxKeyEvent& evt);

	wxDataViewItem m_dragged_item;
	AuxiliaryModel* m_auxiliary_model;
	wxSizer* m_sizer;

	//wxButton* m_nf_btn;
	wxButton* m_if_btn;
	wxButton* m_of_btn;
	wxButton* m_del_btn;
};

#endif //slic3r_GUI_AuxiliaryList_hpp_

