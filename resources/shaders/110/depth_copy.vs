#version 110

// A quad over the whole target, given in clip space; see depth_copy.fs.

attribute vec3 v_position;

void main()
{
    gl_Position = vec4(v_position.xy, 0.0, 1.0);
}
