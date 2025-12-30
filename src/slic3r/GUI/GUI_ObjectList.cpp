#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Factories.hpp"
//#include "GUI_ObjectLayers.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "BitmapComboBox.hpp"
#include "MainFrame.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "OptionsGroup.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Model.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "PartPlate.hpp"
#include "format.hpp"
#include "NotificationManager.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "SingleChoiceDialog.hpp"
#include "StepMeshDialog.hpp"


#include <vector>
#include <unordered_map>
#include <functional>
#include <boost/algorithm/string.hpp>
#include <wx/progdlg.h>
#include <wx/listbook.h>
#include <wx/numformatter.h>
#include <wx/headerctrl.h>

#include "slic3r/Utils/FixModelByWin10.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PrintConfig.hpp"

#ifdef __WXMSW__
#include "wx/uiaction.h"
#include <wx/renderer.h>
#endif /* __WXMSW__ */
#include "Gizmos/GLGizmoScale.hpp"

namespace Slic3r
{
namespace GUI
{

wxDEFINE_EVENT(EVT_OBJ_LIST_OBJECT_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_PARTPLATE_LIST_PLATE_SELECT, IntEvent);

static PrinterTechnology printer_technology()
{
    return wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
}

static const Selection& scene_selection()
{
    //BBS AssembleView canvas has its own selection
    if (wxGetApp().plater()->get_current_canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView)
        return wxGetApp().plater()->get_assmeble_canvas3D()->get_selection();
    
    return wxGetApp().plater()->get_view3D_canvas3D()->get_selection();
}

// Config from current edited printer preset
static DynamicPrintConfig& printer_config()
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().config;
}

static int filaments_count()
{
    return wxGetApp().filaments_cnt();
}

static void take_snapshot(const std::string& snapshot_name)
{
    Plater* plater = wxGetApp().plater();
    if (plater)
        plater->take_snapshot(snapshot_name);
}

class wxRenderer : public wxDelegateRendererNative
{
public:
    wxRenderer() : wxDelegateRendererNative(wxRendererNative::Get()) {}
    virtual void DrawItemSelectionRect(wxWindow *win, wxDC& dc, const wxRect& rect, int flags = 0) override
    {   // ORCA draw selection background to improve consistency between platforms
        dc.SetBrush(StateColor::darkModeColorFor(wxColour("#BFE1DE")));
        dc.DrawRectangle(rect);
        //GetGeneric().DrawItemSelectionRect(win, dc, rect, flags);
    }
    virtual void DrawFocusRect(        wxWindow *win, wxDC& dc, const wxRect& rect, int flags = 0) override
    {   // ORCA draw focus rectangle to improve consistency between platforms
        dc.SetPen(  StateColor::darkModeColorFor(wxColour("#009688")));
        dc.DrawRectangle(rect);
    }
    virtual void DrawTreeItemButton(   wxWindow *win, wxDC& dc, const wxRect& rect, int flags = 0) override
    {   // ORCA draw custom triangle to improve consistency between platforms
        dc.SetPen(  StateColor::darkModeColorFor(wxColour("#7C8282")));
        dc.SetBrush(StateColor::darkModeColorFor(wxColour("#7C8282")));
        bool expanded = (flags == wxCONTROL_EXPANDED || flags == (wxCONTROL_CURRENT | wxCONTROL_EXPANDED));
        wxRect r = rect;
        // stretch rectangle depends on orientation
        r.Deflate((expanded ? wxSize(4, 6) : wxSize(6, 4)) * (wxGetApp().em_unit() * .1));
        wxPoint triangle[3];
        triangle[0] = wxPoint(r.x, r.y);
        triangle[1] = triangle[0] + wxPoint(r.width, expanded ? 0 :r.height/2);
        triangle[2] = triangle[0] + wxPoint(expanded ? r.width/2 : 0, r.height);
        dc.DrawPolygon(3, &triangle[0]);
    }
    virtual void DrawItemText(
        wxWindow* win,
        wxDC& dc,
        const wxString& text,
        const wxRect& rect,
        int align = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL,
        int flags = 0, // wxCONTROL_SELECTED wxCONTROL_FOCUSED wxCONTROL_DISABLED 
        wxEllipsizeMode ellipsizeMode = wxELLIPSIZE_END
    ) override
    {   // ORCA draw custom text to improve consistency between platforms
        //dc.SetFont(win->GetFont()); Without SetFont it pulls font from window
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30"))); // use same color for selected / non-selected
        dc.DrawText(text,wxPoint(rect.x, rect.y));
    }
};

ObjectList::ObjectList(wxWindow* parent) :
    wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_MULTIPLE | wxNO_BORDER | wxDV_NO_HEADER) // ORCA: Remove border and header
{
    wxGetApp().UpdateDVCDarkUI(this, true);

#ifdef __linux__
    // Temporary fix for incorrect dark mode application regarding list item's text color.
    // See: https://github.com/OrcaSlicer/OrcaSlicer/issues/2086
    this->SetForegroundColour(*wxBLACK);
#endif

    SetFont(Label::sysFont(13));
#ifdef __WXMSW__
    static auto render = new wxRenderer;
    wxRendererNative::Set(render);
#endif

    // create control
    create_objects_ctrl();

    //BBS: add part plate related event
    //Bind(EVT_PARTPLATE_LIST_PLATE_SELECT, &ObjectList::on_select_plate, this);

    // describe control behavior
    Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent& event) {
        // detect the current mouse position here, to pass it to list_manipulation() method
        // if we detect it later, the user may have moved the mouse pointer while calculations are performed, and this would mess-up the HitTest() call performed into list_manipulation()
        if (!GetScreenRect().Contains(wxGetMousePosition())) {
            return;
        }
#ifndef __WXOSX__
        const wxPoint mouse_pos = this->get_mouse_position_in_control();
#endif

#ifndef __APPLE__
        // On Windows and Linux:
        // It's not invoked KillFocus event for "temporary" panels (like "Manipulation panel", "Settings", "Layer ranges"),
        // if we change selection in object list.
        // But, if we call SetFocus() for ObjectList it will cause an invoking of a KillFocus event for "temporary" panels
        this->SetFocus();
#else
        // To avoid selection update from SetSelection() and UnselectAll() under osx
        if (m_prevent_list_events)
            return;
#endif // __APPLE__

        /* For multiple selection with pressed SHIFT,
         * event.GetItem() returns value of a first item in selection list
         * instead of real last clicked item.
         * So, let check last selected item in such strange way
         */
#ifdef __WXMSW__
		// Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
		int new_selected_column = -1;
#endif //__WXMSW__
        if (wxGetKeyState(WXK_SHIFT))
        {
            wxDataViewItemArray sels;
            GetSelections(sels);
            if (! sels.empty() && sels.front() == m_last_selected_item)
                m_last_selected_item = sels.back();
            else
                m_last_selected_item = event.GetItem();
        }
        else {
  	      	wxDataViewItem    new_selected_item  = event.GetItem();
            // BBS: use wxDataViewCtrl's internal mechanism
#if 0
#ifdef __WXMSW__
			// Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
		    wxDataViewItem    item;
		    wxDataViewColumn *col;
		    this->HitTest(this->get_mouse_position_in_control(), item, col);
		    new_selected_column = (col == nullptr) ? -1 : (int)col->GetModelColumn();
	        if (new_selected_item == m_last_selected_item && m_last_selected_column != -1 && m_last_selected_column != new_selected_column) {
	        	// Mouse clicked on another column of the active row. Simulate keyboard enter to enter the editing mode of the current column.
	        	wxUIActionSimulator sim;
				sim.Char(WXK_RETURN);
	        }
#endif //__WXMSW__
#endif
	        m_last_selected_item = new_selected_item;
        }
#ifdef __WXMSW__
        m_last_selected_column = new_selected_column;
#endif //__WXMSW__

        ObjectDataViewModelNode* sel_node = (ObjectDataViewModelNode*)event.GetItem().GetID();
        if (sel_node && (sel_node->GetType() & ItemType::itPlate)) {
            if (wxGetApp().plater()->is_preview_shown()) {
                wxGetApp().plater()->select_sliced_plate(sel_node->GetPlateIdx());
            } else {
                wxGetApp().plater()->select_plate(sel_node->GetPlateIdx());
            }
            wxGetApp().plater()->deselect_all();
        }
        else {
            selection_changed();
        }
#ifndef __WXMSW__
        set_tooltip_for_item(this->get_mouse_position_in_control());
#endif //__WXMSW__

#ifndef __WXOSX__
        list_manipulation(mouse_pos);
#endif //__WXOSX__
    });

#ifdef __WXOSX__
    // Key events are not correctly processed by the wxDataViewCtrl on OSX.
    // Our patched wxWidgets process the keyboard accelerators.
    // On the other hand, using accelerators will break in-place editing on Windows & Linux/GTK (there is no in-place editing working on OSX for wxDataViewCtrl for now).
//    Bind(wxEVT_KEY_DOWN, &ObjectList::OnChar, this);
    {
        // Accelerators
        // 	wxAcceleratorEntry entries[25];
        wxAcceleratorEntry entries[26];
        int index = 0;
        entries[index++].Set(wxACCEL_CTRL, (int)'C', wxID_COPY);
        entries[index++].Set(wxACCEL_CTRL, (int)'X', wxID_CUT);
        entries[index++].Set(wxACCEL_CTRL, (int)'V', wxID_PASTE);
        entries[index++].Set(wxACCEL_CTRL, (int)'M', wxID_DUPLICATE);
        entries[index++].Set(wxACCEL_CTRL, (int)'A', wxID_SELECTALL);
        entries[index++].Set(wxACCEL_CTRL, (int)'Z', wxID_UNDO);
        entries[index++].Set(wxACCEL_CTRL, (int)'Y', wxID_REDO);
        entries[index++].Set(wxACCEL_NORMAL, WXK_BACK, wxID_DELETE);
        //entries[index++].Set(wxACCEL_NORMAL, int('+'), wxID_ADD);
        //entries[index++].Set(wxACCEL_NORMAL, WXK_NUMPAD_ADD, wxID_ADD);
        //entries[index++].Set(wxACCEL_NORMAL, int('-'), wxID_REMOVE);
        //entries[index++].Set(wxACCEL_NORMAL, WXK_NUMPAD_SUBTRACT, wxID_REMOVE);
        //entries[index++].Set(wxACCEL_NORMAL, int('p'), wxID_PRINT);

        int numbers_cnt = 0;
        for (auto char_number : { '1', '2', '3', '4', '5', '6', '7', '8', '9' }) {
            entries[index + numbers_cnt].Set(wxACCEL_NORMAL, int(char_number), wxID_LAST + numbers_cnt+1);
            entries[index + 9 + numbers_cnt].Set(wxACCEL_NORMAL, WXK_NUMPAD0 + numbers_cnt - 1, wxID_LAST + numbers_cnt+1);
            numbers_cnt++;
            // index++;
        }
        wxAcceleratorTable accel(26, entries);
        SetAcceleratorTable(accel);

        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->copy();                      }, wxID_COPY);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->paste();                     }, wxID_PASTE);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->select_item_all_children();  }, wxID_SELECTALL);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->remove();                    }, wxID_DELETE);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->undo();  					}, wxID_UNDO);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->redo();                    	}, wxID_REDO);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->cut();                    	}, wxID_CUT);
        this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->clone();                    	}, wxID_DUPLICATE);
        //this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->increase_instances();        }, wxID_ADD);
        //this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->decrease_instances();        }, wxID_REMOVE);
        //this->Bind(wxEVT_MENU, [this](wxCommandEvent &evt) { this->toggle_printable_state();    }, wxID_PRINT);

        for (int i = 1; i < 10; i++)
            this->Bind(wxEVT_MENU, [this, i](wxCommandEvent &evt) {
                if (filaments_count() > 1 && i <= filaments_count())
                    this->set_extruder_for_selected_items(i);
            }, wxID_LAST+i);

        m_accel = accel;
    }
#else //__WXOSX__
    Bind(wxEVT_CHAR, [this](wxKeyEvent& event) { key_event(event); }); // doesn't work on OSX
#endif

#ifdef __WXMSW__
    GetMainWindow()->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        // BBS
        // this->SetFocus();
        set_tooltip_for_item(this->get_mouse_position_in_control());
        event.Skip();
    });
#endif //__WXMSW__

    Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,  &ObjectList::OnContextMenu,     this);

    // BBS
    Bind(wxEVT_DATAVIEW_ITEM_BEGIN_DRAG,    &ObjectList::OnBeginDrag,       this);
    Bind(wxEVT_DATAVIEW_ITEM_DROP_POSSIBLE, &ObjectList::OnDropPossible,    this);
    Bind(wxEVT_DATAVIEW_ITEM_DROP,          &ObjectList::OnDrop,            this);

    Bind(wxEVT_DATAVIEW_ITEM_START_EDITING, &ObjectList::OnStartEditing, this);
    Bind(wxEVT_DATAVIEW_ITEM_EDITING_STARTED, &ObjectList::OnEditingStarted,  this);
    Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE,    &ObjectList::OnEditingDone,     this);

    Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, &ObjectList::ItemValueChanged,  this);

    // BBS: dont need to do extra setting for a deleted object
    //Bind(wxCUSTOMEVT_LAST_VOLUME_IS_DELETED, [this](wxCommandEvent& e)   { last_volume_is_deleted(e.GetInt()); });

    Bind(wxEVT_SIZE, ([this](wxSizeEvent &e) {
             if (m_last_size == this->GetSize()) {
                 e.Skip();
                 return;
             } else {
                 m_last_size = this->GetSize();
             }

        update_name_column_width();

        // BBS
        this->CallAfter([this]() { ensure_current_item_visible(); });

        e.Skip();
    }));
    m_last_size = this->GetSize();
}

ObjectList::~ObjectList()
{
    if (m_objects_model)
            m_objects_model->DecRef();
}

void ObjectList::set_min_height()
{
    // BBS
#if 0
    if (m_items_count == size_t(-1))
        m_items_count = 7;
    int list_min_height = lround(2.25 * (m_items_count + 1) * wxGetApp().em_unit()); // +1 is for height of control header
    this->SetMinSize(wxSize(1, list_min_height));
#endif
}

void ObjectList::update_min_height()
{
    wxDataViewItemArray all_items;
    m_objects_model->GetAllChildren(wxDataViewItem(nullptr), all_items);
    size_t items_cnt = all_items.Count();
#if 0
    if (items_cnt < 7)
        items_cnt = 7;
    else if (items_cnt >= 15)
        items_cnt = 15;
#else
    items_cnt = 8;
#endif

    if (m_items_count == items_cnt)
        return;

    m_items_count = items_cnt;
    set_min_height();
}


void ObjectList::create_objects_ctrl()
{
    // BBS
#if 0
    /* Temporary workaround for the correct behavior of the Scrolled sidebar panel:
     * 1. set a height of the list to some big value
     * 2. change it to the normal(meaningful) min value after first whole Mainframe updating/layouting
     */
    SetMinSize(wxSize(-1, 3000));
#endif

    m_objects_model = new ObjectDataViewModel;
    AssociateModel(m_objects_model);
    m_objects_model->SetAssociatedControl(this);
#if wxUSE_DRAG_AND_DROP && wxUSE_UNICODE
    EnableDragSource(wxDF_UNICODETEXT);
    EnableDropTarget(wxDF_UNICODETEXT);
#endif // wxUSE_DRAG_AND_DROP && wxUSE_UNICODE

    const int em = wxGetApp().em_unit();

    m_columns_width.resize(colCount);
    m_columns_width[colName] = 22;
    m_columns_width[colHeight] = 3;
    m_columns_width[colPrint] = 3;
    m_columns_width[colFilament] = 5;
    m_columns_width[colSupportPaint] = 3;
    m_columns_width[colSinking] = 3;
    m_columns_width[colColorPaint] = 3;
    m_columns_width[colEditing] = 3;

    // column ItemName(Icon+Text) of the view control:
    // And Icon can be consisting of several bitmaps
    BitmapTextRenderer* bmp_text_renderer = new BitmapTextRenderer();
    bmp_text_renderer->set_can_create_editor_ctrl_function([this]() {
        auto type = m_objects_model->GetItemType(GetSelection());
        return type & (itVolume | itObject | itPlate);
    });

    // BBS
    wxDataViewColumn* name_col = new wxDataViewColumn(_L("Name"), bmp_text_renderer,
        colName, m_columns_width[colName] * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    //name_col->SetBitmap(create_scaled_bitmap("organize", nullptr, FromDIP(18)));
    AppendColumn(name_col);

    // column Variable height Property (Icon) of the view control:
    AppendBitmapColumn(" ", colHeight, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, 3 * em,
        wxALIGN_CENTER_HORIZONTAL, 0);

    // column PrintableProperty (Icon) of the view control:
    AppendBitmapColumn(" ", colPrint, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, m_columns_width[colPrint]*em,
        wxALIGN_CENTER_HORIZONTAL, 0);

    // column Extruder of the view control:
    BitmapChoiceRenderer* bmp_choice_renderer = new BitmapChoiceRenderer();
    bmp_choice_renderer->set_can_create_editor_ctrl_function([this]() {
        return m_objects_model->GetItemType(GetSelection()) & (itVolume | itLayer | itObject);
    });
    bmp_choice_renderer->set_default_extruder_idx([this]() {
        return m_objects_model->GetDefaultExtruderIdx(GetSelection());
    });
    bmp_choice_renderer->set_has_default_extruder([this]() {
        return m_objects_model->GetVolumeType(GetSelection()) == ModelVolumeType::PARAMETER_MODIFIER ||
               m_objects_model->GetItemType(GetSelection()) == itLayer;
    });
    AppendColumn(new wxDataViewColumn(_L("Fila."), bmp_choice_renderer,
        colFilament, m_columns_width[colFilament] * em, wxALIGN_CENTER_HORIZONTAL, 0));

    // BBS
    AppendBitmapColumn(" ", colSupportPaint, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, m_columns_width[colSupportPaint] * em,
        wxALIGN_CENTER_HORIZONTAL, 0);
    AppendBitmapColumn(" ", colColorPaint, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, m_columns_width[colColorPaint] * em,
        wxALIGN_CENTER_HORIZONTAL, 0);
    AppendBitmapColumn(" ", colSinking, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, m_columns_width[colSinking] * em,
        wxALIGN_CENTER_HORIZONTAL, 0);

    // column ItemEditing of the view control:
    AppendBitmapColumn(" ", colEditing, wxOSX ? wxDATAVIEW_CELL_EDITABLE : wxDATAVIEW_CELL_INERT, m_columns_width[colEditing] * em,
        wxALIGN_CENTER_HORIZONTAL, 0);

    //for (int cn = colName; cn < colCount; cn++) {
    //    GetColumn(cn)->SetResizeable(cn == colName);
    //}

    // For some reason under OSX on 4K(5K) monitors in wxDataViewColumn constructor doesn't set width of column.
    // Therefore, force set column width.
#ifdef __WXOSX__
        dataview_remove_insets(this);
        for (int cn = colName; cn < colCount; cn++)
            GetColumn(cn)->SetWidth(m_columns_width[cn] * em);
#endif
}

void ObjectList::get_selected_item_indexes(int& obj_idx, int& vol_idx, const wxDataViewItem& input_item/* = wxDataViewItem(nullptr)*/)
{
    const wxDataViewItem item = input_item == wxDataViewItem(nullptr) ? GetSelection() : input_item;

    if (!item)
    {
        obj_idx = vol_idx = -1;
        return;
    }

    const ItemType type = m_objects_model->GetItemType(item);

    obj_idx =   type & itObject ? m_objects_model->GetIdByItem(item) :
                type & itVolume ? m_objects_model->GetIdByItem(m_objects_model->GetObject(item)) : -1;

    vol_idx =   type & itVolume ? m_objects_model->GetVolumeIdByItem(item) : -1;
}

void ObjectList::get_selection_indexes(std::vector<int>& obj_idxs, std::vector<int>& vol_idxs)
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return;

    if ( m_objects_model->GetItemType(sels[0]) & itVolume ||
        (sels.Count()==1 && m_objects_model->GetItemType(m_objects_model->GetParent(sels[0])) & itVolume) ) {
        for (wxDataViewItem item : sels) {
            obj_idxs.emplace_back(m_objects_model->GetIdByItem(m_objects_model->GetObject(item)));

            if (sels.Count() == 1 && m_objects_model->GetItemType(m_objects_model->GetParent(item)) & itVolume)
                item = m_objects_model->GetParent(item);

            assert(m_objects_model->GetItemType(item) & itVolume);
            vol_idxs.emplace_back(m_objects_model->GetVolumeIdByItem(item));
        }
    }
    else {
        for (wxDataViewItem item : sels) {
            const ItemType type = m_objects_model->GetItemType(item);
            obj_idxs.emplace_back(type & itObject ? m_objects_model->GetIdByItem(item) :
                                  m_objects_model->GetIdByItem(m_objects_model->GetObject(item)));
        }
    }

    std::sort(obj_idxs.begin(), obj_idxs.end(), std::less<int>());
    obj_idxs.erase(std::unique(obj_idxs.begin(), obj_idxs.end()), obj_idxs.end());
}

int ObjectList::get_repaired_errors_count(const int obj_idx, const int vol_idx /*= -1*/) const
{
    return obj_idx >= 0 ? (*m_objects)[obj_idx]->get_repaired_errors_count(vol_idx) : 0;
}

static std::string get_warning_icon_name(const TriangleMeshStats& stats)
{
    return stats.manifold() ? (stats.repaired() ? "obj_warning" : "") : "obj_warning";
}

MeshErrorsInfo ObjectList::get_mesh_errors_info(const int obj_idx, const int vol_idx /*= -1*/, wxString* sidebar_info /*= nullptr*/, int* non_manifold_edges) const
{
    if (obj_idx < 0)
        return { {}, {} }; // hide tooltip

    const TriangleMeshStats& stats = vol_idx == -1 ?
        (*m_objects)[obj_idx]->get_object_stl_stats() :
        (*m_objects)[obj_idx]->volumes[vol_idx]->mesh().stats();

    if (!stats.repaired() && stats.manifold()) {
        //if (sidebar_info)
        //    *sidebar_info = _L("No errors");
        return { {}, {} }; // hide tooltip
    }

    wxString tooltip, auto_repaired_info, remaining_info;

    // Create tooltip string, if there are errors
    if (stats.repaired()) {
        const int errors = get_repaired_errors_count(obj_idx, vol_idx);
        auto_repaired_info = format_wxstr(_L_PLURAL("%1$d error repaired", "%1$d errors repaired", errors), errors);
        tooltip += auto_repaired_info + "\n";
    }
    if (!stats.manifold()) {
        remaining_info = format_wxstr(_L_PLURAL("Error: %1$d non-manifold edge.", "Error: %1$d non-manifold edges.", stats.open_edges), stats.open_edges);

        tooltip += _L("Remaining errors") + ":\n";
        tooltip += "\t" + format_wxstr(_L_PLURAL("%1$d non-manifold edge", "%1$d non-manifold edges", stats.open_edges), stats.open_edges) + "\n";
    }

    if (sidebar_info) {
        *sidebar_info = stats.manifold() ? auto_repaired_info : (remaining_info + (stats.repaired() ? ("\n" + auto_repaired_info) : ""));
    }

    if (non_manifold_edges)
        *non_manifold_edges = stats.open_edges;

    if (is_windows10() && !sidebar_info)
        tooltip += "\n" + _L("Click the icon to repair model object");

    return { tooltip, get_warning_icon_name(stats) };
}

MeshErrorsInfo ObjectList::get_mesh_errors_info(wxString* sidebar_info /*= nullptr*/, int* non_manifold_edges)
{
    wxDataViewItem item = GetSelection();
    if (!item)
        return { "", "" };

    int obj_idx, vol_idx;
    get_selected_item_indexes(obj_idx, vol_idx);

    if (obj_idx < 0) { // child of ObjectItem is selected
        if (sidebar_info)
            obj_idx = m_objects_model->GetObjectIdByItem(item);
        else
            return { "", "" };
    }
    return get_mesh_errors_info(obj_idx, vol_idx, sidebar_info, non_manifold_edges);
}

void ObjectList::set_tooltip_for_item(const wxPoint& pt)
{
    wxDataViewItem item;
    wxDataViewColumn* col;
    HitTest(pt, item, col);

    /* GetMainWindow() return window, associated with wxDataViewCtrl.
     * And for this window we should to set tooltips.
     * Just this->SetToolTip(tooltip) => has no effect.
     */

    if (!item || GetSelectedItemsCount() > 1)
    {
        GetMainWindow()->SetToolTip(""); // hide tooltip
        return;
    }

    wxString tooltip = "";
    ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();

    if (col->GetModelColumn() == (unsigned int)colEditing) {
        if (node->IsActionEnabled())
#ifdef __WXOSX__
            tooltip = _(L("Right button click the icon to drop the object settings"));
#else
            tooltip = _(L("Click the icon to reset all settings of the object"));
#endif //__WXMSW__
    }
    else if (col->GetModelColumn() == (unsigned int)colPrint)
#ifdef __WXOSX__
        tooltip = _(L("Right button click the icon to drop the object printable property"));
#else
        tooltip = _(L("Click the icon to toggle printable property of the object"));
#endif //__WXMSW__
    // BBS
    else if (col->GetModelColumn() == (unsigned int)colSupportPaint) {
        if (node->HasSupportPainting())
            tooltip = _(L("Click the icon to edit support painting of the object"));

    }
    else if (col->GetModelColumn() == (unsigned int)colColorPaint) {
        if (node->HasColorPainting())
            tooltip = _(L("Click the icon to edit color painting of the object"));
    }
    else if (col->GetModelColumn() == (unsigned int)colSinking) {
        if (node->HasSinking())
            tooltip = _(L("Click the icon to shift this object to the bed"));
    }
    else if (col->GetModelColumn() == (unsigned int)colName && (pt.x >= 2 * wxGetApp().em_unit() && pt.x <= 4 * wxGetApp().em_unit()))
    {
        if (const ItemType type = m_objects_model->GetItemType(item);
            type & (itObject | itVolume)) {
            int obj_idx = m_objects_model->GetObjectIdByItem(item);
            int vol_idx = type & itVolume ? m_objects_model->GetVolumeIdByItem(item) : -1;
            tooltip = get_mesh_errors_info(obj_idx, vol_idx).tooltip;
        }
    }

    GetMainWindow()->SetToolTip(tooltip);
}

int ObjectList::get_selected_obj_idx() const
{
    if (GetSelectedItemsCount() == 1)
        return m_objects_model->GetIdByItem(m_objects_model->GetObject(GetSelection()));

    return -1;
}

ModelConfig& ObjectList::get_item_config(const wxDataViewItem& item) const
{
    static ModelConfig s_empty_config;

    assert(item);
    const ItemType type = m_objects_model->GetItemType(item);

    if (type & itPlate)
        return s_empty_config;

    const int obj_idx = m_objects_model->GetObjectIdByItem(item);
    const int vol_idx = type & itVolume ? m_objects_model->GetVolumeIdByItem(item) : -1;

    assert(obj_idx >= 0 || ((type & itVolume) && vol_idx >=0));
    return type & itVolume ?(*m_objects)[obj_idx]->volumes[vol_idx]->config :
           type & itLayer  ?(*m_objects)[obj_idx]->layer_config_ranges[m_objects_model->GetLayerRangeByItem(item)] :
                            (*m_objects)[obj_idx]->config;
}

void ObjectList::update_filament_values_for_items(const size_t filaments_count)
{
    for (size_t i = 0; i < m_objects->size(); ++i)
    {
        wxDataViewItem item = m_objects_model->GetItemById(i);
        if (!item) continue;

        auto object = (*m_objects)[i];
        wxString extruder;
        if (!object->config.has("extruder") || size_t(object->config.extruder()) > filaments_count) {
            extruder = "1";
            object->config.set_key_value("extruder", new ConfigOptionInt(1));
        }
        else {
            extruder = wxString::Format("%d", object->config.extruder());
        }
        m_objects_model->SetExtruder(extruder, item);

        static const char *keys[] = {"support_filament", "support_interface_filament"};
        for (auto key : keys)
            if (object->config.has(key) && object->config.opt_int(key) > filaments_count)
                object->config.erase(key);

        if (object->volumes.size() > 1) {
            for (size_t id = 0; id < object->volumes.size(); id++) {
                item = m_objects_model->GetItemByVolumeId(i, id);
                if (!item) continue;
                if (!object->volumes[id]->config.has("extruder") ||
                    size_t(object->volumes[id]->config.extruder()) > filaments_count) {
                    extruder = wxString::Format("%d", object->config.extruder());
                }
                else {
                    extruder = wxString::Format("%d", object->volumes[id]->config.extruder());
                }

                m_objects_model->SetExtruder(extruder, item);

                for (auto key : keys)
                    if (object->volumes[id]->config.has(key) && object->volumes[id]->config.opt_int(key) > filaments_count)
                        object->volumes[id]->config.erase(key);
            }
        }
    }

    // BBS
    wxGetApp().plater()->update();
}

