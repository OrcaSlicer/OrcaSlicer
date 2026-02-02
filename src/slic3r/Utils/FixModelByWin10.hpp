#ifndef slic3r_GUI_Utils_FixModelByWin10_hpp_
#define slic3r_GUI_Utils_FixModelByWin10_hpp_

#include <string>
#include "../GUI/Widgets/ProgressDialog.hpp"

class ProgressDialog;

namespace Slic3r {

class Model;
class ModelObject;
class Print;

// Return false if fixing was canceled. fix_result is empty on success.
extern bool fix_model_by_win10_sdk_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result);

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByWin10_hpp_ */
