#pragma once

#include "AIModelGenerationClient.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <ctime>
#include <array>
#include <functional>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxCollapsiblePane;
class wxColourPickerCtrl;
class wxGauge;
class wxGridSizer;
class wxNotebook;
class wxScrolledWindow;
class wxSpinCtrlDouble;
class wxStaticText;
class wxTextCtrl;
class wxToggleButton;

namespace Slic3r::GUI {

class ModelPreview3D;

class ModelGenerationPanel : public wxPanel
{
public:
    ModelGenerationPanel(wxWindow* parent, AI::IModelArtifactConsumer& artifact_consumer,
                         AI::IPrintablePaletteProvider& palette_provider);
    ~ModelGenerationPanel() override;

    void shutdown();
    void set_service_availability(bool available, const std::string& message = {});
    void set_service_retry_handler(std::function<void()> handler);

private:
    void build_page();
    wxWindow* build_workflow_panel(wxWindow* parent);
    wxWindow* build_preview_panel(wxWindow* parent);
    wxWindow* build_model_library(wxWindow* parent);

    void on_choose_image(wxCommandEvent& event);
    void on_clear_image(wxCommandEvent& event);
    void on_printable_colors_toggled(wxCommandEvent& event);
    void on_palette_source_changed(wxCommandEvent& event);
    void on_add_custom_color(wxCommandEvent& event);
    void on_recommend_palette(wxCommandEvent& event);
    void on_confirm_recommended_palette(wxCommandEvent& event);
    void on_palette_role_changed(size_t role_index);
    void on_preprocess(wxCommandEvent& event);
    void on_generate(wxCommandEvent& event);
    void on_retexture_from_library(const std::string& geometry_job_id, const wxString& title);
    void on_stop(wxCommandEvent& event);
    void on_import(wxCommandEvent& event);
    void on_recheck_model(wxCommandEvent& event);
    void on_visual_review_model(wxCommandEvent& event);
    void on_retry_service(wxCommandEvent& event);
    void on_apply_model_refinement(wxCommandEvent& event);
    void on_apply_local_recolor(wxCommandEvent& event);
    void on_discard(wxCommandEvent& event);
    void on_poll(wxTimerEvent& event);

    void handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence);
    void handle_error(const std::string& error, uint64_t sequence);
    void handle_poll_error(const std::string& error, uint64_t sequence);
    void schedule_poll();
    void restore_latest_job();
    void restore_job(AIModelGenerationClient::JobStatus status, uint64_t sequence);
    void download_restored_input(uint64_t sequence);
    void update_adaptive_text_height(wxTextCtrl* control, int minimum_lines, int maximum_lines);
    void refresh_controls();
    void refresh_palette();
    std::vector<size_t> valid_project_slots() const;
    std::vector<size_t> compatible_project_slots() const;
    std::vector<std::string> project_palette() const;
    std::vector<std::string> current_palette() const;
    size_t current_palette_color_count() const;
    AIModelGenerationClient::PaletteRoles current_palette_roles() const;
    void refresh_palette_roles(const std::vector<std::string>& palette);
    void refresh_palette_recommendation();
    void replace_recommended_color(size_t index);
    void request_style_recommendation();
    void select_style(const std::string& style, bool user_selected);
    void refresh_style_recommendation();
    bool use_printable_colors() const;
    std::string current_style() const;
    std::string current_custom_style() const;
    wxString current_style_label() const;
    std::string current_generation_profile() const;
    wxString current_generation_profile_label() const;
    int current_face_limit() const;
    AIModelGenerationClient::ImagePrintSettings current_print_settings() const;
    bool has_image_input() const;
    bool job_uses_image() const;
    bool job_inputs_match() const;
    bool job_base_inputs_match() const;
    void remove_custom_color(const std::string& color);
    void reset(bool remove_remote);
    void download_preview(uint64_t sequence);
    void download_model_preview(uint64_t sequence);
    void finish_model_preview_download(const boost::filesystem::path& path, uint64_t sequence);
    void download_and_import();
    void import_local_artifact(const boost::filesystem::path& path, uint64_t sequence);
    void cleanup_files();
    void set_preview_empty(const wxString& message);
    void show_selected_image_preview();
    void update_preview_view(bool center = false);
    void apply_preview_stage(bool center = false);
    void download_auxiliary_previews(uint64_t sequence, int stage = 0);
    void set_preview_zoom(double zoom);
    void update_progress(int value, int step, const wxString& phase);
    void update_workflow(const AIModelGenerationClient::JobStatus* status = nullptr);
    void apply_model_quality(const AIModelGenerationClient::ModelQuality& quality);
    void apply_visual_quality(const AIModelGenerationClient::VisualQuality& quality);
    void apply_model_refinement(const AIModelGenerationClient::ModelRefinementAdvice& refinement);
    void clear_model_quality();
    void refresh_model_quality_card();
    void refresh_local_recolor_controls();
    std::vector<std::string> local_recolor_palette() const;
    struct GeneratedModelEntry;
    void load_library_entries();
    void save_library_entry(size_t artifact_size, size_t triangle_count, double width, double depth,
                            double height, size_t color_count, double load_seconds);
    void load_library_entry(const boost::filesystem::path& model_path,
                             const boost::filesystem::path& reference_image_path,
                             const boost::filesystem::path& ai_image_path,
                             const std::vector<std::string>& palette,
                             const AIModelGenerationClient::PaletteRoles& palette_roles,
                             bool use_printable_colors,
                             const boost::filesystem::path& color_intent_path,
                             const std::string& color_intent_schema,
                             const std::string& color_intent_sha256,
                             const std::string& job_id, const wxString& title);
    void delete_library_entry(const GeneratedModelEntry& entry);
    void update_library_provider_tasks(const std::string& job_id,
                                       const AIModelGenerationClient::JobStatus& status);
    void update_library_import_status(const std::string& job_id);
    void record_library_print_feedback(const std::string& job_id, const std::string& feedback);
    void refresh_library();

