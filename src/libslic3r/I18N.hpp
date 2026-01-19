#ifndef slic3r_I18N_hpp_
#define slic3r_I18N_hpp_

#include <string>

#ifdef SLIC3R_CURRENTLY_COMPILING_GUI_MODULE
    #ifndef SLIC3R_ALLOW_LIBSLIC3R_I18N_IN_SLIC3R
        #error You included libslic3r/I18N.hpp into a file belonging to slic3r module.
    #endif
#endif

namespace Slic3r {

namespace I18N {
	typedef std::string (*translate_fn_type)(const char*);
	extern translate_fn_type translate_fn;
	inline void set_translate_callback(translate_fn_type fn) { translate_fn = fn; }
	inline std::string translate(const std::string &s) { return (translate_fn == nullptr) ? s : (*translate_fn)(s.c_str()); }
	inline std::string translate(const char *ptr) { return (translate_fn == nullptr) ? std::string(ptr) : (*translate_fn)(ptr); }
} // namespace I18N

} // namespace Slic3r

// When this is included from slic3r, better do not define the translation functions.
// Macros from slic3r/GUI/I18N.hpp should be used there.
#ifndef SLIC3R_CURRENTLY_COMPILING_GUI_MODULE
	#ifdef L
	    #error L macro is defined where it shouldn't be. Didn't you include slic3r/GUI/I18N.hpp in libslic3r by mistake?
	#endif
	namespace {
		[[maybe_unused]] const char* L(const char* s)    { return s; }
		[[maybe_unused]] const char* L_CONTEXT(const char* s, const char* context) { return s; }
		[[maybe_unused]] std::string _u8L(const char* s) { return Slic3r::I18N::translate(s); }
	}
#endif

#endif /* slic3r_I18N_hpp_ */
