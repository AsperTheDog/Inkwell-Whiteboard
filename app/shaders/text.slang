// Text on the board: one instanced quad per glyph, shaded from a signed distance field atlas. The quad is placed in
// object-local space (the glyph records come from the text system) and transformed to the screen by the object's
// matrix, so text stays sharp at every zoom and rotation. Synthetic bold moves the outline threshold; synthetic
// italics shear the quad about the baseline.

import common;

struct Glyph
{
	float4 rect;     // x, y, width, height of the quad in object-local units
	float4 uv;       // u0, v0, u1, v1
	float baseline;  // object-local y of the baseline
	uint flags;      // 1 = bold, 2 = italic
	float2 padding;
};

[[vk::binding(0, 0)]] StructuredBuffer<Glyph> gGlyphs;
[[vk::binding(1, 0)]] Sampler2D gAtlas;

struct PushConstants
{
	float2 viewportSize;
	float2 padding;
	float4 linear;      // local -> screen 2x2 matrix, column-major (col0.xy, col1.xy)
	float2 translation; // local -> screen translation (pixels)
	float2 padding1;
	float4 color;       // straight alpha
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

// Must match wb::text::sdf in fonts.cppm: the atlas stores 64 pixels per em, 16 value units per pixel, outline at 128
static const float OUTLINE = 128.0 / 255.0;
static const float BOLD_SHIFT = 0.02 * 64.0 * 16.0 / 255.0; // thickens each side by 0.02 em
static const float ITALIC_SHEAR = 0.2;

struct VSOutput
{
	float4 position : SV_Position;
	float2 uv;
	nointerpolation uint flags;
};

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID)
{
	static const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	Glyph g = gGlyphs[instanceId];
	float2 t = corners[vertexId];
	float2 local = g.rect.xy + t * g.rect.zw;
	if ((g.flags & 2u) != 0u)
		local.x += ITALIC_SHEAR * (g.baseline - local.y);
	float2x2 m = float2x2(pc.linear.x, pc.linear.z, pc.linear.y, pc.linear.w); // rows
	float2 pixel = mul(m, local) + pc.translation;

	VSOutput output;
	output.position = float4(screenToNdc(pixel, pc.viewportSize), 0.0, 1.0);
	output.uv = lerp(g.uv.xy, g.uv.zw, t);
	output.flags = g.flags;
	return output;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
	float d = gAtlas.Sample(input.uv).r;
	float edge = OUTLINE;
	if ((input.flags & 1u) != 0u)
		edge -= BOLD_SHIFT;
	// One screen pixel is fwidth(d) of the field: blend across about a pixel for crisp, anti-aliased edges
	float w = max(length(float2(ddx(d), ddy(d))) * 0.7, 1e-4);
	float coverage = smoothstep(edge - w, edge + w, d);
	if (coverage <= 0.0)
		discard;
	return float4(pc.color.rgb * pc.color.a, pc.color.a) * coverage;
}
