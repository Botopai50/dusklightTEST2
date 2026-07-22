// Lightweight cel-shading post process for Dusklight.
//
// The effect runs after the opaque scene. It resolves scene color and depth, reconstructs
// view-space normals, applies three-band lighting, adds depth/normal outlines and optional rim
// lighting, then writes the stylized scene back before translucent and HUD rendering.

#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarLightDirection = 0;
ConfigVarHandle g_cvarToonStrength = 0;
ConfigVarHandle g_cvarShadowLevel = 0;
ConfigVarHandle g_cvarMiddleLevel = 0;
ConfigVarHandle g_cvarHighlightLevel = 0;
ConfigVarHandle g_cvarBandSmoothing = 0;
ConfigVarHandle g_cvarOutlineStrength = 0;
ConfigVarHandle g_cvarOutlineThickness = 0;
ConfigVarHandle g_cvarDepthThreshold = 0;
ConfigVarHandle g_cvarNormalThreshold = 0;
ConfigVarHandle g_cvarRimStrength = 0;
ConfigVarHandle g_cvarRimPower = 0;
ConfigVarHandle g_cvarSaturation = 0;
ConfigVarHandle g_cvarContrast = 0;
ConfigVarHandle g_cvarDebugView = 0;

GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_afterOpaqueHook = 0;
UiWindowHandle g_controlsWindow = 0;

ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_pipelineLayout = nullptr;

// Mirror of Uniforms in res/celshade.wgsl.
struct CelshadeUniforms {
    float view_from_proj[16];
    float size[2];
    float inv_size[2];

    float light_dir_view[3];
    float toon_strength;

    float shadow_level;
    float middle_level;
    float highlight_level;
    float band_smoothing;

    float outline_strength;
    float depth_threshold;
    float normal_threshold;
    float outline_thickness;

    float rim_strength;
    float rim_power;
    float saturation;
    float contrast;

    uint32_t debug_mode;
    uint32_t reversed_z;
    uint32_t _pad[2];
};
static_assert(sizeof(CelshadeUniforms) % 16 == 0);

struct DrawPayload {
    WGPUTextureView color;
    WGPUTextureView depth;
    uint32_t uniform_offset;
    uint32_t uniform_size;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

int64_t get_int_option(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

WGPUShaderModule create_shader_module(const char* label, const ResourceBuffer& source) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};

    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {label, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
}

bool build_pipeline() {
    WGPUShaderModule module = create_shader_module("Cel shading", g_shaderSource);
    if (module == nullptr) {
        return false;
    }

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = g_deviceInfo.color_format;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    // The fullscreen pass does not read or write the active depth attachment, but its format and
    // sample count must match the scene pass.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = g_deviceInfo.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {"Cel shading", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = &depthStencil;
    pipelineDesc.multisample.count = g_deviceInfo.sample_count;
    pipelineDesc.fragment = &fragment;

    g_pipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr) {
        return false;
    }

    g_pipelineLayout = wgpuRenderPipelineGetBindGroupLayout(g_pipeline, 0);
    return g_pipelineLayout != nullptr;
}

void set_light_direction(int64_t preset, float out[3]) {
    switch (std::clamp<int64_t>(preset, 0, 3)) {
    default:
    case 0: // Upper-left, similar to illustrated key lighting.
        out[0] = -0.45f;
        out[1] = 0.65f;
        out[2] = 0.62f;
        break;
    case 1: // Upper-right.
        out[0] = 0.45f;
        out[1] = 0.65f;
        out[2] = 0.62f;
        break;
    case 2: // Mostly frontal.
        out[0] = 0.0f;
        out[1] = 0.30f;
        out[2] = 0.95f;
        break;
    case 3: // Strong overhead light.
        out[0] = 0.0f;
        out[1] = 0.97f;
        out[2] = 0.25f;
        break;
    }

    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (length > 0.0001f) {
        out[0] /= length;
        out[1] /= length;
        out[2] /= length;
    }
}

