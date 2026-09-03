// Copyright 2011-2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/viewers/gpu.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace pmp {

namespace {

std::string to_string(WGPUStringView s)
{
    if (!s.data)
        return {};
    if (s.length == WGPU_STRLEN)
        return std::string(s.data);
    return std::string(s.data, s.length);
}

const char* backend_name(WGPUBackendType t)
{
    switch (t)
    {
        case WGPUBackendType_Vulkan:
            return "Vulkan";
        case WGPUBackendType_Metal:
            return "Metal";
        case WGPUBackendType_D3D12:
            return "Direct3D 12";
        case WGPUBackendType_OpenGL:
            return "OpenGL";
        case WGPUBackendType_WebGPU:
            return "WebGPU";
        default:
            return "unknown backend";
    }
}

void on_device_error(const WGPUDevice*, WGPUErrorType type,
                     WGPUStringView message, void*, void*)
{
    const char* kind = "error";
    switch (type)
    {
        case WGPUErrorType_Validation:
            kind = "validation error";
            break;
        case WGPUErrorType_OutOfMemory:
            kind = "out of memory";
            break;
        case WGPUErrorType_Internal:
            kind = "internal error";
            break;
        default:
            break;
    }
    std::cerr << "WebGPU " << kind << ": " << to_string(message) << std::endl;
}

void on_device_lost(const WGPUDevice*, WGPUDeviceLostReason reason,
                    WGPUStringView message, void*, void*)
{
    if (reason == WGPUDeviceLostReason_Destroyed)
        return;
    std::cerr << "WebGPU device lost: " << to_string(message) << std::endl;
}

// round up to multiple of alignment
uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

WGPUCallbackMode GpuContext::wait_mode()
{
#ifdef __EMSCRIPTEN__
    return WGPUCallbackMode_WaitAnyOnly;
#else
    // wgpu-native has not implemented wgpuInstanceWaitAny; its callbacks
    // fire synchronously or from wgpuDevicePoll()/ProcessEvents()
    return WGPUCallbackMode_AllowProcessEvents;
#endif
}

void GpuContext::wait(WGPUFuture future)
{
#ifdef __EMSCRIPTEN__
    WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
    info.future = future;
    wgpuInstanceWaitAny(instance_, 1, &info, UINT64_MAX);
#else
    (void)future;
    wgpuInstanceProcessEvents(instance_);
#endif
}

GpuContext& GpuContext::get()
{
    static GpuContext ctx;
    return ctx;
}

GpuContext::~GpuContext()
{
    shutdown();
}

void GpuContext::init(GLFWwindow* window)
{
    window_ = window;

#ifndef __EMSCRIPTEN__
    // forward wgpu-native warnings and errors to stderr
    wgpuSetLogCallback(
        [](WGPULogLevel level, WGPUStringView message, void*) {
            std::cerr << (level <= WGPULogLevel_Error ? "wgpu error: "
                                                      : "wgpu warning: ")
                      << to_string(message) << std::endl;
        },
        nullptr);
    wgpuSetLogLevel(std::getenv("PMP_WGPU_TRACE") ? WGPULogLevel_Trace
                                                  : WGPULogLevel_Warn);
#endif

    // instance
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
    // allow blocking on futures (requires ASYNCIFY or JSPI)
    const WGPUInstanceFeatureName features[] = {
        WGPUInstanceFeatureName_TimedWaitAny};
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = features;
#endif
    instance_ = wgpuCreateInstance(&instance_desc);
    if (!instance_)
    {
        std::cerr << "Cannot create WebGPU instance.\n";
        exit(EXIT_FAILURE);
    }

    // surface from GLFW window (X11, Wayland, Cocoa/Metal, or Win32)
    surface_ = glfwCreateWindowWGPUSurface(instance_, window);
    if (!surface_)
    {
        std::cerr << "Cannot create WebGPU surface for window";
#ifndef __EMSCRIPTEN__
        std::cerr << " (GLFW platform " << std::hex << glfwGetPlatform()
                  << std::dec << ")";
#endif
        std::cerr << ".\n";
        exit(EXIT_FAILURE);
    }

    // adapter (synchronously)
    {
        WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
        options.compatibleSurface = surface_;
        options.powerPreference = WGPUPowerPreference_HighPerformance;

        struct Result
        {
            WGPUAdapter adapter{nullptr};
            std::string message;
        } result;

        WGPURequestAdapterCallbackInfo cb =
            WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        cb.mode = wait_mode();
        cb.userdata1 = &result;
        cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                         WGPUStringView message, void* ud, void*) {
            auto* r = static_cast<Result*>(ud);
            if (status == WGPURequestAdapterStatus_Success)
                r->adapter = adapter;
            else
                r->message = to_string(message);
        };

        wait(wgpuInstanceRequestAdapter(instance_, &options, cb));

        if (!result.adapter)
        {
            std::cerr << "Cannot find a WebGPU adapter: " << result.message
                      << "\n";
            exit(EXIT_FAILURE);
        }
        adapter_ = result.adapter;
    }

