// Motion shaders for Rome 2's swaying trees and plants (ATTILA_PARITY_PLAN phase 6, from Attila's windvs.h, its AGENT_LOG
// 174-178). The vegetation moves by the game's wind code, a pure function of the camera block's time_in_sec (camera_VS_PS
// c26.x), the wind block (wind_VS: wind_direction.xy / wind_vegetation_multiplier in c0, hstrength / vstrength in c1), a
// phase per instance and a weight per vertex. Each entry point repeats the game's position math instruction for
// instruction (mad() where the game has mad, precise against fusing, its constants by their bits) once at this frame's time
// with this frame's camera block - equal to the game's clip position bit for bit, because the pass tests depth EQUAL against
// the game's depth - and once at the previous frame's. dlss/proof/r2windtest.c checks both against the game's own shaders
// (captures/bench_mv/run2, the user's battle benchmark, 2026-11-02).
//   tree_a: vs_72e5f54ba5fa935c (and ecdb01be03e86b1c: the same code) - Attila's tree_a with Rome 2's inputs (phase in
//           TEXCOORD3, the second LOD's rows in TEXCOORD4..6) and the weight squared first, then times the strength
//   tree_b: vs_fb22dfcbda8c29cf - the same against Attila's tree_b (LOD blend by a single factor from POSITION6.y)
//   plant:  vs_1d3a911d5b740853 - Rome 2's own: its instance rows in TEXCOORD1..3, phase from its world x / z
// The game's other wind shaders (342ce312, 82eeeb66, 975d9d59, c03a6ec7, d3f651ab, db2cfb02, dccfa543) did not draw in the
// benchmark: they keep the camera's motion until a capture shows them.
// The outputs are r2motionvs.h's (its pixel shader turns the two clip positions into the motion vector).
#pragma once

