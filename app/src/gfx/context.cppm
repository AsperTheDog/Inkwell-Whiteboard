// Vulkan instance, surface, device, queue and VMA allocator.
//
// Baseline is Vulkan 1.2 core plus dynamic rendering, synchronization2 and demote-to-helper, which are core in 1.3 and
// otherwise taken from their extensions. Any conformant driver (discrete, integrated, virtual or software) with that can
// run the app. Optional capabilities are detected and recorded in DeviceInfo.
module;
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include "gfx/vma.hpp"

export module wb.gfx.context;

import wb.platform.window;

export namespace wb::gfx
{
// Thrown when no GPU meets the requirements; the message is meant for the user.
class UnsupportedGpuError : public std::exception
{
public:
	explicit UnsupportedGpuError(std::string p_Message) : m_Message(std::move(p_Message)) {}
	[[nodiscard]] const char* what() const noexcept override { return m_Message.c_str(); }

private:
	std::string m_Message;
};

struct DeviceInfo
{
	std::string name;
	std::string driver;
	VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
	uint32_t apiVersion = 0;
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
	VkDeviceSize deviceLocalBytes = 0;
	VkPhysicalDeviceLimits limits{};

	// Optional capabilities
	bool samplerAnisotropy = false;
	// The device is Vulkan 1.2 and gets these 1.3 features through extensions (apiVersion is then 1.2)
	bool extDynamicRendering = false;
	bool extSynchronization2 = false;
	bool extDemote = false;
	bool timestamps = false; // the graphics queue supports timestamp queries
	float timestampPeriodNs = 0.f;
};

struct ValidationStats
{
	uint32_t errors = 0;
	uint32_t warnings = 0;
};

class GraphicsContext
{
public:
	GraphicsContext() = default;
	GraphicsContext(const GraphicsContext&) = delete;
	GraphicsContext& operator=(const GraphicsContext&) = delete;

	// Throws UnsupportedGpuError when no suitable GPU exists and VulkanError on API failures.
	void init(const platform::Window& p_Window);
	void destroy();

	[[nodiscard]] VkInstance instance() const { return m_Instance; }
	[[nodiscard]] VkSurfaceKHR surface() const { return m_Surface; }
	[[nodiscard]] VkPhysicalDevice physicalDevice() const { return m_PhysicalDevice; }
	[[nodiscard]] VkDevice device() const { return m_Device; }
	[[nodiscard]] VmaAllocator allocator() const { return m_Allocator; }
	[[nodiscard]] VkQueue queue() const { return m_Queue; }
	[[nodiscard]] uint32_t queueFamily() const { return m_QueueFamily; }
	[[nodiscard]] const DeviceInfo& info() const { return m_Info; }
	[[nodiscard]] bool validationEnabled() const { return m_ValidationLayer && m_DebugMessenger != VK_NULL_HANDLE; }
	[[nodiscard]] static ValidationStats validationStats();

	// No-op unless VK_EXT_debug_utils is enabled
	void setName(VkObjectType p_Type, uint64_t p_Handle, const char* p_Name) const;
	template <typename T>
	void setName(const T p_Handle, const VkObjectType p_Type, const char* p_Name) const
	{
		setName(p_Type, reinterpret_cast<uint64_t>(p_Handle), p_Name);
	}

	void waitIdle() const;

private:
	void createInstance();
	void pickPhysicalDevice();
	void createDevice();
	void createAllocator();

	VkInstance m_Instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
	bool m_DebugUtils = false;
	bool m_ValidationLayer = false;
	VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	VmaAllocator m_Allocator = VK_NULL_HANDLE;
	VkQueue m_Queue = VK_NULL_HANDLE;
	uint32_t m_QueueFamily = 0;
	uint32_t m_InstanceApi = 0;
	DeviceInfo m_Info{};
};
} // namespace wb::gfx
