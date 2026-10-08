// Strokes as chains of analytic "uneven capsules" (round-capped segments whose radius varies linearly).
//
// Every segment is one screen-space quad. A fragment evaluates the signed distance to its own segment and to
// its neighbours, and only the segment closest to the fragment writes it (ownership rule). This gives exact
// anti-aliasing at any zoom, smooth width changes and no double blending where consecutive capsules overlap.

import common;

struct StrokeInstance
{
	float4 linear;       // local -> screen 2x2 matrix, column-major (col0.xy, col1.xy)
	float2 translation;  // local -> screen translation (pixels)
	float radiusScale;   // local radius -> screen pixels
	uint flags;          // STROKE_FLAG_*
	float4 color;        // straight alpha
	uint firstPoint;
	uint pointCount;
	uint2 padding;
};

static const uint STROKE_FLAG_LIVE = 1; // points live in the per-frame buffer (stroke being drawn)
static const int NEIGHBOURS = 3;        // segments checked on each side for the ownership rule

[[vk::binding(0, 0)]] StructuredBuffer<float4> gPoints;     // xy = local position, z = radius
[[vk::binding(1, 0)]] StructuredBuffer<float4> gLivePoints; // same layout, rewritten every frame
[[vk::binding(2, 0)]] StructuredBuffer<StrokeInstance> gInstances;

struct PushConstants
{
	float2 viewportSize;
	float minRadius; // strokes thinner than this (pixels) are drawn at this radius with reduced alpha
	float aaWidth;   // anti-aliasing ramp width in pixels
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct ScreenPoint
{
	float2 position;
	float radius;    // clamped to minRadius
	float rawRadius; // before clamping
};

ScreenPoint loadPoint(StrokeInstance inst, uint index)
{
	float4 p = (inst.flags & STROKE_FLAG_LIVE) != 0 ? gLivePoints[inst.firstPoint + index] : gPoints[inst.firstPoint + index];
	float2x2 m = float2x2(inst.linear.x, inst.linear.z, inst.linear.y, inst.linear.w); // rows
	ScreenPoint result;
	result.position = mul(m, p.xy) + inst.translation;
	result.rawRadius = p.z * inst.radiusScale;
	result.radius = max(result.rawRadius, pc.minRadius);
	return result;
}

uint segmentCount(StrokeInstance inst)
{
	return max(inst.pointCount, 2u) - 1u;
}

// Endpoints of segment s (a single point is a zero-length segment: a dot)
void loadSegment(StrokeInstance inst, uint s, out ScreenPoint a, out ScreenPoint b)
{
	a = loadPoint(inst, s);
	b = loadPoint(inst, min(s + 1, inst.pointCount - 1));
}

float cross2(float2 a, float2 b)
{
	return a.x * b.y - a.y * b.x;
}

// Signed distance to an uneven capsule (Inigo Quilez), robust to degenerate cases
float sdUnevenCapsule(float2 p, float2 pa, float2 pb, float ra, float rb)
{
	p -= pa;
	pb -= pa;
	float h = dot(pb, pb);
	float b = ra - rb;
	if (h <= b * b + 1e-6)
	{
		// Zero-length segment, or one end circle contains the other
		return min(length(p) - ra, length(p - pb) - rb);
	}
	float2 q = float2(dot(p, float2(pb.y, -pb.x)), dot(p, pb)) / h;
	q.x = abs(q.x);
	float2 c = float2(sqrt(h - b * b), b);
	float k = cross2(c, q);
	float m = dot(c, q);
	float n = dot(q, q);
	if (k < 0.0)
		return sqrt(h * n) - ra;
	if (k > c.x)
		return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
	return m - ra;
}

float segmentDistance(StrokeInstance inst, uint s, float2 p)
{
	ScreenPoint a, b;
	loadSegment(inst, s, a, b);
	return sdUnevenCapsule(p, a.position, b.position, a.radius, b.radius);
}

struct VSOutput
{
	float4 position : SV_Position;
	nointerpolation uint instance : INSTANCE;
	nointerpolation uint segment : SEGMENT;
};

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID)
{
	StrokeInstance inst = gInstances[instanceId];
	uint s = vertexId / 6;
	uint corner = vertexId % 6;

	ScreenPoint a, b;
	loadSegment(inst, s, a, b);

	float2 axis = b.position - a.position;
	float len = length(axis);
	float2 dir = len > 1e-4 ? axis / len : float2(1.0, 0.0);
	float2 normal = float2(-dir.y, dir.x);
	float margin = pc.aaWidth + 1.0;
	float extent = max(a.radius, b.radius) + margin;

	// Corners: 0..2 and 3..5 form two triangles of the quad around the capsule
	static const float2 CORNERS[6] = { float2(0, -1), float2(1, -1), float2(1, 1), float2(0, -1), float2(1, 1), float2(0, 1) };
	float2 c = CORNERS[corner];
	float2 start = a.position - dir * (a.radius + margin);
	float2 end = b.position + dir * (b.radius + margin);
	float2 pos = lerp(start, end, c.x) + normal * (c.y * extent);

	VSOutput output;
	output.position = float4(screenToNdc(pos, pc.viewportSize), 0.0, 1.0);
	output.instance = instanceId;
	output.segment = s;
	return output;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
	StrokeInstance inst = gInstances[input.instance];
	float2 p = input.position.xy; // pixel center in framebuffer space
	int s = int(input.segment);
	int count = int(segmentCount(inst));

	float own = segmentDistance(inst, uint(s), p);
	if (own > pc.aaWidth * 0.5)
		discard;

	// Ownership: a neighbour that is strictly closer (ties go to the lower index) draws this pixel instead
	for (int j = max(s - NEIGHBOURS, 0); j <= min(s + NEIGHBOURS, count - 1); ++j)
	{
		if (j == s)
			continue;
		float d = segmentDistance(inst, uint(j), p);
		if (d < own || (d == own && j < s))
			discard;
	}

	float coverage = saturate(0.5 - own / pc.aaWidth);

	// Sub-pixel strokes keep a visible minimum width but fade proportionally
	ScreenPoint a, b;
	loadSegment(inst, uint(s), a, b);
	float thin = saturate(max(a.rawRadius, b.rawRadius) / pc.minRadius);

	float alpha = inst.color.a * coverage * thin;
	return float4(inst.color.rgb * alpha, alpha);
}
