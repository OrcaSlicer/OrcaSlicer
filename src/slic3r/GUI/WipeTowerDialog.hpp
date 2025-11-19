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

	wxWebView* m_webview;
	int m_max_flush_volume;

	VolumeMatrix m_raw_matrixs;
	std::vector<double> m_flush_multipliers;
	bool m_submit_flag{ false };
};

#endif  // _WIPE_TOWER_DIALOG_H_
