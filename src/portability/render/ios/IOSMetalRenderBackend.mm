#include "IOSMetalRenderBackend.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>

namespace Slic3r::portability::render::ios {

namespace {
constexpr MTLPixelFormat kPreferredPixelFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kDepthPixelFormat = MTLPixelFormatDepth32Float;

using FloatMatrix4x4 = std::array<float, 16>;

struct MetalVertex
{
    float position[3];
    float color[4];
};

struct MetalSceneUniforms
{
    FloatMatrix4x4 model_view_projection;
};

static FloatMatrix4x4 to_float_matrix(const RenderMatrix4x4& matrix)
{
    FloatMatrix4x4 out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<float>(matrix[i]);
    return out;
}

static FloatMatrix4x4 multiply_matrix(const FloatMatrix4x4& left, const FloatMatrix4x4& right)
{
    FloatMatrix4x4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k)
                value += left[row * 4 + k] * right[k * 4 + col];
            out[row * 4 + col] = value;
        }
    }
    return out;
}

static id<MTLBuffer> update_or_create_buffer(id<MTLDevice> device, id<MTLBuffer>& buffer, const void* data, size_t length, NSString* label)
{
    if (device == nil || data == nullptr || length == 0)
        return nil;

    if (buffer == nil || buffer.length < length) {
        buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
        if (buffer != nil)
            buffer.label = label;
    }

    if (buffer == nil || buffer.length < length)
        return nil;

    std::memcpy(buffer.contents, data, length);
    [buffer didModifyRange:NSMakeRange(0, length)];
    return buffer;
}

static NSString* metal_shader_source()
{
    return @"#include <metal_stdlib>\n"
           "using namespace metal;\n"
           "struct VSIn { float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; };\n"
           "struct VSOut { float4 position [[position]]; float4 color; };\n"
           "struct SceneUniforms { float4x4 mvp; };\n"
           "vertex VSOut model_vertex_main(VSIn in [[stage_in]], constant SceneUniforms& uniforms [[buffer(1)]]) {\n"
           "    VSOut out; out.position = uniforms.mvp * float4(in.position, 1.0); out.color = in.color; return out;\n"
           "}\n"
           "fragment float4 model_fragment_main(VSOut in [[stage_in]]) { return in.color; }\n"
           "vertex VSOut toolpath_vertex_main(VSIn in [[stage_in]], constant SceneUniforms& uniforms [[buffer(1)]]) {\n"
           "    VSOut out; out.position = uniforms.mvp * float4(in.position, 1.0); out.color = in.color; return out;\n"
           "}\n"
           "fragment float4 toolpath_fragment_main(VSOut in [[stage_in]]) { return float4(in.color.rgb, 1.0); }\n";
}
}

struct IOSMetalRenderBackend::MetalBackendState
{
    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> command_queue = nil;
    CAMetalLayer       *metal_layer = nil;
    id<MTLLibrary>      shader_library = nil;
    id<MTLRenderPipelineState> model_pipeline_state = nil;
    id<MTLRenderPipelineState> toolpath_pipeline_state = nil;
    id<MTLDepthStencilState> depth_stencil_state = nil;
    id<MTLTexture>      depth_texture = nil;
    id<MTLBuffer>       model_vertex_buffer = nil;
    id<MTLBuffer>       model_index_buffer = nil;
    NSUInteger          model_index_count = 0;
    id<MTLBuffer>       toolpath_vertex_buffer = nil;
    id<MTLBuffer>       toolpath_index_buffer = nil;
    NSUInteger          toolpath_index_count = 0;
};

IOSMetalRenderBackend::IOSMetalRenderBackend() : m_state(std::make_unique<MetalBackendState>()) {}

IOSMetalRenderBackend::~IOSMetalRenderBackend()
{
    shutdown();
}

std::string_view IOSMetalRenderBackend::backend_name() const
{
    return "ios-metal";
}

BackendType IOSMetalRenderBackend::backend_type() const
{
    return BackendType::Metal;
}

void IOSMetalRenderBackend::bind_metal_layer(CAMetalLayer *layer)
{
    if (m_state->metal_layer == layer)
        return;

    @autoreleasepool {
        if (m_state->metal_layer != nil)
            [m_state->metal_layer release];

        m_state->metal_layer = layer;
        if (m_state->metal_layer != nil) {
            [m_state->metal_layer retain];
            if (m_initialized)
                configure_layer_for_current_state(m_state->metal_layer);
        }
    }
}

