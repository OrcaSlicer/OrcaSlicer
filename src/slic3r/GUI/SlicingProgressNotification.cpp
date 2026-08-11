#include "SlicingProgressNotification.hpp"

#include <libslic3r/Gpu/VulkanSlicer.hpp>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

namespace Slic3r { namespace GUI {

namespace {
	inline void push_style_color(ImGuiCol idx, const ImVec4& col, bool fading_out, float current_fade_opacity)
	{
		if (fading_out)
			ImGui::PushStyleColor(idx, ImVec4(col.x, col.y, col.z, col.w * current_fade_opacity));
		else
			ImGui::PushStyleColor(idx, col);
	}

#ifdef _WIN32
	uint64_t filetime_to_100ns(const FILETIME& value)
	{
		ULARGE_INTEGER ticks;
		ticks.LowPart  = value.dwLowDateTime;
		ticks.HighPart = value.dwHighDateTime;
		return ticks.QuadPart;
	}
#endif

	std::string compact_gpu_name(std::string name)
	{
		for (const char* marker : { "RTX ", "GTX ", "Arc", "Radeon" }) {
			const size_t marker_pos = name.find(marker);
			if (marker_pos != std::string::npos) {
				name = name.substr(marker_pos);
				break;
			}
		}

		constexpr size_t maximum_length = 22;
		if (name.size() > maximum_length)
			name = name.substr(0, maximum_length - 3) + "...";
		return name;
	}

	std::string compact_count(uint64_t value)
	{
		std::ostringstream stream;
		if (value >= 1000000)
			stream << std::fixed << std::setprecision(1) << (double(value) / 1000000.0) << "M";
		else if (value >= 1000)
			stream << std::fixed << std::setprecision(1) << (double(value) / 1000.0) << "k";
		else
			stream << value;
		return stream.str();
	}

	std::string compact_gpu_operation(const std::string& operation)
	{
		if (operation == "CUDA exact infill/support intersections")
			return "CUDA infill/support intersections";
		if (operation == "Vulkan exact infill/support edge intersections")
			return "Vulkan infill/support intersections";
		if (operation == "Exact infill/support edge intersections")
			return "infill/support intersections";
		if (operation == "CPU fallback for a small intersection batch")
			return "CPU fallback (small batch)";
		if (operation == "CPU fallback after GPU dispatch failure")
			return "CPU fallback (GPU failed)";
		if (operation == "Wall topology candidate graph")
			return "wall topology candidates";
		if (operation == "CPU fallback for wall topology preflight")
			return "CPU wall topology fallback";
		return operation.empty() ? "preparing compute" : operation;
	}

	std::string compact_stage(const std::string& stage)
	{
		std::string result = stage;
		for (char& character : result)
			if (character == '\n' || character == '\r')
				character = ' ';
		constexpr size_t maximum_length = 28;
		if (result.size() > maximum_length)
			result = result.substr(0, maximum_length - 3) + "...";
		return result;
	}

