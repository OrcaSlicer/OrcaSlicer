#include "FilamentPickerDialog.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "EncodedFilament.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/DialogButtons.hpp"
#include "wxExtensions.hpp"
#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define COLOR_DEMO_SIZE wxSize(FromDIP(50), FromDIP(50))
#define COLOR_BTN_BITMAP_SIZE wxSize(FromDIP(24), FromDIP(24))
#define COLOR_BTN_SIZE wxSize(FromDIP(30), FromDIP(30))
#define GRID_GAP FromDIP(2)
#define COLS 9  // fixed column count
#define MAX_VISIBLE_ROWS 7  // max rows before scrollbar appears

namespace Slic3r { namespace GUI {

wxColour FilamentPickerDialog::GetSelectedColour() const
{
    if (!m_color_demo) return wxNullColour;
    return m_color_demo->GetBackgroundColour();
}

void FilamentPickerDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    // Handle DPI change
    CreateShapedBitmap();
    SetWindowShape();
    Refresh();
    Layout();
}

// Flash effect implementation
void FilamentPickerDialog::StartFlashing()
{
    // Stop any existing flash timer
    if (m_flash_timer) {
        m_flash_timer->Stop();
        delete m_flash_timer;
    }

    m_flash_timer = new wxTimer(this, wxID_ANY + 1);
    Bind(wxEVT_TIMER, &FilamentPickerDialog::OnFlashTimer, this, m_flash_timer->GetId());
    m_flash_step = 0;
    m_flash_timer->Start(50);
}

FilamentPickerDialog::FilamentPickerDialog(wxWindow *parent, const wxString& fila_id, const FilamentColor& fila_color, const std::string& fila_type)
    : DPIDialog(parent ? parent : wxGetApp().mainframe,
        wxID_ANY,
        _L("Select Filament"),
        wxDefaultPosition,
        wxDefaultSize,
        wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_SHAPED)
{
    SetBackgroundColour(wxColour(255, 255, 255));

    m_color_query = new FilamentColorCodeQuery();
    m_is_data_loaded = LoadFilamentData(fila_id);
    m_cur_filament_color = fila_color;
    wxString color_name = m_color_query->GetFilaColorName(fila_id, fila_color);
    m_cur_color_name = new wxString(color_name);

    wxBoxSizer *container_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Preview panel (always present)
    wxBoxSizer *preview_sizer = CreatePreviewPanel(fila_color, fila_type);
    main_sizer->AddSpacer(FromDIP(4));
    main_sizer->Add(preview_sizer, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(FromDIP(12));

    wxBoxSizer *line_sizer = CreateSeparatorLine();
    main_sizer->Add(line_sizer, 0, wxEXPAND, 0);

    // If caller passed an initial colour, reflect it in preview box.
    if (m_is_data_loaded) {
        // Colour grid with all filaments
        wxScrolledWindow* color_grid = CreateColorGrid();
        main_sizer->Add(color_grid, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    }

    // "More colours" button (always present)
    CreateMoreInfoButton();
    main_sizer->Add(m_more_btn, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    main_sizer->AddSpacer(FromDIP(8));

    // OK / Cancel buttons
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok_btn = dlg_btns->GetOK();
    m_cancel_btn = dlg_btns->GetCANCEL();

    main_sizer->Add(dlg_btns, 0, wxEXPAND);
    container_sizer->Add(main_sizer, 1, wxEXPAND | wxALL, FromDIP(16));

    SetSizer(container_sizer);
    Layout();
    container_sizer->Fit(this);

    // Position the dialog relative to the parent window
    if (GetParent()) {
        // Align the dialog with the sidebar
        auto& sidebar = wxGetApp().sidebar();
        wxPoint sidebar_pos = sidebar.GetScreenPosition();
        wxSize sidebar_size = sidebar.GetSize();

        wxPoint new_pos(
            sidebar_pos.x + sidebar_size.GetWidth() + FromDIP(10),
            sidebar_pos.y + FromDIP(80)
        );
        SetPosition(new_pos);
    } else {
        Centre(wxBOTH); // If no parent window, center the dialog
    }

    // Create shaped window after sizing
    CreateShapedBitmap();
#ifndef __WXGTK__
    // Windows and macOS can set shape immediately
    SetWindowShape();
#endif
#ifdef __WXGTK__
    // GTK platform needs to wait for window creation
    Bind(wxEVT_CREATE, &FilamentPickerDialog::OnWindowCreate, this);
#endif

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    // Set window transparency
    SetTransparent(255);
    BindEvents();

    // Start click detection timer for outside click detection
    StartClickDetection();
}

FilamentPickerDialog::~FilamentPickerDialog()
{
    // Clean up all timers
    CleanupTimers();

    delete m_color_query;
    m_color_query = nullptr;

    delete m_cur_color_name;
    m_cur_color_name = nullptr;
}

void FilamentPickerDialog::CreateShapedBitmap()
{
    wxSize size = GetSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0) {
        return;
    }

    // Create a bitmap with alpha channel
    m_shape_bmp.Create(size.GetWidth(), size.GetHeight(), 32);

    wxMemoryDC dc;
    dc.SelectObject(m_shape_bmp);

    dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
    dc.Clear();

    // Draw main white shape on top, positioned to let shadow show through
    dc.SetBrush(wxBrush(wxColour(255, 255, 255, 255)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(0, 0,
                          size.GetWidth(),
                          size.GetHeight(),
                          FromDIP(m_corner_radius));

    dc.SelectObject(wxNullBitmap);
}

void FilamentPickerDialog::SetWindowShape()
{
    if (!m_shape_bmp.IsOk()) {
        return;
    }

    // Create a region from the bitmap using magenta as transparent mask color
    wxRegion region(m_shape_bmp, wxColour(0, 0, 0));

    if (region.IsOk()) {
        SetShape(region);
    }
}

bool FilamentPickerDialog::LoadFilamentData(const wxString& fila_id)
{
    m_cur_color_codes = m_color_query->GetFilaInfoMap(fila_id);

    if (!m_cur_color_codes) {
        BOOST_LOG_TRIVIAL(warning) << "No color codes found for filament ID: " << fila_id.ToStdString();
        return false;
    }

    FilamentColor2CodeMap* color_map = m_cur_color_codes->GetFilamentColor2CodeMap();
    if (!color_map) {
        BOOST_LOG_TRIVIAL(warning) << "No color map found for filament ID: " << fila_id.ToStdString();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Successfully loaded " << color_map->size() << " color variants for filament " << fila_id.ToStdString();
    return !color_map->empty();
}

wxBoxSizer* FilamentPickerDialog::CreatePreviewPanel(const FilamentColor& fila_color, const std::string& fila_type)
{
    wxBoxSizer *preview_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Create color preview bitmap
    CreateColorBitmap(fila_color);
    preview_sizer->Add(m_color_demo, 0, wxALIGN_CENTER_VERTICAL, 0);
    preview_sizer->AddSpacer(FromDIP(12));

    // Create info labels section
    wxBoxSizer *label_sizer = CreateInfoSection();
    SetupLabelsContent(fila_color, fila_type);
    preview_sizer->Add(label_sizer, 1, wxALIGN_CENTER_VERTICAL, 0);

    return preview_sizer;
}

void FilamentPickerDialog::CreateColorBitmap(const FilamentColor &fila_color)
{
    m_color_demo = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, COLOR_DEMO_SIZE, 0);

    // Generate bitmap content
    if (fila_color.ColorCount() > 0) {
        std::vector<wxColour> wx_colors(fila_color.m_colors.begin(), fila_color.m_colors.end());
        wxBitmap init_bmp = create_filament_bitmap(wx_colors, COLOR_DEMO_SIZE,
                                                fila_color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR);
        m_color_demo->SetBitmap(init_bmp);
    }
    else{
        std::vector<wxColour> wx_colors;
        wx_colors.push_back(wxNullColour);
        wxBitmap init_bmp = create_filament_bitmap(wx_colors, COLOR_DEMO_SIZE, false);
        m_color_demo->SetBitmap(init_bmp);
    }
}

wxBoxSizer* FilamentPickerDialog::CreateInfoSection()
{
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Create the container box
    wxStaticBox *info_box = new wxStaticBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition);
    info_box->SetSize(wxSize(FromDIP(240), FromDIP(24)));
    info_box->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    wxStaticBoxSizer *box_sizer = new wxStaticBoxSizer(info_box, wxHORIZONTAL);

    // Create labels with ellipsize style for text overflow
    m_label_preview_color = new wxStaticText(this, wxID_ANY, _L("Custom Color"),
                                           wxDefaultPosition, wxDefaultSize,
                                           wxST_ELLIPSIZE_END);
    m_label_preview_idx = new wxStaticText(this, wxID_ANY, "",
                                         wxDefaultPosition, wxDefaultSize); // No size limit, no ellipsis
    m_label_preview_type = new wxStaticText(this, wxID_ANY, "",
                                          wxDefaultPosition, wxSize(FromDIP(220), FromDIP(16)),
                                          wxST_ELLIPSIZE_END);

    // Set maximum width for color label to enable proper ellipsis behavior
    m_label_preview_color->SetMaxSize(wxSize(FromDIP(160), -1));

    // Setup fonts
    wxFont bold_font = m_label_preview_color->GetFont();
    bold_font.SetWeight(wxFONTWEIGHT_BOLD);
#ifdef __WXMSW__
    bold_font.SetPointSize(FromDIP(8));
#endif
    m_label_preview_color->SetFont(bold_font);
    m_label_preview_idx->SetFont(bold_font);

    m_label_preview_type->SetForegroundColour(wxColour(128, 128, 128));

    // Layout with platform-specific spacing
#ifdef __WXMSW__
    int spacer = FromDIP(2), vPadding = FromDIP(0), gap1 = FromDIP(-6), gap2 = FromDIP(4);
#else
    int spacer = FromDIP(0), vPadding = FromDIP(-1), gap1 = FromDIP(0), gap2 = FromDIP(2);
#endif

    box_sizer->AddSpacer(spacer);
    box_sizer->Add(m_label_preview_color, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, vPadding);
    box_sizer->AddSpacer(FromDIP(2));
    box_sizer->Add(m_label_preview_idx, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, vPadding);
    box_sizer->AddSpacer(spacer);

    main_sizer->Add(box_sizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP, gap1);
    main_sizer->AddSpacer(gap2);
    main_sizer->Add(m_label_preview_type, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));

    return main_sizer;
}

void FilamentPickerDialog::SetupLabelsContent(const FilamentColor &fila_color, const std::string &fila_type)
{
    m_label_preview_type->SetLabel(from_u8(fila_type));
    if (m_cur_color_name && !m_cur_color_name->IsEmpty()) {
        m_label_preview_color->SetLabel(*m_cur_color_name);

        // Try to get additional color code information
        if (m_cur_color_codes) {
            FilamentColorCode *color_code = m_cur_color_codes->GetColorCode(fila_color);
            if (color_code) {
                m_label_preview_idx->SetLabel(wxString::Format("(%s)", color_code->GetFilaColorCode()));
            }
        }
    }
    else{
        if (fila_color.ColorCount() == 0){
            m_label_preview_color->SetLabel(_L("Null Color"));
        }
        else if (fila_color.ColorCount() == 1) {
            m_label_preview_color->SetLabel(fila_color.m_colors.begin()->GetAsString(wxC2S_HTML_SYNTAX));
        }
        else{
            m_label_preview_color->SetLabel(_L("Multiple Color"));
        }
    }
}

wxBoxSizer* FilamentPickerDialog::CreateSeparatorLine()
{
    wxBoxSizer *line_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxPanel* separator_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)));
    separator_line->SetBackgroundColour(wxColour(238,238,238));
    wxStaticText* line_text = new wxStaticText(this, wxID_ANY, _L("Official Filament"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    line_text->SetForegroundColour(wxColour(128, 128, 128));
    line_sizer->Add(line_text, 0, wxEXPAND, 0);
    line_sizer->AddSpacer(FromDIP(8));
    line_sizer->Add(separator_line, 1, wxALIGN_CENTER_VERTICAL, 0);
    return line_sizer;
}

wxScrolledWindow* FilamentPickerDialog::CreateColorGrid()
{
    if (!m_cur_color_codes) return nullptr;

    FilamentColor2CodeMap* color_map = m_cur_color_codes->GetFilamentColor2CodeMap();
    if (!color_map) return nullptr;

    // Calculate required row count
    int total_colors = color_map->size();
    int needed_rows = (total_colors + COLS - 1) / COLS;  // round-up division
    bool need_scroll = needed_rows > MAX_VISIBLE_ROWS;

    // Create a vertical-only scrolled window
    wxScrolledWindow* scroll_win = new wxScrolledWindow(
        this,
        wxID_ANY,
        wxDefaultPosition,
        wxDefaultSize,
        wxVSCROLL | wxNO_BORDER
    );

    wxGridSizer* grid_sizer = new wxGridSizer(needed_rows, COLS, GRID_GAP, GRID_GAP);

    if (!color_map->empty()) {
        for (const auto& color_pair : *color_map) {
            const FilamentColor& fila_color = color_pair.first;        // color info
            FilamentColorCode* color_code = color_pair.second;         // color code

            if (!color_code) continue;
            std::vector<wxColour> wx_colors(fila_color.m_colors.begin(), fila_color.m_colors.end());
            wxBitmap btn_bmp = create_filament_bitmap(
                wx_colors,
                COLOR_BTN_BITMAP_SIZE,
                fila_color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR
            );

            if (!btn_bmp.IsOk()) {
                BOOST_LOG_TRIVIAL(error) << "Failed to create bitmap for filament " << color_code->GetFilaColorCode().ToStdString();
                continue;
            }

            wxBitmapButton* btn = new wxBitmapButton(
                scroll_win,
                wxID_ANY,
                btn_bmp,
                wxDefaultPosition,
                COLOR_BTN_SIZE,
                wxBU_EXACTFIT | wxNO_BORDER
            );

            if (btn) {
                // Remove any default background and borders
                btn->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

                // Set tooltip with filament information
                wxString tooltip = wxString::Format("%s", color_code->GetFilaColorName());
                btn->SetToolTip(tooltip);

                // Check if this color matches the current color name and set as selected
                bool is_matching_color = (m_cur_color_name &&
                                        !m_cur_color_name->IsEmpty() &&
                                        *m_cur_color_name == color_code->GetFilaColorName());

                if (is_matching_color) {
                    m_cur_filament_color = color_code->GetFilaColor();
                    m_cur_selected_btn = btn;
                    UpdatePreview(*color_code);
                    btn->Bind(wxEVT_PAINT, &FilamentPickerDialog::OnButtonPaint, this);
                }

                // Bind click
                btn->Bind(wxEVT_LEFT_DOWN, [this, btn, color_code](wxMouseEvent& evt) {
                    m_cur_filament_color = color_code->GetFilaColor();
                    UpdatePreview(*color_code);
                    UpdateButtonStates(btn);
                    evt.Skip();
                });

                // Hover highlight
                btn->Bind(wxEVT_ENTER_WINDOW, [btn](wxMouseEvent& evt) {
                    evt.Skip();
                });

                btn->Bind(wxEVT_LEAVE_WINDOW, [btn](wxMouseEvent& evt) {
                    evt.Skip();
                });

                grid_sizer->Add(btn, 0, wxALL | wxALIGN_CENTER, FromDIP(1));
            }
        }
    }

    scroll_win->SetSizer(grid_sizer);

    if (need_scroll) {
        int row_height = COLOR_BTN_SIZE.GetHeight() + FromDIP(2);
        int col_width = COLOR_BTN_SIZE.GetWidth() + FromDIP(4);

        // Reserve space for vertical scrollbar so it doesn't overlay content
        int scrollbar_width = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);

        // Set minimum visible area (including scrollbar width)
        scroll_win->SetMinSize(wxSize(col_width * COLS + scrollbar_width, row_height * MAX_VISIBLE_ROWS));

        // Let wxScrolledWindow calculate appropriate virtual size
        scroll_win->FitInside();
        scroll_win->SetScrollRate(0, row_height);
    } else {
        scroll_win->FitInside();
        scroll_win->SetScrollRate(0, 0);
    }

    return scroll_win;
}

void FilamentPickerDialog::UpdatePreview(const FilamentColorCode& color_code)
{
    FilamentColor fila_color = color_code.GetFilaColor();

    std::vector<wxColour> wx_colors(fila_color.m_colors.begin(), fila_color.m_colors.end());

    // Update preview bitmap
    wxBitmap bmp = create_filament_bitmap(wx_colors, COLOR_DEMO_SIZE,
                                        fila_color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR);

    if (bmp.IsOk()) {
        BOOST_LOG_TRIVIAL(debug) << "Bitmap created successfully: " << bmp.GetWidth() << "x" << bmp.GetHeight();
        m_color_demo->SetBitmap(bmp);
        if (!wx_colors.empty()) {
            m_color_demo->SetBackgroundColour(wx_colors[0]);
        }
        m_color_demo->Refresh();
    } else {
        BOOST_LOG_TRIVIAL(error) << "Failed to create bitmap";
    }

    // Update preview labels
    m_label_preview_color->SetLabel(color_code.GetFilaColorName());
    m_label_preview_idx->SetLabel(wxString::Format("(%s)", color_code.GetFilaColorCode()));
    Layout();
}

void FilamentPickerDialog::UpdateCustomColorPreview(const wxColour& custom_color)
{
    std::vector<wxColour> wx_colors = {custom_color};

    // Update preview bitmap
    wxBitmap bmp = create_filament_bitmap(wx_colors, COLOR_DEMO_SIZE, false);

    if (bmp.IsOk()) {
        BOOST_LOG_TRIVIAL(debug) << "Custom color bitmap created successfully: " << bmp.GetWidth() << "x" << bmp.GetHeight();
        m_color_demo->SetBitmap(bmp);
        m_color_demo->SetBackgroundColour(custom_color);
        m_color_demo->Refresh();
    } else {
        BOOST_LOG_TRIVIAL(error) << "Failed to create custom color bitmap";
    }

    // Update preview labels for custom color
    m_label_preview_color->SetLabel(custom_color.GetAsString(wxC2S_HTML_SYNTAX));
    m_label_preview_idx->SetLabel("");
    Layout();
}

void FilamentPickerDialog::UpdateButtonStates(wxBitmapButton* selected_btn)
{
    // Reset selected button appearance
    if (m_cur_selected_btn) {
        m_cur_selected_btn->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        m_cur_selected_btn->Unbind(wxEVT_PAINT, &FilamentPickerDialog::OnButtonPaint, this);
        m_cur_selected_btn->Refresh();
    }

    if (selected_btn) {
        // Bind paint event to draw custom green border
        selected_btn->Bind(wxEVT_PAINT, &FilamentPickerDialog::OnButtonPaint, this);
        selected_btn->Refresh();
    }

    m_cur_selected_btn = selected_btn;
}

void FilamentPickerDialog::CreateMoreInfoButton()
{
    m_more_btn = new Button(this, "+ " + _L("More Colors"));
    m_more_btn->SetMinSize(wxSize(-1, FromDIP(36)));

    StateColor btn_bg(
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(248, 248, 248), StateColor::Normal)
    );


