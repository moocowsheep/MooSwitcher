/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

// Shared YCbCr helpers: limited-range BT.709/601 conversion and sampling of
// UYVY frames stored as RGBA8 textures at (width/2, height) -- one texel per
// macropixel: r=U g=Y0 b=V a=Y1. All filtering is done manually because the
// packed layout must not be linearly filtered across channels.

vec3 ycbcr_to_rgb(float Y, float Cb, float Cr, int cm) {
    float c = (Y * 255.0 - 16.0) / 219.0;
    float d = (Cb * 255.0 - 128.0) / 224.0;
    float e = (Cr * 255.0 - 128.0) / 224.0;
    vec3 rgb;
    if (cm == 1)  // BT.601
        rgb = vec3(c + 1.402 * e, c - 0.344136 * d - 0.714136 * e, c + 1.772 * d);
    else          // BT.709
        rgb = vec3(c + 1.5748 * e, c - 0.18732 * d - 0.46812 * e, c + 1.8556 * d);
    return clamp(rgb, 0.0, 1.0);
}

// Fetch one full-res pixel (luma coordinates) from a packed UYVY texture.
vec3 uyvy_fetch(sampler2D t, ivec2 fullSize, ivec2 xy, int cm) {
    xy = clamp(xy, ivec2(0), fullSize - 1);
    vec4 mp = texelFetch(t, ivec2(xy.x >> 1, xy.y), 0);
    float Y = ((xy.x & 1) == 1) ? mp.a : mp.g;
    return ycbcr_to_rgb(Y, mp.r, mp.b, cm);
}

// NV12: per-plane textures filter correctly in hardware.
vec3 nv12_sample(sampler2D yTex, sampler2D uvTex, vec2 uv, int cm) {
    float Y = texture(yTex, uv).r;
    vec2 C = texture(uvTex, uv).rg;
    return ycbcr_to_rgb(Y, C.x, C.y, cm);
}

vec3 rgb_to_ycbcr(vec3 rgb, int cm) {
    float y, cb, cr;
    if (cm == 1) {  // BT.601
        y = dot(rgb, vec3(0.299, 0.587, 0.114));
        cb = (rgb.b - y) / 1.772;
        cr = (rgb.r - y) / 1.402;
    } else {        // BT.709
        y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        cb = (rgb.b - y) / 1.8556;
        cr = (rgb.r - y) / 1.5748;
    }
    return vec3(y, cb, cr);
}

// Manual bilinear in RGB space (converts 4 taps, then filters).
vec3 uyvy_sample_bilinear(sampler2D t, ivec2 fullSize, vec2 uv, int cm) {
    vec2 p = uv * vec2(fullSize) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    vec3 c00 = uyvy_fetch(t, fullSize, i0, cm);
    vec3 c10 = uyvy_fetch(t, fullSize, i0 + ivec2(1, 0), cm);
    vec3 c01 = uyvy_fetch(t, fullSize, i0 + ivec2(0, 1), cm);
    vec3 c11 = uyvy_fetch(t, fullSize, i0 + ivec2(1, 1), cm);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}
