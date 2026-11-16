#ifndef slic3r_GUI_Utils_FixModelByCgal_hpp_
#define slic3r_GUI_Utils_FixModelByCgal_hpp_

#include <string>
#include <wx/string.h>

namespace Slic3r::GUI { class ProgressDialog; }

namespace Slic3r {

class Model;
class ModelObject;
class Print;

// Return false if fixing was canceled. fix_result is empty on success; modified tells whether any mesh changed.
extern bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result, bool keep_painting,
                                    bool *modified = nullptr);

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByCgal_hpp_ */
