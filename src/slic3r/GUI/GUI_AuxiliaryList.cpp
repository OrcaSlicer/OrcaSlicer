#include <wx/button.h>
#include "GUI_AuxiliaryList.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"

#include <boost/filesystem.hpp>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r::GUI;
using namespace Slic3r;

AuxiliaryList::AuxiliaryList(wxWindow* parent)
	: wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_NO_HEADER)
{
	wxDataViewTextRenderer* tr = new wxDataViewTextRenderer("string", wxDATAVIEW_CELL_INERT);
	wxDataViewColumn* column0 = new wxDataViewColumn("", tr, 0, 200, wxALIGN_LEFT,
		wxDATAVIEW_COL_SORTABLE | wxDATAVIEW_COL_RESIZABLE);
	this->AppendColumn(column0);

	m_auxiliary_model = new AuxiliaryModel();
	this->AssociateModel(m_auxiliary_model);
	m_sizer = new wxBoxSizer(wxVERTICAL);
	m_sizer->Add(this, 1, wxEXPAND | wxALL, 0);

	wxPanel* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(21)));
	//panel->SetBackgroundColour(*wxLIGHT_GREY);

#if 0
	wxBitmap if_bitmap = create_scaled_bitmap("import_file.png", nullptr, FromDIP(21));
	wxBitmap nf_bitmap = create_scaled_bitmap("new_folder.png", nullptr, FromDIP(21));
	wxBitmap del_bitmap = create_scaled_bitmap("delete.png", nullptr, FromDIP(21));

	wxBitmapButton* m_if_btn = new wxBitmapButton(panel, wxID_OPEN, if_bitmap);
	wxBitmapButton* m_nf_btn = new wxBitmapButton(panel, wxID_NEW, nf_bitmap);
	wxBitmapButton* m_del_btn = new wxBitmapButton(panel, wxID_DELETE, del_bitmap);
#endif

	//m_nf_btn = new wxButton(panel, wxID_NEW, _L("New Folder"));
	m_if_btn = new wxButton(panel, wxID_ADD, _L("Import File"));
	m_of_btn = new wxButton(panel, wxID_OPEN, _("Open File"));
	m_del_btn = new wxButton(panel, wxID_DELETE, _L("Delete"));

	wxBoxSizer* hsizer = new wxBoxSizer(wxHORIZONTAL);
	//hsizer->Add(m_nf_btn, 0, wxRIGHT, 5);
	hsizer->Add(m_if_btn, 0, wxLEFT | wxRIGHT, 5);
	hsizer->Add(m_of_btn, 0, wxLEFT | wxRIGHT, 5);
	hsizer->Add(m_del_btn, 0, wxLEFT | wxRIGHT, 5);
	panel->SetSizer(hsizer);

	m_sizer->Add(panel, 0, wxEXPAND | wxALL, 5);

	EnableDragSource(wxDF_UNICODETEXT);
	EnableDropTarget(wxDF_UNICODETEXT);

	// Keyboard events
	Bind(wxEVT_CHAR, [this](wxKeyEvent& event) { this->handle_key_event(event); });

	// Button events
	//m_nf_btn->Bind(wxEVT_BUTTON, &AuxiliaryList::on_create_folder, this, wxID_NEW);
	m_if_btn->Bind(wxEVT_BUTTON, &AuxiliaryList::on_import_file, this, wxID_ADD);
	m_del_btn->Bind(wxEVT_BUTTON, &AuxiliaryList::on_delete, this, wxID_DELETE);
	m_of_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
		wxDataViewItem sel_item = this->GetSelection();
		AuxiliaryModelNode* sel = (AuxiliaryModelNode*)sel_item.GetID();
		if (sel != nullptr && !sel->IsContainer()) {
			wxLaunchDefaultApplication(sel->path, 0);
		}
		else {
			evt.Skip();
		}
	}, wxID_OPEN);

	// Dataview events
	this->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, &AuxiliaryList::on_context_menu, this);
	this->Bind(wxEVT_DATAVIEW_ITEM_BEGIN_DRAG, &AuxiliaryList::on_begin_drag, this);
	this->Bind(wxEVT_DATAVIEW_ITEM_DROP_POSSIBLE, &AuxiliaryList::on_drop_possible, this);
	this->Bind(wxEVT_DATAVIEW_ITEM_DROP, &AuxiliaryList::on_drop, this);
	this->Bind(wxEVT_DATAVIEW_ITEM_EDITING_STARTED, &AuxiliaryList::on_editing_started, this);
	this->Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE, &AuxiliaryList::on_editing_done, this);

	// Mouse events
	wxWindow* win = this->GetMainWindow();
	win->Bind(wxEVT_LEFT_DCLICK, &AuxiliaryList::on_left_dclick, this);

	Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent& event) {
		wxDataViewItem sel_item = event.GetItem();
		AuxiliaryModelNode* sel_node = (AuxiliaryModelNode*)sel_item.GetID();
		if (sel_node == nullptr)
			return;

		m_del_btn->Enable(!sel_node->IsContainer());
	});
}

