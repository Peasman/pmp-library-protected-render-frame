// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <vector>
#include "pmp/viewers/drawable.h"
#include "pmp/viewers/gpu.h"
#include "pmp/mat_vec.h"

namespace pmp {

// Class for rendering a lasso (selection path) overlay
class LassoDrawable : public Drawable
{
public:
    LassoDrawable();
    ~LassoDrawable() override;

    void update_lasso(const std::vector<ivec2>& lasso_points_screen);
    void update_buffers() override;
    void draw(const mat4& projection, const mat4& modelview) override;

private:
    // create or fetch pipeline matching the current render pass
    WGPURenderPipeline pipeline();

    // GPU resources
    WGPUBuffer vertex_buffer_{nullptr};
    uint64_t vertex_buffer_size_{0};
    WGPUBuffer uniform_buffer_{nullptr};
    WGPUShaderModule shader_{nullptr};
    WGPUBindGroupLayout bind_group_layout_{nullptr};
    WGPUBindGroup bind_group_{nullptr};
    WGPUPipelineLayout pipeline_layout_{nullptr};
    std::map<std::pair<uint32_t, int>, WGPURenderPipeline> pipelines_;

    // lasso data
    std::vector<vec2> lasso_points_ndc_;
};

} // namespace pmp
