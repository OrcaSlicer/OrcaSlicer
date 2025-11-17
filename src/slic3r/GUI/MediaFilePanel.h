//
//  MediaFilePanel.h
//  libslic3r_gui
//
//  Created by cmguo on 2021/12/7.
//

#ifndef MediaFilePanel_h
#define MediaFilePanel_h

#include <set>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include <wx/frame.h>

class Button;
class SwitchButton;
class Label;
class StaticBox;
class PrinterFileSystem;

namespace Slic3r {

class MachineObject;

namespace GUI {

class ImageGrid;

class MediaFilePanel : public wxPanel
{
public:
    MediaFilePanel(wxWindow * parent);

    ~MediaFilePanel();

    void UpdateByObj(MachineObject * obj);

    void SwitchStorage(bool external);

public:
    void Rescale();

    void SetSelecting(bool selecting);

private:
    void modeChanged(wxCommandEvent & e);

    void fetchUrl(boost::weak_ptr<PrinterFileSystem> fs);

    void doAction(size_t index, int action);

private:
    ScalableBitmap m_bmp_loading;
    ScalableBitmap m_bmp_failed;
    ScalableBitmap m_bmp_empty;

    ::StaticBox *m_time_panel = nullptr;
    ::Button    *m_button_year = nullptr;
    ::Button    *m_button_month = nullptr;
    ::Button    *m_button_all = nullptr;
    ::Label     *m_switch_label = nullptr;

    ::StaticBox *   m_type_panel    = nullptr;
    ::Button *      m_button_video   = nullptr;
    ::Button *      m_button_timelapse = nullptr;
    ::Button *      m_button_model = nullptr;

    ::StaticBox *m_manage_panel        = nullptr;
    ::Button *   m_button_delete     = nullptr;
    ::Button *m_button_download = nullptr;
    ::Button *m_button_refresh = nullptr;
    ::Button *m_button_management = nullptr;

    ImageGrid * m_image_grid   = nullptr;

    bool m_external = true;

    std::string m_machine;
    std::string m_lan_ip;
    std::string m_lan_user;
    std::string m_lan_passwd;
    std::string m_dev_ver;
    bool        m_lan_mode      = false;
    bool        m_sdcard_exist  = false;
    int         m_local_proto = false;
    int         m_remote_proto = false;
    bool        m_model_download_support = false;
    bool        m_device_busy  = false;
    bool        m_waiting_enable = false;
    bool        m_waiting_support = false;

    int m_last_mode = 0;
    int m_last_type = 0;
    std::set<int> m_last_errors;
};


class MediaFileFrame : public DPIFrame
{
public:
    MediaFileFrame(wxWindow * parent);

    MediaFilePanel * filePanel() { return m_panel; }

    virtual void on_dpi_changed(const wxRect& suggested_rect);

private:
    MediaFilePanel* m_panel;
};

}}
#endif /* MediaFilePanel_h */
