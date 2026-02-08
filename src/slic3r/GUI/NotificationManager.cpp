#include "NotificationManager.hpp"

#include "HintNotification.hpp"
#include "SlicingProgressNotification.hpp"
#include "GUI.hpp"
#include "ImGuiWrapper.hpp"
#include "wxExtensions.hpp"
#include "ObjectDataViewModel.hpp"
#include "GUI_ObjectList.hpp"
#include "ParamsPanel.hpp"
#include "MainFrame.hpp"
#include "libslic3r/Config.hpp"
#include "format.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/bind/bind.hpp>
#include <boost/nowide/convert.hpp>

#include <iostream>

#include <wx/glcanvas.h>

#include "GUI_App.hpp"
#include "FilamentMapDialog.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

static constexpr float GAP_WIDTH = 10.0f;
static constexpr float SPACE_RIGHT_PANEL = 10.0f;
static constexpr float FADING_OUT_DURATION = 2.0f;
// Time in Miliseconds after next render when fading out is requested
static constexpr int   FADING_OUT_TIMEOUT = 100;

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_EJECT_DRIVE_NOTIFICAION_CLICKED, EjectDriveNotificationClickedEvent);
wxDEFINE_EVENT(EVT_EXPORT_GCODE_NOTIFICAION_CLICKED, ExportGcodeNotificationClickedEvent);
wxDEFINE_EVENT(EVT_PRESET_UPDATE_AVAILABLE_CLICKED, PresetUpdateAvailableClickedEvent);
wxDEFINE_EVENT(EVT_PRINTER_CONFIG_UPDATE_AVAILABLE_CLICKED, PrinterConfigUpdateAvailableClickedEvent);

namespace {
	/* // not used?
	ImFont* add_default_font(float pixel_size)
	{
		ImGuiIO& io = ImGui::GetIO();
		ImFontConfig config;
		config.SizePixels = pixel_size;
		config.OversampleH = config.OversampleV = 1;
		config.PixelSnapH = true;
		ImFont* font = io.Fonts->AddFontDefault(&config);
		return font;
	}
	*/
	inline void push_style_color(ImGuiCol idx, const ImVec4& col, bool fading_out, float current_fade_opacity)
	{
		if (fading_out)
			ImGui::PushStyleColor(idx, ImVec4(col.x, col.y, col.z, col.w * current_fade_opacity));
		else
			ImGui::PushStyleColor(idx, col);
	}

	void open_folder(const std::string& path)
	{
		// Code taken from desktop_open_datadir_folder()

		// Execute command to open a file explorer, platform dependent.
		// FIXME: The const_casts aren't needed in wxWidgets 3.1, remove them when we upgrade.

#ifdef _WIN32
		const wxString widepath = from_u8(path);
		const wchar_t* argv[] = { L"explorer", widepath.GetData(), nullptr };
		::wxExecute(const_cast<wchar_t**>(argv), wxEXEC_ASYNC, nullptr);
#elif __APPLE__
		const char* argv[] = { "open", path.data(), nullptr };
		::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr);
#else
		const char* argv[] = { "xdg-open", path.data(), nullptr };

		// Check if we're running in an AppImage container, if so, we need to remove AppImage's env vars,
		// because they may mess up the environment expected by the file manager.
		// Mostly this is about LD_LIBRARY_PATH, but we remove a few more too for good measure.
		if (wxGetEnv("APPIMAGE", nullptr)) {
			// We're running from AppImage
			wxEnvVariableHashMap env_vars;
			wxGetEnvMap(&env_vars);

			env_vars.erase("APPIMAGE");
			env_vars.erase("APPDIR");
			env_vars.erase("LD_LIBRARY_PATH");
			env_vars.erase("LD_PRELOAD");
			env_vars.erase("UNION_PRELOAD");

			wxExecuteEnv exec_env;
			exec_env.env = std::move(env_vars);

			wxString owd;
			if (wxGetEnv("OWD", &owd)) {
				// This is the original work directory from which the AppImage image was run,
				// set it as CWD for the child process:
				exec_env.cwd = std::move(owd);
			}

			::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, &exec_env);
		}
		else {
			// Looks like we're NOT running from AppImage, we'll make no changes to the environment.
			::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, nullptr);
		}
#endif
	}
}

#if 1
// Reuse ImGUI Windows.
int NotificationManager::NotificationIDProvider::allocate_id()
{
	int id;
	if (m_released_ids.empty())
		id = ++m_next_id;
	else {
		id = m_released_ids.back();
		m_released_ids.pop_back();
	}
	return id;
}
void NotificationManager::NotificationIDProvider::release_id(int id)
{
	m_released_ids.push_back(id);
}
#else
// Don't reuse ImGUI Windows, allocate a new ID every time.
int NotificationManager::NotificationIDProvider::allocate_id() { return ++ m_next_id; }
void NotificationManager::NotificationIDProvider::release_id(int) {}
#endif

//------PopNotification--------
NotificationManager::PopNotification::PopNotification(const NotificationData &n, NotificationIDProvider &id_provider, wxEvtHandler* evt_handler) :
	  m_data                (n)
	, m_id_provider   		(id_provider)
	, m_text1               (n.text1)
	, m_hypertext           (n.hypertext)
	, m_text2               (n.text2)
	, m_evt_handler         (evt_handler)
	, m_notification_start  (GLCanvas3D::timestamp_now())
{
    m_ErrorColor  = ImGuiWrapper::to_ImVec4(decode_color_to_float_array("#E14747")); // ORCA
    m_WarnColor   = ImGuiWrapper::to_ImVec4(decode_color_to_float_array("#F59B16")); // ORCA
    m_NormalColor = ImVec4(0, 0.588, 0.533, 1);

	m_CurrentColor = m_NormalColor;   //Default

	m_WindowBkgColor = ImVec4(1, 1, 1, 1);
    m_TextColor      = ImVec4(.2f, .2f, .2f, 1.0f);
    m_HyperTextColor = ImVec4(0, 0.588, 0.533, 1);
}

// We cannot call plater()->get_current_canvas3D() from constructor, so we do it here
void NotificationManager::PopNotification::ensure_ui_inited()
{
    if (!m_is_dark_inited) {
        m_is_dark        = wxGetApp().plater()->get_current_canvas3D()->get_dark_mode_status();
        m_is_dark_inited = true;
    }

	if (!m_WindowRadius_inited) {
        m_WindowRadius = 4.0f * wxGetApp().plater()->get_current_canvas3D()->get_scale();
        m_WindowRadius_inited = true;
	}
}

void NotificationManager::PopNotification::on_change_color_mode(bool is_dark)
{
    m_is_dark_inited = true;
	m_is_dark = is_dark;
}

void NotificationManager::PopNotification::set_delete_callback(DeleteCallback callback)
{
    m_on_delete_callback =callback;
}

bool NotificationManager::PopNotification::is_valid_delete_callback()
{
    if(m_on_delete_callback)
       return true;
    return false;
}

void NotificationManager::PopNotification::use_bbl_theme()
{
    ensure_ui_inited();

    ImGuiStyle &OldStyle         = ImGui::GetStyle();

    m_DefaultTheme.mWindowPadding = OldStyle.WindowPadding;
    m_DefaultTheme.mWindowBkg     = OldStyle.Colors[ImGuiCol_WindowBg];
    m_DefaultTheme.mBorderColor   = OldStyle.Colors[ImGuiCol_Border];
    m_DefaultTheme.mTextColor     = OldStyle.Colors[ImGuiCol_Text];
    m_DefaultTheme.mWindowRound   = OldStyle.WindowRounding;

    OldStyle.WindowPadding             = ImVec2(0, 0);
    OldStyle.WindowRounding            = m_WindowRadius;

	if (m_data.level == NotificationLevel::ErrorNotificationLevel)
        m_CurrentColor = m_ErrorColor;
    else if (m_data.level == NotificationLevel::WarningNotificationLevel)
        m_CurrentColor = m_WarnColor;
    else {
        if (m_data.use_warn_color)
            m_CurrentColor = m_WarnColor;
        else
            m_CurrentColor = m_NormalColor;
    }

	//OldStyle.Colors[ImGuiCol_Border] = m_CurrentColor;

 //   OldStyle.Colors[ImGuiCol_WindowBg] = m_WindowBkgColor;
 //   OldStyle.Colors[ImGuiCol_Text]     = m_TextColor;

	m_WindowBkgColor = m_is_dark ? ImVec4(45 / 255.f, 45 / 255.f, 49 / 255.f, 1.f) : ImVec4(1, 1, 1, 1);
	m_TextColor = m_is_dark ? ImVec4(224 / 255.f, 224 / 255.f, 224 / 255.f, 1.f) : ImVec4(.2f, .2f, .2f, 1.0f);
	m_HyperTextColor = m_is_dark ? ImVec4(0, 0.588, 0.533, 1) : ImVec4(0, 0.588, 0.533, 1);
	m_is_dark ? push_style_color(ImGuiCol_Border, {62 / 255.f, 62 / 255.f, 69 / 255.f, 1.f}, true, m_current_fade_opacity) : push_style_color(ImGuiCol_Border, m_CurrentColor, true, m_current_fade_opacity);
    push_style_color(ImGuiCol_WindowBg, m_WindowBkgColor, true, m_current_fade_opacity);
    push_style_color(ImGuiCol_Text, m_TextColor, true, m_current_fade_opacity);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, m_WindowRadius / 4);
}


void NotificationManager::PopNotification::restore_default_theme()
{
    ImGuiStyle &OldStyle = ImGui::GetStyle();

    OldStyle.WindowPadding             = m_DefaultTheme.mWindowPadding;
    OldStyle.WindowRounding            = m_DefaultTheme.mWindowRound;

	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar();
	//OldStyle.Colors[ImGuiCol_WindowBg] = m_DefaultTheme.mWindowBkg;
 //   OldStyle.Colors[ImGuiCol_Text]     = m_DefaultTheme.mTextColor;
 //   OldStyle.Colors[ImGuiCol_Border]   = m_DefaultTheme.mBorderColor;
}


void NotificationManager::PopNotification::render(GLCanvas3D& canvas, float initial_y, bool move_from_overlay, float overlay_width, float right_margin)
{
	if (m_state == EState::Unknown)
		init();

	if (m_state == EState::Hidden) {
		m_top_y = initial_y - GAP_WIDTH;
		return;
	}

	if (m_state == EState::ClosePending || m_state == EState::Finished)
	{
		m_state = EState::Finished;
		return;
	}

	Size          cnv_size = canvas.get_canvas_size();
	ImGuiWrapper& imgui = *wxGetApp().imgui();
	ImVec2        mouse_pos = ImGui::GetMousePos();
    float         right_gap  = right_margin + (move_from_overlay ? overlay_width + m_line_height * 5 : 0);
	bool          fading_pop = false;

	if (m_line_height != ImGui::CalcTextSize("A").y)
		init();

	set_next_window_size(imgui);

	// top y of window
	m_top_y = initial_y + m_window_height;

	// position of upper-right corner
	ImVec2 win_pos(1.0f * (float)cnv_size.get_width() - right_gap, 1.0f * (float)cnv_size.get_height() - m_top_y);
	imgui.set_next_window_pos(win_pos.x, win_pos.y, ImGuiCond_Always, 1.0f, 0.0f);
	imgui.set_next_window_size(m_window_width, m_window_height, ImGuiCond_Always);

	// find if hovered FIXME:  do it only in update state?
	if (m_state == EState::Hovered) {
		init();
	}

	if (mouse_pos.x < win_pos.x && mouse_pos.x > win_pos.x - m_window_width && mouse_pos.y > win_pos.y && mouse_pos.y < win_pos.y + m_window_height) {
		// Uncomment if imgui window focus is needed on hover. I cant find any case.
		//ImGui::SetNextWindowFocus();
		set_hovered();
	}

	// color change based on fading out
	if (m_state == EState::FadingOut) {
		push_style_color(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), true, m_current_fade_opacity);
		fading_pop = true;
	}

	bool bgrnd_color_pop = push_background_color();

	// name of window indentifies window - has to be unique string
	if (m_id == 0)
		m_id = m_id_provider.allocate_id();
	std::string name = "!!Ntfctn" + std::to_string(m_id);

    use_bbl_theme();

	int window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (imgui.begin(name, window_flags)) {
		ImVec2 win_size = ImGui::GetWindowSize();

		bbl_render_left_sign(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y);
		render_left_sign(imgui);
		render_text(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y);
        m_minimize_b_visible = false;
        if (m_multiline && m_lines_count > 3)
			render_minimize_button(imgui, win_pos.x, win_pos.y);
        render_close_button(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y); // ORCA draw it after minimize button since its position related to minimize button
	}
	imgui.end();

	restore_default_theme();

	if (bgrnd_color_pop)
		ImGui::PopStyleColor();

	if (fading_pop)
		ImGui::PopStyleColor(3);
}

