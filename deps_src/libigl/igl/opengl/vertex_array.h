#ifndef IGL_OPENGL_VERTEX_ARRAY_H
#define IGL_OPENGL_VERTEX_ARRAY_H
#include <igl/opengl/../igl_inline.h>
#include <igl/opengl/gl.h>
#include <Eigen/Core>
namespace igl
{
  namespace opengl
  {
    // Create a GL_VERTEX_ARRAY for a given mesh (V,F)
    //
    // Inputs:
    //   V  #V by dim list of mesh vertex positions
    //   F  #F by 3 list of mesh triangle indices into V
    // Outputs:
    //   va_id  id of vertex array
    //   ab_id  id of array buffer (vertex buffer object)
    //   eab_id  id of element array buffer (element/face buffer object)
    //
    template <
      typename DerivedV,
      typename DerivedF>
    IGL_INLINE void vertex_array(
      // Note: Unlike most libigl functions, the **input** Eigen matrices must
      // be `Eigen::PlainObjectBase` because we want to directly access it's
      // underlying storage. It cannot be `Eigen::MatrixBase` (see
      // http://stackoverflow.com/questions/25094948/)
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      GLuint & va_id,
      GLuint & ab_id,
      GLuint & eab_id);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "vertex_array.cpp"
#endif
#endif