    m_more_btn->SetBackgroundColor(btn_bg);
    m_more_btn->SetBorderStyle(wxPENSTYLE_SHORT_DASH);
    m_more_btn->SetCornerRadius(FromDIP(0));
}

wxColourData FilamentPickerDialog::GetSingleColorData()
{
    wxColourData data;
    data.SetChooseFull(true);
    if (m_cur_filament_color.ColorCount() > 0) {
        data.SetColour(*m_cur_filament_color.m_colors.begin());
    }
    return data;
}

void FilamentPickerDialog::BindEvents()
{
    // Bind mouse events for window dragging
    Bind(wxEVT_LEFT_DOWN, &FilamentPickerDialog::OnMouseLeftDown, this);
    Bind(wxEVT_MOTION, &FilamentPickerDialog::OnMouseMove, this);
    Bind(wxEVT_LEFT_UP, &FilamentPickerDialog::OnMouseLeftUp, this);

    // Essential window events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (HasCapture()) {
            ReleaseMouse();
        }
        event.Skip();
    });

    // Bind more colors button event
    if (m_more_btn) {
        m_more_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // Pause click detection while color picker is open
            StopClickDetection();

            wxColourData original_data = GetSingleColorData();
            wxColourData result = show_sys_picker_dialog(this, original_data);

            // Resume click detection after color picker closes
            StartClickDetection();

            // Check if user actually selected a different color
            if (result.GetColour() != original_data.GetColour()) {
                wxColour selected_color = result.GetColour();

                // Update m_current_filament_color with the selected color
                m_cur_filament_color.m_colors.clear();
                m_cur_filament_color.m_colors.insert(selected_color);
                m_cur_filament_color.m_color_type = FilamentColor::ColorType::SINGLE_CLR;

                // Update preview
                UpdateCustomColorPreview(selected_color);

                // Clear currently selected button since custom color selected
                UpdateButtonStates(nullptr);
            }
        });
    }

    // Bind OK button event
    if (m_ok_btn) {
        m_ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_OK);
        });
    }

    // Bind Cancel button event
    if (m_cancel_btn) {
        m_cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_CANCEL);
        });
    }
}

