#include "Platform.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem/operations.hpp>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/machine.h>
#endif

namespace Slic3r {

static auto s_platform 		  = Platform::Uninitialized;
static auto s_platform_flavor = PlatformFlavor::Uninitialized;

void detect_platform()
{
#if defined(_WIN32)
    BOOST_LOG_TRIVIAL(info) << "Platform: Windows";
	s_platform 		  = Platform::Windows;
	s_platform_flavor = PlatformFlavor::Generic;
#elif defined(__APPLE__)
    BOOST_LOG_TRIVIAL(info) << "Platform: OSX";
    s_platform        = Platform::OSX;
    s_platform_flavor = PlatformFlavor::GenericOSX;
    {
        cpu_type_t type = 0;
        size_t     size = sizeof(type);
        if (sysctlbyname("hw.cputype", &type, &size, NULL, 0) == 0) {
            type &= ~CPU_ARCH_MASK;
            if (type == CPU_TYPE_X86) {
                int proc_translated = 0;
                size                = sizeof(proc_translated);
                // Detect if native CPU is really X86 or PrusaSlicer runs through Rosetta.
                if (sysctlbyname("sysctl.proc_translated", &proc_translated, &size, NULL, 0) == -1) {
                    if (errno == ENOENT) {
                        // Native CPU is X86, and property sysctl.proc_translated doesn't exist.
                        s_platform_flavor = PlatformFlavor::OSXOnX86;
                        BOOST_LOG_TRIVIAL(info) << "Platform flavor: OSXOnX86";
                    }
                } else if (proc_translated == 1) {
                    // Native CPU is ARM and PrusaSlicer runs through Rosetta.
                    s_platform_flavor = PlatformFlavor::OSXOnArm;
                    BOOST_LOG_TRIVIAL(info) << "Platform flavor: OSXOnArm";
                } else {
                    // Native CPU is X86.
                    s_platform_flavor = PlatformFlavor::OSXOnX86;
                    BOOST_LOG_TRIVIAL(info) << "Platform flavor: OSXOnX86";
                }
            } else if (type == CPU_TYPE_ARM) {
                // Native CPU is ARM
                s_platform_flavor = PlatformFlavor::OSXOnArm;
                BOOST_LOG_TRIVIAL(info) << "Platform flavor: OSXOnArm";
            }
        }
    }
#elif defined(__linux__)
    BOOST_LOG_TRIVIAL(info) << "Platform: Linux";
	s_platform 		  = Platform::Linux;
	s_platform_flavor = PlatformFlavor::GenericLinux;
	// Test for Chromium.
	{
		FILE *f = ::fopen("/proc/version", "rt");
		if (f) {
			char buf[4096];
			// Read the 1st line.
			if (::fgets(buf, 4096, f)) {
				if (strstr(buf, "Chromium OS") != nullptr) {
					s_platform_flavor = PlatformFlavor::LinuxOnChromium;
				    BOOST_LOG_TRIVIAL(info) << "Platform flavor: LinuxOnChromium";
				} else if (strstr(buf, "microsoft") != nullptr || strstr(buf, "Microsoft") != nullptr) {
					if (boost::filesystem::exists("/run/WSL") && getenv("WSL_INTEROP") != nullptr) {
						BOOST_LOG_TRIVIAL(info) << "Platform flavor: WSL2";
						s_platform_flavor = PlatformFlavor::WSL2;
					} else {
						BOOST_LOG_TRIVIAL(info) << "Platform flavor: WSL";
						s_platform_flavor = PlatformFlavor::WSL;
					}
				}
			}
			::fclose(f);
		}
	}
#elif defined(__OpenBSD__)
    BOOST_LOG_TRIVIAL(info) << "Platform: OpenBSD";
	s_platform 		  = Platform::BSDUnix;
	s_platform_flavor = PlatformFlavor::OpenBSD;
#else
	// This should not happen.
    BOOST_LOG_TRIVIAL(info) << "Platform: Unknown";
	static_assert(false, "Unknown platform detected");
	s_platform 		  = Platform::Unknown;
	s_platform_flavor = PlatformFlavor::Unknown;
#endif
}

Platform platform()
{
	return s_platform;
}

PlatformFlavor platform_flavor()
{
	return s_platform_flavor;
}



std::string platform_to_string(Platform platform)
{
    switch (platform) {
        case Platform::Uninitialized: return "Unitialized";
        case Platform::Unknown      : return "Unknown";
        case Platform::Windows      : return "Windows";
        case Platform::OSX          : return "OSX";
        case Platform::Linux        : return "Linux";
        case Platform::BSDUnix      : return "BSDUnix";
    }
    assert(false);
    return "";
}



std::string platform_flavor_to_string(PlatformFlavor pf)
{
    switch (pf) {
        case PlatformFlavor::Uninitialized   : return "Unitialized";
        case PlatformFlavor::Unknown         : return "Unknown";
        case PlatformFlavor::Generic         : return "Generic";
        case PlatformFlavor::GenericLinux    : return "GenericLinux";
        case PlatformFlavor::LinuxOnChromium : return "LinuxOnChromium";
        case PlatformFlavor::WSL             : return "WSL";
        case PlatformFlavor::WSL2            : return "WSL2";
        case PlatformFlavor::OpenBSD         : return "OpenBSD";
        case PlatformFlavor::GenericOSX      : return "GenericOSX";
        case PlatformFlavor::OSXOnX86        : return "OSXOnX86";
        case PlatformFlavor::OSXOnArm        : return "OSXOnArm";
    }
    assert(false);
    return "";
}


} // namespace Slic3r
