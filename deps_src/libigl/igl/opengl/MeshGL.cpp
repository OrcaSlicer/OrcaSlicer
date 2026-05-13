// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "MeshGL.h"
#include "bind_vertex_attrib_array.h"
#include "create_shader_program.h"
#include "destroy_shader_program.h"
#include "verasansmono_compressed.h"
#include <iostream>

IGL_INLINE igl::opengl::MeshGL::MeshGL():
  tex_filter(GL_LINEAR),
  tex_wrap(GL_REPEAT)
{
}

IGL_INLINE void igl::opengl::MeshGL::init_buffers()
{
  // Mesh: Vertex Array Object & Buffer objects
  glGenVertexArrays(1, &vao_mesh);
  glBindVertexArray(vao_mesh);
  glGenBuffers(1, &vbo_V);
  glGenBuffers(1, &vbo_V_normals);
  glGenBuffers(1, &vbo_V_ambient);
  glGenBuffers(1, &vbo_V_diffuse);
  glGenBuffers(1, &vbo_V_specular);
  glGenBuffers(1, &vbo_V_uv);
  glGenBuffers(1, &vbo_F);
  glGenTextures(1, &vbo_tex);
  glGenTextures(1, &font_atlas);

  // Line overlay
  glGenVertexArrays(1, &vao_overlay_lines);
  glBindVertexArray(vao_overlay_lines);
  glGenBuffers(1, &vbo_lines_F);
  glGenBuffers(1, &vbo_lines_V);
  glGenBuffers(1, &vbo_lines_V_colors);

  // Point overlay
  glGenVertexArrays(1, &vao_overlay_points);
  glBindVertexArray(vao_overlay_points);
  glGenBuffers(1, &vbo_points_F);
  glGenBuffers(1, &vbo_points_V);
  glGenBuffers(1, &vbo_points_V_colors);

  // Text Labels
  vertex_labels.init_buffers();
  face_labels.init_buffers();
  custom_labels.init_buffers();

  dirty = MeshGL::DIRTY_ALL;
}

IGL_INLINE void igl::opengl::MeshGL::free_buffers()
{
  if (is_initialized)
  {
    glDeleteVertexArrays(1, &vao_mesh);
    glDeleteVertexArrays(1, &vao_overlay_lines);
    glDeleteVertexArrays(1, &vao_overlay_points);

    glDeleteBuffers(1, &vbo_V);
    glDeleteBuffers(1, &vbo_V_normals);
    glDeleteBuffers(1, &vbo_V_ambient);
    glDeleteBuffers(1, &vbo_V_diffuse);
    glDeleteBuffers(1, &vbo_V_specular);
    glDeleteBuffers(1, &vbo_V_uv);
    glDeleteBuffers(1, &vbo_F);
    glDeleteBuffers(1, &vbo_lines_F);
    glDeleteBuffers(1, &vbo_lines_V);
    glDeleteBuffers(1, &vbo_lines_V_colors);
    glDeleteBuffers(1, &vbo_points_F);
    glDeleteBuffers(1, &vbo_points_V);
    glDeleteBuffers(1, &vbo_points_V_colors);

    // Text Labels
    vertex_labels.free_buffers();
    face_labels.free_buffers();
    custom_labels.free_buffers();

    glDeleteTextures(1, &vbo_tex);
    glDeleteTextures(1, &font_atlas);
  }
}

IGL_INLINE void igl::opengl::MeshGL::TextGL::init_buffers()
{
  glGenVertexArrays(1, &vao_labels);
  glBindVertexArray(vao_labels);
  glGenBuffers(1, &vbo_labels_pos);
  glGenBuffers(1, &vbo_labels_characters);
  glGenBuffers(1, &vbo_labels_offset);
  glGenBuffers(1, &vbo_labels_indices);
}

IGL_INLINE void igl::opengl::MeshGL::TextGL::free_buffers()
{
  glDeleteBuffers(1, &vbo_labels_pos);
  glDeleteBuffers(1, &vbo_labels_characters);
  glDeleteBuffers(1, &vbo_labels_offset);
  glDeleteBuffers(1, &vbo_labels_indices);
}