void ObjectList::update_filament_values_for_items_when_delete_filament(const size_t filament_id, const int replace_id)
{
    int replace_filament_id = replace_id == -1 ? 1 : (replace_id + 1);
    for (size_t i = 0; i < m_objects->size(); ++i) {
        wxDataViewItem item = m_objects_model->GetItemById(i);
        if (!item)
            continue;

        auto     object = (*m_objects)[i];
        wxString extruder;
        if (!object->config.has("extruder")) {
            extruder = std::to_string(1);
            object->config.set_key_value("extruder", new ConfigOptionInt(1));
        }
        else if (size_t(object->config.extruder()) == filament_id + 1) {
            extruder = std::to_string(replace_filament_id);
            object->config.set_key_value("extruder", new ConfigOptionInt(replace_filament_id));
        } else {
            int new_extruder = object->config.extruder() > filament_id ? object->config.extruder() - 1 : object->config.extruder();
            extruder = wxString::Format("%d", new_extruder);
            object->config.set_key_value("extruder", new ConfigOptionInt(new_extruder));
        }
        m_objects_model->SetExtruder(extruder, item);

        static const char *keys[] = {"support_filament", "support_interface_filament"};
        for (auto key : keys) {
            if (object->config.has(key)) {
                if(object->config.opt_int(key) == filament_id + 1)
                    object->config.erase(key);
                else {
                    int new_value = object->config.opt_int(key) > filament_id ? object->config.opt_int(key) - 1 : object->config.opt_int(key);
                    object->config.set_key_value(key, new ConfigOptionInt(new_value));
                }
            }
        }

        //if (object->volumes.size() > 1) {
            for (size_t id = 0; id < object->volumes.size(); id++) {
                item = m_objects_model->GetItemByVolumeId(i, id);
                if (!item)
                    continue;

                for (auto key : keys) {
                    if (object->volumes[id]->config.has(key)) {
                        if (object->volumes[id]->config.opt_int(key) == filament_id + 1)
                            object->volumes[id]->config.erase(key);
                        else {
                            int new_value = object->volumes[id]->config.opt_int(key) > filament_id ? object->volumes[id]->config.opt_int(key) - 1 :
                                                                                                     object->volumes[id]->config.opt_int(key);
                            object->config.set_key_value(key, new ConfigOptionInt(new_value));
                        }
                    }
                }

                if (!object->volumes[id]->config.has("extruder")) {
                    continue;
                }
                else if (size_t(object->volumes[id]->config.extruder()) == filament_id + 1) {
                    object->volumes[id]->config.set_key_value("extruder", new ConfigOptionInt(replace_filament_id));
                } else {
                    int new_extruder = object->volumes[id]->config.extruder() > filament_id ? object->volumes[id]->config.extruder() - 1 : object->volumes[id]->config.extruder();
                    extruder = wxString::Format("%d", new_extruder);
                    object->volumes[id]->config.set_key_value("extruder", new ConfigOptionInt(new_extruder));
                }

                m_objects_model->SetExtruder(extruder, item);
            }
        //}

        item = m_objects_model->GetItemById(i);
        ObjectDataViewModelNode *object_node = static_cast<ObjectDataViewModelNode *>(item.GetID());
        if (object_node->GetChildCount() == 0)
            continue;

        // update height_range
        for (size_t i = 0; i < object_node->GetChildCount(); i++) {
            ObjectDataViewModelNode * layer_root_node = object_node->GetNthChild(i);
            if (layer_root_node->GetType() != ItemType::itLayerRoot)
                continue;
            for (size_t j = 0; j < layer_root_node->GetChildCount(); j++) {
                ObjectDataViewModelNode * layer_node = layer_root_node->GetNthChild(j);
                auto layer_item = wxDataViewItem((void *) layer_root_node->GetNthChild(j));
                if (!layer_item)
                    continue;
                auto l_iter = object->layer_config_ranges.find(layer_node->GetLayerRange());
                if (l_iter != object->layer_config_ranges.end()) {
                    auto& layer_range_item = *(l_iter);
                    if (layer_range_item.second.has("extruder") && layer_range_item.second.option("extruder")->getInt() == filament_id + 1) {
                        int new_extruder = replace_id == -1 ? 0 : (replace_id + 1);
                        extruder         = wxString::Format("%d", new_extruder);
                        layer_range_item.second.set("extruder", new_extruder);
                    } else {
                        int layer_filament_id = layer_range_item.second.option("extruder")->getInt();
                        int new_extruder      = layer_filament_id > filament_id ? layer_filament_id - 1 : layer_filament_id;
                        extruder              = wxString::Format("%d", new_extruder);
                        layer_range_item.second.set("extruder", new_extruder);
                    }
                    m_objects_model->SetExtruder(extruder, layer_item);
                }
            }
        }
    }
}


void ObjectList::update_plate_values_for_items()
{
#ifdef __WXOSX__
    AssociateModel(nullptr);
#endif
    PartPlateList& list = wxGetApp().plater()->get_partplate_list();
    for (size_t i = 0; i < m_objects->size(); ++i)
    {
        wxDataViewItem item = m_objects_model->GetItemById(i);
        if (!item) continue;

        int plate_idx = list.find_instance_belongs(i, 0);
        wxDataViewItem old_parent = m_objects_model->GetParent(item);
        ObjectDataViewModelNode* old_parent_node = (ObjectDataViewModelNode*)old_parent.GetID();
        int old_plate_idx = old_parent_node->GetPlateIdx();
        if (plate_idx == old_plate_idx)
            continue;

        // hotfix for wxDataViewCtrl selection not updated after wxDataViewModel::ItemDeleted()
        Unselect(item);

        bool is_old_parent_expanded = IsExpanded(old_parent);
        bool is_expanded = IsExpanded(item);
        m_objects_model->OnPlateChange(plate_idx, item);
        if (is_old_parent_expanded)
            Expand(old_parent);
        ExpandAncestors(item);
        Expand(item);
        Select(item);
    }
#ifdef __WXOSX__
    AssociateModel(m_objects_model);
#endif
}

// BBS
void ObjectList::update_name_for_items()
{
    m_objects_model->UpdateItemNames();

    wxGetApp().plater()->update();
}

void ObjectList::object_config_options_changed(const ObjectVolumeID& ov_id)
{
    if (ov_id.object == nullptr)
        return;

    ModelObjectPtrs& objects = wxGetApp().model().objects;
    ModelObject* mo = ov_id.object;
    ModelVolume* mv = ov_id.volume;

    wxDataViewItem obj_item = m_objects_model->GetObjectItem(mo);
    if (mv != nullptr) {
        size_t vol_idx;
        for (vol_idx = 0; vol_idx < mo->volumes.size(); vol_idx++) {
            if (mo->volumes[vol_idx] == mv)
                break;
        }
        assert(vol_idx < mo->volumes.size());

        SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&mv->config.get(), false);
        wxDataViewItem vol_item = m_objects_model->GetVolumeItem(obj_item, vol_idx);
        if (cat_options.size() > 0) {
            add_settings_item(vol_item, &mv->config.get());
        }
        else {
            m_objects_model->DeleteSettings(vol_item);
        }
    }
    else {
        SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&mo->config.get(), true);
        if (cat_options.size() > 0) {
            add_settings_item(obj_item, &mo->config.get());
        }
        else {
            m_objects_model->DeleteSettings(obj_item);
        }
    }
}

void ObjectList::printable_state_changed(const std::vector<ObjectVolumeID>& ov_ids)
{
    std::vector<size_t> obj_idxs;
    for (const ObjectVolumeID ov_id : ov_ids) {
        if (ov_id.object == nullptr)
            continue;

        ModelInstance* mi = ov_id.object->instances[0];
        wxDataViewItem obj_item = m_objects_model->GetObjectItem(ov_id.object);
        m_objects_model->SetObjectPrintableState(mi->printable ? piPrintable : piUnprintable, obj_item);

        int obj_idx = m_objects_model->GetObjectIdByItem(obj_item);
        obj_idxs.emplace_back(static_cast<size_t>(obj_idx));
    }

    sort(obj_idxs.begin(), obj_idxs.end());
    obj_idxs.erase(unique(obj_idxs.begin(), obj_idxs.end()), obj_idxs.end());

    // update printable state on canvas
    wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

    // update scene
    wxGetApp().plater()->update();
}

void ObjectList::assembly_plate_object_name()
{
    m_objects_model->assembly_name();
}

void ObjectList::selected_object(ObjectDataViewModelNode* item)
{
    if (!item) {
        return;
    }
    this->SetFocus();
    select_item(wxDataViewItem(item));
    ensure_current_item_visible();
    selection_changed();
}

void ObjectList::update_objects_list_filament_column(size_t filaments_count)
{
    assert(filaments_count >= 1);

    if (printer_technology() == ptSLA)
        filaments_count = 1;

    m_prevent_update_filament_in_config = true;

    // BBS: update extruder values even when filaments_count is 1, because it may be reduced from value greater than 1
    if (m_objects)
        update_filament_values_for_items(filaments_count);

    update_filament_colors();

    // set show/hide for this column
    set_filament_column_hidden(filaments_count == 1);
    //a workaround for a wrong last column width updating under OSX
    auto em = em_unit(this);
    GetColumn(colEditing)->SetWidth(m_columns_width[colEditing]*em);

    m_prevent_update_filament_in_config = false;
}

void ObjectList::update_objects_list_filament_column_when_delete_filament(size_t filament_id, size_t filaments_count, int replace_filament_id)
{
    m_prevent_update_filament_in_config = true;

    // BBS: update extruder values even when filaments_count is 1, because it may be reduced from value greater than 1
    if (m_objects)
        update_filament_values_for_items_when_delete_filament(filament_id, replace_filament_id);

    update_filament_colors();

    // set show/hide for this column
    set_filament_column_hidden(filaments_count == 1);
    // a workaround for a wrong last column width updating under OSX
    GetColumn(colEditing)->SetWidth(25);

    m_prevent_update_filament_in_config = false;
}

void ObjectList::update_filament_colors()
{
    m_objects_model->UpdateColumValues(colFilament);
    // BBS: fix color not refresh
    Refresh();
}

void ObjectList::update_name_column_width() const
{
    wxSize client_size = this->GetClientSize();
    bool p_vbar = this->GetParent()->HasScrollbar(wxVERTICAL);
    bool p_hbar = this->GetParent()->HasScrollbar(wxHORIZONTAL);

    auto em = em_unit(const_cast<ObjectList*>(this));
    // BBS: walkaround for wxDataViewCtrl::HasScrollbar() does not return correct status
    int others_width = 0;
    for (int cn = colName; cn < colCount; cn++) {
        if (cn != colName) {
            if (!GetColumn(cn)->IsHidden())
                others_width += m_columns_width[cn];
        }
    }

    GetColumn(colName)->SetWidth(max(0, client_size.x - (others_width)*em));
}

void ObjectList::set_filament_column_hidden(const bool hide) const
{
    GetColumn(colFilament)->SetHidden(hide);
    update_name_column_width();
}

void ObjectList::set_variable_height_column_hidden(const bool hide) const
{
    GetColumn(colHeight)->SetHidden(hide);
    update_name_column_width();
}

// BBS
void ObjectList::set_color_paint_hidden(const bool hide) const
{
    GetColumn(colColorPaint)->SetHidden(hide);
    update_name_column_width();
}

void ObjectList::set_support_paint_hidden(const bool hide) const
{
    GetColumn(colSupportPaint)->SetHidden(hide);
    update_name_column_width();
}

void GUI::ObjectList::set_sinking_hidden(const bool hide) const
{
    GetColumn(colSinking)->SetHidden(hide);
    update_name_column_width();
}

void ObjectList::update_filament_in_config(const wxDataViewItem& item)
{
    if (m_prevent_update_filament_in_config)
        return;

    const ItemType item_type = m_objects_model->GetItemType(item);
    if (item_type & itObject) {
        const int obj_idx = m_objects_model->GetIdByItem(item);
        m_config = &(*m_objects)[obj_idx]->config;
    }
    else {
        const int obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetObject(item));
        if (item_type & itVolume){
             const int ui_volume_idx = m_objects_model->GetVolumeIdByItem(item);
             if (obj_idx < 0 || ui_volume_idx < 0)
                return;
             int volume_in3d_idx = m_objects_model->get_real_volume_index_in_3d(obj_idx,ui_volume_idx);
             m_config            = &(*m_objects)[obj_idx]->volumes[volume_in3d_idx]->config;
        }
        else if (item_type & itLayer)
            m_config = &get_item_config(item);
    }

    if (!m_config)
        return;

    take_snapshot("Change Filament");

    const int extruder = m_objects_model->GetExtruderNumber(item);
    m_config->set_key_value("extruder", new ConfigOptionInt(extruder));

    // BBS
    if (item_type & itObject) {
        const int obj_idx = m_objects_model->GetIdByItem(item);
        for (ModelVolume* mv : (*m_objects)[obj_idx]->volumes) {
            if (mv->config.has("extruder"))
                mv->config.erase("extruder");
        }
    }

    // update scene
    wxGetApp().plater()->update();
}

void ObjectList::update_name_in_model(const wxDataViewItem& item) const
{
    if (m_objects_model->GetItemType(item) & itPlate) {
        std::string name = m_objects_model->GetName(item).ToUTF8().data();
        int plate_idx = -1;
        const ItemType type0 = m_objects_model->GetItemType(item, plate_idx);
        if (plate_idx >= 0) {
            auto plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
            if (plate->get_plate_name() != name) {
                plate->set_plate_name(name);
            }
            m_objects_model->SetCurSelectedPlateFullName(plate_idx, name);
        }
        return;
    }

    const int obj_idx = m_objects_model->GetObjectIdByItem(item);
    if (obj_idx < 0) return;
    const int volume_id = m_objects_model->GetVolumeIdByItem(item);

    take_snapshot(volume_id < 0 ? "Rename Object" : "Rename Part");

    ModelObject* obj = object(obj_idx);
    if (m_objects_model->GetItemType(item) & itObject) {
        std::string name = m_objects_model->GetName(item).ToUTF8().data();
        if (obj->name != name) {
            obj->name = name;
            // if object has just one volume, rename this volume too
            if (obj->volumes.size() == 1)
                obj->volumes[0]->name = obj->name;
            Slic3r::save_object_mesh(*obj);
        }
        return;
    }

    if (volume_id < 0) return;
    obj->volumes[volume_id]->name = m_objects_model->GetName(item).ToUTF8().data();
}

void ObjectList::update_name_in_list(int obj_idx, int vol_idx) const
{
    if (obj_idx < 0) return;
    wxDataViewItem item = GetSelection();
    if (!item || !(m_objects_model->GetItemType(item) & (itVolume | itObject)))
        return;

    wxString new_name = from_u8(object(obj_idx)->volumes[vol_idx]->name);
    if (new_name.IsEmpty() || m_objects_model->GetName(item) == new_name)
        return;

    m_objects_model->SetName(new_name, item);
}

void ObjectList::selection_changed()
{
    if (m_prevent_list_events) return;

    fix_multiselection_conflicts();

    fix_cut_selection();

    // update object selection on Plater
    if (!m_prevent_canvas_selection_update)
        update_selections_on_canvas();

    // to update the toolbar and info sizer
    if (!GetSelection() || m_objects_model->GetItemType(GetSelection()) == itObject) {
        auto event = SimpleEvent(EVT_OBJ_LIST_OBJECT_SELECT);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }

    if (const wxDataViewItem item = GetSelection())
    {
        const ItemType type = m_objects_model->GetItemType(item);
        // to correct visual hints for layers editing on the Scene
        if (type & (itLayer|itLayerRoot)) {
            wxGetApp().obj_layers()->reset_selection();

            if (type & itLayerRoot)
                wxGetApp().plater()->canvas3D()->handle_sidebar_focus_event("", false);
            else {
                wxGetApp().obj_layers()->set_selectable_range(m_objects_model->GetLayerRangeByItem(item));
                wxGetApp().obj_layers()->update_scene_from_editor_selection();
            }
        }
    }

    part_selection_changed();
}

void ObjectList::copy_layers_to_clipboard()
{
    wxDataViewItemArray sel_layers;
    GetSelections(sel_layers);

    const int obj_idx = m_objects_model->GetObjectIdByItem(sel_layers.front());
    if (obj_idx < 0 || (int)m_objects->size() <= obj_idx)
        return;

    const t_layer_config_ranges& ranges = object(obj_idx)->layer_config_ranges;
    t_layer_config_ranges& cache_ranges = m_clipboard.get_ranges_cache();

    if (sel_layers.Count() == 1 && m_objects_model->GetItemType(sel_layers.front()) & itLayerRoot)
    {
        cache_ranges.clear();
        cache_ranges = ranges;
        return;
    }

    for (const auto& layer_item : sel_layers)
        if (m_objects_model->GetItemType(layer_item) & itLayer) {
            auto range = m_objects_model->GetLayerRangeByItem(layer_item);
            auto it = ranges.find(range);
            if (it != ranges.end())
                cache_ranges[it->first] = it->second;
        }
}

void ObjectList::paste_layers_into_list()
{
    const int obj_idx = m_objects_model->GetObjectIdByItem(GetSelection());
    t_layer_config_ranges& cache_ranges = m_clipboard.get_ranges_cache();

    if (obj_idx < 0 || (int)m_objects->size() <= obj_idx ||
        cache_ranges.empty() || printer_technology() == ptSLA)
        return;

    const wxDataViewItem object_item = m_objects_model->GetItemById(obj_idx);
    wxDataViewItem layers_item = m_objects_model->GetLayerRootItem(object_item);
    if (layers_item)
        m_objects_model->Delete(layers_item);

    t_layer_config_ranges& ranges = object(obj_idx)->layer_config_ranges;

    // and create Layer item(s) according to the layer_config_ranges
    for (const auto& range : cache_ranges)
        ranges.emplace(range);

    layers_item = add_layer_root_item(object_item);

    changed_object(obj_idx);

    select_item(layers_item);
#ifndef __WXOSX__
    selection_changed();
#endif //no __WXOSX__
}

void ObjectList::copy_settings_to_clipboard()
{
    wxDataViewItem item = GetSelection();
    assert(item.IsOk());
    if (m_objects_model->GetItemType(item) & itSettings)
        item = m_objects_model->GetParent(item);

    wxDataViewItemArray items;
    items.push_back(item);
    while ((m_objects_model->GetItemType(items.back()) & itObject) == 0) {
        if (m_objects_model->GetItemType(items.back()) & itLayerRoot)
            items.back() = m_objects_model->GetParent(items.back());
        else
            items.push_back(m_objects_model->GetParent(items.back()));
    }
    auto &config = m_clipboard.get_config_cache();
    config.clear();
    while (!items.empty()) {
        config.apply(get_item_config(items.back()).get());
        items.pop_back();
    }

    m_clipboard.set_type(ItemType(m_objects_model->GetItemType(item) | itSettings));
}

bool GUI::ObjectList::can_paste_settings_into_list()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.empty())
        return false;
    return m_clipboard.get_type() == (m_objects_model->GetItemType(sels.front()) | itSettings);
}

void ObjectList::paste_settings_into_list()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    take_snapshot("Paste settings");

    std::unique_ptr<t_config_option_keys> global_keys; // need copy from global

    for (auto item : sels) {

        if (m_objects_model->GetItemType(item) & itSettings)
            item = m_objects_model->GetParent(item);

        ItemType item_type = m_objects_model->GetItemType(item);
        if(!(item_type & (itObject | itVolume |itLayer)))
            return;

        DynamicPrintConfig& config_cache = m_clipboard.get_config_cache();
        assert(!config_cache.empty());

        auto keys = config_cache.keys();
        auto part_options = SettingsFactory::get_options(true);
        auto config       = &get_item_config(item);
        auto extruder     = config->option("extruder") ? config->option("extruder")->clone() : nullptr;
        config->reset();

        if (item_type & (itVolume | itLayer)) {
            if (global_keys == nullptr) {
                auto object = m_objects_model->GetParent(item);
                DynamicPrintConfig config;
                config.apply_only(*wxGetApp().get_tab(Preset::TYPE_PRINT)->get_config(), keys);
                config.apply_only(get_item_config(object).get(), keys);
                auto equals = config.equal(config_cache);
                global_keys.reset(new t_config_option_keys);
                auto keys2 = get_item_config(object).keys();
                std::copy_if(keys2.begin(), keys2.end(), std::back_inserter(*global_keys),
                             [&equals](auto &e) { return std::find(equals.begin(), equals.end(), e) == equals.end(); });
                keys.erase(std::remove_if(keys.begin(), keys.end(),
                             [&equals](auto &e) { return std::find(equals.begin(), equals.end(), e) != equals.end(); }), keys.end());
            }
            config->apply_only(*wxGetApp().get_tab(Preset::TYPE_PRINT)->get_config(), *global_keys);
        }

        for (const std::string& opt_key: keys) {
            if (item_type & (itVolume | itLayer) &&
                std::find(part_options.begin(), part_options.end(), opt_key) == part_options.end())
                continue; // we can't to add object specific options for the part's(itVolume | itLayer) config

            const ConfigOption* option = config_cache.option(opt_key);
            if (option)
                config->set_key_value(opt_key, option->clone());
        }
        if (extruder)
            config->set_key_value("extruder", extruder);
        else
            config->erase("extruder");

        // Add settings item for object/sub-object and show them
        add_settings_item(item, &config->get());
    }

    part_selection_changed();
}

void ObjectList::paste_volumes_into_list(int obj_idx, const ModelVolumePtrs& volumes)
{
    if ((obj_idx < 0) || ((int)m_objects->size() <= obj_idx))
        return;

    if (volumes.empty())
        return;

    wxDataViewItemArray items = reorder_volumes_and_get_selection(obj_idx, [volumes](const ModelVolume* volume) {
        return std::find(volumes.begin(), volumes.end(), volume) != volumes.end(); });
    if (items.size() > 1) {
        m_selection_mode = smVolume;
        m_last_selected_item = wxDataViewItem(nullptr);
    }

    select_items(items);
    selection_changed();

    //BBS: notify partplate the modify
    notify_instance_updated(obj_idx);
}

void ObjectList::paste_objects_into_list(const std::vector<size_t>& object_idxs)
{
    if (object_idxs.empty())
        return;

    wxDataViewItemArray items;
    for (const size_t object : object_idxs)
    {
        add_object_to_list(object);
        items.Add(m_objects_model->GetItemById(object));
    }

    wxGetApp().plater()->changed_objects(object_idxs);

    select_items(items);
    selection_changed();
}

#ifdef __WXOSX__
/*
void ObjectList::OnChar(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_BACK){
        remove();
    }
    else if (wxGetKeyState(wxKeyCode('A')) && wxGetKeyState(WXK_SHIFT))
        select_item_all_children();

    event.Skip();
}
*/
#endif /* __WXOSX__ */

void ObjectList::OnContextMenu(wxDataViewEvent& evt)
{
    // The mouse position returned by get_mouse_position_in_control() here is the one at the time the mouse button is released (mouse up event)
    wxPoint mouse_pos = this->get_mouse_position_in_control();

    // Do not show the context menu if the user pressed the right mouse button on the 3D scene and released it on the objects list
    GLCanvas3D* canvas = wxGetApp().plater()->canvas3D();
    bool evt_context_menu = (canvas != nullptr) ? !canvas->is_mouse_dragging() : true;
    if (!evt_context_menu)
        canvas->mouse_up_cleanup();

    list_manipulation(mouse_pos, evt_context_menu);
}

void ObjectList::list_manipulation(const wxPoint& mouse_pos, bool evt_context_menu/* = false*/)
{
    if (m_prevent_list_manipulation) return;

    // Interesting fact: when mouse_pos.x < 0, HitTest(mouse_pos, item, col) returns item = null, but column = last column.
    // So, when mouse was moved to scene immediately after clicking in ObjectList, in the scene will be shown context menu for the Editing column.
    if (mouse_pos.x < 0)
        return;

    wxDataViewItem item;
    wxDataViewColumn* col = nullptr;
    HitTest(mouse_pos, item, col);

    if (m_extruder_editor)
        m_extruder_editor->Hide();

    /* Note: Under OSX right click doesn't send "selection changed" event.
     * It means that Selection() will be return still previously selected item.
     * Thus under OSX we should force UnselectAll(), when item and col are nullptr,
     * and select new item otherwise.
     */

    // BBS
    //if (!item)
    {
        if (col == nullptr) {
            if (wxOSX && !multiple_selection())
                UnselectAll();
            else if (!evt_context_menu)
                // Case, when last item was deleted and under GTK was called wxEVT_DATAVIEW_SELECTION_CHANGED,
                // which invoked next list_manipulation(false)
                return;
        }

        if (evt_context_menu) {
            show_context_menu(evt_context_menu);
            return;
        }
    }

    if (wxOSX && item && col) {
        wxDataViewItemArray sels;
        GetSelections(sels);
        UnselectAll();
        if (sels.Count() > 1)
            SetSelections(sels);
        else
            Select(item);
    }

    if (col != nullptr)
    {
	    const wxString title = col->GetTitle();
        ColumnNumber col_num = (ColumnNumber)col->GetModelColumn();
        if (col_num == colHeight)
            enable_layers_editing();
        else if (col_num == colPrint)
	        toggle_printable_state();
        else if (col_num == colSupportPaint) {
            ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
            if (node && node->HasSupportPainting()) {
                GLGizmosManager& gizmos_mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
                if (gizmos_mgr.get_current_type() != GLGizmosManager::EType::FdmSupports)
                    gizmos_mgr.open_gizmo(GLGizmosManager::EType::FdmSupports);
                else
                    gizmos_mgr.reset_all_states();
            }
        }
        else if (col_num == colColorPaint) {
            if (wxGetApp().plater()->get_current_canvas3D()->get_canvas_type() != GLCanvas3D::CanvasAssembleView) {
                ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
                if (node && node->HasColorPainting()) {
                    GLGizmosManager& gizmos_mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
                    if (gizmos_mgr.get_current_type() != GLGizmosManager::EType::MmSegmentation)
                        gizmos_mgr.open_gizmo(GLGizmosManager::EType::MmSegmentation);
                    else
                        gizmos_mgr.reset_all_states();
                }
            }
        }
        else if (col_num == colSinking) {
            Plater *    plater = wxGetApp().plater();
            GLCanvas3D *cnv    = plater->canvas3D();
            if (cnv->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasPreview) {//ban reload_scene in Preview scene
                return;
            }
            int obj_idx, vol_idx;
            get_selected_item_indexes(obj_idx, vol_idx, item);
            if (obj_idx != -1) {
                Plater::TakeSnapshot(plater, "Shift objects to bed");
                (*m_objects)[obj_idx]->ensure_on_bed();
                cnv->reload_scene(true, true);
                update_info_items(obj_idx);
                notify_instance_updated(obj_idx);
            }
        }
        else if (col_num == colEditing) {
            //show_context_menu(evt_context_menu);
            int obj_idx, vol_idx;

            get_selected_item_indexes(obj_idx, vol_idx, item);
            //wxGetApp().plater()->PopupObjectTable(obj_idx, vol_idx, mouse_pos);
            if (m_objects_model->GetItemType(item) & itPlate)
                dynamic_cast<TabPrintPlate*>(wxGetApp().get_plate_tab())->reset_model_config();
            else if (m_objects_model->GetItemType(item) & itLayer)
                dynamic_cast<TabPrintLayer*>(wxGetApp().get_layer_tab())->reset_model_config();
            else if (item.IsOk())
                dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab(vol_idx >= 0))->reset_model_config();
            wxGetApp().params_panel()->notify_object_config_changed();
        }
        else if (col_num == colName)
        {
            if (is_windows10() && m_objects_model->HasWarningIcon(item) &&
                mouse_pos.x > 2 * wxGetApp().em_unit() && mouse_pos.x < 4 * wxGetApp().em_unit())
                fix_through_netfabb();
            else if (evt_context_menu)
                show_context_menu(evt_context_menu); // show context menu for "Name" column too
        }
	    // workaround for extruder editing under OSX
	    else if (wxOSX && evt_context_menu && col_num == colFilament)
	        extruder_editing();
	}

