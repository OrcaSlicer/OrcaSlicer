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
    IGL_INLINE int launch     (bool fullscreen = false, const std::string &name = "libigl viewer", int width = 0, int height = 0);
    IGL_INLINE int launch_init(bool fullscreen = false, const std::string &name = "libigl viewer", int width = 0, int height = 0);
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
    // Render given ViewerCore to a buffer. The width and height are determined
    // by non-zeros dimensions of R or – if both are zero — are set to this
    // core's viewport sizes. Other buffers are resized to fit if needed.
    //
    // Template:
    //   T  image storage type, e.g., unsigned char (values ∈ [0,255]), double
    //     (values ∈ [0.0,1.0]).
    // Inputs:
    //   data  which ViewerData to draw
    //   update_matrices  whether to update view, proj, and norm matrices in
    //     shaders
    // Outputs:
    //   R  width by height red pixel color values
    //   G  width by height green pixel color values
    //   B  width by height blue pixel color values
    //   A  width by height alpha pixel color values
    //   D  width by height depth pixel values. Depth values are _not_
    //     anti-aliased like RGBA.
    //
    template <typename T>
    IGL_INLINE void draw_buffer(
      // can't be const because of writing in and out of `core.viewport`
      /*const*/ igl::opengl::ViewerCore & core, 
      Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & R,
      Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & G,
      Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & B,
      Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & A,
      Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & D);
    // OpenGL context resize
    IGL_INLINE void resize(int w,int h); // explicitly set window size
    IGL_INLINE void post_resize(int w,int h); // external resize due to user interaction
    // Helper functions
    IGL_INLINE void snap_to_canonical_quaternion();
    IGL_INLINE void open_dialog_load_mesh();
    IGL_INLINE void open_dialog_save_mesh();

    ////////////////////////
    // Multi-mesh methods //
    ////////////////////////

    // Return the current mesh, or the mesh corresponding to a given unique identifier
    //
    // Inputs:
    //   mesh_id  unique identifier associated to the desired mesh (current mesh if -1)
    IGL_INLINE ViewerData& data(int mesh_id = -1);
    IGL_INLINE const ViewerData& data(int mesh_id = -1) const;

    // Append a new "slot" for a mesh (i.e., create empty entries at the end of
    // the data_list and opengl_state_list.
    //
    // Inputs:
    //   visible  If true, the new mesh is set to be visible on all existing viewports
    // Returns the id of the last appended mesh
    //
    // Side Effects:
    //   selected_data_index is set this newly created, last entry (i.e.,
    //   #meshes-1)
    IGL_INLINE int append_mesh(bool visible = true);

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

    ////////////////////////////
    // Multi-viewport methods //
    ////////////////////////////

    // Return the current viewport, or the viewport corresponding to a given unique identifier
    //
    // Inputs:
    //   core_id  unique identifier corresponding to the desired viewport (current viewport if 0)
    IGL_INLINE ViewerCore& core(unsigned core_id = 0);
    IGL_INLINE const ViewerCore& core(unsigned core_id = 0) const;

    // Append a new "slot" for a viewport (i.e., copy properties of the current viewport, only
    // changing the viewport size/position)
    //
    // Inputs:
    //   viewport      Vector specifying the viewport origin and size in screen coordinates.
    //   append_empty  If true, existing meshes are hidden on the new viewport.
    //
    // Returns the unique id of the newly inserted viewport. There can be a maximum of 31
    //   viewports created in the same viewport. Erasing a viewport does not change the id of
    //   other existing viewports
    IGL_INLINE int append_core(Eigen::Vector4f viewport, bool append_empty = false);

    // Erase a viewport
    //
    // Inputs:
    //   index  index of the viewport to erase
    IGL_INLINE bool erase_core(const size_t index);

    // Retrieve viewport index from its unique identifier
    // Returns 0 if not found
    IGL_INLINE size_t core_index(const int id) const;

    // Change selected_core_index to the viewport containing the mouse
    // (current_mouse_x, current_mouse_y)
    IGL_INLINE void select_hovered_core();

public:
    //////////////////////
    // Member variables //
    //////////////////////

    // Alec: I call this data_list instead of just data to avoid confusion with
    // old "data" variable.
    // Stores all the data that should be visualized
    std::vector<ViewerData> data_list;

    size_t selected_data_index;
    int next_data_id;
    GLFWwindow* window;

    // Stores all the viewing options
    std::vector<ViewerCore> core_list;
    size_t selected_core_index;
    int next_core_id;

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
    std::function<bool(Viewer& viewer, int w, int h)> callback_post_resize;
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
