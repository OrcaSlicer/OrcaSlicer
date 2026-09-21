#include "vertex_array.h"
#include "report_gl_error.h"
#include "../IGL_ASSERT.h"

template <
  typename DerivedV,
  typename DerivedF>
IGL_INLINE void igl::opengl::vertex_array(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  GLuint & va_id,
  GLuint & ab_id,
  GLuint & eab_id)
{
  // Inputs should be in RowMajor storage. If not, we have no choice but to
  // create a copy.
  if(!(V.Options & Eigen::RowMajor))
  {
    Eigen::Matrix<
      typename DerivedV::Scalar,
      DerivedV::RowsAtCompileTime,
      DerivedV::ColsAtCompileTime,
      Eigen::RowMajor> VR = V;
    return vertex_array(VR,F,va_id,ab_id,eab_id);
  }
  if(!(F.Options & Eigen::RowMajor))
  {
    Eigen::Matrix<
      typename DerivedF::Scalar,
      DerivedF::RowsAtCompileTime,
      DerivedF::ColsAtCompileTime,
      Eigen::RowMajor> FR = F;
    return vertex_array(V,FR,va_id,ab_id,eab_id);
  }
  // Generate and attach buffers to vertex array
  glGenVertexArrays(1, &va_id);
  glGenBuffers(1, &ab_id);
  glGenBuffers(1, &eab_id);
  glBindVertexArray(va_id);
  glBindBuffer(GL_ARRAY_BUFFER, ab_id);
  const auto size_VScalar = sizeof(typename DerivedV::Scalar);
  const auto size_FScalar = sizeof(typename DerivedF::Scalar);
  glBufferData(GL_ARRAY_BUFFER,size_VScalar*V.size(),V.data(),GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eab_id);
  IGL_ASSERT(sizeof(GLuint) == size_FScalar && "F type does not match GLuint");
  glBufferData(
    GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint)*F.size(), F.data(), GL_STATIC_DRAW);
  glVertexAttribPointer(
    0,
    V.cols(),
    size_VScalar==sizeof(float)?GL_FLOAT:GL_DOUBLE,
    GL_FALSE,
    V.cols()*size_VScalar,
    (GLvoid*)0);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0); 
  glBindVertexArray(0);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::opengl::vertex_array<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, unsigned int&, unsigned int&, unsigned int&);
#endif
