// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/viewers/renderer.h"

#include <stb_image.h>

#include "pmp/exceptions.h"
#include "pmp/surface_mesh.h"
#include "pmp/viewers/phong_shader.h"
#include "pmp/viewers/mat_cap_shader.h"
#include "pmp/viewers/cold_warm_texture.h"
#include "pmp/algorithms/normals.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <numbers>

namespace pmp {

namespace {

constexpr uint32_t uniform_slot_size = 256; // minUniformBufferOffsetAlignment
constexpr uint32_t initial_uniform_slots = 64;

template <typename T>
void release(T& handle, void (*fn)(T))
{
    if (handle)
    {
        fn(handle);
        handle = nullptr;
    }
}

WGPUAddressMode to_wgpu(TextureWrap wrap)
{
    switch (wrap)
    {
        case TextureWrap::Repeat:
            return WGPUAddressMode_Repeat;
        case TextureWrap::MirroredRepeat:
            return WGPUAddressMode_MirrorRepeat;
        default:
            return WGPUAddressMode_ClampToEdge;
    }
}

WGPUFilterMode to_wgpu(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? WGPUFilterMode_Nearest
                                            : WGPUFilterMode_Linear;
}

// box-filter downsampling of an RGBA8 image by factor two
std::vector<unsigned char> downsample(const unsigned char* src, uint32_t w,
                                      uint32_t h, uint32_t& ow, uint32_t& oh)
{
    ow = std::max(1u, w / 2);
    oh = std::max(1u, h / 2);
    std::vector<unsigned char> dst((size_t)ow * oh * 4);
    for (uint32_t y = 0; y < oh; ++y)
    {
        const uint32_t y0 = std::min(2 * y, h - 1);
        const uint32_t y1 = std::min(2 * y + 1, h - 1);
        for (uint32_t x = 0; x < ow; ++x)
        {
            const uint32_t x0 = std::min(2 * x, w - 1);
            const uint32_t x1 = std::min(2 * x + 1, w - 1);
            for (uint32_t c = 0; c < 4; ++c)
            {
                const unsigned int sum =
                    src[4 * (y0 * w + x0) + c] + src[4 * (y0 * w + x1) + c] +
                    src[4 * (y1 * w + x0) + c] + src[4 * (y1 * w + x1) + c];
                dst[4 * (y * ow + x) + c] = (unsigned char)((sum + 2) / 4);
            }
        }
    }
    return dst;
}

} // namespace

Renderer::Renderer(const SurfaceMesh& mesh) : mesh_(mesh)
{
    // material parameters
    front_color_ = vec3(0.6, 0.6, 0.6);
    back_color_ = vec3(0.5, 0.0, 0.0);
    ambient_ = 0.1;
    diffuse_ = 0.8;
    specular_ = 0.6;
    shininess_ = 100.0;
    alpha_ = 1.0;
    use_srgb_ = false;
    use_colors_ = true;
    crease_angle_ = 180.0;
    point_size_ = 5;

    texture_mode_ = TextureMode::Other;
}

Renderer::~Renderer()
{
    release(vertex_buffer_, wgpuBufferRelease);
    release(normal_buffer_, wgpuBufferRelease);
    release(tex_coord_buffer_, wgpuBufferRelease);
    release(color_buffer_, wgpuBufferRelease);
    release(edge_buffer_, wgpuBufferRelease);
    release(feature_buffer_, wgpuBufferRelease);
    release(selection_position_buffer_, wgpuBufferRelease);
    release(selection_normal_buffer_, wgpuBufferRelease);
    release(uniform_buffer_, wgpuBufferRelease);

    for (auto& [key, p] : pipelines_)
        wgpuRenderPipelineRelease(p);
    pipelines_.clear();

    release(bind_group_, wgpuBindGroupRelease);
    release(pipeline_layout_, wgpuPipelineLayoutRelease);
    release(bind_group_layout_, wgpuBindGroupLayoutRelease);
    release(phong_module_, wgpuShaderModuleRelease);
    release(matcap_module_, wgpuShaderModuleRelease);

    release_texture();
}

void Renderer::init_gpu_resources()
{
    if (phong_module_)
        return;

    auto& ctx = GpuContext::get();

    // shader modules
    const std::string phong_src =
        std::string(shader_uniforms_wgsl) + phong_shader_wgsl;
    const std::string matcap_src =
        std::string(shader_uniforms_wgsl) + matcap_shader_wgsl;
    phong_module_ = ctx.create_shader_module(phong_src.c_str(), "phong");
    matcap_module_ = ctx.create_shader_module(matcap_src.c_str(), "matcap");

    // bind group layout: uniforms (dynamic offset), sampler, texture
    WGPUBindGroupLayoutEntry entries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[0].buffer.minBindingSize = sizeof(Uniforms);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.label = GpuContext::str("renderer");
    bgl_desc.entryCount = 3;
    bgl_desc.entries = entries;
    bind_group_layout_ =
        wgpuDeviceCreateBindGroupLayout(ctx.device(), &bgl_desc);

    WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl_desc.label = GpuContext::str("renderer");
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &bind_group_layout_;
    pipeline_layout_ = wgpuDeviceCreatePipelineLayout(ctx.device(), &pl_desc);

    // uniform ring buffer
    uniform_slots_ = initial_uniform_slots;
    uniform_buffer_ = ctx.create_buffer(
        WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
        (uint64_t)uniform_slots_ * uniform_slot_size, nullptr, "uniforms");
}

WGPURenderPipeline Renderer::pipeline(ShaderKind shader, Primitive primitive,
                                      bool depth_less_equal)
{
    auto& ctx = GpuContext::get();
    const uint32_t sample_count = ctx.pass_sample_count();
    const WGPUTextureFormat color_format = ctx.pass_color_format();

    const auto key =
        std::make_tuple((int)shader, (int)primitive, depth_less_equal,
                        sample_count, (int)color_format);
    auto it = pipelines_.find(key);
    if (it != pipelines_.end())
        return it->second;

    // vertex layout: positions, normals, texcoords, colors in separate buffers
    WGPUVertexAttribute attributes[4] = {
        WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT,
        WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT};
    WGPUVertexBufferLayout buffers[4] = {
        WGPU_VERTEX_BUFFER_LAYOUT_INIT, WGPU_VERTEX_BUFFER_LAYOUT_INIT,
        WGPU_VERTEX_BUFFER_LAYOUT_INIT, WGPU_VERTEX_BUFFER_LAYOUT_INIT};
    const WGPUVertexFormat formats[4] = {
        WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x3,
        WGPUVertexFormat_Float32x2, WGPUVertexFormat_Float32x3};
    const uint64_t strides[4] = {12, 12, 8, 12};
    const WGPUVertexStepMode step = primitive == Primitive::Points
                                        ? WGPUVertexStepMode_Instance
                                        : WGPUVertexStepMode_Vertex;
    for (uint32_t i = 0; i < 4; ++i)
    {
        attributes[i].format = formats[i];
        attributes[i].offset = 0;
        attributes[i].shaderLocation = i;
        buffers[i].stepMode = step;
        buffers[i].arrayStride = strides[i];
        buffers[i].attributeCount = 1;
        buffers[i].attributes = &attributes[i];
    }

    WGPUShaderModule module =
        shader == ShaderKind::Phong ? phong_module_ : matcap_module_;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = GpuContext::str("mesh");
    desc.layout = pipeline_layout_;

    desc.vertex.module = module;
    desc.vertex.entryPoint = GpuContext::str(
        primitive == Primitive::Points ? "vs_point" : "vs_main");
    desc.vertex.bufferCount = 4;
    desc.vertex.buffers = buffers;

    desc.primitive.topology = primitive == Primitive::Lines
                                  ? WGPUPrimitiveTopology_LineList
                                  : WGPUPrimitiveTopology_TriangleList;
    desc.primitive.frontFace = WGPUFrontFace_CCW;
    desc.primitive.cullMode = WGPUCullMode_None;

    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = GpuContext::depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_True;
    depth.depthCompare = depth_less_equal ? WGPUCompareFunction_LessEqual
                                          : WGPUCompareFunction_Less;
    desc.depthStencil = &depth;

    desc.multisample.count = sample_count;
    desc.multisample.mask = ~0u;
    // alpha-to-coverage allows for transparent rendering without sorting
    desc.multisample.alphaToCoverageEnabled = sample_count > 1;

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = color_format;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = GpuContext::str("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;

    // depth-only passes (picking) have no color attachment
    if (color_format != WGPUTextureFormat_Undefined)
        desc.fragment = &fragment;

    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(ctx.device(), &desc);
    pipelines_[key] = p;
    return p;
}

void Renderer::release_texture()
{
    release(texture_view_, wgpuTextureViewRelease);
    release(texture_, wgpuTextureRelease);
    release(sampler_, wgpuSamplerRelease);
}

void Renderer::create_texture(uint32_t width, uint32_t height,
                              const unsigned char* rgba, bool srgb,
                              TextureFilter min_filter,
                              TextureFilter mag_filter, TextureWrap wrap)
{
    init_gpu_resources();
    auto& ctx = GpuContext::get();

    release_texture();

    const bool mipmaps = (min_filter == TextureFilter::LinearMipmapLinear);
    uint32_t levels = 1;
    if (mipmaps)
    {
        uint32_t s = std::max(width, height);
        while (s > 1)
        {
            s /= 2;
            ++levels;
        }
    }

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = GpuContext::str("texture");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width, height, 1};
    desc.format =
        srgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = levels;
    desc.sampleCount = 1;
    texture_ = wgpuDeviceCreateTexture(ctx.device(), &desc);

    // upload level 0 and (CPU-generated) mip levels
    std::vector<unsigned char> level_data;
    const unsigned char* data = rgba;
    uint32_t w = width, h = height;
    for (uint32_t level = 0; level < levels; ++level)
    {
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = texture_;
        dst.mipLevel = level;

        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.offset = 0;
        layout.bytesPerRow = 4 * w;
        layout.rowsPerImage = h;

        WGPUExtent3D extent{w, h, 1};
        wgpuQueueWriteTexture(ctx.queue(), &dst, data, (size_t)4 * w * h,
                              &layout, &extent);

        if (level + 1 < levels)
        {
            uint32_t nw, nh;
            level_data = downsample(data, w, h, nw, nh);
            data = level_data.data();
            w = nw;
            h = nh;
        }
    }

    texture_view_ = wgpuTextureCreateView(texture_, nullptr);

    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.label = GpuContext::str("texture sampler");
    sampler_desc.addressModeU = to_wgpu(wrap);
    sampler_desc.addressModeV = to_wgpu(wrap);
    sampler_desc.addressModeW = to_wgpu(wrap);
    sampler_desc.magFilter = to_wgpu(mag_filter);
    sampler_desc.minFilter = to_wgpu(min_filter);
    sampler_desc.mipmapFilter =
        mipmaps ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sampler_desc.lodMinClamp = 0.0f;
    sampler_desc.lodMaxClamp = 32.0f;
    sampler_desc.maxAnisotropy = 1;
    sampler_ = wgpuDeviceCreateSampler(ctx.device(), &sampler_desc);

    update_bind_group();
}

void Renderer::update_bind_group()
{
    if (!uniform_buffer_ || !texture_view_ || !sampler_)
        return;

    release(bind_group_, wgpuBindGroupRelease);

    WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].buffer = uniform_buffer_;
    entries[0].offset = 0;
    entries[0].size = sizeof(Uniforms);
    entries[1].binding = 1;
    entries[1].sampler = sampler_;
    entries[2].binding = 2;
    entries[2].textureView = texture_view_;

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = GpuContext::str("renderer");
    desc.layout = bind_group_layout_;
    desc.entryCount = 3;
    desc.entries = entries;
    bind_group_ = wgpuDeviceCreateBindGroup(GpuContext::get().device(), &desc);
}