void NotificationManager::PopNotification::bbl_render_block_notification(GLCanvas3D &canvas, float initial_y, bool move_from_overlay, float overlay_width, float right_margin)
{
	if (m_state == EState::Unknown)
		init();

	if (m_state == EState::Hidden) {
		m_top_y = initial_y - GAP_WIDTH;
		return;
	}

	if (m_state == EState::ClosePending || m_state == EState::Finished)
	{
		m_state = EState::Finished;
		return;
	}

	Size          cnv_size = canvas.get_canvas_size();
	ImGuiWrapper& imgui = *wxGetApp().imgui();
    float         right_gap  = right_margin + (move_from_overlay ? overlay_width + m_line_height * 5 : 0);
	bool          fading_pop = false;

	if (m_line_height != ImGui::CalcTextSize("A").y)
		init();

	set_next_window_size(imgui);

	// top y of window
    m_top_y = initial_y + m_window_height;

    ImVec2 win_pos(1.0f * (float) cnv_size.get_width() - right_gap, 1.0f * (float) cnv_size.get_height() - m_top_y);
    imgui.set_next_window_pos(win_pos.x, win_pos.y, ImGuiCond_Always, 1.0f, 0.0f);
    imgui.set_next_window_size(m_window_width, m_window_height, ImGuiCond_Always);

	// color change based on fading out
	if (m_state == EState::FadingOut) {
		push_style_color(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), true, m_current_fade_opacity);
		fading_pop = true;
	}

	bool bgrnd_color_pop = push_background_color();

	// name of window indentifies window - has to be unique string
	if (m_id == 0)
		m_id = m_id_provider.allocate_id();
	std::string name = "!!Ntfctn" + std::to_string(m_id);

	use_bbl_theme();
    if (m_data.level == NotificationLevel::SeriousWarningNotificationLevel) 
	{   // ORCA match and ensure color usage
        push_style_color(ImGuiCol_Border,   m_WarnColor, true, m_current_fade_opacity);
        push_style_color(ImGuiCol_WindowBg, m_WarnColor, true, m_current_fade_opacity);
	}
    if (m_data.level == NotificationLevel::ErrorNotificationLevel) 
    {   // ORCA match and ensure color usage
        push_style_color(ImGuiCol_Border,   m_ErrorColor, true, m_current_fade_opacity);
        push_style_color(ImGuiCol_WindowBg, m_ErrorColor, true, m_current_fade_opacity);
    }
	push_style_color(ImGuiCol_Text, { 1,1,1,1 }, true, m_current_fade_opacity);

	if (imgui.begin(name, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		ImVec2 win_size = ImGui::GetWindowSize();
		ImVec2 win_pos = ImGui::GetWindowPos();
		if (ImGui::IsMouseHoveringRect(win_pos, win_pos + win_size)) {
			set_hovered();
		}

		//render left icon
		bbl_render_block_notif_left_sign(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y);
		bbl_render_block_notif_text(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y);
		bbl_render_block_notif_buttons(imgui, win_size, win_pos);
		//render_close_button(imgui, win_size.x, win_size.y, win_pos.x, win_pos.y);
		//m_minimize_b_visible = false;
		//if (m_multiline && m_lines_count > 3)
		//	render_minimize_button(imgui, win_pos.x, win_pos.y);
	}
	ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
	imgui.end();

	ImGui::PopStyleColor(3);
	restore_default_theme();

	if (bgrnd_color_pop)
		ImGui::PopStyleColor();

	if (fading_pop)
		ImGui::PopStyleColor(3);
}

void NotificationManager::PopNotification::close()
{
    m_state = EState::ClosePending;
    wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
}

bool NotificationManager::PopNotification::push_background_color()
{
	if (m_is_gray) {
		ImVec4 backcolor(0.7f, 0.7f, 0.7f, 0.5f);
		push_style_color(ImGuiCol_WindowBg, backcolor, m_state == EState::FadingOut, m_current_fade_opacity);
		return true;
	}
	if (m_data.level == NotificationLevel::ErrorNotificationLevel) {
		ImVec4 backcolor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		backcolor.x += 0.3f;
		push_style_color(ImGuiCol_WindowBg, backcolor, m_state == EState::FadingOut, m_current_fade_opacity);
		return true;
	}
	if (m_data.level == NotificationLevel::WarningNotificationLevel) {
		ImVec4 backcolor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		backcolor.x += 0.3f;
		backcolor.y += 0.15f;
		push_style_color(ImGuiCol_WindowBg, backcolor, m_state == EState::FadingOut, m_current_fade_opacity);
		return true;
	}

	return false;
}
void NotificationManager::PopNotification::count_spaces()
{
	//determine line width
	m_line_height = ImGui::CalcTextSize("A").y;

	m_left_indentation = m_line_height;
	//if (m_data.level == NotificationLevel::ErrorNotificationLevel
	//	|| m_data.level == NotificationLevel::WarningNotificationLevel
	//	|| m_data.level == NotificationLevel::PrintInfoNotificationLevel
	//	|| m_data.level == NotificationLevel::PrintInfoShortNotificationLevel) {
	//	std::string text;
	//	text = (m_data.level == NotificationLevel::ErrorNotificationLevel ? ImGui::ErrorMarker : ImGui::WarningMarker);
	//	float picture_width = ImGui::CalcTextSize(text.c_str()).x;
	//	m_left_indentation = picture_width + m_line_height / 2;
	//}
	m_window_width_offset = m_left_indentation + m_line_height * 3.f;
    if (m_data.level == NotificationLevel::ErrorNotificationLevel || m_data.level == NotificationLevel::SeriousWarningNotificationLevel) {
		m_left_indentation = 32 + m_line_height;
		m_window_width_offset = 90.f;
	}
	m_window_width = m_line_height * 25;
}

void NotificationManager::PopNotification::count_lines()
{
	std::string text		= m_text1;
	size_t      last_end	= 0;
	m_lines_count			= 0;

	if (text.empty())
		return;

	// handle with marks
    if (pos_start == string::npos && pos_end == string::npos) {
        pos_start = text.find(error_start);
        if (pos_start != string::npos) {
            text.erase(pos_start, error_start.length());
            pos_end = text.find(error_end);
            if (pos_end != string::npos) {
                text.erase(pos_end, error_end.length());
            }
        }
        m_text1 = text;
    }

	m_endlines.clear();
	while (last_end < text.length())
	{
		size_t next_hard_end = text.find_first_of('\n', last_end);
		if (next_hard_end != std::string::npos && ImGui::CalcTextSize(text.substr(last_end, next_hard_end - last_end).c_str()).x < m_window_width - m_window_width_offset) {
			//next line is ended by '/n'
			m_endlines.push_back(next_hard_end);
			last_end = next_hard_end + 1;
		}
		else {
			// find next suitable endline
			if (ImGui::CalcTextSize(text.substr(last_end).c_str()).x >= m_window_width - m_window_width_offset) {
				// more than one line till end
				size_t next_space = text.find_first_of(' ', last_end);
				if (next_space > 0 && next_space < text.length()) {
                    size_t next_space_candidate = text.find_first_of(' ', next_space + 1);
					while (next_space_candidate > 0 && ImGui::CalcTextSize(text.substr(last_end, next_space_candidate - last_end).c_str()).x < m_window_width - m_window_width_offset) {
						next_space = next_space_candidate;
						next_space_candidate = text.find_first_of(' ', next_space + 1);
					}
				} else {
					next_space = text.length();
				}
				// when one word longer than line.
				if (ImGui::CalcTextSize(text.substr(last_end, next_space - last_end).c_str()).x > m_window_width - m_window_width_offset ||
					ImGui::CalcTextSize(text.substr(last_end, next_space - last_end).c_str()).x < (m_window_width - m_window_width_offset) / 4 * 3
					) {
					//BBS
					wxString wx_text = from_u8(text.substr(last_end, text.length() - last_end).c_str());
                    float    width_of_char = ImGui::CalcTextSize("a").x;
                    int letter_count = (int) ((m_window_width - m_window_width_offset) / width_of_char) / 2;	// give a predict value of char count
					int output_count = 0;
					while (true) {
                        if (letter_count >= wx_text.size())
							break;
                        float used_space = ImGui::CalcTextSize(into_u8(wx_text.SubString(0, letter_count)).c_str()).x;
                        if (used_space > m_window_width - m_window_width_offset)
							break;
						letter_count++;
					}
					if (letter_count > 0) {
						output_count = into_u8(wx_text.SubString(0, letter_count - 1)).size();
                    }
                    m_endlines.push_back(last_end + output_count);
                    last_end += output_count;
				} else {
					m_endlines.push_back(next_space);
					last_end = next_space + 1;
				}
			}
			else {
				m_endlines.push_back(text.length());
				last_end = text.length();
			}

		}
		m_lines_count++;
	}
	// hypertext calculation
	if (!m_hypertext.empty()) {
		int prev_end = m_endlines.size() > 1 ? m_endlines[m_endlines.size() - 2] : 0; // m_endlines.size() - 2 because we are fitting hypertext instead of last endline
		if (ImGui::CalcTextSize((escape_string_cstyle(text.substr(prev_end, last_end - prev_end)) + m_hypertext).c_str()).x > m_window_width - m_window_width_offset) {
			m_endlines.push_back(last_end);
			m_lines_count++;
		}
	}

	// m_text_2 (text after hypertext) is not used for regular notifications right now.
	// its caluculation is in HintNotification::count_lines()
}

void NotificationManager::PopNotification::init()
{
	// Do not init closing notification
	if (is_finished())
		return;

	count_spaces();
	count_lines();

	if (m_lines_count <= 6) {
		if (m_state == EState::Unknown) // if is default state
			m_multiline = true;
	}

	m_notification_start = GLCanvas3D::timestamp_now();
	if (m_state == EState::Unknown || m_state == EState::Hovered)
		m_state = EState::Shown;
}
void NotificationManager::PopNotification::set_next_window_size(ImGuiWrapper& imgui)
{
	m_window_height = m_multiline ?
		std::max(m_lines_count, (size_t)2) * m_line_height :
		2 * m_line_height;
	m_window_height += 1 * m_line_height; // top and bottom
}

void NotificationManager::PopNotification::bbl_render_block_notif_text(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	float	x_offset = m_left_indentation;
	int		last_end = 0;
	float	starting_y = (m_lines_count == 2 ? win_size_y / 2 - m_line_height : (m_lines_count == 1 ? win_size_y / 2 - m_line_height / 2 : m_line_height / 2));
	float	shift_y = m_line_height;
	std::string line;

	for (size_t i = 0; i < (m_multiline ? m_endlines.size() : std::min(m_endlines.size(), (size_t)2)); i++) {
		assert(m_endlines.size() > i && m_text1.size() >= m_endlines[i]);
		line.clear();
		ImGui::SetCursorPosX(x_offset);
		ImGui::SetCursorPosY(starting_y + i * shift_y);
		if (m_endlines.size() > i && m_text1.size() >= m_endlines[i]) {
			if (i == 1 && m_endlines.size() > 2 && !m_multiline) {
				// second line with "more" hypertext
				line = m_text1.substr(m_endlines[0] + (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0), m_endlines[1] - m_endlines[0] - (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0));
				while (ImGui::CalcTextSize(line.c_str()).x > m_window_width - m_window_width_offset - ImGui::CalcTextSize((".." + _u8L("More")).c_str()).x) {
					line = line.substr(0, line.length() - 1);
				}
				line += "..";
			}
			else {
				// regural line
				line = m_text1.substr(last_end, m_endlines[i] - last_end);
			}
			last_end = m_endlines[i];
			if (m_text1.size() > m_endlines[i])
				last_end += (m_text1[m_endlines[i]] == '\n' || m_text1[m_endlines[i]] == ' ' ? 1 : 0);

			if (pos_start != string::npos && pos_end != string::npos && m_endlines[i] - line.length() >= pos_start && m_endlines[i] <= pos_end) {
				push_style_color(ImGuiCol_Text, m_ErrorColor, m_state == EState::FadingOut, m_current_fade_opacity);
				imgui.text(line.c_str());
				ImGui::PopStyleColor();
			}
			else {
				imgui.text(line.c_str());
			}
		}
	}
	//hyperlink text
	if (!m_multiline && m_lines_count > 2) {
		render_hypertext(imgui, x_offset + ImGui::CalcTextSize((line + " ").c_str()).x, starting_y + shift_y, _u8L("More"), true);
	}
	else if (!m_hypertext.empty()) {
		const ImVec2 button_size = {52, 28};
		ImVec2 cursor_pos = { win_size_x - m_line_height * 6.0f, win_size_y / 2 - button_size.y / 2 };
		ImGui::SetCursorPos(cursor_pos);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.f));
        //if (imgui.button(_L("Jump"), button_size.x, button_size.y)) { if (on_text_click()) { close(); } }
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
		render_hypertext(imgui, x_offset + ImGui::CalcTextSize((line + (line.empty() ? "" : " ")).c_str()).x, starting_y + (m_endlines.size() - 1) * shift_y, m_hypertext);
	}

	// text2 (text after hypertext) is not rendered for regular notifications
	// its rendering is in HintNotification::render_text
}

void NotificationManager::PopNotification::render_text(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	float	x_offset = m_left_indentation;
	int		last_end = 0;
	float	starting_y = (m_lines_count == 2 ? win_size_y / 2 - m_line_height : (m_lines_count == 1 ? win_size_y / 2 - m_line_height / 2 : m_line_height / 2));
	float	shift_y = m_line_height;
	std::string line;

	for (size_t i = 0; i < (m_multiline ? m_endlines.size() : std::min(m_endlines.size(), (size_t)2)); i++) {
		assert(m_endlines.size() > i && m_text1.size() >= m_endlines[i]);
		line.clear();
		ImGui::SetCursorPosX(x_offset);
		ImGui::SetCursorPosY(starting_y + i * shift_y);
		if (m_endlines.size() > i && m_text1.size() >= m_endlines[i]) {
			if (i == 1 && m_endlines.size() > 2 && !m_multiline) {
				// second line with "more" hypertext
				line = m_text1.substr(m_endlines[0] + (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0), m_endlines[1] - m_endlines[0] - (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0));
				while (ImGui::CalcTextSize(line.c_str()).x > m_window_width - m_window_width_offset - ImGui::CalcTextSize((".." + _u8L("More")).c_str()).x) {
					line = line.substr(0, line.length() - 1);
				}
				line += "..";
			}
			else {
				// regural line
				line = m_text1.substr(last_end, m_endlines[i] - last_end);
			}
			last_end = m_endlines[i];
			if (m_text1.size() > m_endlines[i])
				last_end += (m_text1[m_endlines[i]] == '\n' || m_text1[m_endlines[i]] == ' ' ? 1 : 0);

            if (pos_start != string::npos && pos_end != string::npos&& m_endlines[i] - line.length() >= pos_start && m_endlines[i] <= pos_end) {
                push_style_color(ImGuiCol_Text, m_ErrorColor, m_state == EState::FadingOut, m_current_fade_opacity);
                imgui.text(line.c_str());
                ImGui::PopStyleColor();
            }
            else {
                imgui.text(line.c_str());
            }
		}
	}
	//hyperlink text
	if (!m_multiline && m_lines_count > 2) {
		render_hypertext(imgui, x_offset + ImGui::CalcTextSize((line + " ").c_str()).x, starting_y + shift_y, _u8L("More"), true);
	}
	else if (!m_hypertext.empty()) {
		render_hypertext(imgui, x_offset + ImGui::CalcTextSize((line + (line.empty() ? "" : " ")).c_str()).x, starting_y + (m_endlines.size() - 1) * shift_y, m_hypertext);
	}

	// text2 (text after hypertext) is not rendered for regular notifications
	// its rendering is in HintNotification::render_text
}

