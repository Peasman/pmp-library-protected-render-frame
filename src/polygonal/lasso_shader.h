// Copyright 2025-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

// clang-format off

// lasso overlay: points are given in normalized device coordinates
static const char* lasso_shader_wgsl = R"wgsl(
struct Uniforms {
    color : vec4<f32>,
};
@group(0) @binding(0) var<uniform> u : Uniforms;

@vertex
fn vs_main(@location(0) position : vec2<f32>) -> @builtin(position) vec4<f32> {
    return vec4<f32>(position, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return u.color;
}
)wgsl";

// clang-format on
