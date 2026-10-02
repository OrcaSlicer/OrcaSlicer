#ifdef _WIN32
	#include <windows.h>
	#include <boost/nowide/convert.hpp>
#else
	// any posix system
	#include <pthread.h>
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/task_scheduler_observer.h>

#include "Thread.hpp"
#include "Utils.hpp"

namespace Slic3r {

#ifdef _WIN32
// The new API is better than the old SEH style thread naming since the names also show up in crash dumpsand ETW traces.
// Because the new API is only available on newer Windows 10, look it up dynamically.

typedef HRESULT(__stdcall* SetThreadDescriptionType)(HANDLE, PCWSTR);
typedef HRESULT(__stdcall* GetThreadDescriptionType)(HANDLE, PWSTR*);

static bool 					s_SetGetThreadDescriptionInitialized = false;
static HMODULE					s_hKernel32 = nullptr;
static SetThreadDescriptionType s_fnSetThreadDescription = nullptr;
static GetThreadDescriptionType	s_fnGetThreadDescription = nullptr;

// Convert the FARPROC from GetProcAddress to Fn through a generic function pointer.
template<typename Fn> static Fn load_proc(HMODULE module, const char* name) {
	return reinterpret_cast<Fn>(reinterpret_cast<void(*)()>(::GetProcAddress(module, name)));
}

static bool WindowsGetSetThreadNameAPIInitialize()
{
	if (! s_SetGetThreadDescriptionInitialized) {
		// Not thread safe! It is therefore a good idea to name the main thread before spawning worker threads
		// to initialize 
		s_hKernel32 = LoadLibraryW(L"Kernel32.dll");
		if (s_hKernel32) {
			s_fnSetThreadDescription = load_proc<SetThreadDescriptionType>(s_hKernel32, "SetThreadDescription");
			s_fnGetThreadDescription = load_proc<GetThreadDescriptionType>(s_hKernel32, "GetThreadDescription");
		}
		s_SetGetThreadDescriptionInitialized = true;
	}
	return s_fnSetThreadDescription && s_fnGetThreadDescription;
}

#ifndef NDEBUG
	// Use the old way by throwing an exception, so at least in Debug mode the thread names are shown by the debugger.
	static constexpr DWORD MSVC_SEH_EXCEPTION_NAME_THREAD = 0x406D1388;

#pragma pack(push,8)
	typedef struct tagTHREADNAME_INFO
	{
		DWORD  dwType; 		// Must be 0x1000.
		LPCSTR szName; 		// Pointer to name (in user addr space).
		DWORD  dwThreadID; 	// Thread ID (-1=caller thread).
		DWORD  dwFlags; 	// Reserved for future use, must be zero.
	} THREADNAME_INFO;
#pragma pack(pop)