AuxiliaryList::~AuxiliaryList()
{
	this->AssociateModel(nullptr);
	delete m_auxiliary_model;
}

void AuxiliaryList::init_auxiliary()
{
	Model& model = wxGetApp().plater()->model();
	std::string aux_path = encode_path(model.get_auxiliary_file_temp_path().c_str());
	m_auxiliary_model->Init(aux_path);
}

void AuxiliaryList::reload(wxString aux_path)
{
	m_auxiliary_model->Reload(aux_path);

	wxDataViewItemArray items;
	m_auxiliary_model->GetChildren(wxDataViewItem(nullptr), items);
	for (wxDataViewItem item : items) {
		Expand(item);
	}
}

void AuxiliaryList::create_new_folder()
{
	wxDataViewItem folder_item = m_auxiliary_model->CreateFolder(wxEmptyString);
	AuxiliaryModelNode* folder = (AuxiliaryModelNode*)folder_item.GetID();
	if (folder == nullptr)
		return;

	Select(folder_item);

	wxDataViewColumn* col = GetColumn(0);
	wxDataViewCellMode mode = col->GetRenderer()->GetMode();
	col->GetRenderer()->SetMode(wxDATAVIEW_CELL_EDITABLE);
	EditItem(folder_item, col);
	col->GetRenderer()->SetMode(mode);
}

