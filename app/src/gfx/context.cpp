module;
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <SDL3/SDL_stdinc.h>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"
#include "gfx/vma.hpp"

module wb.gfx.context;

import wb.gfx.commands;
import wb.platform.window;

namespace wb::gfx
{
namespace
{
constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t REQUIRED_API_VERSION = VK_API_VERSION_1_3;

std::atomic<uint32_t> s_ValidationErrors{ 0 };
std::atomic<uint32_t> s_ValidationWarnings{ 0 };

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(const VkDebugUtilsMessageSeverityFlagBitsEXT p_Severity, const VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* p_Data, void*)
{
	if ((p_Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		++s_ValidationErrors;
		spdlog::error("[vulkan] {}", p_Data->pMessage);
	}
	else if ((p_Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
	{
		++s_ValidationWarnings;
		spdlog::warn("[vulkan] {}", p_Data->pMessage);
	}
	else
	{
		spdlog::debug("[vulkan] {}", p_Data->pMessage);
	}
	return VK_FALSE;
}

// WB_VALIDATION=0/1 overrides the default (on in debug builds, off in release builds)
bool validationRequested()
{
	if (const char* l_Env = SDL_getenv("WB_VALIDATION"); l_Env != nullptr && l_Env[0] != '\0')
		return l_Env[0] != '0';
#ifdef WB_DEBUG
	return true;
#else
	return false;
#endif
}

bool hasLayer(const std::vector<VkLayerProperties>& p_Layers, const std::string_view p_Name)
{
	return std::ranges::any_of(p_Layers, [&](const VkLayerProperties& p_Layer) { return p_Name == p_Layer.layerName; });
}

bool hasExtension(const std::vector<VkExtensionProperties>& p_Extensions, const std::string_view p_Name)
{
	return std::ranges::any_of(p_Extensions, [&](const VkExtensionProperties& p_Ext) { return p_Name == p_Ext.extensionName; });
}

std::string versionString(const uint32_t p_Version)
{
	return std::to_string(VK_API_VERSION_MAJOR(p_Version)) + "." + std::to_string(VK_API_VERSION_MINOR(p_Version)) + "." + std::to_string(VK_API_VERSION_PATCH(p_Version));
}

const char* deviceTypeName(const VkPhysicalDeviceType p_Type)
{
	switch (p_Type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		return "cpu";
	default:
		return "other";
	}
}

// Feature structs of everything the app needs; all of these are mandatory in Vulkan 1.3.
struct FeatureChain
{
	VkPhysicalDeviceFeatures2 core{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceVulkan12Features v12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceVulkan13Features v13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };

	FeatureChain()
	{
		core.pNext = &v12;
		v12.pNext = &v13;
	}
	FeatureChain(const FeatureChain&) = delete;
	FeatureChain& operator=(const FeatureChain&) = delete;
};

// Returns the name of the first missing required feature, or nothing if all are supported
std::optional<std::string_view> missingRequiredFeature(const FeatureChain& p_Features)
{
	const VkPhysicalDeviceVulkan12Features& l_V12 = p_Features.v12;
	const VkPhysicalDeviceVulkan13Features& l_V13 = p_Features.v13;
	if (!l_V13.dynamicRendering)
		return "dynamicRendering";
	if (!l_V13.synchronization2)
		return "synchronization2";
	if (!l_V13.maintenance4)
		return "maintenance4";
	if (!l_V13.shaderDemoteToHelperInvocation)
		return "shaderDemoteToHelperInvocation";
	if (!l_V12.timelineSemaphore)
		return "timelineSemaphore";
	if (!l_V12.descriptorIndexing)
		return "descriptorIndexing";
	if (!l_V12.runtimeDescriptorArray)
		return "runtimeDescriptorArray";
	if (!l_V12.descriptorBindingPartiallyBound)
		return "descriptorBindingPartiallyBound";
	if (!l_V12.shaderSampledImageArrayNonUniformIndexing)
		return "shaderSampledImageArrayNonUniformIndexing";
	if (!l_V12.descriptorBindingSampledImageUpdateAfterBind)
		return "descriptorBindingSampledImageUpdateAfterBind";
	if (!l_V12.descriptorBindingUpdateUnusedWhilePending)
		return "descriptorBindingUpdateUnusedWhilePending";
	return std::nullopt;
}

struct Candidate
{
	VkPhysicalDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	int64_t score = 0;
};
} // namespace

ValidationStats GraphicsContext::validationStats()
{
	return ValidationStats{ .errors = s_ValidationErrors.load(), .warnings = s_ValidationWarnings.load() };
}

void GraphicsContext::init(const platform::Window& p_Window)
{
	if (volkInitialize() != VK_SUCCESS)
		throw UnsupportedGpuError("No Vulkan driver was found.\nPlease install or update your graphics driver.");

	const uint32_t l_LoaderVersion = volkGetInstanceVersion();
	if (l_LoaderVersion < REQUIRED_API_VERSION)
		throw UnsupportedGpuError("The installed Vulkan runtime is version " + versionString(l_LoaderVersion) + ", but Vulkan 1.3 is required.\nPlease update your graphics driver.");

	createInstance();
	m_Surface = p_Window.createSurface(m_Instance);
	pickPhysicalDevice();
	createDevice();
	createAllocator();
}

void GraphicsContext::createInstance()
{
	uint32_t l_LayerCount = 0;
	WB_VK_CHECK(vkEnumerateInstanceLayerProperties(&l_LayerCount, nullptr));
	std::vector<VkLayerProperties> l_Layers(l_LayerCount);
	WB_VK_CHECK(vkEnumerateInstanceLayerProperties(&l_LayerCount, l_Layers.data()));

	uint32_t l_ExtensionCount = 0;
	WB_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &l_ExtensionCount, nullptr));
	std::vector<VkExtensionProperties> l_AvailableExtensions(l_ExtensionCount);
	WB_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &l_ExtensionCount, l_AvailableExtensions.data()));

