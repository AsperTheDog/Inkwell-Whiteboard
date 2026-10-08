// Shared helpers. Imported as a Slang module (`import common;`), not compiled on its own.
module common;

// Window pixels (y down) -> Vulkan clip space (y down)
public float2 screenToNdc(float2 pixels, float2 viewportSize)
{
	return pixels / viewportSize * 2.0 - 1.0;
}

// Full-screen triangle from SV_VertexID (0..2)
public float4 fullscreenTriangle(uint vertexId)
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}
