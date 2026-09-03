// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

#include "pmp/mat_vec.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace pmp {

//! \brief Central WebGPU context shared by all viewer classes.
//! \details Owns the WebGPU instance, adapter, device, queue, and the
//! window surface, together with the multi-sampled color and depth
//! attachments. It also tracks the render pass that is currently being
//! recorded so that Renderer and Drawable objects can issue draw calls
//! without knowing about frame management.
//!
//! wgpu-native selects the native graphics API at runtime: Vulkan on Linux
//! and Windows (DX12 is also available there), Metal on macOS.
//! \ingroup viewers
class GpuContext
{
public:
    //! Depth format used for all depth attachments. Depth32Float is chosen
    //! because it can be copied to a buffer for picking.
    static constexpr WGPUTextureFormat depth_format =
        WGPUTextureFormat_Depth32Float;

    //! Number of samples used for multi-sampling of the main framebuffer.
    static constexpr uint32_t msaa_samples = 4;

    //! Access the global context.
    static GpuContext& get();

    //! Whether the context has been initialized.
    bool is_initialized() const { return device_ != nullptr; }

    //! Initialize instance, adapter, device, and window surface.
    //! Terminates the program if no WebGPU adapter can be found.
    void init(GLFWwindow* window);

    //! Release all WebGPU objects.
    void shutdown();

    //! \name Device access
    //!@{
    WGPUInstance instance() const { return instance_; }
    WGPUDevice device() const { return device_; }
    WGPUQueue queue() const { return queue_; }
    //! Format of the swap chain textures.
    WGPUTextureFormat surface_format() const { return surface_format_; }
    //! Human-readable description of adapter and backend (e.g. "Vulkan").
    const std::string& adapter_description() const { return adapter_info_; }
    //!@}

    //! \name Surface management
    //!@{
    //! (Re-)configure the surface and its attachments to the given size.
    void configure_surface(uint32_t width, uint32_t height);
    //! Enable or disable vertical synchronization (default: enabled).
    void set_vsync(bool enable);
    //! Width in pixels of the currently configured surface / attachments.
    uint32_t surface_width() const { return width_; }
    //! Height in pixels of the currently configured surface / attachments.
    uint32_t surface_height() const { return height_; }
    //!@}

    //! \name Frame management
    //!@{
    //! Acquire the next swap chain texture and begin the main render pass.
    //! Returns false if no texture could be acquired (e.g. window
    //! minimized), in which case the frame should be skipped.
    bool begin_frame(const vec3& clear_color);

    //! End the main render pass, submit all recorded commands, and present.
    void end_frame();

    //! End the current render pass and start a new one that clears only
    //! the depth buffer while preserving color. Mimics
    //! `glClear(GL_DEPTH_BUFFER_BIT)` in the middle of a frame.
    void clear_depth();

    //! Set the viewport (in pixels) for the current render pass.
    //! Follows OpenGL conventions: origin at the bottom-left corner.
    void set_viewport(int x, int y, int width, int height);

    //! Set the depth range of the viewport, like `glDepthRange()`.
    void set_depth_range(float min_depth, float max_depth);

    //! Query current viewport as (x, y, width, height).
    const std::array<int, 4>& viewport() const { return viewport_; }
    //!@}

    //! \name Information about the render pass being recorded
    //!@{
    //! Render pass encoder currently being recorded (nullptr outside a pass).
    WGPURenderPassEncoder pass() const { return pass_; }
    //! Sample count of the current pass' attachments.
    uint32_t pass_sample_count() const { return pass_sample_count_; }
    //! Color format of the current pass, or Undefined for a depth-only pass.
    WGPUTextureFormat pass_color_format() const { return pass_color_format_; }
    //! Counter incremented at each queue submission. Clients that use
    //! per-frame ring buffers can reset their allocation when it changes.
    uint64_t submission_id() const { return submission_id_; }
    //!@}

