#include "DragDropPanel.hpp"
#include "Widgets/Label.hpp"
#include <slic3r/GUI/wxExtensions.hpp>

namespace Slic3r { namespace GUI {

struct CustomData
{
    int filament_id;
    unsigned char r, g, b, a;
    char type[64];
};


wxColor Hex2Color(const std::string& str)
{
    if (str.empty() || (str.length() != 9 && str.length() != 7) || str[0] != '#')
        throw std::invalid_argument("Invalid hex color format");

    auto hexToByte = [](const std::string& hex)->unsigned char
        {
            unsigned int byte;
            std::istringstream(hex) >> std::hex >> byte;
            return static_cast<unsigned char>(byte);
        };
    auto r = hexToByte(str.substr(1, 2));
    auto g = hexToByte(str.substr(3, 2));
    auto b = hexToByte(str.substr(5, 2));
    unsigned char a = 255;
    if (str.size() == 9)
        a = hexToByte(str.substr(7, 2));
    return wxColor(r, g, b, a);
}

// Custom data object used to store information that needs to be backed up during drag and drop
class ColorDataObject : public wxCustomDataObject
{
public:
    ColorDataObject(const wxColour &color = *wxBLACK, int filament_id = 0, const std::string &type = "PLA")
        : wxCustomDataObject(wxDataFormat("application/customize_format"))
    {
        set_custom_data_filament_id(filament_id);
        set_custom_data_color(color);
        set_custom_data_type(type);
    }

    wxColour GetColor() const { return wxColor(m_data.r, m_data.g, m_data.b, m_data.a); }
    void     SetColor(const wxColour &color) { set_custom_data_color(color); }

    int      GetFilament() const { return m_data.filament_id; }
    void     SetFilament(int label) { set_custom_data_filament_id(label); }

    std::string     GetType() const { return m_data.type; }
    void     SetType(const std::string &type) { set_custom_data_type(type); }

    void set_custom_data_type(const std::string& type) {
        std::strncpy(m_data.type, type.c_str(), sizeof(m_data.type) - 1);
        m_data.type[sizeof(m_data.type) - 1] = '\0';
    }

    void set_custom_data_filament_id(int filament_id) {
        m_data.filament_id = filament_id;
    }

    void set_custom_data_color(const wxColor& color) {
        m_data.r           = color.Red();
        m_data.g           = color.Green();
        m_data.b           = color.Blue();
        m_data.a           = color.Alpha();
    }

    virtual size_t GetDataSize() const override { return sizeof(m_data); }
    virtual bool   GetDataHere(void *buf) const override
    {
        char *ptr = static_cast<char *>(buf);
        std::memcpy(buf, &m_data, sizeof(m_data));
        return true;
    }
    virtual bool SetData(size_t len, const void *buf) override
    {
        if (len == GetDataSize()) {
            std::memcpy(&m_data, buf, sizeof(m_data));
            return true;
        }
        return false;
    }
private:
    CustomData m_data;
};

///////////////   ColorPanel  start ////////////////////////
// The UI panel of drag item
ColorPanel::ColorPanel(DragDropPanel *parent, const wxColour &color, int filament_id, const std::string &type)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(32, 40), wxBORDER_NONE), m_parent(parent), m_color(color), m_filament_id(filament_id), m_type(type)
{
    Bind(wxEVT_LEFT_DOWN, &ColorPanel::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &ColorPanel::OnLeftUp, this);
    Bind(wxEVT_PAINT, &ColorPanel::OnPaint, this);
}

void ColorPanel::OnLeftDown(wxMouseEvent &event)
{
    m_parent->set_is_draging(true);
    m_parent->DoDragDrop(this, GetColor(), GetType(), GetFilamentId());
}

void ColorPanel::OnLeftUp(wxMouseEvent &event) { m_parent->set_is_draging(false); }

void ColorPanel::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    wxSize   size  = GetSize();
    // If it matches the parent's width, it will not be displayed completely
    int svg_size = size.GetWidth();
    int type_label_height = FromDIP(10);
    wxString type_label(m_type);
    int type_label_margin = FromDIP(3);

    std::string replace_color = m_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::string svg_name = "outlined_rect";
    if (replace_color == "#FFFFFF00") {
        svg_name = "outlined_rect_transparent";
    }
    static Slic3r::GUI::BitmapCache cache;
    wxBitmap* bmp = cache.load_svg(svg_name, 0, svg_size, false, false, replace_color, 0.f);
    //wxBitmap bmp = ScalableBitmap(this, svg_name, svg_size, false, false, false, { replace_color }).bmp();
    // ScalableBitmap is not drawn at position (0, 0) by default, why?
    dc.DrawBitmap(*bmp, wxPoint(0,0));

    //dc.SetPen(wxPen(*wxBLACK, 1));
    //dc.DrawRectangle(0, 0, svg_size, svg_size);

    wxString label = wxString::Format(wxT("%d"), m_filament_id);
    dc.SetTextForeground(m_color.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);  // set text color
    dc.DrawLabel(label, wxRect(0, 0, svg_size, svg_size), wxALIGN_CENTER);

