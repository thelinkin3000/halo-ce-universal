/*
HOST_VK.H

The Vulkan renderer's host half (port/android/VULKAN.md): what host_vk_startup
brings up and keeps for the backend that phase 2 grows. host.h declares the
entry points the rest of the host uses (host_vk_startup and
host_renderer_vulkan); this is for host_vk.c's own files.
*/

#ifndef HOST_VK_H
#define HOST_VK_H

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "host_vk_driver.h"

/* the instance-level entry points, loaded through the driver's
vkGetInstanceProcAddr; each is the core function, or the extension's where the
instance is 1.0-level and the extension stands in for it */
#define HOST_VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
	X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceProperties2) X(vkGetDeviceProcAddr) \
	X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR)

/* what the startup keeps: nothing in it is made twice (phase 2 adds the device) */
struct host_vk
{
	PFN_vkGetInstanceProcAddr get_instance_proc;
	VkInstance instance;
	/* the version the instance was made for, and the one to use on the device:
	the lower of that and the device's own */
	uint32_t instance_api, api;
	VkPhysicalDevice physical;
	uint32_t queue_family;
	VkPhysicalDeviceProperties properties;
	/* the device extensions it lists, names only, VK_MAX_EXTENSION_NAME_SIZE each */
	char (*extensions)[VK_MAX_EXTENSION_NAME_SIZE];
	uint32_t extension_count;
	int validation;
	VkDebugUtilsMessengerEXT messenger;

#define X(name) PFN_##name name;
	HOST_VK_INSTANCE_FUNCTIONS(X)
#undef X
	PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
	PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
};

/* valid after host_vk_startup returned 1; zeroed otherwise */
extern struct host_vk host_vk;

#endif
