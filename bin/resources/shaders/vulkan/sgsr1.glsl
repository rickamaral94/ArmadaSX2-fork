#version 460 core

//============================================================================================================
// Snapdragon(TM) Game Super Resolution 1 — edge-direction filter.
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
// Ported from sgsr/v1/include/glsl/sgsr1_shader_mobile.frag of
// https://github.com/SnapdragonStudios/snapdragon-gsr (commit d926f074bc), which is the upstream
// this must be diffed against. The filter math — fastLanczos2, weightY, the twelve taps, the
// edge vote, the +-23/255 delta clamp — is unchanged from that file, so a future upstream fix
// applies here line for line.
//
// THREE deliberate deviations, all forced by running inside an emulator rather than a game:
//
//  1. FRAGMENT -> COMPUTE. Qualcomm ships a fragment shader driven by a full-screen triangle's
//     interpolated UV. PCSX2's present-time upscalers (FSR1, CAS) are compute and run before the
//     present render pass opens, so a fragment pass would need its own pass and target. The UV is
//     therefore derived from gl_GlobalInvocationID instead of an input varying; nothing else about
//     the sampling changes.
//
//  2. SOURCE SUB-RECT. A game hands SGSR the whole render target. The GS hands us a texture whose
//     DISPLAYED region is a sub-rect (src_rect) of a larger allocation, exactly as FSR1's
//     FsrEasuConOffset accounts for. SrcRect carries that offset and size so the filter reads and
//     clamps inside the displayed region; feeding it the whole texture would drag whatever sits
//     beside the frame into the edges.
//
//  3. OPERATION MODE FIXED AT RGBA (Qualcomm's mode 1). Their mode 3 (RGBY) needs luma packed in
//     alpha by an earlier pass and mode 4 is plain bilinear, neither of which applies here; with
//     the mode a compile-time constant the dead branches fold away.
//============================================================================================================

// Qualcomm's defaults for mobile. EdgeThreshold is 8/255 for phones and 4/255 for VR; we are a
// handheld, so 8/255 stands. Both are hoisted to push constants ONLY if a future A/B needs them —
// keeping them constant now is what lets the compiler fold the edge test.
#define kEdgeThreshold (8.0 / 255.0)
#define kEdgeSharpness 2.0

layout(push_constant) uniform const_buffer
{
    // xy = 1.0 / source texture size, zw = source texture size, in texels. Matches the layout of
    // Qualcomm's ViewportInfo[0] so their indexing below reads unchanged.
    vec4 ViewportInfo;
    // xy = displayed sub-rect origin, zw = displayed sub-rect size, in source texels.
    vec4 SrcRect;
    // xy = destination size in texels.
    uvec4 DstSize;
};

layout(set = 0, binding = 0) uniform sampler2D imgSrc;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D imgDst;

float fastLanczos2(float x)
{
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, float std)
{
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
    const uvec2 pos = gl_GlobalInvocationID.xy;
    if (pos.x >= DstSize.x || pos.y >= DstSize.y)
        return;

    // Destination pixel centre -> position inside the displayed sub-rect -> normalised source UV.
    // Deviation 2: SrcRect, not the whole texture, is what maps to 0..1 of the output.
    const vec2 dst_uv = (vec2(pos) + vec2(0.5)) / vec2(DstSize.xy);
    const vec2 src_texel = SrcRect.xy + dst_uv * SrcRect.zw;
    const vec2 uv = src_texel * ViewportInfo.xy;

    // From here down this is Qualcomm's shader with mode == 1 (RGBA) folded in, so `mode` is the
    // literal 1 wherever they index a channel: colour[1] and textureGather(..., 1) are the GREEN
    // channel, which SGSR uses as its luma proxy.
    vec4 color;
    color.xyz = textureLod(imgSrc, uv, 0.0).xyz;

    vec2 imgCoord = ((uv * ViewportInfo.zw) + vec2(-0.5, 0.5));
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 coord = (imgCoordPixel * ViewportInfo.xy);
    vec2 pl = (imgCoord + (-imgCoordPixel));
    vec4 left = textureGather(imgSrc, coord, 1);

    float edgeVote = abs(left.z - left.y) + abs(color[1] - left.y) + abs(color[1] - left.z);
    if (edgeVote > kEdgeThreshold)
    {
        coord.x += ViewportInfo.x;

        vec4 right = textureGather(imgSrc, coord + vec2(ViewportInfo.x, 0.0), 1);
        vec4 upDown;
        upDown.xy = textureGather(imgSrc, coord + vec2(0.0, -ViewportInfo.y), 1).wz;
        upDown.zw = textureGather(imgSrc, coord + vec2(0.0, ViewportInfo.y), 1).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[1] - mean;

        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                       (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                     (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float std = 2.181818 / sum;

        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
        aWY += weightY(pl.x, pl.y, left.z, std);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, std);

        float finalY = aWY.y / aWY.x;

        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(kEdgeSharpness * finalY, minY, maxY);

        float deltaY = finalY - color.w;

        // smooth high contrast input
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

        color.x = clamp((color.x + deltaY), 0.0, 1.0);
        color.y = clamp((color.y + deltaY), 0.0, 1.0);
        color.z = clamp((color.z + deltaY), 0.0, 1.0);
    }

    color.w = 1.0; // assume alpha channel is not used
    imageStore(imgDst, ivec2(pos), color);
}
