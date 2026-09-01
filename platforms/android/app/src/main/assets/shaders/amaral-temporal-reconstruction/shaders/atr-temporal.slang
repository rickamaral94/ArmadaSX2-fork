#version 450

// Amaral Temporal Reconstruction (ATR), temporal pass.
// Clean-room GPL-3.0 implementation. No NVIDIA code, model or binary is used.
//
// PS2 games do not provide engine motion vectors. This pass therefore never assumes that a
// previous pixel belongs to the same surface: it derives a conservative confidence from the raw
// current/previous frames, clamps history into the current neighbourhood, and bypasses history
// immediately on motion or a cut. That makes this a temporal reconstruction foundation rather
// than a claim of DLSS compatibility.

#pragma name ATRTemporal
#pragma parameter ATR_STABILITY "ATR · Temporal stability" 0.18 0.00 0.45 0.01
#pragma parameter ATR_MOTION_THRESHOLD "ATR · Motion threshold" 0.060 0.015 0.180 0.005
#pragma parameter ATR_HISTORY_CLAMP "ATR · History clamp" 0.025 0.000 0.080 0.005

layout(push_constant) uniform Push
{
    vec4 OutputSize;
    vec4 OriginalSize;
    vec4 SourceSize;
    uint FrameCount;
    float ATR_STABILITY;
    float ATR_MOTION_THRESHOLD;
    float ATR_HISTORY_CLAMP;
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
layout(set = 0, binding = 3) uniform sampler2D Original;
layout(set = 0, binding = 4) uniform sampler2D OriginalHistory1;
layout(set = 0, binding = 5) uniform sampler2D ATRSpatialFeedback;

float atr_luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 px = params.SourceSize.zw;
    vec3 current = texture(Source, vTexCoord).rgb;
    vec3 n = texture(Source, vTexCoord + vec2(0.0, -px.y)).rgb;
    vec3 s = texture(Source, vTexCoord + vec2(0.0,  px.y)).rgb;
    vec3 w = texture(Source, vTexCoord + vec2(-px.x, 0.0)).rgb;
    vec3 e = texture(Source, vTexCoord + vec2( px.x, 0.0)).rgb;

    vec3 raw_now = texture(Original, vTexCoord).rgb;
    vec3 raw_prev = texture(OriginalHistory1, vTexCoord).rgb;
    vec3 history = texture(ATRSpatialFeedback, vTexCoord).rgb;

    float luma_delta = abs(atr_luma(raw_now) - atr_luma(raw_prev));
    float chroma_delta = max(max(abs(raw_now.r - raw_prev.r), abs(raw_now.g - raw_prev.g)),
        abs(raw_now.b - raw_prev.b));
    float motion = max(luma_delta, chroma_delta * 0.65);
    float stable = 1.0 - smoothstep(params.ATR_MOTION_THRESHOLD * 0.55,
        params.ATR_MOTION_THRESHOLD * 1.80, motion);

    // Clamp old information into the current 5-tap neighbourhood before it can contribute.
    // This is the main disocclusion/ghosting guard when no engine motion vectors exist.
    vec3 local_min = min(current, min(min(n, s), min(w, e))) - vec3(params.ATR_HISTORY_CLAMP);
    vec3 local_max = max(current, max(max(n, s), max(w, e))) + vec3(params.ATR_HISTORY_CLAMP);
    vec3 safe_history = clamp(history, local_min, local_max);

    float local_edge = clamp(max(abs(atr_luma(e) - atr_luma(w)),
        abs(atr_luma(s) - atr_luma(n))) * 3.0, 0.0, 1.0);
    float history_weight = params.FrameCount == 0u ? 0.0 :
        params.ATR_STABILITY * stable * mix(1.0, 0.72, local_edge);

    FragColor = vec4(clamp(mix(current, safe_history, history_weight), 0.0, 1.0), 1.0);
}
