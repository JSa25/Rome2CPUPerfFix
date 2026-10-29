// Rome 2's per-object motion pass shaders (dlss/DLSS_LOG.md 19-22). The vertex shader repeats the skinning of the game's
// animated main-view shaders, taken from the bytecode the game runs (G1: vs a779cfb30091fd73 2 bones; vs
// 8dc6a79851998275 / 16ba4ce7298fd83f 4 bones, position path only) with the same instructions in the same order: bone
// indices floor(POSITION1 x 255.01) x 3, a bone's three texels sampled (point sampler, level 0) at u = i w, i w + w,
// (i w + w) + w in row v = TEXCOORD3.x h (w, h from the game's variant_animation_size cbuffer), rows dot [pos, 1], the
// weighted sum, the instance's world rows TEXCOORD0..2, then view_projection - once with this frame's bones, record and
// camera, once with the previous frame's (PREV0..2 = the previous record's world rows, PREV3.x its animation row, .y < 0:
// no history -> this frame's pose and place, so only the camera moves it). The pixel shader writes the motion in pixels
// of the render size, previous minus current (the bridge's convention, fgbridge.c). SKIN4 for the 4-bone layouts.
#pragma once

static const char r2motion_hlsl[] =
    "cbuffer motion : register(b0) { float2 size; float2 pad; };\n"
    // byte copies of the game's blocks: camera_VS_PS (view_projection at row 9) of this frame and the previous one, and
    // variant_animation_size (x: rows - 1, y: 1 / rows, z: columns - 1, w: 1 / (columns - 1)) of this frame / the previous
    "cbuffer camera : register(b1) { float4 c_pad[9]; float4 vp[4]; };\n"
    "cbuffer prev_camera : register(b2) { float4 p_pad[9]; float4 prev_vp[4]; };\n"
    "cbuffer anim_size : register(b3) { float4 asz; };\n"
    "cbuffer prev_anim_size : register(b4) { float4 prev_asz; };\n"
    "Texture2D<float4> anim : register(t0);\n"
    "Texture2D<float4> prev_anim : register(t1);\n"
    "SamplerState pt : register(s0);\n"
    "struct VIn {\n"
    "  float4 pos : POSITION0; float4 bi : POSITION1;\n"
    "#ifdef SKIN4\n"
    "  float4 bw : POSITION2;\n"
    "#endif\n"
    "  float4 w0 : TEXCOORD0; float4 w1 : TEXCOORD1; float4 w2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "  float4 p0 : PREV0; float4 p1 : PREV1; float4 p2 : PREV2; float4 p3 : PREV3; };\n"
    "struct VOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1;\n"
    "#ifdef OWN\n"
    "  float4 own : TEXCOORD2;\n"
    "#endif\n"
    "};\n"
    // one bone at texel index i3 (= 3 x bone) in row v: rows x, y, z dot p (the game: u = i w; u1 = i w + w; u2 = u1 + w)
    "float3 bone(Texture2D<float4> t, float i3, float w, float v, float4 p) {\n"
    "  float u0 = i3 * w; float u1 = i3 * w + w; float u2 = u1 + w;\n"
    "  return float3(dot(p, t.SampleLevel(pt, float2(u0, v), 0)), dot(p, t.SampleLevel(pt, float2(u1, v), 0)),\n"
    "                dot(p, t.SampleLevel(pt, float2(u2, v), 0))); }\n"
    "float3 skin(Texture2D<float4> t, float4 sz, VIn i, float row) {\n"
    "  float4 p = float4(i.pos.xyz, 1); float v = row * sz.y;\n"
    "#ifdef SKIN4\n"
    "  float4 k = (float4)((int4)floor(i.bi.zyxw * 255.01) * 3);\n"   // bones from .z, .y, .x, .w
    "  float wl = 1 - ((i.bw.y + i.bw.z) + i.bw.x);\n"
    "  float3 r = bone(t, k.y, sz.w, v, p) * i.bw.y;\n"
    "  r = i.bw.z * bone(t, k.x, sz.w, v, p) + r;\n"
    "  r = i.bw.x * bone(t, k.z, sz.w, v, p) + r;\n"
    "  return wl * bone(t, k.w, sz.w, v, p) + r;\n"
    "#else\n"
    "  float2 k = (float2)((int2)floor(i.bi.zy * 255.01) * 3);\n"     // bone A from .z (weight .x), bone B from .y (1 - .x)
    "  float3 r = bone(t, k.y, sz.w, v, p) * (1 - i.bi.x);\n"
    "  return i.bi.x * bone(t, k.x, sz.w, v, p) + r;\n"
    "#endif\n"
    "}\n"
    "float4 place(float3 s, float4 r0, float4 r1, float4 r2, float4 m[4]) {\n"
    "  float4 q = float4(s, 1); float4 w = float4(dot(q, r0), dot(q, r1), dot(q, r2), 1);\n"
    "  return float4(dot(w, m[0]), dot(w, m[1]), dot(w, m[2]), dot(w, m[3])); }\n"
    "VOut vs(VIn i) {\n"
    "  VOut o;\n"
    "  o.cur = place(skin(anim, asz, i, i.t3.x), i.w0, i.w1, i.w2, vp);\n"
    "#ifdef OWN\n"
    "  if (i.p3.y < 0) { o.prev = place(skin(anim, asz, i, i.t3.x), i.w0, i.w1, i.w2, prev_vp); o.own = o.cur; }\n"
    "  else { float3 s = skin(prev_anim, prev_asz, i, i.p3.x); o.prev = place(s, i.p0, i.p1, i.p2, prev_vp); o.own = place(s, i.p0, i.p1, i.p2, vp); }\n"
    "#else\n"
    "  if (i.p3.y < 0) o.prev = place(skin(anim, asz, i, i.t3.x), i.w0, i.w1, i.w2, prev_vp);\n"
    "  else o.prev = place(skin(prev_anim, prev_asz, i, i.p3.x), i.p0, i.p1, i.p2, prev_vp);\n"
    "#endif\n"
    "  o.pos = o.cur;\n"
    "  return o; }\n"
    // target 0: the motion; OWN (the debug view on) target 1: the model's own motion (its last pose and place through this
    // frame's camera), w = 1 where a model was drawn
    "#ifdef OWN\n"
    "struct POut { float2 mv : SV_Target0; float4 own : SV_Target1; };\n"
    "POut ps(VOut i) {\n"
    "  POut r; float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w, q = i.own.xy / i.own.w;\n"
    "  r.mv = (p - c) * float2(0.5, -0.5) * size; r.own = float4((q - c) * float2(0.5, -0.5) * size, 0, 1); return r; }\n"
    "#else\n"
    "float2 ps(VOut i) : SV_Target {\n"
    "  float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w;\n"
    "  return (p - c) * float2(0.5, -0.5) * size; }\n"
    "#endif\n";