	std::vector<const char*> l_Extensions;
	for (const char* l_Ext : platform::Window::requiredInstanceExtensions())
		l_Extensions.push_back(l_Ext);

	std::vector<const char*> l_EnabledLayers;
	const bool l_WantValidation = validationRequested();
	if (l_WantValidation)
	{
		if (hasLayer(l_Layers, VALIDATION_LAYER))
		{
			l_EnabledLayers.push_back(VALIDATION_LAYER);
			m_ValidationLayer = true;
		}
		else
			spdlog::warn("Validation requested but {} is not installed", VALIDATION_LAYER);
	}

	m_DebugUtils = l_WantValidation && hasExtension(l_AvailableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (m_DebugUtils)
		l_Extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	const VkApplicationInfo l_AppInfo{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Whiteboard",
		.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
		.pEngineName = "Whiteboard",
		.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
		.apiVersion = REQUIRED_API_VERSION,
	};

	const VkDebugUtilsMessengerCreateInfoEXT l_MessengerInfo{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = debugCallback,
	};

	const VkInstanceCreateInfo l_CreateInfo{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = m_DebugUtils ? &l_MessengerInfo : nullptr, // also covers vkCreate/DestroyInstance
		.pApplicationInfo = &l_AppInfo,
		.enabledLayerCount = static_cast<uint32_t>(l_EnabledLayers.size()),
		.ppEnabledLayerNames = l_EnabledLayers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(l_Extensions.size()),
		.ppEnabledExtensionNames = l_Extensions.data(),
	};
	WB_VK_CHECK(vkCreateInstance(&l_CreateInfo, nullptr, &m_Instance));
	volkLoadInstanceOnly(m_Instance);

	if (m_DebugUtils)
		WB_VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_Instance, &l_MessengerInfo, nullptr, &m_DebugMessenger));
	g_DebugLabelsEnabled = m_DebugUtils;

	spdlog::info("Vulkan instance created (loader {}, validation {})", versionString(volkGetInstanceVersion()), l_EnabledLayers.empty() ? "off" : "on");
}

