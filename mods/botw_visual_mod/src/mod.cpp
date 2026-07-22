// Breath of the Wild-inspired visual suite v2.
//
// Material-integrated toon lighting is injected into Aurora by
// scripts/apply_shipwright_celshade.py. This module handles effects that belong after the
// opaque/translucent world has been rendered: dynamic sky and weather grading, lightweight
// material classification, wetness, foliage transmission, AO, water, bloom, edge AA,
// selective depth of field and restrained sun shafts.

#include "d/d_kankyo.h"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
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
ConfigVarHandle g_quality = 0;
ConfigVarHandle g_adaptiveQuality = 0;
ConfigVarHandle g_dynamicEnvironment = 0;
ConfigVarHandle g_style = 0;
ConfigVarHandle g_shadowLift = 0;
ConfigVarHandle g_exposure = 0;
ConfigVarHandle g_contrast = 0;
ConfigVarHandle g_vibrance = 0;
ConfigVarHandle g_textureStyle = 0;
ConfigVarHandle g_fogStrength = 0;
ConfigVarHandle g_fogStart = 0;
ConfigVarHandle g_fogEnd = 0;
ConfigVarHandle g_bloomStrength = 0;
ConfigVarHandle g_bloomThreshold = 0;
ConfigVarHandle g_waterStrength = 0;
ConfigVarHandle g_aoStrength = 0;
ConfigVarHandle g_foliageStrength = 0;
ConfigVarHandle g_wetnessStrength = 0;
ConfigVarHandle g_edgeAaStrength = 0;
ConfigVarHandle g_sunRayStrength = 0;
ConfigVarHandle g_dofStrength = 0;
ConfigVarHandle g_skyStrength = 0;
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
float g_wetness = 0.0f;

struct VisualUniforms {
    float inv_size[2];
    float size[2];
    float proj_from_view[16];
    float view_from_proj[16];

    float params0[4]; // exposure stops, contrast, vibrance, shadow lift
    float params1[4]; // fog strength/start/end, bloom strength
    float params2[4]; // bloom threshold, water, AO, time
    float params3[4]; // master style, texture style, edge AA, sky strength

    float env0[4]; // time01, daylight, sunset, night
    float env1[4]; // rain, clouds, wetness, twilight
    float sky_top[4];
    float sky_horizon[4];
    float fog_color[4];
    float sun_color[4];
    float sun_dir_view[4]; // xyz direction toward sun/moon, w visibility
    float sun_screen[4]; // xy screen position, z rays, w DOF
    float effects[4]; // foliage, wetness response, AO quality scale, DOF focus scale

    uint32_t flags[4]; // reversed Z, debug, quality, blur/cutscene
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

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float wrap_degrees(float value) {
    if (!std::isfinite(value)) {
        return 180.0f;
    }
    value = std::fmod(value, 360.0f);
    return value < 0.0f ? value + 360.0f : value;
}

float circular_peak(float value, float center, float halfWidth) {
    float delta = std::fabs(wrap_degrees(value) - wrap_degrees(center));
    delta = std::min(delta, 360.0f - delta);
    return 1.0f - clamp01(delta / std::max(halfWidth, 0.001f));
}

void color_s10_to_float(const GXColorS10& color, float out[4], float fallbackR, float fallbackG,
    float fallbackB) {
    const auto convert = [](int value, float fallback) {
        if (value < -255 || value > 511) {
            return fallback;
        }
        return clamp01(static_cast<float>(value) / 255.0f);
    };
    out[0] = convert(color.r, fallbackR);
    out[1] = convert(color.g, fallbackG);
    out[2] = convert(color.b, fallbackB);
    out[3] = 1.0f;
}

void normalize3(float value[3], const float fallback[3]) {
    const float length =
        std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!std::isfinite(length) || length < 0.0001f) {
        value[0] = fallback[0];
        value[1] = fallback[1];
        value[2] = fallback[2];
        return;
    }
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

void world_direction_to_view(const float world[3], float view[3]) {
    for (int row = 0; row < 3; ++row) {
        view[row] = g_camera.view_from_world[0 * 4 + row] * world[0] +
                    g_camera.view_from_world[1 * 4 + row] * world[1] +
                    g_camera.view_from_world[2 * 4 + row] * world[2];
    }
    static constexpr float kFallback[3] = {0.0f, 1.0f, -1.0f};
    normalize3(view, kFallback);
}

bool project_world_to_screen(const float world[3], float screen[2]) {
    float clip[4]{};
    for (int row = 0; row < 4; ++row) {
        clip[row] = g_camera.proj_from_world[0 * 4 + row] * world[0] +
                    g_camera.proj_from_world[1 * 4 + row] * world[1] +
                    g_camera.proj_from_world[2 * 4 + row] * world[2] +
                    g_camera.proj_from_world[3 * 4 + row];
    }
    if (!std::isfinite(clip[3]) || clip[3] <= 0.0001f) {
        screen[0] = 0.5f;
        screen[1] = -1.0f;
        return false;
    }
    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    screen[0] = ndcX * 0.5f + 0.5f;
    screen[1] = 0.5f - ndcY * 0.5f;
    return std::isfinite(screen[0]) && std::isfinite(screen[1]);
}

uint32_t resolved_quality(uint32_t width, uint32_t height) {
    int64_t configured = std::clamp<int64_t>(get_int(g_quality, 1), 0, 2);
    if (!get_bool(g_adaptiveQuality, true)) {
        return static_cast<uint32_t>(configured);
    }

#if defined(__ANDROID__)
    configured = std::min<int64_t>(configured, 1);
#endif

    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > 2560ull * 1440ull) {
        configured = std::min<int64_t>(configured, 0);
    } else if (pixels > 1920ull * 1080ull) {
        configured = std::min<int64_t>(configured, 1);
    }
    return static_cast<uint32_t>(configured);
}

