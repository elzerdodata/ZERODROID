#version 460

// FSR 1 (RCAS) sharpening pass for the Switch video path.
//
// Second half of AMD FidelityFX Super Resolution 1: Robust Contrast Adaptive
// Sharpening applied to the upscaled (or 1:1) RGB frame produced by the video
// pass. Runs at the framebuffer resolution and reads the intermediate target,
// so it never touches the padded NVDEC surface directly.
//
// Adapted from AMD FidelityFX FSR 1.0.2 (ffx-fsr/ffx_fsr1.h), (c) 2021
// Advanced Micro Devices, Inc. -- MIT License. See THIRD_PARTY_NOTICES.md.
//
// FMAs are kept in plain fp32: the software reciprocal tricks of the reference
// implementation only exist to save instructions on AMD hardware, and Maxwell
// compiles 1.0/x to a single RCP. The guards around the reciprocal keep the
// degenerate all-black/all-white neighbourhoods from producing NaNs.

layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D fsrtex;

layout (std140, binding = 0) uniform Transformation
{
    mat3 yuvmat;
    vec3 offset;
    vec4 luma_uv_data;
    vec4 chroma_uv_data;
    vec4 sharp_data;
    vec4 picture_data;
    vec4 motion_data;
    // x = visible input width, y = visible input height,
    // z = output width, w = output height.
    vec4 fsr_data;
    // x = sharpness (linear, exp2(-stops)); y/z/w reserved.
    vec4 fsr_sharp;
} u;

// This is set at the limit of providing unnatural results for sharpening.
const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

float rcpSafe(float v)
{
    return abs(v) > 1e-8 ? 1.0 / v : 0.0;
}

vec3 fetchRgb(ivec2 p, ivec2 hi)
{
    return texelFetch(fsrtex, clamp(p, ivec2(0), hi), 0).rgb;
}

float luma2(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

void main()
{
    ivec2 hi = ivec2(u.fsr_data.zw) - ivec2(1);
    ivec2 ip = ivec2(vTextureCoord * u.fsr_data.zw);

    // Minimal 3x3 neighbourhood.
    //    b
    //  d e f
    //    h
    vec3 b = fetchRgb(ip + ivec2( 0, -1), hi);
    vec3 d = fetchRgb(ip + ivec2(-1,  0), hi);
    vec3 e = fetchRgb(ip, hi);
    vec3 f = fetchRgb(ip + ivec2( 1,  0), hi);
    vec3 h = fetchRgb(ip + ivec2( 0,  1), hi);

    float bL = luma2(b);
    float dL = luma2(d);
    float eL = luma2(e);
    float fL = luma2(f);
    float hL = luma2(h);

    // Noise detection: damp sharpening where the ring looks like noise.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float nzRange = max(max(max(bL, dL), max(eL, fL)), hL) -
                    min(min(min(bL, dL), min(eL, fL)), hL);
    nz = clamp(abs(nz) * rcpSafe(nzRange), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    // Min and max of the ring.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));

    // Limiters: how far the neighbourhood can be pushed before clipping.
    vec3 hitMin = min(mn4, e) * vec3(rcpSafe(4.0 * mx4.r),
                                     rcpSafe(4.0 * mx4.g),
                                     rcpSafe(4.0 * mx4.b));
    vec3 hitMax = (vec3(1.0) - max(mx4, e)) *
                  vec3(rcpSafe(4.0 * mn4.r - 4.0),
                       rcpSafe(4.0 * mn4.g - 4.0),
                       rcpSafe(4.0 * mn4.b - 4.0));
    vec3 lobe3 = max(-hitMin, hitMax);

    float lobe = max(-FSR_RCAS_LIMIT,
                     min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) *
                 u.fsr_sharp.x;
    lobe *= nz;

    // Resolve.
    vec3 res = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    outColor = vec4(clamp(res, 0.0, 1.0), 1.0);
}