#ifndef __WXMSW__
    GetMainWindow()->SetToolTip(""); // hide tooltip
#endif //__WXMSW__
}

void ObjectList::show_context_menu(const bool evt_context_menu)
{
    // BBS Disable menu popup if current canvas is Preview
    if (wxGetApp().plater()->get_current_canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasPreview)
        return;

    wxMenu* menu {nullptr};
    Plater* plater = wxGetApp().plater();

    if (multiple_selection())
    {
        if (selected_instances_of_same_object())
            menu = plater->instance_menu();
        else
            menu = plater->multi_selection_menu();
    }
    else {
        const auto item = GetSelection();
        if (item)
        {
            const ItemType type = m_objects_model->GetItemType(item);
            if (!(type & (itPlate | itObject | itVolume | itInstance)))
                return;
            if (type & itVolume) {
                int obj_idx, vol_idx;
                get_selected_item_indexes(obj_idx, vol_idx, item);
                if (obj_idx < 0 || vol_idx < 0)
                    return;
                const ModelVolume *volume = object(obj_idx)->volumes[vol_idx];

                menu = volume->is_text() ? plater->text_part_menu() :
			volume->is_svg() ? plater->svg_part_menu() : // ORCA fixes missing "Edit SVG" item for Add/Negative/Modifier SVG objects in object list
                    plater->part_menu();
            }
            else
                menu =  type & itPlate                                              ? plater->plate_menu() :
                        type & itInstance                                           ? plater->instance_menu() :
                        printer_technology() == ptFFF                               ? plater->object_menu() : plater->sla_object_menu();
            plater->SetPlateIndexByRightMenuInLeftUI(-1);
            if (type & itPlate) {
                int            plate_idx = -1;
                const ItemType type0      = m_objects_model->GetItemType(item, plate_idx);
                if (plate_idx >= 0) { 
                    plater->SetPlateIndexByRightMenuInLeftUI(plate_idx);
                }
            }
        }
        else if (evt_context_menu)
            menu = plater->default_menu();
    }

    if (menu)
        plater->PopupMenu(menu);
}

void ObjectList::extruder_editing()
{
    wxDataViewItem item = GetSelection();
    if (!item || !(m_objects_model->GetItemType(item) & (itVolume | itObject)))
        return;

    const int column_width = GetColumn(colFilament)->GetWidth() + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X) + 5;

    wxPoint pos = this->get_mouse_position_in_control();
    wxSize size = wxSize(column_width, -1);
    pos.x = GetColumn(colName)->GetWidth() + GetColumn(colPrint)->GetWidth() + GetColumn(colHeight)->GetWidth() + 5;
    pos.y -= GetTextExtent("m").y;

    apply_extruder_selector(&m_extruder_editor, this, "1", pos, size);

    m_extruder_editor->SetSelection(m_objects_model->GetExtruderNumber(item));
    m_extruder_editor->Show();

    auto set_extruder = [this]()
    {
        wxDataViewItem item = GetSelection();
        if (!item) return;

        const int selection = m_extruder_editor->GetSelection();
        if (selection >= 0)
            m_objects_model->SetExtruder(m_extruder_editor->GetString(selection), item);

        m_extruder_editor->Hide();
        update_filament_in_config(item);
    };

    // to avoid event propagation to other sidebar items
    m_extruder_editor->Bind(wxEVT_COMBOBOX, [set_extruder](wxCommandEvent& evt)
    {
        set_extruder();
        evt.StopPropagation();
    });
}

void ObjectList::copy()
{
    wxPostEvent((wxEvtHandler*)wxGetApp().plater()->canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_COPY));
}

void ObjectList::paste()
{
    wxPostEvent((wxEvtHandler*)wxGetApp().plater()->canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_PASTE));
}

void ObjectList::cut()
{
    wxPostEvent((wxEvtHandler*)wxGetApp().plater()->canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_CUT));
}

void ObjectList::clone()
{
    wxPostEvent((wxEvtHandler*)wxGetApp().plater()->canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_CLONE));
}

bool ObjectList::cut_to_clipboard()
{
    return copy_to_clipboard();
}

bool ObjectList::copy_to_clipboard()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return false;
    ItemType type = m_objects_model->GetItemType(sels.front());
    if (!(type & (itSettings | itLayer | itLayerRoot))) {
        m_clipboard.reset();
        return false;
    }

    if (type & itSettings)
        copy_settings_to_clipboard();
    if (type & (itLayer | itLayerRoot))
        copy_layers_to_clipboard();

    m_clipboard.set_type(type);
    return true;
}

bool ObjectList::paste_from_clipboard()
{
    if (!(m_clipboard.get_type() & (itSettings | itLayer | itLayerRoot))) {
        m_clipboard.reset();
        return false;
    }

    if (m_clipboard.get_type() & itSettings)
        paste_settings_into_list();
    else if (m_clipboard.get_type() & (itLayer | itLayerRoot))
        paste_layers_into_list();

    return true;
}

void ObjectList::undo()
{
	wxGetApp().plater()->undo();
}

void ObjectList::redo()
{
	wxGetApp().plater()->redo();
}

void ObjectList::increase_instances()
{
    wxGetApp().plater()->increase_instances(1);
}

void ObjectList::decrease_instances()
{
    wxGetApp().plater()->decrease_instances(1);
}

#ifndef __WXOSX__
void ObjectList::key_event(wxKeyEvent& event)
{
    //if (event.GetKeyCode() == WXK_TAB)
    //    Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    //else
    if (event.GetKeyCode() == WXK_DELETE /*|| event.GetKeyCode() == WXK_BACK*/ )
        remove();
    //else if (event.GetKeyCode() == WXK_F5)
    //    wxGetApp().plater()->reload_all_from_disk();
    else if (wxGetKeyState(wxKeyCode('A')) && wxGetKeyState(WXK_CONTROL/*WXK_SHIFT*/))
        select_item_all_children();
    else if (wxGetKeyState(wxKeyCode('C')) && wxGetKeyState(WXK_CONTROL))
        copy();
    else if (wxGetKeyState(wxKeyCode('V')) && wxGetKeyState(WXK_CONTROL))
        paste();
    else if (wxGetKeyState(wxKeyCode('Y')) && wxGetKeyState(WXK_CONTROL))
        redo();
    else if (wxGetKeyState(wxKeyCode('Z')) && wxGetKeyState(WXK_CONTROL))
        undo();
    else if (wxGetKeyState(wxKeyCode('X')) && wxGetKeyState(WXK_CONTROL))
        cut();
    else if (wxGetKeyState(wxKeyCode('K')) && wxGetKeyState(WXK_CONTROL))
        clone();
    else if (event.GetUnicodeKey() == '+')
        increase_instances();
    else if (event.GetUnicodeKey() == '-')
        decrease_instances();
    else if (event.GetUnicodeKey() == 'p')
        toggle_printable_state();
    else if (filaments_count() > 1) {
        std::vector<wxChar> numbers = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
        wxChar key_char = event.GetUnicodeKey();
        if (std::find(numbers.begin(), numbers.end(), key_char) != numbers.end()) {
            long extruder_number;
            if (wxNumberFormatter::FromString(wxString(key_char), &extruder_number) &&
                filaments_count() >= extruder_number)
                set_extruder_for_selected_items(int(extruder_number));
        }
        else
            event.Skip();
    }
    else
        event.Skip();
}
#endif /* __WXOSX__ */

void ObjectList::OnBeginDrag(wxDataViewEvent &event)
{
    const bool mult_sel = multiple_selection();
    if (mult_sel) {
        event.Veto();
        return;
    }

    const wxDataViewItem item(event.GetItem());
    const ItemType& type = m_objects_model->GetItemType(item);
    if (!(type & (itVolume | itObject))) {
        event.Veto();
        return;
    }

    if (type & itObject) {
        m_dragged_data.init(m_objects_model->GetIdByItem(item), type);
    }
    else if (type & itVolume){
        m_dragged_data.init(m_objects_model->GetObjectIdByItem(item), m_objects_model->GetVolumeIdByItem(item), type);
    }
#if 0
    if ((mult_sel && !selected_instances_of_same_object()) ||
        (!mult_sel && (GetSelection() != item)) ) {
        event.Veto();
        return;
    }

    if (!(type & (itVolume | itObject | itInstance))) {
        event.Veto();
        return;
    }

    if (mult_sel)
    {
        m_dragged_data.init(m_objects_model->GetObjectIdByItem(item),type);
        std::set<int>& sub_obj_idxs = m_dragged_data.inst_idxs();
        wxDataViewItemArray sels;
        GetSelections(sels);
        for (auto sel : sels )
            sub_obj_idxs.insert(m_objects_model->GetInstanceIdByItem(sel));
    }
    else if (type & itObject)
        m_dragged_data.init(m_objects_model->GetIdByItem(item), type);
    else
        m_dragged_data.init(m_objects_model->GetObjectIdByItem(item),
                            type&itVolume ? m_objects_model->GetVolumeIdByItem(item) :
                                        m_objects_model->GetInstanceIdByItem(item),
                            type);
#endif

    /* Under MSW or OSX, DnD moves an item to the place of another selected item
    * But under GTK, DnD moves an item between another two items.
    * And as a result - call EVT_CHANGE_SELECTION to unselect all items.
    * To prevent such behavior use m_prevent_list_events
    **/
    m_prevent_list_events = true;//it's needed for GTK

    /* Under GTK, DnD requires to the wxTextDataObject been initialized with some valid value,
     * so set some nonempty string
     */
    wxTextDataObject* obj = new wxTextDataObject;
    obj->SetText("Some text");//it's needed for GTK

    event.SetDataObject(obj);
    event.SetDragFlags(wxDrag_DefaultMove); // allows both copy and move;
}

bool ObjectList::can_drop(const wxDataViewItem& item, int& src_obj_id, int& src_plate, int& dest_obj_id, int& dest_plate) const
{
    if (!item.IsOk()
        || (m_objects_model->GetItemType(item) != m_dragged_data.type())
        || !(m_dragged_data.type() & (itVolume|itObject)))
        return false;

    if (m_dragged_data.type() & itObject){
        int from_obj_id = m_dragged_data.obj_idx();
        int to_obj_id   = m_objects_model->GetIdByItem(item);
        PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();

        int from_plate = partplate_list.find_instance(from_obj_id, 0);
        if (from_plate == -1)
            return false;
        int to_plate = partplate_list.find_instance(to_obj_id, 0);
        if ((to_plate == -1) || (from_plate != to_plate))
            return false;

        src_obj_id = from_obj_id;
        dest_obj_id = to_obj_id;
        src_plate = from_plate;
        dest_plate = to_plate;

        // move instance(s) or object on "empty place" of ObjectList
        // if ( (m_dragged_data.type() & (itInstance | itObject)) && !item.IsOk() )
        //    return true;
    }
    else if (m_dragged_data.type() & itVolume) {  // move volumes inside one object only
        if (m_dragged_data.obj_idx() != m_objects_model->GetObjectIdByItem(item))
            return false;
        wxDataViewItem dragged_item = m_objects_model->GetItemByVolumeId(m_dragged_data.obj_idx(), m_dragged_data.sub_obj_idx());
        if (!dragged_item)
            return false;
        ModelVolumeType item_v_type = m_objects_model->GetVolumeType(item);
        ModelVolumeType dragged_item_v_type = m_objects_model->GetVolumeType(dragged_item);

        if (dragged_item_v_type == item_v_type && dragged_item_v_type != ModelVolumeType::MODEL_PART)
            return true;
        if ((dragged_item_v_type != item_v_type) ||   // we can't reorder volumes outside of types
            item_v_type >= ModelVolumeType::SUPPORT_BLOCKER)        // support blockers/enforcers can't change its place
            return false;

        bool only_one_solid_part = true;
        auto& volumes = (*m_objects)[m_dragged_data.obj_idx()]->volumes;
        for (size_t cnt, id = cnt = 0; id < volumes.size() && cnt < 2; id ++)
            if (volumes[id]->type() == ModelVolumeType::MODEL_PART) {
                if (++cnt > 1)
                    only_one_solid_part = false;
            }

        if (dragged_item_v_type == ModelVolumeType::MODEL_PART) {
            if (only_one_solid_part)
                return false;
            return (m_objects_model->GetVolumeIdByItem(item) == 0 ||
                    (m_dragged_data.sub_obj_idx()==0 && volumes[1]->type() == ModelVolumeType::MODEL_PART) ||
                    (m_dragged_data.sub_obj_idx()!=0 && volumes[0]->type() == ModelVolumeType::MODEL_PART));
        }
        if ((dragged_item_v_type == ModelVolumeType::NEGATIVE_VOLUME || dragged_item_v_type == ModelVolumeType::PARAMETER_MODIFIER)) {
            if (only_one_solid_part)
                return false;
            return m_objects_model->GetVolumeIdByItem(item) != 0;
        }
        return false;
    }
    return true;
}

void ObjectList::OnDropPossible(wxDataViewEvent &event)
{
    const wxDataViewItem& item = event.GetItem();

    int src_obj_id, src_plate, dest_obj_id, dest_plate;
    if (!can_drop(item, src_obj_id, src_plate, dest_obj_id, dest_plate)) {
        event.Veto();
        m_prevent_list_events = false;
    }
}

void ObjectList::OnDrop(wxDataViewEvent &event)
{
    const wxDataViewItem& item = event.GetItem();

    int src_obj_id, src_plate, dest_obj_id, dest_plate;
    if (!can_drop(item, src_obj_id, src_plate, dest_obj_id, dest_plate))
    {
        event.Veto();
        m_dragged_data.clear();
        return;
    }

//#if 1
    take_snapshot("Object order changed");

    if(m_dragged_data.type() & itObject){
        int delta = dest_obj_id < src_obj_id ? -1 : 1;
        PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
        /*int cnt = 0, cur_id = src_obj_id, next_id, total = abs(src_obj_id - dest_obj_id);
        //for (cur_id = src_obj_id; cnt < total; id += delta, cnt++)
        next_id = src_obj_id + delta;
        while (cnt < total)
        {
            int cur_plate = partplate_list.find_instance(next_id, 0);
            if (cur_plate != src_plate) {
                cnt ++;
                next_id += delta;
                continue;
            }
            std::swap((*m_objects)[cur_id], (*m_objects)[next_id]);
            cur_id = next_id;
            cnt ++;
            next_id += delta;
        }*/

        int cnt = 0;
        for (int id = src_obj_id; cnt < abs(src_obj_id - dest_obj_id); id += delta, cnt++)
            std::swap((*m_objects)[id], (*m_objects)[id + delta]);

        select_item(m_objects_model->ReorganizeObjects(src_obj_id, dest_obj_id));

        partplate_list.reload_all_objects(false, src_plate);
        changed_object(src_obj_id);
    }
    else if (m_dragged_data.type() & itVolume) {
        int from_volume_id = m_dragged_data.sub_obj_idx();
        int to_volume_id   = m_objects_model->GetVolumeIdByItem(item);
        int delta          = to_volume_id < from_volume_id ? -1 : 1;

        auto &volumes = (*m_objects)[m_dragged_data.obj_idx()]->volumes;

        int cnt = 0;
        for (int id = from_volume_id; cnt < abs(from_volume_id - to_volume_id); id += delta, cnt++) std::swap(volumes[id], volumes[id + delta]);

        select_item(m_objects_model->ReorganizeChildren(from_volume_id, to_volume_id, m_objects_model->GetParent(item)));

        changed_object(m_dragged_data.obj_idx());
    }

    m_dragged_data.clear();

    wxGetApp().plater()->set_current_canvas_as_dirty();
}

void ObjectList::add_category_to_settings_from_selection(const std::vector< std::pair<std::string, bool> >& category_options, wxDataViewItem item)
{
    if (category_options.empty())
        return;

    const ItemType item_type = m_objects_model->GetItemType(item);

    if (!m_config)
        m_config = &get_item_config(item);

    assert(m_config);
    auto opt_keys = m_config->keys();

    const std::string snapshot_text =  item_type & itLayer   ? "Layer setting added" :
                                    item_type & itVolume  ? "Part setting added" :
                                                            "Object setting added";
    take_snapshot(snapshot_text);

    const DynamicPrintConfig& from_config = printer_technology() == ptFFF ?
                                            wxGetApp().preset_bundle->prints.get_edited_preset().config :
                                            wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;

    for (auto& opt : category_options) {
        auto& opt_key = opt.first;
        if (find(opt_keys.begin(), opt_keys.end(), opt_key) != opt_keys.end() && !opt.second)
            m_config->erase(opt_key);

        if (find(opt_keys.begin(), opt_keys.end(), opt_key) == opt_keys.end() && opt.second) {
            const ConfigOption* option = from_config.option(opt_key);
            if (!option) {
                // if current option doesn't exist in prints.get_edited_preset(),
                // get it from default config values
                option = DynamicPrintConfig::new_from_defaults_keys({ opt_key })->option(opt_key);
            }
            m_config->set_key_value(opt_key, option->clone());
        }
    }

    // Add settings item for object/sub-object and show them
    if (!(item_type & (itPlate | itObject | itVolume | itLayer)))
        item = m_objects_model->GetObject(item);
    show_settings(add_settings_item(item, &m_config->get()));
}

void ObjectList::add_category_to_settings_from_frequent(const std::vector<std::string>& options, wxDataViewItem item)
{
    const ItemType item_type = m_objects_model->GetItemType(item);

    if (!m_config)
        m_config = &get_item_config(item);

    assert(m_config);
    auto opt_keys = m_config->keys();

    const std::string snapshot_text = item_type & itLayer  ? "Height range settings added" :
                                   item_type & itVolume ? "Part settings added" :
                                                          "Object settings added";
    take_snapshot(snapshot_text);

    const DynamicPrintConfig& from_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    for (auto& opt_key : options)
    {
        if (find(opt_keys.begin(), opt_keys.end(), opt_key) == opt_keys.end()) {
            const ConfigOption* option = from_config.option(opt_key);
            if (!option) {
                // if current option doesn't exist in prints.get_edited_preset(),
                // get it from default config values
                option = DynamicPrintConfig::new_from_defaults_keys({ opt_key })->option(opt_key);
            }
            m_config->set_key_value(opt_key, option->clone());
        }
    }

    // Add settings item for object/sub-object and show them
    if (!(item_type & (itPlate | itObject | itVolume | itLayer)))
        item = m_objects_model->GetObject(item);

    show_settings(add_settings_item(item, &m_config->get()));
}

void ObjectList::show_settings(const wxDataViewItem settings_item)
{
    if (!settings_item)
        return;

    select_item(settings_item);

    // update object selection on Plater
    if (!m_prevent_canvas_selection_update)
        update_selections_on_canvas();
}

bool ObjectList::is_instance_or_object_selected()
{
    const Selection& selection = scene_selection();
    return selection.is_single_full_instance() || selection.is_single_full_object();
}

void ObjectList::load_subobject(ModelVolumeType type, bool from_galery/* = false*/)
{
    wxDataViewItem item = GetSelection();
    // we can add volumes for Object or Instance
    if (!item || !(m_objects_model->GetItemType(item)&(itObject|itInstance)))
        return;
    const int obj_idx = m_objects_model->GetObjectIdByItem(item);

    if (obj_idx < 0) return;

    // Get object item, if Instance is selected
    if (m_objects_model->GetItemType(item)&itInstance)
        item = m_objects_model->GetItemById(obj_idx);

    wxArrayString input_files;
    /*if (from_galery) {
        GalleryDialog dlg(this);
        if (dlg.ShowModal() != wxID_CLOSE)
            dlg.get_input_files(input_files);
    }
    else*/
        wxGetApp().import_model(wxGetApp().tab_panel()->GetPage(0), input_files);

    if (input_files.IsEmpty())
        return;

    take_snapshot((type == ModelVolumeType::MODEL_PART) ? "Load Part" : "Load Modifier");

    std::vector<ModelVolume*> volumes;
    // ! ysFIXME - delete commented code after testing and rename "load_modifier" to something common
    /*
    if (type == ModelVolumeType::MODEL_PART)
        load_part(*(*m_objects)[obj_idx], volumes, type, from_galery);
    else*/
        load_modifier(input_files, *(*m_objects)[obj_idx], volumes, type, from_galery);

    if (volumes.empty())
        return;

    wxDataViewItemArray items = reorder_volumes_and_get_selection(obj_idx, [volumes](const ModelVolume* volume) {
        return std::find(volumes.begin(), volumes.end(), volume) != volumes.end(); });

    if (type == ModelVolumeType::MODEL_PART)
        // update printable state on canvas
        wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_object((size_t)obj_idx);

    if (items.size() > 1) {
        m_selection_mode = smVolume;
        m_last_selected_item = wxDataViewItem(nullptr);
    }
    select_items(items);

    selection_changed();

    //BBS: notify partplate the modify
    notify_instance_updated(obj_idx);
}
/*
void ObjectList::load_part(ModelObject& model_object, std::vector<ModelVolume*>& added_volumes, ModelVolumeType type, bool from_galery = false)
{
    if (type != ModelVolumeType::MODEL_PART)
        return;

    wxWindow* parent = wxGetApp().tab_panel()->GetPage(0);

    wxArrayString input_files;

    if (from_galery) {
        GalleryDialog dlg(this);
        if (dlg.ShowModal() == wxID_CLOSE)
            return;
        dlg.get_input_files(input_files);
        if (input_files.IsEmpty())
            return;
    }
    else
        wxGetApp().import_model(parent, input_files);

    ProgressDialog dlg(_L("Loading") + dots, "", 100, wxGetApp().mainframe wxPD_AUTO_HIDE);
    wxBusyCursor busy;

    for (size_t i = 0; i < input_files.size(); ++i) {
        std::string input_file = input_files.Item(i).ToUTF8().data();

        dlg.Update(static_cast<int>(100.0f * static_cast<float>(i) / static_cast<float>(input_files.size())),
            _L("Loading file") + ": " + from_path(boost::filesystem::path(input_file).filename()));
        dlg.Fit();

        Model model;
        try {
            model = Model::read_from_file(input_file);
        }
        catch (std::exception &e) {
            auto msg = _L("Error!") + " " + input_file + " : " + e.what() + ".";
            show_error(parent, msg);
            exit(1);
        }

        for (auto object : model.objects) {
            Vec3d delta = Vec3d::Zero();
            if (model_object.origin_translation != Vec3d::Zero()) {
                object->center_around_origin();
                delta = model_object.origin_translation - object->origin_translation;
            }
            for (auto volume : object->volumes) {
                volume->translate(delta);
                auto new_volume = model_object.add_volume(*volume, type);
                new_volume->name = boost::filesystem::path(input_file).filename().string();
                // set a default extruder value, since user can't add it manually
                // BBS
                new_volume->config.set_key_value("extruder", new ConfigOptionInt(1));

                added_volumes.push_back(new_volume);
            }
        }
    }
}
*/
void ObjectList::load_modifier(const wxArrayString& input_files, ModelObject& model_object, std::vector<ModelVolume*>& added_volumes, ModelVolumeType type, bool from_galery)
{
    // ! ysFIXME - delete commented code after testing and rename "load_modifier" to something common
    //if (type == ModelVolumeType::MODEL_PART)
    //    return;

    wxWindow* parent = wxGetApp().tab_panel()->GetPage(0);

    ProgressDialog dlg(_L("Loading") + dots, "", 100, wxGetApp().mainframe, wxPD_AUTO_HIDE);

    wxBusyCursor busy;

    const int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0)
        return;

    const Selection& selection = scene_selection();
    assert(obj_idx == selection.get_object_idx());

    /** Any changes of the Object's composition is duplicated for all Object's Instances
      * So, It's enough to take a bounding box of a first selected Instance and calculate Part(generic_subobject) position
      */
    int instance_idx = *selection.get_instance_idxs().begin();
    assert(instance_idx != -1);
    if (instance_idx == -1)
        return;

    // Bounding box of the selected instance in world coordinate system including the translation, without modifiers.
    const BoundingBoxf3 instance_bb = model_object.instance_bounding_box(instance_idx);

    // First (any) GLVolume of the selected instance. They all share the same instance matrix.
    const GLVolume* v = selection.get_first_volume();
    const Geometry::Transformation inst_transform = v->get_instance_transformation();
    const Transform3d inv_inst_transform = inst_transform.get_matrix_no_offset().inverse();
    const Vec3d instance_offset = v->get_instance_offset();

    for (size_t i = 0; i < input_files.size(); ++i) {
        const std::string input_file = input_files.Item(i).ToUTF8().data();

        dlg.Update(static_cast<int>(100.0f * static_cast<float>(i) / static_cast<float>(input_files.size())),
            _L("Loading file") + ": " + from_path(boost::filesystem::path(input_file).filename()));
        dlg.Fit();
        
        bool is_user_cancel = false;
        Model model;
        try {
            if (boost::iends_with(input_file, ".stp") ||
                boost::iends_with(input_file, ".step")) {
                double linear  = string_to_double_decimal_point(wxGetApp().app_config->get("linear_defletion"));
                if (linear <= 0) linear = 0.003;
                double angle = string_to_double_decimal_point(wxGetApp().app_config->get("angle_defletion"));
                if (angle <= 0) angle = 0.5;
                bool split_compound = wxGetApp().app_config->get_bool("is_split_compound");
                model = Model::read_from_step(
                    input_file, LoadStrategy::LoadModel, nullptr, nullptr,
                    [this, &is_user_cancel, &linear, &angle, &split_compound](Slic3r::Step& file, double& linear_value,
                                                                                     double& angle_value, bool& is_split) -> int {
                        if (wxGetApp().app_config->get_bool("enable_step_mesh_setting")) {
                            StepMeshDialog mesh_dlg(nullptr, file, linear, angle);
                            if (mesh_dlg.ShowModal() == wxID_OK) {
                                linear_value = mesh_dlg.get_linear_defletion();
                                angle_value  = mesh_dlg.get_angle_defletion();
                                is_split     = mesh_dlg.get_split_compound_value();
                                return 1;
                            }
                        } else {
                            linear_value = linear;
                            angle_value  = angle;
                            is_split     = split_compound;
                            return 1;
                        }
                        is_user_cancel = true;
                        return -1;
                    },
                    linear, angle, split_compound);
            } else {
                model = Model::read_from_file(input_file, nullptr, nullptr, LoadStrategy::LoadModel);
            }
        }
        catch (std::exception&) {
            if (!is_user_cancel) {
                // auto msg = _L("Error!") + " " + input_file + " : " + e.what() + ".";
                auto msg = _L("Error!") + " " + _L("Failed to get the model data in the current file.");
                show_error(parent, msg);
            }
            return;
        }

        if (from_galery)
            model.center_instances_around_point(Vec2d::Zero());
        else {
            for (auto object : model.objects) {
                if (model_object.origin_translation != Vec3d::Zero()) {
                    object->center_around_origin();
                    const Vec3d delta = model_object.origin_translation - object->origin_translation;
                    for (auto volume : object->volumes) {
                        volume->translate(delta);
                    }
                }
            }
        }

        model.add_default_instances();
        TriangleMesh mesh = model.mesh();
        // Mesh will be centered when loading.
        ModelVolume* new_volume = model_object.add_volume(std::move(mesh), type);
        new_volume->name = boost::filesystem::path(input_file).filename().string();

        // adjust offset as prusaslicer ObjectList::load_from_files does (works) instead of BBS method
        //// BBS: object_mesh.get_init_shift() keep the relative position
        //TriangleMesh object_mesh = model_object.volumes[0]->mesh();
        //new_volume->set_offset(new_volume->mesh().get_init_shift() - object_mesh.get_init_shift());

        // set a default extruder value, since user can't add it manually
        // BBS
        int extruder_id = 0;
        if (new_volume->type() == ModelVolumeType::MODEL_PART && model_object.config.has("extruder"))
            extruder_id = model_object.config.opt_int("extruder");
        new_volume->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
        // update source data
        new_volume->source.input_file = input_file;
        new_volume->source.object_idx = obj_idx;
        new_volume->source.volume_idx = int(model_object.volumes.size()) - 1;
        if (model.objects.size() == 1 && model.objects.front()->volumes.size() == 1)
            new_volume->source.mesh_offset = model.objects.front()->volumes.front()->source.mesh_offset;

        if (from_galery) {
            // Transform the new modifier to be aligned with the print bed.
            const BoundingBoxf3 mesh_bb = new_volume->mesh().bounding_box();
            new_volume->set_transformation(Geometry::Transformation::volume_to_bed_transformation(inst_transform, mesh_bb));
            // Set the modifier position.
            // Translate the new modifier to be pickable: move to the left front corner of the instance's bounding box, lift to print bed.
            const Vec3d offset = Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()) + 0.5 * mesh_bb.size() - instance_offset;
            new_volume->set_offset(inv_inst_transform * offset);
        }
        else
            new_volume->set_offset(new_volume->source.mesh_offset - model_object.volumes.front()->source.mesh_offset);

        added_volumes.push_back(new_volume);
    }
}

