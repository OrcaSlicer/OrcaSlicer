#ifndef IGL_OPENGL_GLFW_MAP_TEXTURE_H
#define IGL_OPENGL_GLFW_MAP_TEXTURE_H

#ifdef IGL_OPENGL_4

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace opengl
  {
    namespace glfw
    {
      // Given a mesh (V,F) in [0,1]² and new positions (U) and a texture image
      // (in_data), _render_ a new image (out_data) of the same size.
      // Inputs:
      //   V  #V by 2 list of undeformed mesh vertex positions (matching texture)
      //   F  #F by 3 list of mesh triangle indices into V
      //   U  #U by 2 list of deformed vertex positions
      //   in_data  w*h*nc array of color values, channels, then columns, then
      //     rows (e.g., what stbi_image returns and expects)
      //   w  width
      //   h  height
      //   nc  number of channels
      // Outputs:
      //   out_data  h*w*nc list of output colors in same order as input
      //
      template <typename DerivedV, typename DerivedF, typename DerivedU>
      IGL_INLINE bool map_texture(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const Eigen::MatrixBase<DerivedU> & U,
        const unsigned char * in_data,
        const int w,
        const int h,
        const int nc,
        std::vector<unsigned char> & out_data);
      template <typename DerivedV, typename DerivedF, typename DerivedU>
      IGL_INLINE bool map_texture(
        const Eigen::MatrixBase<DerivedV> & _V,
        const Eigen::MatrixBase<DerivedF> & _F,
        const Eigen::MatrixBase<DerivedU> & _U,
        const unsigned char * in_data,
        const int w,
        const int h,
        const int nc,
        std::vector<unsigned char> & out_data,
        int & out_w,
        int & out_h,
        int & out_nc);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "map_texture.cpp"
#endif

#endif // IGL_OPENGL_4

#endif
