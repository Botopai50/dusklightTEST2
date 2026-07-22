struct VisualUniforms {
    inv_size: vec2f,
    size: vec2f,
    proj_from_view: mat4x4f,
    view_from_proj: mat4x4f,

    params0: vec4f, // exposure stops, contrast, vibrance, shadow lift
    params1: vec4f, // fog strength/start/end, bloom strength
    params2: vec4f, // bloom threshold, water, AO, time
    params3: vec4f, // style, texture style, edge AA, sky strength

    env0: vec4f, // time01, daylight, sunset, night
    env1: vec4f, // rain, clouds, wetness, twilight
    sky_top: vec4f,
    sky_horizon: vec4f,
    fog_color: vec4f,
    sun_color: vec4f,
    sun_dir_view: vec4f,
    sun_screen: vec4f,
    effects: vec4f, // foliage, wetness, quality scale, DOF scale

    flags: vec4u, // reversed Z, debug, quality, blur/cutscene
};

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var<uniform> u: VisualUniforms;

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

struct MaterialMasks {
    foliage: f32,
    skin: f32,
    metal: f32,
    water: f32,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOut {
    var positions = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f(3.0, -1.0),
        vec2f(-1.0, 3.0)
    );
    var out: VertexOut;
    let p = positions[vertex_index];
    out.position = vec4f(p, 0.0, 1.0);
    out.uv = p * vec2f(0.5, -0.5) + vec2f(0.5, 0.5);
    return out;
}

fn clamp_coord(coord: vec2i) -> vec2i {
    let limit = vec2i(textureDimensions(scene_color)) - vec2i(1);
    return clamp(coord, vec2i(0), limit);
}

fn clamp_uv(uv: vec2f) -> vec2f {
    return clamp(uv, vec2f(0.0), vec2f(1.0));
}

fn load_color(coord: vec2i) -> vec3f {
    return textureLoad(scene_color, clamp_coord(coord), 0).rgb;
}

fn load_color_uv(uv: vec2f) -> vec3f {
    return load_color(vec2i(clamp_uv(uv) * u.size));
}

fn load_depth(coord: vec2i) -> f32 {
    return textureLoad(scene_depth, clamp_coord(coord), 0).r;
}

fn load_depth_uv(uv: vec2f) -> f32 {
    let limit = vec2i(textureDimensions(scene_depth)) - vec2i(1);
    let coord = clamp(vec2i(clamp_uv(uv) * vec2f(textureDimensions(scene_depth))), vec2i(0), limit);
    return textureLoad(scene_depth, coord, 0).r;
}

fn depth_to_far(depth: f32) -> f32 {
    return select(depth, 1.0 - depth, u.flags.x != 0u);
}

fn geometry_mask_from_depth(depth: f32) -> f32 {
    return 1.0 - smoothstep(0.994, 1.0, depth_to_far(depth));
}

fn geometry_mask_uv(uv: vec2f) -> f32 {
    return geometry_mask_from_depth(load_depth_uv(uv));
}

fn coord_uv(coord: vec2i) -> vec2f {
    return (vec2f(coord) + vec2f(0.5)) * u.inv_size;
}

fn safe_normalize(value: vec3f, fallback: vec3f) -> vec3f {
    let length2 = dot(value, value);
    return select(fallback, value * inverseSqrt(length2), length2 > 0.0000001);
}

fn unproject_view(uv: vec2f, depth: f32) -> vec3f {
    let ndc = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    let view4 = u.view_from_proj * ndc;
    let safe_w = select(-0.000001, 0.000001, view4.w >= 0.0) + view4.w;
    return view4.xyz / safe_w;
}

fn view_position(coord: vec2i) -> vec3f {
    return unproject_view(coord_uv(coord), load_depth(coord));
}

fn view_distance_uv(uv: vec2f) -> f32 {
    return length(unproject_view(uv, load_depth_uv(uv)));
}

