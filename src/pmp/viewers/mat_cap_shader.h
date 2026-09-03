// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

// clang-format off

// mat-cap shader: assume view=(0,0,-1), then the tex-coord for
// spherical environment mapping is just the normal's XY
// scaled by 0.5 and shifted by 0.5.
// scale by 0.49 to avoid artifacts at gracing angles
// (requires shader_uniforms_wgsl from phong_shader.h to be prepended)
static const char* matcap_shader_wgsl = R"wgsl(
struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) normal : vec3<f32>,
};

@vertex
fn vs_main(in : VertexIn) -> VertexOut {
    var out : VertexOut;
    out.normal   = normalize(u.normal_matrix * in.normal);
    out.position = u.modelview_projection_matrix * vec4<f32>(in.position, 1.0);
    return out;
}

@fragment
fn fs_main(in : VertexOut, @builtin(front_facing) front_facing : bool) -> @location(0) vec4<f32> {
    var n = normalize(in.normal);
    if (!front_facing) { n = -n; }
    let uv = n.xy * 0.49 + 0.5;
    var rgba = textureSample(tex, tex_sampler, uv);
    if (!front_facing) {
        // damp color of back faces
        rgba = vec4<f32>(rgba.rgb * 0.5, rgba.a);
    }
    return vec4<f32>(rgba.rgb, rgba.a * u.alpha);
}
)wgsl";

// clang-format on
