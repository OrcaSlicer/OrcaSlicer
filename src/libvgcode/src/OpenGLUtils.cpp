///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/

#include "OpenGLUtils.hpp"

#include <iostream>
#include <assert.h>
#include <cctype>
#include <stdio.h>
#include <cstring>

namespace libvgcode {

#ifdef HAS_GLSAFE
void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name)
{
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;
    const char* sErr = 0;
    switch (err) {
    case GL_INVALID_ENUM:      { sErr = "Invalid Enum"; break; }
    case GL_INVALID_VALUE:     { sErr = "Invalid Value"; break; }
    // be aware that GL_INVALID_OPERATION is generated if glGetError is executed between the execution of glBegin / glEnd 
    case GL_INVALID_OPERATION: { sErr = "Invalid Operation"; break; }
    case GL_OUT_OF_MEMORY:     { sErr = "Out Of Memory"; break; }
    case GL_INVALID_FRAMEBUFFER_OPERATION: { sErr = "Invalid framebuffer operation"; break; }
#if !defined(ENABLE_OPENGL_ES)
    case GL_STACK_OVERFLOW:    { sErr = "Stack Overflow"; break; }
    case GL_STACK_UNDERFLOW:   { sErr = "Stack Underflow"; break; }
#endif // ENABLE_OPENGL_ES
    default:                   { sErr = "Unknown"; break; }
    }
    std::cout << "OpenGL error in " << file_name << ":" << line << ", function " << function_name << "() : " << (int)err << " - " << sErr << "\n";
    assert(false);
}
#endif // HAS_GLSAFE

static const char* OPENGL_ES_PREFIXES[] = { "OpenGL ES-CM ", "OpenGL ES-CL ", "OpenGL ES ", nullptr };

bool OpenGLWrapper::s_valid_context = false;
#ifdef ENABLE_OPENGL_ES
int OpenGLWrapper::s_max_texture_size = 0;
#endif // ENABLE_OPENGL_ES

bool OpenGLWrapper::load_opengl(const std::string& context_version)
{
    s_valid_context = false;

    const char* version = context_version.c_str();
    for (int i = 0; OPENGL_ES_PREFIXES[i] != nullptr; ++i) {
        const size_t length = strlen(OPENGL_ES_PREFIXES[i]);
        if (strncmp(version, OPENGL_ES_PREFIXES[i], length) == 0) {
            version += length;
            break;
        }
    }

    GLint major = 0;
    GLint minor = 0;
#ifdef _MSC_VER
    const int res = sscanf_s(version, "%d.%d", &major, &minor);
#else
    const int res = sscanf(version, "%d.%d", &major, &minor);
#endif // _MSC_VER
    if (res != 2)
        return false;

#ifdef ENABLE_OPENGL_ES
    s_valid_context = major > 3 || (major == 3 && minor >= 0);
    const int glad_res = gladLoaderLoadGLES2();
#else
    s_valid_context = major > 3 || (major == 3 && minor >= 2);
    const int glad_res = gladLoaderLoadGL();
#endif // ENABLE_OPENGL_ES

    if (glad_res == 0)
        return false;

#ifdef ENABLE_OPENGL_ES
    glsafe(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s_max_texture_size));
#endif // ENABLE_OPENGL_ES

    return s_valid_context;
}

void OpenGLWrapper::unload_opengl()
{
#ifdef ENABLE_OPENGL_ES
    gladLoaderUnloadGLES2();
#else
    gladLoaderUnloadGL();
#endif // ENABLE_OPENGL_ES
}

} // namespace libvgcode