void Renderer::load_texture(const std::filesystem::path& filename,
                            TextureFormat format, TextureFilter min_filter,
                            TextureFilter mag_filter, TextureWrap wrap)
{
    // load with stb_image, always expand to RGBA (WebGPU has no RGB8 format)
    int width, height, n;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* img =
        stbi_load(filename.string().c_str(), &width, &height, &n, 4);
    if (!img)
        throw IOException("Failed to load texture file: " + filename.string());

    // formats without alpha: force opaque
    if (format == TextureFormat::RGB || format == TextureFormat::SRGB)
    {
        for (int i = 0; i < width * height; ++i)
            img[4 * i + 3] = 255;
    }

    const bool srgb =
        (format == TextureFormat::SRGB || format == TextureFormat::SRGBA);

    create_texture(width, height, img, srgb, min_filter, mag_filter, wrap);

    // use SRGB rendering?
    use_srgb_ = srgb;

    stbi_image_free(img);

    texture_mode_ = TextureMode::Other;
}

void Renderer::load_matcap(const std::filesystem::path& filename)
{
    load_texture(filename, TextureFormat::RGBA, TextureFilter::Linear,
                 TextureFilter::Linear, TextureWrap::ClampToEdge);
    texture_mode_ = TextureMode::MatCap;
}