	std::string format_memory_size(size_t bytes)
	{
		std::ostringstream stream;
		if (bytes >= size_t(1024) * 1024 * 1024)
			stream << std::fixed << std::setprecision(1) << (double(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
		else
			stream << std::fixed << std::setprecision(1) << (double(bytes) / (1024.0 * 1024.0)) << " MiB";
		return stream.str();
	}
}

void NotificationManager::SlicingProgressNotification::on_change_color_mode(bool is_dark)
{
	PopNotification::on_change_color_mode(is_dark);
	m_dailytips_panel->on_change_color_mode(is_dark);
}

void NotificationManager::SlicingProgressNotification::init()
{
	if (m_sp_state == SlicingProgressState::SP_PROGRESS) {
		PopNotification::init();
		// The original notification width was sized for a one-line status. The
		// live resource rows below need enough room for a device and a concise
		// compute-phase label without clipping either one.
		m_window_width = std::max(m_window_width, m_line_height * 56.0f);
		// PopNotification::init() measured status text at the legacy width.
		// Reflow after widening so it does not retain an unnecessary wrap.
		count_lines();
		if (m_endlines.empty()) {
			m_endlines.push_back(0);
		}
		if (m_lines_count >= 2) {
			m_lines_count = std::min((size_t)3, m_lines_count);
			m_multiline = true;
            while (m_endlines.size() < m_lines_count)
				m_endlines.push_back(m_endlines.back());
		}
		else {
			m_lines_count = 1;
			m_multiline = false;
		}
		if (m_state == EState::Shown)
			m_state = EState::NotFading;
	}
	else {
		PopNotification::init();
	}

}

bool NotificationManager::SlicingProgressNotification::set_progress_state(float percent)
{
	if (percent < 0.f)
		return true;//set_progress_state(SlicingProgressState::SP_CANCELLED);
	else if (percent >= 1.f) {
			m_before_complete_start = GLCanvas3D::timestamp_now();
			return set_progress_state(SlicingProgressState::SP_COMPLETED);
	}
	else
		return set_progress_state(SlicingProgressState::SP_PROGRESS, percent);
}

bool NotificationManager::SlicingProgressNotification::set_progress_state(NotificationManager::SlicingProgressNotification::SlicingProgressState state, float percent/* = 0.f*/)
{
	switch (state)
	{
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_NO_SLICING:
        m_state = EState::Hidden;
        set_percentage(-1);
		reset_resource_usage();
        m_has_print_info = false;
        set_export_possible(false);
        m_sp_state             = state;
        return true;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_BEGAN:
		m_state = EState::Hidden;
		set_percentage(-1);
		reset_resource_usage();
		m_has_print_info = false;
		set_export_possible(false);
		m_sp_state = state;
        m_current_fade_opacity = 1;
		return true;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_PROGRESS:
		if ((m_sp_state != SlicingProgressState::SP_BEGAN && m_sp_state != SlicingProgressState::SP_PROGRESS) || percent < m_percentage)
			return false;
		set_percentage(percent);
		m_sp_state = state;
        m_current_fade_opacity = 1;
		return true;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_CANCELLED:
		set_percentage(-1);
		reset_resource_usage();
		m_has_print_info = false;
		set_export_possible(false);
		m_sp_state = state;
		return true;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_COMPLETED:
		if (m_sp_state != SlicingProgressState::SP_BEGAN && m_sp_state != SlicingProgressState::SP_PROGRESS)
			return false;
		set_percentage(1);
		reset_resource_usage();
		m_has_print_info = false;
		// m_export_possible is important only for SP_PROGRESS state, thus we can reset it here
		set_export_possible(false);
		m_sp_state = state;
		return true;
	default:
		break;
	}
	return false;
}

void NotificationManager::SlicingProgressNotification::set_status_text(const std::string& text)
{
	switch (m_sp_state)
	{
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_NO_SLICING:
		m_state = EState::Hidden;
		break;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_PROGRESS:
	{
		NotificationData data{ NotificationType::SlicingProgress, NotificationLevel::ProgressBarNotificationLevel, 0, text + "." };
		update(data);
		m_state = EState::NotFading;
	}
		break;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_CANCELLED:
	{
		NotificationData data{ NotificationType::SlicingProgress, NotificationLevel::ProgressBarNotificationLevel, 0, text };
		update(data);
		m_state = EState::Shown;
	}
		break;
	case Slic3r::GUI::NotificationManager::SlicingProgressNotification::SlicingProgressState::SP_COMPLETED:
	{
		NotificationData data{ NotificationType::SlicingProgress, NotificationLevel::ProgressBarNotificationLevel, 0,  _u8L("Slice complete") };
		update(data);
		m_state = EState::Shown;
	}
		break;
	default:
		break;
	}
}

void NotificationManager::SlicingProgressNotification::set_print_info(const std::string& info)
{
	if (m_sp_state != SlicingProgressState::SP_COMPLETED) {
		set_progress_state (SlicingProgressState::SP_COMPLETED);
	} else {
		m_has_print_info = true;
		m_print_info = info;
	}
}

void NotificationManager::SlicingProgressNotification::set_sidebar_collapsed(bool collapsed)
{
	m_sidebar_collapsed = collapsed;
	if (m_sp_state == SlicingProgressState::SP_COMPLETED && collapsed)
		m_state = EState::NotFading;
}

void NotificationManager::SlicingProgressNotification::on_cancel_button()
{
	if (m_cancel_callback){
		if (!m_cancel_callback()) {
			set_progress_state(SlicingProgressState::SP_NO_SLICING);
		}
	}
}

int NotificationManager::SlicingProgressNotification::get_duration()
{
	if (m_sp_state == SlicingProgressState::SP_CANCELLED)
		return 3;
	else if (m_sp_state == SlicingProgressState::SP_COMPLETED)
		return 3;
	else
		return 0;
}

bool  NotificationManager::SlicingProgressNotification::update_state(bool paused, const int64_t delta)
{
	bool ret = PopNotification::update_state(paused, delta);
	if (m_sp_state == SlicingProgressState::SP_COMPLETED)
		ret = true;

	// sets Estate to hidden
	if (get_state() == PopNotification::EState::ClosePending || get_state() == PopNotification::EState::Finished)
		set_progress_state(SlicingProgressState::SP_NO_SLICING);
	return ret;
}

void NotificationManager::SlicingProgressNotification::render(GLCanvas3D& canvas, float initial_y, bool move_from_overlay, float overlay_width, float right_margin)
{
	if (m_state == EState::Unknown || m_state == PopNotification::EState::Hovered)
		init();

	ImGuiWrapper& imgui = *wxGetApp().imgui();
	float scale = imgui.get_font_size() / 15.0f;
	if (m_sp_state == SlicingProgressState::SP_PROGRESS)
		update_resource_usage();

	bool fading_pop = false;
	if (m_state == EState::FadingOut) {
		push_style_color(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text), true, m_current_fade_opacity);
		push_style_color(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), true, m_current_fade_opacity);
		fading_pop = true;
	}
	use_bbl_theme();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 0));
	// for debug
	//ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, m_WindowRadius / 4);
	//m_is_dark ? push_style_color(ImGuiCol_Border, { 62 / 255.f, 62 / 255.f, 69 / 255.f, 1.f }, true, m_current_fade_opacity) : push_style_color(ImGuiCol_Border, m_CurrentColor, true, m_current_fade_opacity);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	push_style_color(ImGuiCol_Border, { 0, 0, 0, 0 }, true, m_current_fade_opacity);

