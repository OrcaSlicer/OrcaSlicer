#include "background_window.h"

#include <iostream>

IGL_INLINE bool igl::opengl::glfw::background_window(GLFWwindow* & window)
{
  if(!glfwInit()) return false;
  glfwSetErrorCallback([](int id,const char* m){std::cerr<<m<<std::endl;});
  glfwWindowHint(GLFW_SAMPLES, 4);
  // Use 3.2 core profile
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  // Use background window
  glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
  window = glfwCreateWindow(1, 1,"", NULL, NULL);
  if(!window) return false;
  glfwMakeContextCurrent(window);
  if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress))
  {
    printf("Failed to load OpenGL and its extensions");
  }
  glGetError(); // pull and safely ignore unhandled errors like GL_INVALID_ENUM
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