void Renderer::use_cold_warm_texture()
{
    if (texture_mode_ != TextureMode::ColdWarm)
    {
        // expand RGB color map to RGBA
        std::vector<unsigned char> rgba(256 * 4);
        for (int i = 0; i < 256; ++i)
        {
            rgba[4 * i + 0] = cold_warm_texture[3 * i + 0];
            rgba[4 * i + 1] = cold_warm_texture[3 * i + 1];
            rgba[4 * i + 2] = cold_warm_texture[3 * i + 2];
            rgba[4 * i + 3] = 255;
        }
        create_texture(256, 1, rgba.data(), false, TextureFilter::Linear,
                       TextureFilter::Linear, TextureWrap::ClampToEdge);

        use_srgb_ = false;
        texture_mode_ = TextureMode::ColdWarm;
    }
}

void Renderer::use_checkerboard_texture()
{
    if (texture_mode_ != TextureMode::Checkerboard)
    {
        // generate checkerboard-like image
        const unsigned int res = 512;
        std::vector<unsigned char> tex(res * res * 4);
        unsigned char* tp = tex.data(); // NOLINT(misc-const-correctness)
        for (unsigned int x = 0; x < res; ++x)
        {
            for (unsigned int y = 0; y < res; ++y)
            {
                if (((x & 0x20) == 0) ^ ((y & 0x20) == 0))
                {
                    *(tp++) = 42;
                    *(tp++) = 157;
                    *(tp++) = 223;
                }
                else
                {
                    *(tp++) = 255;
                    *(tp++) = 255;
                    *(tp++) = 255;
                }
                *(tp++) = 255;
            }
        }

        create_texture(res, res, tex.data(), false, TextureFilter::Linear,
                       TextureFilter::Linear, TextureWrap::ClampToEdge);

        use_srgb_ = false;
        texture_mode_ = TextureMode::Checkerboard;
    }
}

