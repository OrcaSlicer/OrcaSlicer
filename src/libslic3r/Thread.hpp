#ifndef GUI_THREAD_HPP
#define GUI_THREAD_HPP

#include <utility>
#include <string>
#include <thread>
#include <boost/thread.hpp>

namespace Slic3r {

// Set / get thread name.
// Returns false if the API is not supported.
//
// It is a good idea to name the main thread before spawning children threads, because dynamic linking is used on Windows 10
// to initialize Get/SetThreadDescription functions, which is not thread safe.
//
// pthread_setname_np supports maximum 15 character thread names! (16th character is the null terminator)
// 
// Methods taking the thread as an argument are not supported by OSX.
// Naming threads is only supported on newer Windows 10.

bool set_thread_name(std::thread &thread, const char *thread_name);
inline bool set_thread_name(std::thread &thread, const std::string &thread_name) { return set_thread_name(thread, thread_name.c_str()); }
bool set_thread_name(boost::thread &thread, const char *thread_name);
inline bool set_thread_name(boost::thread &thread, const std::string &thread_name) { return set_thread_name(thread, thread_name.c_str()); }
bool set_current_thread_name(const char *thread_name);
inline bool set_current_thread_name(const std::string &thread_name) { return set_current_thread_name(thread_name.c_str()); }

// To be called at the start of the application to save the current thread ID as the main (UI) thread ID.
void save_main_thread_id();
// Retrieve the cached main (UI) thread ID.
boost::thread::id get_main_thread_id();
// Checks whether the main (UI) thread is active.
bool is_main_thread_active();

// Returns nullopt if not supported.
// Not supported by OSX.
// Naming threads is only supported on newer Windows 10.
std::optional<std::string> get_current_thread_name();

// To be called somewhere before the TBB threads are spinned for the first time, to
// give them names recognizible in the debugger.
// Also it sets locale of the worker threads to "C" for the G-code generator to produce "." as a decimal separator.
void name_tbb_thread_pool_threads_set_locale();

template<class Fn>
inline boost::thread create_thread(boost::thread::attributes &attrs, Fn &&fn)
{
    // Stack size for our worker threads. Originally duplicated TBB's pool
    // default (4 MB), but the Emboss text-cut path calls into CGAL's
    // Polygon_mesh_processing::corefine, which falls back from filtered
    // interval arithmetic to exact rational arithmetic (mpq_class) on
    // near-degenerate input, and the constrained 2D triangulation walker
    // (Triangulation_2::march_locate_2D) can recurse deeply enough to
    // exceed 4 MB on real models -- producing a SIGBUS at the next thread's
    // stack guard page on macOS / Linux.
    //
    // 16 MB chosen as 4x defensive headroom over the observed crash
    // threshold (n=1 reproducer at exactly 4 MB on macOS arm64). All three
    // platforms defer-commit reserved stack pages: macOS / Linux mmap the
    // stack and only fault in pages on touch; Boost.Thread on Win32 passes
    // STACK_SIZE_PARAM_IS_A_RESERVATION to _beginthreadex, so the value is
    // a reserve, not the initial commit. Resident memory therefore stays
    // proportional to actual stack depth on every target.
    attrs.set_stack_size(16 * 1024 * 1024);
    return boost::thread{attrs, std::forward<Fn>(fn)};
}

template<class Fn> inline boost::thread create_thread(Fn &&fn)
{
    boost::thread::attributes attrs;
    return create_thread(attrs, std::forward<Fn>(fn));    
}

}

#endif // GUI_THREAD_HPP
