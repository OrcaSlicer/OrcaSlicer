#include "Utils.hpp"
#include "I18N.hpp"

#include <atomic>
#include <locale>
#include <ctime>
#include <cstdarg>
#include <stdio.h>
#include <filesystem>

#include "format.hpp"
#include "Platform.hpp"
#include "Time.hpp"
#include "libslic3r.h"

#ifdef __APPLE__
#include "MacUtils.hpp"
#endif

#ifdef WIN32
	#include <windows.h>
	#include <psapi.h>
	#include <direct.h>  // for mkdir
	#include <io.h>  // for _access
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/param.h>
    #include <sys/resource.h>
	#ifdef BSD
		#include <sys/sysctl.h>
	#endif
	#ifdef __APPLE__
		#include <mach/mach.h>
		#include <libproc.h>
	#endif
	#ifdef __linux__
		#include <sys/stat.h>
		#include <fcntl.h>
		#include <sys/sendfile.h>
		#include <dirent.h>
		#include <stdio.h>
	#endif
#endif

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/support/date_time.hpp>

#include <boost/locale.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>

// We are using quite an old TBB 2017 U7, which does not support global control API officially.
// Before we update our build servers, let's use the old API, which is deprecated in up to date TBB.
#include <tbb/tbb.h>
#if ! defined(TBB_VERSION_MAJOR)
    #include <tbb/version.h>
#endif
#if ! defined(TBB_VERSION_MAJOR)
    static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
    #define TBB_HAS_GLOBAL_CONTROL
#endif
#ifdef TBB_HAS_GLOBAL_CONTROL
    #include <tbb/global_control.h>
#else
    #include <tbb/task_scheduler_init.h>
#endif

#if defined(__linux__) || defined(__GNUC__ )
#include <strings.h>
#endif /* __linux__ */

#ifdef _MSC_VER
    #define strcasecmp _stricmp
#endif