    // adapter info
    {
        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        wgpuAdapterGetInfo(adapter_, &info);
        adapter_info_ = std::string(backend_name(info.backendType)) + " on " +
                        to_string(info.device);
        wgpuAdapterInfoFreeMembers(info);
    }

    // device (synchronously)
    {
        WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
        desc.label = str("pmp device");
        desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        desc.deviceLostCallbackInfo.callback = on_device_lost;
        desc.uncapturedErrorCallbackInfo.callback = on_device_error;

        struct Result
        {
            WGPUDevice device{nullptr};
            std::string message;
        } result;

        WGPURequestDeviceCallbackInfo cb =
            WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        cb.mode = wait_mode();
        cb.userdata1 = &result;
        cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                         WGPUStringView message, void* ud, void*) {
            auto* r = static_cast<Result*>(ud);
            if (status == WGPURequestDeviceStatus_Success)
                r->device = device;
            else
                r->message = to_string(message);
        };

        wait(wgpuAdapterRequestDevice(adapter_, &desc, cb));

        if (!result.device)
        {
            std::cerr << "Cannot create WebGPU device: " << result.message
                      << "\n";
            exit(EXIT_FAILURE);
        }
        device_ = result.device;
    }

    queue_ = wgpuDeviceGetQueue(device_);

    // surface capabilities: pick a non-sRGB 8-bit format so that colors
    // are written as-is (matches the previous OpenGL behavior)
    {
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(surface_, adapter_, &caps);

        surface_format_ =
            caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
        for (size_t i = 0; i < caps.formatCount; ++i)
        {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                caps.formats[i] == WGPUTextureFormat_RGBA8Unorm)
            {
                surface_format_ = caps.formats[i];
                break;
            }
        }

        surface_copyable_ = (caps.usages & WGPUTextureUsage_CopySrc) != 0;
#ifdef __EMSCRIPTEN__
        // the browser does not report usages; canvas textures support copies
        surface_copyable_ = true;
#endif

        wgpuSurfaceCapabilitiesFreeMembers(caps);
    }

    int fb_width = 800, fb_height = 600;
    if (window)
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
    configure_surface(fb_width, fb_height);
}

void GpuContext::shutdown()
{
    release_attachments();

    if (pick_depth_view_)
        wgpuTextureViewRelease(pick_depth_view_);
    if (pick_depth_texture_)
        wgpuTextureRelease(pick_depth_texture_);
    pick_depth_view_ = nullptr;
    pick_depth_texture_ = nullptr;

    if (surface_)
    {
        wgpuSurfaceUnconfigure(surface_);
        wgpuSurfaceRelease(surface_);
    }
    if (queue_)
        wgpuQueueRelease(queue_);
    if (device_)
        wgpuDeviceRelease(device_);
    if (adapter_)
        wgpuAdapterRelease(adapter_);
    if (instance_)
        wgpuInstanceRelease(instance_);

    surface_ = nullptr;
    queue_ = nullptr;
    device_ = nullptr;
    adapter_ = nullptr;
    instance_ = nullptr;
}

void GpuContext::configure_surface(uint32_t width, uint32_t height)
{
    if (!device_ || width == 0 || height == 0)
        return;

    width_ = width;
    height_ = height;

    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device_;
    config.format = surface_format_;
    config.usage = WGPUTextureUsage_RenderAttachment;
    if (surface_copyable_)
        config.usage |= WGPUTextureUsage_CopySrc;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode =
        vsync_ ? WGPUPresentMode_Fifo : WGPUPresentMode_Immediate;
    wgpuSurfaceConfigure(surface_, &config);

    release_attachments();
    create_attachments();
}

void GpuContext::set_vsync(bool enable)
{
#ifdef __EMSCRIPTEN__
    (void)enable; // browsers always present with vsync
    return;
#endif
    if (enable != vsync_)
    {
        vsync_ = enable;
        configure_surface(width_, height_);
    }
}

