#include "TryCatchSignalSEH.hpp"

#include <windows.h>

static int signal_seh_filter(int sigcnt, const Slic3r::SignalT *sigs,
                             unsigned long seh_code)
{
    int ret = EXCEPTION_CONTINUE_SEARCH;

    for (int s = 0; s < sigcnt && ret != EXCEPTION_EXECUTE_HANDLER; ++s)
    switch (sigs[s]) {
    case SIGSEGV:
        if (seh_code == STATUS_ACCESS_VIOLATION)
            ret = EXCEPTION_EXECUTE_HANDLER;
        break;
    case SIGILL:
        if (seh_code == STATUS_ILLEGAL_INSTRUCTION)
            ret = EXCEPTION_EXECUTE_HANDLER;
        break;
    case SIGFPE:
        if (seh_code == STATUS_FLOAT_DIVIDE_BY_ZERO ||
            seh_code == STATUS_FLOAT_OVERFLOW ||
            seh_code == STATUS_FLOAT_UNDERFLOW ||
            seh_code == STATUS_INTEGER_DIVIDE_BY_ZERO)
            ret = EXCEPTION_EXECUTE_HANDLER;
        break;
    default: ret = EXCEPTION_CONTINUE_SEARCH;
    }

    return ret;
}

void Slic3r::detail::try_catch_signal_seh(int sigcnt, const SignalT *sigs,
                                          std::function<void()> &&fn,
                                          std::function<void()> &&cfn)
{
    __try {
        fn();
    }
    __except(signal_seh_filter(sigcnt, sigs, GetExceptionCode())) {
        cfn();
    }
}
