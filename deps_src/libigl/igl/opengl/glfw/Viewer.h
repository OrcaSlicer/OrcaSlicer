// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLFW_VIEWER_H
#define IGL_OPENGL_GLFW_VIEWER_H

#ifndef IGL_OPENGL_4
#define IGL_OPENGL_4
#endif

#include "../../igl_inline.h"
#include "../MeshGL.h"
#include "../ViewerCore.h"
#include "../ViewerData.h"
#include "ViewerPlugin.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <vector>
#include <string>
#include <cstdint>

#define IGL_MOD_SHIFT           0x0001
#define IGL_MOD_CONTROL         0x0002
#define IGL_MOD_ALT             0x0004
#define IGL_MOD_SUPER           0x0008

struct GLFWwindow;

namespace igl
{
namespace opengl
{
namespace glfw
{
  // GLFW-based mesh viewer
  class Viewer
  {
  public:
    // UI Enumerations
    enum class MouseButton {Left, Middle, Right};
    enum class MouseMode { None, Rotation, Zoom, Pan, Translation} mouse_mode;
    IGL_INLINE int launch(bool resizable = true,bool fullscreen = false);
    IGL_INLINE int launch_init(bool resizable = true,bool fullscreen = false);
    IGL_INLINE bool launch_rendering(bool loop = true);
    IGL_INLINE void launch_shut();
    IGL_INLINE void init();
    IGL_INLINE void init_plugins();
    IGL_INLINE void shutdown_plugins();
    Viewer();
    ~Viewer();
    // Mesh IO
    IGL_INLINE bool load_mesh_from_file(const std::string & mesh_file_name);
    IGL_INLINE bool   save_mesh_to_file(const std::string & mesh_file_name);
    // Callbacks
    IGL_INLINE bool key_pressed(unsigned int unicode_key,int modifier);
    IGL_INLINE bool key_down(int key,int modifier);
    IGL_INLINE bool key_up(int key,int modifier);
    IGL_INLINE bool mouse_down(MouseButton button,int modifier);
    IGL_INLINE bool mouse_up(MouseButton button,int modifier);
    IGL_INLINE bool mouse_move(int mouse_x,int mouse_y);
    IGL_INLINE bool mouse_scroll(float delta_y);
    // Scene IO
    IGL_INLINE bool load_scene();
    IGL_INLINE bool load_scene(std::string fname);
    IGL_INLINE bool save_scene();
    IGL_INLINE bool save_scene(std::string fname);
    // Draw everything
    IGL_INLINE void draw();
    // OpenGL context resize
    IGL_INLINE void resize(int w,int h); // explicitly set window size
    IGL_INLINE void post_resize(int w,int h); // external resize due to user interaction
    // Helper functions
    IGL_INLINE void snap_to_canonical_quaternion();
    IGL_INLINE void open_dialog_load_mesh();
    IGL_INLINE void open_dialog_save_mesh();
    IGL_INLINE ViewerData& data();

    // Append a new "slot" for a mesh (i.e., create empty entires at the end of
    // the data_list and opengl_state_list.
    //
    // Returns the id of the last appended mesh
    //
    // Side Effects:
    //   selected_data_index is set this newly created, last entry (i.e.,
    //   #meshes-1)
    IGL_INLINE int append_mesh();

    // Erase a mesh (i.e., its corresponding data and state entires in data_list
    // and opengl_state_list)
    //
    // Inputs:
    //   index  index of mesh to erase
    // Returns whether erasure was successful <=> cannot erase last mesh
    //
    // Side Effects:
    //   If selected_data_index is greater than or equal to index then it is
    //   decremented
    // Example:
    //   // Erase all mesh slots except first and clear remaining mesh
    //   viewer.selected_data_index = viewer.data_list.size()-1;
    //   while(viewer.erase_mesh(viewer.selected_data_index)){};
    //   viewer.data().clear();
    //
    IGL_INLINE bool erase_mesh(const size_t index);

    // Retrieve mesh index from its unique identifier
    // Returns 0 if not found
    IGL_INLINE size_t mesh_index(const int id) const;

    // Alec: I call this data_list instead of just data to avoid confusion with
    // old "data" variable.
    // Stores all the data that should be visualized
    std::vector<ViewerData> data_list;

    size_t selected_data_index;
    int next_data_id;
    GLFWwindow* window;
    // Stores all the viewing options
    ViewerCore core;
    // List of registered plugins
    std::vector<ViewerPlugin*> plugins;
    // Temporary data stored when the mouse button is pressed
    Eigen::Quaternionf down_rotation;
    int current_mouse_x;
    int current_mouse_y;
    int down_mouse_x;
    int down_mouse_y;
    float down_mouse_z;
    Eigen::Vector3f down_translation;
    bool down;
    bool hack_never_moved;
    // Keep track of the global position of the scrollwheel
    float scroll_position;
    // C++-style functions
    //
    // Returns **true** if action should be cancelled.
    std::function<bool(Viewer& viewer)> callback_init;
    std::function<bool(Viewer& viewer)> callback_pre_draw;
    std::function<bool(Viewer& viewer)> callback_post_draw;
    std::function<bool(Viewer& viewer, int button, int modifier)> callback_mouse_down;
    std::function<bool(Viewer& viewer, int button, int modifier)> callback_mouse_up;
    std::function<bool(Viewer& viewer, int mouse_x, int mouse_y)> callback_mouse_move;
    std::function<bool(Viewer& viewer, float delta_y)> callback_mouse_scroll;
    std::function<bool(Viewer& viewer, unsigned int key, int modifiers)> callback_key_pressed;
    // THESE SHOULD BE DEPRECATED:
    std::function<bool(Viewer& viewer, unsigned int key, int modifiers)> callback_key_down;
    std::function<bool(Viewer& viewer, unsigned int key, int modifiers)> callback_key_up;
    // Pointers to per-callback data
    void* callback_init_data;
    void* callback_pre_draw_data;
    void* callback_post_draw_data;
    void* callback_mouse_down_data;
    void* callback_mouse_up_data;
    void* callback_mouse_move_data;
    void* callback_mouse_scroll_data;
    void* callback_key_pressed_data;
    void* callback_key_down_data;
    void* callback_key_up_data;

  public:
      EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  };

} // end namespace
} // end namespace
} // end namespace

#ifndef IGL_STATIC_LIBRARY
#  include "Viewer.cpp"
#endif

#endif