bool IOSMetalRenderBackend::initialize()
{
    if (m_initialized)
        return true;

    @autoreleasepool {
        m_state->device = MTLCreateSystemDefaultDevice();
        if (m_state->device == nil) {
            NSLog(@"[OrcaMetal] initialize failed: MTLCreateSystemDefaultDevice returned nil.");
            return false;
        }

        if (![m_state->device supportsFeatureSet:MTLFeatureSet_iOS_GPUFamily1_v1]) {
            NSLog(@"[OrcaMetal] initialize failed: device '%@' does not support iOS_GPUFamily1_v1.", m_state->device.name);
            return false;
        }

        m_state->command_queue = [m_state->device newCommandQueue];
        if (m_state->command_queue == nil) {
            NSLog(@"[OrcaMetal] initialize failed: unable to create command queue for device '%@'.", m_state->device.name);
            return false;
        }

        if (m_state->metal_layer == nil)
            m_state->metal_layer = [[CAMetalLayer layer] retain];

        configure_layer_for_current_state(m_state->metal_layer);

        NSError *library_error = nil;
        m_state->shader_library = [m_state->device newLibraryWithSource:metal_shader_source() options:nil error:&library_error];
        if (m_state->shader_library == nil) {
            NSLog(@"[OrcaMetal] initialize failed: shader compilation error: %@", library_error.localizedDescription);
            return false;
        }

        MTLVertexDescriptor *vertex_descriptor = [[MTLVertexDescriptor alloc] init];
        vertex_descriptor.attributes[0].format = MTLVertexFormatFloat3;
        vertex_descriptor.attributes[0].offset = offsetof(MetalVertex, position);
        vertex_descriptor.attributes[0].bufferIndex = 0;
        vertex_descriptor.attributes[1].format = MTLVertexFormatFloat4;
        vertex_descriptor.attributes[1].offset = offsetof(MetalVertex, color);
        vertex_descriptor.attributes[1].bufferIndex = 0;
        vertex_descriptor.layouts[0].stride = sizeof(MetalVertex);
        vertex_descriptor.layouts[0].stepRate = 1;
        vertex_descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        id<MTLFunction> model_vertex_fn = [m_state->shader_library newFunctionWithName:@"model_vertex_main"];
        id<MTLFunction> model_fragment_fn = [m_state->shader_library newFunctionWithName:@"model_fragment_main"];
        id<MTLFunction> toolpath_vertex_fn = [m_state->shader_library newFunctionWithName:@"toolpath_vertex_main"];
        id<MTLFunction> toolpath_fragment_fn = [m_state->shader_library newFunctionWithName:@"toolpath_fragment_main"];

        if (model_vertex_fn == nil || model_fragment_fn == nil || toolpath_vertex_fn == nil || toolpath_fragment_fn == nil) {
            NSLog(@"[OrcaMetal] initialize failed: required shader entrypoints are unavailable in compiled library.");
            return false;
        }

        NSError *pipeline_error = nil;
        MTLRenderPipelineDescriptor *model_pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
        model_pipeline_desc.label = @"OrcaModelPipeline";
        model_pipeline_desc.vertexFunction = model_vertex_fn;
        model_pipeline_desc.fragmentFunction = model_fragment_fn;
        model_pipeline_desc.vertexDescriptor = vertex_descriptor;
        model_pipeline_desc.colorAttachments[0].pixelFormat = kPreferredPixelFormat;
        model_pipeline_desc.depthAttachmentPixelFormat = kDepthPixelFormat;
        m_state->model_pipeline_state = [m_state->device newRenderPipelineStateWithDescriptor:model_pipeline_desc error:&pipeline_error];
        if (m_state->model_pipeline_state == nil) {
            NSLog(@"[OrcaMetal] initialize failed: model pipeline creation failed: %@", pipeline_error.localizedDescription);
            return false;
        }

        pipeline_error = nil;
        MTLRenderPipelineDescriptor *toolpath_pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
        toolpath_pipeline_desc.label = @"OrcaToolpathPipeline";
        toolpath_pipeline_desc.vertexFunction = toolpath_vertex_fn;
        toolpath_pipeline_desc.fragmentFunction = toolpath_fragment_fn;
        toolpath_pipeline_desc.vertexDescriptor = vertex_descriptor;
        toolpath_pipeline_desc.colorAttachments[0].pixelFormat = kPreferredPixelFormat;
        toolpath_pipeline_desc.depthAttachmentPixelFormat = kDepthPixelFormat;
        m_state->toolpath_pipeline_state = [m_state->device newRenderPipelineStateWithDescriptor:toolpath_pipeline_desc error:&pipeline_error];
        if (m_state->toolpath_pipeline_state == nil) {
            NSLog(@"[OrcaMetal] initialize failed: toolpath pipeline creation failed: %@", pipeline_error.localizedDescription);
            return false;
        }

        MTLDepthStencilDescriptor *depth_stencil_desc = [[MTLDepthStencilDescriptor alloc] init];
        depth_stencil_desc.depthCompareFunction = MTLCompareFunctionLess;
        depth_stencil_desc.depthWriteEnabled = YES;
        m_state->depth_stencil_state = [m_state->device newDepthStencilStateWithDescriptor:depth_stencil_desc];
        if (m_state->depth_stencil_state == nil) {
            NSLog(@"[OrcaMetal] initialize failed: could not create depth stencil state.");
            return false;
        }

        ensure_depth_resources();
#if TARGET_OS_SIMULATOR
        NSLog(@"[OrcaMetal] running on iOS simulator with device '%@'.", m_state->device.name);
#endif
    }

    m_initialized = true;
    return true;
}


