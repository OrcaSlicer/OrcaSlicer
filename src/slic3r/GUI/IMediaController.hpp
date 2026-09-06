#pragma once

#include <wx/mediactrl.h>
#include <wx/uri.h>

#include <slic3r/Utils/IPrinterAgent.hpp>

namespace Slic3r { namespace GUI {

class IMediaController
{
public:
    virtual void Load(wxURI url) = 0;

    // The default keeps existing media controllers unaware of camera-specific modes.
    virtual void Load(wxURI url, CameraStreamMode mode)
    {
        (void) mode;
        Load(url);
    }

    virtual void Play() = 0;

    virtual void Stop() = 0;

    virtual wxMediaState GetState() { return wxMediaState{}; }

    virtual int GetLastError() const { return {}; };

    virtual wxSize GetVideoSize() const { return {}; };

private:
};

}} // namespace Slic3r::GUI
