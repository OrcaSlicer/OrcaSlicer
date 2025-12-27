// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "Viewer.h"

#include <chrono>
#include <thread>

#include <Eigen/LU>

#include "../gl.h"
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <limits>
#include <cassert>

#include <igl/project.h>
#include <igl/get_seconds.h>
#include <igl/readOBJ.h>
#include <igl/readOFF.h>
#include <igl/adjacency_list.h>
#include <igl/writeOBJ.h>
#include <igl/writeOFF.h>
#include <igl/massmatrix.h>
#include <igl/file_dialog_open.h>
#include <igl/file_dialog_save.h>
#include <igl/quat_mult.h>
#include <igl/axis_angle_to_quat.h>
#include <igl/trackball.h>
#include <igl/two_axis_valuator_fixed_up.h>
#include <igl/snap_to_canonical_view_quat.h>
#include <igl/unproject.h>
#include <igl/serialize.h>

// Internal global variables used for glfw event handling
static igl::opengl::glfw::Viewer * __viewer;
static double highdpi = 1;
static double scroll_x = 0;
static double scroll_y = 0;

static void glfw_mouse_press(GLFWwindow* window, int button, int action, int modifier)
{

  igl::opengl::glfw::Viewer::MouseButton mb;

  if (button == GLFW_MOUSE_BUTTON_1)
    mb = igl::opengl::glfw::Viewer::MouseButton::Left;
  else if (button == GLFW_MOUSE_BUTTON_2)
    mb = igl::opengl::glfw::Viewer::MouseButton::Right;
  else //if (button == GLFW_MOUSE_BUTTON_3)
    mb = igl::opengl::glfw::Viewer::MouseButton::Middle;

  if (action == GLFW_PRESS)
    __viewer->mouse_down(mb,modifier);
  else
    __viewer->mouse_up(mb,modifier);
}

static void glfw_error_callback(int error, const char* description)
{
  fputs(description, stderr);
}

static void glfw_char_mods_callback(GLFWwindow* window, unsigned int codepoint, int modifier)
{
  __viewer->key_pressed(codepoint, modifier);
}

static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int modifier)
{
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    glfwSetWindowShouldClose(window, GL_TRUE);

  if (action == GLFW_PRESS)
    __viewer->key_down(key, modifier);
  else if(action == GLFW_RELEASE)
    __viewer->key_up(key, modifier);
}

static void glfw_window_size(GLFWwindow* window, int width, int height)
{
  int w = width*highdpi;
  int h = height*highdpi;

  __viewer->post_resize(w, h);

}

static void glfw_mouse_move(GLFWwindow* window, double x, double y)
{
  __viewer->mouse_move(x*highdpi, y*highdpi);
}

static void glfw_mouse_scroll(GLFWwindow* window, double x, double y)
{
  using namespace std;
  scroll_x += x;
  scroll_y += y;

  __viewer->mouse_scroll(y);
}

static void glfw_drop_callback(GLFWwindow *window,int count,const char **filenames)
{
}

namespace igl
{
namespace opengl
{
namespace glfw
{

  IGL_INLINE int Viewer::launch(bool resizable,bool fullscreen)
  {
    // TODO return values are being ignored...
    launch_init(resizable,fullscreen);
    launch_rendering(true);
    launch_shut();
    return EXIT_SUCCESS;
  }

