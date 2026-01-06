///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_OPENGLUTILS_HPP
#define VGCODE_OPENGLUTILS_HPP

// OpenGL loader
#ifdef ENABLE_OPENGL_ES
#include "../glad/include/glad/gles2.h"
#else
#include "../glad/include/glad/gl.h"
#endif // ENABLE_OPENGL_ES

#include <string>

namespace libvgcode {
#ifndef NDEBUG
#define HAS_GLSAFE
#endif // NDEBUG

#ifdef HAS_GLSAFE
extern void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name);
inline void glAssertRecentCall() { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); }
#define glsafe(cmd) do { cmd; glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#define glcheck() do { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#else
inline void glAssertRecentCall() { }
#define glsafe(cmd) cmd
#define glcheck()
#endif // HAS_GLSAFE

class OpenGLWrapper
{
public:
    static bool load_opengl(const std::string& context_version);
    static void unload_opengl();
    static bool is_valid_context() { return s_valid_context; }
#ifdef ENABLE_OPENGL_ES
    static size_t max_texture_size() { return static_cast<size_t>(s_max_texture_size); }
#endif // ENABLE_OPENGL_ES

private:
    static bool s_valid_context;
#ifdef ENABLE_OPENGL_ES
    static int s_max_texture_size;
#endif // ENABLE_OPENGL_ES
};

} // namespace libvgcode

#endif // VGCODE_OPENGLUTILS_HPP
