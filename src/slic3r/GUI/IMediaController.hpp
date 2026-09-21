#pragma once

#include <wx/mediactrl.h>
#include <wx/uri.h>

namespace Slic3r { namespace GUI {

class IMediaController
{
public:
    virtual ~IMediaController() = default;

    virtual void Load(wxURI url) = 0;

    virtual void Play() = 0;

    virtual void Stop() = 0;

    virtual wxMediaState GetState() { return wxMediaState{}; }

    virtual int GetLastError() const { return {}; };

    virtual wxSize GetVideoSize() const { return {}; };
};

}} // namespace Slic3r::GUI
