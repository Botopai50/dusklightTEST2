struct Uniforms {
    view_from_proj: mat4x4f,
    size: vec2f,
    inv_size: vec2f,

    light_dir_view: vec3f,
    toon_strength: f32,

    shadow_level: f32,
    middle_level: f32,
    highlight_level: f32,
    band_smoothing: f32,

    outline_strength: f32,
    depth_threshold: f32,
    normal_threshold: f32,
    outline_thickness: f32,

    rim_strength: f32,
    rim_power: f32,
    saturation: f32,
    contrast: f32,

    debug_mode: u32,
    reversed_z: u32,
    _pad: vec2u,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var scene_depth: texture_2d<f32>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn clamp_pixel(pixel: vec2i) -> vec2i {
    return clamp(pixel, vec2i(0), vec2i(uniforms.size) - vec2i(1));
}

fn load_color(pixel: vec2i) -> vec4f {
    return textureLoad(scene_color, clamp_pixel(pixel), 0);
}

fn load_depth(pixel: vec2i) -> f32 {
    return textureLoad(scene_depth, clamp_pixel(pixel), 0).r;
}

fn is_background(depth: f32) -> bool {
    if uniforms.reversed_z != 0u {
        return depth <= 0.000001;
    }
    return depth >= 0.999999;
}

fn reconstruct_view_position(pixel: vec2i, depth: f32) -> vec3f {
    let uv = (vec2f(pixel) + 0.5) * uniforms.inv_size;
    let ndc = vec3f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth);
    let view4 = uniforms.view_from_proj * vec4f(ndc, 1.0);
    return view4.xyz / view4.w;
}

fn view_position_at(pixel: vec2i) -> vec3f {
    let p = clamp_pixel(pixel);
    return reconstruct_view_position(p, load_depth(p));
}

fn reconstruct_normal(pixel: vec2i) -> vec3f {
    let p = clamp_pixel(pixel);
    let center_depth = load_depth(p);
    let center = reconstruct_view_position(p, center_depth);

    let left_p = clamp_pixel(p + vec2i(-1, 0));
    let right_p = clamp_pixel(p + vec2i(1, 0));
    let top_p = clamp_pixel(p + vec2i(0, -1));
    let bottom_p = clamp_pixel(p + vec2i(0, 1));

    let left_depth = load_depth(left_p);
    let right_depth = load_depth(right_p);
    let top_depth = load_depth(top_p);
    let bottom_depth = load_depth(bottom_p);

    let left_valid = !is_background(left_depth);
    let right_valid = !is_background(right_depth);
    let top_valid = !is_background(top_depth);
    let bottom_valid = !is_background(bottom_depth);

    var ddx = vec3f(1.0, 0.0, 0.0);
    if left_valid && right_valid {
        let left_delta = center - reconstruct_view_position(left_p, left_depth);
        let right_delta = reconstruct_view_position(right_p, right_depth) - center;
        ddx = select(right_delta, left_delta, dot(left_delta, left_delta) < dot(right_delta, right_delta));
    } else if left_valid {
        ddx = center - reconstruct_view_position(left_p, left_depth);
    } else if right_valid {
        ddx = reconstruct_view_position(right_p, right_depth) - center;
    }

    var ddy = vec3f(0.0, 1.0, 0.0);
    if top_valid && bottom_valid {
        let top_delta = center - reconstruct_view_position(top_p, top_depth);
        let bottom_delta = reconstruct_view_position(bottom_p, bottom_depth) - center;
        ddy = select(bottom_delta, top_delta, dot(top_delta, top_delta) < dot(bottom_delta, bottom_delta));
    } else if top_valid {
        ddy = center - reconstruct_view_position(top_p, top_depth);
    } else if bottom_valid {
        ddy = reconstruct_view_position(bottom_p, bottom_depth) - center;
    }

    var normal = normalize(cross(ddy, ddx));
    if dot(normal, center) > 0.0 {
        normal = -normal;
    }
    return normal;
}

fn relative_depth_delta(pixel: vec2i, center_position: vec3f, offset: vec2i) -> f32 {
    let sample_pixel = clamp_pixel(pixel + offset);
    let sample_depth = load_depth(sample_pixel);
    if is_background(sample_depth) {
        return 1.0;
    }
    let sample_position = reconstruct_view_position(sample_pixel, sample_depth);
    return abs(sample_position.z - center_position.z) / max(abs(center_position.z), 1.0);
}

