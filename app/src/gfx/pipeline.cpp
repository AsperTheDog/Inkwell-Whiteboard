module;
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "gfx/vk_check.hpp"

module wb.gfx.pipeline;

import wb.gfx.context;

namespace wb::gfx
{
std::filesystem::path shaderDirectory()
{
	const char* l_Base = SDL_GetBasePath();
	const std::filesystem::path l_Root = l_Base != nullptr ? std::filesystem::path(reinterpret_cast<const char8_t*>(l_Base)) : std::filesystem::current_path();
	return l_Root / "shaders";
}

VkShaderModule loadShaderModule(const GraphicsContext& p_Context, const std::string& p_Name)
{
	const std::filesystem::path l_Path = shaderDirectory() / (p_Name + ".spv");
	std::ifstream l_File(l_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
		throw std::runtime_error("Missing shader: " + l_Path.string());

	const std::streamsize l_Size = l_File.tellg();
	if (l_Size <= 0 || l_Size % 4 != 0)
		throw std::runtime_error("Malformed shader: " + l_Path.string());
	std::vector<uint32_t> l_Code(static_cast<size_t>(l_Size) / 4);
	l_File.seekg(0);
	l_File.read(reinterpret_cast<char*>(l_Code.data()), l_Size);

	const VkShaderModuleCreateInfo l_Info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = static_cast<size_t>(l_Size),
		.pCode = l_Code.data(),
	};
	VkShaderModule l_Module = VK_NULL_HANDLE;
	WB_VK_CHECK(vkCreateShaderModule(p_Context.device(), &l_Info, nullptr, &l_Module));
	p_Context.setName(l_Module, VK_OBJECT_TYPE_SHADER_MODULE, p_Name.c_str());
	return l_Module;
}

VkPipelineLayout createPipelineLayout(const GraphicsContext& p_Context, const std::span<const VkDescriptorSetLayout> p_SetLayouts, const uint32_t p_PushConstantBytes, const char* p_Name)
{
	const VkPushConstantRange l_Range{
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = p_PushConstantBytes,
	};
	const VkPipelineLayoutCreateInfo l_Info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = static_cast<uint32_t>(p_SetLayouts.size()),
		.pSetLayouts = p_SetLayouts.data(),
		.pushConstantRangeCount = p_PushConstantBytes > 0 ? 1u : 0u,
		.pPushConstantRanges = p_PushConstantBytes > 0 ? &l_Range : nullptr,
	};
	VkPipelineLayout l_Layout = VK_NULL_HANDLE;
	WB_VK_CHECK(vkCreatePipelineLayout(p_Context.device(), &l_Info, nullptr, &l_Layout));
	p_Context.setName(l_Layout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, p_Name);
	return l_Layout;
}

VkPipeline createGraphicsPipeline(const GraphicsContext& p_Context, const GraphicsPipelineDesc& p_Desc)
{
	const VkPipelineShaderStageCreateInfo l_Stages[]{
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p_Desc.module, .pName = p_Desc.vertexEntry },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p_Desc.module, .pName = p_Desc.fragmentEntry },
	};

	constexpr VkPipelineVertexInputStateCreateInfo l_VertexInput{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	const VkPipelineInputAssemblyStateCreateInfo l_InputAssembly{ .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = p_Desc.topology };
	constexpr VkPipelineViewportStateCreateInfo l_Viewport{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
	constexpr VkPipelineRasterizationStateCreateInfo l_Raster{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.f,
	};
	constexpr VkPipelineMultisampleStateCreateInfo l_Multisample{ .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	constexpr VkPipelineDepthStencilStateCreateInfo l_DepthStencil{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

	VkPipelineColorBlendAttachmentState l_BlendAttachment{
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	if (p_Desc.blend == BlendMode::PremultipliedAlpha)
	{
		l_BlendAttachment.blendEnable = VK_TRUE;
		l_BlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		l_BlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		l_BlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		l_BlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		l_BlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		l_BlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	const VkPipelineColorBlendStateCreateInfo l_Blend{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &l_BlendAttachment,
	};

	constexpr VkDynamicState l_DynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	const VkPipelineDynamicStateCreateInfo l_Dynamic{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = l_DynamicStates,
	};

	const VkPipelineRenderingCreateInfo l_Rendering{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &p_Desc.colorFormat,
	};

	const VkGraphicsPipelineCreateInfo l_Info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &l_Rendering,
		.stageCount = 2,
		.pStages = l_Stages,
		.pVertexInputState = &l_VertexInput,
		.pInputAssemblyState = &l_InputAssembly,
		.pViewportState = &l_Viewport,
		.pRasterizationState = &l_Raster,
		.pMultisampleState = &l_Multisample,
		.pDepthStencilState = &l_DepthStencil,
		.pColorBlendState = &l_Blend,
		.pDynamicState = &l_Dynamic,
		.layout = p_Desc.layout,
	};
	VkPipeline l_Pipeline = VK_NULL_HANDLE;
	WB_VK_CHECK(vkCreateGraphicsPipelines(p_Context.device(), VK_NULL_HANDLE, 1, &l_Info, nullptr, &l_Pipeline));
	p_Context.setName(l_Pipeline, VK_OBJECT_TYPE_PIPELINE, p_Desc.name);
	return l_Pipeline;
}
} // namespace wb::gfx
