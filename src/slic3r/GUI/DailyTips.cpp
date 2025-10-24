#include "DailyTips.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace Slic3r { namespace GUI {


struct DailyTipsData {
    std::string main_text;
    std::string wiki_url;
    std::string img_url;
    std::string additional_text;                  // currently not used
    std::string hyper_text;                       // currently not used
    std::function<void(void)> hypertext_callback; // currently not used
};

class DailyTipsDataRenderer {
public:
    DailyTipsDataRenderer(DailyTipsLayout layout);
    ~DailyTipsDataRenderer();
    void update_data(const DailyTipsData& data);
    void render(const ImVec2& pos, const ImVec2& size) const;
    bool has_image() const;
    void on_change_color_mode(bool is_dark);
    void set_fade_opacity(float opacity);

protected:
    void load_texture_from_img_url(const std::string url);
    void open_wiki() const;
    // relative to the window's upper-left position
    void render_img(const ImVec2& start_pos, const ImVec2& size) const;
    void render_text(const ImVec2& start_pos, const ImVec2& size) const;

private:
    DailyTipsData m_data;
    GLTexture* m_texture{ nullptr };
    GLTexture* m_placeholder_texture{ nullptr };
    bool m_is_dark{ false };
    DailyTipsLayout m_layout;
    float m_fade_opacity{ 1.0f };
};

DailyTipsDataRenderer::DailyTipsDataRenderer(DailyTipsLayout layout)
    : m_layout(layout)
{
}

DailyTipsDataRenderer::~DailyTipsDataRenderer() {
    if (m_texture)
        delete m_texture;
    if (m_placeholder_texture)
        delete m_placeholder_texture;
}

void DailyTipsDataRenderer::update_data(const DailyTipsData& data)
{
    m_data = data;
    load_texture_from_img_url(m_data.img_url);
}

void DailyTipsDataRenderer::load_texture_from_img_url(const std::string url)
{
    if (m_texture) {
        delete m_texture;
        m_texture = nullptr;
    }
    if (!url.empty()) {
        m_texture = new GLTexture();
        m_texture->load_from_file(Slic3r::resources_dir() + "/" + url, true, GLTexture::None, false);
    }
    else {
        if (!m_placeholder_texture) {
            m_placeholder_texture = new GLTexture();
            m_placeholder_texture->load_from_file(Slic3r::resources_dir() + "/images/dailytips_placeholder.png", true, GLTexture::None, false);
        }
    }
}

void DailyTipsDataRenderer::open_wiki() const
{
    if (!m_data.wiki_url.empty())
    {
        wxGetApp().open_browser_with_warning_dialog(m_data.wiki_url);
    }
}

void DailyTipsDataRenderer::render(const ImVec2& pos, const ImVec2& size) const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
    int window_flags = parent_window->Flags;
    window_flags &= ~ImGuiWindowFlags_NoScrollbar;
    window_flags &= ~ImGuiWindowFlags_NoScrollWithMouse;
    std::string name = "##DailyTipsDataRenderer" + std::to_string(parent_window->ID);
    ImGui::SetNextWindowPos(pos);
    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
        if (m_layout == DailyTipsLayout::Vertical) {
            ImVec2 img_size(0, 0);
            float img_text_gap = 0.0f;
            if (has_image()) {
                img_size = ImVec2(size.x, 9.0f / 16.0f * size.x);
                render_img({0, 0}, img_size);
                img_text_gap = ImGui::CalcTextSize("A").y;
            }
            render_text({ 0, img_size.y + img_text_gap }, size);
        }
        if (m_layout == DailyTipsLayout::Horizontal) {
            ImVec2 img_size(0, 0);
            float  img_text_gap = 0.0f;
            if (has_image()) {
                img_size = ImVec2(16.0f / 9.0f * size.y, size.y);
                render_img({0, 0}, img_size);
                img_text_gap = ImGui::CalcTextSize("A").y;
            }
            render_text({img_size.x + img_text_gap, 0}, {size.x - img_size.x - img_text_gap, size.y});
        }
    }
    ImGui::EndChild();
}

bool DailyTipsDataRenderer::has_image() const
{
    return !m_data.img_url.empty();
}

void DailyTipsDataRenderer::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
}

