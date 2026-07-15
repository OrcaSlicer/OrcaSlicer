#ifndef slic3r_wxinit_hpp_
#define slic3r_wxinit_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/html/htmlwin.h>

// Perl redefines a _ macro, so we undef this one
#undef _

// We do want to use translation however, so define it as __ so we can do a find/replace
// later when we no longer need to undef _
#define __(s)                     wxGetTranslation((s))

#endif