IGL_INLINE void igl::opengl::MeshGL::bind_mesh()
{
  glBindVertexArray(vao_mesh);
  glUseProgram(shader_mesh);
  bind_vertex_attrib_array(shader_mesh,"position", vbo_V, V_vbo, dirty & MeshGL::DIRTY_POSITION);
  bind_vertex_attrib_array(shader_mesh,"normal", vbo_V_normals, V_normals_vbo, dirty & MeshGL::DIRTY_NORMAL);
  bind_vertex_attrib_array(shader_mesh,"Ka", vbo_V_ambient, V_ambient_vbo, dirty & MeshGL::DIRTY_AMBIENT);
  bind_vertex_attrib_array(shader_mesh,"Kd", vbo_V_diffuse, V_diffuse_vbo, dirty & MeshGL::DIRTY_DIFFUSE);
  bind_vertex_attrib_array(shader_mesh,"Ks", vbo_V_specular, V_specular_vbo, dirty & MeshGL::DIRTY_SPECULAR);
  bind_vertex_attrib_array(shader_mesh,"texcoord", vbo_V_uv, V_uv_vbo, dirty & MeshGL::DIRTY_UV);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo_F);
  if (dirty & MeshGL::DIRTY_FACE)
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*F_vbo.size(), F_vbo.data(), GL_DYNAMIC_DRAW);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vbo_tex);
  if (dirty & MeshGL::DIRTY_TEXTURE)
  {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tex_wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tex_wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex_filter);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_u, tex_v, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.data());
  }
  glUniform1i(glGetUniformLocation(shader_mesh,"tex"), 0);
  dirty &= ~MeshGL::DIRTY_MESH;
}

IGL_INLINE void igl::opengl::MeshGL::bind_overlay_lines()
{
  bool is_dirty = dirty & MeshGL::DIRTY_OVERLAY_LINES;

  glBindVertexArray(vao_overlay_lines);
  glUseProgram(shader_overlay_lines);
 bind_vertex_attrib_array(shader_overlay_lines,"position", vbo_lines_V, lines_V_vbo, is_dirty);
 bind_vertex_attrib_array(shader_overlay_lines,"color", vbo_lines_V_colors, lines_V_colors_vbo, is_dirty);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo_lines_F);
  if (is_dirty)
  {
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*lines_F_vbo.size(), lines_F_vbo.data(), GL_DYNAMIC_DRAW);
  }

  dirty &= ~MeshGL::DIRTY_OVERLAY_LINES;
}

IGL_INLINE void igl::opengl::MeshGL::bind_overlay_points()
{
  bool is_dirty = dirty & MeshGL::DIRTY_OVERLAY_POINTS;

  glBindVertexArray(vao_overlay_points);
  glUseProgram(shader_overlay_points);
 bind_vertex_attrib_array(shader_overlay_points,"position", vbo_points_V, points_V_vbo, is_dirty);
 bind_vertex_attrib_array(shader_overlay_points,"color", vbo_points_V_colors, points_V_colors_vbo, is_dirty);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo_points_F);
  if (is_dirty)
  {
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*points_F_vbo.size(), points_F_vbo.data(), GL_DYNAMIC_DRAW);
  }

  dirty &= ~MeshGL::DIRTY_OVERLAY_POINTS;
}

IGL_INLINE void igl::opengl::MeshGL::init_text_rendering()
{
  // Decompress the png of the font atlas
  unsigned char verasansmono_font_atlas[256*256];
  decompress_verasansmono_atlas(verasansmono_font_atlas);

  // Bind atlas
  glBindTexture(GL_TEXTURE_2D, font_atlas);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 256, 256, 0, GL_RED, GL_UNSIGNED_BYTE, verasansmono_font_atlas);

  // TextGL initialization
  vertex_labels.dirty_flag = MeshGL::DIRTY_VERTEX_LABELS;
  face_labels.dirty_flag = MeshGL::DIRTY_FACE_LABELS;
  custom_labels.dirty_flag = MeshGL::DIRTY_CUSTOM_LABELS;
}