void Renderer::set_crease_angle(Scalar ca)
{
    if (ca != crease_angle_)
    {
        crease_angle_ = std::max(Scalar(0), std::min(Scalar(180), ca));
        update_buffers();
    }
}

void Renderer::upload(WGPUBuffer& buffer, WGPUBufferUsage usage,
                      const void* data, size_t bytes, const char* label)
{
    release(buffer, wgpuBufferRelease);
    buffer = GpuContext::get().create_buffer(usage | WGPUBufferUsage_CopyDst,
                                             bytes, data, label);
}

void Renderer::update_buffers()
{
    init_gpu_resources();
    buffers_initialized_ = true;

    // get properties
    auto vpos = mesh_.get_vertex_property<Point>("v:point");
    auto vcolor = mesh_.get_vertex_property<Color>("v:color");
    auto vtex = mesh_.get_vertex_property<TexCoord>("v:tex");
    auto htex = mesh_.get_halfedge_property<TexCoord>("h:tex");
    auto fcolor = mesh_.get_face_property<Color>("f:color");

    // index array for remapping vertex indices during duplication
    // note: use vertices_size() instead of n_vertices() to also
    // take deleted vertices into account
    std::vector<size_t> vertex_indices(mesh_.vertices_size());

    // produce arrays of points, normals, and texcoords
    // (duplicate vertices to allow for flat shading)
    std::vector<vec3> position_array;
    std::vector<vec3> color_array;
    std::vector<vec3> normal_array;
    std::vector<vec2> tex_array;
    std::vector<ivec3> triangles;

    // we have a mesh: fill arrays by looping over faces
    if (mesh_.n_faces())
    {
        // reserve memory
        position_array.reserve(3 * mesh_.n_faces());
        normal_array.reserve(3 * mesh_.n_faces());
        if (htex || vtex)
            tex_array.reserve(3 * mesh_.n_faces());

        if ((vcolor || fcolor) && use_colors_)
            color_array.reserve(3 * mesh_.n_faces());

        // precompute normals for easy cases
        std::vector<Normal> face_normals;
        std::vector<Normal> vertex_normals;
        if (crease_angle_ < 1)
        {
            // note: use faces_size() instead of n_faces() to
            // take deleted faces into account
            face_normals.resize(mesh_.faces_size());
            for (auto f : mesh_.faces())
                face_normals[f.idx()] = face_normal(mesh_, f);
        }
        else if (crease_angle_ > 170)
        {
            // note: use vertices_size() instead of n_vertices() to
            // take deleted vertices into account
            vertex_normals.resize(mesh_.vertices_size());
            for (auto v : mesh_.vertices())
                vertex_normals[v.idx()] = vertex_normal(mesh_, v);
        }

        // data per face (for all corners)
        std::vector<Halfedge> corner_halfedges;
        std::vector<Vertex> corner_vertices;
        std::vector<vec3> corner_positions;
        std::vector<vec3> corner_colors;
        std::vector<vec3> corner_normals;
        std::vector<vec2> corner_texcoords;

        // convert from degrees to radians
        const Scalar crease_angle_radians =
            crease_angle_ / 180.0 * std::numbers::pi;

        size_t vidx(0);

        // loop over all faces
        for (auto f : mesh_.faces())
        {
            // collect corner positions and normals
            corner_halfedges.clear();
            corner_vertices.clear();
            corner_positions.clear();
            corner_colors.clear();
            corner_normals.clear();
            corner_texcoords.clear();
            Vertex v;
            Normal n;

            for (auto h : mesh_.halfedges(f))
            {
                v = mesh_.to_vertex(h);
                corner_halfedges.push_back(h);
                corner_vertices.push_back(v);
                corner_positions.push_back((vec3)vpos[v]);

                if (crease_angle_ < 1)
                {
                    n = face_normals[f.idx()];
                }
                else if (crease_angle_ > 170)
                {
                    n = vertex_normals[v.idx()];
                }
                else
                {
                    n = corner_normal(mesh_, h, crease_angle_radians);
                }
                corner_normals.push_back((vec3)n);

                if (htex)
                {
                    corner_texcoords.push_back((vec2)htex[h]);
                }
                else if (vtex)
                {
                    corner_texcoords.push_back((vec2)vtex[v]);
                }

                if (vcolor && use_colors_)
                {
                    corner_colors.push_back((vec3)vcolor[v]);
                }
                else if (fcolor && use_colors_)
                {
                    corner_colors.push_back((vec3)fcolor[f]);
                }
            }
            assert(corner_vertices.size() >= 3);

            // tessellate face into triangles
            tessellate(corner_positions, triangles);
            for (auto& t : triangles)
            {
                const int i0 = t[0];
                const int i1 = t[1];
                const int i2 = t[2];

                position_array.push_back(corner_positions[i0]);
                position_array.push_back(corner_positions[i1]);
                position_array.push_back(corner_positions[i2]);

                normal_array.push_back(corner_normals[i0]);
                normal_array.push_back(corner_normals[i1]);
                normal_array.push_back(corner_normals[i2]);

                if (htex || vtex)
                {
                    tex_array.push_back(corner_texcoords[i0]);
                    tex_array.push_back(corner_texcoords[i1]);
                    tex_array.push_back(corner_texcoords[i2]);
                }

                if ((vcolor || fcolor) && use_colors_)
                {
                    color_array.push_back(corner_colors[i0]);
                    color_array.push_back(corner_colors[i1]);
                    color_array.push_back(corner_colors[i2]);
                }

                vertex_indices[corner_vertices[i0].idx()] = vidx++;
                vertex_indices[corner_vertices[i1].idx()] = vidx++;
                vertex_indices[corner_vertices[i2].idx()] = vidx++;
            }
        }
    }

    // we have a point cloud
    else if (mesh_.n_vertices())
    {
        auto position = mesh_.get_vertex_property<Point>("v:point");
        if (position)
        {
            position_array.reserve(mesh_.n_vertices());
            for (auto v : mesh_.vertices())
                position_array.push_back((vec3)position[v]);
        }

        auto normals = mesh_.get_vertex_property<Point>("v:normal");
        if (normals)
        {
            normal_array.reserve(mesh_.n_vertices());
            for (auto v : mesh_.vertices())
                normal_array.push_back((vec3)normals[v]);
        }

        if (vcolor && use_colors_)
        {
            color_array.reserve(mesh_.n_vertices());
            for (auto v : mesh_.vertices())
                color_array.push_back((vec3)vcolor[v]);
        }

        // point clouds keep their vertex indices
        for (auto v : mesh_.vertices())
            vertex_indices[v.idx()] = v.idx();
    }

    n_vertices_ = position_array.size();
    has_normals_ = !normal_array.empty();
    has_texcoords_ = !tex_array.empty();
    has_vertex_colors_ = !color_array.empty();

    // WebGPU requires every vertex buffer slot of the pipeline to be bound
    // with sufficient size: fill missing attributes with zeros.
    if (!has_normals_)
        normal_array.assign(n_vertices_, vec3(0, 0, 0));
    if (!has_texcoords_)
        tex_array.assign(n_vertices_, vec2(0, 0));
    if (!has_vertex_colors_)
        color_array.assign(n_vertices_, vec3(0, 0, 0));

    // upload vertex attributes
    upload(vertex_buffer_, WGPUBufferUsage_Vertex, position_array.data(),
           position_array.size() * sizeof(vec3), "positions");
    upload(normal_buffer_, WGPUBufferUsage_Vertex, normal_array.data(),
           normal_array.size() * sizeof(vec3), "normals");
    upload(tex_coord_buffer_, WGPUBufferUsage_Vertex, tex_array.data(),
           tex_array.size() * sizeof(vec2), "texcoords");
    upload(color_buffer_, WGPUBufferUsage_Vertex, color_array.data(),
           color_array.size() * sizeof(vec3), "colors");

    // edge indices
    if (mesh_.n_edges())
    {
        std::vector<unsigned int> edge_indices;
        edge_indices.reserve(2 * mesh_.n_edges());

        for (auto e : mesh_.edges())
        {
            auto v0 = mesh_.vertex(e, 0).idx();
            auto v1 = mesh_.vertex(e, 1).idx();
            edge_indices.push_back(vertex_indices[v0]);
            edge_indices.push_back(vertex_indices[v1]);
        }

        upload(edge_buffer_, WGPUBufferUsage_Index, edge_indices.data(),
               edge_indices.size() * sizeof(unsigned int), "edges");
        n_edges_ = edge_indices.size();
    }
    else
        n_edges_ = 0;

    // feature edges
    auto efeature = mesh_.get_edge_property<bool>("e:feature");
    if (efeature)
    {
        std::vector<unsigned int> features;

        for (auto e : mesh_.edges())
        {
            if (efeature[e])
            {
                auto v0 = mesh_.vertex(e, 0).idx();
                auto v1 = mesh_.vertex(e, 1).idx();
                features.push_back(vertex_indices[v0]);
                features.push_back(vertex_indices[v1]);
            }
        }

        upload(feature_buffer_, WGPUBufferUsage_Index, features.data(),
               features.size() * sizeof(unsigned int), "features");
        n_features_ = features.size();
    }
    else
        n_features_ = 0;

    // selected points: rendered as instanced quads, so we copy positions
    // and normals of selected vertices into dedicated instance buffers
    auto vselected = mesh_.get_vertex_property<bool>("v:selected");
    if (vselected)
    {
        std::vector<vec3> positions, normals;
        for (auto v : mesh_.vertices())
        {
            if (vselected[v])
            {
                const size_t i = vertex_indices[v.idx()];
                positions.push_back(position_array[i]);
                normals.push_back(normal_array[i]);
            }
        }
        upload(selection_position_buffer_, WGPUBufferUsage_Vertex,
               positions.data(), positions.size() * sizeof(vec3),
               "selection positions");
        upload(selection_normal_buffer_, WGPUBufferUsage_Vertex, normals.data(),
               normals.size() * sizeof(vec3), "selection normals");
        n_selected_ = positions.size();
    }
    else
        n_selected_ = 0;

    // update overlays
    for (auto& overlay : overlays_)
        overlay->update_buffers();
}

