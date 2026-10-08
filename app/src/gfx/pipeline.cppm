// Shader loading and graphics pipeline creation (dynamic rendering, dynamic viewport/scissor).
module;
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <volk.h>

export module wb.gfx.pipeline;

import wb.gfx.context;

export namespace wb::gfx
{
enum class BlendMode : uint8_t
{
	Opaque,
	PremultipliedAlpha, // out = src + dst * (1 - src.a)
};

struct GraphicsPipelineDesc
{
	VkShaderModule module = VK_NULL_HANDLE; // one SPIR-V module holding both entry points
	const char* vertexEntry = "vsMain";
	const char* fragmentEntry = "fsMain";
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkFormat colorFormat = VK_FORMAT_UNDEFINED;
	BlendMode blend = BlendMode::PremultipliedAlpha;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	const char* name = "pipeline";
};

// Directory that holds the compiled shaders (next to the executable)
[[nodiscard]] std::filesystem::path shaderDirectory();
// Loads <shaderDirectory>/<p_Name>.spv. Throws std::runtime_error when missing or malformed.
[[nodiscard]] VkShaderModule loadShaderModule(const GraphicsContext& p_Context, const std::string& p_Name);

[[nodiscard]] VkPipeline createGraphicsPipeline(const GraphicsContext& p_Context, const GraphicsPipelineDesc& p_Desc);
[[nodiscard]] VkPipelineLayout createPipelineLayout(const GraphicsContext& p_Context, std::span<const VkDescriptorSetLayout> p_SetLayouts, uint32_t p_PushConstantBytes, const char* p_Name);
} // namespace wb::gfx