IGL_INLINE void igl::opengl::MeshGL::bind_labels(const TextGL& labels)
{
  bool is_dirty = dirty & labels.dirty_flag;
  glBindTexture(GL_TEXTURE_2D, font_atlas);
  glBindVertexArray(labels.vao_labels);
  glUseProgram(shader_text);
  bind_vertex_attrib_array(shader_text, "position" , labels.vbo_labels_pos       , labels.label_pos_vbo   , is_dirty);
  bind_vertex_attrib_array(shader_text, "character", labels.vbo_labels_characters, labels.label_char_vbo  , is_dirty);
  bind_vertex_attrib_array(shader_text, "offset"   , labels.vbo_labels_offset    , labels.label_offset_vbo, is_dirty);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, labels.vbo_labels_indices);
  if (is_dirty)
  {
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned)*labels.label_indices_vbo.size(), labels.label_indices_vbo.data(), GL_DYNAMIC_DRAW);
  }
  dirty &= ~labels.dirty_flag;
}

IGL_INLINE void igl::opengl::MeshGL::draw_mesh(bool solid)
{
  glPolygonMode(GL_FRONT_AND_BACK, solid ? GL_FILL : GL_LINE);

  /* Avoid Z-buffer fighting between filled triangles & wireframe lines */
  if (solid)
  {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0, 1.0);
  }
  glDrawElements(GL_TRIANGLES, 3*F_vbo.rows(), GL_UNSIGNED_INT, 0);

  glDisable(GL_POLYGON_OFFSET_FILL);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

IGL_INLINE void igl::opengl::MeshGL::draw_overlay_lines()
{
  glDrawElements(GL_LINES, lines_F_vbo.rows(), GL_UNSIGNED_INT, 0);
}

IGL_INLINE void igl::opengl::MeshGL::draw_overlay_points()
{
  glDrawElements(GL_POINTS, points_F_vbo.rows(), GL_UNSIGNED_INT, 0);
}

IGL_INLINE void igl::opengl::MeshGL::draw_labels(const TextGL& labels)
{
  glDrawElements(GL_POINTS, labels.label_indices_vbo.rows(), GL_UNSIGNED_INT, 0);
}

IGL_INLINE void igl::opengl::MeshGL::init()
{
  if(is_initialized)
  {
    return;
  }
  is_initialized = true;
  std::string mesh_vertex_shader_string =
R"(#version 150
  uniform mat4 view;
  uniform mat4 proj;
  uniform mat4 normal_matrix;
  in vec3 position;
  in vec3 normal;
  out vec3 position_eye;
  out vec3 normal_eye;
  in vec4 Ka;
  in vec4 Kd;
  in vec4 Ks;
  in vec2 texcoord;
  out vec2 texcoordi;
  out vec4 Kai;
  out vec4 Kdi;
  out vec4 Ksi;
  uniform mat4 shadow_view;
  uniform mat4 shadow_proj;
  uniform bool shadow_pass;
  uniform bool is_shadow_mapping;
  out vec4 position_shadow;

  void main()
  {
    position_eye = vec3 (view * vec4 (position, 1.0));
    if(!shadow_pass)
    {
      if(is_shadow_mapping)
      {
        position_shadow = shadow_proj * shadow_view * vec4(position, 1.0);
      }
      normal_eye = vec3 (normal_matrix * vec4 (normal, 0.0));
      normal_eye = normalize(normal_eye);
      Kai = Ka;
      Kdi = Kd;
      Ksi = Ks;
      texcoordi = texcoord;
    }
    gl_Position = proj * vec4 (position_eye, 1.0); //proj * view * vec4(position, 1.0);
  }
)";

  std::string mesh_fragment_shader_string =
