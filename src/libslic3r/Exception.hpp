#ifndef _libslic3r_Exception_h_
#define _libslic3r_Exception_h_

#include <stdexcept>
#include <vector>

namespace Slic3r {

// PrusaSlicer's own exception hierarchy is derived from std::runtime_error.
// Base for Slicer's own exceptions.
class Exception : public std::runtime_error { using std::runtime_error::runtime_error; };
#define SLIC3R_DERIVE_EXCEPTION(DERIVED_EXCEPTION, PARENT_EXCEPTION) \
    class DERIVED_EXCEPTION : public PARENT_EXCEPTION { using PARENT_EXCEPTION::PARENT_EXCEPTION; }
// Critical exception produced by Slicer, such exception shall never propagate up to the UI thread.
// If that happens, an ugly fat message box with an ugly fat exclamation mark is displayed.
SLIC3R_DERIVE_EXCEPTION(CriticalException,  Exception);
SLIC3R_DERIVE_EXCEPTION(RuntimeError,       CriticalException);
SLIC3R_DERIVE_EXCEPTION(LogicError,         CriticalException);
SLIC3R_DERIVE_EXCEPTION(HardCrash,          CriticalException);
SLIC3R_DERIVE_EXCEPTION(InvalidArgument,    LogicError);
SLIC3R_DERIVE_EXCEPTION(OutOfRange,         LogicError);
SLIC3R_DERIVE_EXCEPTION(IOError,            CriticalException);
SLIC3R_DERIVE_EXCEPTION(FileIOError,        IOError);
SLIC3R_DERIVE_EXCEPTION(HostNetworkError,   IOError);
SLIC3R_DERIVE_EXCEPTION(ExportError,        CriticalException);
SLIC3R_DERIVE_EXCEPTION(PlaceholderParserError, RuntimeError);
// Runtime exception produced by Slicer. Such exception cancels the slicing process and it shall be shown in notifications.
//SLIC3R_DERIVE_EXCEPTION(SlicingError,       Exception);
class SlicingError : public Exception 
{
public:
    using Exception::Exception;
    SlicingError(std::string const &msg, size_t objectId) : Exception(msg), objectId_(objectId) {}
    size_t objectId() const { return objectId_; }

private:
    size_t objectId_ = 0;
};

class SlicingErrors : public Exception
{
public:
    using Exception::Exception;
    SlicingErrors(const std::vector<SlicingError> &errors) : Exception("Errors"), errors_(errors) {}

    std::vector<SlicingError> errors_;
};

#undef SLIC3R_DERIVE_EXCEPTION

} // namespace Slic3r

#endif // _libslic3r_Exception_h_
