#pragma once
// Included from the global module fragment of the gfx modules (macros cannot be exported from modules).

#include <stdexcept>
#include <string>
#include <volk.h>

namespace wb::gfx
{
[[nodiscard]] inline const char* toString(const VkResult p_Result)
{
	switch (p_Result)
	{
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
	case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
	default: return "VkResult(unknown)";
	}
}

[[nodiscard]] inline const char* toString(const VkPresentModeKHR p_Mode)
{
	switch (p_Mode)
	{
	case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
	case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
	case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
	default: return "other";
	}
}

[[nodiscard]] inline const char* toString(const VkFormat p_Format)
{
	switch (p_Format)
	{
	case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
	case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM";
	case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
	default: return "other";
	}
}

class VulkanError : public std::runtime_error
{
public:
	VulkanError(const VkResult p_Result, const char* p_Expression, const char* p_File, const int p_Line)
		: std::runtime_error(std::string(p_Expression) + " failed with " + toString(p_Result) + " (" + p_File + ":" + std::to_string(p_Line) + ")"), m_Result(p_Result)
	{
	}

	[[nodiscard]] VkResult result() const { return m_Result; }

private:
	VkResult m_Result;
};
} // namespace wb::gfx

// Throws wb::gfx::VulkanError on any negative VkResult. Positive codes (VK_SUBOPTIMAL_KHR, ...) are returned.
#define WB_VK_CHECK(expr)                                                             \
	[&]() -> VkResult                                                                 \
	{                                                                                 \
		const VkResult l_VkCheckResult = (expr);                                      \
		if (l_VkCheckResult < 0)                                                      \
			throw ::wb::gfx::VulkanError(l_VkCheckResult, #expr, __FILE__, __LINE__); \
		return l_VkCheckResult;                                                       \
	}()