uint32_t Renderer::upload_uniforms(const Uniforms& u)
{
    auto& ctx = GpuContext::get();

    // new submission: start from the first slot again
    if (ctx.submission_id() != uniform_submission_)
    {
        uniform_submission_ = ctx.submission_id();
        uniform_slot_ = 0;
    }

    // ring buffer full? allocate a bigger one (the old one stays alive
    // until the commands referencing it have executed)
    if (uniform_slot_ >= uniform_slots_)
    {
        release(uniform_buffer_, wgpuBufferRelease);
        uniform_slots_ *= 2;
        uniform_buffer_ = ctx.create_buffer(
            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
            (uint64_t)uniform_slots_ * uniform_slot_size, nullptr, "uniforms");
        update_bind_group();
    }

    const uint32_t offset = uniform_slot_ * uniform_slot_size;
    wgpuQueueWriteBuffer(ctx.queue(), uniform_buffer_, offset, &u, sizeof(u));
    ++uniform_slot_;
    return offset;
}

void Renderer::draw_call(ShaderKind shader, Primitive primitive,
                         bool depth_less_equal, const Uniforms& uniforms,
                         WGPUBuffer index_buffer, uint32_t count,
                         bool selection)
{
    if (count == 0)
        return;

    auto& ctx = GpuContext::get();
    WGPURenderPassEncoder pass = ctx.pass();
    if (!pass)
        return;

    const uint32_t offset = upload_uniforms(uniforms);

    wgpuRenderPassEncoderSetPipeline(
        pass, pipeline(shader, primitive, depth_less_equal));
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group_, 1, &offset);

    if (selection)
    {
        // texcoords and colors are unused for selected points; bind the
        // (large enough) position buffer to satisfy validation
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 0, selection_position_buffer_, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 1, selection_normal_buffer_,
                                             0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 2, selection_position_buffer_, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 3, selection_position_buffer_, 0, WGPU_WHOLE_SIZE);
    }
    else
    {
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buffer_, 0,
                                             WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 1, normal_buffer_, 0,
                                             WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 2, tex_coord_buffer_, 0,
                                             WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 3, color_buffer_, 0,
                                             WGPU_WHOLE_SIZE);
    }

    if (primitive == Primitive::Points)
    {
        // six quad vertices per point instance
        wgpuRenderPassEncoderDraw(pass, 6, count, 0, 0);
    }
    else if (index_buffer)
    {
        wgpuRenderPassEncoderSetIndexBuffer(
            pass, index_buffer, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(pass, count, 1, 0, 0, 0);
    }
    else
    {
        wgpuRenderPassEncoderDraw(pass, count, 1, 0, 0);
    }
}

