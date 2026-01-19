#ifndef slic3r_GUI_Utils_FixModelByWin10_hpp_
#define slic3r_GUI_Utils_FixModelByWin10_hpp_

#include <string>
#include "../GUI/Widgets/ProgressDialog.hpp"

class ProgressDialog;

namespace Slic3r {

class Model;
class ModelObject;
class Print;

#ifdef HAS_WIN10SDK

extern bool is_windows10();
// returt false, if fixing was canceled
extern bool fix_model_by_win10_sdk_gui(ModelObject &model_object, int volume_idx,GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result);

#else /* HAS_WIN10SDK */

inline bool is_windows10() { return false; }
// returt false, if fixing was canceled
inline bool fix_model_by_win10_sdk_gui(ModelObject &, int, GUI::ProgressDialog &, const wxString &, std::string &) { return false; }

#endif /* HAS_WIN10SDK */

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByWin10_hpp_ */