R"(#version 150
  uniform mat4 view;
  uniform mat4 proj;
  uniform vec4 fixed_color;
  in vec3 position_eye;
  in vec3 normal_eye;
  uniform bool is_directional_light;
  uniform bool is_shadow_mapping;
  uniform bool shadow_pass;
  uniform vec3 light_position_eye;
  vec3 Ls = vec3 (1, 1, 1);
  vec3 Ld = vec3 (1, 1, 1);
  vec3 La = vec3 (1, 1, 1);
  in vec4 Ksi;
  in vec4 Kdi;
  in vec4 Kai;
  in vec2 texcoordi;
  uniform sampler2D tex;
  uniform float specular_exponent;
  uniform float lighting_factor;
  uniform float texture_factor;
  uniform float matcap_factor;
  uniform float double_sided;

  uniform sampler2D shadow_tex;
  in vec4 position_shadow;

  out vec4 outColor;
  void main()
  {
    if(shadow_pass)
    {
      // Would it be better to have a separate no-op frag shader?
      outColor = vec4(0.56,0.85,0.77,1.);
      return;
    }
    // If is_directional_light then assume normalized
    vec3 direction_to_light_eye = light_position_eye;
    if(! is_directional_light)
    {
      vec3 vector_to_light_eye = light_position_eye - position_eye;
      direction_to_light_eye = normalize(vector_to_light_eye);
    }
    float shadow = 1.0;
    if(is_shadow_mapping)
    {
      vec3 shadow_pos = (position_shadow.xyz / position_shadow.w) * 0.5 + 0.5; 
      float currentDepth = shadow_pos.z;
      //float bias = 0.005;
      float ddd = max(dot(normalize(normal_eye), direction_to_light_eye),0);
      float bias = max(0.02 * (1.0 - ddd), 0.005);  
      // 5-point stencil
      if(shadow_pos.z < 1.0)
      {
        float closestDepth = texture( shadow_tex , shadow_pos.xy).r;
        shadow = currentDepth - bias >= closestDepth ? 0.0 : 1.0;  
        vec2 texelSize = 1.0 / textureSize(shadow_tex, 0);
        for(int x = -1; x <= 1; x+=2)
        {
          for(int y = -1; y <= 1; y+=2)
          {
            float pcfDepth = texture(shadow_tex,  shadow_pos.xy + vec2(x, y) * texelSize).r; 
            shadow += currentDepth - bias >= pcfDepth ? 0.0 : 1.0;        
          }    
        }
        shadow /= 5.0;
      }
    }

    if(matcap_factor == 1.0f)
    {
      vec2 uv = normalize(normal_eye).xy * 0.5 + 0.5;
      outColor = mix(Kai,texture(tex, uv),shadow);
    }else
    {
      vec3 Ia = La * vec3(Kai);    // ambient intensity

      float dot_prod = dot (direction_to_light_eye, normalize(normal_eye));
      float clamped_dot_prod = abs(max (dot_prod, -double_sided));
      vec3 Id = Ld * vec3(Kdi) * clamped_dot_prod;    // Diffuse intensity

      vec3 reflection_eye = reflect (-direction_to_light_eye, normalize(normal_eye));
      vec3 surface_to_viewer_eye = normalize (-position_eye);
      float dot_prod_specular = dot (reflection_eye, surface_to_viewer_eye);
      dot_prod_specular = float(abs(dot_prod)==dot_prod) * abs(max (dot_prod_specular, -double_sided));
      float specular_factor = pow (dot_prod_specular, specular_exponent);
      vec3 Is = Ls * vec3(Ksi) * specular_factor;    // specular intensity
      vec4 color = vec4(Ia + shadow*(lighting_factor * (Is + Id) + (1.0-lighting_factor) * vec3(Kdi)),(Kai.a+Ksi.a+Kdi.a)/3);
      outColor = mix(vec4(1,1,1,1), texture(tex, texcoordi), texture_factor) * color;
      if (fixed_color != vec4(0.0)) outColor = fixed_color;
    }
  }
)";

  std::string overlay_vertex_shader_string =
R"(#version 150
  uniform mat4 view;
  uniform mat4 proj;
  in vec3 position;
  in vec3 color;
  out vec3 color_frag;

  void main()
  {
    gl_Position = proj * view * vec4 (position, 1.0);
    color_frag = color;
  }
)";

  std::string overlay_fragment_shader_string =