// Render-worker thread: replace the opaque scene color with the stylized resolved snapshot.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload) || g_pipeline == nullptr || g_pipelineLayout == nullptr) {
        return;
    }

    DrawPayload data{};
    std::memcpy(&data, payload, sizeof(data));
    if (data.color == nullptr || data.depth == nullptr) {
        return;
    }

    WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.color;
    entries[1].binding = 1;
    entries[1].textureView = data.depth;
    entries[2].binding = 2;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniform_offset;
    entries[2].size = data.uniform_size;

    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = g_pipelineLayout;
    bindGroupDesc.entryCount = 3;
    bindGroupDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    if (bindGroup == nullptr) {
        return;
    }

    wgpuRenderPassEncoderSetPipeline(ctx->pass, g_pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

// Game thread, after opaque world objects and before translucent/fog overlays.
void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    if (!get_bool_option(g_cvarEnabled, true)) {
        return;
    }
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext) ||
        stageCtx->game_view == nullptr)
    {
        return;
    }

    CameraInfo camera = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &camera) != MOD_OK) {
        return;
    }

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr || resolved.depth == nullptr || resolved.width == 0 ||
        resolved.height == 0)
    {
        return;
    }

    CelshadeUniforms uniforms{};
    std::memcpy(
        uniforms.view_from_proj, camera.view_from_proj, sizeof(uniforms.view_from_proj));
    uniforms.size[0] = static_cast<float>(resolved.width);
    uniforms.size[1] = static_cast<float>(resolved.height);
    uniforms.inv_size[0] = 1.0f / uniforms.size[0];
    uniforms.inv_size[1] = 1.0f / uniforms.size[1];
    set_light_direction(get_int_option(g_cvarLightDirection, 0), uniforms.light_dir_view);

    uniforms.toon_strength =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarToonStrength, 85), 0, 100)) /
        100.0f;
    uniforms.shadow_level =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarShadowLevel, 62), 10, 120)) /
        100.0f;
    uniforms.middle_level =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarMiddleLevel, 86), 10, 140)) /
        100.0f;
    uniforms.highlight_level = static_cast<float>(
                                   std::clamp<int64_t>(get_int_option(g_cvarHighlightLevel, 108), 10, 160)) /
                               100.0f;
    uniforms.band_smoothing = static_cast<float>(
                                  std::clamp<int64_t>(get_int_option(g_cvarBandSmoothing, 3), 0, 20)) /
                              100.0f;

    uniforms.outline_strength = static_cast<float>(
                                    std::clamp<int64_t>(get_int_option(g_cvarOutlineStrength, 82), 0, 100)) /
                                100.0f;
    uniforms.outline_thickness = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarOutlineThickness, 1), 1, 3));
    uniforms.depth_threshold = static_cast<float>(
                                   std::clamp<int64_t>(get_int_option(g_cvarDepthThreshold, 16), 1, 100)) /
                               10000.0f;
    uniforms.normal_threshold = static_cast<float>(
                                    std::clamp<int64_t>(get_int_option(g_cvarNormalThreshold, 22), 1, 100)) /
                                100.0f;

    uniforms.rim_strength =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarRimStrength, 18), 0, 100)) /
        100.0f;
    uniforms.rim_power = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarRimPower, 4), 1, 12));
    uniforms.saturation =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSaturation, 110), 0, 200)) /
        100.0f;
    uniforms.contrast =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarContrast, 105), 50, 200)) /
        100.0f;
    uniforms.debug_mode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 4));
    uniforms.reversed_z = g_deviceInfo.uses_reversed_z ? 1u : 0u;

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }

    const DrawPayload drawPayload{
        resolved.color, resolved.depth, uniformRange.offset, uniformRange.size};
    svc_gfx->push_draw(mod_ctx, g_drawType, &drawPayload, sizeof(drawPayload));
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle cvar,
    const char** options, uint32_t optionCount, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.options = options;
    control.option_count = optionCount;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "Cel Shading");
    add_toggle(left, "Enabled", g_cvarEnabled, "Applies cel shading to opaque world geometry.");
    static const char* kLightDirections[] = {"Upper Left", "Upper Right", "Front", "Overhead"};
    add_select(left, "Light Direction", g_cvarLightDirection, kLightDirections, 4,
        "View-space key light used to form the toon bands.");
    add_number(left, "Toon Strength", g_cvarToonStrength, 0, 100, 5, "%",
        "Blends between the original scene and three-band lighting.");
    add_number(left, "Shadow Level", g_cvarShadowLevel, 10, 120, 2, "%",
        "Brightness multiplier for the darkest lighting band.");
    add_number(left, "Middle Level", g_cvarMiddleLevel, 10, 140, 2, "%",
        "Brightness multiplier for the middle lighting band.");
    add_number(left, "Highlight Level", g_cvarHighlightLevel, 10, 160, 2, "%",
        "Brightness multiplier for the brightest lighting band.");
    add_number(left, "Band Smoothing", g_cvarBandSmoothing, 0, 20, 1, "%",
        "Softens the transition between toon bands. Set to zero for hard bands.");

    svc_ui->pane_add_section(mod_ctx, left, "Outlines");
    add_number(left, "Outline Strength", g_cvarOutlineStrength, 0, 100, 5, "%",
        "Darkens depth and normal discontinuities.");
    add_number(left, "Outline Thickness", g_cvarOutlineThickness, 1, 3, 1, " px",
        "Sampling radius used by the outline detector.");
    add_number(left, "Depth Threshold", g_cvarDepthThreshold, 1, 100, 1, nullptr,
        "Lower values detect smaller depth changes and produce more outlines.");
    add_number(left, "Normal Threshold", g_cvarNormalThreshold, 1, 100, 1, "%",
        "Lower values outline shallower changes in surface direction.");

    svc_ui->pane_add_section(mod_ctx, left, "Finishing");
    add_number(left, "Rim Light", g_cvarRimStrength, 0, 100, 5, "%",
        "Adds a cool edge highlight to lit silhouettes.");
    add_number(left, "Rim Power", g_cvarRimPower, 1, 12, 1, nullptr,
        "Higher values make the rim highlight thinner.");
    add_number(left, "Saturation", g_cvarSaturation, 0, 200, 5, "%",
        "Adjusts color saturation after toon lighting.");
    add_number(left, "Contrast", g_cvarContrast, 50, 200, 5, "%",
        "Adjusts final scene contrast.");

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugOptions[] = {"Off", "Toon Bands", "Normals", "Depth", "Outlines"};
    add_select(left, "Debug View", g_cvarDebugView, kDebugOptions, 5,
        "Shows the intermediate data used by the effect.");
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }

    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Controls";
    tabs[0].build = build_controls_tab;

    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open cel shading controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarEnabled;
    add_control(panel, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool_option(
    const char* name, bool defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register cel shading option");
    }
    return MOD_OK;
}