void GpuContext::create_attachments()
{
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{width_, height_, 1};
    desc.mipLevelCount = 1;
    desc.sampleCount = msaa_samples;
    desc.usage = WGPUTextureUsage_RenderAttachment;

    desc.label = str("msaa color");
    desc.format = surface_format_;
    msaa_texture_ = wgpuDeviceCreateTexture(device_, &desc);
    msaa_view_ = wgpuTextureCreateView(msaa_texture_, nullptr);

    desc.label = str("msaa depth");
    desc.format = depth_format;
    depth_texture_ = wgpuDeviceCreateTexture(device_, &desc);
    depth_view_ = wgpuTextureCreateView(depth_texture_, nullptr);
}

void GpuContext::release_attachments()
{
    if (msaa_view_)
        wgpuTextureViewRelease(msaa_view_);
    if (msaa_texture_)
        wgpuTextureRelease(msaa_texture_);
    if (depth_view_)
        wgpuTextureViewRelease(depth_view_);
    if (depth_texture_)
        wgpuTextureRelease(depth_texture_);
    msaa_view_ = nullptr;
    msaa_texture_ = nullptr;
    depth_view_ = nullptr;
    depth_texture_ = nullptr;
}

bool GpuContext::begin_frame(const vec3& clear_color)
{
    clear_color_ = clear_color;

    // make sure surface matches the framebuffer size
    int fb_width = width_, fb_height = height_;
    if (window_)
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
    if (fb_width <= 0 || fb_height <= 0)
        return false;
    if ((uint32_t)fb_width != width_ || (uint32_t)fb_height != height_)
        configure_surface(fb_width, fb_height);

    // acquire swap chain texture
    WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);

    switch (surface_texture.status)
    {
        case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
        case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
            break;

        case WGPUSurfaceGetCurrentTextureStatus_Timeout:
        case WGPUSurfaceGetCurrentTextureStatus_Outdated:
        case WGPUSurfaceGetCurrentTextureStatus_Lost:
            if (surface_texture.texture)
                wgpuTextureRelease(surface_texture.texture);
            configure_surface(fb_width, fb_height);
            return false;

        default:
            if (surface_texture.texture)
                wgpuTextureRelease(surface_texture.texture);
            std::cerr << "Cannot acquire swap chain texture.\n";
            return false;
    }

    surface_texture_ = surface_texture.texture;
    surface_view_ = wgpuTextureCreateView(surface_texture_, nullptr);

    WGPUCommandEncoderDescriptor enc_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    enc_desc.label = str("frame");
    encoder_ = wgpuDeviceCreateCommandEncoder(device_, &enc_desc);

    begin_main_pass(WGPULoadOp_Clear, WGPULoadOp_Clear);
    set_viewport(0, 0, width_, height_);
    return true;
}

void GpuContext::begin_main_pass(WGPULoadOp color_load, WGPULoadOp depth_load)
{
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = msaa_view_;
    color.resolveTarget = surface_view_;
    color.loadOp = color_load;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue =
        WGPUColor{clear_color_[0], clear_color_[1], clear_color_[2], 1.0};

    WGPURenderPassDepthStencilAttachment depth =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = depth_view_;
    depth.depthLoadOp = depth_load;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;

    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.label = str("main pass");
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.depthStencilAttachment = &depth;

    pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &desc);
    pass_sample_count_ = msaa_samples;
    pass_color_format_ = surface_format_;
    depth_min_ = 0.0f;
    depth_max_ = 1.0f;
}

void GpuContext::end_pass()
{
    if (pass_)
    {
        wgpuRenderPassEncoderEnd(pass_);
        wgpuRenderPassEncoderRelease(pass_);
        pass_ = nullptr;
    }
}

void GpuContext::submit()
{
    WGPUCommandBufferDescriptor desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder_, &desc);
    wgpuCommandEncoderRelease(encoder_);
    encoder_ = nullptr;
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    ++submission_id_;
}

void GpuContext::clear_depth()
{
    if (!pass_)
        return;
    auto vp = viewport_;
    end_pass();
    begin_main_pass(WGPULoadOp_Load, WGPULoadOp_Clear);
    set_viewport(vp[0], vp[1], vp[2], vp[3]);
}