void IOSMetalRenderBackend::configure_layer_for_current_state(CAMetalLayer *layer)
{
    if (layer == nil || m_state->device == nil)
        return;

    layer.device = m_state->device;
    layer.pixelFormat = kPreferredPixelFormat;
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(std::max(m_width, 1), std::max(m_height, 1));
}

void IOSMetalRenderBackend::submit_scene_state(const RenderSceneState& scene_state)
{
    m_scene_state = scene_state;
    if (!m_initialized)
        return;

    @autoreleasepool {
        update_draw_payload_buffers(scene_state);
    }
}

void IOSMetalRenderBackend::resize(int width, int height)
{
    m_width = std::max(width, 0);
    m_height = std::max(height, 0);

    if (!m_initialized || m_state->metal_layer == nil)
        return;

    configure_layer_for_current_state(m_state->metal_layer);
    ensure_depth_resources();
}

void IOSMetalRenderBackend::render_frame()
{
    if (!m_initialized || m_state->metal_layer == nil || m_state->command_queue == nil)
        return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [m_state->metal_layer nextDrawable];
        if (drawable == nil)
            return;

        MTLRenderPassDescriptor *render_pass_descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        render_pass_descriptor.colorAttachments[0].texture = drawable.texture;
        render_pass_descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        render_pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        const bool looking_downward = m_scene_state.camera.is_looking_downward;
        render_pass_descriptor.colorAttachments[0].clearColor = looking_downward ?
            MTLClearColorMake(0.08, 0.08, 0.12, 1.0) :
            MTLClearColorMake(0.10, 0.08, 0.14, 1.0);
        render_pass_descriptor.depthAttachment.texture = m_state->depth_texture;
        render_pass_descriptor.depthAttachment.loadAction = MTLLoadActionClear;
        render_pass_descriptor.depthAttachment.storeAction = MTLStoreActionDontCare;
        render_pass_descriptor.depthAttachment.clearDepth = 1.0;

        id<MTLCommandBuffer> command_buffer = [m_state->command_queue commandBuffer];
        if (command_buffer == nil)
            return;

        id<MTLRenderCommandEncoder> command_encoder = [command_buffer renderCommandEncoderWithDescriptor:render_pass_descriptor];
        if (command_encoder == nil)
            return;

        MTLViewport viewport = {
            .originX = 0.0,
            .originY = 0.0,
            .width = static_cast<double>(std::max(m_width, 1)),
            .height = static_cast<double>(std::max(m_height, 1)),
            .znear = 0.0,
            .zfar = 1.0,
        };
        [command_encoder setViewport:viewport];
        [command_encoder setDepthStencilState:m_state->depth_stencil_state];

        const bool has_model_geometry = m_state->model_pipeline_state != nil && m_state->model_vertex_buffer != nil &&
                                        m_state->model_index_buffer != nil && m_state->model_index_count > 0;
        if (has_model_geometry) {
            [command_encoder setRenderPipelineState:m_state->model_pipeline_state];
            [command_encoder setVertexBuffer:m_state->model_vertex_buffer offset:0 atIndex:0];
            MetalSceneUniforms uniforms{};
            uniforms.model_view_projection = multiply_matrix(
                to_float_matrix(m_scene_state.camera.projection_matrix),
                multiply_matrix(to_float_matrix(m_scene_state.camera.view_matrix),
                                to_float_matrix(identity_matrix4x4())));
            [command_encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [command_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                        indexCount:m_state->model_index_count
                                         indexType:MTLIndexTypeUInt32
                                       indexBuffer:m_state->model_index_buffer
                                 indexBufferOffset:0];
        }

        const bool has_toolpath_geometry = m_state->toolpath_pipeline_state != nil && m_state->toolpath_vertex_buffer != nil &&
                                           m_state->toolpath_index_buffer != nil && m_state->toolpath_index_count > 0;
        if (has_toolpath_geometry) {
            [command_encoder setRenderPipelineState:m_state->toolpath_pipeline_state];
            [command_encoder setVertexBuffer:m_state->toolpath_vertex_buffer offset:0 atIndex:0];
            MetalSceneUniforms uniforms{};
            uniforms.model_view_projection = multiply_matrix(
                to_float_matrix(m_scene_state.camera.projection_matrix),
                multiply_matrix(to_float_matrix(m_scene_state.camera.view_matrix),
                                to_float_matrix(identity_matrix4x4())));
            [command_encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [command_encoder drawIndexedPrimitives:MTLPrimitiveTypeLine
                                        indexCount:m_state->toolpath_index_count
                                         indexType:MTLIndexTypeUInt32
                                       indexBuffer:m_state->toolpath_index_buffer
                                 indexBufferOffset:0];
        }
        [command_encoder endEncoding];

        [command_buffer presentDrawable:drawable];
        [command_buffer commit];
    }
}

void IOSMetalRenderBackend::shutdown()
{
    if (!m_state)
        return;

    @autoreleasepool {
        m_state->command_queue = nil;
        m_state->device = nil;
        m_state->shader_library = nil;
        m_state->model_pipeline_state = nil;
        m_state->toolpath_pipeline_state = nil;
        m_state->depth_stencil_state = nil;
        m_state->depth_texture = nil;
        m_state->model_vertex_buffer = nil;
        m_state->model_index_buffer = nil;
        m_state->model_index_count = 0;
        m_state->toolpath_vertex_buffer = nil;
        m_state->toolpath_index_buffer = nil;
        m_state->toolpath_index_count = 0;

        if (m_state->metal_layer != nil) {
            [m_state->metal_layer release];
        }

        m_state->metal_layer = nil;
    }

    m_initialized = false;
}

void IOSMetalRenderBackend::ensure_depth_resources()
{
    if (m_state->device == nil)
        return;

    const NSUInteger width = static_cast<NSUInteger>(std::max(m_width, 1));
    const NSUInteger height = static_cast<NSUInteger>(std::max(m_height, 1));
    if (m_state->depth_texture != nil && m_state->depth_texture.width == width && m_state->depth_texture.height == height)
        return;

    MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthPixelFormat width:width height:height mipmapped:NO];
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    m_state->depth_texture = [m_state->device newTextureWithDescriptor:depth_desc];
    if (m_state->depth_texture == nil) {
        NSLog(@"[OrcaMetal] resize failed: unable to allocate depth texture (%lux%lu).", (unsigned long) width, (unsigned long) height);
    } else {
        m_state->depth_texture.label = @"OrcaDepthTexture";
    }
}

