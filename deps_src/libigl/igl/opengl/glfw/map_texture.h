#ifndef IGL_OPENGL_GLFW_MAP_TEXTURE_H
#define IGL_OPENGL_GLFW_MAP_TEXTURE_H

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace opengl
  {
    namespace glfw
    {
      /// Given a mesh (V,F) in [0,1]² and new positions (U) and a texture image
      /// (in_data), _render_ a new image (out_data) of the same size.
      /// @param[in] V  #V by 2 list of undeformed mesh vertex positions ∈ [0,1]²
      /// @param[in] F  #F by 3 list of mesh triangle indices into V
      /// @param[in] U  #U by 2 list of deformed vertex positions ∈ [0,1]²
      /// @param[in] in_data  w*h*nc array of color values, channels, then columns, then
      ///              rows (e.g., what stbi_image returns and expects)
      /// @param[in] w  width
      /// @param[in] h  height
      /// @param[in] nc  number of channels
      /// @param[out] out_data  h*w*nc list of output colors in same order as input
      /// @param[out] out_w  width of output image
      /// @param[out] out_h  height of output image
      /// @param[out] out_nc  number of channels of output image
      ///
      /// \pre Seems like w,h should be equal.
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
      /// \overload
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
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "map_texture.cpp"
#endif

#endif