static TriangleMesh create_mesh(const std::string& type_name, const BoundingBoxf3& bb)
{
    const double side = wxGetApp().plater()->canvas3D()->get_size_proportional_to_max_bed_size(0.1);

    TriangleMesh mesh;
    if (type_name == "Cube")
        // Sitting on the print bed, left front front corner at (0, 0).
        mesh = TriangleMesh(its_make_cube(side, side, side));
    else if (type_name == "Cylinder")
        // Centered around 0, sitting on the print bed.
        // The cylinder has the same volume as the box above.
        mesh = TriangleMesh(its_make_cylinder(0.5 * side, side));
    else if (type_name == "Sphere")
        // Centered around 0, half the sphere below the print bed, half above.
        // The sphere has the same volume as the box above.
        mesh = TriangleMesh(its_make_sphere(0.5 * side, PI / 90));
    else if (type_name == "Slab")
        // Sitting on the print bed, left front front corner at (0, 0).
        mesh = TriangleMesh(its_make_cube(bb.size().x() * 1.5, bb.size().y() * 1.5, bb.size().z() * 0.5));
    else if (type_name == "Cone")
        mesh = TriangleMesh(its_make_cone(0.5 * side, side));
    else if (type_name == "Disc")
        mesh = TriangleMesh(its_make_cylinder(0.5 * side, 0.2f));
    else if (type_name == "Torus")
        mesh = TriangleMesh(its_make_torus(0.5 * side, 0.125 * side,(PI / 60)));
    return mesh;
}

void ObjectList::load_generic_subobject(const std::string& type_name, const ModelVolumeType type)
{
    // BBS: single snapshot
    Plater::SingleSnapshot single(wxGetApp().plater());

    if (type == ModelVolumeType::INVALID) {
        load_shape_object(type_name);
        return;
    }

    const int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0)
        return;

    const Selection& selection = scene_selection();
    assert(obj_idx == selection.get_object_idx());

    /** Any changes of the Object's composition is duplicated for all Object's Instances
      * So, It's enough to take a bounding box of a first selected Instance and calculate Part(generic_subobject) position
      */
    int instance_idx = *selection.get_instance_idxs().begin();
    assert(instance_idx != -1);
    if (instance_idx == -1)
        return;

    take_snapshot("Add primitive");

    // Selected object
    ModelObject  &model_object = *(*m_objects)[obj_idx];
    // Bounding box of the selected instance in world coordinate system including the translation, without modifiers.
    BoundingBoxf3 instance_bb = model_object.instance_bounding_box(instance_idx);

    TriangleMesh mesh = create_mesh(type_name, instance_bb);

	// Mesh will be centered when loading.
    ModelVolume *new_volume = model_object.add_volume(std::move(mesh), type);

    // First (any) GLVolume of the selected instance. They all share the same instance matrix.
    const GLVolume* v = selection.get_first_volume();
    // Transform the new modifier to be aligned with the print bed.
    new_volume->set_transformation(v->get_instance_transformation().get_matrix_no_offset().inverse());
    const BoundingBoxf3 mesh_bb = new_volume->mesh().bounding_box();
    // Set the modifier position.
    auto offset = (type_name == "Slab") ?
        // Slab: Lift to print bed
		Vec3d(0., 0., 0.5 * mesh_bb.size().z() + instance_bb.min.z() - v->get_instance_offset().z()) :
        // Translate the new modifier to be pickable: move to the left front corner of the instance's bounding box, lift to print bed.
        Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()) + 0.5 * mesh_bb.size() - v->get_instance_offset();
    new_volume->set_offset(v->get_instance_transformation().get_matrix_no_offset().inverse() * offset);

    // BBS: backup
    Slic3r::save_object_mesh(model_object);

    const wxString name = _L("Generic") + "-" + _(type_name);
    new_volume->name = into_u8(name);

    // set a default extruder value, since user can't add it manually
    // BBS
    int extruder_id = 0;
    if (new_volume->type() == ModelVolumeType::MODEL_PART && model_object.config.has("extruder"))
        extruder_id = model_object.config.opt_int("extruder");
    new_volume->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
    new_volume->source.is_from_builtin_objects = true;

    select_item([this, obj_idx, new_volume]() {
        wxDataViewItem sel_item;

        wxDataViewItemArray items = reorder_volumes_and_get_selection(obj_idx, [new_volume](const ModelVolume* volume) { return volume == new_volume; });
        if (!items.IsEmpty())
            sel_item = items.front();

        return sel_item;
    });
    if (type == ModelVolumeType::MODEL_PART)
        // update printable state on canvas
        wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_object((size_t)obj_idx);

    // apply the instance transform to all volumes and reset instance transform except the offset
    apply_object_instance_transfrom_to_all_volumes(&model_object);

    selection_changed();

    //BBS: notify partplate the modify
    notify_instance_updated(obj_idx);

    //BBS Switch to Objects List after add a modifier
    wxGetApp().params_panel()->switch_to_object(true);

    //Show Dialog
    if (wxGetApp().app_config->get("do_not_show_modifer_tips").empty()) {
        TipsDialog dlg(wxGetApp().mainframe, _L("Add Modifier"), _L("Switch to per-object setting mode to edit modifier settings."), "do_not_show_modifer_tips");
        dlg.ShowModal();
    }
}

void ObjectList::switch_to_object_process()
{
    wxGetApp().params_panel()->switch_to_object(true);

    // Show Dialog
    if (wxGetApp().app_config->get("do_not_show_object_process_tips").empty()) {
        TipsDialog dlg(wxGetApp().mainframe, _L("Edit Process Settings"), _L("Switch to per-object setting mode to edit process settings of selected objects."), "do_not_show_object_process_tips");
        dlg.ShowModal();
    }
}

void ObjectList::load_shape_object(const std::string &type_name)
{
    const Selection& selection = wxGetApp().plater()->canvas3D()->get_selection();
    //assert(selection.get_object_idx() == -1); // Add nothing is something is selected on 3DScene
    if (selection.get_object_idx() != -1)
        return;

    const int obj_idx = m_objects->size();
    if (obj_idx < 0)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add Primitive");

    // Create mesh
    BoundingBoxf3 bb;
    TriangleMesh mesh = create_mesh(type_name, bb);
    // BBS: remove "Shape" prefix
    load_mesh_object(mesh, _(type_name));
    wxGetApp().mainframe->update_title();
}

void ObjectList::load_mesh_object(const TriangleMesh &mesh, const wxString &name, bool center)
{
    // Add mesh to model as a new object
    Model& model = wxGetApp().plater()->model();

#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    std::vector<size_t> object_idxs;
    auto bb = mesh.bounding_box();
    ModelObject* new_object = model.add_object();
    new_object->name = into_u8(name);
    new_object->add_instance(); // each object should have at least one instance

    ModelVolume* new_volume = new_object->add_volume(mesh);
    new_object->sort_volumes(true);
    new_volume->name = into_u8(name);
    // set a default extruder value, since user can't add it manually
    // BBS
    new_object->config.set_key_value("extruder", new ConfigOptionInt(1));
    new_object->invalidate_bounding_box();
    new_object->translate(-bb.center());

    // BBS: backup
    Slic3r::save_object_mesh(*new_object);

    // BBS: find an empty cell to put the copied object
    auto start_point = wxGetApp().plater()->build_volume().bounding_volume2d().center();
    auto empty_cell  = wxGetApp().plater()->canvas3D()->get_nearest_empty_cell({start_point(0), start_point(1)});

    new_object->instances[0]->set_offset(center ? to_3d(Vec2d(empty_cell(0), empty_cell(1)), -new_object->origin_translation.z()) : bb.center());

    new_object->ensure_on_bed();

    //BBS init assmeble transformation
    Geometry::Transformation t = new_object->instances[0]->get_transformation();
    new_object->instances[0]->set_assemble_transformation(t);

    object_idxs.push_back(model.objects.size() - 1);
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    paste_objects_into_list(object_idxs);

#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */
}

//BBS
bool ObjectList::del_object(const int obj_idx, bool refresh_immediately)
{
    return wxGetApp().plater()->delete_object_from_model(obj_idx, refresh_immediately);
}

// Delete subobject
void ObjectList::del_subobject_item(wxDataViewItem& item)
{
    if (!item) return;

    int obj_idx, idx;
    ItemType type;

    m_objects_model->GetItemInfo(item, type, obj_idx, idx);
    if (type == itUndef)
        return;

    wxDataViewItem parent = m_objects_model->GetParent(item);

    InfoItemType item_info_type = m_objects_model->GetInfoItemType(item);
    if (type & itSettings)
        del_settings_from_config(parent);
    else if (type & itInstanceRoot && obj_idx != -1)
        del_instances_from_object(obj_idx);
    else if (type & itLayerRoot && obj_idx != -1)
        del_layers_from_object(obj_idx);
    else if (type & itLayer && obj_idx != -1)
        del_layer_from_object(obj_idx, m_objects_model->GetLayerRangeByItem(item));
    else if (type & itInfo && obj_idx != -1)
        del_info_item(obj_idx, item_info_type);
    else if (idx == -1)
        return;
    else if (!del_subobject_from_object(obj_idx, idx, type))
        return;

    // If last volume item with warning was deleted, unmark object item
    if (type & itVolume) {
        const std::string& icon_name = get_warning_icon_name(object(obj_idx)->get_object_stl_stats());
        m_objects_model->UpdateWarningIcon(parent, icon_name);
    }

    if (!(type & itInfo) || item_info_type != InfoItemType::CutConnectors) {
        // Connectors Item is already updated/deleted inside the del_info_item()
        m_objects_model->Delete(item);
        update_info_items(obj_idx);
    }
}

void ObjectList::del_info_item(const int obj_idx, InfoItemType type)
{
    Plater* plater = wxGetApp().plater();
    GLCanvas3D* cnv = plater->canvas3D();

    switch (type) {
    case InfoItemType::CustomSupports:
        cnv->get_gizmos_manager().reset_all_states();
        Plater::TakeSnapshot(plater, "Remove support painting");
        for (ModelVolume* mv : (*m_objects)[obj_idx]->volumes)
            mv->supported_facets.reset();
        break;

    // BBS: remove CustomSeam

    case InfoItemType::CutConnectors:
        if (!del_from_cut_object(true)) {
            // there is no need to post EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS if nothing was changed
            return;
        }
        break;

    case InfoItemType::MmSegmentation:
        cnv->get_gizmos_manager().reset_all_states();
        Plater::TakeSnapshot(plater, "Remove color painting");
        for (ModelVolume* mv : (*m_objects)[obj_idx]->volumes)
            mv->mmu_segmentation_facets.reset();
        break;

    case InfoItemType::FuzzySkin:
        cnv->get_gizmos_manager().reset_all_states();
        Plater::TakeSnapshot(plater, _u8L("Remove paint-on fuzzy skin"));
        for (ModelVolume* mv : (*m_objects)[obj_idx]->volumes)
            mv->fuzzy_skin_facets.reset();
        break;

    // BBS: remove Sinking
    case InfoItemType::Undef : assert(false); break;
    }
    cnv->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void ObjectList::del_settings_from_config(const wxDataViewItem& parent_item)
{
    const bool is_layer_settings = m_objects_model->GetItemType(parent_item) == itLayer;

    const size_t opt_cnt = m_config->keys().size();
    if ((opt_cnt == 1 && m_config->has("extruder")) ||
        (is_layer_settings && opt_cnt == 2 && m_config->has("extruder") && m_config->has("layer_height")))
        return;

    take_snapshot("Delete Settings");

    int extruder = m_config->has("extruder") ? m_config->extruder() : -1;

    coordf_t layer_height = 0.0;
    if (is_layer_settings)
        layer_height = m_config->opt_float("layer_height");

    m_config->reset();

    if (extruder >= 0)
        m_config->set_key_value("extruder", new ConfigOptionInt(extruder));
    if (is_layer_settings)
        m_config->set_key_value("layer_height", new ConfigOptionFloat(layer_height));

    changed_object();
}

void ObjectList::del_instances_from_object(const int obj_idx)
{
    auto& instances = (*m_objects)[obj_idx]->instances;
    if (instances.size() <= 1)
        return;

    // BBS: remove snapshot name "Delete All Instances from Object"
    take_snapshot("");

    while ( instances.size()> 1)
        instances.pop_back();

    (*m_objects)[obj_idx]->invalidate_bounding_box(); // ? #ys_FIXME

    changed_object(obj_idx);
}

void ObjectList::del_layer_from_object(const int obj_idx, const t_layer_height_range& layer_range)
{
    const auto del_range = object(obj_idx)->layer_config_ranges.find(layer_range);
    if (del_range == object(obj_idx)->layer_config_ranges.end())
        return;

    take_snapshot("Remove height range");

    object(obj_idx)->layer_config_ranges.erase(del_range);

    changed_object(obj_idx);
}

void ObjectList::del_layers_from_object(const int obj_idx)
{
    object(obj_idx)->layer_config_ranges.clear();

    changed_object(obj_idx);
}

bool ObjectList::del_from_cut_object(bool is_cut_connector, bool is_model_part/* = false*/, bool is_negative_volume/* = false*/)
{
    const long buttons_style = is_cut_connector   ? (wxYES | wxNO | wxCANCEL) : (wxYES | wxCANCEL);

    const wxString title     = is_cut_connector   ? _L("Delete connector from object which is a part of cut") :
                               is_model_part      ? _L("Delete solid part from object which is a part of cut") :
                               is_negative_volume ? _L("Delete negative volume from object which is a part of cut") : "";
                             
    const wxString msg_end   = is_cut_connector   ? ("\n" + _L("To save cut correspondence you can delete all connectors from all related objects.")) : "";

    InfoDialog dialog(wxGetApp().plater(), title,
                      (_L("This action will break a cut correspondence.\n"
                         "After that model consistency can't be guaranteed.\n"
                         "\n"
                         "To manipulate with solid parts or negative volumes you have to invalidate cut information first.") + msg_end ),
                      false, buttons_style | wxCANCEL_DEFAULT | wxICON_WARNING);

    dialog.SetButtonLabel(wxID_YES, _L("Invalidate cut info"));
    if (is_cut_connector)
        dialog.SetButtonLabel(wxID_NO, _L("Delete all connectors"));

    const int answer = dialog.ShowModal();
    if (answer == wxID_CANCEL)
        return false;

    if (answer == wxID_YES)
        invalidate_cut_info_for_selection();
    else if (answer == wxID_NO)
        delete_all_connectors_for_selection();
    return true;
}

bool ObjectList::del_subobject_from_object(const int obj_idx, const int idx, const int type)
{
    assert(idx >= 0);

    // BBS: support partplage logic
    int n_plates = wxGetApp().plater()->get_partplate_list().get_plate_count();
	if ((obj_idx >= 1000 && obj_idx < 1000 + n_plates) || idx<0)
		// Cannot delete a wipe tower or volume with negative id
		return false;

    ModelObject* object = (*m_objects)[obj_idx];

    if (type == itVolume) {
        const auto volume = object->volumes[idx];

        // if user is deleting the last solid part, throw error
        int solid_cnt = 0;
        for (auto vol : object->volumes)
            if (vol->is_model_part())
                ++solid_cnt;
        if (volume->is_model_part() && solid_cnt == 1) {
            Slic3r::GUI::show_error(nullptr, _L("Deleting the last solid part is not allowed."));
            return false;
        }
        if (object->is_cut() && (volume->is_model_part() || volume->is_negative_volume())) {
            del_from_cut_object(volume->is_cut_connector(), volume->is_model_part(), volume->is_negative_volume());
            // in any case return false to break the deletion
            return false;
        }

        take_snapshot("Delete part");

        object->delete_volume(idx);

        if (object->volumes.size() == 1) {
            const auto last_volume = object->volumes[0];
            if (!last_volume->config.empty()) {
                object->config.apply(last_volume->config);
                last_volume->config.reset();

                // update extruder color in ObjectList
                wxDataViewItem obj_item = m_objects_model->GetItemById(obj_idx);
                if (obj_item) {
                    // BBS
                    if (last_volume->config.has("extruder")) {
                        int extruder_id = last_volume->config.opt_int("extruder");
                        object->config.set("extruder", extruder_id);
                    }
                    wxString extruder = object->config.has("extruder") ? wxString::Format("%d", object->config.extruder()) : _devL("1");
                    m_objects_model->SetExtruder(extruder, obj_item);
                }
                // add settings to the object, if it has them
                add_settings_item(obj_item, &object->config.get());
            }
        }
        //BBS: notify partplate the modify
        notify_instance_updated(obj_idx);
    }
    else if (type == itInstance) {
        if (object->instances.size() == 1) {
            // BBS: remove snapshot name "Last instance of an object cannot be deleted."
            Slic3r::GUI::show_error(nullptr, "");
            return false;
        }

        // BBS: remove snapshot name "Delete Instance"
        take_snapshot("");
        object->delete_instance(idx);
    }
    else
        return false;

    changed_object(obj_idx);

    return true;
}

void ObjectList::split()
{
    const auto item = GetSelection();
    const int obj_idx = get_selected_obj_idx();
    if (!item || obj_idx < 0)
        return;

    ModelVolume* volume;
    if (!get_volume_by_item(item, volume)) return;
    DynamicPrintConfig&	config = printer_config();
    // BBS
    const ConfigOptionStrings* filament_colors = config.option<ConfigOptionStrings>("filament_colour", false);
    const auto filament_cnt = (filament_colors == nullptr) ? size_t(1) : filament_colors->size();
    if (!volume->is_splittable()) {
        wxMessageBox(_(L("The target object contains only one part and can not be split.")));
        return;
    }

    take_snapshot("Split to parts");

    volume->split(filament_cnt);

    wxBusyCursor wait;

    auto model_object = (*m_objects)[obj_idx];

    auto parent = m_objects_model->GetObject(item);
    if (parent)
        m_objects_model->DeleteVolumeChildren(parent);
    else
        parent = item;

    for (const ModelVolume* volume : model_object->volumes) {
        const wxDataViewItem& vol_item = m_objects_model->AddVolumeChild(parent, from_u8(volume->name),
            volume->type(),// is_modifier() ? ModelVolumeType::PARAMETER_MODIFIER : ModelVolumeType::MODEL_PART,
            volume->is_text(),
            volume->is_svg(),
			get_warning_icon_name(volume->mesh().stats()),
            volume->config.has("extruder") ? volume->config.extruder() : 0,
            false);
        // add settings to the part, if it has those
        add_settings_item(vol_item, &volume->config.get());
    }

    model_object->input_file.clear();

    if (parent == item)
        Expand(parent);

    changed_object(obj_idx);

    //BBS: notify partplate the modify
    notify_instance_updated(obj_idx);

    update_info_items(obj_idx);
}

void ObjectList::merge(bool to_multipart_object)
{
    wxBusyCursor wait;
    // merge selected objects to the multipart object
    if (to_multipart_object) {
        auto get_object_idxs = [this](std::vector<int>& obj_idxs, wxDataViewItemArray& sels)
        {
            // check selections and split instances to the separated objects...
            bool instance_selection = false;
            for (wxDataViewItem item : sels)
                if (m_objects_model->GetItemType(item) & itInstance) {
                    instance_selection = true;
                    break;
                }

            if (!instance_selection) {
                for (wxDataViewItem item : sels) {
                    assert(m_objects_model->GetItemType(item) & itObject);
                    obj_idxs.emplace_back(m_objects_model->GetIdByItem(item));
                }
                return;
            }

            // map of obj_idx -> set of selected instance_idxs
            std::map<int, std::set<int>> sel_map;
            std::set<int> empty_set;
            for (wxDataViewItem item : sels) {
                if (m_objects_model->GetItemType(item) & itObject) {
                    int obj_idx = m_objects_model->GetIdByItem(item);
                    int inst_cnt = (*m_objects)[obj_idx]->instances.size();
                    if (inst_cnt == 1)
                        sel_map.emplace(obj_idx, empty_set);
                    else
                        for (int i = 0; i < inst_cnt; i++)
                            sel_map[obj_idx].emplace(i);
                    continue;
                }
                int obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetObject(item));
                sel_map[obj_idx].emplace(m_objects_model->GetInstanceIdByItem(item));
            }

            // all objects, created from the instances will be added to the end of list
            int new_objects_cnt = 0; // count of this new objects

            for (auto map_item : sel_map) {
                int obj_idx = map_item.first;
                // object with just 1 instance
                if (map_item.second.empty()) {
                    obj_idxs.emplace_back(obj_idx);
                    continue;
                }

                // object with selected all instances
                if ((*m_objects)[map_item.first]->instances.size() == map_item.second.size()) {
                    instances_to_separated_objects(obj_idx);
                    // first instance stay on its own place and another all add to the end of list :
                    obj_idxs.emplace_back(obj_idx);
                    new_objects_cnt += map_item.second.size() - 1;
                    continue;
                }

                // object with selected some of instances
                instances_to_separated_object(obj_idx, map_item.second);

                if (map_item.second.size() == 1)
                    new_objects_cnt += 1;
                else {// we should split to separate instances last object
                    instances_to_separated_objects(m_objects->size() - 1);
                    // all instances will stay at the end of list :
                    new_objects_cnt += map_item.second.size();
                }
            }

            // all instatnces are extracted to the separate objects and should be selected
            m_prevent_list_events = true;
            sels.Clear();
            for (int obj_idx : obj_idxs)
                sels.Add(m_objects_model->GetItemById(obj_idx));
            int obj_cnt = m_objects->size();
            for (int obj_idx = obj_cnt - new_objects_cnt; obj_idx < obj_cnt; obj_idx++) {
                sels.Add(m_objects_model->GetItemById(obj_idx));
                obj_idxs.emplace_back(obj_idx);
            }
            UnselectAll();
            SetSelections(sels);
            assert(!sels.IsEmpty());
            m_prevent_list_events = false;
        };

        std::vector<int> obj_idxs;
        wxDataViewItemArray sels;
        GetSelections(sels);
        assert(!sels.IsEmpty());

        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Assemble");

        get_object_idxs(obj_idxs, sels);

        // resulted objects merge to the one
        Model* model = (*m_objects)[0]->get_model();
        ModelObject* new_object = model->add_object();
        new_object->name = _u8L("Assembly");
        ModelConfig &config = new_object->config;

        Slic3r::SaveObjectGaurd gaurd(*new_object);

        for (int obj_idx : obj_idxs) {
            ModelObject* object = (*m_objects)[obj_idx];

            const Geometry::Transformation& transformation = object->instances[0]->get_transformation();
            //const Vec3d scale     = transformation.get_scaling_factor();
            //const Vec3d mirror    = transformation.get_mirror();
            //const Vec3d rotation  = transformation.get_rotation();

            if (object->id() == (*m_objects)[obj_idxs.front()]->id())
                new_object->add_instance();
            const Transform3d& transformation_matrix = transformation.get_matrix();

            // merge volumes
            for (const ModelVolume* volume : object->volumes) {
                ModelVolume* new_volume = new_object->add_volume(*volume);

                //BBS: keep volume's transform
                const Transform3d& volume_matrix = new_volume->get_matrix();
                Transform3d new_matrix = transformation_matrix * volume_matrix;
                new_volume->set_transformation(new_matrix);
                //set rotation
                /*const Vec3d vol_rot = new_volume->get_rotation() + rotation;
                new_volume->set_rotation(vol_rot);

                // set scale
                const Vec3d vol_sc_fact = new_volume->get_scaling_factor().cwiseProduct(scale);
                new_volume->set_scaling_factor(vol_sc_fact);

                // set mirror
                const Vec3d vol_mirror = new_volume->get_mirror().cwiseProduct(mirror);
                new_volume->set_mirror(vol_mirror);

                // set offset
                const Vec3d vol_offset = volume_offset_correction* new_volume->get_offset();
                new_volume->set_offset(vol_offset);*/

                //BBS: add config from old objects
                //for object config, it has settings of PrintObjectConfig and PrintRegionConfig
                //for volume config, it only has settings of PrintRegionConfig
                //so we can not copy settings from object to volume
                //but we can copy settings from volume to object
                if (object->volumes.size() > 1){
                    new_volume->config.assign_config(volume->config);
                }
                auto option = new_volume->config.option("extruder");
                if (!option) {
                    auto opt = object->config.option("extruder");
                    if (opt) { new_volume->config.set_key_value("extruder", new ConfigOptionInt(opt->getInt())); }
                }
                new_volume->mmu_segmentation_facets.assign(std::move(volume->mmu_segmentation_facets));
            }
            new_object->sort_volumes(true);

            // merge settings
            auto new_opt_keys = config.keys();
            const ModelConfig& from_config = object->config;
            auto opt_keys = from_config.keys();

            for (auto& opt_key : opt_keys) {
                if (find(new_opt_keys.begin(), new_opt_keys.end(), opt_key) == new_opt_keys.end()) {
                    const ConfigOption* option = from_config.option(opt_key);
                    if (!option) {
                        // if current option doesn't exist in prints.get_edited_preset(),
                        // get it from default config values
                        option = DynamicPrintConfig::new_from_defaults_keys({ opt_key })->option(opt_key);
                    }
                    config.set_key_value(opt_key, option->clone());
                }
            }
            // save extruder value if it was set
            if (object->volumes.size() == 1 && find(opt_keys.begin(), opt_keys.end(), "extruder") != opt_keys.end()) {
                ModelVolume* volume = new_object->volumes.back();
                const ConfigOption* option = from_config.option("extruder");
                if (option)
                    volume->config.set_key_value("extruder", option->clone());
            }

            // merge layers
            for (const auto& range : object->layer_config_ranges)
                new_object->layer_config_ranges.emplace(range);

            // merge brim ears
            BrimPoints temp_brim_points = object->brim_points;
            for(auto& p : temp_brim_points) {
                p.set_transform(transformation_matrix);
                new_object->brim_points.push_back(p);
            }
        }

        //BBS: ensure on bed, and no need to center around origin
        new_object->ensure_on_bed();
        new_object->center_around_origin();
        new_object->translate_instances(-new_object->origin_translation);
        new_object->origin_translation = Vec3d::Zero();
        //BBS init asssmble transformation
        Geometry::Transformation new_object_trsf = new_object->instances[0]->get_transformation();
        new_object->instances[0]->set_assemble_transformation(new_object_trsf);

        const Transform3d& new_object_inverse_matrix = new_object_trsf.get_matrix().inverse();
        for (auto& p : new_object->brim_points) {
            p.set_transform(new_object_inverse_matrix);
        }
        //BBS: notify it before remove
        notify_instance_updated(m_objects->size() - 1);

        // remove selected objects
        remove();

        // Add new object(merged) to the object_list
        add_object_to_list(m_objects->size() - 1);
        select_item(m_objects_model->GetItemById(m_objects->size() - 1));
        update_selections_on_canvas();
    }
    // merge all parts to the one single object
    // all part's settings will be lost
    else {
        wxDataViewItem item = GetSelection();
        if (!item)
            return;
        const int obj_idx = m_objects_model->GetIdByItem(item);
        if (obj_idx == -1)
            return;

        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Merge parts to an object");

        ModelObject* model_object = (*m_objects)[obj_idx];
        model_object->merge();

        m_objects_model->DeleteVolumeChildren(item);

        changed_object(obj_idx);
    }
}

/*void ObjectList::merge_volumes()
{
    std::vector<int> obj_idxs, vol_idxs;
    get_selection_indexes(obj_idxs, vol_idxs);
    if (obj_idxs.empty() && vol_idxs.empty())
        return;

    wxBusyCursor wait;
#if 0
    ModelObjectPtrs objects;
    for (int obj_idx : obj_idxs) {
        ModelObject* object = (*m_objects)[obj_idx];
        object->merge_volumes(vol_idxs);
        //changed_object(obj_idx);
        //remove();
    }
   /* wxGetApp().plater()->load_model_objects(objects);

    Selection& selection = p->view3D->get_canvas3d()->get_selection();
    size_t last_obj_idx = p->model.objects.size() - 1;

    if (vol_idxs.empty()) {
        for (size_t i = 0; i < objects.size(); ++i)
            selection.add_object((unsigned int)(last_obj_idx - i), i == 0);
    }
    else {
        for (int vol_idx : vol_idxs)
            selection.add_volume(last_obj_idx, vol_idx, 0, false);
    }#1#
#else
    wxGetApp().plater()->merge(obj_idxs[0], vol_idxs);
#endif
}*/

