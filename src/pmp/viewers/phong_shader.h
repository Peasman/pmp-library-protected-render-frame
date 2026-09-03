// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

// clang-format off

// Uniform block shared by all mesh shaders. The C++ mirror is
// Renderer::Uniforms in renderer.h; both must stay in sync (240 bytes).
static const char* shader_uniforms_wgsl = R"wgsl(
struct Uniforms {
    modelview_projection_matrix : mat4x4<f32>,
    modelview_matrix            : mat4x4<f32>,
    normal_matrix               : mat3x3<f32>,
    front_color                 : vec3<f32>,
    ambient                     : f32,
    back_color                  : vec3<f32>,
    diffuse                     : f32,
    specular                    : f32,
    shininess                   : f32,
    point_size                  : f32,
    alpha                       : f32,
    viewport                    : vec2<f32>,
    flags                       : u32,
    _pad                        : u32,
};

const FLAG_LIGHTING       : u32 = 1u;
const FLAG_TEXTURE        : u32 = 2u;
const FLAG_SRGB           : u32 = 4u;
const FLAG_VERTEX_COLOR   : u32 = 8u;
const FLAG_ROUND_POINTS   : u32 = 16u;
const FLAG_TEXTURE_LAYOUT : u32 = 32u;

@group(0) @binding(0) var<uniform> u : Uniforms;
@group(0) @binding(1) var tex_sampler : sampler;
@group(0) @binding(2) var tex : texture_2d<f32>;

fn has_flag(f : u32) -> bool { return (u.flags & f) != 0u; }

struct VertexIn {
    @location(0) position : vec3<f32>,
    @location(1) normal   : vec3<f32>,
    @location(2) tex      : vec2<f32>,
    @location(3) color    : vec3<f32>,
};
)wgsl";

static const char* phong_shader_wgsl = R"wgsl(
struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) normal   : vec3<f32>,
    @location(1) tex      : vec2<f32>,
    @location(2) view     : vec3<f32>,
    @location(3) color    : vec3<f32>,
    @location(4) point_uv : vec2<f32>,
};

fn transform(in : VertexIn) -> VertexOut {
    var out : VertexOut;
    var pos = vec4<f32>(in.position, 1.0);
    if (has_flag(FLAG_TEXTURE_LAYOUT)) {
        pos = vec4<f32>(in.tex, 0.0, 1.0);
    }
    out.normal   = u.normal_matrix * in.normal;
    out.tex      = in.tex;
    out.view     = -(u.modelview_matrix * pos).xyz;
    out.color    = in.color;
    out.position = u.modelview_projection_matrix * pos;
    out.point_uv = vec2<f32>(0.0, 0.0);
    return out;
}

// triangles and lines
@vertex
fn vs_main(in : VertexIn) -> VertexOut {
    return transform(in);
}

// points: one instance per point, expanded to a screen-aligned quad of
// point_size pixels (WebGPU has no point size / point sprites)
@vertex
fn vs_point(in : VertexIn, @builtin(vertex_index) vid : u32) -> VertexOut {
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, 1.0), vec2<f32>(-1.0, 1.0));
    var out = transform(in);
    let c = corners[vid];
    let offset = c * u.point_size / u.viewport;
    out.position = vec4<f32>(out.position.xy + offset * out.position.w,
                             out.position.zw);
    out.point_uv = c;
    return out;
}

@fragment
fn fs_main(in : VertexOut, @builtin(front_facing) front_facing : bool) -> @location(0) vec4<f32> {
    // round points
    if (has_flag(FLAG_ROUND_POINTS) && dot(in.point_uv, in.point_uv) > 1.0) {
        discard;
    }

    var color : vec3<f32>;
    if (has_flag(FLAG_VERTEX_COLOR)) {
        color = in.color;
    } else if (front_facing) {
        color = u.front_color;
    } else {
        color = u.back_color;
    }

    var rgb : vec3<f32>;

    if (has_flag(FLAG_LIGHTING)) {
        let L1 = normalize(vec3<f32>( 1.0, 1.0, 1.0));
        let L2 = normalize(vec3<f32>(-1.0, 1.0, 1.0));
        let V  = normalize(in.view);
        var N  = normalize(in.normal);
        if (!front_facing) { N = -N; }

        rgb = u.ambient * 0.1 * color;

        var NL = dot(N, L1);
        if (NL > 0.0) {
            rgb += u.diffuse * NL * color;
            let R  = normalize(-reflect(L1, N));
            let RV = dot(R, V);
            if (RV > 0.0) {
                rgb += vec3<f32>(u.specular * pow(RV, u.shininess));
            }
        }

        NL = dot(N, L2);
        if (NL > 0.0) {
            rgb += u.diffuse * NL * color;
            let R  = normalize(-reflect(L2, N));
            let RV = dot(R, V);
            if (RV > 0.0) {
                rgb += vec3<f32>(u.specular * pow(RV, u.shininess));
            }
        }
    } else {
        rgb = color;
    }

    if (has_flag(FLAG_TEXTURE)) {
        rgb *= textureSample(tex, tex_sampler, in.tex).xyz;
    }
    if (has_flag(FLAG_SRGB)) {
        rgb = pow(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(0.45));
    }

    return vec4<f32>(rgb, u.alpha);
}
)wgsl";

// clang-format on
