// User interface primitives. Every primitive is one instanced quad; the fragment shader evaluates a signed
// distance (rounded boxes, capsules) or samples the glyph atlas. All coordinates are window pixels (y down).
//
//   mode 0  rounded box     fill (color) with an optional inner border (color2, misc.y pixels)
//   mode 1  shadow          blurred rounded box, misc.z = blur radius
//   mode 2  atlas glyph     coverage from the R8 atlas, tinted by color
//   mode 3  capsule         line from shape.xy to shape.zw, misc.x = half width
//   mode 4  saturation/value square of hue misc.z (rounded like mode 0)
//   mode 5  hue bar (horizontal), rounded like mode 0

import common;

struct Prim
{
	float4 bounds;  // quad: x0 y0 x1 y1
	float4 shape;   // boxes: centre.xy, half size.xy; capsules: a.xy, b.xy
	float4 uv;      // glyphs: u0 v0 u1 v1
	float4 color;   // premultiplied
	float4 color2;  // premultiplied (border)
	float4 misc;    // x radius / half width, y border width, z blur or hue, w mode
	float4 clip;    // x0 y0 x1 y1
};

[[vk::binding(0, 0)]] StructuredBuffer<Prim> gPrims;
[[vk::binding(1, 0)]] Sampler2D gAtlas;

struct PushConstants
{
	float2 viewportSize;
	float2 padding;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct VSOutput
{
	float4 position : SV_Position;
	float2 uv;
	nointerpolation uint prim;
};

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID)
{
	static const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	Prim p = gPrims[instanceId];
	float2 t = corners[vertexId];
	float2 pixel = lerp(p.bounds.xy, p.bounds.zw, t);
	VSOutput output;
	output.position = float4(screenToNdc(pixel, pc.viewportSize), 0.0, 1.0);
	output.uv = lerp(p.uv.xy, p.uv.zw, t);
	output.prim = instanceId;
	return output;
}

float roundedBox(float2 p, float2 halfSize, float radius)
{
	float2 q = abs(p) - halfSize + radius;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float distanceToSegment(float2 p, float2 a, float2 b)
{
	float2 pa = p - a;
	float2 ba = b - a;
	float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
	return length(pa - ba * h);
}

float3 hsvToRgb(float h, float s, float v)
{
	float3 k = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
	return v * lerp(float3(1.0, 1.0, 1.0), k, s);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
	Prim p = gPrims[input.prim];
	float2 pix = input.position.xy;
	if (pix.x < p.clip.x || pix.y < p.clip.y || pix.x > p.clip.z || pix.y > p.clip.w)
		discard;

	int mode = (int)(p.misc.w + 0.5);
	if (mode == 2)
	{
		float coverage = gAtlas.Sample(input.uv).r;
		return p.color * coverage;
	}
	if (mode == 3)
	{
		float d = distanceToSegment(pix, p.shape.xy, p.shape.zw) - p.misc.x;
		return p.color * saturate(0.5 - d);
	}

	float d = roundedBox(pix - p.shape.xy, p.shape.zw, p.misc.x);
	if (mode == 1)
	{
		float blur = max(p.misc.z, 0.5);
		float t = saturate(0.5 - 0.5 * d / blur);
		return p.color * (t * t * (3.0 - 2.0 * t));
	}

	float outer = saturate(0.5 - d);
	if (mode == 4 || mode == 5)
	{
		float2 local = (pix - (p.shape.xy - p.shape.zw)) / (2.0 * p.shape.zw);
		float3 rgb = mode == 4 ? hsvToRgb(p.misc.z, saturate(local.x), 1.0 - saturate(local.y)) : hsvToRgb(saturate(local.x), 1.0, 1.0);
		return float4(rgb, 1.0) * (outer * p.color.a);
	}

	float border = p.misc.y;
	if (border <= 0.0)
		return p.color * outer;
	float inner = saturate(0.5 - (d + border));
	return p.color * inner + p.color2 * max(outer - inner, 0.0);
}
