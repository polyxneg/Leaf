// Copyright 2022-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;

// Anaglyph Red-Cyan shader based on Dubois algorithm
// Constants taken from the paper:
// "Conversion of a Stereo Pair to Anaglyph with
// the Least-Squares Projection Method"
// Eric Dubois, March 2009
const mat3 l = mat3( 0.437, 0.449, 0.164,
              -0.062,-0.062,-0.024,
              -0.048,-0.050,-0.017);
const mat3 r = mat3(-0.011,-0.032,-0.007,
               0.377, 0.761, 0.009,
              -0.026,-0.093, 1.234);

layout (push_constant, std140) uniform DrawInfo {
    mat4 modelview_matrix;
    vec4 i_resolution;
    vec4 o_resolution;
    int screen_id_l;
    int screen_id_r;
    int layer;
    int reverse_interlaced;
    vec4 leaf_params;
};

layout (set = 0, binding = 0) uniform sampler2D screen_textures[3];

vec4 GetScreen(int screen_id) {
#ifdef ARRAY_DYNAMIC_INDEX
    return texture(screen_textures[screen_id], frag_tex_coord);
#else
    switch (screen_id) {
    case 0:
        return texture(screen_textures[0], frag_tex_coord);
    case 1:
        return texture(screen_textures[1], frag_tex_coord);
    case 2:
        return texture(screen_textures[2], frag_tex_coord);
    }
#endif
}

// Leaf: x = opacity, y = minimum alpha kept after keying (0 = keying off), z = softness of that
// threshold, w = scrim alpha (keyed areas darken the game behind instead of vanishing).
// Bottom-screen backgrounds are cleared/patched to magenta. Every texel is treated as
// "colour C at alpha a over magenta": the magenta share is min(r, b) - g, so a and C can be
// recovered exactly. Texels are keyed *before* filtering so edges never mix with the key.
const vec3 LEAF_KEY = vec3(1.0, 0.0, 1.0);
// What the game's lavender bubble rims un-mix to: a blue-grey 3D edge meant for the wallpaper.
// On the main screen it becomes a soft neutral drop shadow instead.
const vec3 LEAF_RIM = vec3(0.54, 0.54, 0.885);

vec4 LeafKeyTexel(vec3 c) {
    float a = 1.0 - clamp(min(c.r, c.b) - c.g, 0.0, 1.0);
    vec3 col = clamp((c - (1.0 - a) * LEAF_KEY) / max(a, 0.001), 0.0, 1.0);
    float rim = 1.0 - smoothstep(0.06, 0.14, distance(col, LEAF_RIM));
    col = mix(col, vec3(0.06, 0.05, 0.09), rim);
    a *= mix(1.0, 0.4, rim);
    a *= smoothstep(leaf_params.y, leaf_params.y + leaf_params.z, a); // drop only faint texels
    return vec4(col * a, a); // premultiplied
}

// Composite "C over (scrim over game)" as a single straight-alpha blend.
vec4 LeafComposite(vec4 premul) {
    float a = premul.a;
    vec3 col = a > 0.0001 ? premul.rgb / a : vec3(0.0);
    float total = 1.0 - (1.0 - a) * (1.0 - leaf_params.w);
    return vec4(col * a / max(total, 0.001), total * leaf_params.x);
}

vec4 LeafApplyKey(vec4 c) {
    if (leaf_params.y < 0.0) // backdrop: a solid dimming layer, no texture
        return vec4(0.0, 0.0, 0.0, leaf_params.w * leaf_params.x);
    return LeafComposite(leaf_params.y > 0.0 ? LeafKeyTexel(c.rgb) : vec4(c.rgb, 1.0));
}

void main() {
    vec4 color_tex_l = GetScreen(screen_id_l);
    vec4 color_tex_r = GetScreen(screen_id_r);
    color = vec4(color_tex_l.rgb*l+color_tex_r.rgb*r, color_tex_l.a);
    color = LeafApplyKey(color);
}