void GpuContext::end_frame()
{
    if (!encoder_)
        return;

    end_pass();
    submit();

    // read back framebuffer for screenshots (before presenting)
    if (framebuffer_callback_)
    {
        auto cb = std::move(framebuffer_callback_);
        framebuffer_callback_ = nullptr;

        if (!surface_copyable_)
        {
            std::cerr << "Framebuffer read-back not supported by surface.\n";
        }
        else
        {
            std::vector<unsigned char> pixels;
            copy_texture_to_cpu(surface_texture_, width_, height_, 4, pixels);

            // convert to RGB, flip vertically (OpenGL convention)
            const bool bgra =
                (surface_format_ == WGPUTextureFormat_BGRA8Unorm ||
                 surface_format_ == WGPUTextureFormat_BGRA8UnormSrgb);
            std::vector<unsigned char> rgb(3 * width_ * height_);
            for (uint32_t y = 0; y < height_; ++y)
            {
                const unsigned char* src = &pixels[4 * width_ * y];
                unsigned char* dst = &rgb[3 * width_ * (height_ - 1 - y)];
                for (uint32_t x = 0; x < width_; ++x)
                {
                    dst[3 * x + 0] = src[4 * x + (bgra ? 2 : 0)];
                    dst[3 * x + 1] = src[4 * x + 1];
                    dst[3 * x + 2] = src[4 * x + (bgra ? 0 : 2)];
                }
            }
            cb(rgb, (int)width_, (int)height_);
        }
    }

#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(surface_);
#endif // the browser presents the canvas on the next animation frame

    wgpuTextureViewRelease(surface_view_);
    wgpuTextureRelease(surface_texture_);
    surface_view_ = nullptr;
    surface_texture_ = nullptr;

    // let wgpu run callbacks and clean up
    poll(false);
}

void GpuContext::set_viewport(int x, int y, int width, int height)
{
    viewport_ = {x, y, width, height};
    if (!pass_)
        return;

    // clamp to attachment size (WebGPU requires the viewport to be inside)
    const int fb_height =
        pass_sample_count_ == msaa_samples ? (int)height_ : (int)pick_height_;
    const int fb_width =
        pass_sample_count_ == msaa_samples ? (int)width_ : (int)pick_width_;
    int x0 = std::max(0, x);
    int y0 = std::max(0, fb_height - (y + height)); // flip to top-left origin
    int x1 = std::min(fb_width, x + width);
    int y1 = std::min(fb_height, fb_height - y);
    if (x1 <= x0 || y1 <= y0)
        return;

    wgpuRenderPassEncoderSetViewport(pass_, (float)x0, (float)y0,
                                     (float)(x1 - x0), (float)(y1 - y0),
                                     depth_min_, depth_max_);
}

void GpuContext::set_depth_range(float min_depth, float max_depth)
{
    depth_min_ = min_depth;
    depth_max_ = max_depth;
    set_viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
}

bool GpuContext::map_buffer_sync(WGPUBuffer buffer, uint64_t size)
{
    struct Result
    {
        bool done{false};
        bool ok{false};
    } result;

    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = wait_mode();
    cb.userdata1 = &result;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud,
                     void*) {
        auto* r = static_cast<Result*>(ud);
        r->done = true;
        r->ok = (status == WGPUMapAsyncStatus_Success);
    };
    wait(wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, (size_t)size, cb));

    // wgpu-native: block until the GPU has finished and the callback fired
    for (int i = 0; i < 1000 && !result.done; ++i)
        poll(true);

    return result.ok;
}