void fill_environment(VisualUniforms& uniforms) {
    dScnKy_env_light_c* env = dKy_getEnvlight();
    const bool dynamic = get_bool(g_dynamicEnvironment, true) && env != nullptr;

    float daytime = dynamic ? wrap_degrees(env->daytime) : 180.0f;
    const float noon = circular_peak(daytime, 180.0f, 90.0f);
    const float sunrise = circular_peak(daytime, 90.0f, 42.0f);
    const float sunset = circular_peak(daytime, 270.0f, 42.0f);
    const float sunsetAmount = std::max(sunrise, sunset);
    const float daylight = clamp01(noon * 1.15f + sunsetAmount * 0.45f);
    const float night = 1.0f - daylight;

    float rain = 0.0f;
    float clouds = 0.0f;
    float twilight = 0.0f;
    bool blur = false;
    if (dynamic) {
        rain = clamp01(static_cast<float>(std::max(env->raincnt, env->base_raincnt)) / 120.0f);
        clouds = clamp01(std::max(env->mVrkumoStrength, static_cast<float>(env->wether != 0)));
        twilight = clamp01(std::max(env->senses_effect_strength,
            static_cast<float>(env->mColPatMode != 0 || env->mColpatWeather != 0) * 0.35f));
        blur = env->is_blure != 0;
    }

    const float wetTarget = clamp01(rain * 1.15f + clouds * 0.15f);
    const float wetRate = wetTarget > g_wetness ? 0.025f : 0.0018f;
    g_wetness += (wetTarget - g_wetness) * wetRate;
    g_wetness = clamp01(g_wetness);

    uniforms.env0[0] = daytime / 360.0f;
    uniforms.env0[1] = daylight;
    uniforms.env0[2] = sunsetAmount;
    uniforms.env0[3] = night;
    uniforms.env1[0] = rain;
    uniforms.env1[1] = clouds;
    uniforms.env1[2] = g_wetness;
    uniforms.env1[3] = twilight;

    const float fallbackTop[3] = {
        0.18f + 0.30f * daylight,
        0.30f + 0.38f * daylight,
        0.52f + 0.38f * daylight,
    };
    const float fallbackHorizon[3] = {
        0.34f + 0.40f * daylight + 0.18f * sunsetAmount,
        0.42f + 0.34f * daylight + 0.06f * sunsetAmount,
        0.55f + 0.25f * daylight,
    };
    const float fallbackFog[3] = {
        0.30f + 0.36f * daylight + 0.12f * sunsetAmount,
        0.38f + 0.34f * daylight + 0.04f * sunsetAmount,
        0.50f + 0.32f * daylight,
    };

    if (dynamic) {
        color_s10_to_float(env->vrbox_sky_col, uniforms.sky_top, fallbackTop[0], fallbackTop[1],
            fallbackTop[2]);
        color_s10_to_float(env->vrbox_kasumi_outer_col, uniforms.sky_horizon,
            fallbackHorizon[0], fallbackHorizon[1], fallbackHorizon[2]);
        color_s10_to_float(
            env->fog_col, uniforms.fog_color, fallbackFog[0], fallbackFog[1], fallbackFog[2]);
    } else {
        std::memcpy(uniforms.sky_top, fallbackTop, sizeof(fallbackTop));
        std::memcpy(uniforms.sky_horizon, fallbackHorizon, sizeof(fallbackHorizon));
        std::memcpy(uniforms.fog_color, fallbackFog, sizeof(fallbackFog));
        uniforms.sky_top[3] = uniforms.sky_horizon[3] = uniforms.fog_color[3] = 1.0f;
    }

    // Keep the environment palette recognizable while nudging it toward BOTW's clean atmosphere.
    for (int i = 0; i < 3; ++i) {
        uniforms.sky_top[i] = clamp01(uniforms.sky_top[i] * (1.0f - clouds * 0.18f));
        uniforms.sky_horizon[i] =
            clamp01(uniforms.sky_horizon[i] * (1.0f - clouds * 0.12f));
        uniforms.fog_color[i] = clamp01(uniforms.fog_color[i] * (1.0f - rain * 0.15f));
    }
    uniforms.sky_top[2] = clamp01(uniforms.sky_top[2] + 0.08f * daylight);
    uniforms.sky_horizon[0] =
        clamp01(uniforms.sky_horizon[0] + 0.18f * sunsetAmount);
    uniforms.sky_horizon[1] =
        clamp01(uniforms.sky_horizon[1] + 0.06f * sunsetAmount);

    uniforms.sun_color[0] = clamp01(0.60f + 0.40f * daylight + 0.30f * sunsetAmount);
    uniforms.sun_color[1] = clamp01(0.62f + 0.34f * daylight + 0.08f * sunsetAmount);
    uniforms.sun_color[2] = clamp01(0.72f + 0.22f * daylight - 0.18f * sunsetAmount);
    uniforms.sun_color[3] = 1.0f;

    float sunWorld[3] = {0.2f, 0.8f, -0.5f};
    float sunWorldPos[3] = {10000.0f, 40000.0f, -20000.0f};
    if (dynamic) {
        const cXyz& source = daylight > 0.08f ? env->sun_pos : env->moon_pos;
        sunWorld[0] = source.x;
        sunWorld[1] = source.y;
        sunWorld[2] = source.z;
        sunWorldPos[0] = source.x;
        sunWorldPos[1] = source.y;
        sunWorldPos[2] = source.z;
    }
    static constexpr float kWorldFallback[3] = {0.2f, 0.8f, -0.5f};
    normalize3(sunWorld, kWorldFallback);
    world_direction_to_view(sunWorld, uniforms.sun_dir_view);
    uniforms.sun_dir_view[3] =
        clamp01((daylight + night * 0.30f) * (1.0f - clouds * 0.72f));

    float sunScreen[2]{0.5f, -1.0f};
    const bool sunVisible = project_world_to_screen(sunWorldPos, sunScreen);
    uniforms.sun_screen[0] = sunScreen[0];
    uniforms.sun_screen[1] = sunScreen[1];
    uniforms.sun_screen[2] = sunVisible ? percent(g_sunRayStrength, 18) : 0.0f;
    uniforms.sun_screen[3] = percent(g_dofStrength, 18);
    uniforms.flags[3] = blur ? 1u : 0u;
}