#ifdef __WXGTK__
void FilamentPickerDialog::OnWindowCreate(wxWindowCreateEvent& event)
{
    // GTK platform needs to wait for window creation
    SetWindowShape();
}
#endif

void FilamentPickerDialog::OnMouseLeftDown(wxMouseEvent& event)
{
    // Only allow dragging from empty areas (not from buttons or other controls)
    wxWindow* hit_window = wxFindWindowAtPoint(ClientToScreen(event.GetPosition()));
    if (hit_window && hit_window != this) {
        // Click was on a child control, don't drag
        event.Skip();
        return;
    }

    // Release any existing capture first
    if (HasCapture()) {
        ReleaseMouse();
    }

    CaptureMouse();
    wxPoint pt = ClientToScreen(event.GetPosition());
    wxPoint origin = GetPosition();
    int dx = pt.x - origin.x;
    int dy = pt.y - origin.y;
    m_drag_delta = wxPoint(dx, dy);

    // Don't skip the event for dragging to work properly
}

void FilamentPickerDialog::OnMouseMove(wxMouseEvent& event)
{
    wxPoint pt = event.GetPosition();

    if (event.Dragging() && event.LeftIsDown() && HasCapture()) {
        wxPoint pos = ClientToScreen(pt);
        Move(wxPoint(pos.x - m_drag_delta.x, pos.y - m_drag_delta.y));
    }

    event.Skip();
}