void Renderer::draw(const mat4& projection_matrix, const mat4& modelview_matrix,
                    const std::string& draw_mode)
{
    auto& ctx = GpuContext::get();
    if (!ctx.is_initialized() || !ctx.pass())
        return;

    // did we generate buffers already?
    if (!buffers_initialized_)
        update_buffers();

    // we need some texture to build a complete bind group
    if (!texture_)
        use_checkerboard_texture();

    // empty mesh?
    if (mesh_.is_empty())
        return;

    // setup matrices
    const mat4 mv_matrix = modelview_matrix;
    const mat4 mvp_matrix = projection_matrix * modelview_matrix;
    const mat3 n_matrix = inverse(transpose(linear_part(mv_matrix)));

    // setup uniforms (matrices are column-major in both pmp and WGSL)
    Uniforms u{};
    std::memcpy(u.modelview_projection_matrix, mvp_matrix.data(),
                16 * sizeof(float));
    std::memcpy(u.modelview_matrix, mv_matrix.data(), 16 * sizeof(float));
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            u.normal_matrix[4 * col + row] = n_matrix(row, col);

    auto set_color = [&u](const vec3& front, const vec3& back) {
        for (int i = 0; i < 3; ++i)
        {
            u.front_color[i] = front[i];
            u.back_color[i] = back[i];
        }
    };
    set_color(front_color_, back_color_);
    u.ambient = ambient_;
    u.diffuse = diffuse_;
    u.specular = specular_;
    u.shininess = shininess_;
    u.alpha = alpha_;
    u.point_size = (float)point_size_;
    const auto& vp = ctx.viewport();
    u.viewport[0] = (float)std::max(1, vp[2]);
    u.viewport[1] = (float)std::max(1, vp[3]);
    u.flags = Uniforms::Lighting;
    if (has_vertex_colors_ && use_colors_)
        u.flags |= Uniforms::VertexColor;

    const Uniforms base = u;

    if (draw_mode == "Points")
    {
        u.flags = Uniforms::RoundPoints;
        if (has_normals_)
            u.flags |= Uniforms::Lighting;
        if (has_vertex_colors_)
            u.flags |= Uniforms::VertexColor;
        draw_call(ShaderKind::Phong, Primitive::Points, false, u, nullptr,
                  n_vertices_);
    }

    else if (draw_mode == "Hidden Line")
    {
        if (mesh_.n_faces())
        {
            // draw faces, pushed back a bit
            ctx.set_depth_range(0.01, 1.0);
            draw_call(ShaderKind::Phong, Primitive::Triangles, false, u,
                      nullptr, n_vertices_);

            // overlay edges
            ctx.set_depth_range(0.0, 1.0);
            u = base;
            set_color(vec3(0.1, 0.1, 0.1), vec3(0.1, 0.1, 0.1));
            u.flags = 0;
            draw_call(ShaderKind::Phong, Primitive::Lines, true, u,
                      edge_buffer_, n_edges_);
        }
    }

    else if (draw_mode == "Smooth Shading")
    {
        if (mesh_.n_faces())
        {
            draw_call(ShaderKind::Phong, Primitive::Triangles, false, u,
                      nullptr, n_vertices_);
        }
    }

    else if (draw_mode == "Texture")
    {
        if (mesh_.n_faces())
        {
            if (texture_mode_ == TextureMode::MatCap)
            {
                draw_call(ShaderKind::MatCap, Primitive::Triangles, false, u,
                          nullptr, n_vertices_);
            }
            else
            {
                set_color(vec3(0.9, 0.9, 0.9), vec3(0.3, 0.3, 0.3));
                u.flags = Uniforms::Lighting | Uniforms::Texture;
                if (use_srgb_)
                    u.flags |= Uniforms::SRGB;
                draw_call(ShaderKind::Phong, Primitive::Triangles, false, u,
                          nullptr, n_vertices_);
            }
        }
    }

    else if (draw_mode == "Texture Layout")
    {
        if (mesh_.n_faces() && has_texcoords_)
        {
            // draw faces
            set_color(vec3(0.8, 0.8, 0.8), vec3(0.9, 0.0, 0.0));
            u.flags = Uniforms::TextureLayout;
            ctx.set_depth_range(0.01, 1.0);
            draw_call(ShaderKind::Phong, Primitive::Triangles, false, u,
                      nullptr, n_vertices_);

            // overlay edges
            ctx.set_depth_range(0.0, 1.0);
            set_color(vec3(0.1, 0.1, 0.1), vec3(0.1, 0.1, 0.1));
            draw_call(ShaderKind::Phong, Primitive::Lines, true, u,
                      edge_buffer_, n_edges_);
        }
    }

    // draw feature edges
    if (n_features_)
    {
        u = base;
        set_color(vec3(0, 1, 1), vec3(0, 1, 0));
        u.flags = 0;
        ctx.set_depth_range(0.0, 1.0);
        draw_call(ShaderKind::Phong, Primitive::Lines, true, u, feature_buffer_,
                  n_features_);
    }

    // draw selected points
    if (n_selected_)
    {
        u = base;
        set_color(vec3(0, 1, 1), back_color_);
        u.flags = Uniforms::RoundPoints | Uniforms::Lighting;
        ctx.set_depth_range(0.0, 1.0);
        draw_call(ShaderKind::Phong, Primitive::Points, true, u, nullptr,
                  n_selected_, true);
    }

    // draw overlays
    for (auto& overlay : overlays_)
        overlay->draw(projection_matrix, modelview_matrix);
}

