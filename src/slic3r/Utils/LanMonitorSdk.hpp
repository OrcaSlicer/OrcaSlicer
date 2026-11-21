#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace LanMonitor {

// The SDK is process-owned. Workers never unload its singleton into another SDK user's lifetime.
[[noreturn]] inline void finish(int result)
{
    std::cout.flush();
    std::cerr.flush();
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
#endif
    std::_Exit(result);
}

class SdkLibrary
{
public:
    explicit SdkLibrary(const std::filesystem::path& path)
    {
#ifdef _WIN32
        m_module = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module)
            throw std::runtime_error("Cannot load SDK; Windows error " + std::to_string(GetLastError()));
#else
        m_module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_module)
            throw std::runtime_error("Cannot load SDK: " + std::string(dlerror()));
#endif
    }

    SdkLibrary(const SdkLibrary&) = delete;
    SdkLibrary& operator=(const SdkLibrary&) = delete;

    template<class Function> Function get(const char* name) const
    {
#ifdef _WIN32
        auto address = GetProcAddress(m_module, name);
#else
        auto address = dlsym(m_module, name);
#endif
        if (!address)
            throw std::runtime_error(std::string("Missing SDK export: ") + name);
        return reinterpret_cast<Function>(address);
    }

private:
#ifdef _WIN32
    HMODULE m_module;
#else
    void* m_module;
#endif
};

} // namespace LanMonitor
