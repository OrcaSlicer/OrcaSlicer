#ifndef slic3r_GUI_Utils_FixModelByCgal_hpp_
#define slic3r_GUI_Utils_FixModelByCgal_hpp_

#include <functional>
#include <string>
#include <wx/string.h>

namespace Slic3r::GUI { class ProgressDialog; }

namespace Slic3r {

class Model;
class ModelObject;
class Print;

// Repairs the meshes of `model_object`'s parts, or of part `volume_idx` alone, on a worker thread while the calling
// thread waits. A painted modifier, which follows the mesh of its host, is left as it is. `progress` is called from the
// calling thread before the repair starts, then on every update of the repair's progress and at most 250 ms apart, with
// the percent done and the name of the part being repaired when there are several; returning false cancels. Parts
// repaired before a cancel or an error keep their new mesh. Returns false if the repair was cancelled. fix_result is
// empty on success, else why parts could not be repaired; modified tells whether any mesh changed.
bool fix_model_with_cgal(ModelObject &model_object, int volume_idx,
                         const std::function<bool(int percent, const std::string &what)> &progress, std::string &fix_result,
                         bool keep_painting, bool *modified = nullptr);

// fix_model_with_cgal() shown in `progress_dlg`, whose message is `msg_header` followed by the part being repaired.
extern bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result, bool keep_painting,
                                    bool *modified = nullptr);

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByCgal_hpp_ */