void ObjectList::layers_editing()
{
    const Selection& selection = scene_selection();
    const int obj_idx = selection.get_object_idx();
    wxDataViewItem item = obj_idx >= 0 && GetSelectedItemsCount() > 1 && selection.is_single_full_object() ?
                          m_objects_model->GetItemById(obj_idx) :
                          GetSelection();

    if (!item)
        return;

    const wxDataViewItem obj_item = m_objects_model->GetObject(item);
    wxDataViewItem layers_item = m_objects_model->GetLayerRootItem(obj_item);

    // if it doesn't exist now
    if (!layers_item.IsOk())
    {
        t_layer_config_ranges& ranges = object(obj_idx)->layer_config_ranges;

        // set some default value
        if (ranges.empty()) {
            // BBS: remove snapshot name "Add layers"
            take_snapshot("Add layers");
            ranges[{ 0.0f, 2.0f }].assign_config(get_default_layer_config(obj_idx));
        }

        // create layer root item
        layers_item = add_layer_root_item(obj_item);
    }
    if (!layers_item.IsOk())
        return;

    // to correct visual hints for layers editing on the Scene, reset previous selection
    wxGetApp().obj_layers()->reset_selection();
    wxGetApp().plater()->canvas3D()->handle_sidebar_focus_event("", false);

    // select LayerRoor item and expand
    select_item(layers_item);
    Expand(layers_item);
}

// BBS: merge parts of a single object into one volume, similar to export_stl, but no need to export and then import
void ObjectList::boolean()
{
    std::vector<int> obj_idxs, vol_idxs;
    get_selection_indexes(obj_idxs, vol_idxs);
    if (obj_idxs.empty() && vol_idxs.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "boolean");

    ModelObject* object = (*m_objects)[obj_idxs.front()];
    TriangleMesh mesh = Plater::combine_mesh_fff(*object, -1, [this](const std::string& msg) {return wxGetApp().notification_manager()->push_plater_error_notification(msg); });

    // add mesh to model as a new object, keep the original object's name and config
    Model* model = object->get_model();
    ModelObject* new_object = model->add_object();
    new_object->name = object->name;
    new_object->config.assign_config(object->config);
    if (new_object->instances.empty())
        new_object->add_instance();
    ModelVolume* new_volume = new_object->add_volume(mesh);

    // BBS: ensure on bed but no need to ensure locate in the center around origin
    new_object->ensure_on_bed();
    new_object->center_around_origin();
    new_object->translate_instances(-new_object->origin_translation);
    new_object->origin_translation = Vec3d::Zero();

    // BBS: notify it before move
    notify_instance_updated(m_objects->size() - 1);

    // remove selected objects
    remove();

    // Add new object(UNION) to the object_list
    add_object_to_list(m_objects->size() - 1);
    select_item(m_objects_model->GetItemById(m_objects->size() - 1));
    update_selections_on_canvas();
}

wxDataViewItem ObjectList::add_layer_root_item(const wxDataViewItem obj_item)
{
    const int obj_idx = m_objects_model->GetIdByItem(obj_item);
    if (obj_idx < 0 ||
        object(obj_idx)->layer_config_ranges.empty() ||
        printer_technology() == ptSLA)
        return wxDataViewItem(nullptr);

    // create LayerRoot item
    wxDataViewItem layers_item = m_objects_model->AddLayersRoot(obj_item);

    // and create Layer item(s) according to the layer_config_ranges
    for (const auto& range : object(obj_idx)->layer_config_ranges)
        add_layer_item(range.first, layers_item);

    Expand(layers_item);
    return layers_item;
}

DynamicPrintConfig ObjectList::get_default_layer_config(const int obj_idx)
{
    DynamicPrintConfig config;
    coordf_t layer_height = object(obj_idx)->config.has("layer_height") ?
                            object(obj_idx)->config.opt_float("layer_height") :
                            wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("layer_height");
    config.set_key_value("layer_height",new ConfigOptionFloat(layer_height));
    // BBS
    int extruder = object(obj_idx)->config.has("extruder") ?
        object(obj_idx)->config.opt_int("extruder") :
        wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("extruder");
    config.set_key_value("extruder",    new ConfigOptionInt(0));

    return config;
}

bool ObjectList::get_volume_by_item(const wxDataViewItem& item, ModelVolume*& volume)
{
    auto obj_idx = get_selected_obj_idx();
    if (!item || obj_idx < 0)
        return false;
    const auto volume_id = m_objects_model->GetVolumeIdByItem(item);
    const bool split_part = m_objects_model->GetItemType(item) == itVolume;

    // object is selected
    if (volume_id < 0) {
        if ( split_part || (*m_objects)[obj_idx]->volumes.size() > 1 )
            return false;
        volume = (*m_objects)[obj_idx]->volumes[0];
    }
    // volume is selected
    else
        volume = (*m_objects)[obj_idx]->volumes[volume_id];

    return true;
}

bool ObjectList::is_splittable(bool to_objects)
{
    const wxDataViewItem item = GetSelection();
    if (!item) return false;

    if (to_objects)
    {
        ItemType type = m_objects_model->GetItemType(item);
        if (type == itVolume)
            return false;
        if (type == itObject || m_objects_model->GetItemType(m_objects_model->GetParent(item)) == itObject) {
            auto obj_idx = get_selected_obj_idx();
            if (obj_idx < 0)
                return false;
            if ((*m_objects)[obj_idx]->volumes.size() > 1)
                return true;
            return (*m_objects)[obj_idx]->volumes[0]->is_splittable();
        }
        return false;
    }

    ModelVolume* volume;
    if (!get_volume_by_item(item, volume) || !volume)
        return false;

    return volume->is_splittable();
}

bool ObjectList::selected_instances_of_same_object()
{
    wxDataViewItemArray sels;
    GetSelections(sels);

    const int obj_idx = m_objects_model->GetObjectIdByItem(sels.front());

    for (auto item : sels) {
        if (! (m_objects_model->GetItemType(item) & itInstance) ||
            obj_idx != m_objects_model->GetObjectIdByItem(item))
            return false;
    }
    return true;
}

bool ObjectList::can_split_instances()
{
    const Selection& selection = scene_selection();
    return selection.is_multiple_full_instance() || selection.is_single_full_instance();
}

bool ObjectList::has_selected_cut_object() const
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return false;

    for (wxDataViewItem item : sels) {
        const int obj_idx = m_objects_model->GetObjectIdByItem(item);
        // ys_FIXME: The obj_idx<size condition is a workaround for https://github.com/prusa3d/PrusaSlicer/issues/11186,
        // but not the correct fix. The deleted item probably should not be in sels in the first place.
        if (obj_idx >= 0 && obj_idx < int(m_objects->size()) && object(obj_idx)->is_cut())
            return true;
    }

    return false;
}

void ObjectList::invalidate_cut_info_for_selection()
{
    const wxDataViewItem item = GetSelection();
    if (item) {
        const int obj_idx = m_objects_model->GetObjectIdByItem(item);
        if (obj_idx >= 0)
            invalidate_cut_info_for_object(size_t(obj_idx));
    }
}

void ObjectList::invalidate_cut_info_for_object(int obj_idx)
{
    ModelObject* init_obj = object(obj_idx);
    if (!init_obj->is_cut())
        return;

    take_snapshot(_u8L("Invalidate cut info"));

    const CutObjectBase cut_id = init_obj->cut_id;
    // invalidate cut for related objects (which have the same cut_id)
    for (size_t idx = 0; idx < m_objects->size(); idx++)
        if (ModelObject* obj = object(int(idx)); obj->cut_id.is_equal(cut_id)) {
            obj->invalidate_cut();
            update_info_items(idx);
            add_volumes_to_object_in_list(idx);
        }

    update_lock_icons_for_model();
}

void ObjectList::delete_all_connectors_for_selection()
{
    const wxDataViewItem item = GetSelection();
    if (item) {
        const int obj_idx = m_objects_model->GetObjectIdByItem(item);
        if (obj_idx >= 0)
            delete_all_connectors_for_object(size_t(obj_idx));
    }
}

void ObjectList::delete_all_connectors_for_object(int obj_idx)
{
    ModelObject* init_obj = object(obj_idx);
    if (!init_obj->is_cut())
        return;

    take_snapshot(_u8L("Delete all connectors"));

    const CutObjectBase cut_id = init_obj->cut_id;
    // Delete all connectors for related objects (which have the same cut_id)
    Model& model = wxGetApp().plater()->model();
    for (int idx = int(m_objects->size())-1; idx >= 0; idx--)
        if (ModelObject* obj = object(idx); obj->cut_id.is_equal(cut_id)) {
            obj->delete_connectors();

            if (obj->volumes.empty() || !obj->has_solid_mesh()) {
                model.delete_object(idx);
                m_objects_model->Delete(m_objects_model->GetItemById(idx));
                continue;
            }

            update_info_items(idx);
            add_volumes_to_object_in_list(idx);
            changed_object(int(idx));
        }

    update_lock_icons_for_model();
}

bool ObjectList::can_merge_to_multipart_object() const
{
    if (has_selected_cut_object())
        return false;

    if (printer_technology() == ptSLA)
        return false;

    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return false;

    // should be selected just objects
    for (wxDataViewItem item : sels)
        if (!(m_objects_model->GetItemType(item) & (itObject | itInstance)))
            return false;

    return true;
}

bool ObjectList::can_merge_to_single_object() const
{
    int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0)
        return false;

    // selected object should be multipart
    return (*m_objects)[obj_idx]->volumes.size() > 1;
}

bool ObjectList::can_mesh_boolean() const
{
    int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0)
        return false;

    // selected object should be multi mesh
    return (*m_objects)[obj_idx]->volumes.size() > 1 || ((*m_objects)[obj_idx]->volumes.size() == 1 && (*m_objects)[obj_idx]->volumes[0]->is_splittable());
}

// NO_PARAMETERS function call means that changed object index will be determine from Selection()
void ObjectList::changed_object(const int obj_idx/* = -1*/) const
{
    wxGetApp().plater()->changed_object(obj_idx < 0 ? get_selected_obj_idx() : obj_idx);
}

void ObjectList::part_selection_changed()
{
    if (m_extruder_editor) m_extruder_editor->Hide();
    int obj_idx = -1;
    int volume_id = -1;
    m_config = nullptr;
    wxString og_name = wxEmptyString;

    bool update_and_show_manipulations = false;
    bool update_and_show_settings = false;
    bool update_and_show_layers = false;

    bool enable_manipulation{true};
    bool disable_ss_manipulation{false};
    bool disable_ununiform_scale{false};

    const auto item = GetSelection();
    if (item && m_objects_model->GetItemType(item) == itInfo && m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors) {
        og_name = _L("Cut Connectors information");

        update_and_show_manipulations = true;
        enable_manipulation           = false;
        disable_ununiform_scale       = true;
    }
    else if (item && (m_objects_model->GetItemType(item) & itPlate)) {
        // BBS
        // TODO: og for plate
    }
    else if ( multiple_selection() || (item && m_objects_model->GetItemType(item) == itInstanceRoot )) {
        const Selection& selection = scene_selection();

        if (selection.is_single_full_object()) {
            og_name                       = _L("Object manipulation");
            update_and_show_manipulations = true;

            obj_idx                 = selection.get_object_idx();
            ModelObject *object     = (*m_objects)[obj_idx];
            m_config                = &object->config;
            disable_ss_manipulation = object->is_cut();
        }
        else {
            og_name = _L("Group manipulation");

            // don't show manipulation panel for case of all Object's parts selection
            update_and_show_manipulations = !selection.is_single_full_instance();

            if (int obj_idx = selection.get_object_idx(); obj_idx >= 0) {
                if (selection.is_any_volume() || selection.is_any_modifier())
                    enable_manipulation = !(*m_objects)[obj_idx]->is_cut();
                else // if (item && m_objects_model->GetItemType(item) == itInstanceRoot)
                    disable_ss_manipulation = (*m_objects)[obj_idx]->is_cut();
            }
            else {
                wxDataViewItemArray sels;
                GetSelections(sels);
                if (selection.is_single_full_object() || selection.is_multiple_full_instance()) {
                    int obj_idx             = m_objects_model->GetObjectIdByItem(sels.front());
                    disable_ss_manipulation = (*m_objects)[obj_idx]->is_cut();
                } else if (selection.is_mixed() || selection.is_multiple_full_object()) {
                    std::map<CutObjectBase, std::set<int>> cut_objects;

                    // find cut objects
                    for (auto item : sels) {
                        int                obj_idx = m_objects_model->GetObjectIdByItem(item);
                        const ModelObject *obj     = object(obj_idx);
                        if (obj && obj->is_cut()) {
                            if (cut_objects.find(obj->cut_id) == cut_objects.end())
                                cut_objects[obj->cut_id] = std::set<int>{obj_idx};
                            else
                                cut_objects.at(obj->cut_id).insert(obj_idx);
                        }
                    }

                    // check if selected cut objects are "full selected"
                    for (auto cut_object : cut_objects)
                        if (cut_object.first.check_sum() != cut_object.second.size()) {
                            disable_ss_manipulation = true;
                            break;
                        }
                    disable_ununiform_scale = !cut_objects.empty();
                }
            }
        }

        // BBS: multi config editing
        update_and_show_settings = true;
    }
    else {
        if (item) {
            // BBS
            const ItemType type = m_objects_model->GetItemType(item);
            const wxDataViewItem parent = m_objects_model->GetParent(item);
            const ItemType parent_type = m_objects_model->GetItemType(parent);
            obj_idx = m_objects_model->GetObjectIdByItem(item);

            if ((type & itObject) || type == itInfo) {
                m_config = &(*m_objects)[obj_idx]->config;
                update_and_show_manipulations = true;

                if (type == itInfo) {
                    InfoItemType info_type = m_objects_model->GetInfoItemType(item);
                    switch (info_type)
                    {
                    case InfoItemType::CustomSupports:
                    // BBS: remove CustomSeam
                    //case InfoItemType::CustomSeam:
                    case InfoItemType::MmSegmentation:
                    case InfoItemType::FuzzySkin:
                    {
                        GLGizmosManager::EType gizmo_type = info_type == InfoItemType::CustomSupports ? GLGizmosManager::EType::FdmSupports :
                                                            /*info_type == InfoItemType::CustomSeam ? GLGizmosManager::EType::Seam :*/
                                                            info_type == InfoItemType::FuzzySkin        ? GLGizmosManager::EType::FuzzySkin :
                                                            GLGizmosManager::EType::MmSegmentation;
                        GLGizmosManager& gizmos_mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
                        if (gizmos_mgr.get_current_type() != gizmo_type)
                            gizmos_mgr.open_gizmo(gizmo_type);
                        break;
                    }
                    // BBS: remove Sinking
                    //case InfoItemType::Sinking: { break; }
                    default: { break; }
                    }
                } else {
                    // BBS: select object to edit config
                    m_config = &(*m_objects)[obj_idx]->config;
                    update_and_show_settings = true;
                    disable_ss_manipulation  = (*m_objects)[obj_idx]->is_cut();
                }
            }
            else {
                if (type & itSettings) {
                    if (parent_type & itObject) {
                        og_name  = _L("Object Settings to modify");
                        m_config = &(*m_objects)[obj_idx]->config;
                    }
                    else if (parent_type & itVolume) {
                        og_name   = _L("Part Settings to modify");
                        volume_id = m_objects_model->GetVolumeIdByItem(parent);
                        m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
                    }
                    else if (parent_type & itLayer) {
                        og_name  = _L("Layer range Settings to modify");
                        m_config = &get_item_config(parent);
                    }
                    update_and_show_settings = true;
                }
                else if (type & itVolume) {
                    og_name = _L("Part manipulation");
                    volume_id = m_objects_model->GetVolumeIdByItem(item);
                    m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
                    update_and_show_manipulations = true;
                    m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
                    update_and_show_settings = true;

                    const ModelVolume *volume = (*m_objects)[obj_idx]->volumes[volume_id];
                    enable_manipulation       = !((*m_objects)[obj_idx]->is_cut() && (volume->is_cut_connector() || volume->is_model_part()));
                }
                else if (type & itInstance) {
                    og_name = _L("Instance manipulation");
                    update_and_show_manipulations = true;

                    // fill m_config by object's values
                    m_config = &(*m_objects)[obj_idx]->config;
                    disable_ss_manipulation = (*m_objects)[obj_idx]->is_cut();
                }
                else if (type & (itLayerRoot | itLayer)) {
                    og_name = type & itLayerRoot ? _L("Height ranges") : _L("Settings for height range");
                    update_and_show_layers = true;
                    update_and_show_settings = true;

                    if (type & itLayer)
                        m_config = &get_item_config(item);
                }
            }
        }
    }

    m_selected_object_id = obj_idx;

    if (update_and_show_manipulations) {
        // BBS
        //wxGetApp().obj_manipul()->get_og()->set_name(" " + og_name + " ");

        if (item) {
            // wxGetApp().obj_manipul()->get_og()->set_value("object_name", m_objects_model->GetName(item));
            // BBS
            //wxGetApp().obj_manipul()->update_item_name(m_objects_model->GetName(item));
            //wxGetApp().obj_manipul()->update_warning_icon_state(get_mesh_errors_info(obj_idx, volume_id));
        }

        GLGizmosManager &gizmos_mgr = wxGetApp().plater()->canvas3D()->get_gizmos_manager();

        if (GLGizmoScale3D *scale = dynamic_cast<GLGizmoScale3D *>(gizmos_mgr.get_gizmo(GLGizmosManager::Scale)))
            scale->enable_ununiversal_scale(!disable_ununiform_scale);
    }

#if !NEW_OBJECT_SETTING
    if (update_and_show_settings)
        wxGetApp().obj_settings()->get_og()->set_name(" " + og_name + " ");
#endif

    if (!this->IsShown())
        update_and_show_layers = false;
    if (printer_technology() == ptSLA)
        update_and_show_layers = false;
    else if (update_and_show_layers) {
        ;//wxGetApp().obj_layers()->get_og()->set_name(" " + og_name + " ");
    }

    update_min_height();

    Sidebar& panel = wxGetApp().sidebar();
    panel.Freeze();


    const ItemType type = m_objects_model->GetItemType(item);
    if (!(type & itLayer)) {
        wxGetApp().plater()->canvas3D()->handle_sidebar_focus_event("", false);
    }
    // BBS
    //wxGetApp().obj_manipul() ->UpdateAndShow(update_and_show_manipulations);
    wxGetApp().obj_settings()->UpdateAndShow(update_and_show_settings);
    wxGetApp().obj_layers()  ->UpdateAndShow(update_and_show_layers);
    wxGetApp().plater()->show_object_info();

    panel.Layout();
    panel.Thaw();
}

// Add new SettingsItem for parent_item if it doesn't exist, or just update a digest according to new config
wxDataViewItem ObjectList::add_settings_item(wxDataViewItem parent_item, const DynamicPrintConfig* config)
{
    wxDataViewItem ret = wxDataViewItem(nullptr);

    if (!parent_item)
        return ret;

    const bool is_object_settings = m_objects_model->GetItemType(parent_item) == itObject;
    const bool is_volume_settings = m_objects_model->GetItemType(parent_item) == itVolume;
    const bool is_layer_settings = m_objects_model->GetItemType(parent_item) == itLayer;
    if (!is_object_settings) {
        ModelVolumeType volume_type = m_objects_model->GetVolumeType(parent_item);
        if (volume_type == ModelVolumeType::NEGATIVE_VOLUME || volume_type == ModelVolumeType::SUPPORT_BLOCKER || volume_type == ModelVolumeType::SUPPORT_ENFORCER)
            return ret;
    }

    SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(config, is_object_settings, is_layer_settings);
    if (is_layer_settings) {
        auto tab_object = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab());
        auto object_cfg = tab_object->get_config();
        if (config->opt_float("layer_height") == object_cfg->opt_float("layer_height")) {
            SettingsFactory::Bundle new_cat_options;
            for (auto cat_opt : cat_options) {
                std::vector<string> temp;
                for (auto value : cat_opt.second) {
                    if (value != "layer_height")
                        temp.push_back(value);
                }
                if (!temp.empty())
                    new_cat_options[cat_opt.first] = temp;
            }
            cat_options = new_cat_options;
        }
    }

    if (cat_options.empty()) {
#if NEW_OBJECT_SETTING
        ObjectDataViewModelNode *node = static_cast<ObjectDataViewModelNode*>(parent_item.GetID());
        if (node) node->set_action_icon(false);
        m_objects_model->ItemChanged(parent_item);
        return parent_item;
#else
        return ret;
#endif
    }

    std::vector<std::string> categories;
    categories.reserve(cat_options.size());
    for (auto& cat : cat_options)
        categories.push_back(cat.first);

    if (m_objects_model->GetItemType(parent_item) & itInstance)
        parent_item = m_objects_model->GetObject(parent_item);

#if NEW_OBJECT_SETTING
    ObjectDataViewModelNode *node = static_cast<ObjectDataViewModelNode*>(parent_item.GetID());
    if (node) node->set_action_icon(true);
    m_objects_model->ItemChanged(parent_item);
    return parent_item;
#else
    ret = m_objects_model->IsSettingsItem(parent_item) ? parent_item : m_objects_model->GetSettingsItem(parent_item);

    if (!ret) ret = m_objects_model->AddSettingsChild(parent_item);

    m_objects_model->UpdateSettingsDigest(ret, categories);
    Expand(parent_item);

    return ret;
#endif
}

void ObjectList::update_variable_layer_obj_num(ObjectDataViewModelNode* obj_node, size_t layer_data_count) {
    if (obj_node){
        if (obj_node->IsVaribaleHeight() == hiVariable && layer_data_count <= 4){
            m_variable_layer_obj_num--;
        }
        else if (obj_node->IsVaribaleHeight() == hiUnVariable && layer_data_count > 4){
            m_variable_layer_obj_num++;
        }
        GetColumn(colHeight)->SetHidden(m_variable_layer_obj_num == 0);
        update_name_column_width();
    }
}

void ObjectList::update_info_items(size_t obj_idx, wxDataViewItemArray *selections /* = nullptr*/, bool added_object /* = false*/, bool color_mode_changed /* = false*/)
{
    // BBS
    if (obj_idx >= m_objects->size())
        return;

    const ModelObject* model_object = (*m_objects)[obj_idx];
    wxDataViewItem item_obj = m_objects_model->GetItemById(obj_idx);
    assert(item_obj.IsOk());

    // Cut connectors
    {
        wxDataViewItem item = m_objects_model->GetInfoItemByType(item_obj, InfoItemType::CutConnectors);
        bool shows = item.IsOk();
        bool should_show = model_object->is_cut() && model_object->has_connectors() && model_object->volumes.size() > 1;

        if (!shows && should_show) {
            m_objects_model->AddInfoChild(item_obj, InfoItemType::CutConnectors);
            Expand(item_obj);
            if (added_object)
                wxGetApp().notification_manager()->push_updated_item_info_notification(InfoItemType::CutConnectors);
        } else if (shows && !should_show) {
            if (!selections) Unselect(item);
            m_objects_model->Delete(item);
            if (selections) {
                if (selections->Index(item) != wxNOT_FOUND) {
                    // If info item was deleted from the list,
                    // it's need to be deleted from selection array, if it was there
                    selections->Remove(item);
                    // Select item_obj, if info_item doesn't exist for item anymore, but was selected
                    if (selections->Index(item_obj) == wxNOT_FOUND) selections->Add(item_obj);
                }
            } else
                Select(item_obj);
        }
    }

    {
        ObjectDataViewModelNode* obj_node = static_cast<ObjectDataViewModelNode*>(item_obj.GetID());
        auto layer_data_count = model_object->layer_height_profile.get().size();
        update_variable_layer_obj_num(obj_node, layer_data_count);
        // If the length of layer_height_profile is greater than 4, variable layer height is applied
        m_objects_model->SetObjectVariableHeightState(layer_data_count > 4 ? hiVariable : hiUnVariable, item_obj);
    }

    {
        bool shows = m_objects_model->IsSupportPainted(item_obj);
        bool should_show = printer_technology() == ptFFF
            && std::any_of(model_object->volumes.begin(), model_object->volumes.end(),
                [](const ModelVolume* mv) {
                    return !mv->supported_facets.empty();
                });
        if (shows && !should_show) {
            m_objects_model->SetSupportPaintState(false, item_obj);
        }
        else if (!shows && should_show) {
            m_objects_model->SetSupportPaintState(true, item_obj);
        }
        if (color_mode_changed && shows) {
            m_objects_model->SetSupportPaintState(true, item_obj,true);
        }
    }

    {
        bool shows = m_objects_model->IsColorPainted(item_obj);
        bool should_show = printer_technology() == ptFFF
            && std::any_of(model_object->volumes.begin(), model_object->volumes.end(),
                [](const ModelVolume* mv) {
                    return !mv->mmu_segmentation_facets.empty();
                });
        if (shows && !should_show) {
            m_objects_model->SetColorPaintState(false, item_obj);
        }
        else if (!shows && should_show) {
            m_objects_model->SetColorPaintState(true, item_obj);
        }
        if (color_mode_changed && shows) {
            m_objects_model->SetColorPaintState(true, item_obj, true);
        }
    }

    {
        bool shows = m_objects_model->IsSinked(item_obj);
        bool should_show = printer_technology() == ptFFF
            && wxGetApp().plater()->canvas3D()->is_object_sinking(obj_idx);
        if (shows && !should_show) {
            m_objects_model->SetSinkState(false, item_obj);
        }
        else if (!shows && should_show) {
            m_objects_model->SetSinkState(true, item_obj);
        }
        if (color_mode_changed && shows) {
            m_objects_model->SetSinkState(true, item_obj, true);
        }
    }

    {
        bool shows = this->GetColumn(colSupportPaint)->IsShown();
        bool should_show = false;
        for (ModelObject* mo : *m_objects) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->supported_facets.empty()) {
                    should_show = true;
                    break;
                }
            }
            if (should_show)
                break;
        }

        if (shows && !should_show) {
            this->set_support_paint_hidden(true);
        }
        else if (!shows && should_show) {
            this->set_support_paint_hidden(false);
        }
    }

    {
        bool shows = this->GetColumn(colColorPaint)->IsShown();
        bool should_show = false;
        for (ModelObject* mo : *m_objects) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->mmu_segmentation_facets.empty()) {
                    should_show = true;
                    break;
                }
            }
            if (should_show)
                break;
        }

        if (shows && !should_show) {
            this->set_color_paint_hidden(true);
        }
        else if (!shows && should_show) {
            this->set_color_paint_hidden(false);
        }
    }

    {
        bool shows = this->GetColumn(colSinking)->IsShown();
        bool should_show = false;
        for (int i = 0; i < m_objects->size(); ++i) {
            if (wxGetApp().plater()->canvas3D()->is_object_sinking(i)) {
                should_show = true;
                break;
            }
            if (should_show)
                break;
        }

        if (shows && !should_show) {
            this->set_sinking_hidden(true);
        }
        else if (!shows && should_show) {
            this->set_sinking_hidden(false);
        }
    }
}

void ObjectList::add_objects_to_list(std::vector<size_t> obj_idxs, bool call_selection_changed, bool notify_partplate, bool do_info_update)
{
#ifdef __WXOSX__
    AssociateModel(nullptr);
#endif
    for (const size_t idx : obj_idxs) {
        add_object_to_list(idx, call_selection_changed, notify_partplate, do_info_update);
    }
#ifdef __WXOSX__
    AssociateModel(m_objects_model);
#endif
}