static const char r2wind_hlsl[] =
    "cbuffer motion : register(b0) { float2 size; float dt; float cull_px; };\n"   // as r2motionvs.h
    "cbuffer game_camera : register(b5) { float4 cam[27]; };\n"   // byte copies of the game's camera_VS_PS block (this frame)
    "cbuffer prev_game_camera : register(b6) { float4 pcam[27]; };\n"   // and the previous frame's
    "cbuffer wind : register(b7) { float4 wind0; float4 wind1; };\n"    // a copy of the game's wind_VS block
    "struct VOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1;\n"
    "#ifdef OWN\n"
    "  float4 own : TEXCOORD2;\n"
    "#endif\n"
    "  float clipd : SV_CullDistance0; };\n"   // as r2motionvs.h (the pixel shader is its)
    "float4 clip(float4 p, float4 r0, float4 r1, float4 r2, float4 r3) { return float4(dot(p, r0), dot(p, r1), dot(p, r2), dot(p, r3)); }\n"
    // render pixels per metre at o (an object reaching r metres towards the camera; near the camera: never skipped)
    "float px_per_m(float3 o, float r, float4 m0, float4 m1, float4 m3) {\n"
    "  float w = dot(float4(o, 1), m3) - r;\n"
    "  if (w <= 0.5) return 1e9;\n"
    "  return 0.5 * max(size.x * length(m0.xyz), size.y * length(m1.xyz)) / w; }\n"
    // an instance whose own motion this frame stays under cull_px: every vertex behind the camera, nothing drawn - its
    // pixels keep the camera's motion from depth, which is then right within cull_px
    "VOut culled() { VOut o; o.pos = float4(0, 0, 0, -1); o.cur = o.pos; o.prev = o.pos; o.clipd = -1;\n"
    "#ifdef OWN\n"
    "  o.own = o.pos;\n"
    "#endif\n"
    "  return o; }\n"
    // a tree's sway between the two frames' times, at most: the side sway exactly (tree_sway's r0.x, the same for the whole
    // instance, times weight^2 <= 1); the up-down part sin(6 (x + t)) s(t) |wind0.w| (saturate(wt - 0.7) 10/3 <= 1) with
    // s = sin(0.3 (2 ph + t)) wind1.y the same for the instance: at most 2 |sin(3 dt)| |s(t)| + |s(t) - s(tp)|, times
    // |wind0.w|; instance origin in TEXCOORD0..2 .w, trees up to 30 m towards the camera
    // a vertex's own motion in render pixels: its last pose and place (q) and this frame's (c), both through this frame's
    // camera; the cull distance below drops a triangle whose three corners all moved less than cull_px (its pixels keep the
    // camera's motion, right within cull_px: they moved less than the corners; a clip distance would cut triangles and their depth
    // would no longer equal the game's)
    "float own_px(float4 q, float4 c) {\n"
    "  if (q.w <= 0 || c.w <= 0) return 1e9;\n"
    "  return length((q.xy / q.w - c.xy / c.w) * 0.5 * size); }\n"
    "float tree_h(float t, float ph) { return (sin(mad(ph, 4.0, t)) * sin(t * 3.0) * wind1.x * 0.5 + 1.0) * (sin(t * 0.5) * 0.5 + 0.75) * wind1.x; }\n"
    "bool tree_still(float4 a0, float4 a1, float4 a2, float ph) {\n"
    "  if (cull_px <= 0) return false;\n"
    "  float t = cam[26].x, tp = pcam[26].x;\n"
    "  float s0 = sin(mad(ph, 2.0, t) * 0.3) * wind1.y, s1 = sin(mad(ph, 2.0, tp) * 0.3) * wind1.y;\n"
    "  float m = (abs(tree_h(t, ph) - tree_h(tp, ph)) * length(wind0.xy) + 2.0 * abs(sin(3.0 * (t - tp))) * abs(s0) + abs(s0 - s1)) * abs(wind0.w);\n"
    "  return m * px_per_m(float3(a0.w, a1.w, a2.w), 30.0, cam[9], cam[10], cam[12]) < cull_px; }\n"
    // a plant: side sway 0.5 x 2 max(wind0.w, 0.1) x |wind0.xy| x wind1.x^2 / 2 x a rate up to 4, up-down
    // 7/3 x 2 max(wind0.w, 0.1) x 0.3 x |wind1.y| x a rate up to 5.3 (plant_at; COLOR0.w <= 1); 3 m towards the camera
    "bool plant_still(float4 r0, float4 r1, float4 r2) {\n"
    "  if (cull_px <= 0) return false;\n"
    "  float h = abs(cam[26].x - pcam[26].x), s = max(wind0.w, 0.1);\n"
    "  float m = 2.0 * s * length(wind0.xy) * wind1.x * wind1.x * h + 7.42 * s * abs(wind1.y) * h;\n"
    "  return m * px_per_m(float3(r0.w, r1.w, r2.w), 3.0, cam[9], cam[10], cam[12]) < cull_px; }\n"

    // the trees' sway added to the blended position b (xyz) for weight wt (POSITION7.w) and phase ph (TEXCOORD3.x)
    "float3 tree_sway(float t, float4 b, float wt, float ph) {\n"
    "  precise float4 r0; precise float r2;\n"
    "  r0.x = mad(ph, 4.0, t);\n"
    "  r0.yz = t * float2(3.0, 0.5);\n"
    "  r0.xyz = sin(r0.xyz);\n"
    "  r0.x = r0.y * r0.x;\n"
    "  r0.y = mad(r0.z, 0.5, 0.75);\n"
    "  r0.x = r0.x * wind1.x;\n"
    "  r0.x = mad(r0.x, 0.5, 1.0);\n"
    "  r0.x = r0.y * r0.x;\n"
    "  r0.x = r0.x * wind1.x;\n"
    "  r0.y = wt * wt;\n"                           // Rome 2: the weight squared, then the strength (Attila: wt * s * wt)
    "  r0.y = r0.y * wind0.w;\n"
    "  r0.yz = r0.yy * wind0.xy;\n"
    "  r0.xz = r0.xx * r0.yz;\n"
    "  r0.y = 0;\n"
    "  r0.xyz = r0.xyz + b.xyz;\n"
    "  r0.w = b.x + t;\n"
    "  r0.w = r0.w * 6.0;\n"
    "  r0.w = sin(r0.w);\n"
    "  r2 = mad(ph, 2.0, t);\n"
    "  r2 = r2 * asfloat(0x3e99999a);\n"           // 0.3
    "  r2 = sin(r2);\n"
    "  r2 = r2 * wind1.y;\n"
    "  r0.w = r0.w * r2;\n"
    "  r2 = saturate(wt + asfloat(0xbf333333));\n"  // - 0.7
    "  r2 = r2 * wind0.w;\n"
    "  r0.w = r0.w * r2;\n"
    "  return mad(r0.www, float3(0, asfloat(0x40555555), 0), r0.xyz); }\n"   // 10/3
    "struct TreeIn { float4 p0 : POSITION0; float4 p1 : POSITION1; float4 p6 : POSITION6; float4 p7 : POSITION7;\n"
    "  float4 a0 : TEXCOORD0; float4 a1 : TEXCOORD1; float4 a2 : TEXCOORD2; float4 ph : TEXCOORD3; float4 b0 : TEXCOORD4;\n"
    "  float4 b1 : TEXCOORD5; float4 b2 : TEXCOORD6; };\n"
    "VOut tree_out(float4 r1, float wt, float ph) {\n"
    "  VOut o;\n"
    "  o.cur = clip(float4(tree_sway(cam[26].x, r1, wt, ph), r1.w), cam[9], cam[10], cam[11], cam[12]);\n"
    "  o.prev = clip(float4(tree_sway(pcam[26].x, r1, wt, ph), r1.w), pcam[9], pcam[10], pcam[11], pcam[12]);\n"
    "  o.clipd = cull_px < 0 ? -1 : 1;\n"
    "  if (cull_px > 0) o.clipd = own_px(clip(float4(tree_sway(pcam[26].x, r1, wt, ph), r1.w), cam[9], cam[10], cam[11], cam[12]), o.cur) - cull_px;\n"
    "#ifdef OWN\n"
    "  o.own = clip(float4(tree_sway(pcam[26].x, r1, wt, ph), r1.w), cam[9], cam[10], cam[11], cam[12]);\n"
    "#endif\n"
    "  o.pos = o.cur; return o; }\n"
    "VOut tree_a(TreeIn v) {\n"
    "  if (tree_still(v.a0, v.a1, v.a2, v.ph.x)) return culled();\n"
    "  precise float4 r1, r2;\n"
    "  r1 = float4(dot(v.b0, v.p1), dot(v.b1, v.p1), dot(v.b2, v.p1), v.p1.w);\n"
    "  r1 = r1 * v.p6.y;\n"
    "  r2 = float4(dot(v.a0, v.p0), dot(v.a1, v.p0), dot(v.a2, v.p0), v.p0.w);\n"
    "  r1 = mad(r2, v.p6.x, r1);\n"
    "  return tree_out(r1, v.p7.w, v.ph.x); }\n"
    "VOut tree_b(TreeIn v) {\n"
    "  if (tree_still(v.a0, v.a1, v.a2, v.ph.x)) return culled();\n"
    "  precise float4 r1, r2; precise float f;\n"
    "  f = saturate(0 < v.p6.y && v.p6.y < 1 ? 1 - v.p6.y : v.p6.y);\n"
    "  r1 = float4(dot(v.b0, v.p1), dot(v.b1, v.p1), dot(v.b2, v.p1), v.p1.w);\n"
    "  r2 = float4(dot(v.a0, v.p0), dot(v.a1, v.p0), dot(v.a2, v.p0), v.p0.w);\n"
    "  r1 = r1 - r2;\n"
    "  r1 = mad(f, r1, r2);\n"
    "  return tree_out(r1, v.p7.w, v.ph.x); }\n"
    "struct PlantIn { float4 pos : POSITION0; float4 col : COLOR0; float4 r0 : TEXCOORD1; float4 r1 : TEXCOORD2; float4 r2 : TEXCOORD3; };\n"
    // the plant at time t (vs_1d3a911d5b740853's position path)
    "float4 plant_at(PlantIn v, float t) {\n"
    "  precise float4 r0, r1, r2;\n"
    "  r0.x = saturate(v.col.w + asfloat(0xbf333333));\n"   // - 0.7
    "  r0.y = max(wind0.w, asfloat(0x3dcccccd));\n"         // 0.1
    "  r0.y = r0.y + r0.y;\n"
    "  r0.x = r0.y * r0.x;\n"
    "  r0.y = r0.y * v.col.w;\n"
    "  r0.yz = r0.yy * wind0.xy;\n"
    "  r1.z = dot(v.r2, v.pos);\n"
    "  r1.x = dot(v.r0, v.pos);\n"
    "  r2.xy = r1.xz * 0.25;\n"
    "  r2.xy = sin(r2.xy);\n"
    "  r0.w = mad(r2.x, r2.y, t);\n"
    "  r1.w = r0.w * asfloat(0x3e99999a);\n"                // 0.3
    "  r0.w = sin(r0.w);\n"
    "  r1.w = sin(r1.w);\n"
    "  r1.w = r1.w * wind1.y;\n"
    "  r2.x = r1.x + t;\n"
    "  r2.x = r2.x * 5.0;\n"
    "  r2.x = cos(r2.x);\n"
    "  r1.w = r1.w * r2.x;\n"
    "  r0.x = r0.x * r1.w;\n"
    "  r1.w = t * 3.0;\n"
    "  r1.w = sin(r1.w);\n"
    "  r0.w = r0.w * r1.w;\n"
    "  r0.w = r0.w * wind1.x;\n"
    "  r0.w = mad(r0.w, 0.5, 0.5);\n"
    "  r0.w = r0.w * wind1.x;\n"
    "  r0.yz = r0.ww * r0.yz;\n"
    "  r2.xz = r0.yz * 0.5;\n"
    "  r2.y = 0;\n"
    "  r0.xyz = mad(r0.xxx, float3(0, asfloat(0x40155555), 0), r2.xyz);\n"   // 7/3
    "  r1.y = dot(v.r1, v.pos);\n"
    "  r2.xyz = r0.xyz + r1.xyz;\n"
    "  r2.w = v.pos.w;\n"
    "  return r2; }\n"
    "VOut plant(PlantIn v) {\n"
    "  if (plant_still(v.r0, v.r1, v.r2)) return culled();\n"
    "  VOut o; float4 p = plant_at(v, cam[26].x), q = plant_at(v, pcam[26].x);\n"
    "  o.cur = clip(p, cam[9], cam[10], cam[11], cam[12]);\n"
    "  o.prev = clip(q, pcam[9], pcam[10], pcam[11], pcam[12]);\n"
    "  o.clipd = cull_px > 0 ? own_px(clip(q, cam[9], cam[10], cam[11], cam[12]), o.cur) - cull_px : cull_px < 0 ? -1 : 1;\n"
    "#ifdef OWN\n"
    "  o.own = clip(q, cam[9], cam[10], cam[11], cam[12]);\n"
    "#endif\n"
    "  o.pos = o.cur; return o; }\n";