void DailyTipsDataRenderer::set_fade_opacity(float opacity)
{
    m_fade_opacity = opacity;
}

void DailyTipsDataRenderer::render_img(const ImVec2& start_pos, const ImVec2& size) const
{
    if (has_image())
        ImGui::Image((ImTextureID)(intptr_t)m_texture->get_id(), size, ImVec2(0, 0), ImVec2(1, 1), m_is_dark ? ImVec4(0.8, 0.8, 0.8, m_fade_opacity) : ImVec4(1, 1, 1, m_fade_opacity));
    // else {
    //     ImGui::Image((ImTextureID)(intptr_t)m_placeholder_texture->get_id(), size, ImVec2(0, 0), ImVec2(1, 1), m_is_dark ? ImVec4(0.8, 0.8, 0.8, m_fade_opacity) : ImVec4(1, 1, 1, m_fade_opacity));
    // }
}

bool has_cjk(const std::string &text)
{
    for (size_t i = 0; i < text.size() - 2; ++i) {
        unsigned char c1 = text[i];
        unsigned char c2 = text[i + 1];
        unsigned char c3 = text[i + 2];

        if ((c1 & 0xF0) == 0xE0 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
            int codepoint = ((c1 & 0x0F) << 12 | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
            if (codepoint >= 0x3000 && codepoint <= 0x9FFF) { return true; }
        }
    }
    return false;
}

void DailyTipsDataRenderer::render_text(const ImVec2& start_pos, const ImVec2& size) const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();

    ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.88f * m_fade_opacity) : ImVec4(38 / 255.0f, 46 / 255.0f, 48 / 255.0f, m_fade_opacity));
    // main text
    // first line is headline (for hint notification it must be divided by \n)
    std::string title_line;
    std::string content_lines;
    size_t end_pos = m_data.main_text.find_first_of('\n');
    if (end_pos != std::string::npos) {
        title_line = m_data.main_text.substr(0, end_pos);
        title_line = ImGui::ColorMarkerStart + title_line + ImGui::ColorMarkerEnd;
        content_lines = m_data.main_text.substr(end_pos + 1);
    }

    ImGui::SetCursorPos(start_pos);
    imgui.text(title_line);
    
    bool is_zh = has_cjk(content_lines);
    if (!is_zh) {
        // problem in Chinese with spaces
        ImGui::SetCursorPosX(start_pos.x);
        imgui.text_wrapped(content_lines, size.x);
    }
    else {
        Label* wrapped_text = new Label(wxGetApp().GetTopWindow());
        wrapped_text->Hide();
        wrapped_text->SetLabelText(wxString::FromUTF8(content_lines));
        float wrap_width = size.x + ImGui::CalcTextSize("A").x * 5.0f;
#ifdef __APPLE__
        wrap_width /= 2.0f;
#endif
        wrapped_text->Wrap(wrap_width);
        std::string wrapped_content_lines = wrapped_text->GetLabel().ToUTF8().data();
        wrapped_text->Destroy();
        ImGui::SetCursorPosX(start_pos.x);
        imgui.text(wrapped_content_lines);
    }

    // wiki
    if (!m_data.wiki_url.empty()) {
        std::string tips_line = _u8L("For more information, please check out Wiki");
        std::string wiki_part_text = _u8L("Wiki");
        std::string first_part_text = tips_line.substr(0, tips_line.find(wiki_part_text));
        ImVec2 wiki_part_size = ImGui::CalcTextSize(wiki_part_text.c_str());
        ImVec2 first_part_size = ImGui::CalcTextSize(first_part_text.c_str());

        //text
        ImGui::SetCursorPosX(start_pos.x);
        ImVec2 link_start_pos = ImGui::GetCursorScreenPos();
        imgui.text(first_part_text);

        ImColor HyperColor = ImColor(31, 142, 234, (int)(255 * m_fade_opacity)).Value;
        ImVec2 wiki_part_rect_min = ImVec2(link_start_pos.x + first_part_size.x, link_start_pos.y);
        ImVec2 wiki_part_rect_max = wiki_part_rect_min + wiki_part_size;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        ImGui::SetCursorScreenPos(wiki_part_rect_min);
        imgui.text(wiki_part_text.c_str());
        ImGui::PopStyleColor();

        //click behavior
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true))
        {
            //underline
            ImVec2 lineEnd = ImGui::GetItemRectMax();
            lineEnd.y -= 2.0f;
            ImVec2 lineStart = lineEnd;
            lineStart.x = ImGui::GetItemRectMin().x;
            ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                open_wiki();
        }
    }
    ImGui::PopStyleColor();
}