void ObjectList::add_object_to_list(size_t obj_idx, bool call_selection_changed, bool notify_partplate, bool do_info_update)
{
    auto model_object = (*m_objects)[obj_idx];
    //BBS start add obj_idx for debug
    PartPlateList& list = wxGetApp().plater()->get_partplate_list();
    if (notify_partplate) {
        list.notify_instance_update(obj_idx, 0, true);
    }
    //int plate_idx = list.find_instance_belongs(obj_idx, 0);
    //std::string item_name_str = (boost::format("[P%1%][O%2%]%3%") % plate_idx % std::to_string(obj_idx) % model_object->name).str();
    //std::string item_name_str = (boost::format("[P%1%]%2%") % plate_idx  % model_object->name).str();
    //const wxString& item_name = from_u8(item_name_str);
    const wxString& item_name = from_u8(model_object->name);
    std::string warning_bitmap = get_warning_icon_name(model_object->mesh().stats());
    const auto item = m_objects_model->AddObject(model_object, warning_bitmap, model_object->is_cut());
    Expand(m_objects_model->GetParent(item));

    if (!do_info_update)
        return;

    update_info_items(obj_idx, nullptr, call_selection_changed);

    add_volumes_to_object_in_list(obj_idx);

    // add instances to the object, if it has those
    if (model_object->instances.size()>1)
    {
        std::vector<bool> print_idicator(model_object->instances.size());
        std::vector<int> plate_idicator(model_object->instances.size());
        for (size_t i = 0; i < model_object->instances.size(); ++i) {
            print_idicator[i] = model_object->instances[i]->printable;
            plate_idicator[i] = list.find_instance_belongs(obj_idx, i);
        }

        const wxDataViewItem object_item = m_objects_model->GetItemById(obj_idx);
        m_objects_model->AddInstanceChild(object_item, print_idicator, plate_idicator);
        Expand(m_objects_model->GetInstanceRootItem(object_item));
    }
    else
        m_objects_model->SetPrintableState(model_object->instances[0]->printable ? piPrintable : piUnprintable, obj_idx);

    m_objects_model->SetObjectVariableHeightState(model_object->layer_height_profile.get().size() > 4 ? hiVariable : hiUnVariable, m_objects_model->GetItemById(obj_idx));

    // add settings to the object, if it has those
    add_settings_item(item, &model_object->config.get());

    // Add layers if it has
    add_layer_root_item(item);

#ifndef __WXOSX__
    if (call_selection_changed) {
        UnselectAll();
        Select(item);
        selection_changed();
    }
#endif //__WXMSW__
}

static bool can_add_volumes_to_object(const ModelObject *object)
{
    bool can = object->volumes.size() > 1;

    if (can && object->is_cut()) {
        int no_connectors_cnt = 0;
        for (const ModelVolume *v : object->volumes)
            if (!v->is_cut_connector()) {
                if (!v->is_model_part())
                    return true;
                no_connectors_cnt++;
            }
        can = no_connectors_cnt > 1;
    }

    return can;
}

wxDataViewItemArray ObjectList::add_volumes_to_object_in_list(size_t obj_idx, std::function<bool(const ModelVolume *)> add_to_selection /* = nullptr*/)
{
    const bool is_prevent_list_events = m_prevent_list_events;
    m_prevent_list_events             = true;

    wxDataViewItem object_item = m_objects_model->GetItemById(int(obj_idx));
    m_objects_model->DeleteVolumeChildren(object_item);

    wxDataViewItemArray items;

    const ModelObject *object = (*m_objects)[obj_idx];
    // add volumes to the object
    if (can_add_volumes_to_object(object)) {
        if (object->volumes.size() > 1) {
            wxString obj_item_name = from_u8(object->name);
            if (m_objects_model->GetName(object_item) != obj_item_name)
                m_objects_model->SetName(obj_item_name, object_item);
        }

        int volume_idx{-1};
        auto& ui_and_3d_volume_map = m_objects_model->get_ui_and_3d_volume_map();
        for (auto item : ui_and_3d_volume_map) {
            if (item.first == obj_idx) {
                item.second.clear();
            }
        }
        int ui_volume_idx = 0;
        for (const ModelVolume *volume : object->volumes) {
            ++volume_idx;
            if (object->is_cut() && volume->is_cut_connector())
                continue;

            const wxDataViewItem &vol_item = m_objects_model->AddVolumeChild(
                object_item,
                from_u8(volume->name),
                volume->type(),
                volume->is_text(),
                volume->is_svg(),
                get_warning_icon_name(volume->mesh().stats()),
                volume->config.has("extruder") ? volume->config.extruder() : 0,
                false);
            ui_and_3d_volume_map[obj_idx][ui_volume_idx] = volume_idx;
            ui_volume_idx++;
            add_settings_item(vol_item, &volume->config.get());

            if (add_to_selection && add_to_selection(volume))
                items.Add(vol_item);
        }
        Expand(object_item);
    }

    m_prevent_list_events = is_prevent_list_events;
    return items;
}

void ObjectList::delete_object_from_list()
{
    auto item = GetSelection();
    if (!item)
        return;
    if (m_objects_model->GetParent(item) == wxDataViewItem(nullptr))
        select_item([this, item]() { return m_objects_model->Delete(item); });
    else
        select_item([this, item]() { return m_objects_model->Delete(m_objects_model->GetParent(item)); });
}

void ObjectList::delete_object_from_list(const size_t obj_idx)
{
    select_item([this, obj_idx]() { return m_objects_model->Delete(m_objects_model->GetItemById(obj_idx)); });
}

void ObjectList::delete_volume_from_list(const size_t obj_idx, const size_t vol_idx)
{
    select_item([this, obj_idx, vol_idx]() { return m_objects_model->Delete(m_objects_model->GetItemByVolumeId(obj_idx, vol_idx)); });
}

void ObjectList::delete_instance_from_list(const size_t obj_idx, const size_t inst_idx)
{
    select_item([this, obj_idx, inst_idx]() { return m_objects_model->Delete(m_objects_model->GetItemByInstanceId(obj_idx, inst_idx)); });
}

void ObjectList::delete_from_model_and_list(const ItemType type, const int obj_idx, const int sub_obj_idx)
{
    if (!(type&(itObject|itVolume|itInstance)))
        return;

    take_snapshot("Delete selected");

    if (type&itObject) {
        bool was_cut = object(obj_idx)->is_cut();
        // For variable layer height, the size of layer data is larger than 4
        bool vari_layer_height = (object(obj_idx)->layer_height_profile.get().size() > 4);
        if (del_object(obj_idx)) {
            delete_object_from_list(obj_idx);
            if (was_cut)
                update_lock_icons_for_model();
            if (vari_layer_height) {
                m_variable_layer_obj_num--;
                set_variable_height_column_hidden(m_variable_layer_obj_num == 0);
            }
        }
    }
    else {
        del_subobject_from_object(obj_idx, sub_obj_idx, type);

        type == itVolume ? delete_volume_from_list(obj_idx, sub_obj_idx) :
            delete_instance_from_list(obj_idx, sub_obj_idx);
    }
}

void ObjectList::delete_from_model_and_list(const std::vector<ItemForDelete>& items_for_delete)
{
    if (items_for_delete.empty())
        return;

    m_prevent_list_events = true;
    //BBS
    bool need_update = false;

    std::set<size_t> modified_objects_ids;
    for (std::vector<ItemForDelete>::const_reverse_iterator item = items_for_delete.rbegin(); item != items_for_delete.rend(); ++item) {
        if (!(item->type&(itObject | itVolume | itInstance)))
            continue;
        if (item->type&itObject) {
            // refresh after del_object
            need_update = true;
            bool refresh_immediately = false;
            bool was_cut = object(item->obj_idx)->is_cut();
            // For variable layer height, the size of layer data is larger than 4
            bool vari_layer_height = (object(item->obj_idx)->layer_height_profile.get().size() > 4);
            if (!del_object(item->obj_idx, refresh_immediately))
                return;
            m_objects_model->Delete(m_objects_model->GetItemById(item->obj_idx));
            if (was_cut)
                update_lock_icons_for_model();
            if (vari_layer_height) {
                m_variable_layer_obj_num--;
                set_variable_height_column_hidden(m_variable_layer_obj_num == 0);
            }
        }
        else {
            if (!del_subobject_from_object(item->obj_idx, item->sub_obj_idx, item->type))
                return; //continue;
            if (item->type&itVolume) {
                m_objects_model->Delete(m_objects_model->GetItemByVolumeId(item->obj_idx, item->sub_obj_idx));
                // BBS
#if 0
                ModelObject* obj = object(item->obj_idx);
                if (obj->volumes.size() == 1) {
                    wxDataViewItem parent = m_objects_model->GetItemById(item->obj_idx);
                    if (obj->config.has("extruder")) {
                        const wxString extruder = wxString::Format("%d", obj->config.extruder());
                        m_objects_model->SetExtruder(extruder, parent);
                    }
                    // If last volume item with warning was deleted, unmark object item
                    m_objects_model->UpdateWarningIcon(parent, get_warning_icon_name(obj->get_object_stl_stats()));
                }
#endif
                wxGetApp().plater()->canvas3D()->ensure_on_bed(item->obj_idx, printer_technology() != ptSLA);
            }
            else
                m_objects_model->Delete(m_objects_model->GetItemByInstanceId(item->obj_idx, item->sub_obj_idx));
        }

        modified_objects_ids.insert(static_cast<size_t>(item->obj_idx));
    }

    if (need_update) {
        wxGetApp().plater()->update();
        wxGetApp().plater()->object_list_changed();
    }

    for (size_t id : modified_objects_ids) {
        update_info_items(id);
    }

    m_prevent_list_events = true;
    part_selection_changed();
}

void ObjectList::update_lock_icons_for_model()
{
    // update the icon for cut object
    for (size_t obj_idx = 0; obj_idx < (*m_objects).size(); ++obj_idx)
        if (!(*m_objects)[obj_idx]->is_cut())
            m_objects_model->UpdateCutObjectIcon(m_objects_model->GetItemById(int(obj_idx)), false);
}

void ObjectList::delete_all_objects_from_list()
{
    m_prevent_list_events = true;
    reload_all_plates();
    m_prevent_list_events = false;
    part_selection_changed();
}

void ObjectList::increase_object_instances(const size_t obj_idx, const size_t num)
{
    select_item([this, obj_idx, num]() { return m_objects_model->AddInstanceChild(m_objects_model->GetItemById(obj_idx), num); });
    selection_changed();
}

void ObjectList::decrease_object_instances(const size_t obj_idx, const size_t num)
{
    select_item([this, obj_idx, num]() { return m_objects_model->DeleteLastInstance(m_objects_model->GetItemById(obj_idx), num); });
}

void ObjectList::unselect_objects()
{
    if (!GetSelection())
        return;

    m_prevent_list_events = true;
    UnselectAll();
    part_selection_changed();
    m_prevent_list_events = false;
}

void ObjectList::select_object_item(bool is_msr_gizmo)
{
    if (wxDataViewItem item = GetSelection()) {
        ItemType type = m_objects_model->GetItemType(item);
        bool is_volume_item = type == itVolume || (type == itSettings && m_objects_model->GetItemType(m_objects_model->GetParent(item)) == itVolume);
        if ((is_msr_gizmo && is_volume_item) || type == itObject)
            return;

        if (wxDataViewItem obj_item = m_objects_model->GetTopParent(item)) {
            m_prevent_list_events = true;
            UnselectAll();
            Select(obj_item);
            part_selection_changed();
            m_prevent_list_events = false;
        }
    }
}

static void update_selection(wxDataViewItemArray& sels, ObjectList::SELECTION_MODE mode, ObjectDataViewModel* model)
{
    if (mode == ObjectList::smInstance)
    {
        for (auto& item : sels)
        {
            ItemType type = model->GetItemType(item);
            if (type == itObject)
                continue;
            if (type == itInstanceRoot) {
                wxDataViewItem obj_item = model->GetParent(item);
                sels.Remove(item);
                sels.Add(obj_item);
                update_selection(sels, mode, model);
                return;
            }
            if (type == itInstance)
            {
                wxDataViewItemArray instances;
                model->GetChildren(model->GetParent(item), instances);
                assert(instances.Count() > 0);
                size_t selected_instances_cnt = 0;
                for (auto& inst : instances) {
                    if (sels.Index(inst) == wxNOT_FOUND)
                        break;
                    selected_instances_cnt++;
                }

                if (selected_instances_cnt == instances.Count())
                {
                    wxDataViewItem obj_item = model->GetObject(item);
                    for (auto& inst : instances)
                        sels.Remove(inst);
                    sels.Add(obj_item);
                    update_selection(sels, mode, model);
                    return;
                }
            }
            else
                return;
        }
    }
}

void ObjectList::remove()
{
    if (GetSelectedItemsCount() == 0)
        return;

    auto delete_item = [this](wxDataViewItem item)
    {
        wxDataViewItem parent = m_objects_model->GetParent(item);
        ItemType type = m_objects_model->GetItemType(item);
        if (type & itObject)
            delete_from_model_and_list(itObject, m_objects_model->GetIdByItem(item), -1);
        else {
            if (type & (itLayer | itInstance)) {
                // In case there is just one layer or two instances and we delete it, del_subobject_item will
                // also remove the parent item. Selection should therefore pass to the top parent (object).
                wxDataViewItemArray children;
                if (m_objects_model->GetChildren(parent, children) == (type & itLayer ? 1 : 2))
                    parent = m_objects_model->GetObject(item);
            }

            del_subobject_item(item);
        }

        return parent;
    };

    wxDataViewItemArray sels;
    GetSelections(sels);

    wxDataViewItem parent = wxDataViewItem(nullptr);

    if (sels.Count() == 1)
        parent = delete_item(GetSelection());
    else
    {
        SELECTION_MODE sels_mode = m_selection_mode;
        UnselectAll();
        update_selection(sels, sels_mode, m_objects_model);

        Plater::TakeSnapshot snapshot = Plater::TakeSnapshot(wxGetApp().plater(), "Delete selected");

        for (auto& item : sels)
        {
            if (m_objects_model->InvalidItem(item)) // item can be deleted for this moment (like last 2 Instances or Volumes)
                continue;
            parent = delete_item(item);
        }
    }

    if (parent && !m_objects_model->InvalidItem(parent)) {
        select_item(parent);
        update_selections_on_canvas();
    }
}

void ObjectList::del_layer_range(const t_layer_height_range& range)
{
    const int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0) return;

    t_layer_config_ranges& ranges = object(obj_idx)->layer_config_ranges;

    wxDataViewItem selectable_item = GetSelection();

    if (ranges.size() == 1)
        selectable_item = m_objects_model->GetParent(selectable_item);

    wxDataViewItem layer_item = m_objects_model->GetItemByLayerRange(obj_idx, range);
    del_subobject_item(layer_item);

    select_item(selectable_item);
}

static double get_min_layer_height(const int extruder_idx)
{
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    return config.opt_float("min_layer_height", std::max(0, extruder_idx - 1));
}

static double get_max_layer_height(const int extruder_idx)
{
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    int extruder_idx_zero_based = std::max(0, extruder_idx - 1);
    double max_layer_height = config.opt_float("max_layer_height", extruder_idx_zero_based);

    // In case max_layer_height is set to zero, it should default to 75 % of nozzle diameter:
    if (max_layer_height < EPSILON)
        max_layer_height = 0.75 * config.opt_float("nozzle_diameter", extruder_idx_zero_based);

    return max_layer_height;
}

// When editing this function, please synchronize the conditions with can_add_new_range_after_current().
void ObjectList::add_layer_range_after_current(const t_layer_height_range current_range)
{
    const int obj_idx = get_selected_obj_idx();
    assert(obj_idx >= 0);
    if (obj_idx < 0)
        // This should not happen.
        return;

    const wxDataViewItem layers_item = GetSelection();

    auto& ranges = object(obj_idx)->layer_config_ranges;
    auto it_range = ranges.find(current_range);
    assert(it_range != ranges.end());
    if (it_range == ranges.end())
        // This shoudl not happen.
        return;

    auto it_next_range = it_range;
    bool changed = false;
    if (++ it_next_range == ranges.end())
    {
        // Adding a new layer height range after the last one.
        // BBS: remove snapshot name "Add Height Range"
        take_snapshot("");
        changed = true;

        const t_layer_height_range new_range = { current_range.second, current_range.second + 2. };
        ranges[new_range].assign_config(get_default_layer_config(obj_idx));
        add_layer_item(new_range, layers_item);
    }
    else if (const std::pair<coordf_t, coordf_t> &next_range = it_next_range->first; current_range.second <= next_range.first)
    {
        const int layer_idx = m_objects_model->GetItemIdByLayerRange(obj_idx, next_range);
        assert(layer_idx >= 0);
        if (layer_idx >= 0)
        {
            if (current_range.second == next_range.first)
            {
                // Splitting the next layer height range to two.
                const auto old_config = ranges.at(next_range);
                const coordf_t delta = next_range.second - next_range.first;
                // Layer height of the current layer.
                const coordf_t old_min_layer_height = get_min_layer_height(old_config.opt_int("extruder"));
                // Layer height of the layer to be inserted.
                const coordf_t new_min_layer_height = get_min_layer_height(0);
                if (delta >= old_min_layer_height + new_min_layer_height - EPSILON) {
                    const coordf_t middle_layer_z = (new_min_layer_height > 0.5 * delta) ?
	                    next_range.second - new_min_layer_height :
                    	next_range.first + std::max(old_min_layer_height, 0.5 * delta);
                    t_layer_height_range new_range = { middle_layer_z, next_range.second };

                    // BBS: remove snapshot name "Add Height Range"
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "");
                    changed = true;

                    // create new 2 layers instead of deleted one
                    // delete old layer

                    wxDataViewItem layer_item = m_objects_model->GetItemByLayerRange(obj_idx, next_range);
                    del_subobject_item(layer_item);

                    ranges[new_range] = old_config;
                    add_layer_item(new_range, layers_item, layer_idx);

                    new_range = { current_range.second, middle_layer_z };
                    ranges[new_range].assign_config(get_default_layer_config(obj_idx));
                    add_layer_item(new_range, layers_item, layer_idx);
                }
            }
            else if (next_range.first - current_range.second >= get_min_layer_height(0) - EPSILON)
            {
                // Filling in a gap between the current and a new layer height range with a new one.
                // BBS: remove snapshot name "Add Height Range"
                take_snapshot("");
                changed = true;

                const t_layer_height_range new_range = { current_range.second, next_range.first };
                ranges[new_range].assign_config(get_default_layer_config(obj_idx));
                add_layer_item(new_range, layers_item, layer_idx);
            }
        }
    }

    if (changed)
        changed_object(obj_idx);

    // The layer range panel is updated even if this function does not change the layer ranges, as the panel update
    // may have been postponed from the "kill focus" event of a text field, if the focus was lost for the "add layer" button.
    // select item to update layers sizer
    select_item(layers_item);
}

// Returning an empty string means that the layer could be added after the current layer.
// Otherwise an error tooltip is returned.
// When editing this function, please synchronize the conditions with add_layer_range_after_current().
wxString ObjectList::can_add_new_range_after_current(const t_layer_height_range current_range)
{
    const int obj_idx = get_selected_obj_idx();
    assert(obj_idx >= 0);
    if (obj_idx < 0)
        // This should not happen.
        return "ObjectList assert";

    auto& ranges = object(obj_idx)->layer_config_ranges;
    auto it_range = ranges.find(current_range);
    assert(it_range != ranges.end());
    if (it_range == ranges.end())
        // This shoudl not happen.
        return "ObjectList assert";

    auto it_next_range = it_range;
    if (++ it_next_range == ranges.end())
    	// Adding a layer after the last layer is always possible.
        return "";

    // BBS: remove all layer range message

	// All right, new layer height range could be inserted.
	return "";
}

void ObjectList::add_layer_item(const t_layer_height_range& range,
                                const wxDataViewItem layers_item,
                                const int layer_idx /* = -1*/)
{
    const int obj_idx = m_objects_model->GetObjectIdByItem(layers_item);
    if (obj_idx < 0) return;

    const DynamicPrintConfig& config = object(obj_idx)->layer_config_ranges[range].get();
    if (!config.has("extruder"))
        return;

    const auto layer_item = m_objects_model->AddLayersChild(layers_item,
                                                            range,
                                                            config.opt_int("extruder"),
                                                            layer_idx);
    add_settings_item(layer_item, &config);
}

bool ObjectList::edit_layer_range(const t_layer_height_range& range, coordf_t layer_height)
{
    // Use m_selected_object_id instead of get_selected_obj_idx()
    // because of get_selected_obj_idx() return obj_idx for currently selected item.
    // But edit_layer_range(...) function can be called, when Selection in ObjectList could be changed
    const int obj_idx = m_selected_object_id ;
    if (obj_idx < 0)
        return false;

    ModelConfig* config = &object(obj_idx)->layer_config_ranges[range];
    if (fabs(layer_height - config->opt_float("layer_height")) < EPSILON)
        return false;

    const int extruder_idx = config->opt_int("extruder");

    if (layer_height >= get_min_layer_height(extruder_idx) &&
        layer_height <= get_max_layer_height(extruder_idx))
    {
        config->set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        changed_object(obj_idx);
        return true;
    }

    return false;
}

bool ObjectList::edit_layer_range(const t_layer_height_range& range, const t_layer_height_range& new_range, bool dont_update_ui)
{
    // Use m_selected_object_id instead of get_selected_obj_idx()
    // because of get_selected_obj_idx() return obj_idx for currently selected item.
    // But edit_layer_range(...) function can be called, when Selection in ObjectList could be changed
    const int obj_idx = m_selected_object_id;
    if (obj_idx < 0) return false;

    // BBS: remove snapeshot name "Edit Height Range"
    take_snapshot("");

    const ItemType sel_type = m_objects_model->GetItemType(GetSelection());

    auto& ranges = object(obj_idx)->layer_config_ranges;

    {
        ModelConfig config = std::move(ranges[range]);
        ranges.erase(range);
        ranges[new_range] = std::move(config);
    }

    changed_object(obj_idx);

    wxDataViewItem root_item = m_objects_model->GetLayerRootItem(m_objects_model->GetItemById(obj_idx));
    // To avoid update selection after deleting of a selected item (under GTK)
    // set m_prevent_list_events to true
    m_prevent_list_events = true;
    m_objects_model->DeleteChildren(root_item);

    if (root_item.IsOk()) {
        // create Layer item(s) according to the layer_config_ranges
        for (const auto& r : ranges)
            add_layer_item(r.first, root_item);
    }

    // if this function was invoked from wxEVT_CHANGE_SELECTION selected item could be other than itLayer or itLayerRoot
    if (!dont_update_ui && (sel_type & (itLayer | itLayerRoot)))
        select_item(sel_type&itLayer ? m_objects_model->GetItemByLayerRange(obj_idx, new_range) : root_item);

    Expand(root_item);

    m_prevent_list_events = false;
    return true;
}

void ObjectList::init()
{
    m_objects_model->Init();
    m_objects = &wxGetApp().model().objects;
}

bool ObjectList::multiple_selection() const
{
    return GetSelectedItemsCount() > 1;
}

bool ObjectList::is_selected(const ItemType type) const
{
    const wxDataViewItem& item = GetSelection();
    if (item)
        return m_objects_model->GetItemType(item) == type;

    return false;
}

bool ObjectList::is_connectors_item_selected() const
{
    const wxDataViewItem &item = GetSelection();
    if (item)
        return m_objects_model->GetItemType(item) == itInfo && m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors;

    return false;
}

bool ObjectList::is_connectors_item_selected(const wxDataViewItemArray &sels) const
{
    for (auto item : sels)
        if (m_objects_model->GetItemType(item) == itInfo && m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors)
            return true;

    return false;
}


int ObjectList::get_selected_layers_range_idx() const
{
    const wxDataViewItem& item = GetSelection();
    if (!item)
        return -1;

    const ItemType type = m_objects_model->GetItemType(item);
    if (type & itSettings && m_objects_model->GetItemType(m_objects_model->GetParent(item)) != itLayer)
        return -1;

    return m_objects_model->GetLayerIdByItem(type & itLayer ? item : m_objects_model->GetParent(item));
}

void ObjectList::update_selections()
{
    const Selection& selection = scene_selection();
    wxDataViewItemArray sels;

#if 0
    if (m_selection_mode == smUndef) {
        PartPlate* pp = wxGetApp().plater()->get_partplate_list().get_selected_plate();
        assert(pp != nullptr);
        wxDataViewItem sel_plate = m_objects_model->GetItemByPlateId(pp->get_index());
        sels.Add(sel_plate);
        select_items(sels);

        // Scroll selected Item in the middle of an object list
        ensure_current_item_visible();
        return;
    }
#endif

    if ( ( m_selection_mode & (smSettings|smLayer|smLayerRoot|smVolume) ) == 0)
        m_selection_mode = smInstance;

    // We doesn't update selection if itSettings | itLayerRoot | itLayer Item for the current object/part is selected
    if (GetSelectedItemsCount() == 1 && m_objects_model->GetItemType(GetSelection()) & (itSettings | itLayerRoot | itLayer))
    {
        const auto item = GetSelection();
        if (selection.is_single_full_object()) {
            if (m_objects_model->GetItemType(m_objects_model->GetParent(item)) & itObject &&
                m_objects_model->GetObjectIdByItem(item) == selection.get_object_idx() )
                return;
            sels.Add(m_objects_model->GetItemById(selection.get_object_idx()));
        }
        else if (selection.is_single_volume() || selection.is_any_modifier()) {
            const auto gl_vol = selection.get_first_volume();
            if (m_objects_model->GetVolumeIdByItem(m_objects_model->GetParent(item)) == gl_vol->volume_idx())
                return;
        }
        // but if there is selected only one of several instances by context menu,
        // then select this instance in ObjectList
        else if (selection.is_single_full_instance())
            sels.Add(m_objects_model->GetItemByInstanceId(selection.get_object_idx(), selection.get_instance_idx()));
        // Can be the case, when we have selected itSettings | itLayerRoot | itLayer in the ObjectList and selected object/instance in the Scene
        // and then select some object/instance in 3DScene using Ctrt+left click
        else {
            // Unselect all items in ObjectList
            m_last_selected_item = wxDataViewItem(nullptr);
            m_prevent_list_events = true;
            UnselectAll();
            m_prevent_list_events = false;
            // call this function again to update selection from the canvas
            update_selections();
            return;
        }
    }
    else if (selection.is_single_full_object() || selection.is_multiple_full_object())
    {
        const Selection::ObjectIdxsToInstanceIdxsMap& objects_content = selection.get_content();
        // it's impossible to select Settings, Layer or LayerRoot for several objects
        if (!selection.is_multiple_full_object() && (m_selection_mode & (smSettings | smLayer | smLayerRoot)))
        {
            auto obj_idx = objects_content.begin()->first;
            wxDataViewItem obj_item = m_objects_model->GetItemById(obj_idx);
            if (m_selection_mode & smSettings)
            {
                if (m_selected_layers_range_idx < 0)
                    sels.Add(m_objects_model->GetSettingsItem(obj_item));
                else
                    sels.Add(m_objects_model->GetSettingsItem(m_objects_model->GetItemByLayerId(obj_idx, m_selected_layers_range_idx)));
            }
            else if (m_selection_mode & smLayerRoot)
                sels.Add(m_objects_model->GetLayerRootItem(obj_item));
            else if (m_selection_mode & smLayer) {
                if (m_selected_layers_range_idx >= 0)
                    sels.Add(m_objects_model->GetItemByLayerId(obj_idx, m_selected_layers_range_idx));
                else
                    sels.Add(obj_item);
            }
        }
        else {
        for (const auto& object : objects_content) {
            if (object.second.size() == 1)          // object with 1 instance
                sels.Add(m_objects_model->GetItemById(object.first));
            else if (object.second.size() > 1)      // object with several instances
            {
                wxDataViewItemArray current_sels;
                GetSelections(current_sels);
                const wxDataViewItem frst_inst_item = m_objects_model->GetItemByInstanceId(object.first, 0);

                bool root_is_selected = false;
                for (const auto& item:current_sels)
                    if (item == m_objects_model->GetParent(frst_inst_item) ||
                        item == m_objects_model->GetObject(frst_inst_item)) {
                        root_is_selected = true;
                        sels.Add(item);
                        break;
                    }
                if (root_is_selected)
                    continue;

                const Selection::InstanceIdxsList& instances = object.second;
                for (const auto& inst : instances)
                    sels.Add(m_objects_model->GetItemByInstanceId(object.first, inst));
            }
        } }
    }
    else if (selection.is_any_volume() || selection.is_any_modifier())
    {
        if (m_selection_mode & smSettings)
        {
            const auto gl_vol = selection.get_first_volume();
            if (gl_vol->volume_idx() >= 0) {
                // Only add GLVolumes with non-negative volume_ids. GLVolumes with negative volume ids
                // are not associated with ModelVolumes, but they are temporarily generated by the backend
                // (for example, SLA supports or SLA pad).
                wxDataViewItem vol_item = m_objects_model->GetItemByVolumeId(gl_vol->object_idx(), gl_vol->volume_idx());
                sels.Add(m_objects_model->GetSettingsItem(vol_item));
            }
        }
        else {
        for (auto idx : selection.get_volume_idxs()) {
            const auto gl_vol = selection.get_volume(idx);
            if (gl_vol->volume_idx() >= 0) {
                // Only add GLVolumes with non-negative volume_ids. GLVolumes with negative volume ids
                // are not associated with ModelVolumes, but they are temporarily generated by the backend
                // (for example, SLA supports or SLA pad).
                int obj_idx = gl_vol->object_idx();
                int vol_idx = gl_vol->volume_idx();
                assert(obj_idx >= 0 && vol_idx >= 0);
                if (object(obj_idx)->volumes[vol_idx]->is_cut_connector())
                    sels.Add(m_objects_model->GetInfoItemByType(m_objects_model->GetItemById(obj_idx), InfoItemType::CutConnectors));
                else {
                    vol_idx = m_objects_model->get_real_volume_index_in_ui(obj_idx,vol_idx);
                    sels.Add(m_objects_model->GetItemByVolumeId(obj_idx, vol_idx));
                }
            }
        }
        m_selection_mode = smVolume; }
    }
    else if (selection.is_single_full_instance() || selection.is_multiple_full_instance())
    {
        for (auto idx : selection.get_instance_idxs()) {
            sels.Add(m_objects_model->GetItemByInstanceId(selection.get_object_idx(), idx));
        }
    }
    else if (selection.is_mixed())
    {
        const Selection::ObjectIdxsToInstanceIdxsMap& objects_content_list = selection.get_content();

        for (auto idx : selection.get_volume_idxs()) {
            const auto gl_vol = selection.get_volume(idx);
            const auto& glv_obj_idx = gl_vol->object_idx();
            const auto& glv_ins_idx = gl_vol->instance_idx();

            bool is_selected = false;

            for (auto obj_ins : objects_content_list) {
                if (obj_ins.first == glv_obj_idx) {
                    if (obj_ins.second.find(glv_ins_idx) != obj_ins.second.end() &&
                        !selection.is_from_single_instance() ) // a case when volumes of different types are selected
                    {
                        if (glv_ins_idx == 0 && (*m_objects)[glv_obj_idx]->instances.size() == 1)
                            sels.Add(m_objects_model->GetItemById(glv_obj_idx));
                        else
                            sels.Add(m_objects_model->GetItemByInstanceId(glv_obj_idx, glv_ins_idx));

                        is_selected = true;
                        break;
                    }
                }
            }

            if (is_selected)
                continue;

            const auto& glv_vol_idx = gl_vol->volume_idx();
            if (glv_vol_idx == 0 && (*m_objects)[glv_obj_idx]->volumes.size() == 1)
                sels.Add(m_objects_model->GetItemById(glv_obj_idx));
            else
                sels.Add(m_objects_model->GetItemByVolumeId(glv_obj_idx, glv_vol_idx));
        }
    }

    if (sels.size() == 0 || m_selection_mode & smSettings)
        m_selection_mode = smUndef;

    if (fix_cut_selection(sels) || is_connectors_item_selected(sels)) {
        m_prevent_list_events = true;

        // If some part is selected, unselect all items except of selected parts of the current object
        UnselectAll();
        SetSelections(sels);

        m_prevent_list_events = false;

        // update object selection on Plater
        if (!m_prevent_canvas_selection_update)
            update_selections_on_canvas();

        // to update the toolbar and info sizer
        if (!GetSelection() || m_objects_model->GetItemType(GetSelection()) == itObject || is_connectors_item_selected()) {
            auto event = SimpleEvent(EVT_OBJ_LIST_OBJECT_SELECT);
            event.SetEventObject(this);
            wxPostEvent(this, event);
        }
        part_selection_changed();
    }
    else {
        select_items(sels);

        // Scroll selected Item in the middle of an object list
        ensure_current_item_visible();
    }
}

