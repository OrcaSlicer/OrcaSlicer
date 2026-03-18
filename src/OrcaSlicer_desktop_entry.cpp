#include "OrcaSlicer_bootstrap.hpp"

#include <new>
#include <string>
#include <vector>

#include <boost/nowide/convert.hpp>

#if defined(_MSC_VER) || defined(__MINGW32__)
#include "dev-utils/BaseException.h"

extern "C" {
__declspec(dllexport) int __stdcall orcaslicer_main(int argc, wchar_t **argv)
{
    // Convert wchar_t arguments to UTF8.
    std::vector<std::string> argv_narrow;
    std::vector<char *> argv_ptrs(argc + 1, nullptr);
    for (int i = 0; i < argc; ++i)
        argv_narrow.emplace_back(boost::nowide::narrow(argv[i]));
    for (int i = 0; i < argc; ++i)
        argv_ptrs[i] = argv_narrow[i].data();

// BBS: register default exception handler
#if BBL_RELEASE_TO_PUBLIC
    SET_DEFULTER_HANDLER();
#else
    // AddVectoredExceptionHandler(1, CBaseException::UnhandledExceptionFilter);
    SET_DEFULTER_HANDLER();
#endif
    std::set_new_handler([]() {
        int *a = nullptr;
        *a     = 0;
    });

    return run_orcaslicer_bootstrap(argc, argv_ptrs.data());
}
}
#else /* _MSC_VER */
int main(int argc, char **argv)
{
    return run_orcaslicer_bootstrap(argc, argv);
}
#endif /* _MSC_VER */