R"(#version 150
  in vec3 color_frag;
  out vec4 outColor;
  void main()
  {
    outColor = vec4(color_frag, 1.0);
  }
)";

  std::string overlay_point_fragment_shader_string =
R"(#version 150
  in vec3 color_frag;
  out vec4 outColor;
  void main()
  {
    if (length(gl_PointCoord - vec2(0.5)) > 0.5)
      discard;
    outColor = vec4(color_frag, 1.0);
  }
)";

  std::string text_vert_shader =
R"(#version 330
    in vec3 position;
    in float character;
    in float offset;
    uniform mat4 view;
    uniform mat4 proj;
    out int vCharacter;
    out float vOffset;
    void main()
    {
      vCharacter = int(character);
      vOffset = offset;
      gl_Position = proj * view * vec4(position, 1.0);
    }
)";

  std::string text_geom_shader =
R"(#version 150 core
    layout(points) in;
    layout(triangle_strip, max_vertices = 4) out;
    out vec2 gTexCoord;
    uniform mat4 view;
    uniform mat4 proj;
    uniform vec2 CellSize;
    uniform vec2 CellOffset;
    uniform vec2 RenderSize;
    uniform vec2 RenderOrigin;
    uniform float TextShiftFactor;
    in int vCharacter[1];
    in float vOffset[1];
    void main()
    {
      // Code taken from https://prideout.net/strings-inside-vertex-buffers
      // Determine the final quad's position and size:
      vec4 P = gl_in[0].gl_Position + vec4( vOffset[0]*TextShiftFactor, 0.0, 0.0, 0.0 ); // 0.04
      vec4 U = vec4(1, 0, 0, 0) * RenderSize.x; // 1.0
      vec4 V = vec4(0, 1, 0, 0) * RenderSize.y; // 1.0

      // Determine the texture coordinates:
      int letter = vCharacter[0]; // used to be the character
      letter = clamp(letter - 32, 0, 96);
      int row = letter / 16 + 1;
      int col = letter % 16;
      float S0 = CellOffset.x + CellSize.x * col;
      float T0 = CellOffset.y + 1 - CellSize.y * row;
      float S1 = S0 + CellSize.x - CellOffset.x;
      float T1 = T0 + CellSize.y;

      // Output the quad's vertices:
      gTexCoord = vec2(S0, T1); gl_Position = P - U - V; EmitVertex();
      gTexCoord = vec2(S1, T1); gl_Position = P + U - V; EmitVertex();
      gTexCoord = vec2(S0, T0); gl_Position = P - U + V; EmitVertex();
      gTexCoord = vec2(S1, T0); gl_Position = P + U + V; EmitVertex();
      EndPrimitive();
    }
)";

  std::string text_frag_shader =
R"(#version 330
    out vec4 outColor;
    in vec2 gTexCoord;
    uniform sampler2D font_atlas;
    uniform vec3 TextColor;
    void main()
    {
      float A = texture(font_atlas, gTexCoord).r;
      outColor = vec4(TextColor, A);
    }
)";

  init_buffers();
  init_text_rendering();
  create_shader_program(
    mesh_vertex_shader_string,
    mesh_fragment_shader_string,
    {},
    shader_mesh);
  create_shader_program(
    overlay_vertex_shader_string,
    overlay_fragment_shader_string,
    {},
    shader_overlay_lines);
  create_shader_program(
    overlay_vertex_shader_string,
    overlay_point_fragment_shader_string,
    {},
    shader_overlay_points);
  create_shader_program(
    text_geom_shader,
    text_vert_shader,
    text_frag_shader,
    {},
    shader_text);
}

IGL_INLINE void igl::opengl::MeshGL::free()
{
  const auto free = [](GLuint & id)
  {
    if(id)
    {
      destroy_shader_program(id);
      id = 0;
    }
  };

  if (is_initialized)
  {
    free(shader_mesh);
    free(shader_overlay_lines);
    free(shader_overlay_points);
    free(shader_text);
    free_buffers();
  }
}