	Size cnv_size = canvas.get_canvas_size();

	//m_window_width = 600.f * scale;
	//if (m_sp_state == SlicingProgressNotification::SlicingProgressState::SP_COMPLETED || m_sp_state == SlicingProgressNotification::SlicingProgressState::SP_CANCELLED)
	//	m_window_width = m_line_height * 25;
	const ImVec2 progress_child_window_padding = ImVec2(15.f, 0.f) * scale;
	const ImVec2 dailytips_child_window_padding = m_dailytips_panel->is_expanded() ? ImVec2(15.f, 10.f) * scale : ImVec2(15.f, 0.f) * scale;
	const ImVec2 bottom_padding = ImVec2(0.f, 0.f) * scale;
	const float  progress_panel_width = (m_window_width - 2 * progress_child_window_padding.x);
	const float  resource_lines = m_sp_state == SlicingProgressState::SP_PROGRESS ? 4.5f : 0.0f;
	const float  progress_panel_height = (58.0f * scale) + (m_lines_count - 1 + resource_lines) * m_line_height;
	const float  dailytips_panel_width = (m_window_width - 2 * dailytips_child_window_padding.x);
	const float  gcodeviewer_height = wxGetApp().plater()->get_preview_canvas3D()->get_gcode_viewer().get_legend_height();
	//const float  dailytips_panel_height = std::min(380.0f * scale, std::max(90.0f, (cnv_size.get_height() - gcodeviewer_height - progress_panel_height - dailytips_child_window_padding.y - initial_y - m_line_height * 4)));
	const float  dailytips_panel_height = 125.0f * scale;

	float right_gap = right_margin + (move_from_overlay ? overlay_width + m_line_height * 5 : 0);
	m_window_pos = ImVec2((float)cnv_size.get_width() - right_gap - m_window_width, (float)cnv_size.get_height() - m_top_y);
	imgui.set_next_window_pos(m_window_pos.x, m_window_pos.y, ImGuiCond_Always, 0.0f, 0.0f);
	m_window_height = progress_panel_height + m_dailytips_panel->get_size().y + progress_child_window_padding.y + dailytips_child_window_padding.y + bottom_padding.y;
	m_top_y = initial_y + m_window_height;
	ImGui::SetNextWindowSizeConstraints(ImVec2(m_window_width, m_window_height), ImVec2(m_window_width, m_window_height));

