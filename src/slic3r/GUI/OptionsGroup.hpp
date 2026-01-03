#ifndef slic3r_OptionsGroup_hpp_
#define slic3r_OptionsGroup_hpp_

#include <wx/stattext.h>
#include <wx/settings.h>

#include <map>
#include <functional>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "Field.hpp"
#include "I18N.hpp"

// Translate the ifdef
#ifdef __WXOSX__
    #define wxOSX true
#else
    #define wxOSX false
#endif

#define BORDER(a, b) ((wxOSX ? a : b))

namespace Slic3r { namespace GUI {

// Thrown if the building of a parameter page is canceled.
class UIBuildCanceled : public std::exception {};
class OG_CustomCtrl;

/// Widget type describes a function object that returns a wxWindow (our widget) and accepts a wxWidget (parent window).
using widget_t = std::function<wxSizer*(wxWindow*)>;//!std::function<wxWindow*(wxWindow*)>;

/// Wraps a ConfigOptionDef and adds function object for creating a side_widget.
struct Option {
	ConfigOptionDef			opt { ConfigOptionDef() };
	t_config_option_key		opt_id;//! {""};
    widget_t				side_widget {nullptr};
    bool					readonly {false};

	bool operator==(const Option& rhs) const {
		return  (rhs.opt_id == this->opt_id);
	}

	Option(const ConfigOptionDef& _opt, t_config_option_key id) :
		opt(_opt), opt_id(id) {}
};
using t_option = std::unique_ptr<Option>;	//!

/// Represents option lines
class Line : public UndoValueUIManager
{
	bool		m_is_separator{ false };
public:
    wxString	label;
    wxString	label_tooltip;
	std::string	label_path;
    bool        undo_to_sys{false}; // BBS: object config
    bool        toggle_visible{true}; // BBS: hide some line

    size_t		full_width {0};
    widget_t	widget {nullptr};
    std::function<wxWindow*(wxWindow*)>	near_label_widget{ nullptr };
	wxWindow*	near_label_widget_win {nullptr};
    wxSizer*	widget_sizer {nullptr};
    wxSizer*	extra_widget_sizer {nullptr};
    //BBS: export the extra colume widget
    wxWindow*	extra_widget_win {nullptr};
    //BBS: add api to get the first option's key
    std::string& get_first_option_key() {
        return m_options[0].opt_id;
    }

    void append_option(const Option& option) {
        m_options.push_back(option);
    }
	void append_widget(const widget_t widget) {
		m_extra_widgets.push_back(widget);
    }
	Line(wxString label, wxString tooltip) :
		label(_(label)), label_tooltip(_(tooltip)) {}
	Line() : m_is_separator(true) {}

	bool is_separator() const { return m_is_separator; }
	bool has_only_option(const std::string& opt_key) const { return m_options.size() == 1 && m_options[0].opt_id == opt_key; }

    const std::vector<widget_t>&	get_extra_widgets() const {return m_extra_widgets;}
    const std::vector<Option>&		get_options() const { return m_options; }

private:
	std::vector<Option>		m_options;//! {std::vector<Option>()};
    std::vector<widget_t>	m_extra_widgets;//! {std::vector<widget_t>()};
};

using column_t = std::function<wxWindow*(wxWindow* parent, const Line&)>;

using t_optionfield_map = std::map<t_config_option_key, t_field>;
using t_opt_map = std::map< std::string, std::pair<std::string, int> >;

class OptionsGroup {
public:
    const bool staticbox{true};
    bool split_multi_line{false};
    bool option_label_at_right{false};
    // BBS: new layout
    wxWindow *     stb;
    const wxString  icon;
    const wxString  title;
    size_t			label_width = 20 ;// {200};
    wxSizer*		sizer {nullptr};
	OG_CustomCtrl*  custom_ctrl{ nullptr };
	int				ctrl_horiz_alignment{ wxALIGN_LEFT};
    column_t		extra_column {nullptr};
    t_change		m_on_change { nullptr };
	// To be called when the field loses focus, to assign a new initial value to the field.
	// Used by the relative position / rotation / scale manipulation fields of the Object Manipulation UI.
    t_kill_focus    m_fill_empty_value { nullptr };
	std::function<DynamicPrintConfig()>	m_get_initial_config{ nullptr };
	std::function<DynamicPrintConfig()>	m_get_sys_config{ nullptr };
	std::function<bool()>	have_sys_config{ nullptr };

    std::function<void(wxWindow* win)> rescale_extra_column_item { nullptr };
    std::function<void(wxWindow* win)> rescale_near_label_widget { nullptr };

    std::function<void(const t_config_option_key& opt_key)> edit_custom_gcode { nullptr };

    wxFont			sidetext_font {wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT) };
    wxFont			label_font {wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT) };
	int				sidetext_width{ -1 };
	int				sublabel_width{ -1 };

    /// Returns a copy of the pointer of the parent wxWindow.
    /// Accessor function is because users are not allowed to change the parent
    /// but defining it as const means a lot of const_casts to deal with wx functions.
    inline wxWindow* parent() const { return m_parent; }

    wxWindow*   ctrl_parent() const;