void FilamentPickerDialog::OnMouseLeftUp(wxMouseEvent& event)
{
    if (HasCapture()) {
        ReleaseMouse();
    }

    event.Skip();
}

void FilamentPickerDialog::OnButtonPaint(wxPaintEvent& event)
{
    wxWindow* button = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (!button) {
        event.Skip();
        return;
    }

    // Create paint DC and let default painting happen first
    wxPaintDC dc(button);

    //Clear the button with white background
    dc.SetBrush(wxBrush(*wxTRANSPARENT_BRUSH));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, COLOR_BTN_SIZE.GetWidth(), COLOR_BTN_SIZE.GetHeight());

    // Draw the bitmap in the center
    wxBitmapButton* bmpBtn = dynamic_cast<wxBitmapButton*>(button);
    if (bmpBtn && bmpBtn->GetBitmap().IsOk()) {
        wxBitmap bmp = bmpBtn->GetBitmap();
        int x = (COLOR_BTN_SIZE.GetWidth() - COLOR_BTN_BITMAP_SIZE.GetWidth()) / 2;
        int y = (COLOR_BTN_SIZE.GetHeight() - COLOR_BTN_BITMAP_SIZE.GetHeight()) / 2;
        dc.DrawBitmap(bmp, x, y, true);
    }

    // Draw the green border
    dc.SetPen(wxPen(wxColour("#009688"), 2));  // Green pen, 2px thick
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(1, 1, COLOR_BTN_SIZE.GetWidth() - 1, COLOR_BTN_SIZE.GetHeight() - 1);
}

