//**********************************************************/
/* File: uiAMSBestPositionPopup.hpp
*  Description: The popup with suggest best ams position
*
//**********************************************************/

#include "uiAMSBestPositionPopup.hpp"

#include "slic3r/Utils/WxFontUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"


#include <wx/dcgraph.h>
#include <wx/grid.h>

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(wxEVT_REFRESH_DATA, wxCommandEvent);

UiStyledAMSPanel::UiStyledAMSPanel(wxWindow* parent,
                         wxWindowID id,
                         const wxPoint& pos,
                         const wxSize& size,
                         const wxColour& borderColor,
                         const wxColour& bgColor,
                         bool borderDashed,
                         wxString name,
                         bool isTop)
                         : wxPanel(parent, id, pos, size),
                            m_borderDashed(borderDashed),
                            m_borderColor(borderColor),
                            m_bgColor(bgColor),
                            m_name(name),
                            m_isTop(isTop)
{
    SetDoubleBuffered(true);
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT, &UiStyledAMSPanel::OnPaint, this);
}


void UiStyledAMSPanel::OnPaint(wxPaintEvent& event)
{

    wxPaintDC dc(this);
    dc.Clear();
    m_borderWidth = FromDIP(2);
    m_radius = FromDIP(5);
    wxSize clientSize = GetClientSize();
    int width = clientSize.GetWidth();
    int height = clientSize.GetHeight();


    dc.SetBrush(wxBrush(m_bgColor));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRectangle(wxRect(0, 0, width, height));
    // dc.DrawRoundedRectangle(wxRect(0, 0, width, height), m_radius);


    wxPen borderPen;
    if (m_borderDashed)
    {

        borderPen = wxPen(m_borderColor, m_borderWidth, wxPENSTYLE_SHORT_DASH);
    }
    else
    {

        borderPen = wxPen(m_borderColor, m_borderWidth, wxPENSTYLE_SOLID);
    }
    dc.SetPen(borderPen);

    int offset = m_borderWidth;
    dc.DrawRoundedRectangle(wxRect(offset, offset, width - 2 * offset, height - 2 * offset), m_radius);


    int labelX = offset;
    int labelY = m_isTop ? offset : (height - offset - FromDIP(27));  // true=top, false=bottom


    wxRect labelRect(labelX, labelY, width - 2 * offset, FromDIP(10));
    wxRect labelRoundRect(labelX, labelY, width - 2 * offset, FromDIP(27));
    dc.SetBrush(wxBrush(m_borderColor));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRoundedRectangle(labelRoundRect, m_radius);
    dc.DrawRectangle(labelRect);


    wxFont font12(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    dc.SetTextForeground(wxColour("#858585"));
    // dc.SetFont(font12);
    dc.SetFont(::Label::Body_12);
    wxCoord textWidth, textHeight;
    dc.GetTextExtent(m_name, &textWidth, &textHeight);

    int textPosX = labelX + 4 * offset;
    int textPosY = labelY + (FromDIP(27) -textHeight ) / 2;
    dc.DrawText(m_name, textPosX, textPosY);


    // event.Skip();
}

UiStyledSwitchPanel::UiStyledSwitchPanel(wxWindow* parent,
                                    wxWindowID id = wxID_ANY,
                                    const wxPoint& pos = wxDefaultPosition,
                                    const wxSize& size = wxDefaultSize,
                                    const wxColour& borderColor = wxColour("#EEEEEE"),
                                    const wxColour& bgColor = wxColour("#FFFFFF"),
                                    bool borderDashed = true,
                                    int borderWidth = 2,
                                    int radius = 1,
                                    bool isTop = true)
                                    : wxPanel(parent, id, pos, size),
                                        m_borderDashed(borderDashed),
                                        m_borderWidth(borderWidth),
                                        m_radius(radius),
                                        m_borderColor(borderColor),
                                        m_bgColor(bgColor),
                                        m_isTop(isTop)
{

    SetDoubleBuffered(true);
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT, &UiStyledSwitchPanel::OnPaint, this);



    m_mainSizer = new wxBoxSizer(wxVERTICAL);


    m_mainSizer->AddSpacer(labelHeight);


    m_contentSizer = new wxBoxSizer(wxVERTICAL);


    m_splitSizer = new wxBoxSizer(wxHORIZONTAL);


    m_leftSizer = new wxBoxSizer(wxVERTICAL);
    m_splitSizer->Add(m_leftSizer, 1, wxEXPAND | wxALL, 0);


    m_rightSizer = new wxBoxSizer(wxVERTICAL);
    m_splitSizer->Add(m_rightSizer, 1, wxEXPAND | wxALL, 0);


    m_contentSizer->Add(m_splitSizer, 1, wxEXPAND | wxALL, 0);


    m_mainSizer->Add(m_contentSizer, 1, wxEXPAND | wxALL, FromDIP(1));

    SetSizer(m_mainSizer);
}

void UiStyledSwitchPanel::Clear(bool deleteWindows)
{
    if (m_leftSizer)
    {
        m_leftSizer->Clear(deleteWindows);
        m_leftSizer->Layout();
    }

    if (m_rightSizer)
    {
        m_rightSizer->Clear(deleteWindows);
        m_rightSizer->Layout();
    }

    LayoutAndFit();

    Refresh();
    Update();
}

void UiStyledSwitchPanel::AddToLeft(wxWindow* window, int proportion, int flag, int border)
{
    m_leftSizer->Add(window, proportion, flag, border);
    Layout();
}

void UiStyledSwitchPanel::AddToRight(wxWindow* window, int proportion, int flag, int border)
{
    m_rightSizer->Add(window, proportion, flag, border);
    Layout();
}


