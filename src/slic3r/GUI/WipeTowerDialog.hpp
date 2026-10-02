#ifndef _WIPE_TOWER_DIALOG_H_
#define _WIPE_TOWER_DIALOG_H_

#include <wx/dialog.h>
#include <wx/webview.h>
#include "libslic3r/PrintConfig.hpp"
#include "Widgets/SpinInput.hpp"

#include "RammingChart.hpp"


class RammingPanel : public wxPanel {
public:
    RammingPanel(wxWindow* parent);
    RammingPanel(wxWindow* parent,const std::string& data);
    std::string get_parameters();

private:
    Chart* m_chart = nullptr;
    SpinInput* m_widget_volume = nullptr;
    SpinInput* m_widget_ramming_line_width_multiplicator = nullptr;
    SpinInput* m_widget_ramming_step_multiplicator = nullptr;
    SpinInput* m_widget_time = nullptr;
    int m_ramming_step_multiplicator;
    int m_ramming_line_width_multiplicator;
      
    void line_parameters_changed();
};


class RammingDialog : public wxDialog {
public:
    RammingDialog(wxWindow* parent,const std::string& parameters);    
    std::string get_parameters() { return m_output_data; }
private:
    RammingPanel* m_panel_ramming = nullptr;
    std::string m_output_data;
};



bool is_flush_config_modified();
void open_flushing_dialog(wxEvtHandler *parent, const wxEvent &event);

class WipingDialog : public wxDialog
{
public:
	using VolumeMatrix = std::vector<std::vector<double>>;

	WipingDialog(wxWindow* parent, const int max_flush_volume = Slic3r::g_max_flush_volume);
	static VolumeMatrix CalcFlushingVolumes(int extruder_id);
	std::vector<double> GetFlattenMatrix()const;
	std::vector<double> GetMultipliers()const;
	bool GetSubmitFlag() const { return m_submit_flag; }

private:
	static int CalcFlushingVolume(const wxColour& from_, const wxColour& to_, int min_flush_volume, int nozzle_flush_dataset);
	wxString BuildTableObjStr();
	wxString BuildTextObjStr(bool multi_language = true);
	void StoreFlushData(int extruder_num, const std::vector<std::vector<double>>& flush_volume_vecs, const std::vector<double>& flush_multipliers);
	// Maps the physical-only matrix shown in the table back onto the full config-indexed matrix.
	std::vector<double> ExpandToFullMatrix(const std::vector<double>& sub_matrix, int nozzle_idx) const;

	wxWebView* m_webview;
	int m_max_flush_volume;

	VolumeMatrix m_raw_matrixs;
	std::vector<double> m_flush_multipliers;
	// Config indices of the physical (non-mixed) filaments, in table order.
	std::vector<size_t> m_physical_indices;
	bool m_submit_flag{ false };
};

class CheckBox;
class Label;

// Which filaments share an independent prime tower. Rows/columns are the project filaments; a
// checked pair purges into one tower. The checks are prefilled from the material compatibility
// table, and only pairs that differ from that automatic verdict are stored as overrides in the
// project (prime_tower_share_matrix), so the automatic rule keeps applying to untouched pairs.
class PrimeTowerShareDialog : public wxDialog
{
public:
    PrimeTowerShareDialog(wxWindow *parent);
    bool GetSubmitFlag() const { return m_submit_flag; }
    // Resulting n*n override matrix (-1 auto / 0 separate / 1 share).
    const std::vector<int> &GetMatrix() const { return m_result; }

private:
    bool auto_share(size_t a, size_t b) const;
    bool pair_checked(size_t a, size_t b) const;
    void reset_to_auto();
    void update_summary();

    size_t                                   m_count = 0;
    std::vector<std::string>                 m_types;
    std::vector<std::string>                 m_names;
    std::vector<wxColour>                    m_colours;
    bool                                     m_auto_by_material = true;
    // Upper triangle only: m_checks[a][b] for a < b. Qualified: Slic3r::GUI::CheckBox (Field.hpp)
    // is a different class and becomes visible in translation units that open that namespace.
    std::vector<std::vector<::CheckBox *>>   m_checks;
    Label                                   *m_summary = nullptr;
    Label                                   *m_warning = nullptr;
    std::vector<int>                         m_result;
    bool                                     m_submit_flag = false;
};

// Opens the sharing table and, when confirmed, stores the overrides in the project config and
// posts `event` to `parent` (the plater reschedules the background process on it).
void open_prime_tower_share_dialog(wxEvtHandler *parent, const wxEvent &event);

#endif  // _WIPE_TOWER_DIALOG_H_