	// name of window indentifies window - has to be unique string
	if (m_id == 0)
		m_id = m_id_provider.allocate_id();
	std::string name = "!!Ntfctn" + std::to_string(m_id);
	int window_flags = ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse;
	int child_window_flags = ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse;
	if (imgui.begin(name, window_flags)) {
		ImGuiWindow* parent_window = ImGui::GetCurrentWindow();

		//if (m_sp_state == SlicingProgressState::SP_CANCELLED || m_sp_state == SlicingProgressState::SP_COMPLETED) {
		//	ImVec2 button_size = ImVec2(38.f, 38.f) * scale;
		//	float  button_right_margin_x = 3.0f * scale;
		//	ImVec2 button_pos = m_window_pos + ImVec2(m_window_width - button_size.x - button_right_margin_x, (m_window_height - button_size.y) / 2.0f);
		//	float  text_left_margin_x = 15.0f * scale;
		//	ImVec2 text_pos = m_window_pos + ImVec2(text_left_margin_x, m_window_height / 2.0f - m_line_height * 1.2f);
		//	ImVec2 view_dailytips_text_pos = m_window_pos + ImVec2(text_left_margin_x, m_window_height / 2.0f + m_line_height * 0.2f);

		//	bbl_render_left_sign(imgui, m_window_width, m_window_height, m_window_pos.x + m_window_width, m_window_pos.y);
		//	render_text(text_pos);
		//	render_close_button(button_pos, button_size);
		//	render_show_dailytips(view_dailytips_text_pos);
		//}

		if (m_sp_state == SlicingProgressState::SP_CANCELLED || m_sp_state == SlicingProgressState::SP_PROGRESS ||  m_sp_state == SlicingProgressState::SP_COMPLETED) {
			std::string child_name = "##SlicingProgressPanel" + std::to_string(parent_window->ID);

			ImGui::SetNextWindowPos(parent_window->Pos + progress_child_window_padding);
			if (ImGui::BeginChild(child_name.c_str(), ImVec2(progress_panel_width, progress_panel_height), false, child_window_flags)) {
				ImVec2 child_window_pos = ImGui::GetWindowPos();
				ImVec2 button_size = ImVec2(38.f, 38.f) * scale;
				float  margin_x = 8.0f * scale;
				ImVec2 progress_bar_size = ImVec2(progress_panel_width - button_size.x - margin_x, 4.0f * scale);
                float  text_bottom = progress_bar_size.y + m_line_height * 1.2f + 7.f * scale;
                ImVec2 progress_bar_pos = child_window_pos + ImVec2(0, progress_panel_height - text_bottom);
				ImVec2 button_pos = child_window_pos + ImVec2(progress_panel_width - button_size.x, progress_panel_height - text_bottom - button_size.y / 2.0f);
				ImVec2 text_pos = ImVec2(progress_bar_pos.x, progress_bar_pos.y - m_line_height * (1.2f + resource_lines + m_lines_count - 1));

				render_text(text_pos);
				render_close_button(button_pos, button_size);
				if (m_sp_state == SlicingProgressState::SP_PROGRESS) {
					render_resource_usage(ImVec2(progress_bar_pos.x, progress_bar_pos.y - m_line_height * 4.35f));
					render_bar(progress_bar_pos, progress_bar_size);
					render_cancel_button(button_pos, button_size);
				}
			}
			ImGui::EndChild();

			// Separator Line
			ImVec2 separator_min = ImVec2(ImGui::GetCursorScreenPos().x + progress_child_window_padding.x, ImGui::GetCursorScreenPos().y);
			ImVec2 separator_max = ImVec2(ImGui::GetCursorScreenPos().x + progress_child_window_padding.x + progress_panel_width, ImGui::GetCursorScreenPos().y);
			ImGui::GetCurrentWindow()->DrawList->AddLine(separator_min, separator_max, ImColor(238, 238, 238, (int)(255 * m_current_fade_opacity)));

			child_name = "##DailyTipsPanel" + std::to_string(parent_window->ID);
			ImVec2 dailytips_pos = ImGui::GetCursorScreenPos() + dailytips_child_window_padding;
			ImVec2 dailytips_size = ImVec2(dailytips_panel_width, dailytips_panel_height);
			m_dailytips_panel->set_position(dailytips_pos);
			m_dailytips_panel->set_size(dailytips_size);
			m_dailytips_panel->set_fade_opacity(m_current_fade_opacity);
			ImGui::SetNextWindowPos(dailytips_pos);
			if (ImGui::BeginChild(child_name.c_str(), ImVec2(dailytips_panel_width, dailytips_panel_height), false, child_window_flags)) {
				render_dailytips_panel(dailytips_pos, dailytips_size);
			}
			ImGui::EndChild();
		}

		if (ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize(), true)) {
			set_hovered();
		}
	}
	imgui.end();


	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(1);
	restore_default_theme();
	if (fading_pop)
		ImGui::PopStyleColor(3);
}