void IOSMetalRenderBackend::update_draw_payload_buffers(const RenderSceneState& scene_state)
{
    if (m_state->device == nil)
        return;

    if (!scene_state.model_draw_payload.vertices.empty() && !scene_state.model_draw_payload.indices.empty()) {
        std::vector<MetalVertex> model_vertices;
        model_vertices.reserve(scene_state.model_draw_payload.vertices.size());
        for (const RenderVertex& vertex : scene_state.model_draw_payload.vertices)
            model_vertices.push_back(MetalVertex{{vertex.position[0], vertex.position[1], vertex.position[2]}, {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}});

        update_or_create_buffer(m_state->device, m_state->model_vertex_buffer, model_vertices.data(), model_vertices.size() * sizeof(MetalVertex), @"OrcaModelVertexBuffer");
        update_or_create_buffer(m_state->device, m_state->model_index_buffer, scene_state.model_draw_payload.indices.data(),
                                scene_state.model_draw_payload.indices.size() * sizeof(uint32_t), @"OrcaModelIndexBuffer");
        m_state->model_index_count = static_cast<NSUInteger>(scene_state.model_draw_payload.indices.size());
    } else {
        m_state->model_vertex_buffer = nil;
        m_state->model_index_buffer = nil;
        m_state->model_index_count = 0;
    }

    if (!scene_state.toolpath_draw_payload.vertices.empty() && !scene_state.toolpath_draw_payload.indices.empty()) {
        std::vector<MetalVertex> toolpath_vertices;
        toolpath_vertices.reserve(scene_state.toolpath_draw_payload.vertices.size());
        for (const RenderVertex& vertex : scene_state.toolpath_draw_payload.vertices)
            toolpath_vertices.push_back(MetalVertex{{vertex.position[0], vertex.position[1], vertex.position[2]}, {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}});

        update_or_create_buffer(m_state->device, m_state->toolpath_vertex_buffer, toolpath_vertices.data(), toolpath_vertices.size() * sizeof(MetalVertex), @"OrcaToolpathVertexBuffer");
        update_or_create_buffer(m_state->device, m_state->toolpath_index_buffer, scene_state.toolpath_draw_payload.indices.data(),
                                scene_state.toolpath_draw_payload.indices.size() * sizeof(uint32_t), @"OrcaToolpathIndexBuffer");
        m_state->toolpath_index_count = static_cast<NSUInteger>(scene_state.toolpath_draw_payload.indices.size());
    } else {
        m_state->toolpath_vertex_buffer = nil;
        m_state->toolpath_index_buffer = nil;
        m_state->toolpath_index_count = 0;
    }
}

} // namespace Slic3r::portability::render::ios