int DailyTipsPanel::uid = 0;

DailyTipsPanel::DailyTipsPanel(bool can_expand, DailyTipsLayout layout)
    : m_pos(ImVec2(0, 0)),
    m_width(0),
    m_height(0),
    m_can_expand(can_expand),
    m_layout(layout),
    m_uid(DailyTipsPanel::uid++),
    m_dailytips_renderer(std::make_unique<DailyTipsDataRenderer>(layout))
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    m_footer_height = 30.0f * scale;
    m_is_expanded = wxGetApp().app_config->get("show_hints") == "true";
}

void DailyTipsPanel::set_position(const ImVec2& pos)
{
    m_pos = pos;
}

void DailyTipsPanel::set_size(const ImVec2& size)
{
    m_width = size.x;
    m_height = size.y;
    m_content_height = m_height - m_footer_height;
}

void DailyTipsPanel::set_can_expand(bool can_expand)
{
    m_can_expand = can_expand;
}

ImVec2 DailyTipsPanel::get_size()
{
    return ImVec2(m_width, m_height);
}

void DailyTipsPanel::render()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;

    if (!m_first_enter) {
        retrieve_data_from_hint_database(HintDataNavigation::Curr);
        m_first_enter = true;
    }

    push_styles();
    if (m_can_expand) {
        if (m_is_expanded) {
            m_height = m_content_height + m_footer_height;
        }
        else {
            m_height = m_footer_height;
        }
    }
    ImGui::SetNextWindowPos(m_pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(m_width, m_height), ImVec2(m_width, m_height));
    int window_flags = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild((std::string("##DailyTipsPanel") + std::to_string(m_uid)).c_str(), ImVec2(m_width, m_height), false, window_flags)) {
        if (m_can_expand) {
            if (m_is_expanded) {
                m_dailytips_renderer->render({ m_pos.x, m_pos.y }, { m_width, m_content_height });
                render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
            }
            else {
                render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
            }
        }
        else {
            m_dailytips_renderer->render({ m_pos.x, m_pos.y }, { m_width, m_content_height });
            render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
        }

        //{// for debug
        //    ImVec2 vMin = ImGui::GetWindowContentRegionMin();
        //    ImVec2 vMax = ImGui::GetWindowContentRegionMax();
        //    vMin += ImGui::GetWindowPos();
        //    vMax += ImGui::GetWindowPos();
        //    ImGui::GetForegroundDrawList()->AddRect(vMin, vMax, IM_COL32(180, 180, 255, 255));
        //}
    }
    ImGui::EndChild();
    pop_styles();
}

void DailyTipsPanel::retrieve_data_from_hint_database(HintDataNavigation nav)
{
    HintData* hint_data = HintDatabase::get_instance().get_hint(nav);
    if (hint_data != nullptr)
    {
        DailyTipsData data{ hint_data->text,
                            hint_data->documentation_link,
                            hint_data->image_url,
                            hint_data->follow_text,
                            hint_data->hypertext,
                            hint_data->callback
        };
        m_dailytips_renderer->update_data(data);
    }
}

void DailyTipsPanel::expand(bool expand)
{
    if (!m_can_expand)
        return;
    m_is_expanded = expand;
    wxGetApp().app_config->set_bool("show_hints", expand);
}

void DailyTipsPanel::collapse()
{
    if (!m_can_expand)
        return;
    m_is_expanded = false;
    wxGetApp().app_config->set_bool("show_hints", false);
}

bool DailyTipsPanel::is_expanded()
{
    return m_is_expanded;
}

void DailyTipsPanel::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
    m_dailytips_renderer->on_change_color_mode(is_dark);
}

void DailyTipsPanel::set_fade_opacity(float opacity)
{
    m_fade_opacity = opacity;
    m_dailytips_renderer->set_fade_opacity(opacity);
}