bool FilamentPickerDialog::IsClickOnTopMostWindow(const wxPoint& mouse_pos)
{
    wxWindow* main_window = wxGetApp().mainframe;
    if (!main_window) {
        return false;
    }

    wxRect main_rect = main_window->GetScreenRect();
    bool in_main_app = main_rect.Contains(mouse_pos);

    if (!in_main_app) {
        return false;
    }

#ifdef _WIN32
    // Windows: Use WindowFromPoint to check actual topmost window
    POINT pt = {mouse_pos.x, mouse_pos.y};
    HWND hwnd_at_point = WindowFromPoint(pt);
    HWND main_hwnd = (HWND)main_window->GetHandle();

    // Check if clicked window belongs to our main window hierarchy
    HWND parent_hwnd = hwnd_at_point;
    while (parent_hwnd != NULL) {
        if (parent_hwnd == main_hwnd) {
            return true;
        }
        parent_hwnd = ::GetParent(parent_hwnd);
    }
    return false;

#elif defined(__WXOSX__)
    // macOS: Use focus and active window check
    return (wxGetActiveWindow() == main_window) || main_window->HasFocus();

#else
    // Linux: Use focus check (similar to macOS)
    return (wxGetActiveWindow() == main_window) || main_window->HasFocus();
#endif
}

