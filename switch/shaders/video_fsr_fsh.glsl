#version 460

// FSR 1 (EASU) luma upscale + NV12 -> RGB for the Switch video path.
//
// Luma-only port of AMD FidelityFX Super Resolution 1.0 (EASU): the 12-tap
// edge-adaptive kernel runs on the R8 luma plane, while the half-resolution
// chroma plane keeps the cheap bilinear sample (same trade-off as the mpv FSR
// port). The colour matrix, picture controls and motion blend behave exactly
// like video_fsh.glsl; the internal unsharp mask is disabled while FSR is on.
//
// Adapted from AMD FidelityFX FSR 1.0.2 (ffx-fsr/ffx_fsr1.h) and the mpv GLSL
// port, (c) 2021 Advanced Micro Devices, Inc. -- MIT License. See
// THIRD_PARTY_NOTICES.md.
//
// The 12 taps are fetched by explicit texel index (no textureGather), which is
// portable across UAM/Maxwell and lets us clamp to the visible area so NVDEC's
// alignment padding never bleeds into the upscaled image.

layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D plane0;
layout (binding = 1) uniform sampler2D plane1;
layout (binding = 2) uniform sampler2D motionPlane0;
layout (binding = 3) uniform sampler2D motionPlane1;

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
    // z = output (framebuffer) width, w = output height.
    vec4 fsr_data;
    // x = RCAS sharpness in linear form (exp2(-stops)); y/z/w reserved.
    vec4 fsr_sharp;
} u;

// One luma tap, clamped to the visible area of the padded NVDEC surface.
float lumaFetch(ivec2 p)
{
    ivec2 hi = ivec2(u.fsr_data.xy) - ivec2(1);
    return texelFetch(plane0, clamp(p, ivec2(0), hi), 0).r;
}

// Filtering for a given tap (FsrEasuTap, scalar/luma variant).
void easuTap(inout float aC, inout float aW, vec2 off, vec2 dir, vec2 len,
             float lob, float clp, float c)
{
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;

    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);

    // Appoximation of lanczos2 without sin()/rcp()/sqrt().
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;

    aC += c * w;
    aW += w;
}

// Accumulate gradient direction and length for one quadrant (FsrEasuSet).
void easuSet(inout vec2 dir, inout float len, vec2 pp,
             bool biS, bool biT, bool biU, bool biV,
             float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    float dc = lD - lC;
    float cb = lC - lB;
    float denX = max(abs(dc), abs(cb));
    float dirX = lD - lB;
    float lenX = denX > 0.0 ? clamp(abs(dirX) / denX, 0.0, 1.0) : 0.0;
    lenX *= lenX;

    float ec = lE - lC;
    float ca = lC - lA;
    float denY = max(abs(ec), abs(ca));
    float dirY = lE - lA;
    float lenY = denY > 0.0 ? clamp(abs(dirY) / denY, 0.0, 1.0) : 0.0;
    lenY *= lenY;

    dir += vec2(dirX, dirY) * w;
    len += (lenX + lenY) * w;
}

// EASU resolve for one output pixel. vTextureCoord covers the whole visible
// input area, so uv * visibleSize is this fragment's position in input texels.
float easuLuma(vec2 uv)
{
    vec2 vis = u.fsr_data.xy;
    vec2 pp = uv * vis - vec2(0.5);
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 base = ivec2(fp);

    // 12-tap kernel.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float b = lumaFetch(base + ivec2( 0, -1));
    float c = lumaFetch(base + ivec2( 1, -1));
    float e = lumaFetch(base + ivec2(-1,  0));
    float f = lumaFetch(base + ivec2( 0,  0));
    float g = lumaFetch(base + ivec2( 1,  0));
    float h = lumaFetch(base + ivec2( 2,  0));
    float i = lumaFetch(base + ivec2(-1,  1));
    float j = lumaFetch(base + ivec2( 0,  1));
    float k = lumaFetch(base + ivec2( 1,  1));
    float l = lumaFetch(base + ivec2( 2,  1));
    float n = lumaFetch(base + ivec2( 0,  2));
    float o = lumaFetch(base + ivec2( 1,  2));

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, pp, true,  false, false, false, b, e, f, g, j);
    easuSet(dir, len, pp, false, true,  false, false, c, f, g, h, k);
    easuSet(dir, len, pp, false, false, true,  false, f, i, j, k, n);
    easuSet(dir, len, pp, false, false, false, true,  g, j, k, l, o);

    // Normalize and clean up close to zero (flat areas).
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(max(dirR, 1e-12));
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    // Transform {0..2} -> {0..1} and shape with square.
    len *= 0.5;
    len *= len;

    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    float stretch = dot(dir, dir) /
                    max(max(abs(dir.x), abs(dir.y)), 1e-6);
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // Window shifts from +/-sqrt(2.0) towards slightly beyond 2.0.
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float aC = 0.0;
    float aW = 0.0;
    easuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    easuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

    // Normalize and dering against the 2x2 neighbourhood.
    float res = aW > 0.0 ? aC / aW : f;
    res = clamp(res,
                min(min(f, g), min(j, k)),
                max(max(f, g), max(j, k)));
    return clamp(res, 0.0, 1.0);
}

void main()
{
    vec2 lumaUv = u.luma_uv_data.xy + vTextureCoord * u.luma_uv_data.zw;
    vec2 chromaUv = u.chroma_uv_data.xy +
                    vTextureCoord * u.chroma_uv_data.zw;

    float y;
    if (u.motion_data.y > 0.5) {
        // Motion generates a synthetic midpoint from a second surface. EASU
        // needs 12 taps per surface, so the generated frame keeps the plain
        // bilinear blend instead of mixing filtered and unfiltered luma.
        float y0 = texture(plane0, lumaUv).r;
        float y1 = texture(motionPlane0, lumaUv).r;
        y = mix(y0, y1, u.motion_data.x);
    } else {
        y = easuLuma(vTextureCoord);
    }

    vec2 chroma = texture(plane1, chromaUv).rg;
    vec3 yuv = vec3(y, chroma.r, chroma.g) - u.offset;
    vec3 rgb = u.yuvmat * yuv;

    rgb = (rgb - vec3(0.5)) * u.picture_data.y + vec3(0.5);
    float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luminance), rgb, u.picture_data.z);
    rgb += vec3(u.picture_data.x);
    rgb = pow(clamp(rgb, 0.0, 1.0),
              vec3(1.0 / max(u.picture_data.w, 0.01)));

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
