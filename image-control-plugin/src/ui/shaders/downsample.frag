/// Halves a picture with the 13-tap filter of the Call of Duty bloom, which does not flicker as particles move.
#version 330 core
in vec2 uv;
uniform sampler2D source;
uniform vec2 sourceTexel;
out vec4 colour;
vec3 tap(float x, float y)
{
    return texture(source, uv + vec2(x, y) * sourceTexel).rgb;
}
void main()
{
    vec3 outer = (tap(-2.0, 2.0) + tap(2.0, 2.0) + tap(-2.0, -2.0) + tap(2.0, -2.0)) * 0.03125;
    vec3 edges = (tap(0.0, 2.0) + tap(-2.0, 0.0) + tap(2.0, 0.0) + tap(0.0, -2.0)) * 0.0625;
    vec3 inner = (tap(-1.0, 1.0) + tap(1.0, 1.0) + tap(-1.0, -1.0) + tap(1.0, -1.0)) * 0.125;
    colour = vec4(tap(0.0, 0.0) * 0.125 + outer + edges + inner, 1.0);
}