fn normal_delta(pixel: vec2i, center_normal: vec3f, offset: vec2i) -> f32 {
    let sample_pixel = clamp_pixel(pixel + offset);
    if is_background(load_depth(sample_pixel)) {
        return 1.0;
    }
    let sample_normal = reconstruct_normal(sample_pixel);
    return 1.0 - clamp(dot(center_normal, sample_normal), -1.0, 1.0);
}

fn toon_band(diffuse: f32) -> f32 {
    let smoothing = max(uniforms.band_smoothing, 0.0005);
    let middle_weight = smoothstep(0.34 - smoothing, 0.34 + smoothing, diffuse);
    let highlight_weight = smoothstep(0.72 - smoothing, 0.72 + smoothing, diffuse);
    let shadow_to_middle = mix(uniforms.shadow_level, uniforms.middle_level, middle_weight);
    return mix(shadow_to_middle, uniforms.highlight_level, highlight_weight);
}

fn outline_factor(pixel: vec2i, center_position: vec3f, center_normal: vec3f) -> f32 {
    if uniforms.outline_strength <= 0.0001 {
        return 0.0;
    }

    let radius = i32(clamp(round(uniforms.outline_thickness), 1.0, 3.0));
    let left = vec2i(-radius, 0);
    let right = vec2i(radius, 0);
    let top = vec2i(0, -radius);
    let bottom = vec2i(0, radius);

    let depth_edge = max(
        max(relative_depth_delta(pixel, center_position, left),
            relative_depth_delta(pixel, center_position, right)),
        max(relative_depth_delta(pixel, center_position, top),
            relative_depth_delta(pixel, center_position, bottom))
    );

    let normal_edge = max(
        normal_delta(pixel, center_normal, right),
        normal_delta(pixel, center_normal, bottom)
    );

    let depth_outline = smoothstep(
        uniforms.depth_threshold,
        max(uniforms.depth_threshold * 2.0, uniforms.depth_threshold + 0.0001),
        depth_edge
    );
    let normal_outline = smoothstep(
        uniforms.normal_threshold,
        max(uniforms.normal_threshold * 1.75, uniforms.normal_threshold + 0.0001),
        normal_edge
    );
    return max(depth_outline, normal_outline);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let pixel = clamp_pixel(vec2i(in.uv * uniforms.size));
    let source = load_color(pixel);
    let depth = load_depth(pixel);

    // The sky and untouched clear pixels have no useful geometry normal.
    if is_background(depth) {
        return source;
    }

    let position = reconstruct_view_position(pixel, depth);
    let normal = reconstruct_normal(pixel);
    let light_direction = normalize(uniforms.light_dir_view);
    let view_direction = normalize(-position);
    let diffuse = max(dot(normal, light_direction), 0.0);
    let band = toon_band(diffuse);
    let outline = outline_factor(pixel, position, normal);

    if uniforms.debug_mode == 1u {
        return vec4f(vec3f(band), 1.0);
    }
    if uniforms.debug_mode == 2u {
        return vec4f(normal * 0.5 + 0.5, 1.0);
    }
    if uniforms.debug_mode == 3u {
        let depth_view = exp(-abs(position.z) * 0.00025);
        return vec4f(vec3f(depth_view), 1.0);
    }
    if uniforms.debug_mode == 4u {
        return vec4f(vec3f(outline), 1.0);
    }

    let lighting_factor = mix(1.0, band, uniforms.toon_strength);
    var color = source.rgb * lighting_factor;

    let luminance = dot(color, vec3f(0.299, 0.587, 0.114));
    color = mix(vec3f(luminance), color, uniforms.saturation);
    color = (color - vec3f(0.5)) * uniforms.contrast + vec3f(0.5);

    let fresnel = pow(clamp(1.0 - max(dot(normal, view_direction), 0.0), 0.0, 1.0),
                      max(uniforms.rim_power, 1.0));
    let rim_mask = fresnel * smoothstep(0.05, 0.75, diffuse) * uniforms.rim_strength;
    let rim_color = mix(source.rgb, vec3f(0.72, 0.86, 1.0), 0.70);
    color += rim_color * rim_mask;

    let ink = vec3f(0.015, 0.020, 0.028);
    color = mix(color, ink, outline * uniforms.outline_strength);

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), source.a);
}