void NotificationManager::SlicingProgressNotification::reset_resource_usage()
{
	m_last_resource_sample = {};
	m_last_process_cpu_time_100ns = 0;
	m_logical_processor_count = 0;
	m_resource_monitor_initialized = false;
	m_cpu_resource_text.clear();
	m_gpu_resource_text.clear();
	m_gpu_activity_text.clear();
	m_memory_resource_text.clear();
}

void NotificationManager::SlicingProgressNotification::update_resource_usage()
{
	const auto now = std::chrono::steady_clock::now();
	if (m_resource_monitor_initialized &&
		now - m_last_resource_sample < std::chrono::milliseconds(500))
		return;

	std::ostringstream cpu_text;
	bool cpu_sample_available = false;

#ifdef _WIN32
	FILETIME creation_time, exit_time, kernel_time, user_time;
	if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
		const uint64_t process_cpu_time = filetime_to_100ns(kernel_time) + filetime_to_100ns(user_time);
		if (!m_resource_monitor_initialized) {
			SYSTEM_INFO system_info;
			GetSystemInfo(&system_info);
			m_logical_processor_count = std::max<uint32_t>(1, system_info.dwNumberOfProcessors);
			m_last_process_cpu_time_100ns = process_cpu_time;
			cpu_text << "CPU: sampling / " << m_logical_processor_count << " logical cores";
			cpu_sample_available = true;
		} else {
			const auto elapsed = std::chrono::duration<double>(now - m_last_resource_sample).count();
			if (elapsed > 0.0 && process_cpu_time >= m_last_process_cpu_time_100ns) {
				const double process_seconds = double(process_cpu_time - m_last_process_cpu_time_100ns) / 10000000.0;
				const double cpu_percent = std::min(100.0, 100.0 * process_seconds /
					(elapsed * std::max<uint32_t>(1, m_logical_processor_count)));
				cpu_text << std::fixed << std::setprecision(1)
					<< "CPU: " << cpu_percent << "% / "
					<< m_logical_processor_count << " logical cores / "
					<< compact_stage(m_text1);
				cpu_sample_available = true;
			}
			m_last_process_cpu_time_100ns = process_cpu_time;
		}
	}
#endif

	if (!cpu_sample_available)
		cpu_text << "CPU: unavailable";

	const Gpu::VulkanSlicerRuntimeStats gpu = Gpu::VulkanSlicerBackend::query_runtime_stats();
	const auto backend_mode = Gpu::VulkanSlicerBackend::backend_preference();
	std::ostringstream gpu_text;
	std::ostringstream gpu_activity_text;
	if (backend_mode == Gpu::ComputeBackendPreference::Cpu ||
		(!Gpu::VulkanSlicerBackend::cuda_enabled() && !Gpu::VulkanSlicerBackend::compute_enabled())) {
		gpu_text << "GPU: disabled (CPU/basic mode)";
		gpu_activity_text << "GPU job: none / CPU exact geometry";
	} else if (gpu.selected_device.empty()) {
		gpu_text << "GPU: " << (backend_mode == Gpu::ComputeBackendPreference::Cuda ? "CUDA" : "Vulkan") << " initializing";
		gpu_activity_text << "GPU job: preparing exact compute / batch "
			<< Gpu::VulkanSlicerBackend::batch_size();
	} else if (gpu.dispatch_calls == 0) {
		gpu_text << "GPU: " << compact_gpu_name(gpu.selected_device) << " / "
			<< (gpu.active_backend.empty() ? "GPU" : gpu.active_backend) << " ready, idle";
		if (gpu.skipped_small_workloads != 0) {
			gpu_activity_text << "GPU job: CPU kept "
				<< compact_count(gpu.skipped_small_workloads) << " small batches";
		} else {
			gpu_activity_text << "GPU job: no eligible batch / configured "
				<< Gpu::VulkanSlicerBackend::batch_size();
		}
	} else {
		gpu_text << "GPU: " << compact_gpu_name(gpu.selected_device) << " / "
			<< (gpu.active_backend.empty() ? "GPU" : gpu.active_backend) << " ready";
		gpu_activity_text << "GPU last: " << compact_gpu_operation(gpu.current_operation)
			<< " / batches " << compact_count(gpu.dispatch_calls)
			<< " / requests " << compact_count(gpu.submitted_intersections);
	}

	m_cpu_resource_text = cpu_text.str();
	m_gpu_resource_text = gpu_text.str();
	m_gpu_activity_text = gpu_activity_text.str();

	std::ostringstream memory_text;
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX memory_counters {};
	memory_counters.cb = sizeof(memory_counters);
	if (GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory_counters), sizeof(memory_counters))) {
		const size_t vulkan_staging_bytes = gpu.reusable_staging_capacity * (48 + 32);
		memory_text << "RAM: " << format_memory_size(memory_counters.WorkingSetSize)
			<< " W / " << format_memory_size(memory_counters.PrivateUsage)
			<< " P / staging " << format_memory_size(vulkan_staging_bytes);
	} else {
		memory_text << "RAM: unavailable";
	}