ModResult register_int_option(
    const char* name, int64_t defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register cel shading option");
    }
    return MOD_OK;
}

} // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "celshade.wgsl", &g_shaderSource);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load cel shading shader");
    }

    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) return result;
    result = register_int_option("lightDirection", 0, g_cvarLightDirection, error);
    if (result != MOD_OK) return result;
    result = register_int_option("toonStrength", 85, g_cvarToonStrength, error);
    if (result != MOD_OK) return result;
    result = register_int_option("shadowLevel", 62, g_cvarShadowLevel, error);
    if (result != MOD_OK) return result;
    result = register_int_option("middleLevel", 86, g_cvarMiddleLevel, error);
    if (result != MOD_OK) return result;
    result = register_int_option("highlightLevel", 108, g_cvarHighlightLevel, error);
    if (result != MOD_OK) return result;
    result = register_int_option("bandSmoothing", 3, g_cvarBandSmoothing, error);
    if (result != MOD_OK) return result;
    result = register_int_option("outlineStrength", 82, g_cvarOutlineStrength, error);
    if (result != MOD_OK) return result;
    result = register_int_option("outlineThickness", 1, g_cvarOutlineThickness, error);
    if (result != MOD_OK) return result;
    result = register_int_option("depthThreshold", 16, g_cvarDepthThreshold, error);
    if (result != MOD_OK) return result;
    result = register_int_option("normalThreshold", 22, g_cvarNormalThreshold, error);
    if (result != MOD_OK) return result;
    result = register_int_option("rimStrength", 18, g_cvarRimStrength, error);
    if (result != MOD_OK) return result;
    result = register_int_option("rimPower", 4, g_cvarRimPower, error);
    if (result != MOD_OK) return result;
    result = register_int_option("saturation", 110, g_cvarSaturation, error);
    if (result != MOD_OK) return result;
    result = register_int_option("contrast", 105, g_cvarContrast, error);
    if (result != MOD_OK) return result;
    result = register_int_option("debugMode", 0, g_cvarDebugView, error);
    if (result != MOD_OK) return result;

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query graphics device");
    }
    if (!build_pipeline()) {
        return mods::set_error(error, MOD_ERROR, "failed to create cel shading pipeline");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "Cel shading composite";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register cel shading draw type");
    }

    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_afterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register cel shading stage hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "celshade_mod ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_resource->free(mod_ctx, &g_shaderSource);

    if (g_pipeline != nullptr) {
        wgpuRenderPipelineRelease(g_pipeline);
        g_pipeline = nullptr;
    }
    if (g_pipelineLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_pipelineLayout);
        g_pipelineLayout = nullptr;
    }

    g_cvarEnabled = 0;
    g_cvarLightDirection = 0;
    g_cvarToonStrength = 0;
    g_cvarShadowLevel = 0;
    g_cvarMiddleLevel = 0;
    g_cvarHighlightLevel = 0;
    g_cvarBandSmoothing = 0;
    g_cvarOutlineStrength = 0;
    g_cvarOutlineThickness = 0;
    g_cvarDepthThreshold = 0;
    g_cvarNormalThreshold = 0;
    g_cvarRimStrength = 0;
    g_cvarRimPower = 0;
    g_cvarSaturation = 0;
    g_cvarContrast = 0;
    g_cvarDebugView = 0;
    g_drawType = 0;
    g_afterOpaqueHook = 0;
    g_controlsWindow = 0;
    return MOD_OK;
}

} // extern "C"
