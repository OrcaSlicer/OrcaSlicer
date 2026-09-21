#include "read_pixels.h"

template <typename T>
void igl::opengl::read_pixels(
  const GLuint width,
  const GLuint height,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & R,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & G,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & B,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & A,
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & D)
{
  R.resize(width,height);
  G.resize(width,height);
  B.resize(width,height);
  A.resize(width,height);
  D.resize(width,height);
  typedef typename std::conditional< std::is_floating_point<T>::value,GLfloat,GLubyte>::type GLType;
  GLenum type = std::is_floating_point<T>::value ?  GL_FLOAT : GL_UNSIGNED_BYTE;
  GLType* pixels = (GLType*)calloc(width*height*4,sizeof(GLType));
  GLType * depth = (GLType*)calloc(width*height*1,sizeof(GLType));
  glReadPixels(0, 0,width, height,GL_RGBA,            type, pixels);
  glReadPixels(0, 0,width, height,GL_DEPTH_COMPONENT, type, depth);
  int count = 0;
  for (unsigned j=0; j<height; ++j)
  {
    for (unsigned i=0; i<width; ++i)
    {
      R(i,j) = pixels[count*4+0];
      G(i,j) = pixels[count*4+1];
      B(i,j) = pixels[count*4+2];
      A(i,j) = pixels[count*4+3];
      D(i,j) = depth[count*1+0];
      ++count;
    }
  }
  // Clean up
  free(pixels);
  free(depth);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::opengl::read_pixels<unsigned char>(unsigned int, unsigned int, Eigen::Matrix<unsigned char, -1, -1, 0, -1, -1>&, Eigen::Matrix<unsigned char, -1, -1, 0, -1, -1>&, Eigen::Matrix<unsigned char, -1, -1, 0, -1, -1>&, Eigen::Matrix<unsigned char, -1, -1, 0, -1, -1>&, Eigen::Matrix<unsigned char, -1, -1, 0, -1, -1>&);
template void igl::opengl::read_pixels<double>(unsigned int, unsigned int, Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&);
template void igl::opengl::read_pixels<float>(unsigned int, unsigned int, Eigen::Matrix<float, -1, -1, 0, -1, -1>&, Eigen::Matrix<float, -1, -1, 0, -1, -1>&, Eigen::Matrix<float, -1, -1, 0, -1, -1>&, Eigen::Matrix<float, -1, -1, 0, -1, -1>&, Eigen::Matrix<float, -1, -1, 0, -1, -1>&);
#endif 