	void		append_line(const Line& line);
	// create controls for the option group
	void		activate_line(Line& line);
	//BBS: get line for opt_key
	Line* get_line(const std::string& opt_key);

	// create all controls for the option group from the m_lines
	bool		activate(std::function<void()> throw_if_canceled = [](){}, int horiz_alignment = wxALIGN_LEFT);
	// delete all controls from the option group
	void		clear(bool destroy_custom_ctrl = false);

    Line		create_single_option_line(const Option& option, const std::string& path = std::string()) const;
    void		append_single_option_line(const Option& option, const std::string& path = std::string()) { append_line(create_single_option_line(option, path)); }
	void		append_separator();

    // return a non-owning pointer reference
    inline Field*	get_field(const t_config_option_key& id) const{
							if (m_fields.find(id) == m_fields.end()) return nullptr;
							return m_fields.at(id).get();
    }

	bool			set_value(const t_config_option_key& id, const boost::any& value, bool change_event = false) {
							if (m_fields.find(id) == m_fields.end()) return false;
							m_fields.at(id)->set_value(value, change_event);
							return true;
    }
	boost::any		get_value(const t_config_option_key& id) {
							boost::any out;
    						if (m_fields.find(id) == m_fields.end()) ;
							else
								out = m_fields.at(id)->get_value();
							return out;
    }

	void			show_field(const t_config_option_key& opt_key, bool show = true);
	void			hide_field(const t_config_option_key& opt_key) {  show_field(opt_key, false);  }

	void enable_field(const t_config_option_key& opt_key, bool enable = true);
    void disable_field(const t_config_option_key& opt_key) { enable_field(opt_key, false); }

	void			set_name(const wxString& new_name);

	inline void		enable() { for (auto& field : m_fields) field.second->enable(); }
    inline void		disable() { for (auto& field : m_fields) field.second->disable(); }
	void			set_grid_vgap(int gap) { m_grid_sizer->SetVGap(gap); }

    void            clear_fields_except_of(const std::vector<std::string> left_fields);

    void            hide_labels() { label_width = 0; }

	OptionsGroup(wxWindow *_parent, const wxString &title, const wxString &icon, bool is_tab_opt = false,
                    column_t extra_clmn = nullptr);
	~OptionsGroup() { clear(true); }

    wxGridSizer*        get_grid_sizer() { return m_grid_sizer; }
	const std::vector<Line>& get_lines() { return m_lines; }
	bool				is_legend_line();
	// if we have to set the same control alignment for different option groups,
    // we have to set same max contrtol width to all of them
	void				set_max_win_width(int max_win_width);

	bool				is_activated() { return sizer != nullptr; }

	void remove_option_if(std::function<bool(std::string const &)> const & comp);
    void msw_rescale();
protected:
	std::map<t_config_option_key, Option>	m_options;
    wxWindow*				m_parent {nullptr};
    std::vector<ConfigOptionMode>           m_options_mode;
    std::vector<wxWindow*>                  m_extra_column_item_ptrs;

    std::vector<Line>                       m_lines;

    /// Field list, contains unique_ptrs of the derived type.
    /// using types that need to know what it is beyond the public interface
    /// need to cast based on the related ConfigOptionDef.
    t_optionfield_map		m_fields;
    bool					m_disabled {false};
    wxGridSizer*			m_grid_sizer {nullptr};
	// "true" if option is created in preset tabs
	bool					m_use_custom_ctrl{ false };

	// "true" if control should be created on custom_ctrl
	bool					m_use_custom_ctrl_as_parent { false };

	// This panel is needed for correct showing of the ToolTips for Button, StaticText and CheckBox
	// Tooltips on GTK doesn't work inside wxStaticBoxSizer unless you insert a panel
	// inside it before you insert the other controls.
#if 0//#ifdef__WXGTK__
	wxPanel*				m_panel {nullptr};
#endif /* __WXGTK__ */

    /// Generate a wxSizer or wxWindow from a configuration option
    /// Precondition: opt resolves to a known ConfigOption
    /// Postcondition: fields contains a wx gui object.
	const t_field&		build_field(const t_config_option_key& id, const ConfigOptionDef& opt);
	const t_field&		build_field(const t_config_option_key& id);
	const t_field&		build_field(const Option& opt);

    virtual void		on_kill_focus(const std::string& opt_key) {};
	virtual void		on_change_OG(const t_config_option_key& opt_id, const boost::any& value);
	virtual void		back_to_initial_value(const std::string& opt_key) {}
	virtual void		back_to_sys_value(const std::string& opt_key) {}

public:
	static wxString		get_url(const std::string& path_end);
	static bool			launch_browser(const std::string& path_end);
};