void NotificationManager::PopNotification::render_hypertext(ImGuiWrapper& imgui, const float text_x, const float text_y, const std::string text, bool more)
{
	//invisible button
	ImVec2 part_size = ImGui::CalcTextSize(text.c_str());
	ImGui::SetCursorPosX(text_x -4);
	ImGui::SetCursorPosY(text_y -5);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.4f, .4f, .4f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.5f, .5f, .5f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.7f, .7f, .7f, 0.0f));
	if (imgui.button("      ", part_size.x + 6, part_size.y + 10))
	{
		if (more)
		{
			m_multiline = true;
			set_next_window_size(imgui);
		}
		else if (on_text_click()) {
			close();
		}
	}
	ImGui::PopStyleColor(3);

	//hover color
    ImVec4 HyperColor = m_HyperTextColor;//ImVec4(150.f / 255.f, 100.f / 255.f, 0.f / 255.f, 1)
    if (m_data.level == NotificationLevel::SeriousWarningNotificationLevel) 
		HyperColor = ImVec4(0.f, 0.f, 0.f, 0.4f); 
	if (m_data.level == NotificationLevel::ErrorNotificationLevel) 
		HyperColor = ImVec4(135.f / 255.f, 43 / 255.f, 43 / 255.f, 1); 
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly)) 
	{ 
		HyperColor.y += 0.1f; 
		if (m_data.level == NotificationLevel::SeriousWarningNotificationLevel || m_data.level == NotificationLevel::SeriousWarningNotificationLevel) 
			HyperColor.x += 0.2f; 
	}
		

	//text
    push_style_color(ImGuiCol_Text, HyperColor, m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::SetCursorPosX(text_x);
	ImGui::SetCursorPosY(text_y);
	imgui.text(text.c_str());
	ImGui::PopStyleColor();

	//underline
	ImVec2 lineEnd = ImGui::GetItemRectMax();
	lineEnd.y -= 2;
	ImVec2 lineStart = lineEnd;
	lineStart.x = ImGui::GetItemRectMin().x;
    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd,
                                        IM_COL32((int) (HyperColor.x * 255), (int) (HyperColor.y * 255), (int) (HyperColor.z * 255),
                                                 (int) (HyperColor.w * 255.f * (m_state == EState::FadingOut ? m_current_fade_opacity : 1.f))));

}

void NotificationManager::PopNotification::render_close_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
    ensure_ui_inited();
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));


	std::wstring button_text;
	button_text = m_is_dark ? ImGui::CloseNotifDarkButton : ImGui::CloseNotifButton;
    //button_text = ImGui::PreferencesButton;

	//if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y),
	//	                           ImVec2(win_pos.x, win_pos.y + win_size.y - ( m_minimize_b_visible ? 2 * m_line_height : 0)),
	//	                           true))
    if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y), ImVec2(win_pos.x, win_pos.y + 2 * m_line_height+10),true))
	{
		button_text = m_is_dark ? ImGui::CloseNotifHoverDarkButton : ImGui::CloseNotifHoverButton;
	}
	ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.75f);
	//ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
    if (m_minimize_b_visible)
		ImGui::SetCursorPosY(0);
    else
        ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		close();
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.35f);
	ImGui::SetCursorPosY(0);
	if (imgui.button(" ", m_line_height * 2.125, win_size.y - ( m_minimize_b_visible ? 2 * m_line_height : 0)))
	{
		close();
	}

	ImGui::PopStyleColor(5);
}

void NotificationManager::PopNotification::bbl_render_block_notif_buttons(ImGuiWrapper& imgui, ImVec2 win_size, ImVec2 win_pos)
{
	// close button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

	std::wstring button_text;
	button_text = ImGui::CloseBlockNotifButton;
	ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	ImVec2 button_size(button_pic_size.x * 2.f, button_pic_size.y * 2.f);

	ImVec2 cursor_pos = { win_size.x - m_line_height * 2.0f, win_size.y / 2 - button_size.y / 2 };
	ImGui::SetCursorPos(cursor_pos);
	//if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y + win_size.y / 2 - button_pic_size.y),
	//	ImVec2(win_pos.x - win_size.x / 20.f, win_pos.y + win_size.y / 2 + button_pic_size.y),
	//	true))
	if(ImGui::IsMouseHoveringRect(win_pos + cursor_pos, win_pos + cursor_pos + button_size))
	{
		button_text = ImGui::CloseBlockNotifHoverButton;
	}
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		close();
	}
	ImGui::PopStyleColor(5);


	//// minimize button
	//ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	//ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	//push_style_color(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), m_state == EState::FadingOut, m_current_fade_opacity);
	//push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	//push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	////button - if part if treggered
	//std::wstring button_text;
	//button_text = m_is_dark ? ImGui::MinimalizeDarkButton : ImGui::MinimalizeButton;
	//if (ImGui::IsMouseHoveringRect(ImVec2(win_pos_x - m_window_width / 10.f, win_pos_y + m_window_height - 2 * m_line_height + 1),
	//	ImVec2(win_pos_x, win_pos_y + m_window_height),
	//	true))
	//{
	//	button_text = m_is_dark ? ImGui::MinimalizeHoverDarkButton : ImGui::MinimalizeHoverButton;
	//}
	//ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	//ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	//ImGui::SetCursorPosX(m_window_width - m_line_height * 1.8f);
	//ImGui::SetCursorPosY(m_window_height - button_size.y - 5);
	//if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	//{
	//	m_multiline = false;
	//}

	//ImGui::PopStyleColor(5);
	//m_minimize_b_visible = true;
}

//void NotificationManager::PopNotification::render_multiline(ImGuiWrapper &imgui, const float win_pos_x, const float win_pos_y)
//{
//	if (m_data.type == NotificationType::BBLObjectInfo)
//	{
//		set_Multiline(true);
//	}
//}

void NotificationManager::PopNotification::bbl_render_block_notif_left_sign(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	auto window = ImGui::GetCurrentWindow();
	//window->DrawList->AddImage(user_texture_id, bb.Min + padding + margin, bb.Max - padding - margin, uv0, uv1, ImGui::GetColorU32(tint_col));

	std::wstring text;
	text = ImGui::BlockNotifErrorIcon;
	ImGui::SetCursorPosX(m_line_height / 3);
	ImGui::SetCursorPosY(m_window_height / 2 - m_line_height);
	imgui.text(text.c_str());
	
}

void NotificationManager::PopNotification::bbl_render_left_sign(ImGuiWrapper &imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
    ensure_ui_inited();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
	ImVec2 round_rect_pos = ImVec2(win_pos_x - win_size_x + ImGui::GetStyle().WindowBorderSize, win_pos_y + ImGui::GetStyle().WindowBorderSize);
    ImVec2 round_rect_size = ImVec2(m_WindowRadius * 2, win_size_y - 2 * ImGui::GetStyle().WindowBorderSize);

	ImVec2 rect_pos = round_rect_pos + ImVec2(m_WindowRadius, 0);
    ImVec2 rect_size = ImVec2(round_rect_size.x / 2, round_rect_size.y);

	ImU32 clr = ImGui::GetColorU32(ImVec4(m_CurrentColor.x, m_CurrentColor.y, m_CurrentColor.z, m_current_fade_opacity));

    draw_list->AddRectFilled(round_rect_pos, round_rect_pos + round_rect_size, clr, m_WindowRadius);
    draw_list->AddRectFilled(rect_pos, rect_pos + rect_size, clr, 0);
}

void NotificationManager::PopNotification::render_left_sign(ImGuiWrapper& imgui)
{
	//if (m_data.level == NotificationLevel::ErrorNotificationLevel || m_data.level == NotificationLevel::WarningNotificationLevel) {
	//	std::string text;
	//	text = (m_data.level == NotificationLevel::ErrorNotificationLevel ? ImGui::ErrorMarker : ImGui::WarningMarker);
	//	ImGui::SetCursorPosX(m_line_height / 3);
	//	ImGui::SetCursorPosY(m_window_height / 2 - m_line_height);
	//	imgui.text(text.c_str());
	//} else if (m_data.level == NotificationLevel::PrintInfoNotificationLevel || m_data.level == NotificationLevel::PrintInfoShortNotificationLevel) {
	//	std::wstring text;
	//	text = ImGui::InfoMarker;
	//	ImGui::SetCursorPosX(m_line_height / 3);
	//	ImGui::SetCursorPosY(m_window_height / 2 - m_line_height);
	//	imgui.text(text.c_str());
	//}
}
void NotificationManager::PopNotification::render_minimize_button(ImGuiWrapper& imgui, const float win_pos_x, const float win_pos_y)
{
    ensure_ui_inited();
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);


	//button - if part if treggered
	std::wstring button_text;
	button_text = m_is_dark ? ImGui::MinimalizeDarkButton : ImGui::MinimalizeButton;
	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos_x - m_window_width / 10.f, win_pos_y + m_window_height - 2 * m_line_height + 1),
		ImVec2(win_pos_x, win_pos_y + m_window_height),
		true))
	{
		button_text = m_is_dark ? ImGui::MinimalizeHoverDarkButton : ImGui::MinimalizeHoverButton;
	}
	ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(m_window_width - m_line_height * 1.8f);
	ImGui::SetCursorPosY(m_window_height - button_size.y - 5);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		m_multiline = false;
	}

	ImGui::PopStyleColor(5);
	m_minimize_b_visible = true;
}
bool NotificationManager::PopNotification::on_text_click()
{
	if(m_data.callback != nullptr)
		return m_data.callback(m_evt_handler);
	return false;
}
void NotificationManager::PopNotification::update(const NotificationData& n)
{
	m_text1          = n.text1;
	m_hypertext      = n.hypertext;
    m_text2          = n.text2;
    const_cast<NotificationData&>(m_data).callback	 = n.callback;
	init();
}

void NotificationManager::PopNotification::append(const std::string& append_str)
{
	m_text1            = m_text1 + "\n" + append_str;
	//m_hypertext      = n.hypertext;
	//m_text2          = n.text2;
	//const_cast<NotificationData&>(m_data).callback	 = n.callback;
	init();
	m_multiline        = false;
}

bool NotificationManager::PopNotification::compare_text(const std::string& text) const
{
	std::wstring wt1 = boost::nowide::widen(m_text1);
	std::wstring wt2 = boost::nowide::widen(text);
	wt1.erase(std::remove_if(wt1.begin(), wt1.end(), ::iswspace), wt1.end());
	wt2.erase(std::remove_if(wt2.begin(), wt2.end(), ::iswspace), wt2.end());
	if (wt1.compare(wt2) == 0)
		return true;
	return false;
}

void NotificationManager::PopNotification::hide(bool h)
{
    if (is_finished()) return;
    m_state = h ? EState::Hidden : EState::Unknown;
}

bool NotificationManager::PopNotification::update_state(bool paused, const int64_t delta)
{

	m_next_render = std::numeric_limits<int64_t>::max();

	if (m_state == EState::Unknown) {
		init();
		return true;
	}

	if (m_state == EState::Hidden) {
		return false;
	}

	int64_t now = GLCanvas3D::timestamp_now();

	// reset fade opacity for non-closing notifications or hover during fading
	if (m_state != EState::FadingOut && m_state != EState::ClosePending && m_state != EState::Finished) {
		m_current_fade_opacity = 1.0f;
	}

	// reset timers - hovered state is set in render
	if (m_state == EState::Hovered) {
		init();
	// Timers when not fading
	} else if (m_state != EState::NotFading && m_state != EState::FadingOut && m_state != EState::ClosePending && m_state != EState::Finished && get_duration() != 0 && !paused) {
		int64_t up_time = now - m_notification_start;
		if (up_time >= get_duration() * 1000) {
			m_state					= EState::FadingOut;
			m_fading_start			= now;
		} else {
			m_next_render = get_duration() * 1000 - up_time;
		}
	}
	// Timers when fading
	if (m_state == EState::FadingOut && !paused) {
		int64_t curr_time		= now - m_fading_start;
		int64_t next_render		= FADING_OUT_TIMEOUT - delta;
		m_current_fade_opacity	= std::clamp(1.0f - 0.001f * static_cast<float>(curr_time) / FADING_OUT_DURATION, 0.0f, 1.0f);
		if (m_current_fade_opacity <= 0.0f) {
			m_state = EState::Finished;
			return true;
		} else if (next_render <= 20) {
			m_next_render = FADING_OUT_TIMEOUT;
			return true;
		} else {
			m_next_render = next_render;
			return false;
		}
	}

	if (m_state == EState::Finished) {
		return true;
	}

	if (m_state == EState::ClosePending) {
		m_state = EState::Finished;
		return true;
	}
	return false;
}

//---------------ExportFinishedNotification-----------
void NotificationManager::ExportFinishedNotification::count_spaces()
{
	if (m_eject_pending)
	{
		return PopNotification::count_spaces();
	}
	//determine line width
	m_line_height = ImGui::CalcTextSize("A").y;

	m_left_indentation = m_line_height;
	//if (m_data.level == NotificationLevel::ErrorNotificationLevel || m_data.level == NotificationLevel::WarningNotificationLevel) {
	//	std::string text;
	//	text = (m_data.level == NotificationLevel::ErrorNotificationLevel ? ImGui::ErrorMarker : ImGui::WarningMarker);
	//	float picture_width = ImGui::CalcTextSize(text.c_str()).x;
	//	m_left_indentation = picture_width + m_line_height / 2;
	//}
	//TODO count this properly
	m_window_width_offset = m_left_indentation + m_line_height * (m_to_removable ? 6.f : 3.f);
	m_window_width = m_line_height * 25;
}