void AuxiliaryList::do_import_file(AuxiliaryModelNode* folder)
{
	if (folder == nullptr || !folder->IsContainer())
		return;

	wxString src_path;
	wxString dst_path;
	wxFileDialog dialog(this, _L("Choose files"), wxEmptyString, wxEmptyString,
		wxFileSelectorDefaultWildcardStr, wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
	if (dialog.ShowModal() == wxID_OK) {
		wxArrayString sel_paths;
		dialog.GetPaths(sel_paths);
		wxDataViewItemArray file_items = m_auxiliary_model->ImportFile(folder, sel_paths);
		if (!file_items.empty()) {
			wxDataViewItem file_item = file_items[0];
			AuxiliaryModelNode* file_node = (AuxiliaryModelNode*)file_item.GetID();
			if (file_node != nullptr) {
				if (!m_auxiliary_model->IsOrphan(file_item)) {
					Expand(wxDataViewItem(file_node->GetParent()));
				}
				Select(file_item);
				m_del_btn->Enable(true);
			}
		}
	}
}

void AuxiliaryList::on_create_folder(wxCommandEvent& evt)
{
	create_new_folder();
}

void AuxiliaryList::on_import_file(wxCommandEvent& evt)
{
	wxDataViewItem sel_item = this->GetSelection();
	AuxiliaryModelNode* sel_node = (AuxiliaryModelNode*)sel_item.GetID();
	if (sel_node == nullptr)
		return;

	AuxiliaryModelNode* folder_node = sel_node;
	if (!folder_node->IsContainer()) {
		wxDataViewItem folder_item = m_auxiliary_model->GetParent(sel_item);
		folder_node = (AuxiliaryModelNode*)folder_item.GetID();

		if (folder_node == nullptr)
			return;
	}

	do_import_file(folder_node);
}

void AuxiliaryList::on_delete(wxCommandEvent& evt)
{
	m_auxiliary_model->Delete(this->GetSelection());
}

void AuxiliaryList::on_context_menu(wxDataViewEvent& evt)
{
	wxMenu* menu = new wxMenu();
	wxDataViewItem item = evt.GetItem();
	AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
	if (node == nullptr) {
		append_menu_item(menu, wxID_ANY, _L("New Folder"), wxEmptyString,
			[this](wxCommandEvent&)
			{
				create_new_folder();
			});
	}
	else if (node->IsContainer()) {
		append_menu_item(menu, wxID_ANY, _L("Import File"), wxEmptyString,
			[this, node](wxCommandEvent&)
			{
				do_import_file(node);
			});
		append_menu_item(menu, wxID_ANY, _L("Delete"), wxEmptyString,
			[this, item](wxCommandEvent&)
			{
				m_auxiliary_model->Delete(item);
			});
	}
	else {
		append_menu_item(menu, wxID_ANY, _L("Open"), wxEmptyString,
			[this, node](wxCommandEvent&)
			{
				wxLaunchDefaultApplication(node->path, 0);
			});
		append_menu_item(menu, wxID_ANY, _L("Delete"), wxEmptyString,
			[this, item](wxCommandEvent&)
			{
				m_auxiliary_model->Delete(item);
			});
		append_menu_item(menu, wxID_ANY, _L("Rename"), wxEmptyString,
			[this, item](wxCommandEvent&)
			{
				wxDataViewColumn* col = this->GetColumn(0);
				wxDataViewCellMode mode = col->GetRenderer()->GetMode();
				col->GetRenderer()->SetMode(wxDATAVIEW_CELL_EDITABLE);
				this->EditItem(item, col);
				col->GetRenderer()->SetMode(mode);
			});
	}

	PopupMenu(menu);
}

void AuxiliaryList::on_begin_drag(wxDataViewEvent& evt)
{
	wxDataViewItem sel_item = evt.GetItem();
	AuxiliaryModelNode* sel = (AuxiliaryModelNode*)sel_item.GetID();
	if (sel == nullptr || sel->IsContainer())
		return;

	m_dragged_item = sel_item;

	wxTextDataObject* obj = new wxTextDataObject;
	obj->SetText("Some text");
	evt.SetDataObject(obj);
	evt.SetDragFlags(wxDrag_DefaultMove);
}

void AuxiliaryList::on_drop_possible(wxDataViewEvent& evt)
{
	evt.Allow();
}

void AuxiliaryList::on_drop(wxDataViewEvent& evt)
{
	m_auxiliary_model->MoveItem(evt.GetItem(), m_dragged_item);

	Expand(evt.GetItem());
	Select(m_dragged_item);
	m_dragged_item = wxDataViewItem(nullptr);
}

void AuxiliaryList::on_editing_started(wxDataViewEvent& evt)
{
}

void AuxiliaryList::on_editing_done(wxDataViewEvent& evt)
{
	bool is_done = m_auxiliary_model->Rename(evt.GetItem(), evt.GetValue().GetString());
	if (!is_done)
		evt.Veto();
}

void AuxiliaryList::on_left_dclick(wxMouseEvent& evt)
{
	wxDataViewItem sel_item = this->GetSelection();
	AuxiliaryModelNode* sel = (AuxiliaryModelNode*)sel_item.GetID();
	if (sel != nullptr && !sel->IsContainer()) {
		wxLaunchDefaultApplication(sel->path, 0);
	}
	else {
		evt.Skip();
	}
}

void AuxiliaryList::handle_key_event(wxKeyEvent& evt)
{
	if (evt.GetKeyCode() == WXK_DELETE || evt.GetKeyCode() == WXK_BACK)
		m_auxiliary_model->Delete(this->GetSelection());
}
