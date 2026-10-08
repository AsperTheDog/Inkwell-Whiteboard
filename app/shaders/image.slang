// A picture on the board: one quad, transformed by the object's local -> screen matrix. The texture is a 2D array
// (one layer per animation frame) holding premultiplied RGBA. The quad is inflated by a pixel and its edges are
// anti-aliased from the distance to the border, so rotated pictures do not look jagged.

import common;

struct PushConstants
{
	float2 viewportSize;
	float inflate;      // local units added around the rectangle (about one pixel)
	float layer;        // animation frame; negative draws a flat placeholder
	float4 linear;      // local -> screen 2x2 matrix, column-major (col0.xy, col1.xy)
	float2 translation; // local -> screen translation (pixels)
	float2 size;        // local size of the rectangle
	float opacity;
	float padding0;
	float padding1;
	float padding2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

[[vk::binding(0, 0)]] Sampler2DArray gImage;

struct VSOutput
{
	float4 position : SV_Position;
	float2 uv;
};

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID)
{
	static const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	float2 t = corners[vertexId];
	float2 extent = pc.size + 2.0 * pc.inflate;
	float2 local = (t - 0.5) * extent;
	float2x2 m = float2x2(pc.linear.x, pc.linear.z, pc.linear.y, pc.linear.w); // rows
	float2 pixel = mul(m, local) + pc.translation;

	VSOutput output;
	output.position = float4(screenToNdc(pixel, pc.viewportSize), 0.0, 1.0);
	output.uv = local / pc.size + 0.5;
	return output;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
	// Distance to the nearest border in pixels (positive inside)
	float2 ddxUv = ddx(input.uv);
	float2 ddyUv = ddy(input.uv);
	float2 pixelsPerUv = 1.0 / max(float2(length(float2(ddxUv.x, ddyUv.x)), length(float2(ddxUv.y, ddyUv.y))), 1e-8);
	float2 border = min(input.uv, 1.0 - input.uv) * pixelsPerUv;
	float coverage = saturate(min(border.x, border.y) + 0.5);
	if (coverage <= 0.0)
		discard;

	if (pc.layer < 0.0)
	{
		// Still loading: a faint grey panel
		float4 grey = float4(0.5, 0.5, 0.55, 0.22);
		return float4(grey.rgb * grey.a, grey.a) * coverage * pc.opacity;
	}
	float4 color = gImage.Sample(float3(clamp(input.uv, 0.0, 1.0), pc.layer));
	return color * (coverage * pc.opacity);
}