void NotificationManager::ExportFinishedNotification::render_text(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	if (m_eject_pending)
	{
		return PopNotification::render_text(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	}
	float       x_offset = m_left_indentation;
	std::string fulltext = m_text1 + m_hypertext; //+ m_text2;
	// Lines are always at least two and m_multiline is always true for ExportFinishedNotification.
	// First line has "Export Finished" text and than hyper text open folder.
	// Following lines are path to gcode.
    size_t last_end = 0;
	float starting_y = m_line_height / 2;//10;
	float shift_y = m_line_height;// -m_line_height / 20;
	for (size_t i = 0; i < m_lines_count; i++) {
		assert(m_text1.size() >= m_endlines[i]);
		if (m_text1.size() >= m_endlines[i]) {
			std::string line = m_text1.substr(last_end, m_endlines[i] - last_end);
			last_end = m_endlines[i];
			if (m_text1.size() > m_endlines[i])
				last_end += (m_text1[m_endlines[i]] == '\n' || m_text1[m_endlines[i]] == ' ' ? 1 : 0);
			ImGui::SetCursorPosX(x_offset);
			ImGui::SetCursorPosY(starting_y + i * shift_y);
			imgui.text(line.c_str());
			//hyperlink text
			if ( i == 0 && !m_eject_pending && !m_export_dir_path.empty())  {
				render_hypertext(imgui, x_offset + ImGui::CalcTextSize(line.c_str()).x + ImGui::CalcTextSize("   ").x, starting_y, _u8L("Open Folder."));
			}
		}
	}

}

void NotificationManager::ExportFinishedNotification::render_close_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	PopNotification::render_close_button(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	if (m_to_removable && !m_eject_pending)
		render_eject_button(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
}

void NotificationManager::ExportFinishedNotification::render_eject_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	auto scale = wxGetApp().plater()->get_current_canvas3D()->get_scale();
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

	std::string button_text;
	button_text = ImGui::EjectButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - m_line_height * 5.f, win_pos.y),
		ImVec2(win_pos.x - m_line_height * 2.5f, win_pos.y + win_size.y),
		true))
	{
		button_text = ImGui::EjectHoverButton;
		//tooltip
		ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BACKGROUND);
		ImGui::PushStyleColor(ImGuiCol_Border, { 0,0,0,0 });
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8 * scale, 1 * scale });
		ImGui::BeginTooltip();
		imgui.text(_L("Safely remove hardware."));
		ImGui::EndTooltip();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
	}

	ImVec2 button_pic_size = ImGui::CalcTextSize(button_text.c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 5.0f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		assert(m_evt_handler != nullptr);
		if (m_evt_handler != nullptr)
			wxPostEvent(m_evt_handler, EjectDriveNotificationClickedEvent(EVT_EJECT_DRIVE_NOTIFICAION_CLICKED));
		on_eject_click();
	}

	//invisible large button
		ImGui::SetCursorPosX(win_size.x - m_line_height * 4.625f);
	ImGui::SetCursorPosY(0);
	if (imgui.button("  ", m_line_height * 2.f, win_size.y))
	{
		assert(m_evt_handler != nullptr);
		if (m_evt_handler != nullptr)
			wxPostEvent(m_evt_handler, EjectDriveNotificationClickedEvent(EVT_EJECT_DRIVE_NOTIFICAION_CLICKED));
		on_eject_click();
	}
	ImGui::PopStyleColor(5);
}

bool NotificationManager::ExportFinishedNotification::on_text_click()
{
	open_folder(m_export_dir_path);
	return false;
}
void NotificationManager::ExportFinishedNotification::on_eject_click()
{
	NotificationData data{ get_data().type, get_data().level , 0, _utf8("Ejecting.") };
	m_eject_pending = true;
	m_multiline = false;
	update(data);
}

//------ProgressBar----------------
void NotificationManager::ProgressBarNotification::init()
{
	PopNotification::init();
	//m_lines_count++;
	if (m_endlines.empty()) {
		m_endlines.push_back(0);
	}
	if(m_lines_count >= 2) {
		m_lines_count = 3;
		m_multiline = true;
		while (m_endlines.size() < 3)
			m_endlines.push_back(m_endlines.back());
	} else {
		m_lines_count = 2;
		m_endlines.push_back(m_endlines.back());
		m_multiline = false;
	}
	if(m_state == EState::Shown)
		m_state = EState::NotFading;
}

void NotificationManager::ProgressBarNotification::render_text(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	// hypertext is not rendered at all. If it is needed, it needs to be added here.
	// m_endlines should have endline for each line and then for hypertext thus m_endlines[1] should always be in m_text1
	if (m_multiline) {
		assert(m_text1.size() >= m_endlines[0]  || m_text1.size() >= m_endlines[1]);
		if(m_endlines[0] > m_text1.size() || m_endlines[1] > m_text1.size())
			return;
		// two lines text (what doesnt fit, wont show), one line bar
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(m_line_height / 4);
		imgui.text(m_text1.substr(0, m_endlines[0]).c_str());
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(m_line_height + m_line_height / 4);
		std::string line = m_text1.substr(m_endlines[0] + (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0), m_endlines[1] - m_endlines[0] - (m_text1[m_endlines[0]] == '\n' || m_text1[m_endlines[0]] == ' ' ? 1 : 0));
		imgui.text(line.c_str());
		if (m_has_cancel_button)
			render_cancel_button(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
		render_bar(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	} else {
		assert(m_text1.size() >= m_endlines[0]);
		if (m_endlines[0] > m_text1.size())
			return;
		//one line text, one line bar
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(/*win_size_y / 2 - win_size_y / 6 -*/ m_line_height / 4);
		imgui.text(m_text1.substr(0, m_endlines[0]).c_str());
		if (m_has_cancel_button)
			render_cancel_button(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
		render_bar(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	}


}
void NotificationManager::ProgressBarNotification::render_bar(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	//ImVec4 orange_color			= ImVec4(.99f, .313f, .0f, 1.0f);
	//ImVec4 gray_color			= ImVec4(.34f, .34f, .34f, 1.0f);
    ImVec4 orange_color         = m_NormalColor;
    ImVec4 gray_color           = ImVec4(.7f, .7f, .7f, 1.0f);
	ImVec2 lineEnd				= ImVec2(win_pos_x - m_window_width_offset, win_pos_y + win_size_y / 2 + (m_multiline ? m_line_height / 2 : 0));
	ImVec2 lineStart			= ImVec2(win_pos_x - win_size_x + m_left_indentation, win_pos_y + win_size_y / 2 + (m_multiline ? m_line_height / 2 : 0));
	ImVec2 midPoint				= ImVec2(lineStart.x + (lineEnd.x - lineStart.x) * m_percentage, lineStart.y);
	ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, IM_COL32((int)(gray_color.x * 255), (int)(gray_color.y * 255), (int)(gray_color.z * 255), (m_current_fade_opacity * 255.f)), m_line_height * 0.2f);
	ImGui::GetWindowDrawList()->AddLine(lineStart, midPoint, IM_COL32((int)(orange_color.x * 255), (int)(orange_color.y * 255), (int)(orange_color.z * 255), (m_current_fade_opacity * 255.f)), m_line_height * 0.2f);
	if (m_render_percentage) {
		std::string text;
		std::stringstream stream;
		stream << std::fixed << std::setprecision(2) << (int)(m_percentage * 100) << "%";
		text = stream.str();
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? 0 : m_line_height / 4));
		imgui.text(text.c_str());
	}
}
//------UpdatedItemsInfoNotification-------
void NotificationManager::UpdatedItemsInfoNotification::count_spaces()
{
	//determine line width
	m_line_height = ImGui::CalcTextSize("A").y;

	//std::string text;
	//text =  ImGui::WarningMarker;
	//float picture_width = ImGui::CalcTextSize(text.c_str()).x;
	//m_left_indentation = picture_width + m_line_height / 2;
    m_left_indentation = m_line_height;

	m_window_width_offset = m_left_indentation + m_line_height * 3.f;
	m_window_width = m_line_height * 25;
}
void NotificationManager::UpdatedItemsInfoNotification::add_type(InfoItemType type)
{
	std::vector<std::pair<InfoItemType, size_t>>::iterator it = m_types_and_counts.begin();
	for (; it != m_types_and_counts.end(); ++it) {
		if ((*it).first == type) {
			(*it).second++;
			break;
		}
	}
	if (it == m_types_and_counts.end())
		m_types_and_counts.emplace_back(type, 1);

	std::string text;
	for (it = m_types_and_counts.begin(); it != m_types_and_counts.end(); ++it) {
		if ((*it).second == 0)
			continue;
		switch ((*it).first) {
		case InfoItemType::CustomSupports:      text += format(_L_PLURAL("%1$d Object has custom supports.",		"%1$d Objects have custom supports.",		(*it).second), (*it).second) + "\n"; break;
		// BBS
		//case InfoItemType::CustomSeam:          text += format(("%1$d Object has custom seam.",			"%1$d Objects have custom seam.",			(*it).second), (*it).second) + "\n"; break;
		case InfoItemType::MmSegmentation:     text += format(_L_PLURAL("%1$d Object has color painting.",			"%1$d Objects have color painting.",(*it).second), (*it).second) + "\n"; break;
		// BBS
		//case InfoItemType::Sinking:             text += format(("%1$d Object has partial sinking.",		"%1$d Objects have partial sinking.",		(*it).second), (*it).second) + "\n"; break;
		case InfoItemType::CutConnectors:       text += format(_L_PLURAL("%1$d object was loaded as a part of cut object.",		"%1$d objects were loaded as parts of cut object.", (*it).second), (*it).second) + "\n"; break;
		case InfoItemType::FuzzySkin:           text += format(_L_PLURAL("%1$d object was loaded with fuzzy skin painting.",    "%1$d objects were loaded with fuzzy skin painting.",   (*it).second), (*it).second) + "\n"; break;
		default: BOOST_LOG_TRIVIAL(error) << "Unknown InfoItemType: " << (*it).second; break;
		}
	}
	m_state = EState::Unknown;
	NotificationData data { get_data().type, get_data().level , get_data().duration, text };
	update(data);
}

//------URLDownloadNotification----------------

void NotificationManager::URLDownloadNotification::render_close_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	if (m_percentage < 0.f || m_percentage >= 1.f) {
		render_close_button_inner(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
		if (m_percentage >= 1.f)
			render_open_button_inner(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	} else
		render_pause_cancel_buttons_inner(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
}
void NotificationManager::URLDownloadNotification::render_close_button_inner(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));


	std::string button_text;
    // Orca: Change based on dark mode
	button_text = m_is_dark ? ImGui::CloseNotifDarkButton : ImGui::CloseNotifButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y),
		ImVec2(win_pos.x, win_pos.y + win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)),
		true))
	{
        // Orca: Change based on dark mode
		button_text = m_is_dark ? ImGui::CloseNotifHoverDarkButton : ImGui::CloseNotifHoverButton;
	}
	ImVec2 button_pic_size = ImGui::CalcTextSize(button_text.c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.75f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		close();
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.35f);
	ImGui::SetCursorPosY(0);
	if (imgui.button(" ", m_line_height * 2.125, win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)))
	{
		close();
	}
	ImGui::PopStyleColor(5);

}

void NotificationManager::URLDownloadNotification::render_pause_cancel_buttons_inner(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{

	render_cancel_button_inner(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	render_pause_button_inner(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
}
void NotificationManager::URLDownloadNotification::render_pause_button_inner(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

	std::wstring button_text;
    // Orca: Change based on dark mode
	button_text = m_is_dark ? (m_download_paused ? ImGui::PlayDarkButton : ImGui::PauseDarkButton) : (m_download_paused ? ImGui::PlayButton : ImGui::PauseButton);

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - m_line_height * 5.f, win_pos.y),
		ImVec2(win_pos.x - m_line_height * 2.5f, win_pos.y + win_size.y),
		true))
	{
        // Orca: Change based on dark mode
		button_text = m_is_dark ? (m_download_paused ? ImGui::PlayHoverDarkButton : ImGui::PauseHoverDarkButton) : (m_download_paused ? ImGui::PlayHoverButton : ImGui::PauseHoverButton);
	}

	ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 5.0f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		trigger_user_action_callback(m_download_paused ? DownloaderUserAction::DownloadUserContinued : DownloaderUserAction::DownloadUserPaused);
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 4.625f);
	ImGui::SetCursorPosY(0);
	if (imgui.button("  ", m_line_height * 2.f, win_size.y))
	{
		trigger_user_action_callback(m_download_paused ? DownloaderUserAction::DownloadUserContinued : DownloaderUserAction::DownloadUserPaused);
	}
	ImGui::PopStyleColor(5);
}

void NotificationManager::URLDownloadNotification::render_open_button_inner(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

	std::wstring button_text;
    // Orca: Change based on dark mode
	button_text = m_is_dark ? ImGui::OpenDarkButton : ImGui::OpenButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - m_line_height * 5.f, win_pos.y),
		ImVec2(win_pos.x - m_line_height * 2.5f, win_pos.y + win_size.y),
		true))
	{
        // Orca: Change based on dark mode
		button_text = m_is_dark ? ImGui::OpenHoverDarkButton : ImGui::OpenHoverButton;
	}

	ImVec2 button_pic_size = ImGui::CalcTextSize(into_u8(button_text).c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 5.0f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		trigger_user_action_callback(DownloaderUserAction::DownloadUserOpenedFolder);
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 4.625f);
	ImGui::SetCursorPosY(0);
	if (imgui.button("  ", m_line_height * 2.f, win_size.y))
	{
		trigger_user_action_callback(DownloaderUserAction::DownloadUserOpenedFolder);
	}
	ImGui::PopStyleColor(5);
}

void NotificationManager::URLDownloadNotification::render_cancel_button_inner(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));


	std::string button_text;
	button_text = ImGui::CancelButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y),
		ImVec2(win_pos.x, win_pos.y + win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)),
		true))
	{
		button_text = ImGui::CancelHoverButton;
	}
	ImVec2 button_pic_size = ImGui::CalcTextSize(button_text.c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.75f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		trigger_user_action_callback(DownloaderUserAction::DownloadUserCanceled);
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.35f);
	ImGui::SetCursorPosY(0);
	if (imgui.button(" ", m_line_height * 2.125, win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)))
	{
		trigger_user_action_callback(DownloaderUserAction::DownloadUserCanceled);
	}
	ImGui::PopStyleColor(5);

}

void NotificationManager::URLDownloadNotification::trigger_user_action_callback(DownloaderUserAction action)
{
	if (m_user_action_callback) {
		if (m_user_action_callback(action, m_download_id)) {}
	}
}


