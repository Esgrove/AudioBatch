/// Places a sprite around a particle: the centre from its plot shares, the spread from its bin spacing,
/// never narrower than the sharp core, and the size from the wider of the core and the halo.
/// The core and the halo scale with the particle's size, so louder content draws larger dots,
/// and the halo's brightness scales with the particle's emphasis.
#version 330 core
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 placement;
layout(location = 2) in vec3 tint;
layout(location = 3) in float particleSize;
layout(location = 4) in float particleEmphasis;
uniform vec2 targetSize;
uniform float coreSigma;
uniform float maxSigma;
uniform float haloSigma;
uniform float reach;
out vec2 offset;
flat out vec2 core;
flat out vec2 halo;
flat out vec3 particleColour;
flat out float emphasis;
void main()
{
    vec2 centre = vec2(placement.x, 1.0 - placement.y) * targetSize;
    emphasis = particleEmphasis;
    float sizedCore = coreSigma * particleSize;
    float sizedHalo = haloSigma * particleSize;
    core = vec2(clamp(placement.z * targetSize.x, sizedCore, maxSigma), sizedCore);
    halo = vec2(max(core.x, sizedHalo), sizedHalo);
    offset = corner * halo * reach;
    particleColour = tint;
    gl_Position = vec4((centre + offset) / targetSize * 2.0 - 1.0, 0.0, 1.0);
}