//void DailyTipsPanel::render_header(const ImVec2& pos, const ImVec2& size)
//{
//    ImGuiWrapper& imgui = *wxGetApp().imgui();
//    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
//    int window_flags = parent_window->Flags;
//    std::string name = "##DailyTipsPanelHeader" + std::to_string(parent_window->ID);
//    ImGui::SetNextWindowPos(pos);
//    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
//        ImVec2 text_pos = pos + ImVec2(0, (size.y - ImGui::CalcTextSize("A").y) / 2);
//        ImGui::SetCursorScreenPos(text_pos);
//        imgui.push_bold_font();
//        imgui.text(_u8L("Daily Tips"));
//        imgui.pop_bold_font();
//    }
//    ImGui::EndChild();
//}

void DailyTipsPanel::render_controller_buttons(const ImVec2& pos, const ImVec2& size)
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
    int window_flags = parent_window->Flags;
    std::string name = "##DailyTipsPanelControllers" + std::to_string(parent_window->ID);
    ImGui::SetNextWindowPos(pos);
    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
        ImVec2 button_size = ImVec2(38.0f, 38.0f) * scale;
        float button_margin_x = 8.0f * scale;
        std::wstring button_text;

        // collapse / expand
        ImVec2 btn_pos = pos + ImVec2(0, (size.y - ImGui::CalcTextSize("A").y) / 2);
        ImGui::SetCursorScreenPos(btn_pos);
        if (m_can_expand) {
            if (m_is_expanded) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImColor(144, 144, 144, (int)(255 * m_fade_opacity)).Value);

                button_text = ImGui::CollapseArrowIcon;
                imgui.button((_L("Collapse") + button_text));
                ImVec2 collapse_btn_size = ImGui::CalcTextSize((_u8L("Collapse")).c_str());
                collapse_btn_size.x += button_size.x / 2.0f;
                if (ImGui::IsMouseHoveringRect(btn_pos, btn_pos + collapse_btn_size, true))
                {
                    //underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.x -= ImGui::CalcTextSize("A").x / 2;
                    lineEnd.y -= 2;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, ImColor(144, 144, 144));

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        collapse();
                }

                ImGui::PopStyleColor(4);
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImColor(230, 230, 230, (int)(255 * m_fade_opacity)).Value : ImColor(38, 46, 48, (int)(255 * m_fade_opacity)).Value);

                // for bold font text, split text and icon-font button
                imgui.push_bold_font();
                imgui.button((_L("Daily Tips")));
                imgui.pop_bold_font();
                ImVec2 expand_btn_size = ImGui::CalcTextSize((_u8L("Daily Tips")).c_str());
                ImGui::SetCursorScreenPos(ImVec2(btn_pos.x + expand_btn_size.x + ImGui::CalcTextSize(" ").x, btn_pos.y));
                button_text = ImGui::ExpandArrowIcon;
                imgui.button(button_text.c_str());
                expand_btn_size.x += 19.0f * scale;
                if (ImGui::IsMouseHoveringRect(btn_pos, btn_pos + expand_btn_size, true))
                {
                    //underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.x -= ImGui::CalcTextSize("A").x / 2;
                    lineEnd.y -= 2;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x - expand_btn_size.x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, m_is_dark ? ImColor(230, 230, 230, (int)(255 * m_fade_opacity)) : ImColor(38, 46, 48, (int)(255 * m_fade_opacity)));

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        expand();
                }
                
                ImGui::PopStyleColor(4);

                ImGui::EndChild();
                return;
            }
        }

        // page index
        m_page_index = HintDatabase::get_instance().get_index() + 1;
        m_pages_count = HintDatabase::get_instance().get_count();
        std::string text_str = std::to_string(m_page_index) + "/" + std::to_string(m_pages_count);
        float text_item_width = ImGui::CalcTextSize(text_str.c_str()).x;
        ImGui::PushItemWidth(text_item_width);
        float text_pos_x = (pos + size).x - button_margin_x * 2 - button_size.x * 2 - text_item_width;
        float text_pos_y = pos.y + (size.y - ImGui::CalcTextSize("A").y) / 2;
        ImGui::SetCursorScreenPos(ImVec2(text_pos_x, text_pos_y));
        ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImColor(230, 230, 230, (int)(255 * m_fade_opacity)).Value : ImColor(38, 46, 48, (int)(255 * m_fade_opacity)).Value);
        imgui.text(text_str);
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.0f, .0f, .0f, .0f));

        // prev button
        ImColor button_text_color = m_is_dark ? ImColor(228, 228, 228, (int)(255 * m_fade_opacity)) : ImColor(38, 46, 48, (int)(255 * m_fade_opacity));
        ImVec2 prev_button_pos = pos + size + ImVec2(-button_margin_x - button_size.x * 2, -size.y + (size.y - button_size.y) / 2);
        ImGui::SetCursorScreenPos(prev_button_pos);
        button_text = ImGui::PrevArrowBtnIcon;
        if (ImGui::IsMouseHoveringRect(prev_button_pos, prev_button_pos + button_size, true))
        {
            button_text_color = ImColor(0, 150, 136, (int)(255 * m_fade_opacity));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                retrieve_data_from_hint_database(HintDataNavigation::Prev);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, button_text_color.Value);// for icon-font button
        imgui.button(button_text.c_str());
        ImGui::PopStyleColor();

        // next button
        button_text_color = m_is_dark ? ImColor(228, 228, 228, (int)(255 * m_fade_opacity)) : ImColor(38, 46, 48, (int)(255 * m_fade_opacity));
        ImVec2 next_button_pos = pos + size + ImVec2(-button_size.x, -size.y + (size.y - button_size.y) / 2);
        ImGui::SetCursorScreenPos(next_button_pos);
        button_text = ImGui::NextArrowBtnIcon;
        if (ImGui::IsMouseHoveringRect(next_button_pos, next_button_pos + button_size, true))
        {
            button_text_color = ImColor(0, 150, 136, (int)(255 * m_fade_opacity));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                retrieve_data_from_hint_database(HintDataNavigation::Next);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, button_text_color.Value);// for icon-font button
        imgui.button(button_text.c_str());
        ImGui::PopStyleColor();

        ImGui::PopStyleColor(4);
    }
    ImGui::EndChild();
}