void Renderer::tessellate(const std::vector<vec3>& points,
                          std::vector<ivec3>& triangles)
{
    const int n = points.size();

    triangles.clear();
    triangles.reserve(n - 2);

    // triangle? nothing to do
    if (n == 3)
    {
        triangles.emplace_back(0, 1, 2);
        return;
    }

    // quad? simply compare to two options
    else if (n == 4)
    {
        if (area(points[0], points[1], points[2]) +
                area(points[0], points[2], points[3]) <
            area(points[0], points[1], points[3]) +
                area(points[1], points[2], points[3]))
        {
            triangles.emplace_back(0, 1, 2);
            triangles.emplace_back(0, 2, 3);
        }
        else
        {
            triangles.emplace_back(0, 1, 3);
            triangles.emplace_back(1, 2, 3);
        }
        return;
    }

    // n-gon with n>4? compute triangulation by dynamic programming
    init_triangulation(n);
    int i, j, m, k, imin;
    Scalar w, wmin;

    // initialize 2-gons
    for (i = 0; i < n - 1; ++i)
    {
        triangulation(i, i + 1) = Triangulation(0.0, -1);
    }

    // n-gons with n>2
    for (j = 2; j < n; ++j)
    {
        // for all n-gons [i,i+j]
        for (i = 0; i < n - j; ++i)
        {
            k = i + j;

            wmin = std::numeric_limits<Scalar>::max();
            imin = -1;

            // find best split i < m < i+j
            for (m = i + 1; m < k; ++m)
            {
                w = triangulation(i, m).area +
                    area(points[i], points[m], points[k]) +
                    triangulation(m, k).area;

                if (w < wmin)
                {
                    wmin = w;
                    imin = m;
                }
            }

            triangulation(i, k) = Triangulation(wmin, imin);
        }
    }

    // build triangles from triangulation table
    std::vector<ivec2> todo;
    todo.reserve(n);
    todo.emplace_back(0, n - 1);
    while (!todo.empty())
    {
        ivec2 tri = todo.back();
        todo.pop_back();
        const int start = tri[0];
        const int end = tri[1];
        if (end - start < 2)
            continue;
        const int split = triangulation(start, end).split;

        triangles.emplace_back(start, split, end);

        todo.emplace_back(start, split);
        todo.emplace_back(split, end);
    }
}

} // namespace pmp