void UiStyledSwitchPanel::OnPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    dc.Clear();
    wxSize clientSize = GetClientSize();
    int width = clientSize.GetWidth();
    int height = clientSize.GetHeight() - FromDIP(90); // for selector space

    dc.SetBrush(wxBrush(m_bgColor));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRectangle(wxRect(0, 0, width, height + FromDIP(90)));

    dc.SetBrush(wxBrush(m_bgColor));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRoundedRectangle(wxRect(0, 0, width, height), m_radius);

    wxPen borderPen;
    if (m_borderDashed)
    {
        borderPen = wxPen(wxColour("#ACACAC"), m_borderWidth, wxPENSTYLE_SHORT_DASH);
    }
    else
    {
        borderPen = wxPen(wxColour("#ACACAC"), m_borderWidth, wxPENSTYLE_SOLID);
    }
    dc.SetBrush(wxBrush(*wxTRANSPARENT_BRUSH));
    dc.SetPen(borderPen);
    // dc.DrawRoundedRectangle(wxRect(offset, offset, width - 2 * offset, height - 2 * offset), m_radius);
    dc.DrawRoundedRectangle(wxRect(FromDIP(1), FromDIP(1), width - FromDIP(1), height), m_radius);

    int offset = m_borderWidth;
    int labelX = offset;
    int labelY = m_isTop ? offset : (height - offset - labelHeight);  // true=top, false=bottom
    labelX += FromDIP(1);
    labelY += FromDIP(1);

    wxRect labelRoundRect(labelX, labelY, width - 2 * offset - FromDIP(2), FromDIP(labelHeight));
    wxRect labelRect(labelX, labelY + m_radius, width - 2 * offset - FromDIP(2), FromDIP(labelHeight) - m_radius);
    dc.SetBrush(wxBrush(m_borderColor));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRoundedRectangle(labelRoundRect, m_radius);
    dc.DrawRectangle(labelRect);

    // dc.SetPen(wxPen(m_borderColor, m_borderWidth, wxPENSTYLE_SHORT_DASH));
    dc.SetPen(wxPen(wxColour("#ACACAC"), m_borderWidth, wxPENSTYLE_SHORT_DASH));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawLine(width / 2, offset, width / 2, height - offset);

    wxFont font12(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    dc.SetTextForeground(wxColour("#000000"));
    // dc.SetFont(font12);
    dc.SetFont(::Label::Body_14);
    wxCoord textWidth, textHeight;
    dc.GetTextExtent(_L("Filament Inlet A"), &textWidth, &textHeight);

    int aTextPosX = labelX + 4 * offset;
    int aTextPosY = labelY + (labelHeight - textHeight ) / 2;
    dc.DrawText(_L("Filament Inlet A"), aTextPosX, aTextPosY);

    int bTextPosX = labelX + 4 * offset + width / 2;
    int bTextPosY = aTextPosY;
    dc.DrawText(_L("Filament Inlet B"), bTextPosX, bTextPosY);

    height += FromDIP(90); //total height

    dc.SetBrush(wxBrush(m_bgColor));
    dc.SetPen(wxPen(m_borderColor, m_borderWidth, wxPENSTYLE_SOLID));
    wxCoord selTextWidth, selTextHeight;
    dc.GetTextExtent(_L("Filament Track Switch"), &selTextWidth, &selTextHeight);

    int selWidth = selTextWidth + FromDIP(20);
    int selHeight = FromDIP(44);
    int selBaseX = (width - selWidth) / 2;
    int selBaseY = height - FromDIP(56);
    dc.DrawRoundedRectangle(wxRect(selBaseX, selBaseY, selWidth, selHeight), FromDIP(4));
    // wxFont font(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    dc.SetTextForeground(wxColour("#262E30"));
    // dc.SetFont(font);
    int selTextPosX = selBaseX + (selWidth - selTextWidth) / 2;
    int selTextPosY = selBaseY + (selHeight - selTextHeight) / 2;
    dc.DrawText(_L("Filament Track Switch"), selTextPosX, selTextPosY);

    //draw Horizontal
    int hLBaseX = (width - FromDIP(184)) / 2;
    int hLBaseY = selBaseY - FromDIP(17);
    int hWidth = FromDIP(76);
    int hHeight = FromDIP(5);
    int hGap = FromDIP(32);
    wxRect HL(hLBaseX, hLBaseY + FromDIP(1), hWidth, hHeight);
    wxRect HR(hLBaseX + hWidth + hGap, hLBaseY + FromDIP(1), hWidth, hHeight);
    dc.SetBrush(wxBrush(wxColour("#D9D9D9")));
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.DrawRectangle(HL);
    dc.DrawRectangle(HR);
    //draw vertical
    int vLTopBaseX = hLBaseX;
    int vLTopBaseY = height - FromDIP(90);
    int vWidht = FromDIP(5);
    int vHeight = FromDIP(23);
    // vtopL vtopR
    dc.DrawRectangle(wxRect(vLTopBaseX, vLTopBaseY, vWidht, vHeight));
    dc.DrawRectangle(wxRect(vLTopBaseX + FromDIP(184) - vWidht, vLTopBaseY, vWidht, vHeight));

    // vmidL vmidR
    dc.DrawRectangle(wxRect(vLTopBaseX + hWidth - vWidht, vLTopBaseY + vHeight - vWidht, vWidht, FromDIP(17)));
    dc.DrawRectangle(wxRect(vLTopBaseX + hWidth + hGap, vLTopBaseY + vHeight - vWidht, vWidht, FromDIP(17)));

    //vbotL vbotR
    dc.DrawRectangle(wxRect(vLTopBaseX + hWidth - vWidht, height - FromDIP(12), vWidht,  FromDIP(12)));
    dc.DrawRectangle(wxRect(vLTopBaseX + hWidth + hGap, height -  FromDIP(12), vWidht,  FromDIP(12)));
    // event.Skip();
}