WGPUShaderModule create_shader_module() {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    desc.label = {"BOTW visual suite v2", WGPU_STRLEN};
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
    pipeline.label = {"BOTW visual suite v2", WGPU_STRLEN};
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
            svc_log->warn(mod_ctx, "color/depth snapshots unavailable; BOTW visual suite skipped");
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

    uniforms.params0[0] =
        static_cast<float>(std::clamp<int64_t>(get_int(g_exposure, 5), -100, 100)) / 100.0f;
    uniforms.params0[1] = percent(g_contrast, 105, 50, 160);
    uniforms.params0[2] = percent(g_vibrance, 112, 50, 160);
    uniforms.params0[3] = percent(g_shadowLift, 28);

    uniforms.params1[0] = percent(g_fogStrength, 30);
    uniforms.params1[1] =
        static_cast<float>(std::clamp<int64_t>(get_int(g_fogStart, 2200), 0, 50000));
    uniforms.params1[2] =
        static_cast<float>(std::clamp<int64_t>(get_int(g_fogEnd, 19000), 100, 100000));
    if (uniforms.params1[2] <= uniforms.params1[1]) {
        uniforms.params1[2] = uniforms.params1[1] + 100.0f;
    }
    uniforms.params1[3] = percent(g_bloomStrength, 18);

    uniforms.params2[0] = percent(g_bloomThreshold, 72);
    uniforms.params2[1] = percent(g_waterStrength, 28);
    uniforms.params2[2] = percent(g_aoStrength, 18);
    uniforms.params2[3] = g_time;

    uniforms.params3[0] = percent(g_style, 82);
    uniforms.params3[1] = percent(g_textureStyle, 22);
    uniforms.params3[2] = percent(g_edgeAaStrength, 35);
    uniforms.params3[3] = percent(g_skyStrength, 60);

    uniforms.effects[0] = percent(g_foliageStrength, 38);
    uniforms.effects[1] = percent(g_wetnessStrength, 45);
    uniforms.effects[2] = 1.0f;
    uniforms.effects[3] = 1.0f;

    uniforms.flags[0] = g_deviceInfo.uses_reversed_z ? 1u : 0u;
    uniforms.flags[1] =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_debugMode, 0), 0, 8));
    uniforms.flags[2] = resolved_quality(targets.width, targets.height);
    uniforms.flags[3] = 0u;
    fill_environment(uniforms);

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

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle var, const char** options,
    uint32_t optionCount, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    control.options = options;
    control.option_count = optionCount;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "BOTW Suite");
    add_toggle(left, "Enabled", g_enabled, "Enables the complete BOTW-inspired visual suite.");
    static const char* qualityOptions[] = {"Low / Mobile", "Medium", "High"};
    add_select(left, "Quality", g_quality, qualityOptions, 3,
        "Controls sample counts for bloom, AO, sun shafts, AA and depth of field.");
    add_toggle(left, "Adaptive Quality", g_adaptiveQuality,
        "Automatically caps expensive effects at high output resolutions and on Android.");
    add_toggle(left, "Dynamic Time and Weather", g_dynamicEnvironment,
        "Uses Twilight Princess time, rain, clouds, sky, fog and sun position.");
    add_number(left, "Style Intensity", g_style, 0, 100, 5, "%",
        "Master blend for the complete suite.");
    add_number(left, "Shadow Lift", g_shadowLift, 0, 100, 5, "%",
        "Raises dark cel-shaded regions without softening their boundary.");
    add_number(left, "Exposure", g_exposure, -100, 100, 5, "%",
        "Exposure in fractional stops.");
    add_number(left, "Contrast", g_contrast, 50, 160, 5, "%", "Mid-tone contrast.");
    add_number(left, "Vibrance", g_vibrance, 50, 160, 5, "%",
        "Boosts muted colors more than already saturated colors.");
    add_number(left, "Texture Stylization", g_textureStyle, 0, 100, 5, "%",
        "Reduces noisy texture detail and gently groups colors like painted surfaces.");
    add_number(left, "Edge AA", g_edgeAaStrength, 0, 100, 5, "%",
        "Lightweight edge smoothing and toon-band stability.");

    svc_ui->pane_add_section(mod_ctx, right, "World and Materials");
    add_number(right, "Atmospheric Sky", g_skyStrength, 0, 100, 5, "%",
        "Blends an analytic sky, horizon, sun glow and cloud response over the original sky.");
    add_number(right, "Fog Strength", g_fogStrength, 0, 100, 5, "%",
        "Depth atmosphere synchronized with time and weather.");
    add_number(right, "Fog Start", g_fogStart, 0, 50000, 250, "",
        "Camera distance where atmospheric fog begins.");
    add_number(right, "Fog End", g_fogEnd, 100, 100000, 500, "",
        "Camera distance where atmospheric fog reaches full strength.");
    add_number(right, "Contact AO", g_aoStrength, 0, 100, 5, "%",
        "Lightweight depth-based grounding.");
    add_number(right, "Bloom", g_bloomStrength, 0, 100, 5, "%",
        "Selective glow for bright sky, magic, fire and emissive surfaces.");
    add_number(right, "Bloom Threshold", g_bloomThreshold, 30, 120, 5, "%",
        "Higher values limit bloom to brighter pixels.");
    add_number(right, "Water Style", g_waterStrength, 0, 100, 5, "%",
        "Depth tint, shimmer, Fresnel and shoreline emphasis.");
    add_number(right, "Foliage Transmission", g_foliageStrength, 0, 100, 5, "%",
        "Warm back-light passing through likely leaves and grass.");
    add_number(right, "Rain Wetness", g_wetnessStrength, 0, 100, 5, "%",
        "Darkens exposed materials and adds a sky-colored wet highlight.");
    add_number(right, "Sun Rays", g_sunRayStrength, 0, 100, 5, "%",
        "Restrained radial shafts when the sun is visible.");
    add_number(right, "Selective DOF", g_dofStrength, 0, 100, 5, "%",
        "Depth blur only during scenes where the game requests blur.");

    static const char* debugOptions[] = {"Off", "Material Masks", "Normals", "Fog",
        "Wetness", "Foliage", "Sky Mask", "Sun Rays", "Depth of Field"};
    add_select(right, "Debug View", g_debugMode, debugOptions, 9,
        "Displays the intermediate masks used by the BOTW suite.");
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
    tab.title = "BOTW Visual Suite v2";
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

