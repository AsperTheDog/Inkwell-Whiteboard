// Infinite dot grid. Two levels (5x apart) cross-fade with zoom so dots never pop or crowd.
// Offsets are computed on the CPU in double precision, so the grid stays exact far from the origin.

import common;

struct GridLevel
{
	float2 offset;  // screen position of a grid point, modulo spacing (pixels)
	float spacing;  // pixels between dots
	float alpha;
};

struct PushConstants
{
	GridLevel fine;
	GridLevel coarse;
	float4 color;     // straight alpha
	float dotRadius;  // pixels
	float padding0;
	float padding1;
	float padding2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct VSOutput
{
	float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID)
{
	VSOutput output;
	output.position = fullscreenTriangle(vertexId);
	return output;
}

float dotCoverage(float2 p, GridLevel level)
{
	if (level.alpha <= 0.0)
		return 0.0;
	float2 cell = p - level.offset;
	float2 nearest = round(cell / level.spacing) * level.spacing;
	float d = length(cell - nearest) - pc.dotRadius;
	return saturate(0.5 - d) * level.alpha;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
	float2 p = input.position.xy;
	float coverage = max(dotCoverage(p, pc.fine), dotCoverage(p, pc.coarse));
	if (coverage <= 0.0)
		discard;
	float alpha = pc.color.a * coverage;
	return float4(pc.color.rgb * alpha, alpha);
}