void GraphicsContext::pickPhysicalDevice()
{
	uint32_t l_Count = 0;
	WB_VK_CHECK(vkEnumeratePhysicalDevices(m_Instance, &l_Count, nullptr));
	std::vector<VkPhysicalDevice> l_Devices(l_Count);
	WB_VK_CHECK(vkEnumeratePhysicalDevices(m_Instance, &l_Count, l_Devices.data()));

	// WB_GPU=<substring> forces a device by name (handy on multi-GPU machines and in WSL)
	const char* l_Forced = SDL_getenv("WB_GPU");
	std::optional<Candidate> l_Best;
	std::string l_Rejections;

	for (const VkPhysicalDevice l_Device : l_Devices)
	{
		VkPhysicalDeviceProperties l_Props{};
		vkGetPhysicalDeviceProperties(l_Device, &l_Props);
		const std::string l_Name = l_Props.deviceName;
		const auto l_Reject = [&](const std::string_view p_Reason)
		{
			spdlog::info("GPU '{}' rejected: {}", l_Name, p_Reason);
			l_Rejections += "\n- " + l_Name + ": " + std::string(p_Reason);
		};

		if (l_Forced != nullptr && l_Forced[0] != '\0' && l_Name.find(l_Forced) == std::string::npos)
			continue;
		if (l_Props.apiVersion < REQUIRED_API_VERSION)
		{
			l_Reject("supports Vulkan " + versionString(l_Props.apiVersion) + ", needs 1.3");
			continue;
		}

		uint32_t l_ExtCount = 0;
		vkEnumerateDeviceExtensionProperties(l_Device, nullptr, &l_ExtCount, nullptr);
		std::vector<VkExtensionProperties> l_Exts(l_ExtCount);
		vkEnumerateDeviceExtensionProperties(l_Device, nullptr, &l_ExtCount, l_Exts.data());
		if (!hasExtension(l_Exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
		{
			l_Reject("no swapchain support");
			continue;
		}

		FeatureChain l_Features;
		vkGetPhysicalDeviceFeatures2(l_Device, &l_Features.core);
		if (const std::optional<std::string_view> l_Missing = missingRequiredFeature(l_Features))
		{
			l_Reject("missing feature " + std::string(*l_Missing));
			continue;
		}

		uint32_t l_FamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(l_Device, &l_FamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> l_Families(l_FamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(l_Device, &l_FamilyCount, l_Families.data());

		std::optional<uint32_t> l_Family;
		for (uint32_t i = 0; i < l_FamilyCount && !l_Family; ++i)
		{
			VkBool32 l_Present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(l_Device, i, m_Surface, &l_Present);
			if ((l_Families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && l_Present == VK_TRUE)
				l_Family = i;
		}
		if (!l_Family)
		{
			l_Reject("no queue that can both render and present to the window");
			continue;
		}

		uint32_t l_FormatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(l_Device, m_Surface, &l_FormatCount, nullptr);
		if (l_FormatCount == 0)
		{
			l_Reject("cannot present to the window surface");
			continue;
		}

		VkPhysicalDeviceMemoryProperties l_Memory{};
		vkGetPhysicalDeviceMemoryProperties(l_Device, &l_Memory);
		VkDeviceSize l_LocalBytes = 0;
		for (uint32_t i = 0; i < l_Memory.memoryHeapCount; ++i)
		{
			if ((l_Memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
				l_LocalBytes = std::max(l_LocalBytes, l_Memory.memoryHeaps[i].size);
		}

		// Preference only: every type is accepted
		int64_t l_Score = 0;
		switch (l_Props.deviceType)
		{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			l_Score = 4'000'000;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			l_Score = 3'000'000;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			l_Score = 2'000'000;
			break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			l_Score = 1'000'000;
			break;
		default:
			l_Score = 0;
			break;
		}
		l_Score += static_cast<int64_t>(l_LocalBytes / (1024ull * 1024ull)) % 1'000'000;

		spdlog::info("GPU candidate '{}' ({}, Vulkan {}), score {}", l_Name, deviceTypeName(l_Props.deviceType), versionString(l_Props.apiVersion), l_Score);
		if (!l_Best || l_Score > l_Best->score)
			l_Best = Candidate{ .device = l_Device, .queueFamily = *l_Family, .score = l_Score };
	}

	if (!l_Best)
	{
		std::string l_Message = "No compatible GPU was found. Whiteboard needs a GPU with Vulkan 1.3 support.";
		if (l_Forced != nullptr && l_Forced[0] != '\0')
			l_Message += "\n(WB_GPU is set to '" + std::string(l_Forced) + "'.)";
		if (!l_Rejections.empty())
			l_Message += "\n" + l_Rejections;
		throw UnsupportedGpuError(l_Message);
	}

	m_PhysicalDevice = l_Best->device;
	m_QueueFamily = l_Best->queueFamily;

	VkPhysicalDeviceDriverProperties l_Driver{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 l_Props2{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &l_Driver };
	vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &l_Props2);
	const VkPhysicalDeviceProperties& l_Props = l_Props2.properties;

	VkPhysicalDeviceFeatures l_CoreFeatures{};
	vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &l_CoreFeatures);

	uint32_t l_FamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &l_FamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> l_Families(l_FamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &l_FamilyCount, l_Families.data());

	VkPhysicalDeviceMemoryProperties l_Memory{};
	vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &l_Memory);

	m_Info.name = l_Props.deviceName;
	m_Info.driver = std::string(l_Driver.driverName) + " " + l_Driver.driverInfo;
	m_Info.type = l_Props.deviceType;
	m_Info.apiVersion = l_Props.apiVersion;
	m_Info.vendorId = l_Props.vendorID;
	m_Info.deviceId = l_Props.deviceID;
	m_Info.limits = l_Props.limits;
	m_Info.samplerAnisotropy = l_CoreFeatures.samplerAnisotropy == VK_TRUE;
	m_Info.timestamps = l_Families[m_QueueFamily].timestampValidBits > 0 && l_Props.limits.timestampPeriod > 0.f;
	m_Info.timestampPeriodNs = l_Props.limits.timestampPeriod;
	for (uint32_t i = 0; i < l_Memory.memoryHeapCount; ++i)
	{
		if ((l_Memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
			m_Info.deviceLocalBytes = std::max(m_Info.deviceLocalBytes, l_Memory.memoryHeaps[i].size);
	}

	spdlog::info("Using GPU '{}' ({}, Vulkan {}, driver: {})", m_Info.name, deviceTypeName(m_Info.type), versionString(m_Info.apiVersion), m_Info.driver);
}

void GraphicsContext::createDevice()
{
	FeatureChain l_Features;
	l_Features.core.features.samplerAnisotropy = m_Info.samplerAnisotropy ? VK_TRUE : VK_FALSE;
	l_Features.v12.timelineSemaphore = VK_TRUE;
	l_Features.v12.descriptorIndexing = VK_TRUE;
	l_Features.v12.runtimeDescriptorArray = VK_TRUE;
	l_Features.v12.descriptorBindingPartiallyBound = VK_TRUE;
	l_Features.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	l_Features.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	l_Features.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
	l_Features.v13.dynamicRendering = VK_TRUE;
	l_Features.v13.synchronization2 = VK_TRUE;
	l_Features.v13.maintenance4 = VK_TRUE;
	l_Features.v13.shaderDemoteToHelperInvocation = VK_TRUE; // `discard` in Slang

	constexpr float l_Priority = 1.f;
	const VkDeviceQueueCreateInfo l_QueueInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = m_QueueFamily,
		.queueCount = 1,
		.pQueuePriorities = &l_Priority,
	};

	const char* const l_Extensions[]{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkDeviceCreateInfo l_CreateInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &l_Features.core,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &l_QueueInfo,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = l_Extensions,
	};
	WB_VK_CHECK(vkCreateDevice(m_PhysicalDevice, &l_CreateInfo, nullptr, &m_Device));
	volkLoadDevice(m_Device);
	vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
	setName(m_Queue, VK_OBJECT_TYPE_QUEUE, "main queue");
}

void GraphicsContext::createAllocator()
{
	VmaVulkanFunctions l_Functions{};
	const VmaAllocatorCreateInfo l_ImportInfo{
		.physicalDevice = m_PhysicalDevice,
		.device = m_Device,
		.instance = m_Instance,
		.vulkanApiVersion = REQUIRED_API_VERSION,
	};
	WB_VK_CHECK(vmaImportVulkanFunctionsFromVolk(&l_ImportInfo, &l_Functions));

	VmaAllocatorCreateInfo l_CreateInfo = l_ImportInfo;
	l_CreateInfo.pVulkanFunctions = &l_Functions;
	WB_VK_CHECK(vmaCreateAllocator(&l_CreateInfo, &m_Allocator));
}

void GraphicsContext::setName(const VkObjectType p_Type, const uint64_t p_Handle, const char* p_Name) const
{
	if (!m_DebugUtils || p_Handle == 0)
		return;
	const VkDebugUtilsObjectNameInfoEXT l_Info{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType = p_Type,
		.objectHandle = p_Handle,
		.pObjectName = p_Name,
	};
	vkSetDebugUtilsObjectNameEXT(m_Device, &l_Info);
}

void GraphicsContext::waitIdle() const
{
	if (m_Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_Device);
}

void GraphicsContext::destroy()
{
	if (m_Allocator != VK_NULL_HANDLE)
	{
		vmaDestroyAllocator(m_Allocator);
		m_Allocator = VK_NULL_HANDLE;
	}
	if (m_Device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_Device, nullptr);
		m_Device = VK_NULL_HANDLE;
	}
	if (m_Surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;
	}
	if (m_DebugMessenger != VK_NULL_HANDLE)
	{
		vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
		m_DebugMessenger = VK_NULL_HANDLE;
	}
	if (m_Instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_Instance, nullptr);
		m_Instance = VK_NULL_HANDLE;
	}
	volkFinalize();
}
} // namespace wb::gfx