#if defined(__ANDROID__)
    constexpr int64_t kDefaultQuality = 0;
#else
    constexpr int64_t kDefaultQuality = 1;
#endif

    if ((result = register_bool("effectEnabled", true, g_enabled, error)) != MOD_OK ||
        (result = register_int("quality", kDefaultQuality, g_quality, error)) != MOD_OK ||
        (result = register_bool("adaptiveQuality", true, g_adaptiveQuality, error)) != MOD_OK ||
        (result = register_bool("dynamicEnvironment", true, g_dynamicEnvironment, error)) != MOD_OK ||
        (result = register_int("styleIntensity", 82, g_style, error)) != MOD_OK ||
        (result = register_int("shadowLift", 28, g_shadowLift, error)) != MOD_OK ||
        (result = register_int("exposure", 5, g_exposure, error)) != MOD_OK ||
        (result = register_int("contrast", 105, g_contrast, error)) != MOD_OK ||
        (result = register_int("vibrance", 112, g_vibrance, error)) != MOD_OK ||
        (result = register_int("textureStyle", 22, g_textureStyle, error)) != MOD_OK ||
        (result = register_int("fogStrength", 30, g_fogStrength, error)) != MOD_OK ||
        (result = register_int("fogStart", 2200, g_fogStart, error)) != MOD_OK ||
        (result = register_int("fogEnd", 19000, g_fogEnd, error)) != MOD_OK ||
        (result = register_int("bloomStrength", 18, g_bloomStrength, error)) != MOD_OK ||
        (result = register_int("bloomThreshold", 72, g_bloomThreshold, error)) != MOD_OK ||
        (result = register_int("waterStrength", 28, g_waterStrength, error)) != MOD_OK ||
        (result = register_int("contactAo", 18, g_aoStrength, error)) != MOD_OK ||
        (result = register_int("foliageTransmission", 38, g_foliageStrength, error)) != MOD_OK ||
        (result = register_int("wetnessStrength", 45, g_wetnessStrength, error)) != MOD_OK ||
        (result = register_int("edgeAaStrength", 35, g_edgeAaStrength, error)) != MOD_OK ||
        (result = register_int("sunRayStrength", 18, g_sunRayStrength, error)) != MOD_OK ||
        (result = register_int("dofStrength", 18, g_dofStrength, error)) != MOD_OK ||
        (result = register_int("skyStrength", 60, g_skyStrength, error)) != MOD_OK ||
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
    draw.label = "BOTW visual suite v2";
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

    svc_log->info(mod_ctx, "Breath of the Wild visual suite v2 ready");
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
    g_wetness = 0.0f;
    return MOD_OK;
}

} // extern "C"
