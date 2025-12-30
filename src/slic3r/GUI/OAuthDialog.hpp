#ifndef __OAuthDialog_HPP__
#define __OAuthDialog_HPP__

#include "GUI_Utils.hpp"
#include "Jobs/OAuthJob.hpp"
#include "Jobs/Worker.hpp"

namespace Slic3r {
namespace GUI {

class OAuthDialog : public DPIDialog
{
private:
    OAuthParams _params;
    std::shared_ptr<OAuthResult> _result;

    std::unique_ptr<Worker> m_worker;

    void on_cancel(wxEvent& event);

protected:
    bool Show(bool show) override;
    void on_dpi_changed(const wxRect& suggested_rect) override;

public:
    OAuthDialog(wxWindow* parent, OAuthParams params);

    OAuthResult get_result() { return *_result; }
};
	
}} // namespace Slic3r::GUI

#endif
