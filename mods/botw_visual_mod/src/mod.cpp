// Breath of the Wild-inspired visual finishing pass.
//
// The hard toon band, colored indirect light, warm direct light, stylized GX specular and rim
// response are integrated in Aurora by scripts/apply_shipwright_celshade.py. This mod handles the
// effects that correctly belong after scene rendering: grade, shadow lift, atmosphere, selective
// bloom, lightweight contact AO and a restrained water response.

#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(LogService, svc_log);

namespace {

ConfigVarHandle g_enabled = 0;
ConfigVarHandle g_style = 0;
ConfigVarHandle g_shadowLift = 0;
ConfigVarHandle g_exposure = 0;
ConfigVarHandle g_contrast = 0;
ConfigVarHandle g_vibrance = 0;
ConfigVarHandle g_fogStrength = 0;
ConfigVarHandle g_fogStart = 0;
ConfigVarHandle g_fogEnd = 0;
ConfigVarHandle g_bloomStrength = 0;
ConfigVarHandle g_bloomThreshold = 0;
ConfigVarHandle g_waterStrength = 0;
ConfigVarHandle g_aoStrength = 0;
ConfigVarHandle g_debugMode = 0;

GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_captureCameraHook = 0;
GfxStageHookHandle g_beforeHudHook = 0;
UiWindowHandle g_controlsWindow = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_layout = nullptr;
CameraInfo g_camera = CAMERA_INFO_INIT;
bool g_cameraValid = false;
bool g_warnedNoTargets = false;
float g_time = 0.0f;

struct VisualUniforms {
    float inv_size[2];
    float size[2];
    float proj_from_view[16];
    float view_from_proj[16];
    float params0[4];
    float params1[4];
    float params2[4];
    float params3[4];
    uint32_t flags[4];
};
static_assert(sizeof(VisualUniforms) % 16 == 0);

struct DrawPayload {
    WGPUTextureView color;
    WGPUTextureView depth;
    uint32_t uniform_offset;
    uint32_t uniform_size;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

int64_t get_int(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool get_bool(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

float percent(ConfigVarHandle handle, int64_t fallback, int64_t low = 0, int64_t high = 100) {
    return static_cast<float>(std::clamp<int64_t>(get_int(handle, fallback), low, high)) / 100.0f;
}

WGPUShaderModule create_shader_module() {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    desc.label = {"BOTW visual pass", WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_deviceInfo.device, &desc);
}

bool build_pipeline() {
    WGPUShaderModule module = create_shader_module();
    if (module == nullptr) {
        return false;
    }

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = g_deviceInfo.color_format;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = g_deviceInfo.depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipeline = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipeline.label = {"BOTW visual pass", WGPU_STRLEN};
    pipeline.vertex.module = module;
    pipeline.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipeline.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline.depthStencil = &depth;
    pipeline.multisample.count = g_deviceInfo.sample_count;
    pipeline.fragment = &fragment;

    g_pipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipeline);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr) {
        return false;
    }
    g_layout = wgpuRenderPipelineGetBindGroupLayout(g_pipeline, 0);
    return g_layout != nullptr;
}

void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload) || g_pipeline == nullptr || g_layout == nullptr) {
        return;
    }

    DrawPayload data{};
    std::memcpy(&data, payload, sizeof(data));
    if (data.color == nullptr || data.depth == nullptr) {
        return;
    }

    WGPUBindGroupEntry entries[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.color;
    entries[1].binding = 1;
    entries[1].textureView = data.depth;
    entries[2].binding = 2;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniform_offset;
    entries[2].size = data.uniform_size;

    WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    groupDesc.layout = g_layout;
    groupDesc.entryCount = 3;
    groupDesc.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(ctx->device, &groupDesc);
    if (group == nullptr) {
        return;
    }

    wgpuRenderPassEncoderSetPipeline(ctx->pass, g_pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(group);
}

void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    g_cameraValid = false;
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext) ||
        stageCtx->game_view == nullptr)
    {
        return;
    }

    CameraInfo camera = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &camera) != MOD_OK) {
        return;
    }
    g_camera = camera;
    g_cameraValid = true;
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    if (!get_bool(g_enabled, true) || !g_cameraValid) {
        return;
    }

    GfxResolveDesc resolve = GFX_RESOLVE_DESC_INIT;
    resolve.color = true;
    resolve.depth = true;
    GfxResolvedTargets targets = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolve, &targets) != MOD_OK || targets.color == nullptr ||
        targets.depth == nullptr || targets.width == 0 || targets.height == 0)
    {
        if (!g_warnedNoTargets) {
            g_warnedNoTargets = true;
            svc_log->warn(mod_ctx, "color/depth snapshots unavailable; BOTW visual pass skipped");
        }
        return;
    }

    VisualUniforms uniforms{};
    uniforms.inv_size[0] = 1.0f / static_cast<float>(targets.width);
    uniforms.inv_size[1] = 1.0f / static_cast<float>(targets.height);
    uniforms.size[0] = static_cast<float>(targets.width);
    uniforms.size[1] = static_cast<float>(targets.height);
    std::memcpy(uniforms.proj_from_view, g_camera.proj_from_view, sizeof(uniforms.proj_from_view));
    std::memcpy(uniforms.view_from_proj, g_camera.view_from_proj, sizeof(uniforms.view_from_proj));

    uniforms.params0[0] = static_cast<float>(std::clamp<int64_t>(get_int(g_exposure, 5), -100, 100)) / 100.0f;
    uniforms.params0[1] = percent(g_contrast, 105, 50, 160);
    uniforms.params0[2] = percent(g_vibrance, 112, 50, 160);
    uniforms.params0[3] = percent(g_shadowLift, 24);

    uniforms.params1[0] = percent(g_fogStrength, 28);
    uniforms.params1[1] = static_cast<float>(std::clamp<int64_t>(get_int(g_fogStart, 2500), 0, 50000));
    uniforms.params1[2] = static_cast<float>(std::clamp<int64_t>(get_int(g_fogEnd, 18000), 100, 100000));
    if (uniforms.params1[2] <= uniforms.params1[1]) {
        uniforms.params1[2] = uniforms.params1[1] + 100.0f;
    }
    uniforms.params1[3] = percent(g_bloomStrength, 18);

    uniforms.params2[0] = percent(g_bloomThreshold, 72);
    uniforms.params2[1] = percent(g_waterStrength, 18);
    uniforms.params2[2] = percent(g_aoStrength, 20);
    uniforms.params2[3] = g_time;

    uniforms.params3[0] = percent(g_style, 78);
    uniforms.params3[1] = 0.50f;
    uniforms.params3[2] = 0.62f;
    uniforms.params3[3] = 0.34f;
    uniforms.flags[0] = g_deviceInfo.uses_reversed_z ? 1u : 0u;
    uniforms.flags[1] = static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_debugMode, 0), 0, 3));

    GfxRange range{};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &range) != MOD_OK) {
        return;
    }

    DrawPayload payload{targets.color, targets.depth, range.offset, range.size};
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
    g_time += 1.0f / 30.0f;
    if (g_time > 10000.0f) {
        g_time = 0.0f;
    }
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle var, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle var, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "BOTW Color and Lighting");
    add_toggle(left, "Enabled", g_enabled, "Enables the full BOTW-inspired finishing pass.");
    add_number(left, "Style Intensity", g_style, 0, 100, 5, "%",
        "Master blend for grading, atmosphere, AO, bloom and water.");
    add_number(left, "Shadow Lift", g_shadowLift, 0, 100, 5, "%",
        "Raises dark cel-shaded regions without softening the hard light boundary.");
    add_number(left, "Exposure", g_exposure, -100, 100, 5, "%",
        "Exposure in fractional stops. Small positive values suit the BOTW palette.");
    add_number(left, "Contrast", g_contrast, 50, 160, 5, "%",
        "Mid-tone contrast. Excessive values crush Twilight Princess texture detail.");
    add_number(left, "Vibrance", g_vibrance, 50, 160, 5, "%",
        "Boosts less-saturated colors more strongly than already vivid colors.");

    svc_ui->pane_add_section(mod_ctx, right, "Atmosphere and Materials");
    add_number(right, "Contact AO", g_aoStrength, 0, 100, 5, "%",
        "Lightweight depth-based grounding. Keep low on Android.");
    add_number(right, "Fog Strength", g_fogStrength, 0, 100, 5, "%",
        "Adds blue atmospheric separation with a restrained warm horizon.");
    add_number(right, "Fog Start", g_fogStart, 0, 50000, 250, "",
        "Camera distance where atmospheric fog begins.");
    add_number(right, "Fog End", g_fogEnd, 100, 100000, 500, "",
        "Camera distance where atmospheric fog reaches full configured strength.");
    add_number(right, "Bloom", g_bloomStrength, 0, 100, 5, "%",
        "Selective low-radius bloom for sky, magic, fire and emissive highlights.");
    add_number(right, "Bloom Threshold", g_bloomThreshold, 30, 100, 5, "%",
        "Higher values limit bloom to brighter pixels.");
    add_number(right, "Water Style", g_waterStrength, 0, 100, 5, "%",
        "Adds subtle cyan depth, shimmer and Fresnel response to likely water surfaces.");

    static const char* debugOptions[] = {"Off", "Shadow Mask", "Normals", "Fog Distance"};
    UiControlDesc debug = UI_CONTROL_DESC_INIT;
    debug.kind = UI_CONTROL_SELECT;
    debug.label = "Debug View";
    debug.binding = UI_BINDING_CONFIG_VAR;
    debug.config_var = g_debugMode;
    debug.options = debugOptions;
    debug.option_count = 4;
    add_control(right, debug);
    return MOD_OK;
}