void NotificationManager::URLDownloadNotification::render_bar(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ProgressBarNotification::render_bar(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	std::string text;
	if (m_percentage < 0.f) {
		text = _u8L("ERROR") + ": " + m_error_message;
	} else if (m_percentage >= 1.f) {
		text = _u8L("COMPLETED");
	} else {
		std::stringstream stream;
		stream << std::fixed << std::setprecision(2) << (int)(m_percentage * 100) << "%";
		text = stream.str();
	}
	ImGui::SetCursorPosX(m_left_indentation);
	ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? 0 : m_line_height / 4));
	imgui.text(text.c_str());
}

void NotificationManager::URLDownloadNotification::count_spaces()
{
	ProgressBarNotification::count_spaces();
	m_window_width_offset = m_line_height * 6;
}

//------PrintHostUploadNotification----------------
void NotificationManager::PrintHostUploadNotification::init()
{
	ProgressBarNotification::init();
	if (m_state == EState::NotFading && m_uj_state == UploadJobState::PB_COMPLETED)
		m_state = EState::Shown;
}
void NotificationManager::PrintHostUploadNotification::count_spaces()
{
	//determine line width
	m_line_height = ImGui::CalcTextSize("A").y;

	m_left_indentation = m_line_height;
	if (m_uj_state == UploadJobState::PB_ERROR) {
		std::string text;
		text = (m_data.level == NotificationLevel::ErrorNotificationLevel ? ImGui::ErrorMarker : ImGui::WarningMarker);
		float picture_width = ImGui::CalcTextSize(text.c_str()).x;
		m_left_indentation = picture_width + m_line_height / 2;
	}
	m_window_width_offset = m_line_height * 6; //(m_has_cancel_button ? 6 : 4);
	m_window_width = m_line_height * 25;
}
bool NotificationManager::PrintHostUploadNotification::push_background_color()
{

	if (m_uj_state == UploadJobState::PB_ERROR) {
		ImVec4 backcolor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		backcolor.x += 0.3f;
		push_style_color(ImGuiCol_WindowBg, backcolor, m_state == EState::FadingOut, m_current_fade_opacity);
		return true;
	}
	return false;
}
void NotificationManager::PrintHostUploadNotification::set_percentage(float percent)
{
	m_percentage = percent;
	if (percent >= 1.0f) {
		m_uj_state = UploadJobState::PB_COMPLETED;
		m_has_cancel_button = false;
		init();
	} else if (percent < 0.0f) {
		error();
	} else {
		m_uj_state = UploadJobState::PB_PROGRESS;
		m_has_cancel_button = true;
	}
}
void NotificationManager::PrintHostUploadNotification::render_bar(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	std::string text;
	switch (m_uj_state) {
	case Slic3r::GUI::NotificationManager::PrintHostUploadNotification::UploadJobState::PB_PROGRESS:
	{
		ProgressBarNotification::render_bar(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
		float uploaded = m_file_size * m_percentage;
		std::stringstream stream;
		stream << std::fixed << std::setprecision(2) << (int)(m_percentage * 100) << "% - " << uploaded << " of " << m_file_size << "MB uploaded";
		text = stream.str();
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? 0 : m_line_height / 4));
		break;
	}
	case Slic3r::GUI::NotificationManager::PrintHostUploadNotification::UploadJobState::PB_ERROR:
		text = _u8L("ERROR");
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? m_line_height / 4 : m_line_height / 2));
		break;
	case Slic3r::GUI::NotificationManager::PrintHostUploadNotification::UploadJobState::PB_CANCELLED:
		text = _u8L("CANCELED");
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? m_line_height / 4 : m_line_height / 2));
		break;
	case Slic3r::GUI::NotificationManager::PrintHostUploadNotification::UploadJobState::PB_COMPLETED:
		text = _u8L("COMPLETED");
		ImGui::SetCursorPosX(m_left_indentation);
		ImGui::SetCursorPosY(win_size_y / 2 + win_size_y / 6 - (m_multiline ? m_line_height / 4 : m_line_height / 2));
		break;
	}
	imgui.text(text.c_str());

}
void NotificationManager::PrintHostUploadNotification::render_left_sign(ImGuiWrapper& imgui)
{
	// BBS: do not render left button
	//if (m_uj_state == UploadJobState::PB_ERROR) {
	//	std::string text;
	//	text = ImGui::ErrorMarker;
	//	ImGui::SetCursorPosX(m_line_height / 3);
	//	ImGui::SetCursorPosY(m_window_height / 2 - m_line_height);
	//	imgui.text(text.c_str());
	//}
}
void NotificationManager::PrintHostUploadNotification::render_cancel_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

	std::string button_text;
	button_text = ImGui::CancelButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - m_line_height * 5.f, win_pos.y),
		ImVec2(win_pos.x - m_line_height * 2.5f, win_pos.y + win_size.y),
		true))
	{
		button_text = ImGui::CancelHoverButton;
		// tooltip
		long time_now = wxGetLocalTime();
		if (m_hover_time > 0 && m_hover_time < time_now) {
			ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BACKGROUND);
			ImGui::BeginTooltip();
			imgui.text(_u8L("Cancel upload") + " " + GUI::shortkey_ctrl_prefix() + "T");
			ImGui::EndTooltip();
			ImGui::PopStyleColor();
		}
		if (m_hover_time == 0)
			m_hover_time = time_now;
	}
	else
		m_hover_time = 0;

	ImVec2 button_pic_size = ImGui::CalcTextSize(button_text.c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 5.0f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		wxGetApp().printhost_job_queue().cancel(m_job_id - 1);
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 4.625f);
	ImGui::SetCursorPosY(0);
	if (imgui.button("  ", m_line_height * 2.f, win_size.y))
	{
		wxGetApp().printhost_job_queue().cancel(m_job_id - 1);
	}
	ImGui::PopStyleColor(5);
}
//------ProgressIndicatorNotification-------
void NotificationManager::ProgressIndicatorNotification::set_status_text(const char* text)
{
	NotificationData data{ NotificationType::ProgressIndicator, NotificationLevel::ProgressBarNotificationLevel, 0, text };
	update(data);
}

void NotificationManager::ProgressIndicatorNotification::init()
{
	// skip ProgressBarNotification::init (same code here)
	PopNotification::init();
	if (m_endlines.empty()) {
		m_endlines.push_back(0);
	}
	if (m_lines_count >= 2) {
		m_lines_count = 3;
		m_multiline = true;
		while (m_endlines.size() < 3)
			m_endlines.push_back(m_endlines.back());
	}
	else {
		m_lines_count = 2;
		m_endlines.push_back(m_endlines.back());
		m_multiline = false;
	}
	switch (m_progress_state)
	{
	case Slic3r::GUI::NotificationManager::ProgressIndicatorNotification::ProgressIndicatorState::PIS_HIDDEN:
		m_state = EState::Hidden;
		break;
	case Slic3r::GUI::NotificationManager::ProgressIndicatorNotification::ProgressIndicatorState::PIS_PROGRESS_REQUEST:
	case Slic3r::GUI::NotificationManager::ProgressIndicatorNotification::ProgressIndicatorState::PIS_PROGRESS_UPDATED:
		m_state = EState::NotFading;
		break;
	case Slic3r::GUI::NotificationManager::ProgressIndicatorNotification::ProgressIndicatorState::PIS_COMPLETED:
		m_state = EState::ClosePending;
		break;
	default:
		break;
	}
}
void NotificationManager::ProgressIndicatorNotification::set_percentage(float percent)
{
	ProgressBarNotification::set_percentage(percent);
	if (percent >= 0.0f && percent < 1.0f) {
		m_state = EState::NotFading;
		m_has_cancel_button = true;
		m_progress_state = ProgressIndicatorState::PIS_PROGRESS_REQUEST;
	} else if (percent >= 1.0f) {
		m_state = EState::FadingOut;
		m_progress_state = ProgressIndicatorState::PIS_COMPLETED;
		m_has_cancel_button = false;
	} else {
		m_progress_state = ProgressIndicatorState::PIS_HIDDEN;
		m_state = EState::Hidden;
	}
}
bool NotificationManager::ProgressIndicatorNotification::update_state(bool paused, const int64_t delta)
{
	if (m_progress_state == ProgressIndicatorState::PIS_PROGRESS_REQUEST) {
		// percentage was changed (and it called schedule_extra_frame), now update must know this needs render
		m_next_render = 0;
		m_progress_state = ProgressIndicatorState::PIS_PROGRESS_UPDATED;
		m_current_fade_opacity = 1.0f;
		return true;
	}
	bool ret = ProgressBarNotification::update_state(paused, delta);
	if (get_state() == PopNotification::EState::ClosePending || get_state() == PopNotification::EState::Finished)
		// go to PIS_HIDDEN state
		set_percentage(-1.0f);
	return ret;
}

void NotificationManager::ProgressIndicatorNotification::render_cancel_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	ImVec2 win_size(win_size_x, win_size_y);
	ImVec2 win_pos(win_pos_x, win_pos_y);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));


	std::string button_text;
	button_text = ImGui::CancelButton;

	if (ImGui::IsMouseHoveringRect(ImVec2(win_pos.x - win_size.x / 10.f, win_pos.y),
		ImVec2(win_pos.x, win_pos.y + win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)),
		true))
	{
		button_text = ImGui::CancelHoverButton;
	}
	ImVec2 button_pic_size = ImGui::CalcTextSize(button_text.c_str());
	ImVec2 button_size(button_pic_size.x * 1.25f, button_pic_size.y * 1.25f);
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.75f);
	ImGui::SetCursorPosY(win_size.y / 2 - button_size.y);
	if (imgui.button(button_text.c_str(), button_size.x, button_size.y))
	{
		on_cancel_button();
	}

	//invisible large button
	ImGui::SetCursorPosX(win_size.x - m_line_height * 2.35f);
	ImGui::SetCursorPosY(0);
	if (imgui.button(" ", m_line_height * 2.125, win_size.y - (m_minimize_b_visible ? 2 * m_line_height : 0)))
	{
		on_cancel_button();
	}
	ImGui::PopStyleColor(5);
}
void NotificationManager::ProgressIndicatorNotification::render_close_button(ImGuiWrapper& imgui, const float win_size_x, const float win_size_y, const float win_pos_x, const float win_pos_y)
{
	// Do not render close button while showing progress - cancel button is rendered instead
	if (m_percentage >= 1.0f)
	{
		ProgressBarNotification::render_close_button(imgui, win_size_x, win_size_y, win_pos_x, win_pos_y);
	}
}
//------NotificationManager--------
NotificationManager::NotificationManager(wxEvtHandler* evt_handler) :
	m_evt_handler(evt_handler)
{
}

void NotificationManager::on_change_color_mode(bool is_dark) {
	m_is_dark = is_dark;
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications){
		notification->on_change_color_mode(is_dark);
	}
}

void NotificationManager::push_notification(const NotificationType type, int timestamp)
{
	auto it = std::find_if(std::begin(basic_notifications), std::end(basic_notifications),
		boost::bind(&NotificationData::type, boost::placeholders::_1) == type);
	assert(it != std::end(basic_notifications));
	if (it != std::end(basic_notifications))
		push_notification_data(*it, timestamp);
}
void NotificationManager::push_notification(const std::string& text, int timestamp)
{
	push_notification_data({ NotificationType::CustomNotification, NotificationLevel::RegularNotificationLevel, 10, text }, timestamp);
}

void NotificationManager::push_notification(NotificationType type,
                                            NotificationLevel level,
                                            const std::string& text,
                                            const std::string& hypertext,
                                            std::function<bool(wxEvtHandler*)> callback,
                                            int timestamp)
{
	int duration = get_standard_duration(level);
    push_notification_data({ type, level, duration, text, hypertext, callback }, timestamp);
}

void NotificationManager::push_delayed_notification(const NotificationType type, std::function<bool(void)> condition_callback, int64_t initial_delay, int64_t delay_interval)
{
	auto it = std::find_if(std::begin(basic_notifications), std::end(basic_notifications),
		boost::bind(&NotificationData::type, boost::placeholders::_1) == type);
	assert(it != std::end(basic_notifications));
	if (it != std::end(basic_notifications))
		push_delayed_notification_data(std::make_unique<PopNotification>(*it, m_id_provider, m_evt_handler), condition_callback, initial_delay, delay_interval);
}

void NotificationManager::push_validate_error_notification(StringObjectException const &error)
{
    auto po = dynamic_cast<PrintObjectBase const *>(error.object);
    auto mo = po ? po->model_object() : dynamic_cast<ModelObject const *>(error.object);
	std::function<bool(wxEvtHandler*)> callback;
	if (mo || !error.opt_key.empty()) {
		callback =
			[id = mo ? mo->id() : 0, opt = error.opt_key](wxEvtHandler*) {
			auto& objects = wxGetApp().model().objects;
			auto iter = id.id ? std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; }) : objects.end();
			if (iter != objects.end())
				wxGetApp().obj_list()->select_items({ {*iter, nullptr} });
			if (!opt.empty()) {
				if (iter != objects.end())
					wxGetApp().params_panel()->switch_to_object();
				wxGetApp().sidebar().jump_to_option(opt, Preset::TYPE_PRINT, L"");
			}
			else {
				wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
			}
			return false;
		};
	}
    auto link = (mo || !error.opt_key.empty()) ? _u8L("Jump to") : "";
    if (mo) link += std::string(" [") + mo->name + "]";
    if (!error.opt_key.empty()) link += std::string(" (") + error.opt_key + ")";
    push_notification_data({NotificationType::ValidateError, NotificationLevel::ErrorNotificationLevel, 0, _u8L("Error:") + "\n" + error.string, link, callback}, 0);
	set_slicing_progress_hidden();
}

