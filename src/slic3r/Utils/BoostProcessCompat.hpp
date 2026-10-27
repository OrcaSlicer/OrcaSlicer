#ifndef slic3r_Utils_BoostProcessCompat_hpp_
#define slic3r_Utils_BoostProcessCompat_hpp_

#include <boost/version.hpp>

// Since Boost 1.88 the classic boost/process headers are only available
// under boost/process/v1/, select those there and the originals otherwise.
#if BOOST_VERSION >= 108800
#define BOOST_PROCESS_VERSION 1
#include <boost/process/v1.hpp>
#else
#include <boost/process.hpp>
#endif

#ifdef _WIN32
#if BOOST_VERSION >= 108800
#include <boost/process/v1/windows.hpp>
#else
#include <boost/process/windows.hpp>
#endif
#endif

#endif // slic3r_Utils_BoostProcessCompat_hpp_