namespace Slic3r {

static boost::log::trivial::severity_level logSeverity = boost::log::trivial::error;

static boost::log::trivial::severity_level level_to_boost(unsigned level)
{
    switch (level) {
    // Report fatal errors only.
    case 0: return boost::log::trivial::fatal;
    // Report fatal errors and errors.
    case 1: return boost::log::trivial::error;
    // Report fatal errors, errors and warnings.
    case 2: return boost::log::trivial::warning;
    // Report all errors, warnings and infos.
    case 3: return boost::log::trivial::info;
    // Report all errors, warnings, infos and debugging.
    case 4: return boost::log::trivial::debug;
    // Report everyting including fine level tracing information.
    default: return boost::log::trivial::trace;
    }
}

void set_logging_level(unsigned int level)
{
    logSeverity = level_to_boost(level);

    boost::log::core::get()->set_filter
    (
        boost::log::trivial::severity >= logSeverity
    );
}

unsigned int level_string_to_boost(std::string level)
{
    std::map<std::string, int> Control_Param;
    Control_Param["fatal"] = 0;
    Control_Param["error"] = 1;
    Control_Param["warning"] = 2;
    Control_Param["info"] = 3;
    Control_Param["debug"] = 4;
    Control_Param["trace"] = 5;

    return Control_Param[level];
}

std::string get_string_logging_level(unsigned level)
{
    switch (level) {
    case 0: return "fatal";
    case 1: return "error";
    case 2: return "warning";
    case 3: return "info";
    case 4: return "debug";
    case 5: return "trace";
    default: return "error";
    }
}

unsigned get_logging_level()
{
    switch (logSeverity) {
    case boost::log::trivial::fatal : return 0;
    case boost::log::trivial::error : return 1;
    case boost::log::trivial::warning : return 2;
    case boost::log::trivial::info : return 3;
    case boost::log::trivial::debug : return 4;
    case boost::log::trivial::trace : return 5;
    default: return 1;
    }
}

boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>> g_log_sink;

// Force set_logging_level(<=error) after loading of the DLL.
// This is currently only needed if libslic3r is loaded as a shared library into Perl interpreter
// to perform unit and integration tests.
static struct RunOnInit {
    RunOnInit() {
        set_logging_level(2);

    }
} g_RunOnInit;

void trace(unsigned int level, const char *message)
{
    boost::log::trivial::severity_level severity = level_to_boost(level);

    BOOST_LOG_STREAM_WITH_PARAMS(::boost::log::trivial::logger::get(),\
        (::boost::log::keywords::severity = severity)) << message;
}

void disable_multi_threading()
{
    // Disable parallelization so the Shiny profiler works
#ifdef TBB_HAS_GLOBAL_CONTROL
    tbb::global_control(tbb::global_control::max_allowed_parallelism, 1);
#else // TBB_HAS_GLOBAL_CONTROL
    static tbb::task_scheduler_init *tbb_init = new tbb::task_scheduler_init(1);
    UNUSED(tbb_init);
#endif // TBB_HAS_GLOBAL_CONTROL
}

static std::string g_var_dir;

void set_var_dir(const std::string &dir)
{
    g_var_dir = dir;
}

const std::string& var_dir()
{
    return g_var_dir;
}

std::string var(const std::string &file_name)
{
    boost::system::error_code ec;
    if (boost::filesystem::exists(file_name, ec)) {
       return file_name;
    }

    auto file = (boost::filesystem::path(g_var_dir) / file_name).make_preferred();
    return file.string();
}

static std::string g_resources_dir;

void set_resources_dir(const std::string &dir)
{
    g_resources_dir = dir;
}

const std::string& resources_dir()
{
    return g_resources_dir;
}

//BBS: add temporary dir
static std::string g_temporary_dir;
void set_temporary_dir(const std::string &dir)
{
    g_temporary_dir = dir;
}

const std::string& temporary_dir()
{
    return g_temporary_dir;
}

static std::string g_local_dir;

void set_local_dir(const std::string &dir)
{
    g_local_dir = dir;
}

const std::string& localization_dir()
{
	return g_local_dir;
}

static std::string g_sys_shapes_dir;

void set_sys_shapes_dir(const std::string &dir)
{
    g_sys_shapes_dir = dir;
}

const std::string& sys_shapes_dir()
{
	return g_sys_shapes_dir;
}

static std::string g_custom_gcodes_dir;

void set_custom_gcodes_dir(const std::string &dir)
{
    g_custom_gcodes_dir = dir;
}

const std::string& custom_gcodes_dir()
{
    return g_custom_gcodes_dir;
}

// Translate function callback, to call wxWidgets translate function to convert non-localized UTF8 string to a localized one.
Slic3r::I18N::translate_fn_type Slic3r::I18N::translate_fn = nullptr;
static std::string g_data_dir;

void set_data_dir(const std::string &dir)
{
    g_data_dir = dir;
    if (!g_data_dir.empty() && !boost::filesystem::exists(g_data_dir)) {
       boost::filesystem::create_directory(g_data_dir);
    }
}

const std::string& data_dir()
{
    return g_data_dir;
}

std::string custom_shapes_dir()
{
    return (boost::filesystem::path(g_data_dir) / "shapes").string();
}

static std::atomic<bool> debug_out_path_called(false);

std::string debug_out_path(const char *name, ...)
{
	//static constexpr const char *SLIC3R_DEBUG_OUT_PATH_PREFIX = "out/";
	auto svg_folder = boost::filesystem::path(g_data_dir) / "SVG/";
    if (! debug_out_path_called.exchange(true)) {
		if (!boost::filesystem::exists(svg_folder)) {
			boost::filesystem::create_directory(svg_folder);
		}
		std::string path = boost::filesystem::system_complete(svg_folder).string();
        printf("Debugging output files will be written to %s\n", path.c_str());
    }
	char buffer[2048];
	va_list args;
	va_start(args, name);
	std::vsprintf(buffer, name, args);
	va_end(args);

	std::string buf(buffer);
	if (size_t pos = buf.find_first_of('/'); pos != std::string::npos) {
		std::string sub_dir = buf.substr(0, pos);
		std::filesystem::create_directory(svg_folder.string() + sub_dir);
	}
	return svg_folder.string() + std::string(buffer);
}

namespace logging = boost::log;
namespace src = boost::log::sources;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;
namespace attrs = boost::log::attributes;
void set_log_path_and_level(const std::string& file, unsigned int level)
{
#ifdef __APPLE__
	//currently on old macos, the boost::log::add_file_log will crash
	//TODO: need to be fixed
	if (!is_macos_support_boost_add_file_log()) {
		return;
	}
#endif

	//BBS log file at C:\\Users\\[yourname]\\AppData\\Roaming\\OrcaSlicer\\log\\[log_filename].log
	auto log_folder = boost::filesystem::path(g_data_dir) / "log";
	if (!boost::filesystem::exists(log_folder)) {
		boost::filesystem::create_directory(log_folder);
	}
	auto full_path = (log_folder / file).make_preferred();

	g_log_sink = boost::log::add_file_log(
		keywords::file_name = full_path.string() + ".%N",
		keywords::rotation_size = 100 * 1024 * 1024,
		keywords::format =
		(
			expr::stream
			<< "[" << expr::attr< logging::trivial::severity_level >("Severity") << "]\t"
			<< expr::format_date_time< boost::posix_time::ptime >("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
			<<"[Thread " << expr::attr<attrs::current_thread_id::value_type>("ThreadID") << "]"
			<< ":" << expr::smessage
		),
		keywords::auto_flush = true
	);

	logging::add_common_attributes();

	set_logging_level(level);

	return;
}

void flush_logs()
{
	if (g_log_sink)
		g_log_sink->flush();

	return;
}

#ifdef _WIN32
// The following helpers are borrowed from the LLVM project https://github.com/llvm
namespace WindowsSupport
{
	template <typename HandleTraits>
	class ScopedHandle {
		typedef typename HandleTraits::handle_type handle_type;
		handle_type Handle;
		ScopedHandle(const ScopedHandle &other) = delete;
		void operator=(const ScopedHandle &other) = delete;
	public:
		ScopedHandle() : Handle(HandleTraits::GetInvalid()) {}
	  	explicit ScopedHandle(handle_type h) : Handle(h) {}
	  	~ScopedHandle() { if (HandleTraits::IsValid(Handle)) HandleTraits::Close(Handle); }
	  	handle_type take() {
	    	handle_type t = Handle;
	    	Handle = HandleTraits::GetInvalid();
	    	return t;
	  	}
	  	ScopedHandle &operator=(handle_type h) {
	    	if (HandleTraits::IsValid(Handle))
	      		HandleTraits::Close(Handle);
	    	Handle = h;
	    	return *this;
	  	}
	  	// True if Handle is valid.
	  	explicit operator bool() const { return HandleTraits::IsValid(Handle) ? true : false; }
	  	operator handle_type() const { return Handle; }
	};

	struct CommonHandleTraits {
	  	typedef HANDLE handle_type;
	  	static handle_type GetInvalid() { return INVALID_HANDLE_VALUE; }
	  	static void Close(handle_type h) { ::CloseHandle(h); }
	  	static bool IsValid(handle_type h) { return h != GetInvalid(); }
	};

	typedef ScopedHandle<CommonHandleTraits> ScopedFileHandle;

	std::error_code map_windows_error(unsigned windows_error_code)
	{
		#define MAP_ERR_TO_COND(x, y) case x: return std::make_error_code(std::errc::y)
		switch (windows_error_code) {
			MAP_ERR_TO_COND(ERROR_ACCESS_DENIED, permission_denied);
			MAP_ERR_TO_COND(ERROR_ALREADY_EXISTS, file_exists);
			MAP_ERR_TO_COND(ERROR_BAD_UNIT, no_such_device);
			MAP_ERR_TO_COND(ERROR_BUFFER_OVERFLOW, filename_too_long);
			MAP_ERR_TO_COND(ERROR_BUSY, device_or_resource_busy);
			MAP_ERR_TO_COND(ERROR_BUSY_DRIVE, device_or_resource_busy);
			MAP_ERR_TO_COND(ERROR_CANNOT_MAKE, permission_denied);
			MAP_ERR_TO_COND(ERROR_CANTOPEN, io_error);
			MAP_ERR_TO_COND(ERROR_CANTREAD, io_error);
			MAP_ERR_TO_COND(ERROR_CANTWRITE, io_error);
			MAP_ERR_TO_COND(ERROR_CURRENT_DIRECTORY, permission_denied);
			MAP_ERR_TO_COND(ERROR_DEV_NOT_EXIST, no_such_device);
			MAP_ERR_TO_COND(ERROR_DEVICE_IN_USE, device_or_resource_busy);
			MAP_ERR_TO_COND(ERROR_DIR_NOT_EMPTY, directory_not_empty);
			MAP_ERR_TO_COND(ERROR_DIRECTORY, invalid_argument);
			MAP_ERR_TO_COND(ERROR_DISK_FULL, no_space_on_device);
			MAP_ERR_TO_COND(ERROR_FILE_EXISTS, file_exists);
			MAP_ERR_TO_COND(ERROR_FILE_NOT_FOUND, no_such_file_or_directory);
			MAP_ERR_TO_COND(ERROR_HANDLE_DISK_FULL, no_space_on_device);
			MAP_ERR_TO_COND(ERROR_INVALID_ACCESS, permission_denied);
			MAP_ERR_TO_COND(ERROR_INVALID_DRIVE, no_such_device);
			MAP_ERR_TO_COND(ERROR_INVALID_FUNCTION, function_not_supported);
			MAP_ERR_TO_COND(ERROR_INVALID_HANDLE, invalid_argument);
			MAP_ERR_TO_COND(ERROR_INVALID_NAME, invalid_argument);
			MAP_ERR_TO_COND(ERROR_LOCK_VIOLATION, no_lock_available);
			MAP_ERR_TO_COND(ERROR_LOCKED, no_lock_available);
			MAP_ERR_TO_COND(ERROR_NEGATIVE_SEEK, invalid_argument);
			MAP_ERR_TO_COND(ERROR_NOACCESS, permission_denied);
			MAP_ERR_TO_COND(ERROR_NOT_ENOUGH_MEMORY, not_enough_memory);
			MAP_ERR_TO_COND(ERROR_NOT_READY, resource_unavailable_try_again);
			MAP_ERR_TO_COND(ERROR_OPEN_FAILED, io_error);
			MAP_ERR_TO_COND(ERROR_OPEN_FILES, device_or_resource_busy);
			MAP_ERR_TO_COND(ERROR_OUTOFMEMORY, not_enough_memory);
			MAP_ERR_TO_COND(ERROR_PATH_NOT_FOUND, no_such_file_or_directory);
			MAP_ERR_TO_COND(ERROR_BAD_NETPATH, no_such_file_or_directory);
			MAP_ERR_TO_COND(ERROR_READ_FAULT, io_error);
			MAP_ERR_TO_COND(ERROR_RETRY, resource_unavailable_try_again);
			MAP_ERR_TO_COND(ERROR_SEEK, io_error);
			MAP_ERR_TO_COND(ERROR_SHARING_VIOLATION, permission_denied);
			MAP_ERR_TO_COND(ERROR_TOO_MANY_OPEN_FILES, too_many_files_open);
			MAP_ERR_TO_COND(ERROR_WRITE_FAULT, io_error);
			MAP_ERR_TO_COND(ERROR_WRITE_PROTECT, permission_denied);
			MAP_ERR_TO_COND(WSAEACCES, permission_denied);
			MAP_ERR_TO_COND(WSAEBADF, bad_file_descriptor);
			MAP_ERR_TO_COND(WSAEFAULT, bad_address);
			MAP_ERR_TO_COND(WSAEINTR, interrupted);
			MAP_ERR_TO_COND(WSAEINVAL, invalid_argument);
			MAP_ERR_TO_COND(WSAEMFILE, too_many_files_open);
			MAP_ERR_TO_COND(WSAENAMETOOLONG, filename_too_long);
		default:
			return std::error_code(windows_error_code, std::system_category());
		}
		#undef MAP_ERR_TO_COND
	}

	static std::error_code rename_internal(HANDLE from_handle, const std::wstring &wide_to, bool replace_if_exists)
	{
		std::vector<char> rename_info_buf(sizeof(FILE_RENAME_INFO) - sizeof(wchar_t) + (wide_to.size() * sizeof(wchar_t)));
		FILE_RENAME_INFO &rename_info = *reinterpret_cast<FILE_RENAME_INFO*>(rename_info_buf.data());
		rename_info.ReplaceIfExists = replace_if_exists;
		rename_info.RootDirectory = 0;
		rename_info.FileNameLength = DWORD(wide_to.size() * sizeof(wchar_t));
		std::copy(wide_to.begin(), wide_to.end(), &rename_info.FileName[0]);

		::SetLastError(ERROR_SUCCESS);
		if (! ::SetFileInformationByHandle(from_handle, FileRenameInfo, &rename_info, (DWORD)rename_info_buf.size())) {
			unsigned Error = GetLastError();
			if (Error == ERROR_SUCCESS)
		  		Error = ERROR_CALL_NOT_IMPLEMENTED; // Wine doesn't always set error code.
			return map_windows_error(Error);
		}

		return std::error_code();
	}

	static std::error_code real_path_from_handle(HANDLE H, std::wstring &buffer)
	{
		buffer.resize(MAX_PATH + 1);
		DWORD CountChars = ::GetFinalPathNameByHandleW(H, (LPWSTR)buffer.data(), (DWORD)buffer.size() - 1, FILE_NAME_NORMALIZED);
	  	if (CountChars > buffer.size()) {
	    	// The buffer wasn't big enough, try again.  In this case the return value *does* indicate the size of the null terminator.
	    	buffer.resize((size_t)CountChars);
	    	CountChars = ::GetFinalPathNameByHandleW(H, (LPWSTR)buffer.data(), (DWORD)buffer.size() - 1, FILE_NAME_NORMALIZED);
	  	}
	  	if (CountChars == 0)
	    	return map_windows_error(GetLastError());
	  	buffer.resize(CountChars);
	  	return std::error_code();
	}

	std::error_code rename(const std::string &from, const std::string &to)
	{
		// Convert to utf-16.
		std::wstring wide_from = boost::nowide::widen(from);
		std::wstring wide_to   = boost::nowide::widen(to);

		ScopedFileHandle from_handle;
		// Retry this a few times to defeat badly behaved file system scanners.
		for (unsigned retry = 0; retry != 200; ++ retry) {
			if (retry != 0)
		  		::Sleep(10);
			from_handle = ::CreateFileW((LPWSTR)wide_from.data(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (from_handle)
		  		break;
		}
		//BBS: add some log for error tracing
		if (! from_handle)
		{
			auto err_code = map_windows_error(GetLastError());
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("can not open file %1%, error: %2%") % from.c_str() % err_code.message();
			return err_code;
		}

		// We normally expect this loop to succeed after a few iterations. If it
		// requires more than 200 tries, it's more likely that the failures are due to
		// a true error, so stop trying.
		for (unsigned retry = 0; retry != 200; ++ retry) {
			auto errcode = rename_internal(from_handle, wide_to, true);

			if (errcode == std::error_code(ERROR_CALL_NOT_IMPLEMENTED, std::system_category())) {
		  		// Wine doesn't support SetFileInformationByHandle in rename_internal.
		  		// Fall back to MoveFileEx.
		  		if (std::error_code errcode2 = real_path_from_handle(from_handle, wide_from))
		    		return errcode2;
		  		if (::MoveFileExW((LPCWSTR)wide_from.data(), (LPCWSTR)wide_to.data(), MOVEFILE_REPLACE_EXISTING))
		    		return std::error_code();
		  		return map_windows_error(GetLastError());
			}

			if (! errcode || errcode != std::errc::permission_denied)
		  		return errcode;

			//BBS: add some log for error tracing
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(",first rename file from %1% to %2% failed, reason: %3%") % from.c_str() % to.c_str() % errcode.message();
			// The destination file probably exists and is currently open in another
			// process, either because the file was opened without FILE_SHARE_DELETE or
			// it is mapped into memory (e.g. using MemoryBuffer). Rename it in order to
			// move it out of the way of the source file. Use FILE_FLAG_DELETE_ON_CLOSE
			// to arrange for the destination file to be deleted when the other process
			// closes it.
			ScopedFileHandle to_handle(::CreateFileW((LPCWSTR)wide_to.data(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL));
			if (! to_handle) {
				auto errcode = map_windows_error(GetLastError());
				// Another process might have raced with us and moved the existing file
				// out of the way before we had a chance to open it. If that happens, try
				// to rename the source file again.
				if (errcode == std::errc::no_such_file_or_directory)
					continue;

				//BBS: add some log for error tracing
				BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(",open dest file %1% failed, reason: %2%") % to.c_str() % errcode.message();
				return errcode;
			}

			BY_HANDLE_FILE_INFORMATION FI;
			if (! ::GetFileInformationByHandle(to_handle, &FI))
		  		return map_windows_error(GetLastError());

			// Try to find a unique new name for the destination file.
			for (unsigned unique_id = 0; unique_id != 200; ++ unique_id) {
				std::wstring tmp_filename = wide_to + L".tmp" + std::to_wstring(unique_id);
				std::error_code errcode = rename_internal(to_handle, tmp_filename, false);
				if (errcode) {
					if (errcode == std::make_error_code(std::errc::file_exists) || errcode == std::make_error_code(std::errc::permission_denied)) {
						// Again, another process might have raced with us and moved the file
						// before we could move it. Check whether this is the case, as it
						// might have caused the permission denied error. If that was the
						// case, we don't need to move it ourselves.
						ScopedFileHandle to_handle2(::CreateFileW((LPCWSTR)wide_to.data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
						if (! to_handle2) {
							auto errcode = map_windows_error(GetLastError());
							if (errcode == std::errc::no_such_file_or_directory)
						  		break;
							//BBS: add some log for error tracing
							BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", line %1%, error: %2%") % __LINE__ % errcode.message();
							return errcode;
						}
						BY_HANDLE_FILE_INFORMATION FI2;
						if (! ::GetFileInformationByHandle(to_handle2, &FI2))
							return map_windows_error(GetLastError());
						if (FI.nFileIndexHigh != FI2.nFileIndexHigh || FI.nFileIndexLow != FI2.nFileIndexLow || FI.dwVolumeSerialNumber != FI2.dwVolumeSerialNumber)
							break;
						continue;
					}
					//BBS: add some log for error tracing
					BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", line %1%, error: %2%") % __LINE__ % errcode.message();
					return errcode;
				}
				break;
			}

			// Okay, the old destination file has probably been moved out of the way at
			// this point, so try to rename the source file again. Still, another
			// process might have raced with us to create and open the destination
			// file, so we need to keep doing this until we succeed.
		}

		// The most likely root cause.
		//BBS: add some log for error tracing
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", line %1%, error in the end, permission_denied") % __LINE__;
		return std::make_error_code(std::errc::permission_denied);
	}
} // namespace WindowsSupport
#endif /* _WIN32 */

// borrowed from LVVM lib/Support/Windows/Path.inc
std::error_code rename_file(const std::string &from, const std::string &to)
{
#ifdef _WIN32
	return WindowsSupport::rename(from, to);
#else
	boost::nowide::remove(to.c_str());
	return std::make_error_code(static_cast<std::errc>(boost::nowide::rename(from.c_str(), to.c_str())));
#endif
}

#ifdef __linux__
// Copied from boost::filesystem.
// Called by copy_file_linux() in case linux sendfile() API is not supported.
int copy_file_linux_read_write(int infile, int outfile, uintmax_t file_size)
{
    std::vector<char> buf(
	    // Prefer the buffer to be larger than the file size so that we don't have
	    // to perform an extra read if the file fits in the buffer exactly.
    	std::clamp<size_t>(file_size + (file_size < ~static_cast<uintmax_t >(0u)),
		// Min and max buffer sizes are selected to minimize the overhead from system calls.
		// The values are picked based on coreutils cp(1) benchmarking data described here:
		// https://github.com/coreutils/coreutils/blob/d1b0257077c0b0f0ee25087efd46270345d1dd1f/src/ioblksize.h#L23-L72
    			   		   8u * 1024u, 256u * 1024u),
    	0);

#if defined(POSIX_FADV_SEQUENTIAL)
    ::posix_fadvise(infile, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // Don't use file size to limit the amount of data to copy since some filesystems, like procfs or sysfs,
    // provide files with generated content and indicate that their size is zero or 4096. Just copy as much data
    // as we can read from the input file.
    while (true) {
        ssize_t sz_read = ::read(infile, buf.data(), buf.size());
        if (sz_read == 0)
            break;
        if (sz_read < 0) {
            int err = errno;
            if (err == EINTR)
                continue;
            return err;
        }
        // Allow for partial writes - see Advanced Unix Programming (2nd Ed.),
        // Marc Rochkind, Addison-Wesley, 2004, page 94
        for (ssize_t sz_wrote = 0; sz_wrote < sz_read;) {
            ssize_t sz = ::write(outfile, buf.data() + sz_wrote, static_cast<std::size_t>(sz_read - sz_wrote));
            if (sz < 0) {
                int err = errno;
                if (err == EINTR)
                    continue;
                return err;
            }
            sz_wrote += sz;
        }
    }
    return 0;
}

// Copied from boost::filesystem, to support copying a file to a weird filesystem, which does not support changing file attributes,
// for example ChromeOS Linux integration or FlashAIR WebDAV.
// Copied and simplified from boost::filesystem::detail::copy_file() with option = overwrite_if_exists and with just the Linux path kept,
// and only features supported by Linux 3.10 (on our build server with CentOS 7) are kept, namely sendfile with ranges and statx() are not supported.
bool copy_file_linux(const boost::filesystem::path &from, const boost::filesystem::path &to, boost::system::error_code &ec)
{
	using namespace boost::filesystem;

	struct fd_wrapper
	{
		int fd { -1 };
		fd_wrapper() = default;
		explicit fd_wrapper(int fd) throw() : fd(fd) {}
		~fd_wrapper() throw() { if (fd >= 0) ::close(fd); }
	};

	ec.clear();
  	int err = 0;

  	// Note: Declare fd_wrappers here so that errno is not clobbered by close() that may be called in fd_wrapper destructors
  	fd_wrapper infile, outfile;

  	while (true) {
    	infile.fd = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    	if (infile.fd < 0) {
      		err = errno;
      		if (err == EINTR)
        		continue;
		fail:
			ec.assign(err, boost::system::system_category());
  			return false;
    	}
    	break;
  	}

	struct ::stat from_stat;
	if (::fstat(infile.fd, &from_stat) != 0) {
		fail_errno:
		err = errno;
		goto fail;
	}

  	const mode_t from_mode = from_stat.st_mode;
  	if (!S_ISREG(from_mode)) {
    	err = ENOSYS;
    	goto fail;
  	}

  	// Enable writing for the newly created files. Having write permission set is important e.g. for NFS,
  	// which checks the file permission on the server, even if the client's file descriptor supports writing.
  	mode_t to_mode = from_mode | S_IWUSR;
  	int oflag = O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC;

	while (true) {
	  	outfile.fd = ::open(to.c_str(), oflag, to_mode);
	  	if (outfile.fd < 0) {
	    	err = errno;
	    	if (err == EINTR)
	      		continue;
	    	goto fail;
	  	}
	  	break;
	}

	struct ::stat to_stat;
	if (::fstat(outfile.fd, &to_stat) != 0)
		goto fail_errno;

	to_mode = to_stat.st_mode;
	if (!S_ISREG(to_mode)) {
		err = ENOSYS;
		goto fail;
	}

	if (from_stat.st_dev == to_stat.st_dev && from_stat.st_ino == to_stat.st_ino) {
		err = EEXIST;
		goto fail;
	}

	//! copy_file implementation that uses sendfile loop. Requires sendfile to support file descriptors.
	//FIXME Vojtech: This is a copy loop valid for Linux 2.6.33 and newer.
	// copy_file_data_copy_file_range() supports cross-filesystem copying since 5.3, but Vojtech did not want to polute this
	// function with that, we don't think the performance gain is worth it for the types of files we are copying,
	// and our build server based on CentOS 7 with Linux 3.10 does not support that anyways.
	{
		// sendfile will not send more than this amount of data in one call
		constexpr std::size_t max_send_size = 0x7ffff000u;
		uintmax_t offset = 0u;
		while (off_t(offset) < from_stat.st_size) {
			uintmax_t size_left = from_stat.st_size - offset;
			std::size_t size_to_copy = max_send_size;
			if (size_left < static_cast<uintmax_t>(max_send_size))
				size_to_copy = static_cast<std::size_t>(size_left);
			ssize_t sz = ::sendfile(outfile.fd, infile.fd, nullptr, size_to_copy);
			if (sz < 0) {
				err = errno;
	            if (offset == 0u) {
	                // sendfile may fail with EINVAL if the underlying filesystem does not support it.
	                // See https://patchwork.kernel.org/project/linux-nfs/patch/20190411183418.4510-1-olga.kornievskaia@gmail.com/
	                // https://bugzilla.redhat.com/show_bug.cgi?id=1783554.
	                // https://github.com/boostorg/filesystem/commit/4b9052f1e0b2acf625e8247582f44acdcc78a4ce
	                if (err == EINVAL || err == EOPNOTSUPP) {
						err = copy_file_linux_read_write(infile.fd, outfile.fd, from_stat.st_size);
						if (err < 0)
							goto fail;
						// Succeeded.
	                	break;
	                }
	            }
				if (err == EINTR)
					continue;
				if (err == 0)
					break;
				goto fail; // err already contains the error code
			}
			offset += sz;
		}
	}

	// If we created a new file with an explicitly added S_IWUSR permission,
	// we may need to update its mode bits to match the source file.
	if (to_mode != from_mode && ::fchmod(outfile.fd, from_mode) != 0) {
		if (platform_flavor() == PlatformFlavor::LinuxOnChromium) {
			// Ignore that. 9p filesystem does not allow fmod().
			BOOST_LOG_TRIVIAL(info) << "copy_file_linux() failed to fchmod() the output file \"" << to.string() << "\" to " << from_mode << ": " << ec.message() <<
				" This may be expected when writing to a 9p filesystem.";
		} else {
			// Generic linux. Write out an error to console. At least we may get some feedback.
			BOOST_LOG_TRIVIAL(error) << "copy_file_linux() failed to fchmod() the output file \"" << to.string() << "\" to " << from_mode << ": " << ec.message();
		}
	}

	// Note: Use fsync/fdatasync followed by close to avoid dealing with the possibility of close failing with EINTR.
	// Even if close fails, including with EINTR, most operating systems (presumably, except HP-UX) will close the
	// file descriptor upon its return. This means that if an error happens later, when the OS flushes data to the
	// underlying media, this error will go unnoticed and we have no way to receive it from close. Calling fsync/fdatasync
	// ensures that all data have been written, and even if close fails for some unfathomable reason, we don't really
	// care at that point.
	err = ::fdatasync(outfile.fd);
	if (err != 0)
		goto fail_errno;

	return true;
}
#endif // __linux__

CopyFileResult copy_file_inner(const std::string& from, const std::string& to, std::string& error_message)
{
	const boost::filesystem::path source(from);
	const boost::filesystem::path target(to);
	static const auto perms = boost::filesystem::owner_read | boost::filesystem::owner_write | boost::filesystem::group_read | boost::filesystem::others_read;   // aka 644

	// Make sure the file has correct permission both before and after we copy over it.
	// NOTE: error_code variants are used here to supress expception throwing.
	// Error code of permission() calls is ignored on purpose - if they fail,
	// the copy_file() function will fail appropriately and we don't want the permission()
	// calls to cause needless failures on permissionless filesystems (ie. FATs on SD cards etc.)
	// or when the target file doesn't exist.
	boost::system::error_code ec;
	boost::filesystem::permissions(target, perms, ec);
	if (ec)
		BOOST_LOG_TRIVIAL(debug) << "boost::filesystem::permisions before copy error message (this could be irrelevant message based on file system): " << ec.message();
	ec.clear();
#ifdef __linux__
	// We want to allow copying files on Linux to succeed even if changing the file attributes fails.
	// That may happen when copying on some exotic file system, for example Linux on Chrome.
	copy_file_linux(source, target, ec);
#else // __linux__
	boost::filesystem::copy_file(source, target, boost::filesystem::copy_option::overwrite_if_exists, ec);
#endif // __linux__
	if (ec) {
		error_message = ec.message();
        BOOST_LOG_TRIVIAL(error) << boost::format("###copy_file from %1% to %2% failed, error: %3% ")
            %source.string() %target.string() % error_message;
		return FAIL_COPY_FILE;
	}
	ec.clear();
	boost::filesystem::permissions(target, perms, ec);
	if (ec)
		BOOST_LOG_TRIVIAL(debug) << "boost::filesystem::permisions after copy error message (this could be irrelevant message based on file system): " << ec.message();
	return SUCCESS;
}

CopyFileResult copy_file(const std::string &from, const std::string &to, std::string& error_message, const bool with_check)
{
#ifdef WIN32
    //wxString src = from_u8(from);
    //wxString dest = from_u8(to);
    const char* src_str = from.c_str();
    const char* dest_str = to.c_str();
    int src_wlen = ::MultiByteToWideChar(CP_UTF8, NULL, src_str, strlen(src_str), NULL, 0);
    wchar_t* src_wstr = new wchar_t[src_wlen + 1];
    ::MultiByteToWideChar(CP_UTF8, NULL, src_str, strlen(src_str), src_wstr, src_wlen);
    src_wstr[src_wlen] = '\0';

    int dst_wlen = ::MultiByteToWideChar(CP_UTF8, NULL, dest_str, strlen(dest_str), NULL, 0);
    wchar_t* dst_wstr = new wchar_t[dst_wlen + 1];
    ::MultiByteToWideChar(CP_UTF8, NULL, dest_str, strlen(dest_str), dst_wstr, dst_wlen);
    dst_wstr[dst_wlen] = '\0';

    CopyFileResult ret = SUCCESS;
    BOOL result = CopyFileW(src_wstr, dst_wstr, FALSE);
    if (!result) {
        DWORD errCode = GetLastError();
        error_message = "Error: " + errCode;
        ret = FAIL_COPY_FILE;
        goto __finished;
    }

__finished:
    if (src_wstr)
        delete[] src_wstr;
    if (dst_wstr)
        delete[] dst_wstr;

    return ret;
#else
    std::string to_temp = to + ".tmp";
    CopyFileResult ret_val = copy_file_inner(from, to_temp, error_message);
    if(ret_val == SUCCESS)
    {
        if (with_check)
            ret_val = check_copy(from, to_temp);

        if (ret_val == 0 && rename_file(to_temp, to))
            ret_val = FAIL_RENAMING;
    }
    return ret_val;
#endif
}

bool copy_framework(const std::string &from, const std::string &to)
{
    boost::filesystem::path src(from), dst(to);
    try {
        if (!boost::filesystem::is_directory(src)) {
            std::cerr << "Error: Source is not a directory: " << src << std::endl;
            return false;
        }
        boost::filesystem::create_directories(dst);
        for (boost::filesystem::directory_iterator it(src); it != boost::filesystem::directory_iterator(); ++it) {
            const auto &entry     = it->path();
            const auto  dest_path = dst / entry.filename();

            if (boost::filesystem::is_symlink(entry)) {
                boost::filesystem::copy_symlink(entry, dest_path);
            } else if (boost::filesystem::is_directory(entry)) {
                copy_framework(it->path().string(), dest_path.string());
            } else {
                boost::filesystem::copy(entry, dest_path, boost::filesystem::copy_options::overwrite_existing);
            }
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Filesystem error: " << e.what();
    }
    return false;
}

CopyFileResult check_copy(const std::string &origin, const std::string &copy)
{
	boost::nowide::ifstream f1(origin, std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
	boost::nowide::ifstream f2(copy, std::ifstream::in | std::ifstream::binary | std::ifstream::ate);

	if (f1.fail())
		return FAIL_CHECK_ORIGIN_NOT_OPENED;
	if (f2.fail())
		return FAIL_CHECK_TARGET_NOT_OPENED;

	std::streampos fsize = f1.tellg();
	if (fsize != f2.tellg())
		return FAIL_FILES_DIFFERENT;

	f1.seekg(0, std::ifstream::beg);
	f2.seekg(0, std::ifstream::beg);

	// Compare by reading 8 MiB buffers one at a time.
	size_t 			  buffer_size = 8 * 1024 * 1024;
	std::vector<char> buffer_origin(buffer_size, 0);
	std::vector<char> buffer_copy(buffer_size, 0);
	do {
		f1.read(buffer_origin.data(), buffer_size);
        f2.read(buffer_copy.data(), buffer_size);
		std::streampos origin_cnt = f1.gcount();
		std::streampos copy_cnt   = f2.gcount();
		if (origin_cnt != copy_cnt ||
			(origin_cnt > 0 && std::memcmp(buffer_origin.data(), buffer_copy.data(), origin_cnt) != 0))
			// Files are different.
			return FAIL_FILES_DIFFERENT;
		fsize -= origin_cnt;
    } while (f1.good() && f2.good());

    // All data has been read and compared equal.
    return (f1.eof() && f2.eof() && fsize == 0) ? SUCCESS : FAIL_FILES_DIFFERENT;
}

// Ignore system and hidden files, which may be created by the DropBox synchronisation process.
bool is_plain_file(const boost::filesystem::directory_entry &dir_entry)
{
    if (! boost::filesystem::is_regular_file(dir_entry.status()))
        return false;
#ifdef _MSC_VER
    DWORD attributes = GetFileAttributesW(boost::nowide::widen(dir_entry.path().string()).c_str());
    return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0;
#else
    return true;
#endif
}

bool is_ini_file(const boost::filesystem::directory_entry &dir_entry)
{
    return is_plain_file(dir_entry) && strcasecmp(dir_entry.path().extension().string().c_str(), ".ini") == 0;
}

bool is_idx_file(const boost::filesystem::directory_entry &dir_entry)
{
	return is_plain_file(dir_entry) && strcasecmp(dir_entry.path().extension().string().c_str(), ".idx") == 0;
}

//BBS: refine gcode appendix
bool is_gcode_file(const std::string &path)
{
	return boost::iends_with(path, ".gcode"); // || boost::iends_with(path, ".g");
}

//BBS: add json support
bool is_json_file(const std::string& path)
{
	return boost::iends_with(path, ".json");
}

bool is_img_file(const std::string &path)
{
	return boost::iends_with(path, ".png") || boost::iends_with(path, ".svg");
}

bool is_gallery_file(const boost::filesystem::directory_entry& dir_entry, char const* type)
{
	return is_plain_file(dir_entry) && strcasecmp(dir_entry.path().extension().string().c_str(), type) == 0;
}

bool is_gallery_file(const std::string &path, char const* type)
{
	return boost::iends_with(path, type);
}

bool is_shapes_dir(const std::string& dir)
{
	return dir == sys_shapes_dir() || dir == custom_shapes_dir();
}

} // namespace Slic3r

#ifdef WIN32
    #ifndef NOMINMAX
    # define NOMINMAX
    #endif
    #include <windows.h>
#endif /* WIN32 */

namespace Slic3r {

size_t get_utf8_sequence_length(const std::string& text, size_t pos)
{
	assert(pos < text.size());
	return get_utf8_sequence_length(text.c_str() + pos, text.size() - pos);
}

size_t get_utf8_sequence_length(const char *seq, size_t size)
{
	size_t length = 0;
	unsigned char c = seq[0];
	if (c < 0x80) { // 0x00-0x7F
		// is ASCII letter
		length++;
	}
	// Bytes 0x80 to 0xBD are trailer bytes in a multibyte sequence.
	// pos is in the middle of a utf-8 sequence. Add the utf-8 trailer bytes.
	else if (c < 0xC0) { // 0x80-0xBF
		length++;
		while (length < size) {
			c = seq[length];
			if (c < 0x80 || c >= 0xC0) {
				break; // prevent overrun
			}
			length++; // add a utf-8 trailer byte
		}
	}
	// Bytes 0xC0 to 0xFD are header bytes in a multibyte sequence.
	// The number of one bits above the topmost zero bit indicates the number of bytes (including this one) in the whole sequence.
	else if (c < 0xE0) { // 0xC0-0xDF
	 // add a utf-8 sequence (2 bytes)
		if (2 > size) {
			return size; // prevent overrun
		}
		length += 2;
	}
	else if (c < 0xF0) { // 0xE0-0xEF
	 // add a utf-8 sequence (3 bytes)
		if (3 > size) {
			return size; // prevent overrun
		}
		length += 3;
	}
	else if (c < 0xF8) { // 0xF0-0xF7
	 // add a utf-8 sequence (4 bytes)
		if (4 > size) {
			return size; // prevent overrun
		}
		length += 4;
	}
	else if (c < 0xFC) { // 0xF8-0xFB
	 // add a utf-8 sequence (5 bytes)
		if (5 > size) {
			return size; // prevent overrun
		}
		length += 5;
	}
	else if (c < 0xFE) { // 0xFC-0xFD
	 // add a utf-8 sequence (6 bytes)
		if (6 > size) {
			return size; // prevent overrun
		}
		length += 6;
	}
	else { // 0xFE-0xFF
	 // not a utf-8 sequence
		length++;
	}
	return length;
}

// Encode an UTF-8 string to the local code page.
std::string encode_path(const char *src)
{
#ifdef WIN32
    // Convert the source utf8 encoded string to a wide string.
    std::wstring wstr_src = boost::nowide::widen(src);
    if (wstr_src.length() == 0)
        return std::string();
    // Convert a wide string to a local code page.
    int size_needed = ::WideCharToMultiByte(0, 0, wstr_src.data(), (int)wstr_src.size(), nullptr, 0, nullptr, nullptr);
    std::string str_dst(size_needed, 0);
    ::WideCharToMultiByte(0, 0, wstr_src.data(), (int)wstr_src.size(), str_dst.data(), size_needed, nullptr, nullptr);
    return str_dst;
#else /* WIN32 */
    return src;
#endif /* WIN32 */
}

// Encode an 8-bit string from a local code page to UTF-8.
// Multibyte to utf8
std::string decode_path(const char *src)
{
#ifdef WIN32
    int len = int(strlen(src));
    if (len == 0)
        return std::string();
    // Convert the string encoded using the local code page to a wide string.
    int size_needed = ::MultiByteToWideChar(0, 0, src, len, nullptr, 0);
    std::wstring wstr_dst(size_needed, 0);
    ::MultiByteToWideChar(0, 0, src, len, wstr_dst.data(), size_needed);
    // Convert a wide string to utf8.
    return boost::nowide::narrow(wstr_dst.c_str());
#else /* WIN32 */
    return src;
#endif /* WIN32 */
}

std::string normalize_utf8_nfc(const char *src)
{
    static std::locale locale_utf8(boost::locale::generator().generate(""));
    return boost::locale::normalize(src, boost::locale::norm_nfc, locale_utf8);
}

std::vector<std::string> split_string(const std::string &str, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string substr;

    while (std::getline(ss, substr, delimiter)) {
        result.push_back(substr);
    }
    return result;
}

namespace PerlUtils {
    // Get a file name including the extension.
    std::string path_to_filename(const char *src)       { return boost::filesystem::path(src).filename().string(); }
    // Get a file name without the extension.
    std::string path_to_stem(const char *src)           { return boost::filesystem::path(src).stem().string(); }
    // Get just the extension.
    std::string path_to_extension(const char *src)      { return boost::filesystem::path(src).extension().string(); }
    // Get a directory without the trailing slash.
    std::string path_to_parent_path(const char *src)    { return boost::filesystem::path(src).parent_path().string(); }
};


std::string string_printf(const char *format, ...)
{
    va_list args1;
    va_start(args1, format);
    va_list args2;
    va_copy(args2, args1);

    static const size_t INITIAL_LEN = 200;
    std::string buffer(INITIAL_LEN, '\0');

    int bufflen = ::vsnprintf(buffer.data(), INITIAL_LEN - 1, format, args1);

    if (bufflen >= int(INITIAL_LEN)) {
        buffer.resize(size_t(bufflen) + 1);
        ::vsnprintf(buffer.data(), buffer.size(), format, args2);
    }

    buffer.resize(bufflen);

    return buffer;
}

std::string header_slic3r_generated()
{
	return std::string(SLIC3R_APP_NAME " " SoftFever_VERSION);
}

std::string header_gcodeviewer_generated()
{
	return std::string(GCODEVIEWER_APP_NAME " " SoftFever_VERSION);
}

unsigned get_current_pid()
{
#ifdef WIN32
    return GetCurrentProcessId();
#else
    return ::getpid();
#endif
}

// BBS: backup & restore
std::string get_process_name(int pid)
{
#ifdef WIN32
	char name[MAX_PATH] = { 0 };
	if (pid == 0) {
		GetModuleFileNameA(NULL, name, MAX_PATH);
	}
	else {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
		if (h == INVALID_HANDLE_VALUE) return {};
		GetModuleFileNameExA(h, NULL, name, MAX_PATH);
		CloseHandle(h);
	}
	char* p = name;
	while (auto q = strchr(p + 1, '\\'))
		p = q;
	return decode_path(p);
#elif defined __APPLE__
	char pathbuf[PROC_PIDPATHINFO_MAXSIZE] = { 0 };
	if (pid == 0) pid = ::getpid();
	int ret = proc_pidpath(pid, pathbuf, sizeof(pathbuf));
	if (ret <= 0) return {};
	char* p = pathbuf;
	while (auto q = strchr(p + 1, '/')) p = q;
	return p;
#else
    char pathbuf[512]  = {0};
    char proc_path[32] = "/proc/self/exe";
    if (pid != 0) { snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", pid); }
    if (readlink(proc_path, pathbuf, sizeof(pathbuf)) < 0) {
        perror(NULL);
        return {};
    }
    char *p = pathbuf;
    while (auto q = strchr(p + 1, '/')) p = q;
    return p;
#endif
}

//FIXME this has potentially O(n^2) time complexity!
std::string xml_escape(std::string text, bool is_marked/* = false*/)
{
    std::string::size_type pos = 0;
    for (;;)
    {
        pos = text.find_first_of("\"\'&<>", pos);
        if (pos == std::string::npos)
            break;

        std::string replacement;
        switch (text[pos])
        {
        case '\"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        case '&':  replacement = "&amp;";  break;
        case '<':  replacement = is_marked ? "<" :"&lt;"; break;
        case '>':  replacement = is_marked ? ">" :"&gt;"; break;
        default: break;
        }

        text.replace(pos, 1, replacement);
        pos += replacement.size();
    }

    return text;
}

// Definition of escape symbols https://www.w3.org/TR/REC-xml/#AVNormalize
// During the read of xml attribute normalization of white spaces is applied
// Soo for not lose white space character it is escaped before store
std::string xml_escape_double_quotes_attribute_value(std::string text)
{
    std::string::size_type pos = 0;
    for (;;) {
        pos = text.find_first_of("\"&<\r\n\t", pos);
        if (pos == std::string::npos) break;

        std::string replacement;
        switch (text[pos]) {
        case '\"': replacement = "&quot;"; break;
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '\r': replacement = "&#xD;"; break;
        case '\n': replacement = "&#xA;"; break;
        case '\t': replacement = "&#x9;"; break;
        default: break;
        }

        text.replace(pos, 1, replacement);
        pos += replacement.size();
    }

    return text;
}

std::string xml_unescape(std::string s)
{
	std::string ret;
	std::string::size_type i = 0;
	std::string::size_type pos = 0;
	while (i < s.size()) {
		std::string rep;
		if (s[i] == '&') {
			if (s.substr(i, 4) == "&lt;") {
				ret += s.substr(pos, i - pos) + "<";
				i += 4;
				pos = i;
			}
			else if (s.substr(i, 4) == "&gt;") {
				ret += s.substr(pos, i - pos) + ">";
				i += 4;
				pos = i;
			}
			else if (s.substr(i, 5) == "&amp;") {
				ret += s.substr(pos, i - pos) + "&";
				i += 5;
				pos = i;
			}
			else {
				++i;
			}
		}
		else {
			++i;
		}
	}

	ret += s.substr(pos);
	return ret;
}

std::string format_memsize_MB(size_t n)
{
    std::string out;
    size_t n2 = 0;
    size_t scale = 1;
    // Round to MB
    n +=  500000;
    n /= 1000000;
    while (n >= 1000) {
        n2 = n2 + scale * (n % 1000);
        n /= 1000;
        scale *= 1000;
    }
    char buf[8];
    sprintf(buf, "%d", (int)n);
    out = buf;
    while (scale != 1) {
        scale /= 1000;
        n = n2 / scale;
        n2 = n2  % scale;
        sprintf(buf, ",%03d", (int)n);
        out += buf;
    }
    return out + "MB";
}

// Returns platform-specific string to be used as log output or parsed in SysInfoDialog.
// The latter parses the string with (semi)colons as separators, it should look about as
// "desc1: value1; desc2: value2" or similar (spaces should not matter).
std::string log_memory_info(bool ignore_loglevel)
{
    std::string out;
    if (ignore_loglevel || logSeverity <= boost::log::trivial::info) {
#ifdef WIN32
    #ifndef PROCESS_MEMORY_COUNTERS_EX
        // MingW32 doesn't have this struct in psapi.h
        typedef struct _PROCESS_MEMORY_COUNTERS_EX {
          DWORD  cb;
          DWORD  PageFaultCount;
          SIZE_T PeakWorkingSetSize;
          SIZE_T WorkingSetSize;
          SIZE_T QuotaPeakPagedPoolUsage;
          SIZE_T QuotaPagedPoolUsage;
          SIZE_T QuotaPeakNonPagedPoolUsage;
          SIZE_T QuotaNonPagedPoolUsage;
          SIZE_T PagefileUsage;
          SIZE_T PeakPagefileUsage;
          SIZE_T PrivateUsage;
        } PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;
    #endif /* PROCESS_MEMORY_COUNTERS_EX */

        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
            out = " WorkingSet: " + format_memsize_MB(pmc.WorkingSetSize) + "; PrivateBytes: " + format_memsize_MB(pmc.PrivateUsage) + "; Pagefile(peak): " + format_memsize_MB(pmc.PagefileUsage) + "(" + format_memsize_MB(pmc.PeakPagefileUsage) + ")";
        else
            out += " Used memory: N/A";
#elif defined(__linux__) or defined(__APPLE__)
        // Get current memory usage.
    #ifdef __APPLE__
        struct mach_task_basic_info info;
        mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
        out += " Resident memory: ";
        if ( task_info( mach_task_self( ), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount ) == KERN_SUCCESS )
            out += format_memsize_MB((size_t)info.resident_size);
        else
            out += "N/A";
    #else // i.e. __linux__
        size_t tSize = 0, resident = 0, share = 0;
        std::ifstream buffer("/proc/self/statm");
        if (buffer && (buffer >> tSize >> resident >> share)) {
            size_t page_size = (size_t)sysconf(_SC_PAGE_SIZE); // in case x86-64 is configured to use 2MB pages
            size_t rss = resident * page_size;
            out += " Resident memory: " + format_memsize_MB(rss);
            out += "; Shared memory: " + format_memsize_MB(share * page_size);
            out += "; Private memory: " + format_memsize_MB(rss - share * page_size);
        }
        else
            out += " Used memory: N/A";
    #endif
        // Now get peak memory usage.
        out += "; Peak memory usage: ";
        rusage memory_info;
        if (getrusage(RUSAGE_SELF, &memory_info) == 0)
        {
            size_t peak_mem_usage = (size_t)memory_info.ru_maxrss;
            #ifdef __linux__
                peak_mem_usage *= 1024;// getrusage returns the value in kB on linux
            #endif
            out += format_memsize_MB(peak_mem_usage);
        }
        else
            out += "N/A";
#endif
    }
    return out;
}

// Returns the size of physical memory (RAM) in bytes.
// http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
size_t total_physical_memory()
{
#if defined(_WIN32) && (defined(__CYGWIN__) || defined(__CYGWIN32__))
	// Cygwin under Windows. ------------------------------------
	// New 64-bit MEMORYSTATUSEX isn't available.  Use old 32.bit
	MEMORYSTATUS status;
	status.dwLength = sizeof(status);
	GlobalMemoryStatus( &status );
	return (size_t)status.dwTotalPhys;
#elif defined(_WIN32)
	// Windows. -------------------------------------------------
	// Use new 64-bit MEMORYSTATUSEX, not old 32-bit MEMORYSTATUS
	MEMORYSTATUSEX status;
	status.dwLength = sizeof(status);
	GlobalMemoryStatusEx( &status );
	return (size_t)status.ullTotalPhys;
#elif defined(__unix__) || defined(__unix) || defined(unix) || (defined(__APPLE__) && defined(__MACH__))
	// UNIX variants. -------------------------------------------
	// Prefer sysctl() over sysconf() except sysctl() HW_REALMEM and HW_PHYSMEM

#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
	int mib[2];
	mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
	mib[1] = HW_MEMSIZE;            // OSX. ---------------------
#elif defined(HW_PHYSMEM64)
	mib[1] = HW_PHYSMEM64;          // NetBSD, OpenBSD. ---------
#endif
	int64_t size = 0;               // 64-bit
	size_t len = sizeof( size );
	if ( sysctl( mib, 2, &size, &len, NULL, 0 ) == 0 )
		return (size_t)size;
	return 0L;			// Failed?

#elif defined(_SC_AIX_REALMEM)
	// AIX. -----------------------------------------------------
	return (size_t)sysconf( _SC_AIX_REALMEM ) * (size_t)1024L;

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
	// FreeBSD, Linux, OpenBSD, and Solaris. --------------------
	return (size_t)sysconf( _SC_PHYS_PAGES ) *
		(size_t)sysconf( _SC_PAGESIZE );

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
	// Legacy. --------------------------------------------------
	return (size_t)sysconf( _SC_PHYS_PAGES ) *
		(size_t)sysconf( _SC_PAGE_SIZE );

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
	// DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. --------
	int mib[2];
	mib[0] = CTL_HW;
#if defined(HW_REALMEM)
	mib[1] = HW_REALMEM;		// FreeBSD. -----------------
#elif defined(HW_PYSMEM)
	mib[1] = HW_PHYSMEM;		// Others. ------------------
#endif
	unsigned int size = 0;		// 32-bit
	size_t len = sizeof( size );
	if ( sysctl( mib, 2, &size, &len, NULL, 0 ) == 0 )
		return (size_t)size;
	return 0L;			// Failed?
#endif // sysctl and sysconf variants

#else
	return 0L;			// Unknown OS.
#endif
}

bool makedir(const std::string path) {
	// if dir doesn't exist, make it
#ifdef WIN32
	if (_access(path.c_str(), 0) != 0)
		return _mkdir(path.c_str()) == 0;
#elif __linux__
	if (opendir(path.c_str()) == NULL) {
		return mkdir(path.c_str(), 0777) == 0;
	}
#else  // I don't know how to make dir on Mac...
#endif
	return true;  // dir already exists
}

bool bbl_calc_md5(std::string &filename, std::string &md5_out)
{
    unsigned char digest[16];
    MD5_CTX       ctx;
    MD5_Init(&ctx);
    boost::nowide::ifstream ifs(filename, std::ios::binary);
    std::string                 buf(64 * 1024, 0);
    const std::size_t &         size      = boost::filesystem::file_size(filename);
    std::size_t                 left_size = size;
    while (ifs) {
        ifs.read(buf.data(), buf.size());
        int read_bytes = ifs.gcount();
        MD5_Update(&ctx, (unsigned char *) buf.data(), read_bytes);
    }
    MD5_Final(digest, &ctx);
    char md5_str[33];
    for (int j = 0; j < 16; j++) { sprintf(&md5_str[j * 2], "%02X", (unsigned int) digest[j]); }
    md5_out = std::string(md5_str);
    return true;
}

// SoftFever: copy directory recursively
void copy_directory_recursively(const boost::filesystem::path &source, const boost::filesystem::path &target, std::function<bool(const std::string)> filter)
{
    BOOST_LOG_TRIVIAL(info) << Slic3r::format("copy_directory_recursively %1% -> %2%", source, target);
    std::string error_message;

    if (boost::filesystem::exists(target))
        boost::filesystem::remove_all(target);
    boost::filesystem::create_directories(target);
    for (auto &dir_entry : boost::filesystem::directory_iterator(source))
    {
        std::string source_file = dir_entry.path().string();
        std::string name = dir_entry.path().filename().string();
        std::string target_file = target.string() + "/" + name;

        if (boost::filesystem::is_directory(dir_entry)) {
            const auto target_path = target / name;
            copy_directory_recursively(dir_entry, target_path);
        }
        else {
			if(filter && filter(name))
				continue;
            CopyFileResult cfr = copy_file(source_file, target_file, error_message, false);
            if (cfr != CopyFileResult::SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << "Copying failed(" << cfr << "): " << error_message;
                throw Slic3r::CriticalException(Slic3r::format(
                    ("Copying directory %1% to %2% failed: %3%"),
                    source, target, error_message));
            }
        }
    }
    return;
}

void save_string_file(const boost::filesystem::path& p, const std::string& str)
{
    boost::nowide::ofstream file;
    file.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    file.open(p.generic_string(), std::ios_base::binary);
    file.write(str.c_str(), str.size());
}

void load_string_file(const boost::filesystem::path& p, std::string& str)
{
    boost::nowide::ifstream file;
    file.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    file.open(p.generic_string(), std::ios_base::binary);
    std::size_t sz = static_cast<std::size_t>(boost::filesystem::file_size(p));
    str.resize(sz, '\0');
    file.read(&str[0], sz);
}

// pattern string supprt these pattern: "
// 1. 5#1, insert 1 solid layer every 5 layers. this can be simplified to 5
// 2."1,7,9", explicitly insert solid layer at layer 1, 7, 9
bool check_layer_id_pattern(const std::string& pattern, int layer_id){
    if (pattern.empty() || layer_id < 0)
        return false;

	// layer_id is 0-based, so we need to add 1 to make it 1-based
	layer_id++;

    // Remove whitespace and surrounding quotes.
    std::string p; p.reserve(pattern.size());
    for (char c : pattern) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        p.push_back(c);
    }
    if (!p.empty() && (p.front() == '"' || p.front() == '\''))
        p.erase(p.begin());
    if (!p.empty() && (p.back() == '"' || p.back() == '\''))
        p.pop_back();
    if (p.empty())
        return false;

    // Explicit list form: "1,7,9" or with counts per entry: "5,9#2,18"
    if (p.find(',') != std::string::npos) {
        size_t start = 0;
        while (start < p.size()) {
            size_t end = p.find(',', start);
            std::string token = p.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
            if (!token.empty()) {
                try {
                    size_t hash_pos_token = token.find('#');
                    if (hash_pos_token == std::string::npos) {
                        int value = std::stoi(token);
                        if (value == layer_id)
                            return true;
                    } else {
                        int base_layer = std::stoi(token.substr(0, hash_pos_token));
                        std::string count_str = token.substr(hash_pos_token + 1);
                        int local_count = 1;
                        if (!count_str.empty())
                            local_count = std::stoi(count_str);
                        if (base_layer > 0 && local_count > 0) {
                            if (layer_id >= base_layer && layer_id < base_layer + local_count)
                                return true;
                        }
                    }
                } catch (...) {
                    // Ignore invalid tokens
                }
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return false;
    }

    // Interval form: "N#K" or simplified "N" (equals to N#1)
    int interval = 0;
    int count    = 1;
    size_t hash_pos = p.find('#');
    try {
        if (hash_pos == std::string::npos) {
            interval = std::stoi(p);
        } else {
            interval = std::stoi(p.substr(0, hash_pos));
            std::string count_str = p.substr(hash_pos + 1);
            if (!count_str.empty())
                count = std::stoi(count_str);
        }
    } catch (...) {
        return false;
    }

    if (interval <= 0 || count <= 0)
        return false;

    // Layers are 1-based. Match layers interval, interval+1, ..., interval+count-1, then repeat every interval.
    if (layer_id < interval)
        return false;
    int mod = layer_id % interval; // For multiples, mod == 0
    return mod >= 0 && mod < count;
}


}; // namespace Slic3r
