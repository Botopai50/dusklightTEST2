struct VisualUniforms {
    inv_size: vec2f,
    size: vec2f,
    proj_from_view: mat4x4f,
    view_from_proj: mat4x4f,
    params0: vec4f, // exposure stops, contrast, vibrance, shadow lift
    params1: vec4f, // fog strength, fog start, fog end, bloom strength
    params2: vec4f, // bloom threshold, water strength, AO strength, time
    params3: vec4f, // master style, warm highlights, cool shadows, filmic strength
    flags: vec4u,   // x: reversed Z, y: debug mode
};

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var<uniform> u: VisualUniforms;

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
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

fn load_color(coord: vec2i) -> vec3f {
    return textureLoad(scene_color, clamp_coord(coord), 0).rgb;
}

fn load_depth(coord: vec2i) -> f32 {
    return textureLoad(scene_depth, clamp_coord(coord), 0).r;
}

fn coord_uv(coord: vec2i) -> vec2f {
    return (vec2f(coord) + vec2f(0.5)) * u.inv_size;
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
    return normalize(cross(dy, dx));
}

fn luminance(color: vec3f) -> f32 {
    return dot(color, vec3f(0.2126, 0.7152, 0.0722));
}

fn bright_part(color: vec3f, threshold: f32) -> vec3f {
    let luma = luminance(color);
    let weight = smoothstep(threshold, min(threshold + 0.22, 1.0), luma);
    return color * weight;
}

fn sample_bloom(coord: vec2i, threshold: f32) -> vec3f {
    var sum = bright_part(load_color(coord), threshold) * 0.20;
    sum += bright_part(load_color(coord + vec2i(2, 0)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(-2, 0)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(0, 2)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(0, -2)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(3, 3)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(-3, 3)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(3, -3)), threshold) * 0.10;
    sum += bright_part(load_color(coord + vec2i(-3, -3)), threshold) * 0.10;
    return sum;
}

fn contact_ao(coord: vec2i, center_distance: f32) -> f32 {
    let radius = 2;
    let scale = max(center_distance * 0.018, 2.0);
    var occ = 0.0;
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(radius, 0))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(-radius, 0))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(0, radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(0, -radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(radius, radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(-radius, radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(radius, -radius))), 0.0) / scale);
    occ += smoothstep(0.04, 1.0, max(center_distance - length(view_position(coord + vec2i(-radius, -radius))), 0.0) / scale);
    return clamp(occ * 0.125, 0.0, 1.0);
}

fn aces_filmic(x: vec3f) -> vec3f {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + vec3f(b))) / (x * (c * x + vec3f(d)) + vec3f(e)), vec3f(0.0), vec3f(1.0));
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4f {
    let coord = clamp_coord(vec2i(in.position.xy));
    let raw = load_color(coord);
    let depth = load_depth(coord);
    let view_pos = unproject_view(in.uv, depth);
    let distance_to_camera = length(view_pos);
    let normal = reconstruct_normal(coord);
    let view_dir = normalize(-view_pos);

    let style = clamp(u.params3.x, 0.0, 1.0);
    var color = max(raw, vec3f(0.0));

    // Lightweight contact AO: restrained enough to reinforce grounding without dirty outlines.
    if (u.params2.z > 0.001) {
        let ao = contact_ao(coord, distance_to_camera);
        let ao_factor = 1.0 - ao * u.params2.z * 0.34 * style;
        color *= ao_factor;
    }

    // Cyan + approximately horizontal + non-sky mask for a cheap stylized water response.
    if (u.params2.y > 0.001) {
        let cyan_bias = min(color.g, color.b) - color.r * 0.72;
        let cyan_mask = smoothstep(0.025, 0.24, cyan_bias);
        let horizontal_mask = smoothstep(0.30, 0.82, abs(normal.y));
        let far_depth = select(depth, 1.0 - depth, u.flags.x != 0u);
        let geometry_mask = 1.0 - smoothstep(0.996, 1.0, far_depth);
        let water_mask = cyan_mask * horizontal_mask * geometry_mask;
        let fresnel = pow(clamp(1.0 - abs(dot(normal, view_dir)), 0.0, 1.0), 3.0);
        let shimmer = 0.5 + 0.5 * sin((in.uv.x * 410.0 + in.uv.y * 270.0) + u.params2.w * 1.7);
        let water_tint = color * vec3f(0.80, 1.035, 1.105) + vec3f(0.015, 0.045, 0.075) * shimmer;
        color = mix(color, water_tint, water_mask * u.params2.y * 0.55 * style);
        color += vec3f(0.10, 0.17, 0.24) * fresnel * water_mask * u.params2.y * style;
    }

    // Bloom remains selective and low-radius so Android does not pay for a multi-pass blur chain.
    if (u.params1.w > 0.001) {
        color += sample_bloom(coord, u.params2.x) * u.params1.w * style;
    }

    // Depth atmosphere separates distant landscape layers and shifts the horizon toward sky color.
    if (u.params1.x > 0.001) {
        let fog_range = max(u.params1.z - u.params1.y, 1.0);
        let fog_amount = smoothstep(0.0, 1.0, (distance_to_camera - u.params1.y) / fog_range);
        let horizon = 1.0 - clamp(abs(in.uv.y - 0.50) * 2.2, 0.0, 1.0);
        let sky_fog = vec3f(0.59, 0.72, 0.86);
        let warm_horizon = vec3f(0.86, 0.70, 0.54);
        let fog_color = mix(sky_fog, warm_horizon, horizon * 0.18);
        color = mix(color, fog_color, fog_amount * u.params1.x * style);
    }

    // BOTW-like grade: lifted cool shadows, restrained warm highlights, vibrance and soft filmic rolloff.
    color *= exp2(u.params0.x);
    var luma = luminance(color);
    let shadow_mask = 1.0 - smoothstep(0.16, 0.58, luma);
    let highlight_mask = smoothstep(0.50, 0.92, luma);
    color += shadow_mask * u.params0.w * vec3f(0.10, 0.14, 0.20) * u.params3.z * style;
    color *= mix(vec3f(1.0), vec3f(0.965, 0.992, 1.055), shadow_mask * u.params3.z * style);
    color *= mix(vec3f(1.0), vec3f(1.055, 1.018, 0.955), highlight_mask * u.params3.y * style);

    luma = luminance(color);
    let saturation = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    let vibrance_weight = 1.0 - clamp(saturation, 0.0, 1.0);
    color = mix(vec3f(luma), color, 1.0 + (u.params0.z - 1.0) * vibrance_weight * style);
    color = (color - vec3f(0.5)) * mix(1.0, u.params0.y, style) + vec3f(0.5);

    let filmic = aces_filmic(max(color, vec3f(0.0)));
    color = mix(color, filmic, u.params3.w * style);

    if (u.flags.y == 1u) {
        return vec4f(vec3f(shadow_mask), 1.0);
    }
    if (u.flags.y == 2u) {
        let n = normal * 0.5 + vec3f(0.5);
        return vec4f(n, 1.0);
    }
    if (u.flags.y == 3u) {
        let fog_debug = clamp((distance_to_camera - u.params1.y) / max(u.params1.z - u.params1.y, 1.0), 0.0, 1.0);
        return vec4f(vec3f(fog_debug), 1.0);
    }

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
