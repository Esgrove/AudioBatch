/// A Gaussian core with a faint wider halo, evaluated at the exact sub-pixel offset from the centre,
/// sharing the particle's weight with a wide, coreless haze Gaussian by its haze share.
#version 330 core
in vec2 offset;
flat in vec2 core;
flat in vec2 halo;
flat in vec2 hazeSpread;
flat in vec3 particleColour;
flat in float emphasis;
flat in float haze;
uniform float haloWeight;
out vec4 colour;
void main()
{
    vec2 inCore = offset / core;
    vec2 inHalo = offset / halo;
    vec2 inHaze = offset / hazeSpread;
    float sprite = exp(-0.5 * dot(inCore, inCore)) + haloWeight * emphasis * exp(-0.5 * dot(inHalo, inHalo));
    float weight = mix(sprite, exp(-0.5 * dot(inHaze, inHaze)), haze);
    colour = vec4(particleColour * weight, 0.0);
}
