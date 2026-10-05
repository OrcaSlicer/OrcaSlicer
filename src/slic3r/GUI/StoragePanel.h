#pragma once

#include <wx/panel.h>

namespace Slic3r {
class MachineObject;

namespace GUI {

class MediaFilePanel;
class OrcaFilesPanel;

class StoragePanel : public wxPanel
{
public:
    explicit StoragePanel(wxWindow* parent);

    void UpdateByObj(MachineObject* obj);
    void SwitchStorage(bool external);
    void Rescale();
    void on_sys_color_changed();

private:
    enum class Child { None, Bambu, Orca };

    MediaFilePanel* ensureBambuPanel();
    OrcaFilesPanel* ensureOrcaPanel();
    void showChild(Child child);

    MediaFilePanel* m_bambu_panel = nullptr;
    OrcaFilesPanel* m_orca_panel = nullptr;
    Child m_active_child = Child::None;
};

}}
