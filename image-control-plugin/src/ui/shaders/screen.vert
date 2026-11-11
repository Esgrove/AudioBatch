/// Makes a triangle that covers the viewport from the vertex index, with texture coordinates from 0 to 1.
#version 330 core
out vec2 uv;
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