void FilamentPickerDialog::StartClickDetection()
{
    if (m_click_timer) {
        StopClickDetection();
    }

    m_click_timer = new wxTimer(this, wxID_ANY);
    Bind(wxEVT_TIMER, &FilamentPickerDialog::OnTimerCheck, this, m_click_timer->GetId());
    m_click_timer->Start(50);
}

void FilamentPickerDialog::StopClickDetection()
{
    if (m_click_timer) {
        m_click_timer->Stop();
        delete m_click_timer;
        m_click_timer = nullptr;
    }
}

void FilamentPickerDialog::CleanupTimers()
{
    StopClickDetection();

    if (m_flash_timer) {
        m_flash_timer->Stop();
        delete m_flash_timer;
        m_flash_timer = nullptr;
    }
}

// Only perform complex detection when the mouse state actually changes
void FilamentPickerDialog::OnTimerCheck(wxTimerEvent& event)
{
    static wxPoint last_mouse_pos(-1, -1);
    wxPoint current_pos = wxGetMousePosition();

    // If the mouse position and button state haven't changed, skip the detection
    if (current_pos == last_mouse_pos &&
        wxGetMouseState().LeftIsDown() == m_last_mouse_down) {
        return;
    }

    last_mouse_pos = current_pos;

    wxPoint mouse_pos = wxGetMousePosition();
    wxRect window_rect = GetScreenRect();

    // Check if mouse button state changed
    bool mouse_down = wxGetMouseState().LeftIsDown();

    if (mouse_down != m_last_mouse_down) {
        if (mouse_down) {
            bool in_dialog = window_rect.Contains(mouse_pos);
            bool is_valid_click = IsClickOnTopMostWindow(mouse_pos);

            if (is_valid_click && !in_dialog) {
                StartFlashing();
            }
        }

        m_last_mouse_down = mouse_down;
    }
}

void FilamentPickerDialog::OnFlashTimer(wxTimerEvent& event)
{
    // 5 flashes = 10 steps (semi-transparent -> opaque for each flash)
    if (m_flash_step < 10) {
        if (m_flash_step % 2 == 0) {
            // Even steps: semi-transparent
            SetTransparent(120);
        } else {
            // Odd steps: opaque
            SetTransparent(255);
        }

        m_flash_step++;
    } else {
        // Complete flash sequence
        SetTransparent(255);
        m_flash_timer->Stop();
        delete m_flash_timer;
        m_flash_timer = nullptr;
    }
}

}} // namespace Slic3r::GUI