void ObjectList::update_selections_on_canvas()
{
    auto canvas_type = wxGetApp().plater()->get_current_canvas3D()->get_canvas_type();
    GLCanvas3D* canvas = canvas_type == GLCanvas3D::ECanvasType::CanvasAssembleView ? wxGetApp().plater()->get_current_canvas3D() : wxGetApp().plater()->get_view3D_canvas3D();
    Selection& selection = canvas->get_selection();

    const int sel_cnt = GetSelectedItemsCount();
    if (sel_cnt == 0) {
        selection.remove_all();
        if (canvas_type != GLCanvas3D::ECanvasType::CanvasPreview)
            wxGetApp().plater()->get_current_canvas3D()->update_gizmos_on_off_state();
        return;
    }

    std::vector<unsigned int> volume_idxs;
    Selection::EMode mode = Selection::Volume;
    bool single_selection = sel_cnt == 1;
    auto add_to_selection = [this, &volume_idxs, &single_selection](const wxDataViewItem& item, const Selection& selection, int instance_idx, Selection::EMode& mode)
    {
        const ItemType& type = m_objects_model->GetItemType(item);
        const int obj_idx = m_objects_model->GetObjectIdByItem(item);

        if (type == itVolume) {
            int vol_idx = m_objects_model->GetVolumeIdByItem(item);
            vol_idx                        = m_objects_model->get_real_volume_index_in_3d(obj_idx,vol_idx);
            std::vector<unsigned int> idxs = selection.get_volume_idxs_from_volume(obj_idx, std::max(instance_idx, 0), vol_idx);
            volume_idxs.insert(volume_idxs.end(), idxs.begin(), idxs.end());
        }
        else if (type == itInstance) {
            const int inst_idx = m_objects_model->GetInstanceIdByItem(item);
            mode = Selection::Instance;
            std::vector<unsigned int> idxs = selection.get_volume_idxs_from_instance(obj_idx, inst_idx);
            volume_idxs.insert(volume_idxs.end(), idxs.begin(), idxs.end());
        }
        else if (type == itInfo) {
            if (m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors) {
                mode = Selection::Volume;

                // When selecting CutConnectors info item, select all object volumes, which are marked as a connector
                const ModelObject *obj = object(obj_idx);
                for (unsigned int vol_idx = 0; vol_idx < obj->volumes.size(); vol_idx++)
                    if (obj->volumes[vol_idx]->is_cut_connector()) {
                        std::vector<unsigned int> idxs = selection.get_volume_idxs_from_volume(obj_idx, std::max(instance_idx, 0), vol_idx);
                        volume_idxs.insert(volume_idxs.end(), idxs.begin(), idxs.end());
                    }
            } else {
                // When selecting an info item, select one instance of the
                // respective object - a gizmo may want to be opened.
                int inst_idx      = selection.get_instance_idx();
                int scene_obj_idx = selection.get_object_idx();
                mode              = Selection::Instance;
                // select first instance, unless an instance of the object is already selected
                if (scene_obj_idx == -1 || inst_idx == -1 || scene_obj_idx != obj_idx) inst_idx = 0;
                std::vector<unsigned int> idxs = selection.get_volume_idxs_from_instance(obj_idx, inst_idx);
                volume_idxs.insert(volume_idxs.end(), idxs.begin(), idxs.end());
            }
        }
        else
        {
            mode = Selection::Instance;
            single_selection &= (obj_idx != selection.get_object_idx());
            std::vector<unsigned int> idxs = selection.get_volume_idxs_from_object(obj_idx);
            volume_idxs.insert(volume_idxs.end(), idxs.begin(), idxs.end());
        }
    };

    // stores current instance idx before to clear the selection
    int instance_idx = selection.get_instance_idx();

    if (sel_cnt == 1) {
        wxDataViewItem item = GetSelection();
        if (m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors)
            selection.remove_all();

        if (m_objects_model->GetItemType(item) & (itSettings | itInstanceRoot | itLayerRoot | itLayer))
            add_to_selection(m_objects_model->GetParent(item), selection, instance_idx, mode);
        else
            add_to_selection(item, selection, instance_idx, mode);
    }
    else
    {
        wxDataViewItemArray sels;
        GetSelections(sels);

        // clear selection before adding new elements
        selection.clear(); //OR remove_all()?

        for (auto item : sels)
        {
            add_to_selection(item, selection, instance_idx, mode);
        }
    }

    if (selection.contains_all_volumes(volume_idxs))
    {
        // remove
        volume_idxs = selection.get_missing_volume_idxs_from(volume_idxs);
        if (volume_idxs.size() > 0)
        {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Remove selected from list", UndoRedo::SnapshotType::Selection);
            selection.remove_volumes(mode, volume_idxs);
        }
    }
    else
    {
        // add
        // to avoid lost of some volumes in selection
        // check non-selected volumes only if selection mode wasn't changed
        // OR there is no single selection
        if (selection.get_mode() == mode || !single_selection)
            volume_idxs = selection.get_unselected_volume_idxs_from(volume_idxs);
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add selected to list", UndoRedo::SnapshotType::Selection);
        selection.add_volumes(mode, volume_idxs, single_selection);
    }

    if (canvas_type != GLCanvas3D::ECanvasType::CanvasPreview)
        wxGetApp().plater()->get_current_canvas3D()->update_gizmos_on_off_state();
    wxGetApp().plater()->canvas3D()->render();
}

void ObjectList::select_item(const wxDataViewItem& item)
{
    if (! item.IsOk()) { return; }

    m_prevent_list_events = true;

    UnselectAll();
    Select(item);
    part_selection_changed();

    m_prevent_list_events = false;
}

void ObjectList::select_item(std::function<wxDataViewItem()> get_item)
{
    if (!get_item)
        return;

    m_prevent_list_events = true;
    m_prevent_list_manipulation = true;

    wxDataViewItem item = get_item();
    if (item.IsOk()) {
        UnselectAll();
        Select(item);
        part_selection_changed();
    }

    m_prevent_list_manipulation = false;
    m_prevent_list_events = false;
}

// BBS
void ObjectList::select_item(const ObjectVolumeID& ov_id)
{
    std::vector<ObjectVolumeID> ov_ids;
    ov_ids.push_back(ov_id);
    select_items(ov_ids);
}

void ObjectList::select_items(const std::vector<ObjectVolumeID>& ov_ids)
{
    ModelObjectPtrs& objects = wxGetApp().model().objects;

    wxDataViewItemArray sel_items;
    for (auto ov_id : ov_ids) {
        if (ov_id.object == nullptr)
            continue;

        ModelObject* mo = ov_id.object;
        ModelVolume* mv = ov_id.volume;

        wxDataViewItem obj_item = m_objects_model->GetObjectItem(mo);
        if (mv != nullptr) {
            size_t vol_idx;
            for (vol_idx = 0; vol_idx < mo->volumes.size(); vol_idx++) {
                if (mo->volumes[vol_idx] == mv)
                    break;
            }
            assert(vol_idx < mo->volumes.size());

            wxDataViewItem vol_item = m_objects_model->GetVolumeItem(obj_item, vol_idx);
            if (vol_item.GetID() != nullptr) {
                sel_items.push_back(vol_item);
            }
            else {
                sel_items.push_back(obj_item);
            }
        }
        else {
            sel_items.push_back(obj_item);
        }
    }

    select_items(sel_items);
    selection_changed();
}

void ObjectList::select_items(const wxDataViewItemArray& sels)
{
    m_prevent_list_events = true;
    m_prevent_list_manipulation = true;
    m_last_selected_item = sels.empty() ? wxDataViewItem(nullptr) : sels.back();

    UnselectAll();

    if (!sels.empty()) {
        SetSelections(sels);
    }
    else {
        int curr_plate_idx = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
        on_plate_selected(curr_plate_idx);
    }

    part_selection_changed();

    m_prevent_list_manipulation = false;
    m_prevent_list_events = false;
}

void ObjectList::select_all()
{
    SelectAll();
    selection_changed();
}

void ObjectList::select_item_all_children()
{
    if (wxGetApp().plater()  && !wxGetApp().plater()->canvas3D()->get_gizmos_manager().is_allow_select_all()) {
        return;
    }
    wxDataViewItemArray sels;

    // There is no selection before OR some object is selected   =>  select all objects
    if (!GetSelection() || m_objects_model->GetItemType(GetSelection()) == itObject) {
        for (size_t i = 0; i < m_objects->size(); i++)
            sels.Add(m_objects_model->GetItemById(i));
        m_selection_mode = smInstance;
    }
    else {
        const auto item = GetSelection();
        const ItemType item_type = m_objects_model->GetItemType(item);
        // Some volume/layer/instance is selected    =>  select all volumes/layers/instances inside the current object
        if (item_type & (itVolume | itInstance | itLayer))
            m_objects_model->GetChildren(m_objects_model->GetParent(item), sels);

        m_selection_mode = item_type&itVolume ? smVolume :
                           item_type&itLayer  ? smLayer  : smInstance;
    }

    SetSelections(sels);
    selection_changed();
}

// update selection mode for non-multiple selection
void ObjectList::update_selection_mode()
{
    m_selected_layers_range_idx=-1;
    // All items are unselected
    if (!GetSelection())
    {
        m_last_selected_item = wxDataViewItem(nullptr);
        m_selection_mode = smUndef;
        return;
    }

    const ItemType type = m_objects_model->GetItemType(GetSelection());
    m_selection_mode =  type & itSettings ? smUndef  :
                        type & itLayer    ? smLayer  :
                        type & itVolume   ? smVolume : smInstance;
}

// check last selected item. If is it possible to select it
bool ObjectList::check_last_selection(wxString& msg_str)
{
    if (!m_last_selected_item)
        return true;

    const bool is_shift_pressed = wxGetKeyState(WXK_SHIFT);

    /* We can't mix Volumes, Layers and Objects/Instances.
     * So, show information about it
     */
    const ItemType type = m_objects_model->GetItemType(m_last_selected_item);

    // check a case of a selection of the same type items from different Objects
    auto impossible_multi_selection = [type, this](const ItemType item_type, const SELECTION_MODE selection_mode) {
        if (!(type & item_type && m_selection_mode & selection_mode))
            return false;

        wxDataViewItemArray sels;
        GetSelections(sels);
        for (const auto& sel : sels)
            if (sel != m_last_selected_item &&
                m_objects_model->GetObject(sel) != m_objects_model->GetObject(m_last_selected_item))
                return true;

        return false;
    };

    if (impossible_multi_selection(itVolume, smVolume) ||
        impossible_multi_selection(itLayer,  smLayer ) ||
        type & itSettings ||
        (type & itVolume   && !(m_selection_mode & smVolume  )) ||
        (type & itLayer    && !(m_selection_mode & smLayer   )) ||
        (type & itInstance && !(m_selection_mode & smInstance))
        )
    {
        // Inform user why selection isn't completed
        // BBS: change "Object or Instance" to "Object"
        const wxString item_type = m_selection_mode & smInstance ? _(L("Object")) :
                                   m_selection_mode & smVolume   ? _(L("Part")) : _(L("Layer"));

        if (m_selection_mode == smInstance) {
            msg_str = wxString::Format(_(L("Selection conflicts")) + "\n\n" +
                _(L("If the first selected item is an object, the second should also be an object.")) + "\n");
        }
        else {
            msg_str = wxString::Format(_(L("Selection conflicts")) + "\n\n" +
                _(L("If the first selected item is a part, the second should be a part in the same object.")) + "\n");
        }

        // Unselect last selected item, if selection is without SHIFT
        if (!is_shift_pressed) {
            Unselect(m_last_selected_item);
            show_info(this, msg_str, _(L("Info")));
        }

        return is_shift_pressed;
    }

    return true;
}

void ObjectList::fix_multiselection_conflicts()
{
    if (GetSelectedItemsCount() <= 1) {
        update_selection_mode();
        return;
    }

    wxString msg_string;
    if (!check_last_selection(msg_string))
        return;

    m_prevent_list_events = true;

    wxDataViewItemArray sels;
    GetSelections(sels);

    if (m_selection_mode & (smVolume|smLayer))
    {
        // identify correct parent of the initial selected item
        const wxDataViewItem& parent = m_objects_model->GetParent(m_last_selected_item == sels.front() ? sels.back() : sels.front());

        sels.clear();
        wxDataViewItemArray children; // selected volumes from current parent
        m_objects_model->GetChildren(parent, children);

        const ItemType item_type = m_selection_mode & smVolume ? itVolume : itLayer;

        for (const auto& child : children)
            if (IsSelected(child) && m_objects_model->GetItemType(child) & item_type)
                sels.Add(child);

        // If some part is selected, unselect all items except of selected parts of the current object
        UnselectAll();
        SetSelections(sels);
    }
    else
    {
        for (const auto& item : sels)
        {
            if (!IsSelected(item)) // if this item is unselected now (from previous actions)
                continue;

            if (m_objects_model->GetItemType(item) & itSettings) {
                Unselect(item);
                continue;
            }

            const wxDataViewItem& parent = m_objects_model->GetParent(item);
            if (parent != wxDataViewItem(nullptr) && IsSelected(parent))
                Unselect(parent);
            else
            {
                wxDataViewItemArray unsels;
                m_objects_model->GetAllChildren(item, unsels);
                for (const auto& unsel_item : unsels)
                    Unselect(unsel_item);
            }

            if (m_objects_model->GetItemType(item) & itVolume)
                Unselect(item);

            m_selection_mode = smInstance;
        }
    }

    if (!msg_string.IsEmpty())
        show_info(this, msg_string, _(L("Info")));

    if (!IsSelected(m_last_selected_item))
        m_last_selected_item = wxDataViewItem(nullptr);

    m_prevent_list_events = false;
}

void ObjectList::fix_cut_selection()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (fix_cut_selection(sels)) {
        m_prevent_list_events = true;

        // If some part is selected, unselect all items except of selected parts of the current object
        UnselectAll();
        SetSelections(sels);

        m_prevent_list_events = false;
    }
}

