// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "lasso_drawable.h"
#include "lasso_shader.h"
#include "pmp/mat_vec.h"

namespace pmp {

LassoDrawable::LassoDrawable()
{
    auto& ctx = GpuContext::get();

    shader_ = ctx.create_shader_module(lasso_shader_wgsl, "lasso");

    // uniform buffer holding the color
    const float color[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    uniform_buffer_ =
        ctx.create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                          sizeof(color), color, "lasso color");

    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = sizeof(color);

    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.entryCount = 1;
    bgl_desc.entries = &entry;
    bind_group_layout_ =
        wgpuDeviceCreateBindGroupLayout(ctx.device(), &bgl_desc);

    WGPUBindGroupEntry bg_entry = WGPU_BIND_GROUP_ENTRY_INIT;
    bg_entry.binding = 0;
    bg_entry.buffer = uniform_buffer_;
    bg_entry.size = sizeof(color);

    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = bind_group_layout_;
    bg_desc.entryCount = 1;
    bg_desc.entries = &bg_entry;
    bind_group_ = wgpuDeviceCreateBindGroup(ctx.device(), &bg_desc);

    WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &bind_group_layout_;
    pipeline_layout_ = wgpuDeviceCreatePipelineLayout(ctx.device(), &pl_desc);
}

LassoDrawable::~LassoDrawable()
{
    for (auto& [key, p] : pipelines_)
        wgpuRenderPipelineRelease(p);
    if (pipeline_layout_)
        wgpuPipelineLayoutRelease(pipeline_layout_);
    if (bind_group_)
        wgpuBindGroupRelease(bind_group_);
    if (bind_group_layout_)
        wgpuBindGroupLayoutRelease(bind_group_layout_);
    if (shader_)
        wgpuShaderModuleRelease(shader_);
    if (uniform_buffer_)
        wgpuBufferRelease(uniform_buffer_);
    if (vertex_buffer_)
        wgpuBufferRelease(vertex_buffer_);
}

WGPURenderPipeline LassoDrawable::pipeline()
{
    auto& ctx = GpuContext::get();
    const auto key =
        std::make_pair(ctx.pass_sample_count(), (int)ctx.pass_color_format());
    auto it = pipelines_.find(key);
    if (it != pipelines_.end())
        return it->second;

    WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
    attribute.format = WGPUVertexFormat_Float32x2;
    attribute.offset = 0;
    attribute.shaderLocation = 0;

    WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    buffer.stepMode = WGPUVertexStepMode_Vertex;
    buffer.arrayStride = sizeof(vec2);
    buffer.attributeCount = 1;
    buffer.attributes = &attribute;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = GpuContext::str("lasso");
    desc.layout = pipeline_layout_;
    desc.vertex.module = shader_;
    desc.vertex.entryPoint = GpuContext::str("vs_main");
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &buffer;
    desc.primitive.topology = WGPUPrimitiveTopology_LineStrip;
    desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;

    // overlay: no depth test, no depth write
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = GpuContext::depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;
    desc.depthStencil = &depth;

    desc.multisample.count = ctx.pass_sample_count();
    desc.multisample.mask = ~0u;

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = ctx.pass_color_format();
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader_;
    fragment.entryPoint = GpuContext::str("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;
    desc.fragment = &fragment;

    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(ctx.device(), &desc);
    pipelines_[key] = p;
    return p;
}

void LassoDrawable::update_lasso(const std::vector<ivec2>& lasso_points_screen)
{
    lasso_points_ndc_.clear();

    // get viewport data
    const auto& viewport = GpuContext::get().viewport();

    // screen (x,y) to ndc
    for (const auto& p : lasso_points_screen)
    {
        float x_ndc = (2.0f * (p[0] - viewport[0])) / viewport[2] - 1.0f;
        float y_ndc =
            (2.0f * (viewport[3] - (p[1] - viewport[1]))) / viewport[3] - 1.0f;
        lasso_points_ndc_.emplace_back(x_ndc, y_ndc);
    }

    // Close the lasso by adding the first point at the end
    if (!lasso_points_ndc_.empty())
    {
        lasso_points_ndc_.push_back(lasso_points_ndc_.front());
    }

    update_buffers();
}

void LassoDrawable::update_buffers()
{
    if (lasso_points_ndc_.empty())
        return;

    auto& ctx = GpuContext::get();
    const uint64_t bytes = lasso_points_ndc_.size() * sizeof(vec2);

    // grow buffer if needed, otherwise just overwrite
    if (!vertex_buffer_ || vertex_buffer_size_ < bytes)
    {
        if (vertex_buffer_)
            wgpuBufferRelease(vertex_buffer_);
        vertex_buffer_size_ = std::max<uint64_t>(bytes, 64 * sizeof(vec2));
        vertex_buffer_ =
            ctx.create_buffer(WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                              vertex_buffer_size_, nullptr, "lasso points");
    }
    wgpuQueueWriteBuffer(ctx.queue(), vertex_buffer_, 0,
                         lasso_points_ndc_.data(), (size_t)bytes);
}

void LassoDrawable::draw(const mat4&, const mat4&)
{
    auto& ctx = GpuContext::get();
    WGPURenderPassEncoder pass = ctx.pass();

    // nothing to draw, or depth-only pass (picking)
    if (lasso_points_ndc_.empty() || !pass || !vertex_buffer_ ||
        ctx.pass_color_format() == WGPUTextureFormat_Undefined)
        return;

    wgpuRenderPassEncoderSetPipeline(pass, pipeline());
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 0, vertex_buffer_, 0, lasso_points_ndc_.size() * sizeof(vec2));
    wgpuRenderPassEncoderDraw(pass, lasso_points_ndc_.size(), 1, 0, 0);
}

} // namespace pmp
