#ifndef slic3r_GUI_AnimaController_hpp_
#define slic3r_GUI_AnimaController_hpp_

#include "../wxExtensions.hpp"
#include "Label.hpp"


class AnimaIcon : public wxPanel
{
public:
    AnimaIcon(wxWindow *parent, wxWindowID id, std::vector<std::string> img_list, std::string img_enable, int ivt = 1000);
    ~AnimaIcon();

    void Play();
    void Stop();
    void Enable();
    bool IsPlaying() const { return IsRunning(); };
    bool IsRunning() const;

private:
    wxBitmap              m_image_enable;
    wxStaticBitmap *      m_bitmap{nullptr};
    std::vector<wxBitmap> m_images;
    wxTimer *             m_timer;
    int                   m_current_frame = 0;
    int                   m_ivt;
    int                   m_size;
};

#endif // !slic3r_GUI_AnimaController_hpp_
