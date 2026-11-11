/// A Gaussian core with a faint wider halo, evaluated at the exact sub-pixel offset from the centre.
#version 330 core
in vec2 offset;
flat in vec2 core;
flat in vec2 halo;
flat in vec3 particleColour;
flat in float emphasis;
uniform float haloWeight;
out vec4 colour;
void main()
{
    vec2 inCore = offset / core;
    vec2 inHalo = offset / halo;
    float weight = exp(-0.5 * dot(inCore, inCore)) + haloWeight * emphasis * exp(-0.5 * dot(inHalo, inHalo));
    colour = vec4(particleColour * weight, 0.0);
}