    struct GeneratedModelEntry
    {
        wxString title;
        wxString details;
        boost::filesystem::path model_path;
        boost::filesystem::path preview_path;
        boost::filesystem::path reference_image_path;
        boost::filesystem::path ai_image_path;
        boost::filesystem::path color_intent_path;
        std::vector<std::string> palette;
        AIModelGenerationClient::PaletteRoles palette_roles;
        std::string color_intent_schema;
        std::string color_intent_sha256;
        std::string job_id;
        std::string provider_name;
        std::string provider_task_id;
        std::string provider_conversion_task_id;
        std::time_t generated_at { 0 };
        std::time_t imported_at { 0 };
        size_t triangle_count { 0 };
        double load_seconds { 0.0 };
        std::string print_feedback;
        bool use_printable_colors { false };
    };

    AI::IModelArtifactConsumer&    m_artifact_consumer;
    AI::IPrintablePaletteProvider& m_palette_provider;
    AIModelGenerationClient m_client;

    wxStaticText*   m_prompt_label { nullptr };
    wxTextCtrl*     m_prompt { nullptr };
    wxChoice*       m_style { nullptr };
    wxChoice*       m_stylized_style { nullptr };
    wxPanel*        m_style_recommendation_panel { nullptr };
    wxStaticText*   m_style_recommendation_title { nullptr };
    wxStaticText*   m_style_recommendation_reason { nullptr };
    wxStaticText*   m_style_recommendation_alternative_label { nullptr };
    std::array<wxButton*, 2> m_style_recommendation_alternatives { nullptr, nullptr };
    wxPanel*        m_custom_style_panel { nullptr };
    wxTextCtrl*     m_custom_style { nullptr };
    wxChoice*       m_quality { nullptr };
    wxButton*       m_choose_image { nullptr };
    wxButton*       m_clear_image { nullptr };
    wxStaticText*   m_selected_image { nullptr };
    wxStaticText*   m_upload_notice { nullptr };
    wxPanel*        m_palette_panel { nullptr };
    wxPanel*        m_custom_color_panel { nullptr };
    wxGridSizer*    m_palette_sizer { nullptr };
    wxCheckBox*     m_use_printable_colors { nullptr };
    wxChoice*       m_palette_source { nullptr };
    wxChoice*       m_import_color_mode { nullptr };
    wxColourPickerCtrl* m_custom_color { nullptr };
    wxButton*       m_add_custom_color { nullptr };
    wxStaticText*   m_palette_summary { nullptr };
    wxPanel*        m_palette_recommendation_panel { nullptr };
    wxChoice*       m_palette_color_count { nullptr };
    wxButton*       m_recommend_palette { nullptr };
    wxButton*       m_confirm_recommended_palette { nullptr };
    wxStaticText*   m_palette_recommendation_summary { nullptr };
    std::array<wxPanel*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_recommendation_cards {};
    std::array<wxPanel*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_recommendation_swatches {};
    std::array<wxStaticText*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_recommendation_details {};
    std::array<wxButton*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_recommendation_replace {};
    std::array<wxButton*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_recommendation_remove {};
    wxPanel*        m_palette_roles_panel { nullptr };
    std::array<wxChoice*, Slic3r::AI::kMaxTargetPaletteColors> m_palette_role_choices {};
    wxPanel*        m_model_settings_panel { nullptr };
    wxPanel*        m_import_settings_panel { nullptr };
    wxButton*       m_advanced_toggle { nullptr };
    wxPanel*        m_advanced_options { nullptr };
    wxSpinCtrlDouble* m_print_width { nullptr };
    wxSpinCtrlDouble* m_nozzle_size { nullptr };
    wxSpinCtrlDouble* m_line_width { nullptr };
    wxSpinCtrlDouble* m_minimum_feature { nullptr };
    wxChoice*       m_shadow_color { nullptr };
    wxStaticText*   m_preprocess_section { nullptr };
    wxButton*       m_preprocess { nullptr };
    wxStaticText*   m_prepared_prompt_label { nullptr };
    wxTextCtrl*     m_prepared_prompt { nullptr };
    wxStaticText*   m_workflow_steps { nullptr };
    std::array<wxStaticText*, 4> m_step_labels { nullptr, nullptr, nullptr, nullptr };
    wxStaticText*   m_workflow_phase { nullptr };
    wxStaticText*   m_preview_kind { nullptr };
    wxChoice*       m_preview_stage { nullptr };
    wxStaticText*   m_preview_stage_hint { nullptr };
    wxCollapsiblePane* m_preview_details_pane { nullptr };
    wxStaticText*   m_preview_technical_details { nullptr };
    wxNotebook*     m_preview_book { nullptr };
    wxButton*       m_zoom_out { nullptr };
    wxButton*       m_zoom_fit { nullptr };
    wxButton*       m_zoom_in { nullptr };
    wxStaticText*   m_preview_zoom { nullptr };
    wxStaticText*   m_preview_message { nullptr };
    wxStaticText*   m_result_summary { nullptr };
    ModelPreview3D* m_model_preview { nullptr };
    wxStaticText*   m_model_preview_message { nullptr };
    wxStaticText*   m_model_stats { nullptr };
    wxButton*       m_front_model_view { nullptr };
    wxButton*       m_reset_model_view { nullptr };
    wxPanel*        m_local_recolor_panel { nullptr };
    wxToggleButton* m_local_recolor_toggle { nullptr };
    wxPanel*        m_local_recolor_controls { nullptr };
    std::array<wxToggleButton*, 3> m_region_operation_buttons { nullptr, nullptr, nullptr };
    wxChoice*       m_region_range { nullptr };
    std::array<wxButton*, Slic3r::AI::kMaxTargetPaletteColors> m_region_material_buttons {};
    std::array<wxToggleButton*, Slic3r::AI::kMaxPhysicalColorChannels> m_region_color_buttons {};
    wxStaticText*   m_region_selection_summary { nullptr };
    wxButton*       m_undo_region_selection { nullptr };
    wxButton*       m_clear_region_selection { nullptr };
    wxButton*       m_apply_region_color { nullptr };
    int             m_region_operation_index { 0 };
    int             m_region_color_index { 0 };
    std::vector<std::string> m_region_palette;
    wxPanel*        m_model_decision_panel { nullptr };
    wxStaticText*   m_model_decision_status { nullptr };
    wxStaticText*   m_model_decision_summary { nullptr };
    wxCollapsiblePane* m_model_advanced_pane { nullptr };
    wxPanel*        m_model_quality_panel { nullptr };
    wxStaticText*   m_model_quality_status { nullptr };
    wxStaticText*   m_model_quality_summary { nullptr };
    wxCollapsiblePane* m_model_quality_details_pane { nullptr };
    wxStaticText*   m_model_quality_details { nullptr };
    wxButton*       m_recheck_model { nullptr };
    wxButton*       m_locate_thin_regions { nullptr };
    wxButton*       m_locate_overhang_regions { nullptr };
    bool            m_thin_region_navigation_active { false };
    size_t          m_thin_region_navigation_index { 0 };
    wxStaticText*   m_visual_quality_status { nullptr };
    wxStaticText*   m_visual_quality_summary { nullptr };
    wxButton*       m_visual_review_model { nullptr };
    wxPanel*        m_model_refinement_panel { nullptr };
    wxStaticText*   m_model_refinement_status { nullptr };
    wxStaticText*   m_model_refinement_summary { nullptr };
    wxButton*       m_apply_model_refinement { nullptr };
    wxButton*       m_generate { nullptr };
    wxButton*       m_stop { nullptr };
    wxButton*       m_retry_service { nullptr };
    wxStaticText*   m_progress_percent { nullptr };
    wxGauge*        m_generation_progress { nullptr };
    wxStaticText*   m_status { nullptr };
    wxButton*       m_import { nullptr };
    wxButton*       m_discard { nullptr };
    wxScrolledWindow* m_preview_area { nullptr };
    wxScrolledWindow* m_library_scroller { nullptr };
    wxBoxSizer*     m_library_sizer { nullptr };
    wxStaticText*   m_library_empty { nullptr };
    wxTimer         m_poll_timer;