fn reconstruct_normal(coord: vec2i) -> vec3f {
    let center = view_position(coord);
    let left = view_position(coord + vec2i(-1, 0));
    let right = view_position(coord + vec2i(1, 0));
    let up = view_position(coord + vec2i(0, -1));
    let down = view_position(coord + vec2i(0, 1));

    let dx0 = center - left;
    let dx1 = right - center;
    let dy0 = center - up;
    let dy1 = down - center;
    let dx = select(dx1, dx0, dot(dx0, dx0) < dot(dx1, dx1));
    let dy = select(dy1, dy0, dot(dy0, dy0) < dot(dy1, dy1));
    return safe_normalize(cross(dy, dx), vec3f(0.0, 0.0, 1.0));
}

fn luminance(color: vec3f) -> f32 {
    return dot(color, vec3f(0.2126, 0.7152, 0.0722));
}

fn saturation_range(color: vec3f) -> f32 {
    return max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
}

fn hash12(p: vec2f) -> f32 {
    let p3 = fract(vec3f(p.xyx) * 0.1031);
    let q = p3 + dot(p3, p3.yzx + vec3f(33.33));
    return fract((q.x + q.y) * q.z);
}

fn smooth_noise(p: vec2f) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let s = f * f * (3.0 - 2.0 * f);
    let a = hash12(i);
    let b = hash12(i + vec2f(1.0, 0.0));
    let c = hash12(i + vec2f(0.0, 1.0));
    let d = hash12(i + vec2f(1.0, 1.0));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

fn classify_material(color: vec3f, normal: vec3f, geometry: f32) -> MaterialMasks {
    let luma = luminance(color);
    let sat = saturation_range(color);
    let green_bias = color.g - max(color.r, color.b) * 0.86;
    let warm_bias = color.r - color.b;
    let cyan_bias = min(color.g, color.b) - color.r * 0.72;

    var masks: MaterialMasks;
    masks.foliage = smoothstep(0.015, 0.20, green_bias) *
        smoothstep(0.04, 0.88, luma) * geometry;
    masks.skin = smoothstep(0.035, 0.22, warm_bias) *
        smoothstep(0.16, 0.78, luma) *
        (1.0 - smoothstep(0.48, 0.88, sat)) * geometry;
    masks.metal = smoothstep(0.46, 0.90, luma) *
        (1.0 - smoothstep(0.18, 0.55, sat)) * geometry;
    masks.water = smoothstep(0.025, 0.24, cyan_bias) *
        smoothstep(0.28, 0.86, abs(normal.y)) * geometry;
    return masks;
}

fn edge_aa(coord: vec2i, center: vec3f) -> vec3f {
    if u.params3.z <= 0.001 {
        return center;
    }
    let north = load_color(coord + vec2i(0, -1));
    let south = load_color(coord + vec2i(0, 1));
    let east = load_color(coord + vec2i(1, 0));
    let west = load_color(coord + vec2i(-1, 0));
    let center_luma = luminance(center);
    let min_luma = min(center_luma, min(min(luminance(north), luminance(south)),
        min(luminance(east), luminance(west))));
    let max_luma = max(center_luma, max(max(luminance(north), luminance(south)),
        max(luminance(east), luminance(west))));
    let contrast = max_luma - min_luma;
    let edge = smoothstep(0.035, 0.18, contrast);
    let average = (north + south + east + west) * 0.25;
    let strength = u.params3.z * mix(0.55, 0.85, f32(u.flags.z) * 0.5);
    return mix(center, average, edge * strength);
}

fn stylize_texture(coord: vec2i, color: vec3f, geometry: f32) -> vec3f {
    let amount = u.params3.y * geometry;
    if amount <= 0.001 {
        return color;
    }
    let center_luma = luminance(color);
    var weighted = color * 2.0;
    var weight = 2.0;
    let offsets = array<vec2i, 4>(
        vec2i(2, 0), vec2i(-2, 0), vec2i(0, 2), vec2i(0, -2)
    );
    for (var i = 0u; i < 4u; i += 1u) {
        let sample_color = load_color(coord + offsets[i]);
        let similarity = 1.0 - smoothstep(0.025, 0.24,
            abs(luminance(sample_color) - center_luma));
        weighted += sample_color * similarity;
        weight += similarity;
    }
    let painted = weighted / max(weight, 0.001);
    let levels = mix(48.0, 18.0, amount);
    let grouped = floor(max(painted, vec3f(0.0)) * levels + vec3f(0.5)) / levels;
    return mix(color, grouped, amount * 0.62);
}