float GpuContext::read_depth(int x, int y, const std::function<void()>& draw)
{
    if (!device_ || width_ == 0 || height_ == 0)
        return 1.0f;
    if (x < 0 || y < 0 || (uint32_t)x >= width_ || (uint32_t)y >= height_)
        return 1.0f;

    // (re)create single-sampled depth texture
    if (!pick_depth_texture_ || pick_width_ != width_ ||
        pick_height_ != height_)
    {
        if (pick_depth_view_)
            wgpuTextureViewRelease(pick_depth_view_);
        if (pick_depth_texture_)
            wgpuTextureRelease(pick_depth_texture_);

        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = str("pick depth");
        desc.dimension = WGPUTextureDimension_2D;
        desc.size = WGPUExtent3D{width_, height_, 1};
        desc.mipLevelCount = 1;
        desc.sampleCount = 1;
        desc.format = depth_format;
        desc.usage =
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        pick_depth_texture_ = wgpuDeviceCreateTexture(device_, &desc);
        pick_depth_view_ = wgpuTextureCreateView(pick_depth_texture_, nullptr);
        pick_width_ = width_;
        pick_height_ = height_;
    }

    // render depth-only pass
    WGPUCommandEncoderDescriptor enc_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    enc_desc.label = str("pick");
    encoder_ = wgpuDeviceCreateCommandEncoder(device_, &enc_desc);

    WGPURenderPassDepthStencilAttachment depth =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = pick_depth_view_;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;

    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.label = str("pick pass");
    desc.colorAttachmentCount = 0;
    desc.depthStencilAttachment = &depth;

    pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &desc);
    pass_sample_count_ = 1;
    pass_color_format_ = WGPUTextureFormat_Undefined;
    depth_min_ = 0.0f;
    depth_max_ = 1.0f;
    set_viewport(0, 0, width_, height_);

    draw();

    end_pass();
    submit();

    // read back the depth buffer (partial copies of depth textures are not
    // allowed, so copy the whole texture and pick the value we need)
    const uint32_t bytes_per_row = align_up(width_ * 4, 256);
    const uint64_t size = (uint64_t)bytes_per_row * height_;

    WGPUBufferDescriptor buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buf_desc.label = str("pick readback");
    buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    buf_desc.size = size;
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(device_, &buf_desc);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = pick_depth_texture_;
    src.aspect = WGPUTextureAspect_DepthOnly;

    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buffer;
    dst.layout.offset = 0;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height_;

    WGPUExtent3D extent{width_, height_, 1};

    encoder_ = wgpuDeviceCreateCommandEncoder(device_, &enc_desc);
    wgpuCommandEncoderCopyTextureToBuffer(encoder_, &src, &dst, &extent);
    submit();

    float depth_value = 1.0f;
    if (map_buffer_sync(buffer, size))
    {
        const auto* mapped = static_cast<const unsigned char*>(
            wgpuBufferGetConstMappedRange(buffer, 0, size));
        // flip y: OpenGL has y=0 at the bottom, WebGPU at the top
        const uint32_t row = height_ - 1 - (uint32_t)y;
        std::memcpy(&depth_value, mapped + (size_t)row * bytes_per_row + 4 * x,
                    sizeof(float));
        wgpuBufferUnmap(buffer);
    }
    wgpuBufferRelease(buffer);

    return depth_value;
}

void GpuContext::copy_texture_to_cpu(WGPUTexture texture, uint32_t width,
                                     uint32_t height, uint32_t bytes_per_pixel,
                                     std::vector<unsigned char>& out)
{
    const uint32_t bytes_per_row = align_up(width * bytes_per_pixel, 256);
    const uint64_t size = (uint64_t)bytes_per_row * height;

    WGPUBufferDescriptor buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buf_desc.label = str("readback");
    buf_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    buf_desc.size = size;
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(device_, &buf_desc);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = texture;

    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buffer;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height;

    WGPUExtent3D extent{width, height, 1};

    WGPUCommandEncoderDescriptor enc_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    enc_desc.label = str("readback");
    encoder_ = wgpuDeviceCreateCommandEncoder(device_, &enc_desc);
    wgpuCommandEncoderCopyTextureToBuffer(encoder_, &src, &dst, &extent);
    submit();

    out.assign((size_t)width * height * bytes_per_pixel, 0);
    if (map_buffer_sync(buffer, size))
    {
        const auto* mapped = static_cast<const unsigned char*>(
            wgpuBufferGetConstMappedRange(buffer, 0, size));
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(&out[(size_t)y * width * bytes_per_pixel],
                        mapped + (size_t)y * bytes_per_row,
                        (size_t)width * bytes_per_pixel);
        }
        wgpuBufferUnmap(buffer);
    }
    wgpuBufferRelease(buffer);
}

void GpuContext::request_framebuffer(
    std::function<void(const std::vector<unsigned char>&, int, int)> cb)
{
    framebuffer_callback_ = std::move(cb);
}

WGPUBuffer GpuContext::create_buffer(WGPUBufferUsage usage, uint64_t size,
                                     const void* data, const char* label)
{
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    if (label)
        desc.label = str(label);
    desc.usage = usage;
    // buffer sizes must be a multiple of 4
    desc.size = std::max<uint64_t>(4, (size + 3) / 4 * 4);
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(device_, &desc);
    if (data && size)
        wgpuQueueWriteBuffer(queue_, buffer, 0, data, (size_t)size);
    return buffer;
}

WGPUShaderModule GpuContext::create_shader_module(const char* wgsl,
                                                  const char* label)
{
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = str(wgsl);

    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &source.chain;
    if (label)
        desc.label = str(label);
    return wgpuDeviceCreateShaderModule(device_, &desc);
}

void GpuContext::poll(bool wait)
{
#ifdef __EMSCRIPTEN__
    (void)wait;
    wgpuInstanceProcessEvents(instance_);
#else
    wgpuDevicePoll(device_, wait, nullptr);
    wgpuInstanceProcessEvents(instance_);
#endif
}

} // namespace pmp