void NotificationManager::push_slicing_error_notification(const std::string &text, std::vector<ModelObject const *> objs)
{
    std::vector<ObjectID> ids;
    for (auto optr : objs) {
        if (optr)
            ids.push_back(optr->id());
    }
	std::function<bool(wxEvtHandler*)> callback;
	if (!objs.empty()) {
		callback = [ids](wxEvtHandler*) {
			auto& objects = wxGetApp().model().objects;
			std::vector<ObjectVolumeID> ovs;
			for (auto id : ids) {
				auto iter = std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; });
				if (iter != objects.end()) { ovs.push_back({ *iter, nullptr }); }
			}
			if (!ovs.empty()) {
				wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
				wxGetApp().obj_list()->select_items(ovs);
			}
			return false;
		};
	}
    auto link     = callback ? _u8L("Jump to") : "";
    if (!objs.empty()) {
        link += " [";
        for (auto obj : objs) {
            if (obj)
                link += obj->name + ", ";
        }
        if (!objs.empty()) {
            link.pop_back();
            link.pop_back();
        }
        link += "] ";
    }
    set_all_slicing_errors_gray(false);
	push_notification_data({ NotificationType::SlicingError, NotificationLevel::ErrorNotificationLevel, 0,  _u8L("Error:") + "\n" + text, link, callback }, 0);
	set_slicing_progress_hidden();
}
void NotificationManager::push_slicing_warning_notification(const std::string& text, bool gray, ModelObject const * obj, ObjectID oid, int warning_step, int warning_msg_id, NotificationLevel level/* = NotificationLevel::WarningNotificationLevel*/)
{
	std::function<bool(wxEvtHandler*)> callback;
	if (obj) {
		callback = [id = obj->id()](wxEvtHandler*) {
			auto& objects = wxGetApp().model().objects;
			auto iter = std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; });
			if (iter != objects.end()) {
				wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
				wxGetApp().obj_list()->select_items({ {*iter, nullptr} });
			}
			return false;
		};
	}
    auto link = callback ? _u8L("Jump to") : "";
    if (obj) link += std::string(" [") + obj->name + "]";
	NotificationData data { NotificationType::SlicingWarning, level, 0,  _u8L("Warning:") + "\n" + text, link, callback };

	data.sub_msg_id = warning_msg_id;
	data.ori_text = text;

	auto notification = std::make_unique<NotificationManager::ObjectIDNotification>(data, m_id_provider, m_evt_handler);
	notification->object_id = oid;
	notification->warning_step = warning_step;
	if (push_notification_data(std::move(notification), 0)) {
		m_pop_notifications.back()->set_gray(gray);
	}
}
void NotificationManager::push_plater_error_notification(const std::string& text)
{
	push_notification_data({ NotificationType::PlaterError, NotificationLevel::ErrorNotificationLevel, 0,  _u8L("Error:") + "\n" + text }, 0);
}

void NotificationManager::close_plater_error_notification(const std::string& text)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PlaterError) {
			notification->close();
		}
	}
}

void NotificationManager::push_general_error_notification(const std::string& text)
{
	push_notification_data({ NotificationType::BBLGeneralError, NotificationLevel::ErrorNotificationLevel, 0,  _u8L("Error:") + "\n" + text }, 0);
}

void NotificationManager::close_general_error_notification(const std::string& text)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::BBLGeneralError && notification->compare_text(_u8L("Error:") + "\n" + text)) {
			notification->close();
		}
	}
}

void NotificationManager::push_slicing_customize_error_notification(NotificationType type, NotificationLevel level, const std::string &text, const std::string &hypertext, std::function<bool(wxEvtHandler *)> callback)
{
    set_all_slicing_errors_gray(false);
    std::string prefix_msg = level == NotificationLevel::WarningNotificationLevel ? _u8L("Warning:") : _u8L("Error:");
    push_notification_data({type, level, 0, prefix_msg + "\n" + text, hypertext, callback}, 0);
}

void NotificationManager::close_slicing_customize_error_notification(NotificationType type, NotificationLevel level)
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == type && notification->get_data().level == level) {
            notification->close();
        }
    }
}

void NotificationManager::push_plater_warning_notification(const std::string& text)
{
	// Find if was not hidden
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PlaterWarning && notification->compare_text(_u8L("Warning:") + "\n" + text)) {
			if (notification->get_state() == PopNotification::EState::Hidden) {
				//dynamic_cast<PlaterWarningNotification*>(notification.get())->show();
				return;
			}
		}
	}

	NotificationData data{ NotificationType::PlaterWarning, NotificationLevel::WarningNotificationLevel, 0,  _u8L("Warning:") + "\n" + text };

	auto notification = std::make_unique<NotificationManager::PlaterWarningNotification>(data, m_id_provider, m_evt_handler);
	push_notification_data(std::move(notification), 0);
	// dissaper if in preview
	apply_in_preview();
}

void NotificationManager::close_plater_warning_notification(const std::string& text)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PlaterWarning && notification->compare_text(_u8L("Warning:") + "\n" + text)) {
			dynamic_cast<PlaterWarningNotification*>(notification.get())->real_close();
		}
	}
}

void NotificationManager::push_flushing_volume_error_notification(NotificationType type, NotificationLevel level, const std::string &text, const std::string &hypertext, std::function<bool(wxEvtHandler *)> callback)
{
    set_all_slicing_errors_gray(false);
    std::string prefix_msg = level == NotificationLevel::WarningNotificationLevel ? _u8L("Warning:") : _u8L("Error:");
    push_notification_data({type, level, 0, prefix_msg + "\n" + text, hypertext, callback}, 0);
}

void NotificationManager::close_flushing_volume_error_notification(NotificationType type, NotificationLevel level)
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == type && notification->get_data().level == level) { notification->close(); }
    }
}

void NotificationManager::set_all_slicing_errors_gray(bool g)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingError) {
			notification->set_gray(g);
		}
	}
}
void NotificationManager::set_all_slicing_warnings_gray(bool g)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingWarning) {
			notification->set_gray(g);
		}
	}
}
/*
void NotificationManager::set_slicing_warning_gray(const std::string& text, bool g)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingWarning && notification->compare_text(text)) {
			notification->set_gray(g);
		}
	}
}
*/
void NotificationManager::close_slicing_errors_and_warnings()
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingError || notification->get_type() == NotificationType::SlicingWarning) {
			notification->close();
		}
	}
}
void NotificationManager::close_slicing_error_notification(const std::string& text)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingError && notification->compare_text(_u8L("Error:") + "\n" + text)) {
			notification->close();
		}
	}
}
void  NotificationManager::push_simplify_suggestion_notification(const std::string& text, ObjectID object_id, const std::string& hypertext/* = ""*/, std::function<bool(wxEvtHandler*)> callback/* = std::function<bool(wxEvtHandler*)>()*/)
{
	NotificationData data{ NotificationType::SimplifySuggestion, NotificationLevel::PrintInfoNotificationLevel, 0,  text, hypertext, callback };
	auto notification = std::make_unique<NotificationManager::ObjectIDNotification>(data, m_id_provider, m_evt_handler);
	notification->object_id = object_id;
	push_notification_data(std::move(notification), 0);
}
void NotificationManager::close_notification_of_type(const NotificationType type)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
		if (notification->get_type() == type) {
			notification->close();
		}
	}
}

void NotificationManager::close_and_delete_self(PopNotification * self)
{
    for (auto it = m_pop_notifications.begin(); it != m_pop_notifications.end();) {
        std::unique_ptr<PopNotification> &notification = *it;
        if (notification.get() == self) {
            m_pop_notifications.erase(it);
            break;
        }else
            ++it;
    }
}

void NotificationManager::remove_notification_of_type(const NotificationType type) {
    for (auto it = m_pop_notifications.begin(); it != m_pop_notifications.end();) {
        std::unique_ptr<PopNotification> &notification = *it;
        if (notification->get_type() == type) {
            it = m_pop_notifications.erase(it);
            break;
        } else
            ++it;
    }
}

void NotificationManager::clear_all()
{
    for (size_t i = 0; i < size_t(NotificationType::NotificationTypeCount); i++) {
        remove_notification_of_type((NotificationType)i);
    }
}

void NotificationManager::remove_slicing_warnings_of_released_objects(const std::vector<ObjectID>& living_oids)
{
	for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
		if (notification->get_type() == NotificationType::SlicingWarning) {
			if (! std::binary_search(living_oids.begin(), living_oids.end(),
				static_cast<ObjectIDNotification*>(notification.get())->object_id))
				notification->close();
		}
}
void NotificationManager::remove_simplify_suggestion_of_released_objects(const std::vector<ObjectID>& living_oids)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications)
		if (notification->get_type() == NotificationType::SimplifySuggestion) {
			if (!std::binary_search(living_oids.begin(), living_oids.end(),
				static_cast<ObjectIDNotification*>(notification.get())->object_id))
				notification->close();
		}
}

void NotificationManager::remove_simplify_suggestion_with_id(const ObjectID oid)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications)
		if (notification->get_type() == NotificationType::SimplifySuggestion) {
			if (static_cast<ObjectIDNotification*>(notification.get())->object_id == oid)
				notification->close();
		}
}

void NotificationManager::set_simplify_suggestion_multiline(const ObjectID oid, bool bMulti)
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::SimplifySuggestion) {
            if (static_cast<ObjectIDNotification *>(notification.get())->object_id == oid)
				notification->set_Multiline(bMulti);
        }
}

void NotificationManager::push_exporting_finished_notification(const std::string& path, const std::string& dir_path, bool on_removable)
{
	close_notification_of_type(NotificationType::ExportFinished);
	NotificationData data{ NotificationType::ExportFinished, NotificationLevel::RegularNotificationLevel, on_removable ? 0 : 20,  _u8L("Exported successfully") + "\n" + path };
	push_notification_data(std::make_unique<NotificationManager::ExportFinishedNotification>(data, m_id_provider, m_evt_handler, on_removable, path, dir_path), 0);
	set_slicing_progress_hidden();
}

void NotificationManager::push_import_finished_notification(const std::string& path, const std::string& dir_path, bool on_removable)
{
    close_notification_of_type(NotificationType::ExportFinished);
    NotificationData data{ NotificationType::ExportFinished, NotificationLevel::RegularNotificationLevel, on_removable ? 0 : 20,  _u8L("Model file downloaded.") + "\n" + path };
    push_notification_data(std::make_unique<NotificationManager::ExportFinishedNotification>(data, m_id_provider, m_evt_handler, on_removable, path, dir_path), 0);
    set_slicing_progress_hidden();
}

void NotificationManager::push_download_URL_progress_notification(size_t id, const std::string& text, std::function<bool(DownloaderUserAction, int)> user_action_callback)
{
    // If already exists
    for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::URLDownload && dynamic_cast<URLDownloadNotification*>(notification.get())->get_download_id() == id) {
            return;
        }
    }
    // push new one
    NotificationData data{ NotificationType::URLDownload, NotificationLevel::ProgressBarNotificationLevel, 5, _u8L("Download") + ": " + text };
    push_notification_data(std::make_unique<NotificationManager::URLDownloadNotification>(data, m_id_provider, m_evt_handler, id, user_action_callback), 0);
}

void NotificationManager::set_download_URL_progress(size_t id, float percentage)
{
    for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::URLDownload) {
            URLDownloadNotification* ntf = dynamic_cast<URLDownloadNotification*>(notification.get());
            if (ntf->get_download_id() != id)
                continue;
            // if this changes the percentage, it should be shown now
            float percent_b4 = ntf->get_percentage();
            ntf->set_percentage(percentage);
            ntf->set_paused(false);
            if (ntf->get_percentage() != percent_b4)
                wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
            return;
        }
    }
}

void NotificationManager::set_download_URL_paused(size_t id)
{
    for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::URLDownload) {
            URLDownloadNotification* ntf = dynamic_cast<URLDownloadNotification*>(notification.get());
            if (ntf->get_download_id() != id)
                continue;
            ntf->set_paused(true);
            wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
            return;
        }
    }
}

void NotificationManager::set_download_URL_canceled(size_t id)
{
    for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::URLDownload) {
            URLDownloadNotification* ntf = dynamic_cast<URLDownloadNotification*>(notification.get());
            if (ntf->get_download_id() != id)
                continue;
            ntf->close();
            wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
            return;
        }
    }
}
void NotificationManager::set_download_URL_error(size_t id, const std::string& text)
{
    for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::URLDownload) {
            URLDownloadNotification* ntf = dynamic_cast<URLDownloadNotification*>(notification.get());
            if (ntf->get_download_id() != id)
                continue;
            float percent_b4 = ntf->get_percentage();
            ntf->set_percentage(-1.f);
            ntf->set_error_message(text);
            if (ntf->get_percentage() != percent_b4)
                wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
            return;
        }
    }
}

void  NotificationManager::push_upload_job_notification(int id, float filesize, const std::string& filename, const std::string& host, float percentage)
{
	// find if upload with same id was not already in notification
	// done by compare_jon_id not compare_text thus has to be performed here
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PrintHostUpload && dynamic_cast<PrintHostUploadNotification*>(notification.get())->compare_job_id(id)) {
			return;
		}
	}
	std::string text = PrintHostUploadNotification::get_upload_job_text(id, filename, host);
	NotificationData data{ NotificationType::PrintHostUpload, NotificationLevel::ProgressBarNotificationLevel, 10, text };
	push_notification_data(std::make_unique<NotificationManager::PrintHostUploadNotification>(data, m_id_provider, m_evt_handler, 0, id, filesize), 0);
}
void NotificationManager::set_upload_job_notification_percentage(int id, const std::string& filename, const std::string& host, float percentage)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PrintHostUpload) {
			PrintHostUploadNotification* phun = dynamic_cast<PrintHostUploadNotification*>(notification.get());
			if (phun->compare_job_id(id)) {
				phun->set_percentage(percentage);
				wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
				break;
			}
		}
	}
}
void NotificationManager::upload_job_notification_show_canceled(int id, const std::string& filename, const std::string& host)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PrintHostUpload) {
			PrintHostUploadNotification* phun = dynamic_cast<PrintHostUploadNotification*>(notification.get());
			if (phun->compare_job_id(id)) {
				phun->cancel();
				wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
				break;
			}
		}
	}
}
void NotificationManager::upload_job_notification_show_error(int id, const std::string& filename, const std::string& host)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::PrintHostUpload) {
			PrintHostUploadNotification* phun = dynamic_cast<PrintHostUploadNotification*>(notification.get());
			if(phun->compare_job_id(id)) {
				phun->error();
				wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
				break;
			}
		}
	}
}