UiAMSSlot::UiAMSSlot(wxWindow* parent,
                 const std::vector<wxColour>& bgColours,
                 const wxString& text,
                 DataStatusType status,
                 wxWindowID id,
                 const wxPoint& pos,
                 const wxSize& size,
                 double colourFactor,
                 double scaleFactor)
    : wxPanel(parent, id, pos, wxDefaultSize, wxBORDER_NONE),
      m_bgColours(bgColours), m_text(text), m_status(status), m_size(size), m_colourFactor(colourFactor), m_scaleFactor(scaleFactor)
{
    SetDoubleBuffered(true);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_ams_slot_readonly = new ScalableBitmap(this, "ams_readonly", 10 * m_scaleFactor); // Orca: reuse the existing readonly glyph

    m_size = wxSize(
        static_cast<int>(size.GetWidth() * scaleFactor),
        static_cast<int>(size.GetHeight() * scaleFactor)
    );
    SetMinSize(m_size);
    // SetMaxSize(m_size);
    Bind(wxEVT_PAINT, &UiAMSSlot::OnPaint, this);
}

void UiAMSSlot::DrawRectangle(wxPaintDC& dc, const wxSize& cli)
{

    if (!m_bgColours.empty())
    {
        auto baseX = (cli.x - rectangleW) / 2;
        auto step = rectangleW / m_bgColours.size();

        for (int i = 0; i < m_bgColours.size(); i++)
        {
            wxRect rr(baseX + step * i, (cli.y - rectangleH) / 2, step, rectangleH);

            dc.SetBrush(wxBrush(LightenColour(m_bgColours[i])));
            dc.SetPen(wxPen(LightenColour(m_bgColours[i])));
            dc.DrawRectangle(rr);
        }
    }

    wxString line1 = m_text.BeforeFirst('\n');
    wxString line2 = m_text.AfterFirst ('\n');
    wxFont font12 = ::Label::Body_12;
    wxFont font14 = ::Label::Body_14;
    int newSizeFont12 = static_cast<int>(font12.GetPointSize() * m_scaleFactor);
    int newSizeFont14 = static_cast<int>(font14.GetPointSize() * m_scaleFactor);
    newSizeFont12 = std::max(1, newSizeFont12);
    newSizeFont14 = std::max(1, newSizeFont14);
    font12.SetPointSize(newSizeFont12);
    font14.SetPointSize(newSizeFont14);
    wxColour textColour = wxColour("#000000");
    if (!m_bgColours.empty())
    {
        textColour = IsDark(m_bgColours.front()) ? wxColour("#FFFFFF")
                                               : wxColour("#000000");
    }

    dc.SetTextForeground(textColour);

    dc.SetFont(font12);
    // dc.SetFont(::Label::Body_12);
    wxCoord w1, h1;
    dc.GetTextExtent(_L(line1), &w1, &h1);

    dc.SetFont(font14);
    // dc.SetFont(::Label::Body_14);
    wxCoord w2, h2;
    dc.GetTextExtent(_L(line2), &w2, &h2);

    int maxLine2Width = rectangleW - FromDIP(2);
    if (w2 > maxLine2Width && newSizeFont14 > 1)
    {
        int currentFontSize = newSizeFont14;
        while (currentFontSize > 1)
        {
            currentFontSize--;
            font14.SetPointSize(currentFontSize);
            dc.SetFont(font14);
            dc.GetTextExtent(_L(line2), &w2, &h2);
            if (w2 <= maxLine2Width)
            {
                break;
            }
        }
    }

    int topGap = static_cast<int>(FromDIP(5) * m_scaleFactor);
    int textGap = static_cast<int>(FromDIP(5) * m_scaleFactor);
    int bmpH = m_ams_slot_readonly->GetBmpHeight();
    int bmpW = m_ams_slot_readonly->GetBmpWidth();
    int baseY = (cli.y - h1 - h2 - topGap - textGap - bmpH - textGap) / 2;

    dc.SetFont(font12);
    // dc.SetFont(::Label::Body_12);
    dc.DrawText(_L(line1), (cli.x - w1) / 2, baseY + topGap);
    dc.SetFont(font14);
    // dc.SetFont(::Label::Body_14);
    dc.DrawText(_L(line2), (cli.x - w2) / 2, baseY + topGap + h1 + textGap);

    //draw svg
    if (line2 != "Empty")
    {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(*wxWHITE);
        dc.DrawBitmap(m_ams_slot_readonly->bmp(), wxPoint((cli.x - bmpW) / 2, baseY + topGap + h1 + textGap + h2 + textGap));
    }
}

