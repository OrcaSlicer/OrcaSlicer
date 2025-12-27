#ifndef IGL_OPENGL_GLFW_BACKGROUND_WINDOW_H
#define IGL_OPENGL_GLFW_BACKGROUND_WINDOW_H
#include "../../igl_inline.h"

#include "../gl.h"
#include <GLFW/glfw3.h>

namespace igl
{
  namespace opengl
  {
    namespace glfw
    {
      // Create a background window with a valid core profile opengl context
      // set to current.
      //
      // After you're finished with this window you may call
      // `glfwDestroyWindow(window)`
      //
      // After you're finished with glfw you should call `glfwTerminate()`
      //
      // Outputs:
      //    window  pointer to glfw window
      // Returns true iff success
      IGL_INLINE bool background_window(GLFWwindow* & window);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "background_window.cpp"
#endif

#endif
