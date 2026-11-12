# Applied as PATCH_COMMAND for OpenEXR 2.5.5 on Windows ARM64.
#
# Root cause of the ARM64 build failure: OpenEXR/IlmImf/ImfSimd.h hard-codes
#     #if defined __SSE2__ || (_MSC_VER >= 1300 && !_M_CEE_PURE)
#         #define IMF_HAVE_SSE2 1
#     #endif
# The `_MSC_VER >= 1300` arm is true for *every* MSVC, including ARM64, so
# IMF_HAVE_SSE2 gets defined and <emmintrin.h> (an x86-only header) is pulled
# in -> error C1189. This is a pure-preprocessor decision, so no CMake cache
# variable can suppress it. Patch the header to also require an x86 target.

set(_simd "OpenEXR/IlmImf/ImfSimd.h")
if(EXISTS "${_simd}")
    file(READ "${_simd}" _content)
    set(_old "#if defined __SSE2__ || (_MSC_VER >= 1300 && !_M_CEE_PURE)")
    set(_new "#if (defined __SSE2__ || (_MSC_VER >= 1300 && !_M_CEE_PURE)) && (defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__))")
    if(_content MATCHES "_M_IX86")
        message(STATUS "[ARM64 patch] ImfSimd.h already guarded")
    else()
        string(REPLACE "${_old}" "${_new}" _patched "${_content}")
        if(_patched STREQUAL _content)
            message(FATAL_ERROR "[ARM64 patch] Failed to match SSE2 guard in ${_simd}")
        endif()
        file(WRITE "${_simd}" "${_patched}")
        message(STATUS "[ARM64 patch] Guarded IMF_HAVE_SSE2 with x86 arch check in ${_simd}")
    endif()
else()
    message(FATAL_ERROR "[ARM64 patch] Not found: ${_simd}")
endif()