void UiAMSSlot::DrawLine(wxPaintDC& dc, const wxSize& cli)
{
    // no scaled
    int bgRemainPointX = (cli.x - FromDIP(30)) / 2;
    int bgRemainPointY = 0;
    int bgRemainH = FromDIP(4);
    int bgRemainW = FromDIP(30);
    int forRemainW = FromDIP(30 - 8);
    //remain status bar
    dc.SetBrush(wxBrush(wxColour("#c2c2c2")));
    dc.SetPen(wxPen(wxColour("#c2c2c2")));
    dc.DrawRoundedRectangle(wxRect(bgRemainPointX, 0, bgRemainW, bgRemainH), FromDIP(3));

    if (!m_bgColours.empty()) {
        if (m_bgColours.size() == 1) {
            dc.SetBrush(wxBrush(LightenColour(m_bgColours[0])));
            dc.SetPen(wxPen(LightenColour(m_bgColours[0])));
            dc.DrawRoundedRectangle(wxRect(bgRemainPointX, bgRemainPointY, forRemainW, bgRemainH), FromDIP(3));
        } else {
            auto step = bgRemainH / m_bgColours.size();
            for (int i = 0; i < m_bgColours.size(); i++) {
                dc.SetBrush(wxBrush(LightenColour(m_bgColours[i])));
                dc.SetPen(wxPen(LightenColour(m_bgColours[i])));
                dc.DrawRoundedRectangle(wxRect(bgRemainPointX, bgRemainPointY + step * i, forRemainW, bgRemainH / m_bgColours.size()), FromDIP(3));
            }
        }
    }

    if (m_status == DataStatusType::ADJUST)
    {
        dc.SetBrush(wxColour("#ff6f00"));
        dc.SetPen(wxPen(wxColour("#ff6f00")));
        //top
        int topGap = static_cast<int>(FromDIP(8) * m_scaleFactor);
        int bottomGap = static_cast<int>(FromDIP(4) * m_scaleFactor);
        int tbHeight = static_cast<int>(FromDIP(3) * m_scaleFactor);
        dc.DrawRectangle(wxRect((cli.x - rectangleW) / 2, (cli.y - rectangleH - topGap) / 2, rectangleW, tbHeight));
        //bottom
        dc.DrawRectangle(wxRect((cli.x - rectangleW) / 2, (cli.y - rectangleH - topGap) / 2 + rectangleH + bottomGap, rectangleW, tbHeight));

        // left
        int radius = static_cast<int>(FromDIP(4) * m_scaleFactor);
        int width = static_cast<int>(FromDIP(10) * m_scaleFactor);
        int height = static_cast<int>(cli.y - FromDIP(3));
        dc.DrawRoundedRectangle(wxRect(0, FromDIP(static_cast<int>(3 * m_scaleFactor)), width, height), radius);
        //right
        // dc.DrawRectangle(wxRect(55, 7, 3, 78));
        dc.DrawRoundedRectangle(wxRect(static_cast<int>(cli.x - FromDIP(10) * m_scaleFactor),
                                static_cast<int>(FromDIP(3) * m_scaleFactor),
                                static_cast<int>(FromDIP(10) * m_scaleFactor),
                                static_cast<int>(cli.y - FromDIP(3))), radius);
    }

    if (m_status != DataStatusType::UNMATCHED)
    {
        dc.SetBrush(wxColour(144, 144, 144));
        dc.SetPen(wxPen(wxColour(144, 144, 144)));
        dc.DrawRoundedRectangle(wxRect(FromDIP(static_cast<int>(3 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(6 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(4 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(81 * m_scaleFactor))), FromDIP(static_cast<int>(2 * m_scaleFactor)));
        dc.DrawRoundedRectangle(wxRect(FromDIP(static_cast<int>(51 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(6 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(4 * m_scaleFactor)),
                                    FromDIP(static_cast<int>(81 * m_scaleFactor))), FromDIP(static_cast<int>(2 * m_scaleFactor)));
    }

}

wxColour UiAMSSlot::LightenColour(const wxColour& original)
{
    if (std::abs(m_colourFactor - 1.0) < 1e-3)
    {
        return original;
    }
    m_colourFactor = std::clamp(m_colourFactor, 0.0, 1.0);

    int r = original.Red();
    int g = original.Green();
    int b = original.Blue();
    int a = original.Alpha();

    r = static_cast<int>(r + (255 - r) * (1 - m_colourFactor));
    g = static_cast<int>(g + (255 - g) * (1 - m_colourFactor));
    b = static_cast<int>(b + (255 - b) * (1 - m_colourFactor));

    r = std::min(r, 255);
    g = std::min(g, 255);
    b = std::min(b, 255);

    return wxColour(r, g, b, a);
}

void UiAMSSlot::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    dc.Clear();
    //draw all background
    rectangleW = FromDIP(m_rectangleW);
    rectangleH = FromDIP(m_rectangleH);
    rectangleW = static_cast<int>(rectangleW * m_scaleFactor);
    rectangleH = static_cast<int>(rectangleH * m_scaleFactor);
    wxSize cli = GetClientSize();
    dc.SetBrush(wxBrush(wxColour("#ffffff")));
    dc.SetPen(wxPen(wxColour("#ffffff")));
    if (m_scaleFactor < 1.0)
    {
        dc.SetBrush(wxBrush(wxColour("#F8F8F8")));
        dc.SetPen(wxPen(wxColour("#F8F8F8")));
    }
    dc.DrawRectangle(wxRect(0, 0, cli.x, cli.y));
    DrawLine(dc, cli);
    DrawRectangle(dc, cli);
}


UiAMS::UiAMS( wxWindow* parent,
         const std::vector<DataAmsSlotInfo>& amsInfo,
         wxWindowID id = wxID_ANY,
         const wxPoint& pos = wxDefaultPosition,
         const wxSize& minSize = wxDefaultSize)
    : UiStyledAMSPanel(parent, id, pos, wxDefaultSize, wxColour("#dbdbdb"), wxColour("#ffffff"), false, amsInfo.front().amsName),

     m_amsInfo(amsInfo), m_minSize(minSize)
{
    if (m_amsInfo.size() == 1)
    {
        wxSize size(FromDIP(92), FromDIP(134));
        SetMinSize(size);
        m_minSize = size;
    }
    else // 4 slot
    {
        wxSize size(FromDIP(262), FromDIP(134));
        SetMinSize(size);
        m_minSize = size;
    }
    init();
}

void UiAMS::init()
{
    // int minW =  m_minSize.GetWidth() * m_amsInfo.size();
    int minW =  m_minSize.GetWidth();
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    //ams model
    // wxPanel* amsPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition);

    wxBoxSizer* amsPanelSizer = new wxBoxSizer(wxHORIZONTAL);
    wxSize wxSlotSize = wxSize(FromDIP(58), FromDIP(90));
    int allEmptySpacer = minW - m_amsInfo.size() * wxSlotSize.GetWidth();
    int gap = (m_amsInfo.size() == 1 ? allEmptySpacer / 2 : allEmptySpacer / ( 2 * m_amsInfo.size() - 1));
    for (const auto& slot : m_amsInfo)
    {
        amsPanelSizer->AddStretchSpacer(gap);
        amsPanelSizer->Add(new UiAMSSlot(this, slot.colours, slot.name, slot.status, wxID_ANY, wxDefaultPosition, wxSlotSize, slot.colourFactor, slot.scaleFactor));
        amsPanelSizer->AddStretchSpacer(gap);
    }
    // amsPanel->SetSizer(amsPanelSizer);
    // mainSizer->AddStretchSpacer();
    mainSizer->Add(amsPanelSizer, 0, wxALIGN_CENTER | wxEXPAND | wxALL, FromDIP(5));
    // mainSizer->AddStretchSpacer();
    SetSizer(mainSizer);
}

ReselectMachineDialog::ReselectMachineDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Suggested rearrangement"),
               wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(wxColour("#FFFFFF"));
    // SetSize(wxSize(FromDIP(630), FromDIP(719)));
    SetMinSize(wxSize(FromDIP(630), -1));
    SetMaxSize(wxSize(FromDIP(630), -1));

    mainSizer = new wxBoxSizer(wxVERTICAL);


    textPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    textSizer = new wxBoxSizer(wxVERTICAL);
    suggestText = new Label(textPanel, wxEmptyString);

    linkwiki = new Label(textPanel, _L("How to save time?→"));
    linkwiki->SetForegroundColour(wxColour("#009688")); // Orca: accent green
    linkwiki->SetBackgroundColour(wxColour("#FFFFFF"));
    linkwiki->SetFont(Label::Body_14);
    // Orca: no vendor wiki page for this feature; hide the placeholder link and drop the external URL.
    linkwiki->Hide();
    textSizer->Add(suggestText, 0, wxALIGN_LEFT | wxTOP, FromDIP(5));
    textSizer->Add(linkwiki, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));
    textPanel->SetSizer(textSizer);

    summaryText = new Label(this, wxEmptyString);
    summaryText->SetFont(Label::Body_14);
    // summaryText->SetFont(wxGetApp().normal_font());

    filamentSwitch = new UiStyledSwitchPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, //wxSize(FromDIP(571), FromDIP(439)),
                                                    wxColour("#EEEEEE"), wxColour("#ffffff"), true, FromDIP(1), FromDIP(5), true);


    filamentTips = new wxStaticText(this, wxID_ANY, _L("Filament Status:"));
    filamentTips->SetFont(wxGetApp().normal_font());

    // statusBar = new UiStatusContainer(this);
    statusBar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    statusBar->SetBackgroundColour(wxColour("#F8F8F8"));
    statusBar->SetMinSize(wxSize(FromDIP(570), -1));
    wxBoxSizer* statusBarSizer = new wxBoxSizer(wxHORIZONTAL);
    statusBar->SetSizer(statusBarSizer);

    wxBoxSizer* adjustGroupSizer = new wxBoxSizer(wxHORIZONTAL);
    UiAMSSlot* amsSlotAdjust= new UiAMSSlot(statusBar, colourAdjust, wxString("Ax\nPLA"), DataStatusType::ADJUST, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(58), FromDIP(90)), 1.0, 1.0);
    wxStaticText* adjustLabel = new wxStaticText(statusBar, wxID_ANY, _L("Position adjustment required"));
    adjustLabel->SetFont(wxGetApp().normal_font());
    adjustLabel->SetForegroundColour(wxColour("#6B6B6B"));

    adjustGroupSizer->Add(amsSlotAdjust, 0, wxALIGN_CENTER);
    adjustGroupSizer->Add(adjustLabel, 0, wxALIGN_CENTER | wxLEFT, FromDIP(8));

    wxBoxSizer* okGroupSizer = new wxBoxSizer(wxHORIZONTAL);
    UiAMSSlot* amsSlotOK= new UiAMSSlot(statusBar, colourOK, wxString("Ax\nPLA"), DataStatusType::OK, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(58), FromDIP(90)), 1.0, 1.0);
    wxStaticText* okLabel = new wxStaticText(statusBar, wxID_ANY, _CTX(L_CONTEXT("OK", "FilamentTrack"), "FilamentTrack"));
    okLabel->SetFont(wxGetApp().normal_font());
    okLabel->SetForegroundColour(wxColour("#6B6B6B"));

    okGroupSizer->Add(amsSlotOK, 0, wxALIGN_CENTER);
    okGroupSizer->Add(okLabel, 0, wxALIGN_CENTER | wxLEFT, FromDIP(8));

    wxBoxSizer* unusedGroupSizer = new wxBoxSizer(wxHORIZONTAL);
    UiAMSSlot* amsSlotUnused= new UiAMSSlot(statusBar, colourUnused, wxString("Ax\nPLA"), DataStatusType::UNMATCHED, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(58), FromDIP(90)), 0.2, 1.0);
    wxStaticText* unusedLabel = new wxStaticText(statusBar, wxID_ANY, _L("Unused"));
    unusedLabel->SetFont(wxGetApp().normal_font());
    unusedLabel->SetForegroundColour(wxColour("#6B6B6B"));

    unusedGroupSizer->Add(amsSlotUnused, 0, wxALIGN_CENTER);
    unusedGroupSizer->Add(unusedLabel, 0, wxALIGN_CENTER | wxLEFT, FromDIP(8));

    statusBarSizer->Add(adjustGroupSizer, 1, wxALIGN_CENTER | wxLEFT, FromDIP(10));
    statusBarSizer->Add(okGroupSizer, 1, wxALIGN_CENTER | wxLEFT, FromDIP(30));
    statusBarSizer->Add(unusedGroupSizer, 1, wxALIGN_CENTER | wxLEFT, FromDIP(40));

    statusBarSizer->AddStretchSpacer(1);

    btnSizer = new wxBoxSizer(wxHORIZONTAL);
    m_buttonRefresh = new Button(this, _L("Refresh"));
    m_buttonRefresh->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_buttonRefresh->SetMaxSize(wxSize(FromDIP(80), FromDIP(32)));
    m_buttonRefresh->Bind(wxEVT_BUTTON, &ReselectMachineDialog::OnRefreshButton, this);
    m_buttonRefresh->Hide();
    m_buttonClose = new Button(this, _L("Close"));
    m_buttonClose->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_buttonClose->SetMaxSize(wxSize(FromDIP(80), FromDIP(32)));
    m_buttonClose->SetBackgroundColor(wxColour("#009688")); // Orca: accent green
    m_buttonClose->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e){
        this->Close();
    });
    btnSizer->Add(m_buttonRefresh, 0, wxALIGN_LEFT);
    btnSizer->Add(m_buttonClose, 0, wxALIGN_LEFT | wxLEFT, FromDIP(10));
    //btn

    mainSizer->Add(0, FromDIP(20));
    mainSizer->Add(textPanel, 0, wxALIGN_LEFT | wxLEFT, FromDIP(26));
    mainSizer->Add(0, FromDIP(12));
    mainSizer->Add(summaryText, 0, wxALIGN_LEFT | wxLEFT | wxEXPAND, FromDIP(30));
    mainSizer->Add(0, FromDIP(15));
    mainSizer->Add(filamentSwitch, 0, wxALIGN_LEFT | wxLEFT, FromDIP(26));
    // mainSizer->Add(m_bitmapSelectMachine, 0,  wxALIGN_LEFT | wxLEFT, FromDIP(214));
    mainSizer->Add(0, FromDIP(8));
    mainSizer->Add(filamentTips, 0, wxALIGN_LEFT | wxLEFT, FromDIP(23));
    mainSizer->Add(0, FromDIP(7));
    mainSizer->Add(statusBar, 0, wxALIGN_LEFT | wxLEFT, FromDIP(23));
    mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, FromDIP(23));
    SetSizer(mainSizer);

    this->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
        m_buttonRefresh->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
        m_buttonRefresh->SetMaxSize(wxSize(FromDIP(80), FromDIP(32)));
        m_buttonClose->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
        m_buttonClose->SetMaxSize(wxSize(FromDIP(80), FromDIP(32)));
        Layout();
        Refresh();
        evt.Skip();
    });

    Fit();
    Layout();
    Centre();
}

