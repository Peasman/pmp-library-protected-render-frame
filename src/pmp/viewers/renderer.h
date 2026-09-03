// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <tuple>

#include "pmp/types.h"
#include "pmp/viewers/drawable.h"
#include "pmp/viewers/gpu.h"
#include "pmp/mat_vec.h"

namespace pmp {

class SurfaceMesh;

//! Pixel format of textures loaded with Renderer::load_texture().
//! \ingroup viewers
enum class TextureFormat
{
    RGB,  //!< 8-bit RGB, linear
    RGBA, //!< 8-bit RGBA, linear
    SRGB, //!< 8-bit RGB, sRGB-encoded (decoded to linear on sampling)
    SRGBA //!< 8-bit RGBA, sRGB-encoded (decoded to linear on sampling)
};

//! Texture filtering modes.
//! \ingroup viewers
enum class TextureFilter
{
    Nearest,           //!< nearest neighbor
    Linear,            //!< bilinear interpolation
    LinearMipmapLinear //!< trilinear interpolation using mipmaps
};

//! Texture coordinate wrapping modes.
//! \ingroup viewers
enum class TextureWrap
{
    ClampToEdge,
    Repeat,
    MirroredRepeat
};

//! Class for rendering surface meshes using WebGPU
//! \ingroup viewers
class Renderer
{
public:
    //! Constructor
    explicit Renderer(const SurfaceMesh& mesh);

    //! Default destructor, deletes all GPU buffers.
    ~Renderer();

    //! get front color
    const vec3& front_color() const { return front_color_; }
    //! set front color
    void set_front_color(const vec3& color) { front_color_ = color; }

    //! get back color
    const vec3& back_color() const { return back_color_; }
    //! set back color
    void set_back_color(const vec3& color) { back_color_ = color; }

    //! get ambient reflection coefficient
    float ambient() const { return ambient_; }
    //! set ambient reflection coefficient
    void set_ambient(float a) { ambient_ = a; }

    //! get diffuse reflection coefficient
    float diffuse() const { return diffuse_; }
    //! set diffuse reflection coefficient
    void set_diffuse(float d) { diffuse_ = d; }

    //! get specular reflection coefficient
    float specular() const { return specular_; }
    //! set specular reflection coefficient
    void set_specular(float s) { specular_ = s; }

    //! get specular shininess coefficient
    float shininess() const { return shininess_; }
    //! set specular shininess coefficient
    void set_shininess(float s) { shininess_ = s; }

    //! get alpha value for transparent rendering
    float alpha() const { return alpha_; }
    //! set alpha value for transparent rendering
    void set_alpha(float a) { alpha_ = a; }

    //! get crease angle (in degrees) for visualization of sharp edges
    Scalar crease_angle() const { return crease_angle_; }
    //! set crease angle (in degrees) for visualization of sharp edges
    void set_crease_angle(Scalar ca);

    //! get point size for visualization of points
    int point_size() const { return point_size_; }
    //! set point size for visualization of points
    void set_point_size(int ps) { point_size_ = ps; }

    //! \brief Control usage of color information.
    //! \details Either per-vertex or per-face colors can be used. Vertex colors
    //! are only used if the mesh has a per-vertex property of type Color
    //! named \c "v:color". Face colors are only used if the mesh has a per-face
    //! property of type Color named \c "f:color". If set to false, the
    //! default front and back colors are used. Default is \c true.
    //! \note Vertex colors take precedence over face colors.
    void set_use_colors(bool use_colors) { use_colors_ = use_colors; }

    //! Draw the mesh into the render pass currently recorded by GpuContext.
    void draw(const mat4& projection_matrix, const mat4& modelview_matrix,
              const std::string& draw_mode);

    //! Update all GPU buffers for rendering.
    void update_buffers();

    //! \deprecated Use update_buffers() instead.
    void update_opengl_buffers() { update_buffers(); }

    //! Use color map to visualize scalar fields.
    void use_cold_warm_texture();

    //! Use checkerboard texture.
    void use_checkerboard_texture();

    //! Load texture from file.
    //! \param filename the location and name of the texture
    //! \param format pixel format (linear or sRGB, with or without alpha)
    //! \param min_filter interpolation filter for minification
    //! \param mag_filter interpolation filter for magnification
    //! \param wrap texture coordinates wrap preference
    //! \throw IOException in case of failure to load texture from file
    void load_texture(
        const std::filesystem::path& filename,
        TextureFormat format = TextureFormat::RGB,
        TextureFilter min_filter = TextureFilter::LinearMipmapLinear,
        TextureFilter mag_filter = TextureFilter::Linear,
        TextureWrap wrap = TextureWrap::ClampToEdge);

    //! Load mat-cap texture from file. The mat-cap will be used
    //! whenever the drawing mode is "Texture". This also means
    //! that you cannot have texture and mat-cap at the same time.
    //! \param filename the location and name of the texture
    //! \sa See src/apps/mview.cpp for an example usage.
    //! \throw IOException in case of failure to load texture from file
    void load_matcap(const std::filesystem::path& filename);

    //! Add a drawable overlay
    void add_overlay(std::shared_ptr<Drawable> drawable)
    {
        overlays_.push_back(std::move(drawable));
    }

