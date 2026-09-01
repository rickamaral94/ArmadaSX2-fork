#version 450

// Amaral Temporal Reconstruction (ATR), spatial pass.
// Clean-room GPL-3.0 implementation. No NVIDIA code, model or binary is used.
//
// This pass reconstructs the source at the final viewport resolution and restores a bounded
// amount of local contrast. The min/max clamp is load-bearing: without it, sharpening a bright
// PS2 HUD over a dark scene creates ringing that the temporal pass would then preserve.

#pragma name ATRSpatial
#pragma parameter ATR_DETAIL "ATR · Detail recovery" 0.32 0.00 0.70 0.01
#pragma parameter ATR_EDGE_GUARD "ATR · Edge/ringing guard" 0.70 0.00 1.00 0.01

layout(push_constant) uniform Push
{
    vec4 OutputSize;
    vec4 OriginalSize;
    vec4 SourceSize;
    float ATR_DETAIL;
    float ATR_EDGE_GUARD;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

float atr_luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 px = params.SourceSize.zw;
    vec3 c = texture(Source, vTexCoord).rgb;
    vec3 n = texture(Source, vTexCoord + vec2(0.0, -px.y)).rgb;
    vec3 s = texture(Source, vTexCoord + vec2(0.0,  px.y)).rgb;
    vec3 w = texture(Source, vTexCoord + vec2(-px.x, 0.0)).rgb;
    vec3 e = texture(Source, vTexCoord + vec2( px.x, 0.0)).rgb;

    vec3 local_min = min(c, min(min(n, s), min(w, e)));
    vec3 local_max = max(c, max(max(n, s), max(w, e)));
    vec3 laplacian = c * 4.0 - (n + s + w + e);

    float gx = abs(atr_luma(e) - atr_luma(w));
    float gy = abs(atr_luma(s) - atr_luma(n));
    float edge = clamp(max(gx, gy) * 3.0, 0.0, 1.0);
    float gain = params.ATR_DETAIL * mix(1.0, 0.42, edge * params.ATR_EDGE_GUARD);

    // A tiny expansion retains legitimate contrast while still preventing visible halos.
    vec3 guard = vec3(mix(0.035, 0.008, params.ATR_EDGE_GUARD));
    vec3 reconstructed = clamp(c + laplacian * gain, local_min - guard, local_max + guard);
    FragColor = vec4(clamp(reconstructed, 0.0, 1.0), 1.0);
}