void ReselectMachineDialog::Update(MachineObject* obj, const std::map<int, int>&  best_pos_map, const std::vector<FilamentInfo>& ams_mapping, wxString save_time)
{

    if (suggestText)
    {
        suggestText->SetLabel(wxString::Format(_L("Based on the diagram below, rearrange the filaments on the printer for optimal results to save approximately %s."), (save_time.empty() ? "0s" : save_time)));
        suggestText->Wrap(FromDIP(600));
        suggestText->SetBackgroundColour(wxColour("#FFFFFF"));
        suggestText->SetFont(Label::Body_14);
        suggestText->Show();
    }

    if (summaryText)
    {
        wxString summaryInfo;
        if (!save_time.empty()) {
            summaryInfo = wxString::Format(_L("Summary:"));

            for (const auto& pair : best_pos_map) {
                for (auto fila : ams_mapping) {
                    if (fila.id == pair.first) {
                        const auto& ams_opt = obj->GetFilaSystem()->GetAmsById(fila.ams_id);
                        if (ams_opt && ams_opt->GetSwitcherPos() != pair.second) {
                            summaryInfo += "\n";
                            const auto& recommned_pos = (pair.second == DevFilaSwitch::SwitchPos::POS_IN_A) ? _L("AMS on Filament Inlet A") : _L("AMS on Filament Inlet B");
                            summaryInfo += wxString::Format(_L("The No.%d slicing filament is matched to '%s'. Assigning it to '%s' can save printing time."),
                                                            pair.first + 1, getTrayID(obj, fila.ams_id, fila.slot_id), recommned_pos);
                        }

                        break;
                    }
                }
            }

            summaryInfo += "\n\n";
            summaryInfo += _L("*Tips: If you have moved the spool position on the machine, please close this page and re‑match the filament.");

        } else {
            summaryInfo = wxString::Format(_L("Summary: This is currently the most suitable position for placement."));
        }

        summaryText->SetLabel(summaryInfo);
        summaryText->Wrap(FromDIP(571));
        summaryText->Layout();
        summaryText->Fit();
    }

    auto switchHight = CaculateSwitcherDistribution(obj, best_pos_map, ams_mapping);
    wxSize switchSize;
    if (122 == switchHight) //32 + 90 fixed size
    {
        switchSize = wxSize(FromDIP(571), FromDIP(439));
    }
    else
    {
        switchSize = wxSize(FromDIP(571), FromDIP(switchHight));
    }
    if (filamentSwitch)
    {
        filamentSwitch->Clear(true);
        filamentSwitch->SetMinSize(switchSize);
        filamentSwitch->SetMaxSize(switchSize);
        filamentSwitch->SetSize(switchSize);
        std::sort(inAAMS.begin(), inAAMS.end(), [](const auto& a, const auto& b) {
            return a.size() > b.size();
        });
        std::sort(inBAMS.begin(), inBAMS.end(), [](const auto& a, const auto& b) {
            return a.size() > b.size();
        });

        auto addToSwitchFun = [this] (DevFilaSwitch::SwitchPos dir) {
            const auto& INAMS = (dir == DevFilaSwitch::SwitchPos::POS_IN_A ? inAAMS : inBAMS);
            for (size_t i = 0; i < INAMS.size(); ++i)
            {
                const auto& slots = INAMS[i];
                if (slots.size() == 4)
                {
                    UiAMS* ams = new UiAMS(this->filamentSwitch, slots, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(92), FromDIP(134)));
                    if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
                    {
                        this->filamentSwitch->AddToLeft(ams, 0, wxALL, FromDIP(10));
                        this->filamentSwitch->GetLeftSizer()->Add(0, FromDIP(20));

                    }
                    if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
                    {
                        this->filamentSwitch->AddToRight(ams, 0, wxALL, FromDIP(10));
                        this->filamentSwitch->GetRightSizer()->Add(0, FromDIP(20));
                    }
                }
                if (slots.size() == 1)
                {
                    if (i + 1 < INAMS.size())
                    {
                        const auto& slots2 = INAMS[i + 1];

                        wxPanel* pairPanel = new wxPanel(this->filamentSwitch);
                        wxBoxSizer* pairSizer = new wxBoxSizer(wxHORIZONTAL);

                        UiAMS* ams1 = new UiAMS(pairPanel, slots, wxID_ANY,
                                        wxDefaultPosition, wxSize(FromDIP(92), FromDIP(134)));
                        pairSizer->Add(ams1, 0, wxALL, FromDIP(5));

                        UiAMS* ams2 = new UiAMS(pairPanel, slots2, wxID_ANY,
                                        wxDefaultPosition, wxSize(FromDIP(92), FromDIP(134)));
                        pairSizer->Add(ams2, 0, wxALL, FromDIP(5));

                        pairPanel->SetSizer(pairSizer);
                        if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
                        {
                            this->filamentSwitch->AddToLeft(pairPanel, 0, wxALL, FromDIP(10));
                        }
                        if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
                        {
                            this->filamentSwitch->AddToRight(pairPanel, 0, wxALL, FromDIP(10));
                        }

                        ++i;
                    }
                    else
                    {
                        UiAMS* ams = new UiAMS(this->filamentSwitch, slots, wxID_ANY,
                                        wxDefaultPosition, wxSize(FromDIP(92), FromDIP(134)));
                        if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
                        {
                            this->filamentSwitch->AddToLeft(ams, 0, wxALL, FromDIP(10));
                        }
                        if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
                        {
                            this->filamentSwitch->AddToRight(ams, 0, wxALL, FromDIP(10));
                        }
                    }

                    if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
                    {
                        this->filamentSwitch->GetLeftSizer()->Add(0, FromDIP(20));
                    }
                    if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
                    {
                        this->filamentSwitch->GetRightSizer()->Add(0, FromDIP(20));
                    }
                }
            }
        };
        addToSwitchFun(DevFilaSwitch::SwitchPos::POS_IN_A);
        addToSwitchFun(DevFilaSwitch::SwitchPos::POS_IN_B);
        filamentSwitch->LayoutAndFit();
    }

    //FilamentInfo
    mainSizer->Layout();

    Layout();
    Fit();
    Refresh(true);
    // wxDialog::Update();
}

