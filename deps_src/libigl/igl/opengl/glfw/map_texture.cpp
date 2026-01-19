#ifdef IGL_OPENGL_4

#include "map_texture.h"
#include "background_window.h"
#include "../create_shader_program.h"

#include "../gl.h"
#include <GLFW/glfw3.h>

#include <iostream>
#include <string>

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE bool igl::opengl::glfw::map_texture(
  const Eigen::MatrixBase<DerivedV> & _V,
  const Eigen::MatrixBase<DerivedF> & _F,
  const Eigen::MatrixBase<DerivedU> & _U,
  const unsigned char * in_data,
  const int w,
  const int h,
  const int nc,
  std::vector<unsigned char> & out_data)
{
  int out_w = w;
  int out_h = h;
  int out_nc = nc;
  return map_texture(_V,_F,_U,in_data,w,h,nc,out_data,out_w,out_h,out_nc);
}


template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE bool igl::opengl::glfw::map_texture(
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
  int & out_nc)
{
  const auto fail = [](const std::string msg)
  {
    std::cerr<<msg<<std::endl;
    glfwTerminate();
    return false;
  };
  // Force inputs to be RowMajor at the cost of a copy
  Eigen::Matrix<
    double,
    DerivedV::RowsAtCompileTime,
    DerivedV::ColsAtCompileTime,
    Eigen::RowMajor> V = _V.template cast<double>();
  Eigen::Matrix<
    double,
    DerivedU::RowsAtCompileTime,
    DerivedU::ColsAtCompileTime,
    Eigen::RowMajor> U = _U.template cast<double>();
  Eigen::Matrix<
    int,
    DerivedF::RowsAtCompileTime,
    DerivedF::ColsAtCompileTime,
    Eigen::RowMajor> F = _F.template cast<int>();
  const int dim = U.cols();
  GLFWwindow * window;
  if(!background_window(window))
  {
    fail("Could not initialize glfw window");
  }

  // Compile each shader
  std::string vertex_shader = dim == 2 ?
    R"(
#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 tex_coord_v;
out vec2 tex_coord_f;
void main()
{
  tex_coord_f = vec2(tex_coord_v.x,1.-tex_coord_v.y);
  gl_Position = vec4( 2.*position.x-1., 2.*(1.-position.y)-1., 0.,1.);
}
)"
    :
    R"(
#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 tex_coord_v;
out vec2 tex_coord_f;
void main()
{
  tex_coord_f = vec2(tex_coord_v.x,1.-tex_coord_v.y);
  gl_Position = vec4( 2.*position.x-1., 2.*(1.-position.y)-1., position.z,1.);
}
)"
    ;
  std::string fragment_shader = R"(
#version 330 core
layout(location = 0) out vec3 color;
uniform sampler2D tex;
in vec2 tex_coord_f;
void main()
{
  color = texture(tex,tex_coord_f).rgb;
}
)";
  GLuint prog_id =
    igl::opengl::create_shader_program(vertex_shader,fragment_shader,{});
  glUniform1i(glGetUniformLocation(prog_id, "tex"),0);
  // Generate and attach buffers to vertex array
  glDisable(GL_CULL_FACE);
  GLuint VAO = 0;
  glGenVertexArrays(1,&VAO);
  glBindVertexArray(VAO);
  GLuint ibo,vbo,tbo;
  glGenBuffers(1,&ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint)*F.size(), F.data(), GL_STATIC_DRAW);
  glGenBuffers(1,&vbo);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER,vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(double)*U.size(), U.data(), GL_STATIC_DRAW);
  glVertexAttribLPointer(0, U.cols(), GL_DOUBLE, U.cols() * sizeof(GLdouble), (GLvoid*)0);
  glGenBuffers(1,&tbo);
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER,tbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(double)*V.size(), V.data(), GL_STATIC_DRAW);
  glVertexAttribLPointer(1, V.cols(), GL_DOUBLE, V.cols() * sizeof(GLdouble), (GLvoid*)0);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  // Prepare texture
  GLuint in_tex;
  GLenum format;
  {
    format = nc==1 ? GL_RED : (nc==3 ? GL_RGB : (nc == 4 ? GL_RGBA : GL_FALSE));
    glGenTextures(1, &in_tex);
    glBindTexture(GL_TEXTURE_2D, in_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0,GL_RGB, w, h, 0,format, GL_UNSIGNED_BYTE, in_data);
  }
  // Prepare framebuffer
  GLuint fb = 0;
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  GLuint out_tex;
  glGenTextures(1, &out_tex);
  glBindTexture(GL_TEXTURE_2D, out_tex);
  // always use float for internal storage
  assert(out_nc == 3);
  glTexImage2D(GL_TEXTURE_2D, 0,GL_RGB, out_w, out_h, 0,GL_RGB, GL_FLOAT, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, out_tex, 0);
  {
    GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
  }
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    fail("framebuffer setup failed.");
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  // clear screen and set viewport
  glClearColor(0.0,1.0,0.0,0.);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(0,0,out_w,out_h);
  // Attach shader program
  glUseProgram(prog_id);
  glActiveTexture(GL_TEXTURE0 + 0);
  glBindTexture(GL_TEXTURE_2D, in_tex);
  // Draw mesh as wireframe
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glBindVertexArray(VAO);
  glDrawElements(GL_TRIANGLES, F.size(), GL_UNSIGNED_INT, 0);
  glBindVertexArray(0);
  // Write into memory
  assert(out_nc == 3);
  out_data.resize(out_nc*out_w*out_h);
  glBindTexture(GL_TEXTURE_2D, out_tex);
  glGetTexImage(GL_TEXTURE_2D, 0, format, GL_UNSIGNED_BYTE, &out_data[0]);
  // OpenGL cleanup
  glDeleteBuffers(1,&fb);
  glDeleteBuffers(1,&ibo);
  glDeleteBuffers(1,&vbo);
  glDeleteBuffers(1,&tbo);
  glDeleteTextures(1,&in_tex);
  glDeleteTextures(1,&out_tex);
  glDeleteVertexArrays(1,&VAO);
  glUseProgram(0);
  glDeleteProgram(prog_id);
  // GLFW cleanup
  glfwDestroyWindow(window);
  glfwTerminate();
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::opengl::glfw::map_texture<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, -1, 1, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, unsigned char const*, int, int, int, std::vector<unsigned char, std::allocator<unsigned char> >&);
#endif

#endif // IGL_OPENGL_4