  IGL_INLINE int  Viewer::launch_init(bool resizable,bool fullscreen)
  {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
      return EXIT_FAILURE;
    }
    glfwWindowHint(GLFW_SAMPLES, 8);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    #ifdef __APPLE__
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
      glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
    if(fullscreen)
    {
      GLFWmonitor *monitor = glfwGetPrimaryMonitor();
      const GLFWvidmode *mode = glfwGetVideoMode(monitor);
      window = glfwCreateWindow(mode->width,mode->height,"libigl viewer",monitor,nullptr);
    }
    else
    {
      if (core.viewport.tail<2>().any()) {
        window = glfwCreateWindow(core.viewport(2),core.viewport(3),"libigl viewer",nullptr,nullptr);
      } else {
        window = glfwCreateWindow(1280,800,"libigl viewer",nullptr,nullptr);
      }
    }
    if (!window)
    {
      glfwTerminate();
      return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    // Load OpenGL and its extensions
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress))
    {
      printf("Failed to load OpenGL and its extensions\n");
      return(-1);
    }
    #if defined(DEBUG) || defined(_DEBUG)
      printf("OpenGL Version %d.%d loaded\n", GLVersion.major, GLVersion.minor);
      int major, minor, rev;
      major = glfwGetWindowAttrib(window, GLFW_CONTEXT_VERSION_MAJOR);
      minor = glfwGetWindowAttrib(window, GLFW_CONTEXT_VERSION_MINOR);
      rev = glfwGetWindowAttrib(window, GLFW_CONTEXT_REVISION);
      printf("OpenGL version received: %d.%d.%d\n", major, minor, rev);
      printf("Supported OpenGL is %s\n", (const char*)glGetString(GL_VERSION));
      printf("Supported GLSL is %s\n", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    #endif
    glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
    // Initialize FormScreen
    __viewer = this;
    // Register callbacks
    glfwSetKeyCallback(window, glfw_key_callback);
    glfwSetCursorPosCallback(window,glfw_mouse_move);
    glfwSetWindowSizeCallback(window,glfw_window_size);
    glfwSetMouseButtonCallback(window,glfw_mouse_press);
    glfwSetScrollCallback(window,glfw_mouse_scroll);
    glfwSetCharModsCallback(window,glfw_char_mods_callback);
    glfwSetDropCallback(window,glfw_drop_callback);
    // Handle retina displays (windows and mac)
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    int width_window, height_window;
    glfwGetWindowSize(window, &width_window, &height_window);
    highdpi = width/width_window;
    glfw_window_size(window,width_window,height_window);
    //opengl.init();
    core.align_camera_center(data().V,data().F);
    // Initialize IGL viewer
    init();
    return EXIT_SUCCESS;
  }



  IGL_INLINE bool Viewer::launch_rendering(bool loop)
  {
    // glfwMakeContextCurrent(window);
    // Rendering loop
    const int num_extra_frames = 5;
    int frame_counter = 0;
    while (!glfwWindowShouldClose(window))
    {
      double tic = get_seconds();
      draw();
      glfwSwapBuffers(window);
      if(core.is_animating || frame_counter++ < num_extra_frames)
      {
        glfwPollEvents();
        // In microseconds
        double duration = 1000000.*(get_seconds()-tic);
        const double min_duration = 1000000./core.animation_max_fps;
        if(duration<min_duration)
        {
          std::this_thread::sleep_for(std::chrono::microseconds((int)(min_duration-duration)));
        }
      }
      else
      {
        glfwWaitEvents();
        frame_counter = 0;
      }
      if (!loop)
        return !glfwWindowShouldClose(window);
    }
    return EXIT_SUCCESS;
  }

  IGL_INLINE void Viewer::launch_shut()
  {
    for(auto & data : data_list)
    {
      data.meshgl.free();
    }
    core.shut();
    shutdown_plugins();
    glfwDestroyWindow(window);
    glfwTerminate();
    return;
  }

  IGL_INLINE void Viewer::init()
  {
    core.init();

    if (callback_init)
      if (callback_init(*this))
        return;

    init_plugins();
  }

  IGL_INLINE void Viewer::init_plugins()
  {
    // Init all plugins
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      plugins[i]->init(this);
    }
  }