    if(m_parent)
        dc.SetTextForeground(this->m_parent->GetBackgroundColour().GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    else
        dc.SetTextForeground(*wxBLACK);
    if (type_label.length() > 4) {
        // text is too long
        wxString first = type_label.Mid(0, 4);
        wxString rest = type_label.Mid(4);
        dc.DrawLabel(first, wxRect(0, svg_size + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
        dc.DrawLabel(rest, wxRect(0, svg_size + type_label_height + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
    }else {
        dc.DrawLabel(type_label, wxRect(0, svg_size + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
    }
}
///////////////   ColorPanel  end ////////////////////////


// Save the source object information to m_data when dragging
class ColorDropSource : public wxDropSource
{
public:
    ColorDropSource(wxPanel *parent, wxPanel *color_block, const wxColour &color, const std::string& type, int filament_id) : wxDropSource(parent)
    {
        m_data.SetColor(color);
        m_data.SetFilament(filament_id);
        m_data.SetType(type);
        SetData(m_data);  // Set drag source data
    }

private:
    ColorDataObject m_data;
};

///////////////   ColorDropTarget  start ////////////////////////
// Get the data from the drag source when drop it
class ColorDropTarget : public wxDropTarget
{
public:
    ColorDropTarget(DragDropPanel *panel) : wxDropTarget(/*new wxDataObjectComposite*/), m_panel(panel)
    {
        m_data = new ColorDataObject();
        SetDataObject(m_data);
    }

    virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override;
    virtual bool         OnDrop(wxCoord x, wxCoord y) override {
        return true;
    }

private:
    DragDropPanel * m_panel;
    ColorDataObject* m_data;
};

wxDragResult ColorDropTarget::OnData(wxCoord x, wxCoord y, wxDragResult def)
{
    if (!GetData())
        return wxDragNone;

    ColorDataObject *dataObject = dynamic_cast<ColorDataObject *>(GetDataObject());
    m_panel->AddColorBlock(m_data->GetColor(), m_data->GetType(), m_data->GetFilament());

    return wxDragCopy;
}
///////////////   ColorDropTarget  end ////////////////////////


DragDropPanel::DragDropPanel(wxWindow *parent, const wxString &label, bool is_auto)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_is_auto(is_auto)
{
    SetBackgroundColour(0xF8F8F8);

    m_sizer    = new wxBoxSizer(wxVERTICAL);

    auto title_panel = new wxPanel(this);
    title_panel->SetBackgroundColour(0xEEEEEE);
    auto title_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_panel->SetSizer(title_sizer);

    Label* static_text = new Label(this, label);
    static_text->SetFont(Label::Head_13);
    static_text->SetBackgroundColour(0xEEEEEE);

    title_sizer->Add(static_text, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    m_sizer->Add(title_panel, 0, wxEXPAND);
    m_sizer->AddSpacer(10);

    m_grid_item_sizer = new wxGridSizer(0, 6, FromDIP(8), FromDIP(8));   // row = 0, col = 3,  10 10 is space
    m_sizer->Add(m_grid_item_sizer, 1, wxEXPAND | wxALL, FromDIP(8));

    // set droptarget
    auto drop_target = new ColorDropTarget(this);
    SetDropTarget(drop_target);

    SetSizer(m_sizer);
    Layout();
    Fit();
}

void DragDropPanel::AddColorBlock(const wxColour &color, const std::string &type, int filament_id, bool update_ui)
{
    ColorPanel *panel = new ColorPanel(this, color, filament_id, type);
    panel->SetMinSize(wxSize(FromDIP(30), FromDIP(60)));
    m_grid_item_sizer->Add(panel, 0);
    m_filament_blocks.push_back(panel);
    if (update_ui) {
        m_filament_blocks.front()->Refresh();  // FIX BUG: STUDIO-8467
        GetParent()->GetParent()->Layout();
        GetParent()->GetParent()->Fit();
    }
}

void DragDropPanel::RemoveColorBlock(ColorPanel *panel, bool update_ui)
{
    m_sizer->Detach(panel);
    panel->Destroy();
    m_filament_blocks.erase(std::remove(m_filament_blocks.begin(), m_filament_blocks.end(), panel), m_filament_blocks.end());
    if (update_ui) {
        GetParent()->GetParent()->Layout();
        GetParent()->GetParent()->Fit();
    }
}

void DragDropPanel::DoDragDrop(ColorPanel *panel, const wxColour &color, const std::string &type, int filament_id)
{
    if (m_is_auto)
        return;

    ColorDropSource source(this, panel, color, type, filament_id);
    if (source.DoDragDrop(wxDrag_CopyOnly) == wxDragResult::wxDragCopy) {
        this->RemoveColorBlock(panel);
    }
}

std::vector<int> DragDropPanel::GetAllFilaments() const
{
    std::vector<int>          filaments;
    for (size_t i = 0; i < m_grid_item_sizer->GetItemCount(); ++i) {
        wxSizerItem *item = m_grid_item_sizer->GetItem(i);
        if (item != nullptr) {
            wxWindow *  window     = item->GetWindow();
            ColorPanel *colorPanel = dynamic_cast<ColorPanel *>(window);
            if (colorPanel != nullptr) {
                filaments.push_back(colorPanel->GetFilamentId());
            }
        }
    }

    return filaments;
}

}} // namespace Slic3r::GUI