int ReselectMachineDialog::CaculateSwitcherDistribution(MachineObject* obj, const std::map<int, int>&  best_pos_map, const std::vector<FilamentInfo>& ams_mapping)
{
    if (!obj)
    {
        return 0;
    }

    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    std::map<DevFilaSwitch::SwitchPos, std::vector<AMSinfo>> ams_info; //all ams set
    std::map<DevFilaSwitch::SwitchPos, std::vector<wxString>> ams_DisplayName;
    int inAFourSlotNum = 0, inAOneSlotNum = 0;
    int inBFourSlotNum = 0, inBOneSlotNum = 0;
    for (auto ams = ams_list.begin(); ams != ams_list.end(); ams++) //not include extend ams
    {
        AMSinfo info;
        info.ams_id = ams->first;
        if (ams->second->IsExist() && info.parse_ams_info(obj, ams->second, obj->GetFilaSystem()->IsDetectRemainEnabled(), obj->is_support_ams_humidity))
        {
            auto pos = ams->second->GetSwitcherPos();
            auto amsName = ams->second->GetDisplayName();
            if (pos.has_value())
            {
                ams_info[pos.value()].push_back(info);
                ams_DisplayName[pos.value()].push_back(amsName);
                if (pos.value() == DevFilaSwitch::SwitchPos::POS_IN_A)
                {
                    ams->second->GetSlotCount() == 4 ? ++inAFourSlotNum : ++inAOneSlotNum;
                }
                if (pos.value() == DevFilaSwitch::SwitchPos::POS_IN_B)
                {
                    ams->second->GetSlotCount() == 4 ? ++inBFourSlotNum : ++inBOneSlotNum;
                }
            }
        }
    }
                                //ams id       //slot id
    using trayHelper = std::tuple<std::string, std::string>;
    std::vector<trayHelper> posOK, posAdjust;
    for (const auto& fila : ams_mapping)
    {
        auto it = std::find_if(best_pos_map.begin(), best_pos_map.end(), [&](const std::pair<int, int>& p){
            return fila.id == p.first;
        });
        if (it != best_pos_map.end())
        {
            posAdjust.push_back({fila.ams_id, fila.slot_id});
        }
        else
        {
            posOK.push_back({fila.ams_id, fila.slot_id});
        }
    }


    int inAHeight = (inAFourSlotNum + (inAOneSlotNum / 2 + inAOneSlotNum % 2)) * 150;   //150 one AMS height and spacer
    int inBHeight = (inBFourSlotNum + (inBOneSlotNum / 2 + inBOneSlotNum % 2)) * 150;

    auto caculateAMSDistribution = [this, obj] (std::map<DevFilaSwitch::SwitchPos, std::vector<AMSinfo>>& amsInfo,
                                           std::map<DevFilaSwitch::SwitchPos, std::vector<wxString>>& amsDisplayName,
                                           const std::vector<trayHelper>& posOK, const std::vector<trayHelper>& posAdjust,
                                           DevFilaSwitch::SwitchPos dir) {
        if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
        {
            this->inAAMS.clear();
        }
        if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
        {
            this->inBAMS.clear();
        }
        auto& AMSVector = amsInfo[dir];
        auto& AMSNameVector = amsDisplayName[dir];
        for (auto i = 0; i < AMSVector.size(); i++)
        {
            const auto& ams = AMSVector[i];
            std::vector<DataAmsSlotInfo> allSlot;
            for (auto j = 0; j < ams.cans.size(); j++)
            {
                const auto& can = ams.cans[j];
                auto id = getTrayID(obj, ams.ams_id, can.can_id);
                auto material = can.material_name;
                if (can.material_state == AMSCanType::AMS_CAN_TYPE_THIRDBRAND ||
                    can.material_state == AMSCanType::AMS_CAN_TYPE_BRAND ||
                    can.material_state == AMSCanType::AMS_CAN_TYPE_VIRTUAL)
                {
                    if (can.material_name.empty())
                    {
                        material = L("?");
                    }
                }
                if (can.material_state == AMSCanType::AMS_CAN_TYPE_EMPTY)
                {
                    material = "Empty";
                }

                auto itOK = std::find_if(posOK.begin(), posOK.end(), [&](const trayHelper& tray){
                    auto amsID = std::get<0>(tray);
                    auto slotID = std::get<1>(tray);
                    return amsID == ams.ams_id && slotID == can.can_id;
                });
                auto itAdjust = std::find_if(posAdjust.begin(), posAdjust.end(), [&](const trayHelper& tray){
                    auto amsID = std::get<0>(tray);
                    auto slotID = std::get<1>(tray);
                    return amsID == ams.ams_id && slotID == can.can_id;
                });
                double colourFactor = itOK != posOK.end() ? 1.0 : 0.2;
                DataStatusType status = DataStatusType::UNMATCHED;
                if (itOK != posOK.end())
                {
                    status = DataStatusType::OK;
                }
                if (itAdjust != posAdjust.end())
                {
                    status = DataStatusType::ADJUST;
                }
                if (can.material_cols.empty()) {
                    allSlot.push_back(DataAmsSlotInfo{ AMSNameVector[i], wxString(id + "\n" + material), {can.material_colour}, colourFactor, 1.0, status });
                } else {
                    allSlot.push_back(DataAmsSlotInfo{ AMSNameVector[i], wxString(id + "\n" + material), can.material_cols, colourFactor, 1.0, status });
                }
            }
            if (dir == DevFilaSwitch::SwitchPos::POS_IN_A)
            {
                this->inAAMS.push_back(allSlot);
            }
            if (dir == DevFilaSwitch::SwitchPos::POS_IN_B)
            {
                this->inBAMS.push_back(allSlot);
            }
        }
    };

    caculateAMSDistribution(ams_info, ams_DisplayName, posOK, posAdjust, DevFilaSwitch::SwitchPos::POS_IN_A);
    caculateAMSDistribution(ams_info, ams_DisplayName, posOK, posAdjust, DevFilaSwitch::SwitchPos::POS_IN_B);
    return std::max(inAHeight, inBHeight) + 32 + 90; //32 label height //184 * 90 selector  width height
}