fn contact_ao(coord: vec2i, center_distance: f32) -> f32 {
    let radius = select(2, 3, u.flags.z >= 2u);
    let scale = max(center_distance * 0.018, 2.0);
    var occ = 0.0;
    var samples = 4.0;
    occ += smoothstep(0.04, 1.0,
        max(center_distance - length(view_position(coord + vec2i(radius, 0))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0,
        max(center_distance - length(view_position(coord + vec2i(-radius, 0))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0,
        max(center_distance - length(view_position(coord + vec2i(0, radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0,
        max(center_distance - length(view_position(coord + vec2i(0, -radius))), 0.0) / scale);
    if u.flags.z >= 2u {
        occ += smoothstep(0.04, 1.0,
            max(center_distance - length(view_position(coord + vec2i(radius, radius))), 0.0) / scale);
        occ += smoothstep(0.04, 1.0,
            max(center_distance - length(view_position(coord + vec2i(-radius, radius))), 0.0) / scale);
        occ += smoothstep(0.04, 1.0,
            max(center_distance - length(view_position(coord + vec2i(radius, -radius))), 0.0) / scale);
        occ += smoothstep(0.04, 1.0,
            max(center_distance - length(view_position(coord + vec2i(-radius, -radius))), 0.0) / scale);
        samples = 8.0;
    }
    return clamp(occ / samples, 0.0, 1.0);
}

fn bright_part(color: vec3f, threshold: f32) -> vec3f {
    let luma = luminance(color);
    let weight = smoothstep(threshold, threshold + 0.24, luma);
    return color * weight;
}

fn sample_bloom(coord: vec2i, threshold: f32) -> vec3f {
    var sum = bright_part(load_color(coord), threshold) * 0.28;
    sum += bright_part(load_color(coord + vec2i(3, 0)), threshold) * 0.18;
    sum += bright_part(load_color(coord + vec2i(-3, 0)), threshold) * 0.18;
    sum += bright_part(load_color(coord + vec2i(0, 3)), threshold) * 0.18;
    sum += bright_part(load_color(coord + vec2i(0, -3)), threshold) * 0.18;
    if u.flags.z >= 2u {
        sum += bright_part(load_color(coord + vec2i(5, 5)), threshold) * 0.09;
        sum += bright_part(load_color(coord + vec2i(-5, 5)), threshold) * 0.09;
        sum += bright_part(load_color(coord + vec2i(5, -5)), threshold) * 0.09;
        sum += bright_part(load_color(coord + vec2i(-5, -5)), threshold) * 0.09;
        sum *= 0.74;
    }
    return sum;
}

fn shoreline_mask(coord: vec2i, center_depth: f32) -> f32 {
    let dx = abs(depth_to_far(load_depth(coord + vec2i(2, 0))) - depth_to_far(center_depth));
    let dy = abs(depth_to_far(load_depth(coord + vec2i(0, 2))) - depth_to_far(center_depth));
    return smoothstep(0.0004, 0.006, max(dx, dy));
}

fn apply_material_response(color_in: vec3f, masks: MaterialMasks, normal: vec3f,
    view_dir: vec3f, coord: vec2i, depth: f32) -> vec3f {
    var color = color_in;
    let sun_dir = safe_normalize(u.sun_dir_view.xyz, vec3f(0.0, 1.0, -1.0));
    let daylight = u.env0.y;
    let rain_wetness = u.env1.z * u.effects.y;

    // Skin keeps readable midtones instead of falling into the same dark band as cloth/stone.
    let skin_lift = masks.skin * (1.0 - smoothstep(0.28, 0.68, luminance(color)));
    color += vec3f(0.055, 0.030, 0.018) * skin_lift * u.params0.w * 0.55;

    // Foliage transmission gives leaves and grass their characteristic warm back-light.
    let transmission = pow(max(dot(-normal, sun_dir), 0.0), 1.65) *
        masks.foliage * u.effects.x * daylight * u.sun_dir_view.w;
    color += mix(vec3f(0.08, 0.20, 0.025), u.sun_color.rgb * vec3f(0.70, 1.00, 0.35), 0.62) *
        transmission;

    // Metal receives a compact colored highlight rather than a broad white gloss.
    let half_dir = safe_normalize(sun_dir + view_dir, vec3f(0.0, 0.0, 1.0));
    let metal_spec = smoothstep(0.82, 0.94, max(dot(normal, half_dir), 0.0)) *
        masks.metal * u.sun_dir_view.w;
    color += u.sun_color.rgb * metal_spec * 0.22;

    // Wet surfaces darken and pick up a sky-colored, view-dependent highlight.
    let exposed = smoothstep(0.05, 0.85, normal.y * 0.5 + 0.5);
    let not_absorbent = 1.0 - masks.foliage * 0.45 - masks.skin * 0.70;
    let wet = clamp(rain_wetness * exposed * not_absorbent, 0.0, 1.0);
    color *= 1.0 - wet * 0.13;
    let wet_fresnel = pow(clamp(1.0 - max(dot(normal, view_dir), 0.0), 0.0, 1.0), 3.0);
    let wet_spec = smoothstep(0.55, 0.92, max(dot(normal, half_dir), 0.0));
    color += mix(u.sky_horizon.rgb, u.sky_top.rgb, 0.55) *
        wet * (wet_fresnel * 0.16 + wet_spec * 0.22);

    if masks.water > 0.001 && u.params2.y > 0.001 {
        let fresnel = pow(clamp(1.0 - abs(dot(normal, view_dir)), 0.0, 1.0), 3.0);
        let shimmer = 0.5 + 0.5 * sin(
            (f32(coord.x) * 0.47 + f32(coord.y) * 0.31) + u.params2.w * 1.7);
        let shore = shoreline_mask(coord, depth);
        let water_tint = color * vec3f(0.80, 1.035, 1.105) +
            vec3f(0.015, 0.045, 0.075) * shimmer;
        let water_amount = masks.water * u.params2.y;
        color = mix(color, water_tint, water_amount * 0.58);
        color += mix(u.sky_horizon.rgb, u.sky_top.rgb, 0.65) *
            fresnel * water_amount * 0.22;
        color += vec3f(0.38, 0.52, 0.58) * shore * water_amount * 0.12;
    }

    return color;
}

fn analytic_sky(uv: vec2f) -> vec3f {
    let top_factor = pow(clamp(1.0 - uv.y, 0.0, 1.0), 0.72);
    var sky = mix(u.sky_horizon.rgb, u.sky_top.rgb, top_factor);

    let sun_delta = (uv - u.sun_screen.xy) * vec2f(u.size.x / max(u.size.y, 1.0), 1.0);
    let sun_distance = length(sun_delta);
    let sun_disk = 1.0 - smoothstep(0.006, 0.018, sun_distance);
    let sun_glow = exp(-sun_distance * 10.0) * u.sun_dir_view.w;
    sky += u.sun_color.rgb * (sun_disk * 0.90 + sun_glow * 0.24);

    let cloud_uv = uv * vec2f(5.0, 2.2) +
        vec2f(u.params2.w * 0.006, u.params2.w * 0.002);
    let cloud_noise = smooth_noise(cloud_uv) * 0.65 +
        smooth_noise(cloud_uv * 2.1 + vec2f(3.7, 1.2)) * 0.35;
    let cloud_mask = smoothstep(0.52 - u.env1.y * 0.16, 0.74, cloud_noise) *
        (1.0 - smoothstep(0.38, 1.0, uv.y));
    let cloud_color = mix(vec3f(0.62, 0.67, 0.72), vec3f(0.98, 0.96, 0.90),
        u.env0.y * 0.75 + u.env0.z * 0.25);
    sky = mix(sky, cloud_color, cloud_mask * (0.18 + u.env1.y * 0.48));
    return sky;
}

fn sun_shafts(uv: vec2f) -> vec3f {
    if u.sun_screen.z <= 0.001 || u.sun_dir_view.w <= 0.001 {
        return vec3f(0.0);
    }
    let steps = select(4u, select(6u, 8u, u.flags.z >= 2u), u.flags.z >= 1u);
    let to_sun = u.sun_screen.xy - uv;
    var sample_uv = uv;
    var visibility = 0.0;
    for (var i = 0u; i < 8u; i += 1u) {
        if i >= steps {
            break;
        }
        sample_uv += to_sun / f32(steps);
        let sky = 1.0 - geometry_mask_uv(sample_uv);
        let brightness = smoothstep(0.48, 1.0, luminance(load_color_uv(sample_uv)));
        visibility += sky * (0.35 + brightness * 0.65);
    }
    visibility /= max(f32(steps), 1.0);
    let radial = pow(clamp(1.0 - length(to_sun), 0.0, 1.0), 1.5);
    return u.sun_color.rgb * visibility * radial * u.sun_screen.z * u.sun_dir_view.w * 0.22;
}

fn selective_dof(uv: vec2f, color: vec3f, distance_to_camera: f32) -> vec3f {
    let enabled = u.flags.w != 0u && u.sun_screen.w > 0.001;
    if !enabled {
        return color;
    }
    let focus_distance = view_distance_uv(vec2f(0.5, 0.5));
    let coc = clamp(abs(distance_to_camera - focus_distance) / max(focus_distance, 1.0), 0.0, 1.0);
    let radius = mix(1.0, select(2.0, 3.5, u.flags.z >= 2u), coc) * u.sun_screen.w;
    let offset_x = vec2f(u.inv_size.x * radius, 0.0);
    let offset_y = vec2f(0.0, u.inv_size.y * radius);
    var blurred = load_color_uv(uv + offset_x) + load_color_uv(uv - offset_x) +
        load_color_uv(uv + offset_y) + load_color_uv(uv - offset_y);
    var count = 4.0;
    if u.flags.z >= 2u {
        let diagonal = vec2f(u.inv_size.x * radius, u.inv_size.y * radius);
        blurred += load_color_uv(uv + diagonal) + load_color_uv(uv - diagonal) +
            load_color_uv(uv + vec2f(diagonal.x, -diagonal.y)) +
            load_color_uv(uv + vec2f(-diagonal.x, diagonal.y));
        count = 8.0;
    }
    blurred /= count;
    return mix(color, blurred, coc * u.sun_screen.w * 0.72);
}

fn aces_filmic(x: vec3f) -> vec3f {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + vec3f(b))) /
        (x * (c * x + vec3f(d)) + vec3f(e)), vec3f(0.0), vec3f(1.0));
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4f {
    let coord = clamp_coord(vec2i(in.position.xy));
    let depth = load_depth(coord);
    let geometry = geometry_mask_from_depth(depth);
    let sky_mask = 1.0 - geometry;
    let view_pos = unproject_view(in.uv, depth);
    let distance_to_camera = length(view_pos);
    let normal = reconstruct_normal(coord);
    let view_dir = safe_normalize(-view_pos, vec3f(0.0, 0.0, 1.0));

    let style = clamp(u.params3.x, 0.0, 1.0);
    var color = edge_aa(coord, max(load_color(coord), vec3f(0.0)));
    color = stylize_texture(coord, color, geometry);

    let masks = classify_material(color, normal, geometry);

    if u.params2.z > 0.001 && geometry > 0.001 {
        let ao = contact_ao(coord, distance_to_camera);
        let ao_factor = 1.0 - ao * u.params2.z * 0.30 * style * geometry;
        color *= ao_factor;
    }

    color = apply_material_response(color, masks, normal, view_dir, coord, depth);

    if sky_mask > 0.001 && u.params3.w > 0.001 {
        let sky = analytic_sky(in.uv);
        color = mix(color, sky, sky_mask * u.params3.w * style * 0.78);
    }

    if u.params1.w > 0.001 {
        color += sample_bloom(coord, u.params2.x) * u.params1.w * style;
    }

    if u.params1.x > 0.001 && geometry > 0.001 {
        let fog_range = max(u.params1.z - u.params1.y, 1.0);
        var fog_amount = smoothstep(0.0, 1.0,
            (distance_to_camera - u.params1.y) / fog_range);
        fog_amount *= 1.0 + u.env1.x * 0.55 + u.env1.y * 0.25;
        let height_bias = 1.0 - clamp(abs(in.uv.y - 0.52) * 2.0, 0.0, 1.0);
        let fog_color = mix(u.fog_color.rgb, u.sky_horizon.rgb, height_bias * 0.42);
        color = mix(color, fog_color, clamp(fog_amount * u.params1.x * style, 0.0, 0.92));
    }

    color += sun_shafts(in.uv) * style;
    color = selective_dof(in.uv, color, distance_to_camera);

    // Dynamic BOTW grade: cool lifted shadows, warm daylight, softer rain and blue night.
    color *= exp2(u.params0.x);
    var luma = luminance(color);
    let shadow_mask = 1.0 - smoothstep(0.16, 0.58, luma);
    let highlight_mask = smoothstep(0.50, 0.92, luma);
    let cool_shadow = mix(vec3f(0.965, 0.992, 1.055), vec3f(0.90, 0.96, 1.10), u.env0.w);
    let warm_highlight = mix(vec3f(1.035, 1.012, 0.975),
        vec3f(1.10, 1.025, 0.90), u.env0.z);
    color += shadow_mask * u.params0.w * vec3f(0.10, 0.14, 0.20) *
        (0.55 + u.env0.w * 0.35) * style;
    color *= mix(vec3f(1.0), cool_shadow, shadow_mask * style * 0.55);
    color *= mix(vec3f(1.0), warm_highlight,
        highlight_mask * style * (0.30 + u.env0.y * 0.35));
    color = mix(color, color * vec3f(0.93, 0.97, 1.02),
        u.env1.x * style * 0.22);

    luma = luminance(color);
    let saturation = saturation_range(color);
    let vibrance_weight = 1.0 - clamp(saturation, 0.0, 1.0);
    color = mix(vec3f(luma), color,
        1.0 + (u.params0.z - 1.0) * vibrance_weight * style);
    color = (color - vec3f(0.5)) * mix(1.0, u.params0.y, style) + vec3f(0.5);

    let filmic_amount = style * mix(0.26, 0.40, f32(u.flags.z) * 0.5);
    color = mix(color, aces_filmic(max(color, vec3f(0.0))), filmic_amount);

    if u.flags.y == 1u {
        return vec4f(masks.foliage, masks.skin, max(masks.metal, masks.water), 1.0);
    }
    if u.flags.y == 2u {
        return vec4f(normal * 0.5 + vec3f(0.5), 1.0);
    }
    if u.flags.y == 3u {
        let fog_debug = clamp((distance_to_camera - u.params1.y) /
            max(u.params1.z - u.params1.y, 1.0), 0.0, 1.0);
        return vec4f(vec3f(fog_debug), 1.0);
    }
    if u.flags.y == 4u {
        return vec4f(vec3f(u.env1.z * geometry), 1.0);
    }
    if u.flags.y == 5u {
        return vec4f(masks.foliage, masks.foliage * u.env0.y, 0.0, 1.0);
    }
    if u.flags.y == 6u {
        return vec4f(vec3f(sky_mask), 1.0);
    }
    if u.flags.y == 7u {
        return vec4f(sun_shafts(in.uv) * 4.0, 1.0);
    }
    if u.flags.y == 8u {
        let focus = view_distance_uv(vec2f(0.5, 0.5));
        let coc = clamp(abs(distance_to_camera - focus) / max(focus, 1.0), 0.0, 1.0);
        return vec4f(vec3f(coc), 1.0);
    }

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