  IGL_INLINE void Viewer::shutdown_plugins()
  {
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      plugins[i]->shutdown();
    }
  }

  IGL_INLINE Viewer::Viewer():
    data_list(1),
    selected_data_index(0),
    next_data_id(1)
  {
    window = nullptr;
    data_list.front().id = 0;

    // Temporary variables initialization
    down = false;
    hack_never_moved = true;
    scroll_position = 0.0f;

    // Per face
    data().set_face_based(false);

    // C-style callbacks
    callback_init         = nullptr;
    callback_pre_draw     = nullptr;
    callback_post_draw    = nullptr;
    callback_mouse_down   = nullptr;
    callback_mouse_up     = nullptr;
    callback_mouse_move   = nullptr;
    callback_mouse_scroll = nullptr;
    callback_key_down     = nullptr;
    callback_key_up       = nullptr;

    callback_init_data          = nullptr;
    callback_pre_draw_data      = nullptr;
    callback_post_draw_data     = nullptr;
    callback_mouse_down_data    = nullptr;
    callback_mouse_up_data      = nullptr;
    callback_mouse_move_data    = nullptr;
    callback_mouse_scroll_data  = nullptr;
    callback_key_down_data      = nullptr;
    callback_key_up_data        = nullptr;

#ifndef IGL_VIEWER_VIEWER_QUIET
    const std::string usage(R"(igl::opengl::glfw::Viewer usage:
  [drag]  Rotate scene
  A,a     Toggle animation (tight draw loop)
  F,f     Toggle face based
  I,i     Toggle invert normals
  L,l     Toggle wireframe
  O,o     Toggle orthographic/perspective projection
  T,t     Toggle filled faces
  Z       Snap to canonical view
  [,]     Toggle between rotation control types (trackball, two-axis
          valuator with fixed up, 2D mode with no rotation))
  <,>     Toggle between models
  ;       Toggle vertex labels
  :       Toggle face labels)"
);
    std::cout<<usage<<std::endl;