void DailyTipsPanel::push_styles()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    imgui.push_common_window_style(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // framePadding cannot be zero. Otherwise, there is a problem with icon font button display
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, m_is_dark ? ImGuiWrapper::COL_WINDOW_BG_DARK : ImGuiWrapper::COL_WINDOW_BG);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
}

void DailyTipsPanel::pop_styles()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.pop_common_window_style();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(4);
}


DailyTipsWindow::DailyTipsWindow()
{
    m_panel = new DailyTipsPanel(false, DailyTipsLayout::Vertical);
}

void DailyTipsWindow::open()
{
    m_show = true;
    m_panel->retrieve_data_from_hint_database(HintDataNavigation::Curr);
}

void DailyTipsWindow::close()
{
    m_show = false;
}

void DailyTipsWindow::render()
{
    if (!m_show)
        return;
    //if (m_show)
    //    ImGui::OpenPopup((_u8L("Daily Tips")).c_str());

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;

    const Size& cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
    ImVec2 center = ImVec2(cnv_size.get_width() * 0.5f, cnv_size.get_height() * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImVec2 padding = ImVec2(25, 25) * scale;
    imgui.push_menu_style(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7) * scale);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, m_is_dark ? ImVec4(54 / 255.0f, 54 / 255.0f, 60 / 255.0f, 1.00f) : ImVec4(245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1.00f));
    ImGui::GetCurrentContext()->DimBgRatio = 1.0f;
    int windows_flag =
        ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;
    imgui.push_bold_font();
    //if (ImGui::BeginPopupModal((_u8L("Daily Tips")).c_str(), NULL, windows_flag))
    if (ImGui::Begin((_u8L("Daily Tips")).c_str(), &m_show, windows_flag))
    {
        imgui.pop_bold_font();

        ImVec2 panel_pos = ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin();
        ImVec2 panel_size = ImVec2(400.0f, 435.0f) * scale;
        m_panel->set_position(panel_pos);
        m_panel->set_size(panel_size);
        m_panel->render();

        if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
            m_show = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::End();
        //ImGui::EndPopup();
    }
    else {
        imgui.pop_bold_font();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
    imgui.pop_menu_style();
}

void DailyTipsWindow::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
    m_panel->on_change_color_mode(is_dark);
}

}}