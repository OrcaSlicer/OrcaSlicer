#pragma once

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "Printer/RemoteFileGridModel.h"

#include <boost/smart_ptr/shared_ptr.hpp>
#include <wx/panel.h>

class Button;
class Label;
class StaticBox;

namespace Slic3r {
class MachineObject;

namespace GUI {

class ImageGrid;

class OrcaFilesPanel : public wxPanel
{
public:
    explicit OrcaFilesPanel(wxWindow* parent);
    ~OrcaFilesPanel() override;

    void UpdateByObj(MachineObject* obj);
    void Rescale();
    void on_sys_color_changed();

private:
    void applyThemeColors();
    void updateGroupButtons(FileGridGroup mode);
    void updateStatus(RemoteFileGridModel::Status status);

    ScalableBitmap m_bmp_loading;
    ScalableBitmap m_bmp_failed;
    ScalableBitmap m_bmp_empty;
    Label* m_root_label = nullptr;
    ::StaticBox* m_time_panel = nullptr;
    ::Button* m_button_year = nullptr;
    ::Button* m_button_month = nullptr;
    ::Button* m_button_all = nullptr;
    ::Button* m_button_refresh = nullptr;
    ImageGrid* m_image_grid = nullptr;
    boost::shared_ptr<RemoteFileGridModel> m_model;
    std::string m_device_id;
    FileGridGroup m_group_mode = FileGridGroup::All;
};

}}