	static void WindowsSetThreadNameSEH(HANDLE hThread, const char* thread_name)
	{
		THREADNAME_INFO info;
		info.dwType 	= 0x1000;
		info.szName 	= thread_name;
		info.dwThreadID = ::GetThreadId(hThread);
		info.dwFlags 	= 0;
		__try {
			RaiseException(MSVC_SEH_EXCEPTION_NAME_THREAD, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
#endif // NDEBUG

static bool WindowsSetThreadName(HANDLE hThread, const char *thread_name)
{
	if (! WindowsGetSetThreadNameAPIInitialize()) {
#ifdef NDEBUG
		return false;
#else // NDEBUG
		// Running on Windows 7 or old Windows 7 in debug mode,
		// inform the debugger about the thread name by throwing an SEH.
		WindowsSetThreadNameSEH(hThread, thread_name);
		return true;
#endif // NDEBUG
	}

	size_t len = strlen(thread_name);
	if (len < 1024) {
		// Allocate the temp string on stack.
		wchar_t buf[1024];
		s_fnSetThreadDescription(hThread, boost::nowide::widen(buf, 1024, thread_name));
	} else {
		// Allocate dynamically.
		s_fnSetThreadDescription(hThread, boost::nowide::widen(thread_name).c_str());
	}
	return true;
}

bool set_thread_name(std::thread &thread, const char *thread_name)
{
   	return WindowsSetThreadName(static_cast<HANDLE>(thread.native_handle()), thread_name);
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
   	return WindowsSetThreadName(static_cast<HANDLE>(thread.native_handle()), thread_name);
}

bool set_current_thread_name(const char *thread_name)
{
    return WindowsSetThreadName(::GetCurrentThread(), thread_name);
}

std::optional<std::string> get_current_thread_name()
{
	if (! WindowsGetSetThreadNameAPIInitialize())
		return std::nullopt;

	wchar_t *ptr = nullptr;
	s_fnGetThreadDescription(::GetCurrentThread(), &ptr);
	return (ptr == nullptr) ? std::string() : boost::nowide::narrow(ptr);
}

#else // _WIN32

#ifdef __APPLE__

// Appe screwed the Posix norm.
bool set_thread_name(std::thread &thread, const char *thread_name)
{
// not supported
//   	pthread_setname_np(thread.native_handle(), thread_name);
	return false;
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
// not supported	
//   	pthread_setname_np(thread.native_handle(), thread_name);
	return false;
}

bool set_current_thread_name(const char *thread_name)
{
	pthread_setname_np(thread_name);
	return true;
}

std::optional<std::string> get_current_thread_name()
{
// not supported	
//	char buf[16];
//	return std::string(thread_getname_np(buf, 16) == 0 ? buf : "");
	return std::nullopt;
}

#else

// posix
bool set_thread_name(std::thread &thread, const char *thread_name)
{
   	pthread_setname_np(thread.native_handle(), thread_name);
	return true;
}

bool set_thread_name(boost::thread &thread, const char *thread_name)
{
   	pthread_setname_np(thread.native_handle(), thread_name);
	return true;
}

bool set_current_thread_name(const char *thread_name)
{
	pthread_setname_np(pthread_self(), thread_name);
	return true;
}

std::optional<std::string> get_current_thread_name()
{
	char buf[16];
	return std::string(pthread_getname_np(pthread_self(), buf, 16) == 0 ? buf : "");
}

#endif

#endif // _WIN32

// To be called at the start of the application to save the current thread ID as the main (UI) thread ID.
static boost::thread::id g_main_thread_id;

void save_main_thread_id()
{
	g_main_thread_id = boost::this_thread::get_id();
}

// Retrieve the cached main (UI) thread ID.
boost::thread::id get_main_thread_id()
{
	return g_main_thread_id;
}

// Checks whether the main (UI) thread is active.
bool is_main_thread_active()
{
	return get_main_thread_id() == boost::this_thread::get_id();
}

// Name the current TBB worker thread and set its locale to "C", so that the G-code generator
// produces "." as a decimal separator. Called once per worker thread, before it runs its first task.
static void setup_tbb_worker_thread()
{
	static std::atomic<size_t> s_worker_idx{ 0 };
	std::ostringstream name;
	name << "slic3r_tbb_" << (1 + s_worker_idx.fetch_add(1, std::memory_order_relaxed));
	set_current_thread_name(name.str().c_str());
#ifdef _WIN32
	_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
	std::setlocale(LC_ALL, "C");
#else
	// We are leaking some memory here, because the newlocale() produced memory will never be released.
	// This is not a problem though, as there will be a maximum one worker thread created per physical thread.
	uselocale(newlocale(
#ifdef __APPLE__
		LC_ALL_MASK
#else // some Unix / Linux / BSD
		LC_ALL
#endif
		, "C", nullptr));
#endif
}

// Sets up the TBB worker threads of the arena of the thread, which activated the observation.
// A worker sets itself up on entry to the arena, before it executes its first task, thus unlike a barrier
// inside a parallel_for, this does not depend on TBB running any number of tasks simultaneously.
class TBBWorkerThreadSetupObserver : public tbb::task_scheduler_observer
{
public:
	TBBWorkerThreadSetupObserver() { this->observe(true); }

	void on_scheduler_entry(bool is_worker) override
	{
		// Leave the external threads (the calling / UI thread) alone, their name and locale must not be modified here.
		if (! is_worker)
			return;
		// A worker thread enters an arena many times, while its name and locale have to be set just once.
		static thread_local bool initialized = false;
		if (initialized)
			return;
		initialized = true;
		setup_tbb_worker_thread();
	}
};

// Name the threads of the Intel TBB thread pool by an index and set their locale to "C"
// for the G-code generator to produce "." as a decimal separator.
// Formerly all the worker threads were caught inside a single parallel_for, which was held on a condition
// variable barrier until max_concurrency() of its chunks were running. TBB guarantees no such simultaneity,
// thus the barrier was able to block the slicing threads indefinitely. The TBB scheduler observer below
// sets each worker up on its own, thus no two chunks have to run at the same time.
void name_tbb_thread_pool_threads_set_locale()
{
#ifdef SLIC3R_PROFILE
	// Shiny profiler is not thread safe, thus disable parallelization.
	disable_multi_threading();
#endif

	// An observer is local to the arena of the thread which activates it, thus one observer is registered
	// per calling thread. Being function local and thread local, it is also initialized exactly once per
	// thread without a race. It is intentionally never destroyed, as it has to stay alive as long as the
	// TBB scheduler may notify it, which includes the shutdown of the process.
	static thread_local tbb::task_scheduler_observer *observer = new TBBWorkerThreadSetupObserver();
	(void)observer;
}

}