void on_controls_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tab = UI_TAB_DESC_INIT;
    tab.title = "BOTW Visuals";
    tab.build = build_controls_tab;
    UiWindowDesc window = UI_WINDOW_DESC_INIT;
    window.tabs = &tab;
    window.tab_count = 1;
    window.on_closed = on_controls_closed;
    if (svc_ui->window_push(mod_ctx, &window, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open BOTW visual controls");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc toggle = UI_CONTROL_DESC_INIT;
    toggle.kind = UI_CONTROL_TOGGLE;
    toggle.label = "Enabled";
    toggle.binding = UI_BINDING_CONFIG_VAR;
    toggle.config_var = g_enabled;
    add_control(panel, toggle);

    UiControlDesc button = UI_CONTROL_DESC_INIT;
    button.kind = UI_CONTROL_BUTTON;
    button.label = "Open Controls";
    button.on_pressed = on_open_controls;
    add_control(panel, button);
    return MOD_OK;
}

ModResult register_bool(
    const char* name, bool defaultValue, ConfigVarHandle& handle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register BOTW visual toggle");
    }
    return MOD_OK;
}

ModResult register_int(
    const char* name, int64_t defaultValue, ConfigVarHandle& handle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register BOTW visual option");
    }
    return MOD_OK;
}

} // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "botw_visual.wgsl", &g_shaderSource);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load BOTW visual shader");
    }

    if ((result = register_bool("enabled", true, g_enabled, error)) != MOD_OK ||
        (result = register_int("styleIntensity", 78, g_style, error)) != MOD_OK ||
        (result = register_int("shadowLift", 24, g_shadowLift, error)) != MOD_OK ||
        (result = register_int("exposure", 5, g_exposure, error)) != MOD_OK ||
        (result = register_int("contrast", 105, g_contrast, error)) != MOD_OK ||
        (result = register_int("vibrance", 112, g_vibrance, error)) != MOD_OK ||
        (result = register_int("fogStrength", 28, g_fogStrength, error)) != MOD_OK ||
        (result = register_int("fogStart", 2500, g_fogStart, error)) != MOD_OK ||
        (result = register_int("fogEnd", 18000, g_fogEnd, error)) != MOD_OK ||
        (result = register_int("bloomStrength", 18, g_bloomStrength, error)) != MOD_OK ||
        (result = register_int("bloomThreshold", 72, g_bloomThreshold, error)) != MOD_OK ||
        (result = register_int("waterStrength", 18, g_waterStrength, error)) != MOD_OK ||
        (result = register_int("contactAo", 20, g_aoStrength, error)) != MOD_OK ||
        (result = register_int("debugMode", 0, g_debugMode, error)) != MOD_OK)
    {
        return result;
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query graphics device");
    }
    if (!build_pipeline()) {
        return mods::set_error(error, MOD_ERROR, "failed to create BOTW visual pipeline");
    }

    GfxDrawTypeDesc draw = GFX_DRAW_TYPE_DESC_INIT;
    draw.label = "BOTW visual pass";
    draw.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &draw, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register BOTW visual draw");
    }

    GfxStageHookDesc capture = GFX_STAGE_HOOK_DESC_INIT;
    capture.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &capture, &g_captureCameraHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register BOTW camera capture");
    }

    GfxStageHookDesc finish = GFX_STAGE_HOOK_DESC_INIT;
    finish.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &finish, &g_beforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register BOTW finishing pass");
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    svc_log->info(mod_ctx, "Breath of the Wild visual pass ready");
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
    if (g_layout != nullptr) {
        wgpuBindGroupLayoutRelease(g_layout);
        g_layout = nullptr;
    }
    g_drawType = 0;
    g_captureCameraHook = 0;
    g_beforeHudHook = 0;
    g_controlsWindow = 0;
    g_cameraValid = false;
    return MOD_OK;
}

} // extern "C"