    //! \name Read-back
    //!@{
    //! \brief Read the depth value at pixel (x, y) (OpenGL convention: y=0 is
    //! the bottom row) by rendering the scene once more into an
    //! off-screen, single-sampled depth buffer.
    //! \param draw callback that issues all draw calls of the scene
    //! \return depth value in [0,1], 1.0 if nothing was hit or on failure
    float read_depth(int x, int y, const std::function<void()>& draw);

    //! \brief Request that the next presented frame is copied to CPU memory.
    //! \details The callback receives tightly packed RGB pixels, bottom row
    //! first (OpenGL convention), together with width and height.
    void request_framebuffer(
        std::function<void(const std::vector<unsigned char>&, int, int)> cb);
    //!@}

    //! \name Helpers
    //!@{
    //! Create a buffer with the given usage and (optional) initial data.
    WGPUBuffer create_buffer(WGPUBufferUsage usage, uint64_t size,
                             const void* data = nullptr,
                             const char* label = nullptr);

    //! Compile a WGSL shader module.
    WGPUShaderModule create_shader_module(const char* wgsl,
                                          const char* label = nullptr);

    //! Process pending events. If \p wait is true, block until all
    //! submitted work has finished (like `glFinish()`).
    void poll(bool wait);

    //! \brief Matrix converting an OpenGL-style projection (clip z in
    //! [-1,1]) to the WebGPU convention (clip z in [0,1]).
    //! \details All matrices in the viewers follow OpenGL conventions; apply
    //! this on the left of the projection matrix before uploading it, i.e.
    //! `depth_zero_to_one() * projection * modelview`. Depth values read back
    //! from the GPU can then be mapped to OpenGL NDC with `2 * z - 1`.
    static mat4 depth_zero_to_one()
    {
        mat4 m = mat4::identity();
        m(2, 2) = 0.5f;
        m(2, 3) = 0.5f;
        return m;
    }

    //! Construct a string view from a C string.
    static WGPUStringView str(const char* s)
    {
        return WGPUStringView{s, WGPU_STRLEN};
    }
    //!@}

private:
    GpuContext() = default;
    ~GpuContext();
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    void create_attachments();
    void release_attachments();
    void begin_main_pass(WGPULoadOp color_load, WGPULoadOp depth_load);
    void end_pass();
    void submit();
    bool map_buffer_sync(WGPUBuffer buffer, uint64_t size);
    // callback mode / blocking wait appropriate for the implementation
    static WGPUCallbackMode wait_mode();
    void wait(WGPUFuture future);
    void copy_texture_to_cpu(WGPUTexture texture, uint32_t width,
                             uint32_t height, uint32_t bytes_per_pixel,
                             std::vector<unsigned char>& out);

    GLFWwindow* window_{nullptr};

    WGPUInstance instance_{nullptr};
    WGPUAdapter adapter_{nullptr};
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUSurface surface_{nullptr};
    WGPUTextureFormat surface_format_{WGPUTextureFormat_Undefined};
    bool surface_copyable_{false};
    bool vsync_{true};
    std::string adapter_info_;

    uint32_t width_{0}, height_{0};
    WGPUTexture msaa_texture_{nullptr};
    WGPUTextureView msaa_view_{nullptr};
    WGPUTexture depth_texture_{nullptr};
    WGPUTextureView depth_view_{nullptr};

    // single-sampled depth buffer for picking
    WGPUTexture pick_depth_texture_{nullptr};
    WGPUTextureView pick_depth_view_{nullptr};
    uint32_t pick_width_{0}, pick_height_{0};

    // per-frame state
    WGPUTexture surface_texture_{nullptr};
    WGPUTextureView surface_view_{nullptr};
    WGPUCommandEncoder encoder_{nullptr};
    WGPURenderPassEncoder pass_{nullptr};
    uint32_t pass_sample_count_{1};
    WGPUTextureFormat pass_color_format_{WGPUTextureFormat_Undefined};
    std::array<int, 4> viewport_{0, 0, 0, 0};
    float depth_min_{0.0f}, depth_max_{1.0f};
    vec3 clear_color_{1, 1, 1};
    uint64_t submission_id_{0};

    std::function<void(const std::vector<unsigned char>&, int, int)>
        framebuffer_callback_;
};

} // namespace pmp