    //! Uniform block layout shared by all mesh shaders (mirrors the WGSL
    //! struct in phong_shader.h).
    struct Uniforms
    {
        float modelview_projection_matrix[16];
        float modelview_matrix[16];
        float normal_matrix[12]; // 3 columns padded to vec4
        float front_color[3];
        float ambient;
        float back_color[3];
        float diffuse;
        float specular;
        float shininess;
        float point_size;
        float alpha;
        float viewport[2];
        uint32_t flags;
        uint32_t pad;

        enum Flags : uint32_t
        {
            Lighting = 1,
            Texture = 2,
            SRGB = 4,
            VertexColor = 8,
            RoundPoints = 16,
            TextureLayout = 32
        };
    };
    static_assert(sizeof(Uniforms) == 240, "Uniforms must match WGSL layout");

protected:
    const SurfaceMesh& mesh_;

    // helpers for computing triangulation of a polygon
    struct Triangulation
    {
        Triangulation(Scalar a = std::numeric_limits<Scalar>::max(), int s = -1)
            : area(a), split(s)
        {
        }
        Scalar area;
        int split;
    };

    // table to hold triangulation data
    std::vector<Triangulation> triangulation_;

    // valence of currently triangulated polygon
    unsigned int polygon_valence_;

    // reserve n*n array for computing triangulation
    void init_triangulation(unsigned int n)
    {
        triangulation_.clear();
        triangulation_.resize(n * n);
        polygon_valence_ = n;
    }

    // access triangulation array
    Triangulation& triangulation(int start, int end)
    {
        return triangulation_[polygon_valence_ * start + end];
    }

    // compute squared area of triangle. used for triangulate().
    inline Scalar area(const vec3& p0, const vec3& p1, const vec3& p2) const
    {
        return sqrnorm(cross(p1 - p0, p2 - p0));
    }

    // triangulate a polygon such that the sum of squared triangle areas is minimized.
    // this prevents overlapping/folding triangles for non-convex polygons.
    void tessellate(const std::vector<vec3>& points,
                    std::vector<ivec3>& triangles);

    // shaders and primitive types used for pipeline selection
    enum class ShaderKind
    {
        Phong,
        MatCap
    };
    enum class Primitive
    {
        Triangles,
        Lines,
        Points
    };

    // lazily create shader modules, layouts, uniform ring buffer
    void init_gpu_resources();

    // create or fetch a cached render pipeline matching the current pass
    WGPURenderPipeline pipeline(ShaderKind shader, Primitive primitive,
                                bool depth_less_equal);

    // upload a texture (RGBA8) and create sampler / bind group
    void create_texture(uint32_t width, uint32_t height,
                        const unsigned char* rgba, bool srgb,
                        TextureFilter min_filter, TextureFilter mag_filter,
                        TextureWrap wrap);
    void release_texture();
    void update_bind_group();

    // write uniforms into ring buffer, returns dynamic offset
    uint32_t upload_uniforms(const Uniforms& u);

    // issue one draw call with the given state
    void draw_call(ShaderKind shader, Primitive primitive,
                   bool depth_less_equal, const Uniforms& uniforms,
                   WGPUBuffer index_buffer, uint32_t count,
                   bool selection = false);

    // helper to replace a GPU buffer
    void upload(WGPUBuffer& buffer, WGPUBufferUsage usage, const void* data,
                size_t bytes, const char* label);

    // GPU vertex/index buffers
    WGPUBuffer vertex_buffer_{nullptr};
    WGPUBuffer normal_buffer_{nullptr};
    WGPUBuffer tex_coord_buffer_{nullptr};
    WGPUBuffer color_buffer_{nullptr};
    WGPUBuffer edge_buffer_{nullptr};
    WGPUBuffer feature_buffer_{nullptr};
    WGPUBuffer selection_position_buffer_{nullptr};
    WGPUBuffer selection_normal_buffer_{nullptr};
    bool buffers_initialized_{false};

    // buffer sizes
    uint32_t n_vertices_{0};
    uint32_t n_edges_{0};
    uint32_t n_triangles_{0};
    uint32_t n_features_{0};
    uint32_t n_selected_{0};
    bool has_normals_{false};
    bool has_texcoords_{false};
    bool has_vertex_colors_{false};

    // shader modules, layouts, pipelines
    WGPUShaderModule phong_module_{nullptr};
    WGPUShaderModule matcap_module_{nullptr};
    WGPUBindGroupLayout bind_group_layout_{nullptr};
    WGPUPipelineLayout pipeline_layout_{nullptr};
    WGPUBindGroup bind_group_{nullptr};
    std::map<std::tuple<int, int, bool, uint32_t, int>, WGPURenderPipeline>
        pipelines_;

    // uniform ring buffer (dynamic offsets, one slot per draw call)
    WGPUBuffer uniform_buffer_{nullptr};
    uint32_t uniform_slots_{0};
    uint32_t uniform_slot_{0};
    uint64_t uniform_submission_{~uint64_t(0)};

    // material properties
    vec3 front_color_, back_color_;
    float ambient_, diffuse_, specular_, shininess_, alpha_;
    bool use_srgb_;
    bool use_colors_;
    float crease_angle_;
    int point_size_;

    // texture for scalar fields, checkerboard, images, or mat-caps
    WGPUTexture texture_{nullptr};
    WGPUTextureView texture_view_{nullptr};
    WGPUSampler sampler_{nullptr};
    enum class TextureMode
    {
        ColdWarm,
        Checkerboard,
        MatCap,
        Other
    } texture_mode_;

    // Overlay drawables
    std::vector<std::shared_ptr<Drawable>> overlays_;
};

} // namespace pmp