#endif
  }

  IGL_INLINE Viewer::~Viewer()
  {
  }

  IGL_INLINE bool Viewer::load_mesh_from_file(
      const std::string & mesh_file_name_string)
  {

    // first try to load it with a plugin
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      if (plugins[i]->load(mesh_file_name_string))
      {
        return true;
      }
    }

    // Create new data slot and set to selected
    if(!(data().F.rows() == 0  && data().V.rows() == 0))
    {
      append_mesh();
    }
    data().clear();

    size_t last_dot = mesh_file_name_string.rfind('.');
    if (last_dot == std::string::npos)
    {
      std::cerr<<"Error: No file extension found in "<<
        mesh_file_name_string<<std::endl;
      return false;
    }

    std::string extension = mesh_file_name_string.substr(last_dot+1);

    if (extension == "off" || extension =="OFF")
    {
      Eigen::MatrixXd V;
      Eigen::MatrixXi F;
      if (!igl::readOFF(mesh_file_name_string, V, F))
        return false;
      data().set_mesh(V,F);
    }
    else if (extension == "obj" || extension =="OBJ")
    {
      Eigen::MatrixXd corner_normals;
      Eigen::MatrixXi fNormIndices;

      Eigen::MatrixXd UV_V;
      Eigen::MatrixXi UV_F;
      Eigen::MatrixXd V;
      Eigen::MatrixXi F;

      if (!(
            igl::readOBJ(
              mesh_file_name_string,
              V, UV_V, corner_normals, F, UV_F, fNormIndices)))
      {
        return false;
      }

      data().set_mesh(V,F);
      data().set_uv(UV_V,UV_F);

    }
    else
    {
      // unrecognized file type
      printf("Error: %s is not a recognized file type.\n",extension.c_str());
      return false;
    }

    data().compute_normals();
    data().uniform_colors(Eigen::Vector3d(51.0/255.0,43.0/255.0,33.3/255.0),
                   Eigen::Vector3d(255.0/255.0,228.0/255.0,58.0/255.0),
                   Eigen::Vector3d(255.0/255.0,235.0/255.0,80.0/255.0));

    // Alec: why?
    if (data().V_uv.rows() == 0)
    {
      data().grid_texture();
    }

    core.align_camera_center(data().V,data().F);

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->post_load())
        return true;

    return true;
  }

  IGL_INLINE bool Viewer::save_mesh_to_file(
      const std::string & mesh_file_name_string)
  {
    // first try to load it with a plugin
    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->save(mesh_file_name_string))
        return true;

    size_t last_dot = mesh_file_name_string.rfind('.');
    if (last_dot == std::string::npos)
    {
      // No file type determined
      std::cerr<<"Error: No file extension found in "<<
        mesh_file_name_string<<std::endl;
      return false;
    }
    std::string extension = mesh_file_name_string.substr(last_dot+1);
    if (extension == "off" || extension =="OFF")
    {
      return igl::writeOFF(
        mesh_file_name_string,data().V,data().F);
    }
    else if (extension == "obj" || extension =="OBJ")
    {
      Eigen::MatrixXd corner_normals;
      Eigen::MatrixXi fNormIndices;

      Eigen::MatrixXd UV_V;
      Eigen::MatrixXi UV_F;

      return igl::writeOBJ(mesh_file_name_string,
          data().V,
          data().F,
          corner_normals, fNormIndices, UV_V, UV_F);
    }
    else
    {
      // unrecognized file type
      printf("Error: %s is not a recognized file type.\n",extension.c_str());
      return false;
    }
    return true;
  }

  IGL_INLINE bool Viewer::key_pressed(unsigned int unicode_key,int modifiers)
  {
    if (callback_key_pressed)
      if (callback_key_pressed(*this,unicode_key,modifiers))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      if (plugins[i]->key_pressed(unicode_key, modifiers))
      {
        return true;
      }
    }

    switch(unicode_key)
    {
      case 'A':
      case 'a':
      {
        core.is_animating = !core.is_animating;
        return true;
      }
      case 'F':
      case 'f':
      {
        data().set_face_based(!data().face_based);
        return true;
      }
      case 'I':
      case 'i':
      {
        data().dirty |= MeshGL::DIRTY_NORMAL;
        data().invert_normals = !data().invert_normals;
        return true;
      }
      case 'L':
      case 'l':
      {
        data().show_lines = !data().show_lines;
        return true;
      }
      case 'O':
      case 'o':
      {
        core.orthographic = !core.orthographic;
        return true;
      }
      case 'T':
      case 't':
      {
        data().show_faces = !data().show_faces;
        return true;
      }
      case 'Z':
      {
        snap_to_canonical_quaternion();
        return true;
      }
      case '[':
      case ']':
      {
        if(core.rotation_type == ViewerCore::ROTATION_TYPE_TRACKBALL)
          core.set_rotation_type(ViewerCore::ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP);
        else
          core.set_rotation_type(ViewerCore::ROTATION_TYPE_TRACKBALL);

        return true;
      }
      case '<':
      case '>':
      {
        selected_data_index =
          (selected_data_index + data_list.size() + (unicode_key=='>'?1:-1))%data_list.size();
        return true;
      }
      case ';':
        data().show_vertid = !data().show_vertid;
        return true;
      case ':':
        data().show_faceid = !data().show_faceid;
        return true;
      default: break;//do nothing
    }
    return false;
  }

  IGL_INLINE bool Viewer::key_down(int key,int modifiers)
  {
    if (callback_key_down)
      if (callback_key_down(*this,key,modifiers))
        return true;
    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->key_down(key, modifiers))
        return true;
    return false;
  }

  IGL_INLINE bool Viewer::key_up(int key,int modifiers)
  {
    if (callback_key_up)
      if (callback_key_up(*this,key,modifiers))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->key_up(key, modifiers))
        return true;

    return false;
  }

  IGL_INLINE bool Viewer::mouse_down(MouseButton button,int modifier)
  {
    // Remember mouse location at down even if used by callback/plugin
    down_mouse_x = current_mouse_x;
    down_mouse_y = current_mouse_y;

    if (callback_mouse_down)
      if (callback_mouse_down(*this,static_cast<int>(button),modifier))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if(plugins[i]->mouse_down(static_cast<int>(button),modifier))
        return true;

    down = true;

    down_translation = core.camera_translation;


    // Initialization code for the trackball
    Eigen::RowVector3d center;
    if (data().V.rows() == 0)
    {
      center << 0,0,0;
    }else
    {
      center = data().V.colwise().sum()/data().V.rows();
    }

    Eigen::Vector3f coord =
      igl::project(
        Eigen::Vector3f(center(0),center(1),center(2)),
        core.view,
        core.proj,
        core.viewport);
    down_mouse_z = coord[2];
    down_rotation = core.trackball_angle;

    mouse_mode = MouseMode::Rotation;

    switch (button)
    {
      case MouseButton::Left:
        if (core.rotation_type == ViewerCore::ROTATION_TYPE_NO_ROTATION) {
          mouse_mode = MouseMode::Translation;
        } else {
          mouse_mode = MouseMode::Rotation;
        }
        break;

      case MouseButton::Right:
        mouse_mode = MouseMode::Translation;
        break;

      default:
        mouse_mode = MouseMode::None;
        break;
    }

    return true;
  }

  IGL_INLINE bool Viewer::mouse_up(MouseButton button,int modifier)
  {
    down = false;

    if (callback_mouse_up)
      if (callback_mouse_up(*this,static_cast<int>(button),modifier))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if(plugins[i]->mouse_up(static_cast<int>(button),modifier))
          return true;

    mouse_mode = MouseMode::None;

    return true;
  }

  IGL_INLINE bool Viewer::mouse_move(int mouse_x,int mouse_y)
  {
    if(hack_never_moved)
    {
      down_mouse_x = mouse_x;
      down_mouse_y = mouse_y;
      hack_never_moved = false;
    }
    current_mouse_x = mouse_x;
    current_mouse_y = mouse_y;

    if (callback_mouse_move)
      if (callback_mouse_move(*this,mouse_x,mouse_y))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->mouse_move(mouse_x, mouse_y))
        return true;

    if (down)
    {
      switch (mouse_mode)
      {
        case MouseMode::Rotation:
        {
          switch(core.rotation_type)
          {
            default:
              assert(false && "Unknown rotation type");
            case ViewerCore::ROTATION_TYPE_NO_ROTATION:
              break;
            case ViewerCore::ROTATION_TYPE_TRACKBALL:
              igl::trackball(
                core.viewport(2),
                core.viewport(3),
                2.0f,
                down_rotation,
                down_mouse_x,
                down_mouse_y,
                mouse_x,
                mouse_y,
                core.trackball_angle);
              break;
            case ViewerCore::ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP:
              igl::two_axis_valuator_fixed_up(
                core.viewport(2),core.viewport(3),
                2.0,
                down_rotation,
                down_mouse_x, down_mouse_y, mouse_x, mouse_y,
                core.trackball_angle);
              break;
          }
          //Eigen::Vector4f snapq = core.trackball_angle;

          break;
        }

        case MouseMode::Translation:
        {
          //translation
          Eigen::Vector3f pos1 = igl::unproject(Eigen::Vector3f(mouse_x, core.viewport[3] - mouse_y, down_mouse_z), core.view, core.proj, core.viewport);
          Eigen::Vector3f pos0 = igl::unproject(Eigen::Vector3f(down_mouse_x, core.viewport[3] - down_mouse_y, down_mouse_z), core.view, core.proj, core.viewport);

          Eigen::Vector3f diff = pos1 - pos0;
          core.camera_translation = down_translation + Eigen::Vector3f(diff[0],diff[1],diff[2]);

          break;
        }
        case MouseMode::Zoom:
        {
          float delta = 0.001f * (mouse_x - down_mouse_x + mouse_y - down_mouse_y);
          core.camera_zoom *= 1 + delta;
          down_mouse_x = mouse_x;
          down_mouse_y = mouse_y;
          break;
        }

        default:
          break;
      }
    }
    return true;
  }

  IGL_INLINE bool Viewer::mouse_scroll(float delta_y)
  {
    scroll_position += delta_y;

    if (callback_mouse_scroll)
      if (callback_mouse_scroll(*this,delta_y))
        return true;

    for (unsigned int i = 0; i<plugins.size(); ++i)
      if (plugins[i]->mouse_scroll(delta_y))
        return true;

    // Only zoom if there's actually a change
    if(delta_y != 0)
    {
      float mult = (1.0+((delta_y>0)?1.:-1.)*0.05);
      const float min_zoom = 0.1f;
      core.camera_zoom = (core.camera_zoom * mult > min_zoom ? core.camera_zoom * mult : min_zoom);
    }
    return true;
  }

  IGL_INLINE bool Viewer::load_scene()
  {
    std::string fname = igl::file_dialog_open();
    if(fname.length() == 0)
      return false;
    return load_scene(fname);
  }

  IGL_INLINE bool Viewer::load_scene(std::string fname)
  {
    igl::deserialize(core,"Core",fname.c_str());
    igl::deserialize(data(),"Data",fname.c_str());
    return true;
  }

  IGL_INLINE bool Viewer::save_scene()
  {
    std::string fname = igl::file_dialog_save();
    if (fname.length() == 0)
      return false;
    return save_scene(fname);
  }

  IGL_INLINE bool Viewer::save_scene(std::string fname)
  {
    igl::serialize(core,"Core",fname.c_str(),true);
    igl::serialize(data(),"Data",fname.c_str());

    return true;
  }

  IGL_INLINE void Viewer::draw()
  {
    using namespace std;
    using namespace Eigen;

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    int width_window, height_window;
    glfwGetWindowSize(window, &width_window, &height_window);

    auto highdpi_tmp = width/width_window;

    if(fabs(highdpi_tmp-highdpi)>1e-8)
    {
      post_resize(width, height);
      highdpi=highdpi_tmp;
    }

    core.clear_framebuffers();
    if (callback_pre_draw)
    {
      if (callback_pre_draw(*this))
      {
        return;
      }
    }
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      if (plugins[i]->pre_draw())
      {
        return;
      }
    }
    for(int i = 0;i<data_list.size();i++)
    {
      core.draw(data_list[i]);
    }
    if (callback_post_draw)
    {
      if (callback_post_draw(*this))
      {
        return;
      }
    }
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      if (plugins[i]->post_draw())
      {
        break;
      }
    }
  }

  IGL_INLINE void Viewer::resize(int w,int h)
  {
    if (window) {
      glfwSetWindowSize(window, w/highdpi, h/highdpi);
    }
    post_resize(w, h);
  }

  IGL_INLINE void Viewer::post_resize(int w,int h)
  {
    core.viewport = Eigen::Vector4f(0,0,w,h);
    for (unsigned int i = 0; i<plugins.size(); ++i)
    {
      plugins[i]->post_resize(w, h);
    }
  }

  IGL_INLINE void Viewer::snap_to_canonical_quaternion()
  {
    Eigen::Quaternionf snapq = this->core.trackball_angle;
    igl::snap_to_canonical_view_quat(snapq,1.0f,this->core.trackball_angle);
  }

  IGL_INLINE void Viewer::open_dialog_load_mesh()
  {
    std::string fname = igl::file_dialog_open();

    if (fname.length() == 0)
      return;

    this->load_mesh_from_file(fname.c_str());
  }

  IGL_INLINE void Viewer::open_dialog_save_mesh()
  {
    std::string fname = igl::file_dialog_save();

    if(fname.length() == 0)
      return;

    this->save_mesh_to_file(fname.c_str());
  }

  IGL_INLINE ViewerData& Viewer::data()
  {
    assert(!data_list.empty() && "data_list should never be empty");
    assert(
      (selected_data_index >= 0 && selected_data_index < data_list.size()) &&
      "selected_data_index should be in bounds");
    return data_list[selected_data_index];
  }

  IGL_INLINE int Viewer::append_mesh()
  {
    assert(data_list.size() >= 1);

    data_list.emplace_back();
    selected_data_index = data_list.size()-1;
    data_list.back().id = next_data_id++;
    return data_list.back().id;
  }

  IGL_INLINE bool Viewer::erase_mesh(const size_t index)
  {
    assert((index >= 0 && index < data_list.size()) && "index should be in bounds");
    assert(data_list.size() >= 1);
    if(data_list.size() == 1)
    {
      // Cannot remove last mesh
      return false;
    }
    data_list[index].meshgl.free();
    data_list.erase(data_list.begin() + index);
    if(selected_data_index >= index && selected_data_index>0)
    {
      selected_data_index--;
    }
    return true;
  }

  IGL_INLINE size_t Viewer::mesh_index(const int id) const {
    for (size_t i = 0; i < data_list.size(); ++i)
    {
      if (data_list[i].id == id)
        return i;
    }
    return 0;
  }


} // end namespace
} // end namespace
}