#else
	memory_text << "RAM: platform monitor unavailable";
#endif
	m_memory_resource_text = memory_text.str();
	m_last_resource_sample = now;
	m_resource_monitor_initialized = true;
}

void NotificationManager::SlicingProgressNotification::render_resource_usage(const ImVec2& pos)
{
	ImGuiWrapper& imgui = *wxGetApp().imgui();
	ImGui::SetCursorScreenPos(pos);
	imgui.text(m_cpu_resource_text.c_str());
	ImGui::SetCursorScreenPos(pos + ImVec2(0.0f, m_line_height * 1.1f));
	imgui.text(m_gpu_resource_text.c_str());
	ImGui::SetCursorScreenPos(pos + ImVec2(0.0f, m_line_height * 2.2f));
	imgui.text(m_gpu_activity_text.c_str());
	ImGui::SetCursorScreenPos(pos + ImVec2(0.0f, m_line_height * 3.3f));
	imgui.text(m_memory_resource_text.c_str());
}

void Slic3r::GUI::NotificationManager::SlicingProgressNotification::render_text(const ImVec2& pos)
{
	ImGuiWrapper& imgui = *wxGetApp().imgui();
	float scale = imgui.get_font_size() / 15.0f;
	ImVec2 icon_size = ImVec2(38.f, 38.f) * scale;
	if (m_sp_state == SlicingProgressState::SP_COMPLETED) {
		// complete icon
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
		push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

		ImGui::SetCursorScreenPos(pos);
		std::wstring icon_text;
		icon_text = ImGui::CompleteIcon;
		imgui.button(icon_text.c_str());

		ImGui::PopStyleColor(5);

		// complete text
		imgui.push_bold_font();
		ImGui::SetCursorScreenPos(ImVec2(pos.x + icon_size.x + ImGui::CalcTextSize(" ").x, pos.y + (icon_size.y - m_line_height) / 2));
		imgui.text(m_text1.substr(0, m_endlines[0]).c_str());
		imgui.pop_bold_font();
		return;
	}
	if (m_sp_state == SlicingProgressState::SP_CANCELLED) {
		imgui.push_bold_font();
		ImGui::SetCursorScreenPos(ImVec2(pos.x + ImGui::CalcTextSize(" ").x, pos.y + (icon_size.y - m_line_height) / 2));
		imgui.text(m_text1.substr(0, m_endlines[0]).c_str());
		imgui.pop_bold_font();
	}
	if(m_sp_state == SlicingProgressState::SP_PROGRESS)	{
		// multi-line text
        int last_end = 0;
        for (auto i = 0; i < m_lines_count; i++) {
            ImGui::SetCursorScreenPos(pos + ImVec2(0, i * m_line_height));
            imgui.text(m_text1.substr(last_end, m_endlines[i] - last_end).c_str());
            last_end = m_endlines[i];
        }
	}
}