    boost::filesystem::path m_selected_image_path;
    boost::filesystem::path m_job_image_path;
    boost::filesystem::path m_preview_path;
    boost::filesystem::path m_reference_image_path;
    boost::filesystem::path m_raw_preview_path;
    boost::filesystem::path m_artifact_path;
    boost::filesystem::path m_color_intent_path;
    boost::filesystem::path m_displayed_model_path;
    wxImage m_reference_image;
    wxImage m_raw_preview_image;
    wxImage m_model_reference_image;
    wxImage m_strict_preview_image;
    wxImage m_model_views_image;
    wxImage m_clean_preview_image;
    wxImage m_heatmap_image;
    wxImage m_style_preview_image;
    wxBitmap m_reference_bitmap;
    wxBitmap m_style_preview_bitmap;
    wxRect m_reference_preview_pane;
    wxRect m_style_preview_pane;
    wxString m_style_preview_placeholder;
    std::vector<GeneratedModelEntry> m_library_entries;
    std::vector<std::string> m_palette;
    std::vector<std::string> m_custom_palette;
    std::vector<std::string> m_job_palette;
    AIModelGenerationClient::PaletteRoles m_palette_roles;
    AIModelGenerationClient::PaletteRoles m_job_palette_roles;
    std::vector<std::string> m_palette_roles_source;
    std::vector<std::string> m_displayed_model_palette;
    AIModelGenerationClient::PaletteRoles m_displayed_model_palette_roles;
    AIModelGenerationClient::PaletteRecommendation m_palette_recommendation;
    AIModelGenerationClient::StyleRecommendation m_style_recommendation;
    std::vector<std::string> m_user_adjusted_palette_colors;
    std::string m_palette_recommendation_job_id;
    wxString m_job_prompt;
    std::string m_job_style;
    std::string m_job_custom_style;
    AIModelGenerationClient::ImagePrintSettings m_job_print_settings;
    size_t m_job_palette_color_count { Slic3r::AI::kLegacyDefaultTargetPaletteColors };
    int m_job_face_limit { 2000000 };
    std::string m_job_generation_profile { "quality" };
    std::string m_job_id;
    std::string m_job_phase;
    std::string m_job_provider_name;
    std::string m_job_provider_task_id;
    std::string m_job_provider_conversion_task_id;
    std::string m_displayed_model_job_id;
    std::string m_artifact_format;
    std::string m_artifact_color_encoding;
    std::string m_color_intent_schema;
    std::string m_color_intent_sha256;
    uint64_t m_sequence { 0 };
    uint64_t m_style_recommendation_sequence { 0 };
    bool m_busy { false };
    bool m_awaiting_confirmation { false };
    bool m_awaiting_palette_confirmation { false };
    bool m_palette_recommendation_confirmed { false };
    bool m_ready { false };
    bool m_artifact_download_started { false };
    bool m_model_preview_ready { false };
    bool m_library_model_loaded { false };
    bool m_service_available { false };
    bool m_restore_checked { false };
    bool m_restoring_input { false };
    bool m_shutdown { false };
    bool m_updating_preview { false };
    bool m_style_preview_ready { false };
    bool m_style_recommendation_loading { false };
    bool m_style_recommendation_available { false };
    bool m_style_user_selected { false };
    bool m_job_preview_expected { false };
    bool m_palette_is_custom { false };
    bool m_advanced_options_expanded { false };
    bool m_job_use_printable_colors { false };
    bool m_raw_preview_available { false };
    bool m_model_reference_available { false };
    bool m_strict_preview_available { false };
    bool m_model_views_available { false };
    bool m_heatmap_available { false };
    bool m_palette_quality_ok { true };
    bool m_material_fragmentation_ok { true };
    bool m_model_input_eligible { true };
    std::string m_model_input_primary_blocker;
    bool m_preview_metrics_available { false };
    bool m_quality_check_busy { false };
    bool m_visual_check_busy { false };
    bool m_journey_model_submitted { false };
    int m_poll_connection_failures { 0 };
    int m_meaningful_palette_count { 0 };
    int m_meaningful_subject_color_count { 0 };
    double m_preview_zoom_factor { 1.0 };
    double m_preview_changed_pixel_ratio { 0.0 };
    int m_preview_minimum_feature_px { 0 };
    AIModelGenerationClient::ModelQuality m_model_quality;
    AIModelGenerationClient::VisualQuality m_visual_quality;
    AIModelGenerationClient::ModelRefinementAdvice m_model_refinement;
    std::function<void()> m_service_retry_handler;
};

} // namespace Slic3r::GUI