class ConfigOptionsGroup: public OptionsGroup {
public:
	ConfigOptionsGroup(	wxWindow* parent, const wxString& title, const wxString& icon, DynamicConfigWithDef* config = nullptr,
						bool is_tab_opt = false, column_t extra_clmn = nullptr) :
		OptionsGroup(parent, title, icon, is_tab_opt, extra_clmn), m_config(config) {}
	ConfigOptionsGroup(	wxWindow* parent, const wxString& title, DynamicConfigWithDef* config = nullptr,
						bool is_tab_opt = false, column_t extra_clmn = nullptr) :
		ConfigOptionsGroup(parent, title, wxEmptyString, config, is_tab_opt, extra_clmn) {}
	ConfigOptionsGroup(	wxWindow* parent, const wxString& title, ModelConfig* config,
						bool is_tab_opt = false, column_t extra_clmn = nullptr) :
		OptionsGroup(parent, title, wxEmptyString, is_tab_opt, extra_clmn), m_config(&config->get()), m_modelconfig(config) {}
	ConfigOptionsGroup(	wxWindow* parent) :
		OptionsGroup(parent, wxEmptyString, wxEmptyString, true, nullptr) {}

	const wxString& config_category() const throw() { return m_config_category; }
	int config_type() const throw() { return m_config_type; }
	const t_opt_map&   opt_map() const throw() { return m_opt_map; }

	void 		set_config_category_and_type(const wxString &category, int type) { m_config_category = category; m_config_type = type; }
    void        set_config(DynamicConfigWithDef* config) {
		m_config = config; m_modelconfig = nullptr; }
	Option		get_option(const std::string& opt_key, int opt_index = -1);
	Line		create_single_option_line(const std::string& title, const std::string& path = std::string(), int idx = -1) /*const*/{
		Option option = get_option(title, idx);
		return OptionsGroup::create_single_option_line(option, path);
	}
	Line		create_single_option_line(const Option& option, const std::string& path = std::string()) const {
		return OptionsGroup::create_single_option_line(option, path);
	}
	void		append_single_option_line(const Option& option, const std::string& path = std::string())	{
		OptionsGroup::append_single_option_line(option, path);
	}
	void		append_single_option_line(const std::string title, const std::string& path = std::string(), int idx = -1)
	{
		Option option = get_option(title, idx);
		append_single_option_line(option, path);
	}

	void		on_change_OG(const t_config_option_key& opt_id, const boost::any& value) override;
	void		back_to_initial_value(const std::string& opt_key) override;
	void		back_to_sys_value(const std::string& opt_key) override;
	void		back_to_config_value(const DynamicConfigWithDef& config, const std::string& opt_key);
    void		on_kill_focus(const std::string& opt_key) override;
	void		reload_config();
    // return value shows visibility : false => all options are hidden
    void        Hide();
    void        Show(const bool show);
    bool        is_visible(ConfigOptionMode mode);
    bool        update_visibility(ConfigOptionMode mode);
    // void        msw_rescale();
    void        sys_color_changed();
    void        refresh();
	boost::any	config_value(const std::string& opt_key, int opt_index, bool deserialize);
	// return option value from config
	boost::any	get_config_value(const DynamicConfigWithDef& config, const std::string& opt_key, int opt_index = -1);
	// BBS: restore all pages in preset
	boost::any	get_config_value2(const DynamicConfigWithDef& config, const std::string& opt_key, int opt_index = -1);
	Field*		get_fieldc(const t_config_option_key& opt_key, int opt_index);
	std::pair<OG_CustomCtrl*, bool*>	get_custom_ctrl_with_blinking_ptr(const t_config_option_key& opt_key, int opt_index/* = -1*/);

	// BBS. Change private to protected to make change_opt_value() method available to
	// its child class.
protected:
    // Reference to libslic3r config or ModelConfig::get(), non-owning pointer.
    // The reference is const, so that the spots which modify m_config are clearly
    // demarcated by const_cast and m_config_changed_callback is called afterwards.
    const DynamicConfigWithDef*	m_config {nullptr};
    // If the config is modelconfig, then ModelConfig::touch() has to be called after value change.
    ModelConfig*				m_modelconfig { nullptr };
	t_opt_map					m_opt_map;
    wxString                    m_config_category;
    int                         m_config_type;

    // Change an option on m_config, possibly call ModelConfig::touch().
	void 	change_opt_value(const t_config_option_key& opt_key, const boost::any& value, int opt_index = 0);
};

// BBS. Add ExtruderOptionsGroup to change all members in vector option.
// It is designed for single extruder multiple material machine.
class ExtruderOptionsGroup : public ConfigOptionsGroup {
public:
	ExtruderOptionsGroup(wxWindow* parent, const wxString& title, const wxString& icon, DynamicConfigWithDef* config = nullptr, // ORCA: add support for icons
		bool is_tab_opt = false, column_t extra_clmn = nullptr) :
		ConfigOptionsGroup(parent, title, icon, config, is_tab_opt, extra_clmn) {}

	void on_change_OG(const t_config_option_key& opt_id, const boost::any& value) override;
};

//  Static text shown among the options.
class ogStaticText :public wxStaticText{
public:
	ogStaticText() {}
	ogStaticText(wxWindow* parent, const wxString& text);
	~ogStaticText() {}

	void		SetText(const wxString& value, bool wrap = true);
	// Set special path end. It will be used to generation of the hyperlink on info page
	void		SetPathEnd(const std::string& link);
	void		FocusText(bool focus);
};

}}

#endif /* slic3r_OptionsGroup_hpp_ */