void NotificationManager::push_slicing_serious_warning_notification(const std::string &text, std::vector<ModelObject const *> objs) 
{
    std::vector<ObjectID> ids;
    for (auto optr : objs) {
        if (optr) ids.push_back(optr->id());
    }
	std::function<bool(wxEvtHandler*)> callback;
	if (!objs.empty()) {
		callback = [ids](wxEvtHandler*) {
			auto& objects = wxGetApp().model().objects;
			std::vector<ObjectVolumeID> ovs;
			for (auto id : ids) {
				auto iter = std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; });
				if (iter != objects.end()) { ovs.push_back({ *iter, nullptr }); }
			}
			if (!ovs.empty()) {
				wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
				wxGetApp().obj_list()->select_items(ovs);
			}
			return false;
		};
	}
    auto link     = callback ? _u8L("Jump to") : "";
    if (!objs.empty()) {
        link += " [";
        for (auto obj : objs) {
            if (obj) link += obj->name + ", ";
        }
        if (!objs.empty()) {
            link.pop_back();
            link.pop_back();
        }
        link += "] ";
    }
    set_all_slicing_warnings_gray(false);
    push_notification_data({NotificationType::SlicingSeriousWarning, NotificationLevel::SeriousWarningNotificationLevel, 0, _u8L("Serious warning:") + "\n" + text, link,
                            callback},
                           0);
    set_slicing_progress_hidden();
}

void NotificationManager::close_slicing_serious_warning_notification(const std::string &text) 
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::SlicingSeriousWarning && notification->compare_text(_u8L("Serious warning:") + "\n" + text)) { notification->close(); }
    }
}

void NotificationManager::init_slicing_progress_notification(std::function<bool()> cancel_callback)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_cancel_callback(cancel_callback);
			return;
		}
	}
	NotificationData data{ NotificationType::SlicingProgress, NotificationLevel::ProgressBarNotificationLevel, 0,  std::string(),std::string(),
						  [](wxEvtHandler* evnthndlr) {
							  if (evnthndlr != nullptr)
								  wxPostEvent(evnthndlr, ExportGcodeNotificationClickedEvent(EVT_EXPORT_GCODE_NOTIFICAION_CLICKED));
							  return true;
						  }
	};
	push_notification_data(std::make_unique<NotificationManager::SlicingProgressNotification>(data, m_id_provider, m_evt_handler, cancel_callback), 0);
}
void NotificationManager::update_slicing_notif_dailytips(bool need_change)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			SlicingProgressNotification* spn = dynamic_cast<SlicingProgressNotification*>(notification.get());
			if (need_change) {
				wxGetApp().plater()->get_dailytips()->close();
				spn->get_dailytips_panel()->retrieve_data_from_hint_database(HintDataNavigation::Random);
				wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
			}
			return;
		}
	}
	// Slicing progress notification was not found - init it thru plater so correct cancel callback function is appended
	wxGetApp().plater()->init_notification_manager();
}
void NotificationManager::set_slicing_progress_began()
{
	for (std::unique_ptr<PopNotification> & notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			SlicingProgressNotification* spn = dynamic_cast<SlicingProgressNotification*>(notification.get());
			spn->set_progress_state(SlicingProgressNotification::SlicingProgressState::SP_BEGAN);
			return;
		}
	}
	// Slicing progress notification was not found - init it thru plater so correct cancel callback function is appended
	wxGetApp().plater()->init_notification_manager();
}
void NotificationManager::set_slicing_progress_percentage(const std::string& text, float percentage)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			SlicingProgressNotification* spn = dynamic_cast<SlicingProgressNotification*>(notification.get());
			if(spn->set_progress_state(percentage)) {
				spn->set_status_text(text);
				wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
			}
			return;
		}
	}
	// Slicing progress notification was not found - init it thru plater so correct cancel callback function is appended
	wxGetApp().plater()->init_notification_manager();
}
void NotificationManager::set_slicing_progress_canceled(const std::string& text)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			SlicingProgressNotification* spn = dynamic_cast<SlicingProgressNotification*>(notification.get());
			spn->set_progress_state(SlicingProgressNotification::SlicingProgressState::SP_CANCELLED);
			spn->set_status_text(text);
			wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
			return;
		}
	}
	// Slicing progress notification was not found - init it thru plater so correct cancel callback function is appended
	wxGetApp().plater()->init_notification_manager();
}
void NotificationManager::set_slicing_progress_hidden()
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			SlicingProgressNotification* notif = dynamic_cast<SlicingProgressNotification*>(notification.get());
			notif->set_progress_state(SlicingProgressNotification::SlicingProgressState::SP_NO_SLICING);
			wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
			return;
		}
	}
	// Slicing progress notification was not found - init it thru plater so correct cancel callback function is appended
	wxGetApp().plater()->init_notification_manager();
}
void NotificationManager::set_slicing_complete_print_time(const std::string& info, bool sidebar_colapsed)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_sidebar_collapsed(sidebar_colapsed);
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_print_info(info);
			break;
		}
	}
}
void NotificationManager::set_sidebar_collapsed(bool collapsed)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_sidebar_collapsed(collapsed);
			break;
		}
	}
}
void NotificationManager::set_fff(bool fff)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_fff(fff);
			break;
		}
	}
}
void NotificationManager::set_slicing_progress_export_possible()
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::SlicingProgress) {
			dynamic_cast<SlicingProgressNotification*>(notification.get())->set_export_possible(true);
			break;
		}
	}
}
void NotificationManager::init_progress_indicator()
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			return;
		}
	}
	NotificationData data{ NotificationType::ProgressIndicator, NotificationLevel::ProgressBarNotificationLevel, 1};
	auto notification = std::make_unique<NotificationManager::ProgressIndicatorNotification>(data, m_id_provider, m_evt_handler);
	push_notification_data(std::move(notification), 0);
}

void NotificationManager::progress_indicator_set_range(int range)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			dynamic_cast<ProgressIndicatorNotification*>(notification.get())->set_range(range);
			return;
		}
	}
	init_progress_indicator();
}
void NotificationManager::progress_indicator_set_cancel_callback(CancelFn callback/* = CancelFn()*/)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			dynamic_cast<ProgressIndicatorNotification*>(notification.get())->set_cancel_callback(callback);
			return;
		}
	}
	init_progress_indicator();
}
void NotificationManager::progress_indicator_set_progress(int pr)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			dynamic_cast<ProgressIndicatorNotification*>(notification.get())->set_progress(pr);
			// Ask for rendering - needs to be done on every progress. Calls to here doesnt trigger IDLE event or rendering.
			wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(100);
			return;
		}
	}
	init_progress_indicator();
}
void NotificationManager::progress_indicator_set_status_text(const char* text)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			dynamic_cast<ProgressIndicatorNotification*>(notification.get())->set_status_text(text);
			return;
		}
	}
	init_progress_indicator();
}
int  NotificationManager::progress_indicator_get_range() const
{
	for (const std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ProgressIndicator) {
			return dynamic_cast<ProgressIndicatorNotification*>(notification.get())->get_range();
		}
	}
	return 0;
}

void NotificationManager::push_hint_notification(bool open_next)
{
	return;
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::DidYouKnowHint) {
			(dynamic_cast<HintNotification*>(notification.get()))->open_next();
			return;
		}
	}

	NotificationData data{ NotificationType::DidYouKnowHint, NotificationLevel::HintNotificationLevel, 300, "" };
	// from user - open now
	if (!open_next) {
		push_notification_data(std::make_unique<NotificationManager::HintNotification>(data, m_id_provider, m_evt_handler, open_next), 0);
		stop_delayed_notifications_of_type(NotificationType::DidYouKnowHint);
		// at startup - delay for half a second to let other notification pop up, than try every 30 seconds
		// show only if no notifications are shown
	}
	else {
		auto condition = [&self = std::as_const(*this)]() {
			return self.get_notification_count() == 0;
		};
		push_delayed_notification_data(std::make_unique<NotificationManager::HintNotification>(data, m_id_provider, m_evt_handler, open_next), condition, 500, 30000);
	}
}

bool NotificationManager::is_hint_notification_open()
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::DidYouKnowHint)
			return true;
	}
	return false;
}
void NotificationManager::deactivate_loaded_hints()
{
	;
}
void NotificationManager::push_updated_item_info_notification(InfoItemType type)
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::UpdatedItemsInfo) {
			(dynamic_cast<UpdatedItemsInfoNotification*>(notification.get()))->add_type(type);
			return;
		}
	}

	NotificationData data{ NotificationType::UpdatedItemsInfo, NotificationLevel::PrintInfoNotificationLevel, 10, "" };
	auto notification = std::make_unique<NotificationManager::UpdatedItemsInfoNotification>(data, m_id_provider, m_evt_handler, type);
	if (push_notification_data(std::move(notification), 0)) {
		(dynamic_cast<UpdatedItemsInfoNotification*>(m_pop_notifications.back().get()))->add_type(type);
	}

}
bool NotificationManager::push_notification_data(const NotificationData& notification_data, int timestamp)
{
	return push_notification_data(std::make_unique<PopNotification>(notification_data, m_id_provider, m_evt_handler), timestamp);
}
bool NotificationManager::push_notification_data(std::unique_ptr<NotificationManager::PopNotification> notification, int timestamp)
{
	// if timestamped notif, push only new one
	if (timestamp != 0) {
		if (m_used_timestamps.find(timestamp) == m_used_timestamps.end()) {
			m_used_timestamps.insert(timestamp);
		} else {
			return false;
		}
	}
    if(!notification->is_valid_delete_callback()){
        auto delete_self = [this](NotificationManager::PopNotification* it) {
             m_to_delete_after_finish_render = it;
        };
        notification->set_delete_callback(delete_self);
    }
	bool retval = false;
	if (this->activate_existing(notification.get())) {
		if (m_initialized) { // ignore update action - it cant be initialized if canvas and imgui context is not ready
			if (notification->get_type() == NotificationType::SlicingWarning) {
				m_pop_notifications.back()->append(notification->get_data().ori_text);
			} else {
                m_pop_notifications.back()->update(notification->get_data());
            }
		}
	} else {
		m_pop_notifications.emplace_back(std::move(notification));
        
		retval = true;
	}
	if (!m_initialized)
		return retval;
	GLCanvas3D& canvas = *wxGetApp().plater()->get_current_canvas3D();
	canvas.schedule_extra_frame(0);
	return retval;
}

void NotificationManager::push_delayed_notification_data(std::unique_ptr<NotificationManager::PopNotification> notification,
                                                         std::function<bool(void)>                             condition_callback,
                                                         int64_t                                               initial_delay,
                                                         int64_t                                               delay_interval)
{
	if (initial_delay == 0 && condition_callback()) {
		if( push_notification_data(std::move(notification), 0))
			return;
	}
	m_waiting_notifications.emplace_back(std::move(notification), condition_callback, initial_delay == 0 ? delay_interval : initial_delay, delay_interval);
	wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(initial_delay == 0 ? delay_interval : initial_delay);
}

void NotificationManager::stop_delayed_notifications_of_type(const NotificationType type)
{
	for (auto it = m_waiting_notifications.begin(); it != m_waiting_notifications.end();) {
		if ((*it).notification->get_type() == type) {
			it = m_waiting_notifications.erase(it);
		}
		else {
			++it;
		}
	}
}

void NotificationManager::render_notifications(GLCanvas3D &canvas, float overlay_width, float bottom_margin, float right_margin)
{
	sort_notifications();

	float bottom_up_last_y = bottom_margin; // ORCA dont scale margins

	int i = 0;
	for (const auto& notification : m_pop_notifications) {
        if (notification->get_data().level == NotificationLevel::ErrorNotificationLevel || notification->get_data().level == NotificationLevel::SeriousWarningNotificationLevel) {
            notification->bbl_render_block_notification(canvas, bottom_up_last_y, m_move_from_overlay && !m_in_preview, overlay_width * m_scale, right_margin);  // ORCA dont scale margins
            if (notification->get_state() != PopNotification::EState::Finished) 
				bottom_up_last_y = notification->get_top() + GAP_WIDTH;
		}
		else {
			if (notification->get_state() != PopNotification::EState::Hidden && notification->get_state() != PopNotification::EState::Finished) {
				i++;
				notification->render(canvas, bottom_up_last_y, m_move_from_overlay && !m_in_preview, overlay_width * m_scale, right_margin); // ORCA dont scale margins
				if (notification->get_state() != PopNotification::EState::Finished)
					bottom_up_last_y = notification->get_top() + GAP_WIDTH;
			}
		}
	}
	for (const auto& notification : m_pop_notifications) {
		if (notification->get_data().type == NotificationType::SlicingProgress && notification->get_state() != PopNotification::EState::Hidden && notification->get_state() != PopNotification::EState::Finished) {
			;// assert(i <= 1);
		}
	}
    if (m_to_delete_after_finish_render) {
        close_and_delete_self(m_to_delete_after_finish_render);
        m_to_delete_after_finish_render = nullptr;
    }
	m_last_render = GLCanvas3D::timestamp_now();
}

bool NotificationManager::update_notifications(GLCanvas3D& canvas)
{
	// no update if not top window
	wxWindow* p = dynamic_cast<wxWindow*>(wxGetApp().plater());
	while (p->GetParent() != nullptr)
		p = p->GetParent();
	wxTopLevelWindow* top_level_wnd = dynamic_cast<wxTopLevelWindow*>(p);
	if (!top_level_wnd->IsActive())
		return false;

	// next_render() returns numeric_limits::max if no need for frame
	const int64_t max = std::numeric_limits<int64_t>::max();
	int64_t       next_render = max;
	const int64_t time_since_render = GLCanvas3D::timestamp_now() - m_last_render;
	bool		  request_render = false;
	// During render, each notification detects if its currently hovered and changes its state to EState::Hovered
	// If any notification is hovered, all restarts its countdown
	bool          hover = false;
	for (const std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->is_hovered()) {
			hover = true;
			break;
		}
	}
	// update state of all notif and erase finished
	for (auto it = m_pop_notifications.begin(); it != m_pop_notifications.end();) {
		std::unique_ptr<PopNotification>& notification = *it;
		request_render |= notification->update_state(hover, time_since_render);
		next_render = std::min<int64_t>(next_render, notification->next_render());
		if (notification->get_state() == PopNotification::EState::Finished)
			it = m_pop_notifications.erase(it);
		else
			++it;
	}

	// delayed notifications
	for (auto it = m_waiting_notifications.begin(); it != m_waiting_notifications.end();) {
		// substract time
		if ((*it).remaining_time > 0)
			(*it).remaining_time -= time_since_render;
		if ((*it).remaining_time <= 0) {
			if ((*it).notification && (*it).condition_callback()) { // push notification, erase it from waiting list (frame is scheduled by push)
				(*it).notification->reset_timer();
				// if activate_existing returns false, we expect push to return true.
				if(!this->activate_existing((*it).notification.get()) || (*it).delay_interval == 0) {
					push_notification_data(std::move((*it).notification), 0);
					it = m_waiting_notifications.erase(it);
					continue;
				}
			}
			// not possible to push, delay for delay_interval
			(*it).remaining_time = (*it).delay_interval;
		}
		next_render = std::min<int64_t>(next_render, (*it).remaining_time);
		++it;
	}

	// request next frame in future
	if (next_render < max)
		canvas.schedule_extra_frame(int(next_render));

	return request_render;
}