void Slic3r::GUI::NotificationManager::SlicingProgressNotification::render_bar(const ImVec2& pos, const ImVec2& size)
{
	if (m_sp_state != SlicingProgressState::SP_PROGRESS)
		return;

	ImGuiWrapper& imgui = *wxGetApp().imgui();

	ImColor progress_color = ImColor(0, 150, 136, (int)(255 * m_current_fade_opacity));
	ImColor bg_color = ImColor(217, 217, 217, (int)(255 * m_current_fade_opacity));

	ImVec2 lineStart = pos;
	ImVec2 lineEnd = lineStart + size;
	ImVec2 midPoint = ImVec2(lineStart.x + (lineEnd.x - lineStart.x) * m_percentage, lineEnd.y);
	ImGui::GetWindowDrawList()->AddRectFilled(lineStart, lineEnd, bg_color);
	ImGui::GetWindowDrawList()->AddRectFilled(lineStart, midPoint, progress_color);
	
	// percentage text
	ImVec2 text_pos = ImVec2(pos.x, pos.y + size.y + m_line_height * 0.2f);
	std::string text;
	std::stringstream stream;
	stream << std::fixed << std::setprecision(2) << (int)(m_percentage * 100) << "%";
	text = stream.str();
	ImGui::SetCursorScreenPos(text_pos);
	imgui.text(text.c_str());
}

void NotificationManager::SlicingProgressNotification::render_dailytips_panel(const ImVec2& pos, const ImVec2& size)
{
	m_dailytips_panel->render();
}

void NotificationManager::SlicingProgressNotification::render_show_dailytips(const ImVec2& pos)
{
	if (m_sp_state != SlicingProgressState::SP_COMPLETED && m_sp_state != SlicingProgressState::SP_CANCELLED)
		return;

	ImGuiWrapper& imgui = *wxGetApp().imgui();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImColor(31, 142, 234).Value);

	ImGui::SetCursorScreenPos(pos);
	std::wstring button_text;
	button_text = ImGui::OpenArrowIcon;
	imgui.button(_L("View all Daily tips") + " " + button_text);
	//click behavior
	if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true))
	{
		//underline
		ImVec2 lineEnd = ImGui::GetItemRectMax();
		lineEnd.x -= ImGui::CalcTextSize("A").x / 2;
		lineEnd.y -= 2;
		ImVec2 lineStart = lineEnd;
		lineStart.x = ImGui::GetItemRectMin().x;
		ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, ImColor(31, 142, 234));

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			on_show_dailytips();
	}

	ImGui::PopStyleColor(4);
}

void NotificationManager::SlicingProgressNotification::on_show_dailytips()
{
	wxGetApp().plater()->get_dailytips()->open();
}

void Slic3r::GUI::NotificationManager::SlicingProgressNotification::render_cancel_button(const ImVec2& pos, const ImVec2& size)
{
	if (m_sp_state == SlicingProgressState::SP_PROGRESS) {
		ImGuiWrapper& imgui = *wxGetApp().imgui();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
		push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

		ImVec2 button_size = size;
		ImVec2 button_pos = pos;
		ImGui::SetCursorScreenPos(button_pos);

		std::wstring button_text;
		button_text = ImGui::CancelButton;
		if (ImGui::IsMouseHoveringRect(button_pos, button_pos + button_size, true))
		{
			button_text = ImGui::CancelHoverButton;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				on_cancel_button();
		}
		imgui.button(button_text.c_str());

		ImGui::PopStyleColor(5);
	}
}

void NotificationManager::SlicingProgressNotification::render_close_button(const ImVec2& pos, const ImVec2& size)
{
	if (m_sp_state == SlicingProgressState::SP_CANCELLED || m_sp_state == SlicingProgressState::SP_COMPLETED) {
		ImGuiWrapper& imgui = *wxGetApp().imgui();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
		push_style_color(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		push_style_color(ImGuiCol_TextSelectedBg, ImVec4(0, .75f, .75f, 1.f), m_state == EState::FadingOut, m_current_fade_opacity);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));

		ImVec2 button_size = size;
		ImVec2 button_pos = pos;
		ImGui::SetCursorScreenPos(button_pos);

		std::wstring button_text;
		button_text = m_is_dark ? ImGui::CloseNotifDarkButton : ImGui::CloseNotifButton;
		if (ImGui::IsMouseHoveringRect(button_pos, button_pos + button_size, true))
		{
			button_text = m_is_dark ? ImGui::CloseNotifHoverDarkButton : ImGui::CloseNotifHoverButton;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				close();
		}
		imgui.button(button_text.c_str());

		ImGui::PopStyleColor(5);
	}
}

}}
