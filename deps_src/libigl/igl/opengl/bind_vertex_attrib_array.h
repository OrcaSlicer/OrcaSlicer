#ifndef IGL_OPENGL_BIND_VERTEX_ATTRIB_ARRAY_H
#define IGL_OPENGL_BIND_VERTEX_ATTRIB_ARRAY_H
#include "gl.h"
#include "../igl_inline.h"
#include <Eigen/Core>
#include <string>
namespace igl
{
  namespace opengl
  {
    // Bind a per-vertex array attribute and refresh its contents from an Eigen
    // matrix
    //
    // Inputs:
    //   program_shader  id of shader program
    //   name  name of attribute in vertex shader
    //   bufferID  id of buffer to bind to
    //   M  #V by dim matrix of per-vertex data
    //   refresh  whether to actually call glBufferData or just bind the buffer
    // Returns id of named attribute in shader
    IGL_INLINE GLint bind_vertex_attrib_array(
      const GLuint program_shader,
      const std::string &name, 
      GLuint bufferID, 
      const Eigen::Matrix<float,Eigen::Dynamic,Eigen::Dynamic,Eigen::RowMajor> &M, 
      bool refresh);
  }
}
#ifndef IGL_STATIC_LIBRARY
#include "bind_vertex_attrib_array.cpp"
#endif
#endif