wxString ReselectMachineDialog::getTrayID(MachineObject* obj, const std::string& amsID, const std::string& slotID)
{
    if (!amsID.empty() && !slotID.empty())
    {
        if (obj)
        {
            auto filaSys = obj->GetFilaSystem();
            if (filaSys)
            {
                DevAms* ams = filaSys->GetAmsById(amsID);
                if (!ams) { return ""; }
                int     ams_id_int  = atoi(amsID.c_str());
                int     slot_id_int = atoi(slotID.c_str());
                int     tray_id     = 0;
                if (ams->GetAmsType() == DevAmsType::AMS || ams->GetAmsType() == DevAmsType::AMS_LITE || ams->GetAmsType() == DevAmsType::N3F) {
                    tray_id = ams_id_int * 4 + slot_id_int;
                } else if (ams->GetAmsType() == DevAmsType::N3S) {
                    tray_id = ams_id_int + slot_id_int;
                } else if (ams->GetAmsType() == DevAmsType::EXT_SPOOL) {
                    tray_id = slot_id_int;
                }
                return wxGetApp().transition_tridid(tray_id);
            }
        }
    }
    return "";
}


void ReselectMachineDialog::OnRefreshButton(wxCommandEvent& event)
{
    // EndModal(wxID_CANCEL);
    wxCommandEvent evt(wxEVT_REFRESH_DATA, GetId());
    evt.SetEventObject(this);
    GetParent()->ProcessWindowEvent(evt);
}

ReselectMachineDialog::~ReselectMachineDialog()
{

}

} // namespace GUI

} // namespace Slic3r