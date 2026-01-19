#ifndef TRY_CATCH_SIGNAL_SEH_HPP
#define TRY_CATCH_SIGNAL_SEH_HPP

#include <functional>
#include <csignal>

namespace Slic3r {

using SignalT = decltype (SIGSEGV);

namespace detail {

void try_catch_signal_seh(int sigcnt, const SignalT *sigs,
                          std::function<void()> &&fn,
                          std::function<void()> &&cfn);

}

template<class TryFn, class CatchFn, int N>
void try_catch_signal(const SignalT (&sigs)[N], TryFn &&fn, CatchFn &&cfn)
{
    detail::try_catch_signal_seh(N, sigs, fn, cfn);
}

} // Slic3r

#endif // TRY_CATCH_SIGNAL_SEH_HPP
