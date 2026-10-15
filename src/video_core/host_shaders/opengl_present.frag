// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

//? #version 430 core

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D color_texture;

uniform vec4 i_resolution;
uniform vec4 o_resolution;
uniform int layer;
uniform vec4 leaf_params;

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

// Bilinear filtering of individually keyed texels.
vec4 LeafSampleKeyed() {
    ivec2 size = textureSize(color_texture, 0);
    ivec2 hi = size - ivec2(1);
    vec2 p = frag_tex_coord * vec2(size) - 0.5;
    vec2 f = fract(p);
    ivec2 i = ivec2(floor(p));
    vec4 c00 = LeafKeyTexel(texelFetch(color_texture, clamp(i, ivec2(0), hi), 0).rgb);
    vec4 c10 = LeafKeyTexel(texelFetch(color_texture, clamp(i + ivec2(1, 0), ivec2(0), hi), 0).rgb);
    vec4 c01 = LeafKeyTexel(texelFetch(color_texture, clamp(i + ivec2(0, 1), ivec2(0), hi), 0).rgb);
    vec4 c11 = LeafKeyTexel(texelFetch(color_texture, clamp(i + ivec2(1, 1), ivec2(0), hi), 0).rgb);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

void main() {
    if (leaf_params.y < 0.0) {
        color = vec4(0.0, 0.0, 0.0, leaf_params.w * leaf_params.x);
    } else if (leaf_params.y > 0.0) {
        color = LeafComposite(LeafSampleKeyed());
    } else {
        color = LeafApplyKey(texture(color_texture, frag_tex_coord));
    }
}