bool ObjectList::fix_cut_selection(wxDataViewItemArray &sels)
{
    if (wxGetApp().plater()->canvas3D()->get_gizmos_manager().get_current_type() == GLGizmosManager::Scale) {
        for (const auto &item : sels) {
            if (m_objects_model->GetItemType(item) & (itInstance | itObject) ||
                (m_objects_model->GetItemType(item) & itSettings && m_objects_model->GetItemType(m_objects_model->GetParent(item)) & itObject)) {
                bool is_instance_selection = m_objects_model->GetItemType(item) & itInstance;

                int object_idx = m_objects_model->GetObjectIdByItem(item);
                int inst_idx   = is_instance_selection ? m_objects_model->GetInstanceIdByItem(item) : 0;

                if (auto obj = object(object_idx); obj->is_cut()) {
                    sels.Clear();

                    auto cut_id = obj->cut_id;

                    int objects_cnt = int((*m_objects).size());
                    for (int obj_idx = 0; obj_idx < objects_cnt; ++obj_idx) {
                        auto object = (*m_objects)[obj_idx];
                        if (object->is_cut() && object->cut_id.has_same_id(cut_id))
                            sels.Add(is_instance_selection ? m_objects_model->GetItemByInstanceId(obj_idx, inst_idx) : m_objects_model->GetItemById(obj_idx));
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

ModelVolume* ObjectList::get_selected_model_volume()
{
    wxDataViewItem item = GetSelection();
    if (!item)
        return nullptr;
    if (m_objects_model->GetItemType(item) != itVolume) {
        if (m_objects_model->GetItemType(m_objects_model->GetParent(item)) == itVolume)
            item = m_objects_model->GetParent(item);
        else
            return nullptr;
    }

    const auto vol_idx = m_objects_model->GetVolumeIdByItem(item);
    const auto obj_idx = get_selected_obj_idx();
    if (vol_idx < 0 || obj_idx < 0)
        return nullptr;

    return (*m_objects)[obj_idx]->volumes[vol_idx];
}

void ObjectList::change_part_type()
{
  wxDataViewItemArray selections;
  GetSelections(selections);

  if (selections.size() <= 1) {
    int obj_idx = get_selected_obj_idx();
    if (obj_idx < 0) {
      return;
    }

    ModelVolume* volume = get_selected_model_volume();
    if (!volume) {
      return;
    }

    const ModelVolumeType type = volume->type();
    if (type == ModelVolumeType::MODEL_PART) {
      int model_part_cnt = 0;
      for (auto vol : (*m_objects)[obj_idx]->volumes) {
        if (vol->type() == ModelVolumeType::MODEL_PART)
          ++model_part_cnt;
      }

      if (model_part_cnt == 1) {
        Slic3r::GUI::show_error(nullptr, _(L("The type of the last solid object part is not to be changed.")));
        return;
      }
    }

    // ORCA: Fix crash when changing type of svg / text modifier
    wxArrayString names;
    names.Add(_L("Part"));
    names.Add(_L("Negative Part"));
    names.Add(_L("Modifier"));
    if (!volume->is_svg() && !volume->is_text()) {
      names.Add(_L("Support Blocker"));
      names.Add(_L("Support Enforcer"));
    }

    SingleChoiceDialog dlg(_L("Type:"), _L("Choose part type"), names, int(type));
    auto new_type = ModelVolumeType(dlg.GetSingleChoiceIndex());

    if (new_type == type || new_type == ModelVolumeType::INVALID) {
      return;
    }

    take_snapshot("Change part type");

    volume->set_type(new_type);
    wxDataViewItemArray sel = reorder_volumes_and_get_selection(obj_idx, [volume](const ModelVolume* vol) { return vol == volume; });
    if (!sel.IsEmpty()) {
      select_item(sel.front());
    }
    
  return;
  }

  // --- Multi Selection ---
  struct Target { int obj_idx; Slic3r::ModelVolume* vol; };
  std::vector<Target> targets;
  targets.reserve(selections.size());
  bool any_text_or_svg = false;

  for (const auto& item : selections) {
    auto typeMask = m_objects_model->GetItemType(item);
    if (!(typeMask & itVolume)) {
      continue;
    }

    int obj_idx = -1, vol_idx = -1;
    get_selected_item_indexes(obj_idx, vol_idx, item);
    if (obj_idx < 0 || vol_idx < 0) {
      continue;
    }

    ModelVolume* vol = (*m_objects)[obj_idx]->volumes[vol_idx];
    if (!vol) {
      continue;
    }

    targets.push_back({ obj_idx, vol });
    if (vol->is_svg() || vol->is_text())
      any_text_or_svg = true;
  }
  
  if (targets.empty()) {
    return;
  }

  wxArrayString names;
  names.Add(_L("Part"));
  names.Add(_L("Negative Part"));
  names.Add(_L("Modifier"));
  if (!any_text_or_svg) {
    names.Add(_L("Support Blocker"));
    names.Add(_L("Support Enforcer"));
  }

  // Preselect current type of the first selected volume
  ModelVolumeType initial_type = targets.front().vol->type();
  SingleChoiceDialog dlg(_L("Type:"), _L("Choose part type"), names, int(initial_type));
  auto new_type = ModelVolumeType(dlg.GetSingleChoiceIndex());
  if (new_type == ModelVolumeType::INVALID) {
    return;
  }

  if (new_type != ModelVolumeType::MODEL_PART) {
    // Count initial MODEL_PARTs per object
    std::unordered_map<int, int> parts_initial;
    for (const auto& t : targets) {
      int cnt = 0;
      for (auto v : (*m_objects)[t.obj_idx]->volumes)
          if (v->type() == ModelVolumeType::MODEL_PART) ++cnt;
      parts_initial[t.obj_idx] = cnt;
    }

    // Count how many selected MODEL_PARTs would be converted away, per object
    std::unordered_map<int, int> parts_to_remove;
    for (const auto& t : targets) {
      if (t.vol->type() == ModelVolumeType::MODEL_PART) {
        ++parts_to_remove[t.obj_idx];
      }
    }

    // If for any object: initial_parts > 0 and removals == initial_parts => would remove all
    bool would_remove_all_for_any = false;
    for (const auto& kv : parts_to_remove) {
      const int obj_idx   = kv.first;
      const int removing  = kv.second;
      const int initial   = parts_initial[obj_idx];
      if (initial > 0 && removing == initial) {
        would_remove_all_for_any = true;
        break;
      }
    }

    if (would_remove_all_for_any) {
      Slic3r::GUI::show_error(nullptr, _(L("The type of the last solid object part is not to be changed.")));
      return;
    }
  }

  take_snapshot("Change part type (multi)");

  // Apply changes
  size_t applied = 0, skipped_same = 0;
  std::unordered_map<int, std::vector<ModelVolume*>> changed_per_object;
  for (const auto& t : targets) {
    const auto current = t.vol->type();
    if (current == new_type) {
        ++skipped_same;
        continue;
    }
    t.vol->set_type(new_type);
    changed_per_object[t.obj_idx].push_back(t.vol);
    ++applied;
  }

  if (applied == 0) {
    // Nothing changed; keep the original selection as-is
    select_items(selections);
    return;
  }

 // Reorder per object and rebuild selection to follow changed volumes 
  wxDataViewItemArray new_selection;
  for (const auto& kv : changed_per_object) {
    const int obj_idx = kv.first;
    const auto& changed_vols = kv.second;
    
    std::unordered_set<const ModelVolume*> changed_set;
    changed_set.reserve(changed_vols.size());
    for (const auto* v : changed_vols) {
      changed_set.insert(v);
    }

    wxDataViewItemArray sel = reorder_volumes_and_get_selection(
      obj_idx,
      [&changed_set](const ModelVolume* v) -> bool {
        return changed_set.find(v) != changed_set.end();
      }
    );

    // Append to new_selection
    for (const auto& it : sel) new_selection.Add(it);
  }

  if (!new_selection.IsEmpty()) {
    select_items(new_selection);
  } else {
    select_items(selections);
  }

  return;
}

void ObjectList::last_volume_is_deleted(const int obj_idx)
{
    // BBS: object (obj_idx calc in obj list) is already removed from m_objects in Plater::priv::remove().
#if 0
    if (obj_idx < 0 || size_t(obj_idx) >= m_objects->size() || (*m_objects)[obj_idx]->volumes.size() != 1)
        return;

    auto volume = (*m_objects)[obj_idx]->volumes.front();

    // clear volume's config values
    volume->config.reset();

    // set a default extruder value, since user can't add it manually
    // BBS
    volume->config.set_key_value("extruder", new ConfigOptionInt(1));
#endif
}

void ObjectList::update_and_show_object_settings_item()
{
    //const wxDataViewItem item = GetSelection();
    //if (!item) return;

    //const wxDataViewItem& obj_item = m_objects_model->IsSettingsItem(item) ? m_objects_model->GetParent(item) : item;
    //select_item([this, obj_item](){ return add_settings_item(obj_item, &get_item_config(obj_item).get()); });
    part_selection_changed();
}

// Update settings item for item had it
void ObjectList::update_settings_item_and_selection(wxDataViewItem item, wxDataViewItemArray& selections)
{
    const wxDataViewItem old_settings_item = m_objects_model->GetSettingsItem(item);
    const wxDataViewItem new_settings_item = add_settings_item(item, &get_item_config(item).get());

    if (!new_settings_item && old_settings_item)
        m_objects_model->Delete(old_settings_item);

    // if ols settings item was is selected area
    if (selections.Index(old_settings_item) != wxNOT_FOUND)
    {
        // If settings item was just updated
        if (old_settings_item == new_settings_item)
        {
            Sidebar& panel = wxGetApp().sidebar();
            panel.Freeze();

            // update settings list
            wxGetApp().obj_settings()->UpdateAndShow(true);

            panel.Layout();
            panel.Thaw();
        }
        else
        // If settings item was deleted from the list,
        // it's need to be deleted from selection array, if it was there
        {
            selections.Remove(old_settings_item);

            // Select item, if settings_item doesn't exist for item anymore, but was selected
            if (selections.Index(item) == wxNOT_FOUND) {
                selections.Add(item);
                select_item(item); // to correct update of the SettingsList and ManipulationPanel sizers
            }
        }
    }
}

void ObjectList::update_object_list_by_printer_technology()
{
    m_prevent_canvas_selection_update = true;
    wxDataViewItemArray sel;
    GetSelections(sel); // stash selection

    wxDataViewItemArray object_items;
    m_objects_model->GetChildren(wxDataViewItem(nullptr), object_items);

    for (auto& object_item : object_items) {
        // update custom supports info
        update_info_items(m_objects_model->GetObjectIdByItem(object_item), &sel);

        // Update Settings Item for object
        update_settings_item_and_selection(object_item, sel);

        // Update settings for Volumes
        wxDataViewItemArray all_object_subitems;
        m_objects_model->GetChildren(object_item, all_object_subitems);
        for (auto item : all_object_subitems)
            if (m_objects_model->GetItemType(item) & itVolume)
                // update settings for volume
                update_settings_item_and_selection(item, sel);

        // Update Layers Items
        wxDataViewItem layers_item = m_objects_model->GetLayerRootItem(object_item);
        if (!layers_item)
            layers_item = add_layer_root_item(object_item);
        else if (printer_technology() == ptSLA) {
            // If layers root item will be deleted from the list, so
            // it's need to be deleted from selection array, if it was there
            wxDataViewItemArray del_items;
            bool some_layers_was_selected = false;
            m_objects_model->GetAllChildren(layers_item, del_items);
            for (auto& del_item:del_items)
                if (sel.Index(del_item) != wxNOT_FOUND) {
                    some_layers_was_selected = true;
                    sel.Remove(del_item);
                }
            if (sel.Index(layers_item) != wxNOT_FOUND) {
                some_layers_was_selected = true;
                sel.Remove(layers_item);
            }

            // delete all "layers" items
            m_objects_model->Delete(layers_item);

            // Select object_item, if layers_item doesn't exist for item anymore, but was some of layer items was/were selected
            if (some_layers_was_selected)
                sel.Add(object_item);
        }
        else {
            wxDataViewItemArray all_obj_layers;
            m_objects_model->GetChildren(layers_item, all_obj_layers);

            for (auto item : all_obj_layers)
                // update settings for layer
                update_settings_item_and_selection(item, sel);
        }
    }

    // restore selection:
    SetSelections(sel);
    m_prevent_canvas_selection_update = false;
}

void ObjectList::instances_to_separated_object(const int obj_idx, const std::set<int>& inst_idxs)
{
    if ((*m_objects)[obj_idx]->instances.size() == inst_idxs.size())
    {
        instances_to_separated_objects(obj_idx);
        return;
    }

    // create new object from selected instance
    ModelObject* model_object = (*m_objects)[obj_idx]->get_model()->add_object(*(*m_objects)[obj_idx]);
    for (int inst_idx = int(model_object->instances.size()) - 1; inst_idx >= 0; inst_idx--)
    {
        if (find(inst_idxs.begin(), inst_idxs.end(), inst_idx) != inst_idxs.end())
            continue;
        model_object->delete_instance(inst_idx);
    }

    // Add new object to the object_list
    const size_t new_obj_indx = static_cast<size_t>(m_objects->size() - 1);
    add_object_to_list(new_obj_indx);

    for (std::set<int>::const_reverse_iterator it = inst_idxs.rbegin(); it != inst_idxs.rend(); ++it)
    {
        // delete selected instance from the object
        del_subobject_from_object(obj_idx, *it, itInstance);
        delete_instance_from_list(obj_idx, *it);
    }

    // update printable state for new volumes on canvas3D
    wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_object(new_obj_indx);
    update_info_items(new_obj_indx);
}

void ObjectList::instances_to_separated_objects(const int obj_idx)
{
    const int inst_cnt = (*m_objects)[obj_idx]->instances.size();

    std::vector<size_t> object_idxs;

    for (int i = inst_cnt-1; i > 0 ; i--)
    {
        // create new object from initial
        ModelObject* object = (*m_objects)[obj_idx]->get_model()->add_object(*(*m_objects)[obj_idx]);
        for (int inst_idx = object->instances.size() - 1; inst_idx >= 0; inst_idx--)
        {
            if (inst_idx == i)
                continue;
            // delete unnecessary instances
            object->delete_instance(inst_idx);
        }

        // Add new object to the object_list
        const size_t new_obj_indx = static_cast<size_t>(m_objects->size() - 1);
        add_object_to_list(new_obj_indx);
        object_idxs.push_back(new_obj_indx);

        // delete current instance from the initial object
        del_subobject_from_object(obj_idx, i, itInstance);
        delete_instance_from_list(obj_idx, i);
    }

    // update printable state for new volumes on canvas3D
    wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_objects(object_idxs);
    for (size_t object : object_idxs)
        update_info_items(object);
}

void ObjectList::split_instances()
{
    const Selection& selection = scene_selection();
    const int obj_idx = selection.get_object_idx();
    if (obj_idx == -1)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Instances to Separated Objects");

    if (selection.is_single_full_object())
    {
        instances_to_separated_objects(obj_idx);
        return;
    }

    const int inst_idx = selection.get_instance_idx();
    const std::set<int> inst_idxs = inst_idx < 0 ?
                                    selection.get_instance_idxs() :
                                    std::set<int>{ inst_idx };

    instances_to_separated_object(obj_idx, inst_idxs);
}

void ObjectList::rename_item()
{
    const wxDataViewItem item = GetSelection();
    if (!item || !(m_objects_model->GetItemType(item) & (itVolume | itObject)))
        return ;

    const wxString new_name = wxGetTextFromUser(_(L("Enter new name"))+":", _(L("Renaming")),
                                                m_objects_model->GetName(item), this);

    if (new_name.IsEmpty())
        return;

    if (Plater::has_illegal_filename_characters(new_name)) {
        Plater::show_illegal_characters_warning(this);
        return;
    }

    if (m_objects_model->SetName(new_name, item))
        update_name_in_model(item);
}

void ObjectList::fix_through_netfabb()
{
    // Do not fix anything when a gizmo is open. There might be issues with updates
    // and what is worse, the snapshot time would refer to the internal stack.
    if (!wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().check_gizmos_closed_except(GLGizmosManager::Undefined))
        return;

    //          model_name
    std::vector<std::string>                           succes_models;
    //                   model_name     failing reason
    std::vector<std::pair<std::string, std::string>>   failed_models;

    std::vector<int> obj_idxs, vol_idxs;
    get_selection_indexes(obj_idxs, vol_idxs);

    std::vector<std::string> model_names;

    // clear selections from the non-broken models if any exists
    // and than fill names of models to repairing
    if (vol_idxs.empty()) {
#if !FIX_THROUGH_NETFABB_ALWAYS
        for (int i = int(obj_idxs.size())-1; i >= 0; --i)
                if (object(obj_idxs[i])->get_repaired_errors_count() == 0)
                    obj_idxs.erase(obj_idxs.begin()+i);
#endif // FIX_THROUGH_NETFABB_ALWAYS
        for (int obj_idx : obj_idxs)
            if (object(obj_idx))
                model_names.push_back(object(obj_idx)->name);
    }
    else {
        ModelObject* obj = object(obj_idxs.front());
        if (obj) {
#if !FIX_THROUGH_NETFABB_ALWAYS
            for (int i = int(vol_idxs.size()) - 1; i >= 0; --i)
                if (obj->get_repaired_errors_count(vol_idxs[i]) == 0)
                    vol_idxs.erase(vol_idxs.begin() + i);
#endif // FIX_THROUGH_NETFABB_ALWAYS
            for (int vol_idx : vol_idxs)
                model_names.push_back(obj->volumes[vol_idx]->name);
        }
    }

    auto plater = wxGetApp().plater();

    auto fix_and_update_progress = [this, plater, model_names](const int obj_idx, const int vol_idx,
                                          int model_idx,
                                          ProgressDialog& progress_dlg,
                                          std::vector<std::string>& succes_models,
                                          std::vector<std::pair<std::string, std::string>>& failed_models)
    {
        if (!object(obj_idx))
            return false;

        const std::string& model_name = model_names[model_idx];
        wxString msg = _L("Repairing model object");
        if (model_names.size() == 1)
            msg += ": " + from_u8(model_name) + "\n";
        else {
            msg += ":\n";
            for (int i = 0; i < int(model_names.size()); ++i)
                msg += (i == model_idx ? " > " : "   ") + from_u8(model_names[i]) + "\n";
            msg += "\n";
        }

        plater->clear_before_change_mesh(obj_idx);
        std::string res;
        if (!fix_model_by_win10_sdk_gui(*(object(obj_idx)), vol_idx, progress_dlg, msg, res))
            return false;
        //wxGetApp().plater()->changed_mesh(obj_idx);
        object(obj_idx)->ensure_on_bed();
        plater->changed_mesh(obj_idx);

        plater->get_partplate_list().notify_instance_update(obj_idx, 0);
        plater->sidebar().obj_list()->update_plate_values_for_items();

        if (res.empty())
            succes_models.push_back(model_name);
        else
            failed_models.push_back({ model_name, res });

        update_item_error_icon(obj_idx, vol_idx);
        update_info_items(obj_idx);

        return true;
    };

    Plater::TakeSnapshot snapshot(plater, "Repairing model object");

    // Open a progress dialog.
    ProgressDialog progress_dlg(_L("Repairing model object"), "", 100, find_toplevel_parent(plater),
                                    wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT, true);
    int model_idx{ 0 };
    if (vol_idxs.empty()) {
        int vol_idx{ -1 };
        for (int obj_idx : obj_idxs) {
#if !FIX_THROUGH_NETFABB_ALWAYS
            if (object(obj_idx)->get_repaired_errors_count(vol_idx) == 0)
                continue;
#endif // FIX_THROUGH_NETFABB_ALWAYS
            if (!fix_and_update_progress(obj_idx, vol_idx, model_idx, progress_dlg, succes_models, failed_models))
                break;
            model_idx++;
        }
    }
    else {
        int obj_idx{ obj_idxs.front() };
        for (int vol_idx : vol_idxs) {
            if (!fix_and_update_progress(obj_idx, vol_idx, model_idx, progress_dlg, succes_models, failed_models))
                break;
            model_idx++;
        }
    }
    // Close the progress dialog
    progress_dlg.Update(100, "");

    // Show info notification
    wxString msg;
    wxString bullet_suf = "\n   - ";
    if (!succes_models.empty()) {
        msg = _L_PLURAL("Following model object has been repaired", "Following model objects have been repaired", succes_models.size()) + ":";
        for (auto& model : succes_models)
            msg += bullet_suf + from_u8(model);
        msg += "\n\n";
    }
    if (!failed_models.empty()) {
        msg += _L_PLURAL("Failed to repair following model object", "Failed to repair following model objects", failed_models.size()) + ":\n";
        for (auto& model : failed_models)
            msg += bullet_suf + from_u8(model.first) + ": " + _(model.second);
    }
    if (msg.IsEmpty())
        msg = _L("Repairing was canceled");
    plater->get_notification_manager()->push_notification(NotificationType::NetfabbFinished, NotificationManager::NotificationLevel::PrintInfoShortNotificationLevel, into_u8(msg));
}

void ObjectList::simplify()
{
    auto plater = wxGetApp().plater();
    GLGizmosManager& gizmos_mgr = plater->get_view3D_canvas3D()->get_gizmos_manager();

    // Do not simplify when a gizmo is open. There might be issues with updates
    // and what is worse, the snapshot time would refer to the internal stack.
    if (! gizmos_mgr.check_gizmos_closed_except(GLGizmosManager::EType::Simplify))
        return;

    if (gizmos_mgr.get_current_type() == GLGizmosManager::Simplify) {
        // close first
        gizmos_mgr.open_gizmo(GLGizmosManager::EType::Simplify);
    }
    gizmos_mgr.open_gizmo(GLGizmosManager::EType::Simplify);
}

void ObjectList::update_item_error_icon(const int obj_idx, const int vol_idx) const
{
    auto obj = object(obj_idx);
    if (wxDataViewItem obj_item = m_objects_model->GetItemById(obj_idx)) {
        const std::string& icon_name = get_warning_icon_name(obj->get_object_stl_stats());
        m_objects_model->UpdateWarningIcon(obj_item, icon_name);
    }

    if (vol_idx < 0)
        return;

    if (wxDataViewItem vol_item = m_objects_model->GetItemByVolumeId(obj_idx, vol_idx)) {
        const std::string& icon_name = get_warning_icon_name(obj->volumes[vol_idx]->mesh().stats());
        m_objects_model->UpdateWarningIcon(vol_item, icon_name);
    }
}

void ObjectList::msw_rescale()
{
    set_min_height();

    const int em = wxGetApp().em_unit();

    for (int cn = colName; cn < colCount; cn++)
        GetColumn(cn)->SetWidth(m_columns_width[cn] * em);

    // rescale/update existing items with bitmaps
    m_objects_model->Rescale();

    Layout();
}

void ObjectList::sys_color_changed()
{
    wxGetApp().UpdateDVCDarkUI(this, true);
    
    msw_rescale();

    if (m_objects_model) { m_objects_model->sys_color_changed(); }
}

void ObjectList::ItemValueChanged(wxDataViewEvent &event)
{
    if (event.GetColumn() == colName)
        update_name_in_model(event.GetItem());
    else if (event.GetColumn() == colFilament) {
        wxDataViewItem item = event.GetItem();
        if (m_objects_model->GetItemType(item) == itObject)
            m_objects_model->UpdateVolumesExtruderBitmap(item, true);
        update_filament_in_config(item);
    }
}

void GUI::ObjectList::OnStartEditing(wxDataViewEvent &event)
{
    auto col  = event.GetColumn();
    auto item = event.GetItem();
    if (col == colName) {
        ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
        if (node->GetType() & itPlate) {
            int plate_idx = node->GetPlateIdx();
            if (plate_idx >= 0) {
                auto plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
                m_objects_model->SetName(from_u8(plate->get_plate_name()), GetSelection());
            }
        }
    }
}

// Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
// Here the last active column is forgotten, so when leaving the editing mode, the next mouse click will not enter the editing mode of the newly selected column.
void ObjectList::OnEditingStarted(wxDataViewEvent &event)
{
#ifdef __WXMSW__
	m_last_selected_column = -1;
#else
    event.Veto(); // Not edit with NSTableView's text
    auto col = event.GetColumn();
    auto item = event.GetItem();
    if (col == colPrint) {
        toggle_printable_state();
        return;
    } else if (col == colSupportPaint) {
        ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
        if (node->HasSupportPainting()) {
            GLGizmosManager& gizmos_mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
            if (gizmos_mgr.get_current_type() != GLGizmosManager::EType::FdmSupports)
                gizmos_mgr.open_gizmo(GLGizmosManager::EType::FdmSupports);
            else
                gizmos_mgr.reset_all_states();
        }
        return;
    }
    else if (col == colColorPaint) {
        ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
        if (node->HasColorPainting()) {
            GLGizmosManager& gizmos_mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
            if (gizmos_mgr.get_current_type() != GLGizmosManager::EType::MmSegmentation)
                gizmos_mgr.open_gizmo(GLGizmosManager::EType::MmSegmentation);
            else
                gizmos_mgr.reset_all_states();
        }
        return;
    }
    else if (col == colSinking) {
        Plater *    plater = wxGetApp().plater();
        GLCanvas3D *cnv    = plater->canvas3D();
        Plater::TakeSnapshot(plater, "Shift objects to bed");
        int obj_idx, vol_idx;
        get_selected_item_indexes(obj_idx, vol_idx, item);
        (*m_objects)[obj_idx]->ensure_on_bed();
        cnv->reload_scene(true, true);
        update_info_items(obj_idx);
        notify_instance_updated(obj_idx);
    }
    else if (col == colEditing) {
        //show_context_menu(evt_context_menu);
        int obj_idx, vol_idx;

        get_selected_item_indexes(obj_idx, vol_idx, item);
        //wxGetApp().plater()->PopupObjectTable(obj_idx, vol_idx, mouse_pos);
        dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab(vol_idx >= 0))->reset_model_config();
        return;
    }
    if (col != colFilament && col != colName)
        return;
    auto column = GetColumn(col);
    const auto renderer = column->GetRenderer();
    if (!renderer->GetEditorCtrl()) {
        renderer->StartEditing(item, GetItemRect(item, column));
        if (col == colName) // TODO: for colName editing, disable shortcuts
            SetAcceleratorTable(wxNullAcceleratorTable);
    }
#ifdef __WXOSX__
    SetCustomRendererPtr(dynamic_cast<wxDataViewCustomRenderer*>(renderer));
#endif
#endif //__WXMSW__
}

void ObjectList::OnEditingDone(wxDataViewEvent &event)
{
    if (event.GetColumn() != colName)
        return;

    if (event.IsEditCancelled()) {
        if (m_objects_model->GetItemType(event.GetItem()) & itPlate) {
            int plate_idx = -1;
            m_objects_model->GetItemType(event.GetItem(), plate_idx);
            if (plate_idx >= 0) {
                auto plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
                m_objects_model->SetCurSelectedPlateFullName(plate_idx, plate->get_plate_name());
            }
        }
    }

    const auto renderer = dynamic_cast<BitmapTextRenderer*>(GetColumn(colName)->GetRenderer());
#if __WXOSX__
    SetAcceleratorTable(m_accel);
#endif

    if (renderer->WasCanceled())
		wxTheApp->CallAfter([this]{ Plater::show_illegal_characters_warning(this); });

#ifdef __WXMSW__
	// Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
	// Here the last active column is forgotten, so when leaving the editing mode, the next mouse click will not enter the editing mode of the newly selected column.
	m_last_selected_column = -1;
#endif //__WXMSW__

    Plater* plater = wxGetApp().plater();
    if (plater)
        plater->set_current_canvas_as_dirty();
}

// BBS: remove "const" qualifier
void ObjectList::set_extruder_for_selected_items(const int extruder)
{
    // BBS: check extruder id
    std::vector<std::string> colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    if (extruder > colors.size())
        return;

    wxDataViewItemArray sels;
    GetSelections(sels);

    if (sels.empty())
        return;

    take_snapshot("Change Filaments");

    for (const wxDataViewItem& sel_item : sels)
    {
        /* We can change extruder for Object/Volume only.
         * So, if Instance is selected, get its Object item and change it
         */
        ItemType sel_item_type = m_objects_model->GetItemType(sel_item);
        wxDataViewItem item = (sel_item_type & itInstance) ? m_objects_model->GetObject(item) : sel_item;
        ItemType type = m_objects_model->GetItemType(item);
        if (type & itVolume) {
            const int obj_idx = m_objects_model->GetObjectIdByItem(item);
            int vol_idx = m_objects_model->GetVolumeIdByItem(item);
            vol_idx     = m_objects_model->get_real_volume_index_in_3d(obj_idx, vol_idx);
            if ((obj_idx < m_objects->size()) && (obj_idx < (*m_objects)[obj_idx]->volumes.size())) {
                auto volume_type = (*m_objects)[obj_idx]->volumes[vol_idx]->type();
                if (volume_type != ModelVolumeType::MODEL_PART && volume_type != ModelVolumeType::PARAMETER_MODIFIER)
                    continue;
            }
        }

        if (type & itLayerRoot)
            continue;

        // BBS: handle extruder 0 for part, use it's parent extruder
        int new_extruder = extruder;
        if (extruder == 0) {
            if (type & itObject) {
                new_extruder = 1;
            }
            else if ((type & itVolume) && (m_objects_model->GetVolumeType(sel_item) == ModelVolumeType::MODEL_PART)) {
                new_extruder = m_objects_model->GetExtruderNumber(m_objects_model->GetParent(sel_item));
            }
        }

        if (type & itInfo && m_objects_model->GetInfoItemType(item) == InfoItemType::CutConnectors) {
            const int obj_idx = m_objects_model->GetObjectIdByItem(item);
            for (size_t i = 0; i < (*m_objects)[obj_idx]->volumes.size(); i++) {
                auto mv = (*m_objects)[obj_idx]->volumes[i];
                if (mv->is_cut_connector()) {
                    ModelConfig &config = mv->config;
                    if (config.has("extruder"))
                        config.set("extruder", new_extruder);
                    else
                        config.set_key_value("extruder", new ConfigOptionInt(new_extruder));
                }
            }
        } else {
            ModelConfig &config = get_item_config(item);
            if (config.has("extruder"))
                config.set("extruder", new_extruder);
            else
                config.set_key_value("extruder", new ConfigOptionInt(new_extruder));
        }

        // for object, clear all its part volume's extruder config
        if (type & itObject) {
            ObjectDataViewModelNode* node = (ObjectDataViewModelNode*)item.GetID();
            for (ModelVolume* mv : node->m_model_object->volumes) {
                if (mv->type() == ModelVolumeType::MODEL_PART && mv->config.has("extruder"))
                    mv->config.erase("extruder");
            }
        }

        const wxString extruder_str = wxString::Format("%d", new_extruder);
        m_objects_model->SetExtruder(extruder_str, item);
    }

    // update scene
    wxGetApp().plater()->update();

    // BBS: update extruder/filament column
    Refresh();
}

void ObjectList::on_plate_added(PartPlate* part_plate)
{
    wxDataViewItem plate_item = m_objects_model->AddPlate(part_plate);
}

void ObjectList::on_plate_deleted(int plate_idx)
{
    m_objects_model->DeletePlate(plate_idx);

    wxDataViewItemArray top_list;
    m_objects_model->GetChildren(wxDataViewItem(nullptr), top_list);
    for (wxDataViewItem item : top_list) {
        Expand(item);
    }
}

void ObjectList::reload_all_plates(bool notify_partplate)
{
    m_prevent_canvas_selection_update = true;

    // Unselect all objects before deleting them, so that no change of selection is emitted during deletion.

    /* To avoid execution of selection_changed()
     * from wxEVT_DATAVIEW_SELECTION_CHANGED emitted from DeleteAll(),
     * wrap this two functions into m_prevent_list_events *
     * */
    m_prevent_list_events = true;
    this->UnselectAll();
    m_objects_model->ResetAll();
    m_prevent_list_events = false;

    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < ppl.get_plate_count(); i++) {
        PartPlate* pp = ppl.get_plate(i);
        m_objects_model->AddPlate(pp, wxEmptyString);
    }

    size_t obj_idx = 0;
    std::vector<size_t> obj_idxs;
    obj_idxs.reserve(m_objects->size());
    while (obj_idx < m_objects->size()) {
        add_object_to_list(obj_idx, false, notify_partplate);
        obj_idxs.push_back(obj_idx);
        ++obj_idx;
    }

    update_selections();

    m_prevent_canvas_selection_update = false;

    // update scene
    wxGetApp().plater()->update();
    // update printable states on canvas
    wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_objects(obj_idxs);
}

void ObjectList::on_plate_selected(int plate_index)
{
    wxDataViewItem item = m_objects_model->GetItemByPlateId(plate_index);
    wxDataViewItem sel = GetSelection();

    if (sel == item)
        return;

    UnselectAll();
    Select(item);
}

//BBS: notify partplate the instance added/updated
void ObjectList::notify_instance_updated(int obj_idx)
{
    const int inst_cnt = (*m_objects)[obj_idx]->instances.size();
    PartPlateList& list = wxGetApp().plater()->get_partplate_list();
    for (int index = 0; index < inst_cnt; index ++)
        list.notify_instance_update(obj_idx, index);
}

void ObjectList::update_after_undo_redo()
{
    Plater::SuppressSnapshots suppress(wxGetApp().plater());
    //BBS: undo/redo will rebuild all the plates before
    //no need to notify instance to partplate
    reload_all_plates(false);
}

wxDataViewItemArray ObjectList::reorder_volumes_and_get_selection(int obj_idx, std::function<bool(const ModelVolume*)> add_to_selection/* = nullptr*/)
{
    wxDataViewItemArray items;
    if (obj_idx < 0 || obj_idx >= m_objects->size()) {
        return items;
    }
    ModelObject* object = (*m_objects)[obj_idx];
    if (object->volumes.size() <= 1)
        return items;

    object->sort_volumes(true);
    update_info_items(obj_idx, nullptr, true);
    items = add_volumes_to_object_in_list(obj_idx, std::move(add_to_selection));

    changed_object(obj_idx);
    return items;
}

void ObjectList::apply_volumes_order()
{
    if (!m_objects)
        return;

    for (size_t obj_idx = 0; obj_idx < m_objects->size(); obj_idx++)
        reorder_volumes_and_get_selection(obj_idx);
}

void ObjectList::update_printable_state(int obj_idx, int instance_idx)
{
    ModelObject* object = (*m_objects)[obj_idx];

    const PrintIndicator printable = object->instances[instance_idx]->printable ? piPrintable : piUnprintable;
    if (object->instances.size() == 1)
        instance_idx = -1;

    m_objects_model->SetPrintableState(printable, obj_idx, instance_idx);
}

void ObjectList::toggle_printable_state()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return;

    wxDataViewItem frst_item = sels[0];

    ItemType type = m_objects_model->GetItemType(frst_item);
    if (!(type & (itObject | itInstance)))
        return;


    int obj_idx = m_objects_model->GetObjectIdByItem(frst_item);
    int inst_idx = type == itObject ? 0 : m_objects_model->GetInstanceIdByItem(frst_item);
    bool printable = !object(obj_idx)->instances[inst_idx]->printable;

    // BBS: remove snapshot name "Set Printable group", "Set Unprintable group", "Set Printable"...
    take_snapshot("");

    std::vector<size_t> obj_idxs;
    for (auto item : sels)
    {
        type = m_objects_model->GetItemType(item);
        if (!(type & (itObject | itInstance)))
            continue;

        obj_idx = m_objects_model->GetObjectIdByItem(item);
        ModelObject* obj = object(obj_idx);

        obj_idxs.emplace_back(static_cast<size_t>(obj_idx));

        // set printable value for selected instance/instances in object
        if (type == itInstance) {
            inst_idx = m_objects_model->GetInstanceIdByItem(item);
            obj->instances[m_objects_model->GetInstanceIdByItem(item)]->printable = printable;
        }
        else
            for (auto inst : obj->instances)
                inst->printable = printable;

        // update printable state in ObjectList
        m_objects_model->SetObjectPrintableState(printable ? piPrintable : piUnprintable, item);
    }

    sort(obj_idxs.begin(), obj_idxs.end());
    obj_idxs.erase(unique(obj_idxs.begin(), obj_idxs.end()), obj_idxs.end());

    // update printable state on canvas
    wxGetApp().plater()->get_view3D_canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

    // update scene
    wxGetApp().plater()->update();
    wxGetApp().plater()->reload_paint_after_background_process_apply();
}

void ObjectList::enable_layers_editing()
{
    wxDataViewItemArray sels;
    GetSelections(sels);
    if (sels.IsEmpty())
        return;

    wxDataViewItem frst_item = sels[0];

    ItemType type = m_objects_model->GetItemType(frst_item);
    if (!(type & itObject))
        return;
    //take_snapshot("");

    auto view3d = wxGetApp().plater()->get_view3D_canvas3D();
    if (view3d != nullptr && m_objects_model->IsVariableHeight(frst_item)){
        view3d->enable_layers_editing(true);
    }
}

ModelObject* ObjectList::object(const int obj_idx) const
{
    if (obj_idx < 0)
        return nullptr;

    return (*m_objects)[obj_idx];
}

void ObjectList::apply_object_instance_transfrom_to_all_volumes(ModelObject *model_object, bool need_update_assemble_matrix)
{
    const Geometry::Transformation &instance_transformation  = model_object->instances[0]->get_transformation();
    Vec3d                           original_instance_center = instance_transformation.get_offset();

    if (need_update_assemble_matrix) {
        // apply the instance_transform(except offset) to assemble_transform
        Geometry::Transformation instance_transformation_copy = instance_transformation;
        instance_transformation_copy.set_offset(Vec3d(0, 0, 0)); // remove the effect of offset
        const Transform3d &instance_inverse_matrix = instance_transformation_copy.get_matrix().inverse();
        const Transform3d &assemble_matrix         = model_object->instances[0]->get_assemble_transformation().get_matrix();
        Transform3d        new_assemble_transform  = assemble_matrix * instance_inverse_matrix;
        model_object->instances[0]->set_assemble_from_transform(new_assemble_transform);
    }

    // apply the instance_transform to volumn
    const Transform3d &transformation_matrix = instance_transformation.get_matrix();
    for (ModelVolume *volume : model_object->volumes) {
        const Transform3d &volume_matrix = volume->get_matrix();
        Transform3d        new_matrix    = transformation_matrix * volume_matrix;
        volume->set_transformation(new_matrix);
    }
    model_object->instances[0]->set_transformation(Geometry::Transformation());

    model_object->ensure_on_bed();
    // keep new instance center the same as the original center
    model_object->translate(-original_instance_center);
    model_object->translate_instances(original_instance_center);

    // update the cache data in selection to keep the data of ModelVolume and GLVolume are consistent
    wxGetApp().plater()->update();
}

} //namespace GUI
} //namespace Slic3r
