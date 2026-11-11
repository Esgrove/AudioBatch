/// Doubles a picture with a 3×3 tent filter. The blend function adds it to the next larger level.
#version 330 core
in vec2 uv;
uniform sampler2D source;
uniform vec2 sourceTexel;
uniform float radius;
out vec4 colour;
vec3 tap(float x, float y)
{
    return texture(source, uv + vec2(x, y) * sourceTexel * radius).rgb;
}
void main()
{
    vec3 sum = tap(0.0, 0.0) * 4.0;
    sum += (tap(0.0, 1.0) + tap(-1.0, 0.0) + tap(1.0, 0.0) + tap(0.0, -1.0)) * 2.0;
    sum += tap(-1.0, 1.0) + tap(1.0, 1.0) + tap(-1.0, -1.0) + tap(1.0, -1.0);
    colour = vec4(sum / 16.0, 1.0);
}