void NotificationManager::sort_notifications()
{
	// Stable sorting, so that the order of equal ranges is stable.
	std::stable_sort(m_pop_notifications.begin(), m_pop_notifications.end(), [](const std::unique_ptr<PopNotification> &n1, const std::unique_ptr<PopNotification> &n2) {
		int n1l = (int)n1->get_data().level;
		int n2l = (int)n2->get_data().level;
		if (n1l == n2l && n1->is_gray() && !n2->is_gray())
			return true;
		return (n1l < n2l);
		});
}

bool NotificationManager::activate_existing(const NotificationManager::PopNotification* notification)
{
	NotificationType   new_type = notification->get_type();
	const std::string &new_text = notification->get_data().text1;
	for (auto it = m_pop_notifications.begin(); it != m_pop_notifications.end(); ++it) {
		if ((*it)->get_type() == new_type && !(*it)->is_finished()) {
			// multiple of one type allowed, but must have different text
			if (std::find(m_multiple_types.begin(), m_multiple_types.end(), new_type) != m_multiple_types.end()) {
				// If found same type and same text, return true - update will be performed on the old notif
				if ((*it)->compare_text(new_text) == false) {
					continue;
				}
			// multiple of one type allowed, but must have different text nad ObjectID
			} else if (new_type == NotificationType::SlicingWarning) {
				auto w1 = dynamic_cast<const ObjectIDNotification*>(notification);
				auto w2 = dynamic_cast<const ObjectIDNotification*>(it->get());
				if (w1 != nullptr && w2 != nullptr) {
					const NotificationData& data1 = w1->get_data();
					const NotificationData& data2 = w2->get_data();
					if (data1.sub_msg_id != data2.sub_msg_id)
						continue;
					//if (!(*it)->compare_text(new_text) || w1->object_id != w2->object_id) {
					//	continue;
					//}
				} else {
					continue;
				}
			}
			if (it != m_pop_notifications.end() - 1)
				std::rotate(it, it + 1, m_pop_notifications.end());
			return true;
		}
	}
	return false;
}

void NotificationManager::set_in_preview(bool preview)
{
    m_in_preview = preview;
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::PlaterWarning)
            notification->hide(preview);
        if (notification->get_type() == NotificationType::BBLPlateInfo)
            notification->hide(preview);
        if (notification->get_type() == NotificationType::SignDetected)
            notification->hide(!preview);
        if (notification->get_type() == NotificationType::BBLObjectInfo)
            notification->hide(preview);
        if (notification->get_type() == NotificationType::BBLSeqPrintInfo)
            notification->hide(preview);
		if (m_in_preview && notification->get_type() == NotificationType::DidYouKnowHint)
			notification->close();
        if (notification->get_type() == NotificationType::ValidateWarning) 
			notification->hide(preview);
    }
}

bool NotificationManager::has_slicing_error_notification()
{
	return std::any_of(m_pop_notifications.begin(), m_pop_notifications.end(), [](auto &n) {
    	return n->get_type() == NotificationType::SlicingError;
    });
}

void NotificationManager::new_export_began(bool on_removable)
{
	close_notification_of_type(NotificationType::ExportFinished);
	// If we want to hold information of ejecting removable on later export finished notifications
	/*
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ExportToRemovableFinished) {
			if (!on_removable) {
				const NotificationData old_data = notification->get_data();
				notification->update( {old_data.type, old_data.level ,old_data.duration, std::string(), old_data.hypertext} );
			} else {
				notification->close();
			}
			return;
		}
	}
	*/
}
void NotificationManager::device_ejected()
{
	for (std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_type() == NotificationType::ExportFinished && dynamic_cast<ExportFinishedNotification*>(notification.get())->m_to_removable)
			notification->close();
	}
}
size_t NotificationManager::get_notification_count() const
{
	size_t ret = 0;
	for (const std::unique_ptr<PopNotification>& notification : m_pop_notifications) {
		if (notification->get_state() != PopNotification::EState::Hidden)
			ret++;
	}
	return ret;
}

void NotificationManager::bbl_show_plateinfo_notification(const std::string &text)
{
    NotificationData data{NotificationType::BBLPlateInfo, NotificationLevel::PrintInfoNotificationLevel, BBL_NOTICE_MAX_INTERVAL, text};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLPlateInfo) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_3mf_warn_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBL3MFInfo) {
            notification->close();
        }
}

void NotificationManager::bbl_show_3mf_warn_notification(const std::string &text)
{
    NotificationData data{NotificationType::BBL3MFInfo, NotificationLevel::ErrorNotificationLevel, BBL_NOTICE_MAX_INTERVAL, text};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBL3MFInfo) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_plateinfo_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLPlateInfo) {
            notification->close();
        }
}


void NotificationManager::bbl_show_preview_only_notification(const std::string &text)
{
    NotificationData data{NotificationType::BBLPreviewOnlyMode, NotificationLevel::WarningNotificationLevel, 0, text};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLPreviewOnlyMode) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_preview_only_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLPreviewOnlyMode) {
            notification->close();
        }
}

void NotificationManager::bbl_show_objectsinfo_notification(const std::string &text, bool is_warning, bool is_hidden)
{
    std::string hyper_text;
    auto callback = std::function<bool(wxEvtHandler *)>();
    if (is_warning) {
        callback =[](wxEvtHandler *) {
            wxCommandEvent *evt = new wxCommandEvent(EVT_REPAIR_MODEL);
            wxQueueEvent(wxGetApp().plater(), evt);
            return false;
        };
        hyper_text =  _u8L(" (Repair)");
    }
    NotificationData data{NotificationType::BBLObjectInfo, NotificationLevel::PrintInfoNotificationLevel, BBL_NOTICE_MAX_INTERVAL, text, hyper_text, callback};
    if (is_warning)
        data.use_warn_color = true;

    for (auto it = m_pop_notifications.begin(); it != m_pop_notifications.end();) {
        std::unique_ptr<PopNotification>& notification = *it;
        if (notification->get_type() == NotificationType::BBLObjectInfo) {
            it = m_pop_notifications.erase(it);
            break;
        }
        else
            ++it;
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    notification->set_Multiline(true);
    if (is_hidden)
        notification->hide(true);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_objectsinfo_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLObjectInfo) { notification->close(); }
}

void NotificationManager::bbl_show_seqprintinfo_notification(const std::string &text)
{
    NotificationData data{NotificationType::BBLSeqPrintInfo, NotificationLevel::PrintInfoNotificationLevel, BBL_NOTICE_MAX_INTERVAL, text};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLSeqPrintInfo) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    notification->set_Multiline(true);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_seqprintinfo_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLSeqPrintInfo) { notification->close(); }
}

void NotificationManager::bbl_show_plugin_install_notification(const std::string &text)
{
    std::string hyper_text;
    auto callback = [](wxEvtHandler *) {
        wxCommandEvent *evt = new wxCommandEvent(EVT_INSTALL_PLUGIN_NETWORKING);
        wxQueueEvent(wxGetApp().plater(), evt);
        return false;
    };
    hyper_text =  _u8L(" Click here to install it.");
    NotificationData data{NotificationType::BBLPluginInstallHint, NotificationLevel::WarningNotificationLevel, 0, text, hyper_text, callback};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLPluginInstallHint) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    notification->set_Multiline(true);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_close_plugin_install_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLPluginInstallHint) { notification->close(); }
}

void NotificationManager::bbl_show_slice_emptylayer_notification(const std::string &text, bool bOverride)
{
    NotificationData data{NotificationType::BBLSliceEmptyLayer, NotificationLevel::WarningNotificationLevel, 0, _u8L("WARNING:") + "\n" + text};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLSliceEmptyLayer) {
            if (bOverride)
                notification->update(data);
            else {
                NotificationData sdata{NotificationType::BBLSliceEmptyLayer, NotificationLevel::WarningNotificationLevel, 0, notification->get_text1() + "\n" + text};
                notification->update(sdata);
            }

            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    notification->set_Multiline(true);
    push_notification_data(std::move(notification), 0);
}

void NotificationManager::bbl_show_app_newversion_notification()
{
	push_notification(NotificationType::NewAppAvailable);
}

void NotificationManager::bbl_show_need_support_on_notification()
{
    NotificationData data{NotificationType::BBLNeedSupportON, NotificationLevel::WarningNotificationLevel, 0,
                          _u8L("Warning:") + "\n" + _u8L("Your model needs support! Please enable support material.")};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLNeedSupportON) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    push_notification_data(std::move(notification), 0);
}


void NotificationManager::bbl_close_need_support_on_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLNeedSupportON) { notification->close(); }
}


void NotificationManager::bbl_show_gcode_overlap_notification()
{
    NotificationData data{NotificationType::BBLGcodeOverlap, NotificationLevel::WarningNotificationLevel, 0,
                          _u8L("Warning:") + "\n" + _u8L("G-code path overlap")};

    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == NotificationType::BBLGcodeOverlap) {
            notification->reinit();
            notification->update(data);
            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    push_notification_data(std::move(notification), 0);
}


void NotificationManager::bbl_close_gcode_overlap_notification()
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == NotificationType::BBLGcodeOverlap) { notification->close(); }
}

void NotificationManager::bbl_show_bed_filament_incompatible_notification(const std::string& text)
{
	auto callback = [](wxEvtHandler*) {
		const wxString bed_filament_compatibility_wiki = "https://wiki.bambulab.com/en/general/filament-guide-material-table";
		wxGetApp().open_browser_with_warning_dialog(bed_filament_compatibility_wiki);
		return false;
	};
	push_notification_data({ NotificationType::BBLBedFilamentIncompatible,NotificationLevel::ErrorNotificationLevel,0,_u8L("Error:") + "\n" + text,"Click for more.",callback }, 0);
}

void NotificationManager::bbl_close_bed_filament_incompatible_notification()
{
	close_notification_of_type(NotificationType::BBLBedFilamentIncompatible);
}

void NotificationManager::bbl_show_filament_map_invalid_notification_before_slice(const NotificationType type,const std::string& text)
{
    auto callback = [](wxEvtHandler*) {
        auto plater = wxGetApp().plater();
        auto partplate = plater->get_partplate_list().get_curr_plate();
        try_pop_up_before_slice(false, plater, partplate, true); // ignore the return value
        return false;
    };

    push_notification_data({ type,NotificationLevel::ErrorNotificationLevel,0,_u8L("Error:") + "\n" + text,_u8L("Click here to regroup"),callback }, 0);
}

void NotificationManager::bbl_close_filament_map_invalid_notification_before_slice(const NotificationType type)
{
    close_notification_of_type(type);
}

void NotificationManager::bbl_show_filament_map_invalid_notification_after_slice(const NotificationType type, const std::string& text)
{
    auto callback = [](wxEvtHandler*) {
        auto plater = wxGetApp().plater();
        wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
        evt.SetEventObject(plater);
        auto canvas_type = plater->canvas3D()->get_canvas_type();
        if (canvas_type == GLCanvas3D::ECanvasType::CanvasPreview)
            evt.SetInt(1); // 1 means from gcode viewer, should do slice right now
        else
            evt.SetInt(0);
        wxPostEvent(plater, evt);
        return false;
    };

    push_notification_data({ type,NotificationLevel::ErrorNotificationLevel,0,_u8L("Error:") + "\n" + text,_u8L("Click here to regroup"),callback }, 0);
}

void NotificationManager::bbl_close_filament_map_invalid_notification_after_slice(const NotificationType type)
{
    close_notification_of_type(type);
}

void NotificationManager::bbl_show_sole_text_notification(NotificationType sType, const std::string &text, bool bOverride, int level, bool autohide) {

	NotificationLevel nlevel;
    int               nHideTime = 20;
    switch ( level) {
    case 1:
		nlevel = NotificationLevel::WarningNotificationLevel;
		nHideTime = 0;
		break;
    case 2:
		nlevel = NotificationLevel::ErrorNotificationLevel; break;
        nHideTime = 0;
    case 0:
    default:
		nlevel = NotificationLevel::PrintInfoNotificationLevel;

		if (autohide == false) nHideTime = BBL_NOTICE_MAX_INTERVAL;
        break;
    }

    NotificationData data{sType, nlevel, nHideTime, text};
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications) {
        if (notification->get_type() == sType) {
            if (bOverride) {
                notification->update(data);
            }
            else {
                NotificationData sdata{sType, nlevel, nHideTime, notification->get_text1() + "\n" + text};
                notification->update(sdata);
            }

            return;
        }
    }

    auto notification = std::make_unique<NotificationManager::PopNotification>(data, m_id_provider, m_evt_handler);
    notification->set_Multiline(true);
    push_notification_data(std::move(notification), 0);
}
void NotificationManager::bbl_chose_sole_text_notification(NotificationType sType)
{
    for (std::unique_ptr<PopNotification> &notification : m_pop_notifications)
        if (notification->get_type() == sType) { notification->close(); }
}


void NotificationManager::set_scale(float scale)
{
	if(m_scale != scale) m_scale = scale;
	for (auto& notif : m_pop_notifications) {
		notif->set_scale(scale);
	}
}


void NotificationManager::PlaterWarningNotification::close()
{
    if (is_finished())
        return;
    m_state = EState::Hidden;
    wxGetApp().plater()->get_current_canvas3D()->schedule_extra_frame(0);
    if(m_on_delete_callback)
        m_on_delete_callback(this);
}
}}//namespace Slic3r
