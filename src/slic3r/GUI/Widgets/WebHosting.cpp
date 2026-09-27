#include "WebHosting.hpp"

#include "slic3r/GUI/GUI.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>

#include <wx/uri.h>

namespace Slic3r { namespace GUI { namespace web_hosting {

namespace {

// Injected into the top-level page at document start (before the plugin's own
// scripts). Defines window.orca as the only host surface the page may use. It
// references window.wx lazily (at call time) so it never races the backend's
// deferred registration of the "wx" message handler. Guarded against
// double-injection so it is harmless if also prepended.
constexpr char ORCA_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function send(kind, data) {
    try {
      window.wx.postMessage(JSON.stringify({
        channel: 'orca', kind: kind, data: (data === undefined ? null : data)
      }));
    } catch (e) { /* bridge not ready yet */ }
  }
  window.orca = {
    postMessage: function (d) { send('message', d); },
    submit:      function (d) { send('submit', d); },
    close:       function ()  { send('close'); },
    onMessage:   function (cb) { if (typeof cb === 'function') handlers.push(cb); }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";

} // namespace

wxString bootstrap_url()
{
    return wxString("file://") + from_u8((boost::filesystem::path(resources_dir()) / BOOTSTRAP_PAGE).make_preferred().string());
}

wxString content_base_url()
{
    const std::string dir = (boost::filesystem::path(resources_dir()) / "web").make_preferred().string();
    return wxString("file://") + from_u8(dir) + "/";
}

bool is_content_url(const wxString& url)
{
    // The web view reports the URL it parsed, which escapes anything the resources path holds
    // (a space, a non-ASCII character), while content_base_url() is the raw path.
    return wxURI::Unescape(url.BeforeFirst('#')) == content_base_url();
}

const char* orca_bridge_script() { return ORCA_BRIDGE_JS; }

}}} // namespace Slic3r::GUI::web_hosting
