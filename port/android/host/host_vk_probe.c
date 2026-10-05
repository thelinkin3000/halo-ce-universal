/*
HOST_VK_PROBE.C

The Vulkan probe (port/android/VULKAN.md, phase 0): measures what a device's
Vulkan driver offers and costs, before a renderer is built on it. It runs
instead of the game when debug.vk_probe is set in config.toml, after the guest
image is loaded and SDL's video is up (host_main.c), and ends the app.

It is written to measure, not to be the backend: every step builds what it
needs, times it and destroys it. Results are lines "section.key: value" in
vk_probe.txt in the data folder (flushed after every line, so a probe that
dies leaves the line it died after) and in logcat with the prefix "vk probe:".

Steps, run in this order whatever order they are asked for in:
	caps       what the device has (step 1)
	memory     whether the GPU can read the guest's kind of memory in place (3)
	compile    glslang's cost on the device (4)
	pipelines  pipeline creation (5)
	draw       the game's converted shaders drawn with their descriptor sets (phase 0, part B)
	present    a swapchain, the surface's loss, the pacing (2; it waits for a tester)

A step that kills the probe is written to vk_probe_running.txt first; the next
run finds the file and leaves that step out (vk_probe_skip.txt) and says so.

Vulkan is loaded through the driver module (host_vk_driver.c: the phone's own
driver, or the archive display.vk_driver names, through libadrenotools) into a
table, and an entry point that is missing is reported by name, never called
through a null pointer. The host does not link libvulkan, so the GL ES path
loads nothing new. The surface is made on the window's ANativeWindow with
vkCreateAndroidSurfaceKHR: no SDL_Vulkan_* call is made, because SDL would load
the phone's driver beside the chosen one.
*/

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "host.h"
#include "host_vk_driver.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "glslang/Include/glslang_c_interface.h"
#include "glslang/Public/resource_limits_c.h"

/* HALO_PROBE_BUILD: the commit the build is from, written by tools/android_build_stamp.py at every build */
#include "probe_build.h"

/* ---------- the report */

static FILE *report_file;
static char report_path[640];

static void rep(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void rep(const char *format, ...)
{
	char line[1536];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (report_file)
	{
		fprintf(report_file, "%s\n", line);
		fflush(report_file);
	}
	host_logf(HOST_LOG_INFO, "vk probe: %s", line);
}

static const char *yesno(int value)
{
	return value ? "yes" : "no";
}

/* ---------- time */

static uint64_t now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

static uint64_t cpu_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

static int compare_doubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* the first value on its own, then the minimum and median of the rest
(milliseconds) */
static void rep_times(const char *key, const double *values, int count)
{
	double *sorted;

	if (count < 1)
		return;
	if (count == 1)
	{
		rep("%s: first %.3f ms", key, values[0]);
		return;
	}
	sorted = malloc(sizeof(double) * (size_t)(count - 1));
	memcpy(sorted, values + 1, sizeof(double) * (size_t)(count - 1));
	qsort(sorted, (size_t)(count - 1), sizeof(double), compare_doubles);
	rep("%s: first %.3f ms, then min %.3f ms, median %.3f ms, max %.3f ms (%d more)", key, values[0],
		sorted[0], sorted[(count - 1) / 2], sorted[count - 2], count - 1);
	free(sorted);
}

/* min, median, p99 and max of all the values, milliseconds */
static void rep_spread(const char *key, const double *values, int count)
{
	double *sorted;

	if (count < 1)
		return;
	sorted = malloc(sizeof(double) * (size_t)count);
	memcpy(sorted, values, sizeof(double) * (size_t)count);
	qsort(sorted, (size_t)count, sizeof(double), compare_doubles);
	rep("%s: min %.3f ms, median %.3f ms, p99 %.3f ms, max %.3f ms (%d)", key, sorted[0], sorted[count / 2],
		sorted[(count * 99) / 100 < count ? (count * 99) / 100 : count - 1], sorted[count - 1], count);
	free(sorted);
}

/* ---------- Vulkan's entry points, in a table of the same names

VK_NO_PROTOTYPES leaves the names free, so the code reads as Vulkan's own. */

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
static PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;

#define INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
	X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
	X(vkDestroySurfaceKHR) X(vkCreateAndroidSurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceExternalBufferProperties)

#define VALIDATION_FUNCTIONS(X) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)

#define DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
	X(vkQueuePresentKHR) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
	X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) X(vkCreateBuffer) X(vkDestroyBuffer) \
	X(vkBindBufferMemory) X(vkGetBufferMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) \
	X(vkBindImageMemory) X(vkGetImageMemoryRequirements) X(vkCreateImageView) X(vkDestroyImageView) \
	X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
	X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
	X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
	X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
	X(vkDestroySemaphore) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
	X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) \
	X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) \
	X(vkCmdClearAttachments) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
	X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
	X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetLineWidth) \
	X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBounds) \
	X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
	X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR)

/* functions of extensions (or of a core version, under the extension's name
on an older device): loaded only when the extension is enabled or the
version is there, and found under the core name first */
#define OPTIONAL_FUNCTIONS(X) \
	X(vkCmdBeginRendering, KHR, "VK_KHR_dynamic_rendering", 3) \
	X(vkCmdEndRendering, KHR, "VK_KHR_dynamic_rendering", 3) \
	X(vkCmdSetCullMode, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetFrontFace, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetPrimitiveTopology, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetViewportWithCount, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetScissorWithCount, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdBindVertexBuffers2, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetDepthTestEnable, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetDepthWriteEnable, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetDepthCompareOp, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetDepthBoundsTestEnable, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetStencilTestEnable, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetStencilOp, EXT, "VK_EXT_extended_dynamic_state", 3) \
	X(vkCmdSetRasterizerDiscardEnable, EXT, "VK_EXT_extended_dynamic_state2", 3) \
	X(vkCmdSetDepthBiasEnable, EXT, "VK_EXT_extended_dynamic_state2", 3) \
	X(vkCmdSetPrimitiveRestartEnable, EXT, "VK_EXT_extended_dynamic_state2", 3) \
	X(vkCmdSetLogicOpEXT, , "VK_EXT_extended_dynamic_state2", 99) \
	X(vkCmdSetDepthClampEnableEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetPolygonModeEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetRasterizationSamplesEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetSampleMaskEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetAlphaToCoverageEnableEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetAlphaToOneEnableEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetLogicOpEnableEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetColorBlendEnableEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetColorBlendEquationEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetColorWriteMaskEXT, , "VK_EXT_extended_dynamic_state3", 99) \
	X(vkCmdSetVertexInputEXT, , "VK_EXT_vertex_input_dynamic_state", 99) \
	X(vkGetMemoryHostPointerPropertiesEXT, , "VK_EXT_external_memory_host", 99) \
	X(vkGetMemoryFdKHR, , "VK_KHR_external_memory_fd", 99) \
	X(vkCmdPushDescriptorSetKHR, , "VK_KHR_push_descriptor", 99)

#define X(name) static PFN_##name name;
INSTANCE_FUNCTIONS(X)
VALIDATION_FUNCTIONS(X)
DEVICE_FUNCTIONS(X)
#undef X
#define X(name, suffix, extension, core) static PFN_##name name;
OPTIONAL_FUNCTIONS(X)
#undef X

/* ---------- state of the probe */

#define MAXIMUM_EXTENSIONS 512

struct probe
{
	SDL_Window *window;
	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkSurfaceKHR surface;
	char driver_setting[256];     /* display.vk_driver */
	char driver_description[512]; /* what the driver module opened */
	uint32_t instance_api;  /* what the instance was made for */
	uint32_t device_api;    /* the device's own */
	uint32_t api;           /* the lesser: what the code may call as core */
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceMemoryProperties memory;
	VkPhysicalDeviceFeatures2 features2;
	VkPhysicalDeviceVulkan13Features features13;
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1;
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids;
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_khr;
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties;
	VkQueueFamilyProperties family_properties;
	char (*extensions)[VK_MAX_EXTENSION_NAME_SIZE];
	int extension_count;
	char (*enabled)[VK_MAX_EXTENSION_NAME_SIZE];
	int enabled_count;
	int validation;       /* the layer is on */
	int dynamic_rendering; /* vkCmdBeginRendering is loaded */
	int eds1_on, eds2_on, eds3_on, vids_on;
	int host_import;       /* VK_EXT_external_memory_host is enabled */
	int timestamps;        /* the queue's family can write timestamps */
	char data_root[512];
};

static struct probe P;

#define CHECK_RESULT(call) probe_check((call), #call, __LINE__)

static int probe_check(VkResult result, const char *what, int line)
{
	if (result != VK_SUCCESS)
	{
		rep("error: %s (line %d) returned %d", what, line, (int)result);
		return 0;
	}
	return 1;
}

static int have_extension(const char *name)
{
	int index;

	for (index = 0; index < P.extension_count; index++)
	{
		if (!strcmp(P.extensions[index], name))
			return 1;
	}
	return 0;
}

static int extension_enabled(const char *name)
{
	int index;

	for (index = 0; index < P.enabled_count; index++)
	{
		if (!strcmp(P.enabled[index], name))
			return 1;
	}
	return 0;
}

static void enable_extension(const char *name)
{
	if (extension_enabled(name))
		return;
	snprintf(P.enabled[P.enabled_count++], VK_MAX_EXTENSION_NAME_SIZE, "%s", name);
}

static char *version_text(uint32_t version)
{
	static char texts[4][32];
	static int next;
	char *text = texts[next++ & 3];

	snprintf(text, 32, "%u.%u.%u", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
	return text;
}

/* a negative test sets this, so that what the layer says of it is marked */
static int expecting_errors;
static int validation_messages;
static int validation_errors;
/* warnings and errors: what a module that the layer minds adds to */
static int validation_complaints;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	const char *level = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" :
		severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "warning" :
		severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT ? "info" : "verbose";

	(void)user;
	validation_messages++;
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		validation_errors++;
	if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
		validation_complaints++;
	rep("validation.message: [%s%s%s] %s: %s", level, expecting_errors ? ", from a negative test" : "",
		types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT ? ", performance" : "",
		data->pMessageIdName ? data->pMessageIdName : "-", data->pMessage ? data->pMessage : "");
	return VK_FALSE;
}

/* ---------- the config.toml settings the probe reads itself */

static int config_validation(void)
{
	char path[640];
	toml_result_t result;
	int value = 0;

	snprintf(path, sizeof(path), "%s/config.toml", P.data_root);
	result = toml_parse_file_ex(path);
	if (!result.ok)
		return 0;
	{
		toml_datum_t datum = toml_seek(result.toptab, "debug.vk_validation");

		value = datum.type == TOML_BOOLEAN && datum.u.boolean;
	}
	toml_free(result);
	return value;
}

/* a string setting of config.toml, "" if it is not there (the probe reads its settings itself) */
static void config_string_setting(const char *key, char *out, size_t size)
{
	char path[640];
	toml_result_t result;

	out[0] = 0;
	snprintf(path, sizeof(path), "%s/config.toml", P.data_root);
	result = toml_parse_file_ex(path);
	if (!result.ok)
		return;
	{
		toml_datum_t datum = toml_seek(result.toptab, key);

		if (datum.type == TOML_STRING)
			snprintf(out, size, "%s", datum.u.s);
	}
	toml_free(result);
}

/* ---------- instance, surface, device */

static int load_instance_functions(void)
{
	int missing = 0;

#define X(name) \
	name = (PFN_##name)vkGetInstanceProcAddr(P.instance, #name); \
	if (!name) { rep("error: the instance has no %s", #name); missing++; }
	INSTANCE_FUNCTIONS(X)
#undef X
	return !missing;
}

static void load_optional_functions(void)
{
	/* a function loaded only when the extension is enabled, or the version
	it became core in (the minor number) is there */
#define X(name, suffix, extension, core) \
	if (extension_enabled(extension) || P.api >= VK_MAKE_API_VERSION(0, 1, core, 0)) \
	{ \
		name = (PFN_##name)vkGetDeviceProcAddr(P.device, #name); \
		if (!name && sizeof(#suffix) > 1) \
		{ \
			static const char suffixed[] = #name #suffix; \
			name = (PFN_##name)vkGetDeviceProcAddr(P.device, suffixed); \
		} \
	}
	OPTIONAL_FUNCTIONS(X)
#undef X
}

static int load_device_functions(void)
{
	int missing = 0;

#define X(name) \
	name = (PFN_##name)vkGetDeviceProcAddr(P.device, #name); \
	if (!name) { rep("error: the device has no %s", #name); missing++; }
	DEVICE_FUNCTIONS(X)
#undef X
	return !missing;
}

static int create_instance(void)
{
	uint32_t loader_version = VK_API_VERSION_1_0, count = 0, index;
	const char *extensions[16];
	uint32_t extension_count = 0;
	VkApplicationInfo application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	const char *layer = "VK_LAYER_KHRONOS_validation";
	VkLayerProperties *layers;
	int layer_found = 0;
	int want_validation = config_validation();
	VkExtensionProperties *available;
	uint32_t count_available;
	int has_debug_utils = 0;

	rep("driver.setting: \"%s\"", P.driver_setting);
	vkGetInstanceProcAddr = host_vk_driver_open(P.driver_setting, P.driver_description, sizeof(P.driver_description));
	rep("driver.opened: %s", P.driver_description);
	if (!vkGetInstanceProcAddr)
	{
		rep("caps.vulkan_library: no driver could be opened");
		return 0;
	}
	vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
	vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
	vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
		vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
	vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)
		vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceLayerProperties");
	if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties || !vkEnumerateInstanceLayerProperties)
	{
		rep("error: the loader lacks a global entry point");
		return 0;
	}
	if (vkEnumerateInstanceVersion)
		vkEnumerateInstanceVersion(&loader_version);
	rep("caps.instance_version: %s (loader)", version_text(loader_version));
	if (loader_version < VK_API_VERSION_1_1)
	{
		rep("caps.instance_version_too_old: the probe needs the loader to offer 1.1 (properties2); stopping");
		return 0;
	}
	P.instance_api = loader_version < VK_API_VERSION_1_3 ? loader_version : VK_API_VERSION_1_3;

	/* what the instance offers */
	vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
	available = malloc(sizeof(*available) * (count ? count : 1));
	vkEnumerateInstanceExtensionProperties(NULL, &count, available);
	count_available = count;
	for (index = 0; index < count; index++)
	{
		rep("caps.instance_extension: %s %u", available[index].extensionName, available[index].specVersion);
		if (!strcmp(available[index].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
			has_debug_utils = 1;
	}

	count = 0;
	vkEnumerateInstanceLayerProperties(&count, NULL);
	layers = malloc(sizeof(*layers) * (count ? count : 1));
	vkEnumerateInstanceLayerProperties(&count, layers);
	for (index = 0; index < count; index++)
	{
		rep("caps.instance_layer: %s (spec %s)", layers[index].layerName, version_text(layers[index].specVersion));
		if (!strcmp(layers[index].layerName, layer))
			layer_found = 1;
	}
	free(layers);
	rep("validation.requested: %s", yesno(want_validation));
	rep("validation.layer_found: %s", yesno(layer_found));

	/* the surface, the Android surface and (listed by the loader) properties2, each only if offered */
	{
		static const char *const wanted[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
			VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
		uint32_t w, a;

		for (w = 0; w < 3; w++)
		{
			for (a = 0; a < count_available && strcmp(available[a].extensionName, wanted[w]); a++)
				;
			if (a < count_available)
				extensions[extension_count++] = wanted[w];
			else
				rep("error: the instance does not offer %s", wanted[w]);
		}
	}
	free(available);
	if (want_validation && layer_found && has_debug_utils)
		extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	for (index = 0; index < extension_count; index++)
		rep("caps.instance_extension_enabled: %s", extensions[index]);

	application.pApplicationName = "Halo Vulkan probe";
	application.apiVersion = P.instance_api;
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = extension_count;
	info.ppEnabledExtensionNames = extensions;
	if (want_validation && layer_found)
	{
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = &layer;
		P.validation = 1;
	}
	if (!CHECK_RESULT(vkCreateInstance(&info, NULL, &P.instance)))
		return 0;
	rep("validation.enabled: %s", yesno(P.validation));
	if (!load_instance_functions())
		return 0;
	if (P.validation && has_debug_utils)
	{
		VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };

		vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr(P.instance, "vkCreateDebugUtilsMessengerEXT");
		vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr(P.instance, "vkDestroyDebugUtilsMessengerEXT");
		messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = debug_callback;
		if (vkCreateDebugUtilsMessengerEXT)
			CHECK_RESULT(vkCreateDebugUtilsMessengerEXT(P.instance, &messenger, NULL, &P.messenger));
	}
	return 1;
}

/* a surface on the window's current ANativeWindow (after the app was away it is a new one, so it is read
again each time); no SDL_Vulkan_* call, which would load the phone's driver beside the chosen one */
static int make_surface(int quiet)
{
	VkAndroidSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
	struct ANativeWindow *native = (struct ANativeWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(P.window),
		SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);

	if (!native)
	{
		if (!quiet)
			rep("error: the window has no ANativeWindow (%s)", SDL_GetError());
		return 0;
	}
	info.window = native;
	if (quiet)
		return vkCreateAndroidSurfaceKHR(P.instance, &info, NULL, &P.surface) == VK_SUCCESS;
	return CHECK_RESULT(vkCreateAndroidSurfaceKHR(P.instance, &info, NULL, &P.surface));
}

/* the window is made without SDL_WINDOW_VULKAN, which makes SDL load the system loader, and without
SDL_WINDOW_OPENGL, so SDL makes no EGL surface on it either */
static int create_surface(void)
{
	P.window = SDL_CreateWindow("Halo Vulkan probe", 0, 0, SDL_WINDOW_FULLSCREEN);
	if (!P.window)
	{
		rep("error: SDL_CreateWindow failed: %s", SDL_GetError());
		return 0;
	}
	return make_surface(0);
}

/* appends a structure to a pNext chain whose last structure is *tail */
static void chain_features(void *head, void *next, void **tail)
{
	(void)head;
	((VkBaseOutStructure *)*tail)->pNext = (VkBaseOutStructure *)next;
	*tail = next;
}

static int select_device(void)
{
	uint32_t count = 0, index, family_count, family;
	VkPhysicalDevice *devices;
	VkQueueFamilyProperties *families;

	CHECK_RESULT(vkEnumeratePhysicalDevices(P.instance, &count, NULL));
	devices = malloc(sizeof(*devices) * (count ? count : 1));
	CHECK_RESULT(vkEnumeratePhysicalDevices(P.instance, &count, devices));
	rep("caps.physical_devices: %u", count);
	for (index = 0; index < count; index++)
	{
		VkPhysicalDeviceProperties properties;

		vkGetPhysicalDeviceProperties(devices[index], &properties);
		rep("caps.physical_device.%u: %s (vendor %04x device %04x api %s)", index, properties.deviceName,
			properties.vendorID, properties.deviceID, version_text(properties.apiVersion));
	}
	for (index = 0; index < count && !P.physical; index++)
	{
		family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, NULL);
		families = malloc(sizeof(*families) * (family_count ? family_count : 1));
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0; family < family_count && !P.physical; family++)
		{
			VkBool32 present = VK_FALSE;

			vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, P.surface, &present);
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
			{
				P.physical = devices[index];
				P.family = family;
				P.family_properties = families[family];
			}
		}
		if (index == 0)
		{
			for (family = 0; family < family_count; family++)
				rep("caps.queue_family.%u: flags 0x%x count %u timestampValidBits %u", family,
					families[family].queueFlags, families[family].queueCount, families[family].timestampValidBits);
		}
		free(families);
	}
	free(devices);
	if (!P.physical)
	{
		rep("error: no device with a graphics queue that can present");
		return 0;
	}
	vkGetPhysicalDeviceProperties(P.physical, &P.properties);
	vkGetPhysicalDeviceMemoryProperties(P.physical, &P.memory);
	P.device_api = P.properties.apiVersion;
	P.api = P.device_api < P.instance_api ? P.device_api : P.instance_api;
	P.timestamps = P.family_properties.timestampValidBits != 0;
	rep("caps.selected: %s, queue family %u", P.properties.deviceName, P.family);
	return 1;
}

/* the device's extensions, and the features its structures can tell */
static void query_extensions_and_features(void)
{
	uint32_t count = 0, index;
	VkExtensionProperties *available;
	void *tail;

	vkEnumerateDeviceExtensionProperties(P.physical, NULL, &count, NULL);
	available = malloc(sizeof(*available) * (count ? count : 1));
	vkEnumerateDeviceExtensionProperties(P.physical, NULL, &count, available);
	P.extensions = calloc(MAXIMUM_EXTENSIONS, VK_MAX_EXTENSION_NAME_SIZE);
	P.enabled = calloc(MAXIMUM_EXTENSIONS, VK_MAX_EXTENSION_NAME_SIZE);
	for (index = 0; index < count && P.extension_count < MAXIMUM_EXTENSIONS; index++)
	{
		snprintf(P.extensions[P.extension_count++], VK_MAX_EXTENSION_NAME_SIZE, "%s", available[index].extensionName);
		rep("caps.extension: %s %u", available[index].extensionName, available[index].specVersion);
	}
	free(available);

	/* a structure of an extension the device does not list is not chained */
	memset(&P.features2, 0, sizeof(P.features2));
	P.features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	tail = &P.features2;
	if (P.api >= VK_API_VERSION_1_3)
	{
		memset(&P.features13, 0, sizeof(P.features13));
		P.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		chain_features(&P.features2, &P.features13, &tail);
	}
	else if (have_extension("VK_KHR_dynamic_rendering"))
	{
		memset(&P.dynamic_rendering_khr, 0, sizeof(P.dynamic_rendering_khr));
		P.dynamic_rendering_khr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
		chain_features(&P.features2, &P.dynamic_rendering_khr, &tail);
	}
	if (have_extension("VK_EXT_extended_dynamic_state"))
	{
		memset(&P.eds1, 0, sizeof(P.eds1));
		P.eds1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
		chain_features(&P.features2, &P.eds1, &tail);
	}
	if (have_extension("VK_EXT_extended_dynamic_state2"))
	{
		memset(&P.eds2, 0, sizeof(P.eds2));
		P.eds2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
		chain_features(&P.features2, &P.eds2, &tail);
	}
	if (have_extension("VK_EXT_extended_dynamic_state3"))
	{
		memset(&P.eds3, 0, sizeof(P.eds3));
		P.eds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
		chain_features(&P.features2, &P.eds3, &tail);
	}
	if (have_extension("VK_EXT_vertex_input_dynamic_state"))
	{
		memset(&P.vids, 0, sizeof(P.vids));
		P.vids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
		chain_features(&P.features2, &P.vids, &tail);
	}
	vkGetPhysicalDeviceFeatures2(P.physical, &P.features2);
}

/* ---------- step 1: what the device has */

struct report_features
{
	VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border;
	VkPhysicalDevice4444FormatsFeaturesEXT formats4444;
	VkPhysicalDeviceMaintenance4FeaturesKHR maintenance4;
	VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5;
	VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cache_control;
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT pipeline_library;
};

static const char *memory_flag_text(VkMemoryPropertyFlags flags)
{
	static char text[8][96];
	static int next;
	char *out = text[next++ & 7];

	snprintf(out, 96, "%s%s%s%s%s%s", flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device_local " : "",
		flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host_visible " : "",
		flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "host_coherent " : "",
		flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "host_cached " : "",
		flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? "lazy " : "",
		flags & VK_MEMORY_PROPERTY_PROTECTED_BIT ? "protected " : "");
	return out;
}

#define FORMAT_LIST(X) \
	X(B8G8R8A8_UNORM) X(R8G8B8A8_UNORM) X(A8B8G8R8_UNORM_PACK32) X(R5G6B5_UNORM_PACK16) \
	X(B5G6R5_UNORM_PACK16) X(A1R5G5B5_UNORM_PACK16) X(R5G5B5A1_UNORM_PACK16) X(B5G5R5A1_UNORM_PACK16) \
	X(R4G4B4A4_UNORM_PACK16) X(B4G4R4A4_UNORM_PACK16) X(A4R4G4B4_UNORM_PACK16_EXT) X(R8_UNORM) \
	X(R8G8_UNORM) X(R16_UNORM) X(R16G16_UNORM) X(R8G8_SNORM) X(R16G16_SNORM) X(BC1_RGBA_UNORM_BLOCK) \
	X(BC2_UNORM_BLOCK) X(BC3_UNORM_BLOCK) X(D16_UNORM) X(D24_UNORM_S8_UINT) X(D32_SFLOAT_S8_UINT) \
	X(X8_D24_UNORM_PACK32) X(D32_SFLOAT)

static const struct { const char *name; VkFormat format; } format_table[] = {
#define X(name) { #name, VK_FORMAT_##name },
	FORMAT_LIST(X)
#undef X
};

static void property_text(const char *name, char *out, size_t size)
{
	out[0] = 0;
	if (__system_property_get(name, out) <= 0)
		snprintf(out, size, "(none)");
}

static void report_device(void)
{
	char value[PROP_VALUE_MAX + 8];

	rep("build.git: %s", HALO_PROBE_BUILD);
	property_text("ro.product.model", value, sizeof(value));
	rep("device.model: %s", value);
	property_text("ro.product.manufacturer", value, sizeof(value));
	rep("device.manufacturer: %s", value);
	property_text("ro.hardware", value, sizeof(value));
	rep("device.hardware: %s", value);
	property_text("ro.hardware.vulkan", value, sizeof(value));
	rep("device.hardware_vulkan: %s", value);
	property_text("ro.board.platform", value, sizeof(value));
	rep("device.board_platform: %s", value);
	property_text("ro.build.version.sdk", value, sizeof(value));
	rep("device.android_api: %s", value);
	property_text("ro.build.version.release", value, sizeof(value));
	rep("device.android_release: %s", value);
}

static void step_caps(void)
{
	VkPhysicalDeviceProperties2 properties2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDevicePushDescriptorPropertiesKHR push = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
	struct report_features other;
	VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	void *tail;
	const VkPhysicalDeviceLimits *limits = &P.properties.limits;
	const VkPhysicalDeviceFeatures *features;
	uint32_t index;
	int have_driver = P.api >= VK_API_VERSION_1_2 || have_extension("VK_KHR_driver_properties");
	char value[PROP_VALUE_MAX + 8];

	rep("step.caps: start");
	rep("caps.device.api_version: %s", version_text(P.properties.apiVersion));
	rep("caps.device.driver_version: 0x%x (%u.%u.%u)", P.properties.driverVersion,
		VK_VERSION_MAJOR(P.properties.driverVersion), VK_VERSION_MINOR(P.properties.driverVersion),
		VK_VERSION_PATCH(P.properties.driverVersion));
	rep("caps.device.vendor_id: 0x%04x", P.properties.vendorID);
	rep("caps.device.device_id: 0x%04x", P.properties.deviceID);
	rep("caps.device.name: %s", P.properties.deviceName);
	rep("caps.device.type: %d", (int)P.properties.deviceType);
	rep("caps.device.pipeline_cache_uuid: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		P.properties.pipelineCacheUUID[0], P.properties.pipelineCacheUUID[1], P.properties.pipelineCacheUUID[2],
		P.properties.pipelineCacheUUID[3], P.properties.pipelineCacheUUID[4], P.properties.pipelineCacheUUID[5],
		P.properties.pipelineCacheUUID[6], P.properties.pipelineCacheUUID[7], P.properties.pipelineCacheUUID[8],
		P.properties.pipelineCacheUUID[9], P.properties.pipelineCacheUUID[10], P.properties.pipelineCacheUUID[11],
		P.properties.pipelineCacheUUID[12], P.properties.pipelineCacheUUID[13], P.properties.pipelineCacheUUID[14],
		P.properties.pipelineCacheUUID[15]);
	property_text("ro.hardware.vulkan", value, sizeof(value));
	rep("caps.device.ro_hardware_vulkan: %s", value);
	property_text("ro.board.platform", value, sizeof(value));
	rep("caps.device.ro_board_platform: %s", value);
	rep("caps.api_used_as_core: %s", version_text(P.api));

	/* properties */
	properties2.pNext = NULL;
	tail = &properties2;
	if (have_driver)
		chain_features(&properties2, &driver, &tail);
	if (have_extension("VK_KHR_push_descriptor"))
		chain_features(&properties2, &push, &tail);
	if (have_extension("VK_EXT_external_memory_host"))
		chain_features(&properties2, &host, &tail);
	vkGetPhysicalDeviceProperties2(P.physical, &properties2);
	if (have_driver)
	{
		rep("caps.driver.id: %d", (int)driver.driverID);
		rep("caps.driver.name: %s", driver.driverName);
		rep("caps.driver.info: %s", driver.driverInfo);
		rep("caps.driver.conformance: %u.%u.%u.%u", driver.conformanceVersion.major, driver.conformanceVersion.minor,
			driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
	}
	else
	{
		rep("caps.driver: not reported (no VK_KHR_driver_properties)");
	}
	if (have_extension("VK_EXT_external_memory_host"))
	{
		rep("caps.min_imported_host_pointer_alignment: %llu", (unsigned long long)host.minImportedHostPointerAlignment);
		P.host_properties = host;
	}
	if (have_extension("VK_KHR_push_descriptor"))
		rep("caps.max_push_descriptors: %u", push.maxPushDescriptors);

	/* features that decide the design */
	rep("caps.feature.dynamic_rendering: %s", yesno(P.api >= VK_API_VERSION_1_3 ? P.features13.dynamicRendering :
		P.dynamic_rendering_khr.dynamicRendering));
	rep("caps.feature.dynamic_rendering_extension: %s", yesno(have_extension("VK_KHR_dynamic_rendering")));
	rep("caps.feature.extended_dynamic_state: %s%s", yesno(P.eds1.extendedDynamicState),
		P.api >= VK_API_VERSION_1_3 ? " (core in 1.3)" : "");
	rep("caps.feature.extended_dynamic_state2: %s%s", yesno(P.eds2.extendedDynamicState2),
		P.api >= VK_API_VERSION_1_3 ? " (core in 1.3)" : "");
	rep("caps.feature.extended_dynamic_state2_logic_op: %s", yesno(P.eds2.extendedDynamicState2LogicOp));
	rep("caps.feature.extended_dynamic_state2_patch_control_points: %s", yesno(P.eds2.extendedDynamicState2PatchControlPoints));
#define EDS3(name) rep("caps.feature.extended_dynamic_state3_" #name ": %s", yesno(P.eds3.extendedDynamicState3##name));
	EDS3(TessellationDomainOrigin) EDS3(DepthClampEnable) EDS3(PolygonMode) EDS3(RasterizationSamples)
	EDS3(SampleMask) EDS3(AlphaToCoverageEnable) EDS3(AlphaToOneEnable) EDS3(LogicOpEnable)
	EDS3(ColorBlendEnable) EDS3(ColorBlendEquation) EDS3(ColorWriteMask) EDS3(RasterizationStream)
	EDS3(ConservativeRasterizationMode) EDS3(ExtraPrimitiveOverestimationSize) EDS3(DepthClipEnable)
	EDS3(SampleLocationsEnable) EDS3(ColorBlendAdvanced) EDS3(ProvokingVertexMode) EDS3(LineRasterizationMode)
	EDS3(LineStippleEnable) EDS3(DepthClipNegativeOneToOne) EDS3(ViewportWScalingEnable) EDS3(ViewportSwizzle)
	EDS3(CoverageToColorEnable) EDS3(CoverageToColorLocation) EDS3(CoverageModulationMode)
	EDS3(CoverageModulationTableEnable) EDS3(CoverageModulationTable) EDS3(CoverageReductionMode)
	EDS3(RepresentativeFragmentTestEnable) EDS3(ShadingRateImageEnable)
#undef EDS3
	rep("caps.feature.vertex_input_dynamic_state: %s", yesno(P.vids.vertexInputDynamicState));
	rep("caps.feature.external_memory_host: %s", yesno(have_extension("VK_EXT_external_memory_host")));
	rep("caps.feature.push_descriptor: %s", yesno(have_extension("VK_KHR_push_descriptor")));

	memset(&other, 0, sizeof(other));
	features2.pNext = NULL;
	tail = &features2;
	other.custom_border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
	other.formats4444.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT;
	other.maintenance4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR;
	other.maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
	other.cache_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
	other.pipeline_library.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
	if (have_extension("VK_EXT_custom_border_color"))
		chain_features(&features2, &other.custom_border, &tail);
	if (have_extension("VK_EXT_4444_formats"))
		chain_features(&features2, &other.formats4444, &tail);
	if (have_extension("VK_KHR_maintenance4"))
		chain_features(&features2, &other.maintenance4, &tail);
	if (have_extension("VK_KHR_maintenance5"))
		chain_features(&features2, &other.maintenance5, &tail);
	if (have_extension("VK_EXT_pipeline_creation_cache_control"))
		chain_features(&features2, &other.cache_control, &tail);
	if (have_extension("VK_EXT_graphics_pipeline_library"))
		chain_features(&features2, &other.pipeline_library, &tail);
	vkGetPhysicalDeviceFeatures2(P.physical, &features2);
	rep("caps.feature.custom_border_color: %s (extension %s)", yesno(other.custom_border.customBorderColors),
		yesno(have_extension("VK_EXT_custom_border_color")));
	rep("caps.feature.custom_border_color_without_format: %s", yesno(other.custom_border.customBorderColorWithoutFormat));
	rep("caps.feature.4444_formats: a4r4g4b4 %s, a4b4g4r4 %s (extension %s)", yesno(other.formats4444.formatA4R4G4B4),
		yesno(other.formats4444.formatA4B4G4R4), yesno(have_extension("VK_EXT_4444_formats")));
	rep("caps.feature.maintenance4: %s (extension %s%s)", yesno(other.maintenance4.maintenance4 ||
		(P.api >= VK_API_VERSION_1_3 && P.features13.maintenance4)), yesno(have_extension("VK_KHR_maintenance4")),
		P.api >= VK_API_VERSION_1_3 ? ", core in 1.3" : "");
	rep("caps.feature.maintenance5: %s (extension %s)", yesno(other.maintenance5.maintenance5),
		yesno(have_extension("VK_KHR_maintenance5")));
	rep("caps.feature.pipeline_creation_cache_control: %s (extension %s)", yesno(other.cache_control.pipelineCreationCacheControl ||
		(P.api >= VK_API_VERSION_1_3 && P.features13.pipelineCreationCacheControl)),
		yesno(have_extension("VK_EXT_pipeline_creation_cache_control")));
	rep("caps.feature.graphics_pipeline_library: %s (extension %s)", yesno(other.pipeline_library.graphicsPipelineLibrary),
		yesno(have_extension("VK_EXT_graphics_pipeline_library")));
	rep("caps.feature.pipeline_library_extension: %s", yesno(have_extension("VK_KHR_pipeline_library")));

	{
		VkPhysicalDeviceFeatures core;

		vkGetPhysicalDeviceFeatures(P.physical, &core);
		features = &core;
		rep("caps.feature.textureCompressionBC: %s", yesno(features->textureCompressionBC));
		rep("caps.feature.textureCompressionASTC_LDR: %s", yesno(features->textureCompressionASTC_LDR));
		rep("caps.feature.textureCompressionETC2: %s", yesno(features->textureCompressionETC2));
		rep("caps.feature.samplerAnisotropy: %s", yesno(features->samplerAnisotropy));
		rep("caps.feature.occlusionQueryPrecise: %s", yesno(features->occlusionQueryPrecise));
		rep("caps.feature.depthBiasClamp: %s", yesno(features->depthBiasClamp));
		rep("caps.feature.depthClamp: %s", yesno(features->depthClamp));
		rep("caps.feature.fillModeNonSolid: %s", yesno(features->fillModeNonSolid));
		rep("caps.feature.independentBlend: %s", yesno(features->independentBlend));
		rep("caps.feature.logicOp: %s", yesno(features->logicOp));
		rep("caps.feature.shaderClipDistance: %s", yesno(features->shaderClipDistance));
		rep("caps.feature.wideLines: %s", yesno(features->wideLines));
		rep("caps.feature.pipelineStatisticsQuery: %s", yesno(features->pipelineStatisticsQuery));
	}

	/* limits */
	rep("caps.limit.maxVertexInputAttributes: %u", limits->maxVertexInputAttributes);
	rep("caps.limit.maxVertexInputBindings: %u", limits->maxVertexInputBindings);
	rep("caps.limit.maxVertexInputAttributeOffset: %u", limits->maxVertexInputAttributeOffset);
	rep("caps.limit.maxVertexInputBindingStride: %u", limits->maxVertexInputBindingStride);
	rep("caps.limit.maxUniformBufferRange: %u", limits->maxUniformBufferRange);
	rep("caps.limit.maxPushConstantsSize: %u", limits->maxPushConstantsSize);
	rep("caps.limit.maxBoundDescriptorSets: %u", limits->maxBoundDescriptorSets);
	rep("caps.limit.maxPerStageDescriptorSamplers: %u", limits->maxPerStageDescriptorSamplers);
	rep("caps.limit.maxSamplerAnisotropy: %.1f", limits->maxSamplerAnisotropy);
	rep("caps.limit.maxSamplerLodBias: %.1f", limits->maxSamplerLodBias);
	rep("caps.limit.maxImageDimension2D: %u", limits->maxImageDimension2D);
	rep("caps.limit.maxImageDimension3D: %u", limits->maxImageDimension3D);
	rep("caps.limit.maxImageDimensionCube: %u", limits->maxImageDimensionCube);
	rep("caps.limit.maxColorAttachments: %u", limits->maxColorAttachments);
	rep("caps.limit.minUniformBufferOffsetAlignment: %llu", (unsigned long long)limits->minUniformBufferOffsetAlignment);
	rep("caps.limit.nonCoherentAtomSize: %llu", (unsigned long long)limits->nonCoherentAtomSize);
	rep("caps.limit.optimalBufferCopyOffsetAlignment: %llu", (unsigned long long)limits->optimalBufferCopyOffsetAlignment);
	rep("caps.limit.optimalBufferCopyRowPitchAlignment: %llu", (unsigned long long)limits->optimalBufferCopyRowPitchAlignment);
	rep("caps.limit.timestampPeriod: %.4f ns", limits->timestampPeriod);
	rep("caps.limit.timestampComputeAndGraphics: %s", yesno(limits->timestampComputeAndGraphics));
	rep("caps.queue.timestampValidBits: %u", P.family_properties.timestampValidBits);
	rep("caps.limit.maxMemoryAllocationCount: %u", limits->maxMemoryAllocationCount);

	/* memory */
	for (index = 0; index < P.memory.memoryHeapCount; index++)
		rep("caps.memory_heap.%u: %llu MB%s", index, (unsigned long long)(P.memory.memoryHeaps[index].size >> 20),
			P.memory.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device_local" : "");
	{
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		VkBuffer buffer;
		VkMemoryRequirements requirements;

		info.size = 65536;
		info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (CHECK_RESULT(vkCreateBuffer(P.device, &info, NULL, &buffer)))
		{
			vkGetBufferMemoryRequirements(P.device, buffer, &requirements);
			rep("caps.memory.buffer_type_bits: 0x%x (size %llu, alignment %llu)", requirements.memoryTypeBits,
				(unsigned long long)requirements.size, (unsigned long long)requirements.alignment);
			for (index = 0; index < P.memory.memoryTypeCount; index++)
				rep("caps.memory_type.%u: heap %u, %s(a plain buffer %s use it)", index, P.memory.memoryTypes[index].heapIndex,
					memory_flag_text(P.memory.memoryTypes[index].propertyFlags),
					requirements.memoryTypeBits & (1u << index) ? "can" : "cannot");
			vkDestroyBuffer(P.device, buffer, NULL);
		}
	}

	/* formats */
	for (index = 0; index < sizeof(format_table) / sizeof(format_table[0]); index++)
	{
		VkFormatProperties properties;
		VkFlags bits;

		vkGetPhysicalDeviceFormatProperties(P.physical, format_table[index].format, &properties);
		bits = properties.optimalTilingFeatures;
		rep("caps.format.%s: sampled=%s linear=%s color=%s blend=%s depth=%s transfer_dst=%s transfer_src=%s",
			format_table[index].name, yesno(bits & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
			yesno(bits & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
			yesno(bits & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT), yesno(bits & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT),
			yesno(bits & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT), yesno(bits & VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
			yesno(bits & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT));
	}

	/* the surface */
	{
		VkSurfaceCapabilitiesKHR capabilities;
		uint32_t format_count = 0, mode_count = 0;
		VkSurfaceFormatKHR *formats;
		VkPresentModeKHR *modes;

		if (CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.physical, P.surface, &capabilities)))
		{
			rep("caps.surface.image_count: min %u max %u", capabilities.minImageCount, capabilities.maxImageCount);
			rep("caps.surface.current_extent: %ux%u", capabilities.currentExtent.width, capabilities.currentExtent.height);
			rep("caps.surface.min_extent: %ux%u", capabilities.minImageExtent.width, capabilities.minImageExtent.height);
			rep("caps.surface.max_extent: %ux%u", capabilities.maxImageExtent.width, capabilities.maxImageExtent.height);
			rep("caps.surface.max_image_array_layers: %u", capabilities.maxImageArrayLayers);
			rep("caps.surface.supported_transforms: 0x%x", capabilities.supportedTransforms);
			rep("caps.surface.current_transform: 0x%x", capabilities.currentTransform);
			rep("caps.surface.supported_composite_alpha: 0x%x", capabilities.supportedCompositeAlpha);
			rep("caps.surface.supported_usage: 0x%x (color attachment %s, transfer dst %s, transfer src %s, sampled %s)",
				capabilities.supportedUsageFlags, yesno(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
				yesno(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT),
				yesno(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
				yesno(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT));
		}
		vkGetPhysicalDeviceSurfaceFormatsKHR(P.physical, P.surface, &format_count, NULL);
		formats = malloc(sizeof(*formats) * (format_count ? format_count : 1));
		vkGetPhysicalDeviceSurfaceFormatsKHR(P.physical, P.surface, &format_count, formats);
		for (index = 0; index < format_count; index++)
			rep("caps.surface.format.%u: format %d colorspace %d", index, (int)formats[index].format, (int)formats[index].colorSpace);
		free(formats);
		vkGetPhysicalDeviceSurfacePresentModesKHR(P.physical, P.surface, &mode_count, NULL);
		modes = malloc(sizeof(*modes) * (mode_count ? mode_count : 1));
		vkGetPhysicalDeviceSurfacePresentModesKHR(P.physical, P.surface, &mode_count, modes);
		for (index = 0; index < mode_count; index++)
			rep("caps.surface.present_mode.%u: %d", index, (int)modes[index]);
		free(modes);
	}
	rep("step.caps: done");
}

/* ---------- the device */

static VkPhysicalDeviceVulkan13Features create13;
static VkPhysicalDeviceDynamicRenderingFeaturesKHR create_dynamic_rendering;
static VkPhysicalDeviceExtendedDynamicStateFeaturesEXT create_eds1;
static VkPhysicalDeviceExtendedDynamicState2FeaturesEXT create_eds2;
static VkPhysicalDeviceExtendedDynamicState3FeaturesEXT create_eds3;
static VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT create_vids;

static int create_device(void)
{
	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	const char *names[64];
	float priority = 1.0f;
	void *tail = &features;
	int index, dynamic_rendering;

	enable_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	dynamic_rendering = P.api >= VK_API_VERSION_1_3 ? P.features13.dynamicRendering : P.dynamic_rendering_khr.dynamicRendering;
	if (dynamic_rendering)
	{
		if (P.api >= VK_API_VERSION_1_3)
		{
			create13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			create13.dynamicRendering = VK_TRUE;
			chain_features(&features, &create13, &tail);
		}
		else
		{
			/* the extension and what it depends on, where they are not core */
			static const char *const dependencies[] = { "VK_KHR_depth_stencil_resolve", "VK_KHR_create_renderpass2",
				"VK_KHR_multiview", "VK_KHR_maintenance2" };

			enable_extension("VK_KHR_dynamic_rendering");
			if (P.api < VK_API_VERSION_1_2)
			{
				for (index = 0; index < 4; index++)
					if (have_extension(dependencies[index]))
						enable_extension(dependencies[index]);
			}
			create_dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
			create_dynamic_rendering.dynamicRendering = VK_TRUE;
			chain_features(&features, &create_dynamic_rendering, &tail);
		}
	}
	if (P.eds1.extendedDynamicState)
	{
		enable_extension("VK_EXT_extended_dynamic_state");
		create_eds1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
		create_eds1.extendedDynamicState = VK_TRUE;
		chain_features(&features, &create_eds1, &tail);
		P.eds1_on = 1;
	}
	if (P.eds2.extendedDynamicState2)
	{
		enable_extension("VK_EXT_extended_dynamic_state2");
		create_eds2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
		create_eds2.extendedDynamicState2 = VK_TRUE;
		create_eds2.extendedDynamicState2LogicOp = P.eds2.extendedDynamicState2LogicOp;
		chain_features(&features, &create_eds2, &tail);
		P.eds2_on = 1;
	}
	if (have_extension("VK_EXT_extended_dynamic_state3"))
	{
		/* the states of the probe's pipelines: no more of the extension is asked for */
		create_eds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
		create_eds3.extendedDynamicState3DepthClampEnable = P.eds3.extendedDynamicState3DepthClampEnable;
		create_eds3.extendedDynamicState3PolygonMode = P.eds3.extendedDynamicState3PolygonMode;
		create_eds3.extendedDynamicState3RasterizationSamples = P.eds3.extendedDynamicState3RasterizationSamples;
		create_eds3.extendedDynamicState3SampleMask = P.eds3.extendedDynamicState3SampleMask;
		create_eds3.extendedDynamicState3AlphaToCoverageEnable = P.eds3.extendedDynamicState3AlphaToCoverageEnable;
		create_eds3.extendedDynamicState3AlphaToOneEnable = P.eds3.extendedDynamicState3AlphaToOneEnable;
		create_eds3.extendedDynamicState3LogicOpEnable = P.eds3.extendedDynamicState3LogicOpEnable;
		create_eds3.extendedDynamicState3ColorBlendEnable = P.eds3.extendedDynamicState3ColorBlendEnable;
		create_eds3.extendedDynamicState3ColorBlendEquation = P.eds3.extendedDynamicState3ColorBlendEquation;
		create_eds3.extendedDynamicState3ColorWriteMask = P.eds3.extendedDynamicState3ColorWriteMask;
		if (create_eds3.extendedDynamicState3DepthClampEnable || create_eds3.extendedDynamicState3PolygonMode ||
			create_eds3.extendedDynamicState3RasterizationSamples || create_eds3.extendedDynamicState3SampleMask ||
			create_eds3.extendedDynamicState3AlphaToCoverageEnable || create_eds3.extendedDynamicState3AlphaToOneEnable ||
			create_eds3.extendedDynamicState3LogicOpEnable || create_eds3.extendedDynamicState3ColorBlendEnable ||
			create_eds3.extendedDynamicState3ColorBlendEquation || create_eds3.extendedDynamicState3ColorWriteMask)
		{
			enable_extension("VK_EXT_extended_dynamic_state3");
			chain_features(&features, &create_eds3, &tail);
			P.eds3_on = 1;
		}
	}
	if (P.vids.vertexInputDynamicState)
	{
		enable_extension("VK_EXT_vertex_input_dynamic_state");
		create_vids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
		create_vids.vertexInputDynamicState = VK_TRUE;
		chain_features(&features, &create_vids, &tail);
		P.vids_on = 1;
	}
	if (have_extension("VK_EXT_external_memory_host"))
	{
		enable_extension("VK_EXT_external_memory_host");
		/* the extension depends on VK_KHR_external_memory, core in 1.1 */
		if (P.api < VK_API_VERSION_1_1 && have_extension("VK_KHR_external_memory"))
			enable_extension("VK_KHR_external_memory");
		P.host_import = 1;
	}
	/* step draw's push-descriptor case */
	if (have_extension("VK_KHR_push_descriptor"))
		enable_extension("VK_KHR_push_descriptor");
	/* BC1 textures for step draw, where the device has them */
	features.features.textureCompressionBC = P.features2.features.textureCompressionBC;
	/* the beyond-the-plan export case of step 3 */
	if (have_extension("VK_KHR_external_memory_fd"))
	{
		enable_extension("VK_KHR_external_memory_fd");
		if (P.api < VK_API_VERSION_1_1 && have_extension("VK_KHR_external_memory"))
			enable_extension("VK_KHR_external_memory");
	}
	for (index = 0; index < P.enabled_count; index++)
	{
		names[index] = P.enabled[index];
		rep("caps.device_extension_enabled: %s", P.enabled[index]);
	}

	queue_info.queueFamilyIndex = P.family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	info.pNext = &features;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue_info;
	info.enabledExtensionCount = (uint32_t)P.enabled_count;
	info.ppEnabledExtensionNames = names;
	if (!CHECK_RESULT(vkCreateDevice(P.physical, &info, NULL, &P.device)))
		return 0;
	if (!load_device_functions())
		return 0;
	load_optional_functions();
	vkGetDeviceQueue(P.device, P.family, 0, &P.queue);
	P.dynamic_rendering = vkCmdBeginRendering != NULL && vkCmdEndRendering != NULL && dynamic_rendering;
	rep("caps.dynamic_rendering_used: %s", yesno(P.dynamic_rendering));
	return 1;
}

/* a risky sub-step's guard (below, with the skip file): false if it killed the probe before */
static int guard_begin(const char *name);
static void guard_end(void);

/* ---------- helpers: memory, buffers, images, commands */

static VkCommandPool main_pool;
static VkCommandBuffer main_command;
static VkFence main_fence;

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoid)
{
	uint32_t index;

	for (index = 0; index < P.memory.memoryTypeCount; index++)
	{
		VkMemoryPropertyFlags flags = P.memory.memoryTypes[index].propertyFlags;

		if ((bits & (1u << index)) && (flags & required) == required && !(flags & avoid))
			return index;
	}
	return UINT32_MAX;
}

struct buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *map;
	VkDeviceSize size;
	uint32_t type;
};

static int make_buffer(struct buffer *out, VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoid, int map)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

	memset(out, 0, sizeof(*out));
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!CHECK_RESULT(vkCreateBuffer(P.device, &info, NULL, &out->buffer)))
		return 0;
	vkGetBufferMemoryRequirements(P.device, out->buffer, &requirements);
	out->type = find_memory_type(requirements.memoryTypeBits, required, avoid);
	if (out->type == UINT32_MAX)
	{
		rep("error: no memory type with flags 0x%x for a buffer", required);
		vkDestroyBuffer(P.device, out->buffer, NULL);
		out->buffer = VK_NULL_HANDLE;
		return 0;
	}
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = out->type;
	if (!CHECK_RESULT(vkAllocateMemory(P.device, &allocate, NULL, &out->memory)))
		return 0;
	if (!CHECK_RESULT(vkBindBufferMemory(P.device, out->buffer, out->memory, 0)))
		return 0;
	out->size = size;
	if (map && !CHECK_RESULT(vkMapMemory(P.device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map)))
		return 0;
	return 1;
}

static void free_buffer(struct buffer *buffer)
{
	if (buffer->map)
		vkUnmapMemory(P.device, buffer->memory);
	if (buffer->buffer)
		vkDestroyBuffer(P.device, buffer->buffer, NULL);
	if (buffer->memory)
		vkFreeMemory(P.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

struct image
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFormat format;
	uint32_t width, height;
	VkImageAspectFlags aspect;
};

static int make_image(struct image *out, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	int depth = format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D24_UNORM_S8_UINT ||
		format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT;

	memset(out, 0, sizeof(*out));
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!CHECK_RESULT(vkCreateImage(P.device, &info, NULL, &out->image)))
		return 0;
	vkGetImageMemoryRequirements(P.device, out->image, &requirements);
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (allocate.memoryTypeIndex == UINT32_MAX)
		allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, 0, 0);
	if (!CHECK_RESULT(vkAllocateMemory(P.device, &allocate, NULL, &out->memory)))
		return 0;
	if (!CHECK_RESULT(vkBindImageMemory(P.device, out->image, out->memory, 0)))
		return 0;
	out->format = format;
	out->width = width;
	out->height = height;
	out->aspect = depth ? (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
		format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
		: VK_IMAGE_ASPECT_COLOR_BIT;
	if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
	{
		view.image = out->image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		/* a view's aspect is one of depth or color (a sampled or attached depth-stencil image
		takes both only in an attachment view's case: both bits) */
		view.subresourceRange.aspectMask = out->aspect;
		view.subresourceRange.levelCount = 1;
		view.subresourceRange.layerCount = 1;
		if (!CHECK_RESULT(vkCreateImageView(P.device, &view, NULL, &out->view)))
			return 0;
	}
	return 1;
}

static void free_image(struct image *image)
{
	if (image->view)
		vkDestroyImageView(P.device, image->view, NULL);
	if (image->image)
		vkDestroyImage(P.device, image->image, NULL);
	if (image->memory)
		vkFreeMemory(P.device, image->memory, NULL);
	memset(image, 0, sizeof(*image));
}

/* a whole-subresource barrier, conservative on purpose: the probe measures
drivers, not barriers */
static void image_barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

	barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* makes everything before it visible to everything after it (buffers) */
static void memory_barrier(VkCommandBuffer command)
{
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

static int make_command_pool(VkCommandPool *pool)
{
	VkCommandPoolCreateInfo info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };

	info.queueFamilyIndex = P.family;
	info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	return CHECK_RESULT(vkCreateCommandPool(P.device, &info, NULL, pool));
}

static int allocate_command(VkCommandPool pool, VkCommandBuffer *command)
{
	VkCommandBufferAllocateInfo info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };

	info.commandPool = pool;
	info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	info.commandBufferCount = 1;
	return CHECK_RESULT(vkAllocateCommandBuffers(P.device, &info, command));
}

static int make_fence(VkFence *fence, int signaled)
{
	VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

	info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
	return CHECK_RESULT(vkCreateFence(P.device, &info, NULL, fence));
}

static VkCommandBuffer command_begin(void)
{
	VkCommandBufferBeginInfo info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

	vkResetCommandPool(P.device, main_pool, 0);
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(main_command, &info);
	return main_command;
}

#define WAIT_FOREVER 10000000000ull

/* ends the buffer, submits it and waits; 0 on failure (and the reason is logged) */
static int command_submit_wait(VkCommandBuffer command)
{
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkResult result;

	if (!CHECK_RESULT(vkEndCommandBuffer(command)))
		return 0;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	vkResetFences(P.device, 1, &main_fence);
	if (!CHECK_RESULT(vkQueueSubmit(P.queue, 1, &submit, main_fence)))
		return 0;
	result = vkWaitForFences(P.device, 1, &main_fence, VK_TRUE, WAIT_FOREVER);
	if (result != VK_SUCCESS)
	{
		rep("error: the fence did not signal in 10 s (result %d)", (int)result);
		return 0;
	}
	return 1;
}

/* ---------- the shader compiler (glslang, loaded only when asked for) */

struct glslang_api
{
	void *library;
	int (*initialize_process)(void);
	void (*finalize_process)(void);
	glslang_shader_t *(*shader_create)(const glslang_input_t *);
	void (*shader_delete)(glslang_shader_t *);
	int (*shader_preprocess)(glslang_shader_t *, const glslang_input_t *);
	int (*shader_parse)(glslang_shader_t *, const glslang_input_t *);
	const char *(*shader_info_log)(glslang_shader_t *);
	glslang_program_t *(*program_create)(void);
	void (*program_delete)(glslang_program_t *);
	void (*program_add_shader)(glslang_program_t *, glslang_shader_t *);
	int (*program_link)(glslang_program_t *, int);
	void (*program_spirv_generate)(glslang_program_t *, glslang_stage_t);
	size_t (*program_spirv_size)(glslang_program_t *);
	unsigned int *(*program_spirv_ptr)(glslang_program_t *);
	const char *(*program_spirv_messages)(glslang_program_t *);
	const char *(*program_info_log)(glslang_program_t *);
	const glslang_resource_t *(*default_resource)(void);
	int initialized;
};

static struct glslang_api G;

static int glslang_load(void)
{
	if (G.initialized)
		return 1;
	if (G.library)
		return 0;
	G.library = dlopen("libhalo_glslang.so", RTLD_NOW | RTLD_LOCAL);
	if (!G.library)
	{
		rep("error: libhalo_glslang.so did not load: %s", dlerror());
		G.library = (void *)1;
		return 0;
	}
#define LOAD(field, name) \
	if (!(*(void **)&G.field = dlsym(G.library, name))) { rep("error: glslang has no %s", name); return 0; }
	LOAD(initialize_process, "glslang_initialize_process")
	LOAD(finalize_process, "glslang_finalize_process")
	LOAD(shader_create, "glslang_shader_create")
	LOAD(shader_delete, "glslang_shader_delete")
	LOAD(shader_preprocess, "glslang_shader_preprocess")
	LOAD(shader_parse, "glslang_shader_parse")
	LOAD(shader_info_log, "glslang_shader_get_info_log")
	LOAD(program_create, "glslang_program_create")
	LOAD(program_delete, "glslang_program_delete")
	LOAD(program_add_shader, "glslang_program_add_shader")
	LOAD(program_link, "glslang_program_link")
	LOAD(program_spirv_generate, "glslang_program_SPIRV_generate")
	LOAD(program_spirv_size, "glslang_program_SPIRV_get_size")
	LOAD(program_spirv_ptr, "glslang_program_SPIRV_get_ptr")
	LOAD(program_spirv_messages, "glslang_program_SPIRV_get_messages")
	LOAD(program_info_log, "glslang_program_get_info_log")
	LOAD(default_resource, "glslang_default_resource")
#undef LOAD
	if (!G.initialize_process())
	{
		rep("error: glslang_initialize_process failed");
		return 0;
	}
	G.initialized = 1;
	return 1;
}

static void glslang_unload(void)
{
	if (G.initialized)
		G.finalize_process();
	G.initialized = 0;
}

struct spirv
{
	uint32_t *words;
	size_t count; /* in 32-bit words */
};

/* source to SPIR-V for Vulkan 1.0; the timing is of this call alone, in nanoseconds */
static int glslang_compile(const char *source, glslang_stage_t stage, struct spirv *out, uint64_t *wall, uint64_t *cpu, int quiet)
{
	glslang_input_t input;
	glslang_shader_t *shader;
	glslang_program_t *program;
	uint64_t wall_start = now_ns(), cpu_start = cpu_ns();
	int ok = 0;

	memset(&input, 0, sizeof(input));
	input.language = GLSLANG_SOURCE_GLSL;
	input.stage = stage;
	input.client = GLSLANG_CLIENT_VULKAN;
	input.client_version = GLSLANG_TARGET_VULKAN_1_0;
	input.target_language = GLSLANG_TARGET_SPV;
	input.target_language_version = GLSLANG_TARGET_SPV_1_0;
	input.code = source;
	input.default_version = 450;
	input.default_profile = GLSLANG_NO_PROFILE;
	input.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
	input.resource = G.default_resource();
	out->words = NULL;
	out->count = 0;
	shader = G.shader_create(&input);
	if (!G.shader_preprocess(shader, &input))
	{
		if (!quiet)
			rep("error: glslang preprocess: %s", G.shader_info_log(shader));
		G.shader_delete(shader);
		return 0;
	}
	if (!G.shader_parse(shader, &input))
	{
		if (!quiet)
			rep("error: glslang parse: %s", G.shader_info_log(shader));
		G.shader_delete(shader);
		return 0;
	}
	program = G.program_create();
	G.program_add_shader(program, shader);
	if (G.program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
	{
		G.program_spirv_generate(program, stage);
		out->count = G.program_spirv_size(program);
		if (out->count)
		{
			out->words = malloc(out->count * 4);
			memcpy(out->words, G.program_spirv_ptr(program), out->count * 4);
			ok = 1;
		}
		else if (!quiet)
			rep("error: glslang made no SPIR-V: %s", G.program_spirv_messages(program));
	}
	else if (!quiet)
	{
		rep("error: glslang link: %s", G.program_info_log(program));
	}
	G.program_delete(program);
	G.shader_delete(shader);
	if (wall)
		*wall = now_ns() - wall_start;
	if (cpu)
		*cpu = cpu_ns() - cpu_start;
	return ok;
}

/* a file of the APK's assets/vk_probe/, as text the caller frees */
static char *asset_text(const char *name)
{
	char path[160];
	size_t size = 0;
	void *data;
	char *text;

	snprintf(path, sizeof(path), "vk_probe/%s", name);
	data = SDL_LoadFile(path, &size);
	if (!data)
	{
		rep("error: the APK has no %s (%s)", path, SDL_GetError());
		return NULL;
	}
	text = malloc(size + 1);
	memcpy(text, data, size);
	text[size] = 0;
	SDL_free(data);
	return text;
}

static glslang_stage_t stage_of(const char *name)
{
	size_t length = strlen(name);

	return length > 5 && !strcmp(name + length - 5, ".vert") ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
}

/* an asset compiled; NULL after logging why */
static int compile_asset(const char *name, struct spirv *out)
{
	char *text = asset_text(name);
	int ok;

	if (!text)
		return 0;
	if (!glslang_load())
	{
		free(text);
		return 0;
	}
	ok = glslang_compile(text, stage_of(name), out, NULL, NULL, 0);
	free(text);
	return ok;
}

static VkShaderModule make_shader_module(const struct spirv *code)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule module = VK_NULL_HANDLE;

	info.codeSize = code->count * 4;
	info.pCode = code->words;
	if (!CHECK_RESULT(vkCreateShaderModule(P.device, &info, NULL, &module)))
		return VK_NULL_HANDLE;
	return module;
}

/* ---------- render targets and pipelines shared by the steps */

struct vertex_layout
{
	uint32_t stride;
	int count;
	struct { uint32_t location; VkFormat format; uint32_t offset; } attributes[16];
};

/* position (two floats) and colour (four unsigned bytes): the probe's own shaders' inputs */
static const struct vertex_layout layout_color = { 12, 2, { { 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, VK_FORMAT_R8G8B8A8_UNORM, 8 } } };

struct target
{
	struct image color, depth;
	int has_depth;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
};

static int format_has_stencil(VkFormat format)
{
	return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkRenderPass make_render_pass(VkFormat color, VkFormat depth)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference color_reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depth_reference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	VkRenderPass pass = VK_NULL_HANDLE;

	memset(attachments, 0, sizeof(attachments));
	memset(&subpass, 0, sizeof(subpass));
	memset(dependencies, 0, sizeof(dependencies));
	attachments[0].format = color;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[1].format = depth;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = format_has_stencil(depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pDepthStencilAttachment = depth ? &depth_reference : NULL;
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	dependencies[1] = dependencies[0];
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	info.attachmentCount = depth ? 2 : 1;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	if (!CHECK_RESULT(vkCreateRenderPass(P.device, &info, NULL, &pass)))
		return VK_NULL_HANDLE;
	return pass;
}

static int target_make(struct target *target, uint32_t width, uint32_t height, VkFormat color, VkFormat depth)
{
	memset(target, 0, sizeof(*target));
	if (!make_image(&target->color, width, height, color,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		return 0;
	if (depth)
	{
		if (!make_image(&target->depth, width, height, depth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
			return 0;
		target->has_depth = 1;
	}
	if (!P.dynamic_rendering)
	{
		VkImageView views[2] = { target->color.view, target->depth.view };
		VkFramebufferCreateInfo info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

		target->render_pass = make_render_pass(color, depth);
		if (!target->render_pass)
			return 0;
		info.renderPass = target->render_pass;
		info.attachmentCount = depth ? 2 : 1;
		info.pAttachments = views;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (!CHECK_RESULT(vkCreateFramebuffer(P.device, &info, NULL, &target->framebuffer)))
			return 0;
	}
	return 1;
}

static void target_free(struct target *target)
{
	if (target->framebuffer)
		vkDestroyFramebuffer(P.device, target->framebuffer, NULL);
	if (target->render_pass)
		vkDestroyRenderPass(P.device, target->render_pass, NULL);
	free_image(&target->depth);
	free_image(&target->color);
	memset(target, 0, sizeof(*target));
}

/* begins rendering into the target, clearing it (whatever it held before is lost); with
dynamic rendering the layouts are changed here, with a render pass by the pass itself */
static void pass_begin(VkCommandBuffer command, struct target *target, const float color[4])
{
	VkClearValue clears[2];

	memset(clears, 0, sizeof(clears));
	memcpy(clears[0].color.float32, color, sizeof(float) * 4);
	clears[1].depthStencil.depth = 1.0f;
	if (P.dynamic_rendering)
	{
		VkRenderingAttachmentInfo color_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		VkRenderingAttachmentInfo depth_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };

		image_barrier(command, target->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		color_attachment.imageView = target->color.view;
		color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color_attachment.clearValue = clears[0];
		info.renderArea.extent.width = target->color.width;
		info.renderArea.extent.height = target->color.height;
		info.layerCount = 1;
		info.colorAttachmentCount = 1;
		info.pColorAttachments = &color_attachment;
		if (target->has_depth)
		{
			image_barrier(command, target->depth.image, target->depth.aspect, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			depth_attachment.imageView = target->depth.view;
			depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depth_attachment.clearValue = clears[1];
			info.pDepthAttachment = &depth_attachment;
			if (format_has_stencil(target->depth.format))
				info.pStencilAttachment = &depth_attachment;
		}
		vkCmdBeginRendering(command, &info);
	}
	else
	{
		VkRenderPassBeginInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };

		info.renderPass = target->render_pass;
		info.framebuffer = target->framebuffer;
		info.renderArea.extent.width = target->color.width;
		info.renderArea.extent.height = target->color.height;
		info.clearValueCount = target->has_depth ? 2 : 1;
		info.pClearValues = clears;
		vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
	}
}

static void pass_end(VkCommandBuffer command)
{
	if (P.dynamic_rendering)
		vkCmdEndRendering(command);
	else
		vkCmdEndRenderPass(command);
}

/* the render passes pipelines are made against when the device has no dynamic rendering, one per
format pair (a pipeline needs a compatible pass, not the same one) */
static struct { VkFormat color, depth; VkRenderPass pass; } pipeline_passes[4];

static VkRenderPass pipeline_render_pass(VkFormat color, VkFormat depth)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		if (pipeline_passes[index].pass && pipeline_passes[index].color == color && pipeline_passes[index].depth == depth)
			return pipeline_passes[index].pass;
		if (!pipeline_passes[index].pass)
		{
			pipeline_passes[index].color = color;
			pipeline_passes[index].depth = depth;
			pipeline_passes[index].pass = make_render_pass(color, depth);
			return pipeline_passes[index].pass;
		}
	}
	return VK_NULL_HANDLE;
}

/* The states made dynamic: `full` is everything this device can, otherwise only the viewport
and the scissor (core states, with no extension at all). Where the extended state's
variant that carries a count exists, it replaces the plain one: both may not be listed. */
struct dynamic_set
{
	VkDynamicState states[64];
	int count;
	int counted_viewport; /* the viewport and scissor are set with a count (extended dynamic state) */
	int stride;           /* the vertex stride is set when binding (extended dynamic state) */
	int vertex_input;     /* the vertex input is set by command (vertex input dynamic state) */
};

static void dynamic_add(struct dynamic_set *set, VkDynamicState state)
{
	set->states[set->count++] = state;
}

static void dynamic_choose(struct dynamic_set *set, int full)
{
	memset(set, 0, sizeof(*set));
	if (full && P.eds1_on && vkCmdSetViewportWithCount && vkCmdSetScissorWithCount)
	{
		dynamic_add(set, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
		dynamic_add(set, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
		set->counted_viewport = 1;
	}
	else
	{
		dynamic_add(set, VK_DYNAMIC_STATE_VIEWPORT);
		dynamic_add(set, VK_DYNAMIC_STATE_SCISSOR);
	}
	if (!full)
		return;
	dynamic_add(set, VK_DYNAMIC_STATE_LINE_WIDTH);
	dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_BIAS);
	dynamic_add(set, VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	dynamic_add(set, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	dynamic_add(set, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	dynamic_add(set, VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	if (P.eds1_on && vkCmdSetCullMode && vkCmdSetFrontFace && vkCmdSetPrimitiveTopology && vkCmdSetDepthTestEnable &&
		vkCmdSetDepthWriteEnable && vkCmdSetDepthCompareOp && vkCmdSetDepthBoundsTestEnable &&
		vkCmdSetStencilTestEnable && vkCmdSetStencilOp)
	{
		dynamic_add(set, VK_DYNAMIC_STATE_CULL_MODE);
		dynamic_add(set, VK_DYNAMIC_STATE_FRONT_FACE);
		dynamic_add(set, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
		dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
		dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_STENCIL_OP);
		/* the stride, unless the vertex input is dynamic: that state carries it */
		if (!(P.vids_on && vkCmdSetVertexInputEXT) && vkCmdBindVertexBuffers2)
		{
			dynamic_add(set, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
			set->stride = 1;
		}
	}
	if (P.eds2_on && vkCmdSetRasterizerDiscardEnable && vkCmdSetDepthBiasEnable && vkCmdSetPrimitiveRestartEnable)
	{
		dynamic_add(set, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE);
		dynamic_add(set, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
		if (create_eds2.extendedDynamicState2LogicOp && vkCmdSetLogicOpEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_LOGIC_OP_EXT);
	}
	if (P.eds3_on)
	{
		if (create_eds3.extendedDynamicState3DepthClampEnable && vkCmdSetDepthClampEnableEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
		if (create_eds3.extendedDynamicState3PolygonMode && vkCmdSetPolygonModeEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
		if (create_eds3.extendedDynamicState3RasterizationSamples && vkCmdSetRasterizationSamplesEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
		if (create_eds3.extendedDynamicState3SampleMask && vkCmdSetSampleMaskEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT);
		if (create_eds3.extendedDynamicState3AlphaToCoverageEnable && vkCmdSetAlphaToCoverageEnableEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
		if (create_eds3.extendedDynamicState3AlphaToOneEnable && vkCmdSetAlphaToOneEnableEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT);
		if (create_eds3.extendedDynamicState3LogicOpEnable && vkCmdSetLogicOpEnableEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT);
		if (create_eds3.extendedDynamicState3ColorBlendEnable && vkCmdSetColorBlendEnableEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
		if (create_eds3.extendedDynamicState3ColorBlendEquation && vkCmdSetColorBlendEquationEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
		if (create_eds3.extendedDynamicState3ColorWriteMask && vkCmdSetColorWriteMaskEXT)
			dynamic_add(set, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
	}
	if (P.vids_on && vkCmdSetVertexInputEXT)
	{
		dynamic_add(set, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
		set->vertex_input = 1;
	}
}

static void dynamic_report(const char *key, const struct dynamic_set *set)
{
	char text[1024];
	int index, length = 0;

	for (index = 0; index < set->count && length < (int)sizeof(text) - 24; index++)
		length += snprintf(text + length, sizeof(text) - (size_t)length, "%s%d", index ? "," : "", (int)set->states[index]);
	rep("%s: %d states (VkDynamicState values %s)", key, set->count, text);
}

/* the vertex input as the dynamic-state command takes it */
static void set_vertex_input(VkCommandBuffer command, const struct vertex_layout *layout)
{
	VkVertexInputBindingDescription2EXT binding = { VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT };
	VkVertexInputAttributeDescription2EXT attributes[16];
	int index;

	binding.binding = 0;
	binding.stride = layout->stride;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	binding.divisor = 1;
	for (index = 0; index < layout->count; index++)
	{
		memset(&attributes[index], 0, sizeof(attributes[index]));
		attributes[index].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
		attributes[index].location = layout->attributes[index].location;
		attributes[index].binding = 0;
		attributes[index].format = layout->attributes[index].format;
		attributes[index].offset = layout->attributes[index].offset;
	}
	vkCmdSetVertexInputEXT(command, 1, &binding, (uint32_t)layout->count, attributes);
}

/* sets every state of the set, with the values of the probe's own draws, before a draw; blend is what the
pipeline was made with, for a device where the blend enable is dynamic state too (extended dynamic state 3):
a dynamic state is the command's, not the pipeline's, so it must say the same thing or the draw is not the
one the pipeline describes */
static void dynamic_record(VkCommandBuffer command, const struct dynamic_set *set, const struct vertex_layout *layout,
	uint32_t width, uint32_t height, int blend)
{
	VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { width, height } };
	VkStencilOpState dummy_unused;
	float blend_constants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkBool32 enable = blend ? VK_TRUE : VK_FALSE;
	VkColorBlendEquationEXT equation;
	VkSampleMask sample_mask = 0xffffffffu;
	int index;

	(void)dummy_unused;
	memset(&equation, 0, sizeof(equation));
	equation.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	equation.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	equation.colorBlendOp = VK_BLEND_OP_ADD;
	equation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	equation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	equation.alphaBlendOp = VK_BLEND_OP_ADD;
	for (index = 0; index < set->count; index++)
	{
		switch (set->states[index])
		{
		case VK_DYNAMIC_STATE_VIEWPORT: vkCmdSetViewport(command, 0, 1, &viewport); break;
		case VK_DYNAMIC_STATE_SCISSOR: vkCmdSetScissor(command, 0, 1, &scissor); break;
		case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: vkCmdSetViewportWithCount(command, 1, &viewport); break;
		case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: vkCmdSetScissorWithCount(command, 1, &scissor); break;
		case VK_DYNAMIC_STATE_LINE_WIDTH: vkCmdSetLineWidth(command, 1.0f); break;
		case VK_DYNAMIC_STATE_DEPTH_BIAS: vkCmdSetDepthBias(command, 0.0f, 0.0f, 0.0f); break;
		case VK_DYNAMIC_STATE_BLEND_CONSTANTS: vkCmdSetBlendConstants(command, blend_constants); break;
		case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); break;
		case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); break;
		case VK_DYNAMIC_STATE_STENCIL_REFERENCE: vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0); break;
		case VK_DYNAMIC_STATE_CULL_MODE: vkCmdSetCullMode(command, VK_CULL_MODE_NONE); break;
		case VK_DYNAMIC_STATE_FRONT_FACE: vkCmdSetFrontFace(command, VK_FRONT_FACE_COUNTER_CLOCKWISE); break;
		case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: vkCmdSetPrimitiveTopology(command, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); break;
		case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: vkCmdSetDepthTestEnable(command, VK_TRUE); break;
		case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: vkCmdSetDepthWriteEnable(command, VK_TRUE); break;
		case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: vkCmdSetDepthCompareOp(command, VK_COMPARE_OP_LESS_OR_EQUAL); break;
		case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: vkCmdSetDepthBoundsTestEnable(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: vkCmdSetStencilTestEnable(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_STENCIL_OP:
			vkCmdSetStencilOp(command, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
				VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
			break;
		case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE: break; /* with the buffer, in bind_vertex_buffer */
		case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: vkCmdSetRasterizerDiscardEnable(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: vkCmdSetDepthBiasEnable(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: vkCmdSetPrimitiveRestartEnable(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_LOGIC_OP_EXT: vkCmdSetLogicOpEXT(command, VK_LOGIC_OP_COPY); break;
		case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT: vkCmdSetDepthClampEnableEXT(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_POLYGON_MODE_EXT: vkCmdSetPolygonModeEXT(command, VK_POLYGON_MODE_FILL); break;
		case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT: vkCmdSetRasterizationSamplesEXT(command, VK_SAMPLE_COUNT_1_BIT); break;
		case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT: vkCmdSetSampleMaskEXT(command, VK_SAMPLE_COUNT_1_BIT, &sample_mask); break;
		case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT: vkCmdSetAlphaToCoverageEnableEXT(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT: vkCmdSetAlphaToOneEnableEXT(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT: vkCmdSetLogicOpEnableEXT(command, VK_FALSE); break;
		case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT: vkCmdSetColorBlendEnableEXT(command, 0, 1, &enable); break;
		case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT: vkCmdSetColorBlendEquationEXT(command, 0, 1, &equation); break;
		case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT: vkCmdSetColorWriteMaskEXT(command, 0, 1, &write_mask); break;
		case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: set_vertex_input(command, layout); break;
		default: break;
		}
	}
}

static void bind_vertex_buffer(VkCommandBuffer command, const struct dynamic_set *set, const struct vertex_layout *layout,
	VkBuffer buffer, VkDeviceSize offset)
{
	if (set->stride)
	{
		VkDeviceSize stride = layout->stride;

		vkCmdBindVertexBuffers2(command, 0, 1, &buffer, &offset, NULL, &stride);
	}
	else
	{
		vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
	}
}

struct pipeline_request
{
	VkShaderModule vertex, fragment;
	const struct vertex_layout *layout;
	VkPipelineLayout pipeline_layout;
	VkFormat color, depth;
	int depth_test;
	int blend;
	const struct dynamic_set *dynamic;
	VkPipelineCache cache;
	/* the value of the specialization constant 0 both shaders carry (so that two runs' pipelines differ);
	-1 for no specialization */
	int32_t salt;
};

/* one graphics pipeline; returns it and the time taken (nanoseconds, wall and the thread's CPU) */
static VkPipeline create_pipeline(const struct pipeline_request *request, uint64_t *wall, uint64_t *cpu)
{
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[16];
	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState attachment;
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkSpecializationMapEntry entry = { 0, 0, sizeof(int32_t) };
	VkSpecializationInfo specialization;
	int32_t salt = request->salt;
	VkPipeline pipeline = VK_NULL_HANDLE;
	uint64_t wall_start, cpu_start;
	VkResult result;
	int index;

	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = request->vertex;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = request->fragment;
	if (request->salt >= 0)
	{
		specialization.mapEntryCount = 1;
		specialization.pMapEntries = &entry;
		specialization.dataSize = sizeof(salt);
		specialization.pData = &salt;
		stages[0].pSpecializationInfo = &specialization;
		stages[1].pSpecializationInfo = &specialization;
	}
	binding.binding = 0;
	binding.stride = request->layout->stride;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	for (index = 0; index < request->layout->count; index++)
	{
		attributes[index].location = request->layout->attributes[index].location;
		attributes[index].binding = 0;
		attributes[index].format = request->layout->attributes[index].format;
		attributes[index].offset = request->layout->attributes[index].offset;
	}
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = (uint32_t)request->layout->count;
	vertex_input.pVertexAttributeDescriptions = attributes;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	/* the counted variants take their count from the command: none here */
	viewport.viewportCount = request->dynamic->counted_viewport ? 0 : 1;
	viewport.scissorCount = request->dynamic->counted_viewport ? 0 : 1;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	depth.depthTestEnable = request->depth_test ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = request->depth_test ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	memset(&attachment, 0, sizeof(attachment));
	attachment.blendEnable = request->blend ? VK_TRUE : VK_FALSE;
	attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	attachment.colorBlendOp = VK_BLEND_OP_ADD;
	attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;
	dynamic.dynamicStateCount = (uint32_t)request->dynamic->count;
	dynamic.pDynamicStates = request->dynamic->states;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = request->depth ? &depth : NULL;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = request->pipeline_layout;
	if (P.dynamic_rendering)
	{
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachmentFormats = &request->color;
		rendering.depthAttachmentFormat = request->depth;
		rendering.stencilAttachmentFormat = format_has_stencil(request->depth) ? request->depth : VK_FORMAT_UNDEFINED;
		info.pNext = &rendering;
	}
	else
	{
		info.renderPass = pipeline_render_pass(request->color, request->depth);
	}
	wall_start = now_ns();
	cpu_start = cpu_ns();
	result = vkCreateGraphicsPipelines(P.device, request->cache, 1, &info, NULL, &pipeline);
	if (wall)
		*wall = now_ns() - wall_start;
	if (cpu)
		*cpu = cpu_ns() - cpu_start;
	if (result != VK_SUCCESS)
	{
		rep("error: vkCreateGraphicsPipelines returned %d", (int)result);
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

static VkPipelineLayout make_pipeline_layout(VkDescriptorSetLayout set_layout)
{
	VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout = VK_NULL_HANDLE;

	info.setLayoutCount = set_layout ? 1 : 0;
	info.pSetLayouts = &set_layout;
	if (!CHECK_RESULT(vkCreatePipelineLayout(P.device, &info, NULL, &layout)))
		return VK_NULL_HANDLE;
	return layout;
}

/* the colour format of the probe's targets, and the depth format its pipelines use */
#define PROBE_COLOR_FORMAT VK_FORMAT_R8G8B8A8_UNORM

static VkFormat pick_depth_format(void)
{
	static const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
	int index;

	for (index = 0; index < 2; index++)
	{
		VkFormatProperties properties;

		vkGetPhysicalDeviceFormatProperties(P.physical, candidates[index], &properties);
		if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return candidates[index];
	}
	return VK_FORMAT_D32_SFLOAT_S8_UINT; /* the format every device has to have one of; reported if it fails */
}

/* ---------- step 3: the game's memory, seen by the GPU (memory) */

/* the probe's own pass-through shaders and a pipeline for them, made once and shared by steps 3 and 5 */
struct simple
{
	struct spirv vs_code, fs_code;
	VkShaderModule vs, fs;
	VkPipelineLayout layout;
	struct dynamic_set dynamic;
	VkPipeline pipeline;
	int ready, failed;
};

static struct simple S;

static int simple_init(void)
{
	struct pipeline_request request;

	if (S.ready)
		return 1;
	if (S.failed)
		return 0;
	S.failed = 1;
	if (!compile_asset("pass.vert", &S.vs_code) || !compile_asset("pass.frag", &S.fs_code))
		return 0;
	S.vs = make_shader_module(&S.vs_code);
	S.fs = make_shader_module(&S.fs_code);
	S.layout = make_pipeline_layout(VK_NULL_HANDLE);
	if (!S.vs || !S.fs || !S.layout)
		return 0;
	/* the plain state: only the viewport and the scissor dynamic */
	dynamic_choose(&S.dynamic, 0);
	memset(&request, 0, sizeof(request));
	request.vertex = S.vs;
	request.fragment = S.fs;
	request.layout = &layout_color;
	request.pipeline_layout = S.layout;
	request.color = PROBE_COLOR_FORMAT;
	request.dynamic = &S.dynamic;
	request.salt = -1;
	S.pipeline = create_pipeline(&request, NULL, NULL);
	if (!S.pipeline)
		return 0;
	S.failed = 0;
	S.ready = 1;
	return 1;
}

static void simple_free(void)
{
	if (S.pipeline)
		vkDestroyPipeline(P.device, S.pipeline, NULL);
	if (S.layout)
		vkDestroyPipelineLayout(P.device, S.layout, NULL);
	if (S.fs)
		vkDestroyShaderModule(P.device, S.fs, NULL);
	if (S.vs)
		vkDestroyShaderModule(P.device, S.vs, NULL);
	free(S.vs_code.words);
	free(S.fs_code.words);
	memset(&S, 0, sizeof(S));
}

#define TARGET_SIZE 16

/* a draw of three vertices (or three indices) covering the 16x16 target, read back; the target's
pixels must all be `expect`. Returns 1 if they are, and says what it saw if not. */
struct draw_check
{
	struct target target;
	struct buffer readback;
	int ready;
};

static struct draw_check D;

static int draw_check_init(void)
{
	if (D.ready)
		return 1;
	if (!simple_init())
		return 0;
	if (!target_make(&D.target, TARGET_SIZE, TARGET_SIZE, PROBE_COLOR_FORMAT, VK_FORMAT_UNDEFINED))
		return 0;
	if (!make_buffer(&D.readback, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
		return 0;
	D.ready = 1;
	return 1;
}

static void draw_check_free(void)
{
	free_buffer(&D.readback);
	target_free(&D.target);
	memset(&D, 0, sizeof(D));
}

static void host_barrier(VkCommandBuffer command)
{
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* the pixels are in D.readback; say whether all equal expect (0xAABBGGRR as bytes r, g, b, a) */
static int draw_check_compare(uint32_t expect, uint32_t *first_seen)
{
	const uint32_t *pixels = D.readback.map;
	int index, wrong = 0;

	*first_seen = pixels[0];
	for (index = 0; index < TARGET_SIZE * TARGET_SIZE; index++)
		if (pixels[index] != expect)
			wrong++;
	return wrong == 0;
}

/* vertex buffer vb at offset (6 vertices of layout_color, the first three are the picture), with
indices from ib (NULL: a plain draw of three vertices from the buffer's start) */
static int draw_and_check(VkBuffer vb, VkBuffer ib, uint32_t expect, uint32_t *seen)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	VkCommandBuffer command = command_begin();
	VkBufferImageCopy copy;

	pass_begin(command, &D.target, black);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipeline);
	dynamic_record(command, &S.dynamic, &layout_color, TARGET_SIZE, TARGET_SIZE, 0);
	{
		VkDeviceSize offset = 0;

		vkCmdBindVertexBuffers(command, 0, 1, &vb, &offset);
	}
	if (ib)
	{
		vkCmdBindIndexBuffer(command, ib, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(command, 3, 1, 0, 0, 0);
	}
	else
	{
		vkCmdDraw(command, 3, 1, 0, 0);
	}
	pass_end(command);
	image_barrier(command, D.target.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = TARGET_SIZE;
	copy.imageExtent.height = TARGET_SIZE;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(command, D.target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, D.readback.buffer, 1, &copy);
	host_barrier(command);
	if (!command_submit_wait(command))
		return 0;
	return draw_check_compare(expect, seen);
}

/* the picture of the vertices: a triangle larger than the target, one colour for all its vertices */
struct color_vertex
{
	float x, y;
	uint8_t color[4];
};

static void fill_triangle(struct color_vertex *vertices, uint8_t r, uint8_t g, uint8_t b)
{
	static const float corners[3][2] = { { -1.0f, -1.0f }, { 3.0f, -1.0f }, { -1.0f, 3.0f } };
	int index;

	for (index = 0; index < 3; index++)
	{
		vertices[index].x = corners[index][0];
		vertices[index].y = corners[index][1];
		vertices[index].color[0] = r;
		vertices[index].color[1] = g;
		vertices[index].color[2] = b;
		vertices[index].color[3] = 255;
	}
}

static uint32_t color_word(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u;
}

/* ---- importing guest-style memory */

struct imported
{
	void *cpu;
	VkDeviceSize size;
	VkDeviceMemory memory;
	VkBuffer buffer;
	uint32_t type;
	VkMemoryPropertyFlags flags;
	VkResult result;
};

static const char *import_stage;

/* Imports [cpu, cpu + size) as a buffer. The result code of the stage that failed is returned
and logged by the caller; a range that is not aligned is the negative test's, and the layer's
message about it is marked as expected. */
static VkResult import_range(void *cpu, VkDeviceSize size, VkBufferUsageFlags usage, struct imported *out)
{
	VkExternalMemoryBufferCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryHostPointerPropertiesEXT pointer_properties = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	VkImportMemoryHostPointerInfoEXT import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;
	uint32_t bits, type;
	VkResult result;

	memset(out, 0, sizeof(*out));
	out->cpu = cpu;
	out->size = size;
	import_stage = "vkGetMemoryHostPointerPropertiesEXT";
	result = vkGetMemoryHostPointerPropertiesEXT(P.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, cpu,
		&pointer_properties);
	if (result != VK_SUCCESS)
		return result;
	rep("memory.import.pointer_memory_type_bits: 0x%x", pointer_properties.memoryTypeBits);
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	info.pNext = &external;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	import_stage = "vkCreateBuffer";
	result = vkCreateBuffer(P.device, &info, NULL, &out->buffer);
	if (result != VK_SUCCESS)
		return result;
	vkGetBufferMemoryRequirements(P.device, out->buffer, &requirements);
	bits = pointer_properties.memoryTypeBits & requirements.memoryTypeBits;
	if (!bits)
	{
		rep("memory.import.no_common_memory_type: pointer 0x%x, buffer 0x%x", pointer_properties.memoryTypeBits,
			requirements.memoryTypeBits);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	/* a coherent type if there is one: the CPU's writes then need no flush */
	type = find_memory_type(bits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
	if (type == UINT32_MAX)
		type = find_memory_type(bits, 0, 0);
	if (size < requirements.size)
	{
		rep("memory.import.range_smaller_than_requirements: %llu < %llu", (unsigned long long)size,
			(unsigned long long)requirements.size);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	import.pHostPointer = cpu;
	allocate.pNext = &import;
	allocate.allocationSize = size;
	allocate.memoryTypeIndex = type;
	import_stage = "vkAllocateMemory";
	result = vkAllocateMemory(P.device, &allocate, NULL, &out->memory);
	if (result != VK_SUCCESS)
		return result;
	import_stage = "vkBindBufferMemory";
	result = vkBindBufferMemory(P.device, out->buffer, out->memory, 0);
	if (result != VK_SUCCESS)
		return result;
	out->type = type;
	out->flags = P.memory.memoryTypes[type].propertyFlags;
	import_stage = "ok";
	return VK_SUCCESS;
}

static void import_free(struct imported *imported)
{
	if (imported->buffer)
		vkDestroyBuffer(P.device, imported->buffer, NULL);
	if (imported->memory)
		vkFreeMemory(P.device, imported->memory, NULL);
	memset(imported, 0, sizeof(*imported));
}

static void *low_range(size_t size, size_t alignment)
{
	/* host_low_map returns pages; more of them and a rounded-up start give any alignment */
	char *memory = host_low_map(size + alignment, PROT_READ | PROT_WRITE);
	uintptr_t start;

	if (!memory)
		return NULL;
	start = ((uintptr_t)memory + alignment - 1) & ~((uintptr_t)alignment - 1);
	memset((void *)start, 0, size); /* committed, as the game's memory is once it is used */
	return (void *)start;
}

static VkBufferUsageFlags import_usage(void)
{
	return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
}

/* what the driver says importing a host allocation for this buffer usage can do */
static int external_buffer_importable(VkBufferUsageFlags usage, VkExternalMemoryHandleTypeFlagBits type, const char *name)
{
	VkPhysicalDeviceExternalBufferInfo info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
	VkExternalBufferProperties properties = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };

	info.usage = usage;
	info.handleType = type;
	vkGetPhysicalDeviceExternalBufferProperties(P.physical, &info, &properties);
	rep("memory.%s.buffer_properties: features 0x%x (exportable %s, importable %s, dedicated only %s), compatible handle types 0x%x",
		name, properties.externalMemoryProperties.externalMemoryFeatures,
		yesno(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
		yesno(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
		yesno(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT),
		properties.externalMemoryProperties.compatibleHandleTypes);
	return (properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

/* the pattern of the copy tests: word n of pass `seed` */
static uint32_t pattern_word(uint32_t seed, size_t index)
{
	uint32_t value = (uint32_t)index * 2654435761u + seed * 40503u;

	value ^= value >> 15;
	return value * 2246822519u;
}

/* copies the first and the last 64 KB of the buffer to a readback buffer and compares them to the
pattern; returns 1 if both are right, and says what was seen otherwise */
static int copy_matches(struct imported *imported, uint32_t seed, struct buffer *readback)
{
	VkBufferCopy regions[2];
	VkCommandBuffer command = command_begin();
	const uint32_t *words = readback->map;
	size_t index, wrong = 0, first_wrong = (size_t)-1;
	VkDeviceSize chunk = 65536;
	int region;

	regions[0].srcOffset = 0;
	regions[0].dstOffset = 0;
	regions[0].size = chunk;
	regions[1].srcOffset = imported->size - chunk;
	regions[1].dstOffset = chunk;
	regions[1].size = chunk;
	vkCmdCopyBuffer(command, imported->buffer, readback->buffer, 2, regions);
	host_barrier(command);
	if (!command_submit_wait(command))
		return 0;
	for (region = 0; region < 2; region++)
	{
		size_t base = region ? (size_t)((imported->size - chunk) / 4) : 0;

		for (index = 0; index < chunk / 4; index++)
		{
			if (words[region * (chunk / 4) + index] != pattern_word(seed, base + index))
			{
				if (first_wrong == (size_t)-1)
					first_wrong = (size_t)region * (size_t)(chunk / 4) + index;
				wrong++;
			}
		}
	}
	if (wrong)
		rep("memory.import.copy_mismatch: %zu of %zu words differ, the first at word %zu of the readback (seed %u)",
			wrong, (size_t)(chunk / 2), first_wrong, seed);
	return wrong == 0;
}

static void fill_pattern(void *memory, size_t size, uint32_t seed)
{
	uint32_t *words = memory;
	size_t index;

	for (index = 0; index < size / 4; index++)
		words[index] = pattern_word(seed, index);
}

static void memory_import_cases(void)
{
	static const size_t sizes[] = { 64 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024, 64 * 1024 * 1024 };
	size_t alignment = (size_t)P.host_properties.minImportedHostPointerAlignment;
	struct buffer readback;
	int case_index;

	if (!P.host_import || !vkGetMemoryHostPointerPropertiesEXT)
	{
		rep("memory.import: not possible: VK_EXT_external_memory_host is not offered by this device");
		return;
	}
	if (!alignment || (alignment & (alignment - 1)))
	{
		rep("memory.import: the device reports minImportedHostPointerAlignment %zu, not a power of two", alignment);
		return;
	}
	if (!external_buffer_importable(import_usage(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "import"))
	{
		rep("memory.import: the driver says a buffer of this usage cannot import a host allocation");
		return;
	}
	if (!make_buffer(&readback, 131072, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
		return;
	for (case_index = 0; case_index < 4; case_index++)
	{
		size_t size = (sizes[case_index] + alignment - 1) & ~(alignment - 1);
		void *cpu = low_range(size, alignment);
		struct imported imported;
		VkResult result;
		char key[48];

		snprintf(key, sizeof(key), "memory.import.%zuK", size / 1024);
		if (!cpu)
		{
			rep("%s: no memory below 4 GB for the test", key);
			continue;
		}
		rep("%s.range: %p (guest-kind memory below 4 GB, alignment %zu)", key, cpu, alignment);
		result = import_range(cpu, size, import_usage(), &imported);
		if (result != VK_SUCCESS)
		{
			rep("%s.imported: no (%s returned %d)", key, import_stage, (int)result);
			import_free(&imported);
			host_low_unmap(cpu, size);
			continue;
		}
		rep("%s.imported: yes (result 0)", key);
		rep("%s.memory_type: %u, %s", key, imported.type, memory_flag_text(imported.flags));

		/* reading by copy, then the rewritten pattern */
		fill_pattern(cpu, size, 1);
		rep("%s.copy_correct: %s", key, yesno(copy_matches(&imported, 1, &readback)));
		fill_pattern(cpu, size, 2);
		rep("%s.rewrite_seen: %s%s", key, yesno(copy_matches(&imported, 2, &readback)),
			imported.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "" : " (the memory type is not host-coherent, and nothing was flushed)");

		/* reading by vertex fetch and by index fetch */
		if (case_index == 0 && draw_check_init())
		{
			struct color_vertex *vertices = cpu;
			uint32_t *indices = (uint32_t *)((char *)cpu + 4096);
			uint32_t seen = 0;
			struct imported index_buffer;
			int vertex_ok, rewrite_ok, index_ok = 0, index_rewrite_ok = 0, index_imported = 0;

			/* six vertices: the first triangle red, the second green */
			fill_triangle(vertices, 255, 0, 0);
			fill_triangle(vertices + 3, 0, 255, 0);
			vertex_ok = draw_and_check(imported.buffer, NULL, color_word(255, 0, 0), &seen);
			rep("%s.vertex_fetch_correct: %s (first pixel 0x%08x)", key, yesno(vertex_ok), seen);
			fill_triangle(vertices, 0, 0, 255);
			rewrite_ok = draw_and_check(imported.buffer, NULL, color_word(0, 0, 255), &seen);
			rep("%s.vertex_fetch_rewrite_seen: %s (first pixel 0x%08x)", key, yesno(rewrite_ok), seen);

			/* an index buffer in imported memory of its own: {0,1,2}, then rewritten to {3,4,5} */
			{
				void *index_cpu = low_range(alignment, alignment);
				VkResult index_result;

				if (index_cpu)
				{
					uint32_t *words = index_cpu;

					words[0] = 0;
					words[1] = 1;
					words[2] = 2;
					fill_triangle(vertices, 255, 0, 0);
					index_result = import_range(index_cpu, alignment, import_usage(), &index_buffer);
					if (index_result == VK_SUCCESS)
					{
						index_imported = 1;
						index_ok = draw_and_check(imported.buffer, index_buffer.buffer, color_word(255, 0, 0), &seen);
						rep("%s.index_fetch_correct: %s (first pixel 0x%08x)", key, yesno(index_ok), seen);
						words[0] = 3;
						words[1] = 4;
						words[2] = 5;
						index_rewrite_ok = draw_and_check(imported.buffer, index_buffer.buffer, color_word(0, 255, 0), &seen);
						rep("%s.index_fetch_rewrite_seen: %s (first pixel 0x%08x)", key, yesno(index_rewrite_ok), seen);
						import_free(&index_buffer);
					}
					else
					{
						rep("%s.index_fetch: the index range was not imported (%s returned %d)", key, import_stage, (int)index_result);
						import_free(&index_buffer);
					}
					host_low_unmap(index_cpu, alignment);
				}
			}
			(void)indices;
			(void)index_imported;
		}
		import_free(&imported);
		host_low_unmap(cpu, size);
	}

	/* the same pages imported twice, and a range that is not aligned (which Vulkan forbids: the
	refusal should be a result code). Each can kill a driver, so they are guarded. */
	{
		size_t size = alignment;
		void *cpu = low_range(size * 2, alignment);

		if (cpu)
		{
			struct imported first, second;
			VkResult a, b;

			/* importing the same pages twice is not forbidden, and a driver may not expect it */
			if (guard_begin("memory.same_pages"))
			{
				a = import_range(cpu, size, import_usage(), &first);
				b = import_range(cpu, size, import_usage(), &second);
				rep("memory.import.same_pages_twice: first %d, second %d (%s)", (int)a, (int)b,
					a == VK_SUCCESS && b == VK_SUCCESS ? "both allowed" : "refused");
				import_free(&second);
				import_free(&first);
				guard_end();
			}
			if (guard_begin("memory.negative"))
			{
				struct imported bad;
				VkResult result;

				expecting_errors = 1;
				result = import_range((char *)cpu + 4, size, import_usage(), &bad);
				rep("memory.import.unaligned_pointer: %s (%s returned %d; Vulkan forbids it, so a refusal is the expected answer)",
					result == VK_SUCCESS ? "accepted" : "refused", import_stage, (int)result);
				import_free(&bad);
				result = import_range(cpu, size / 2 ? size / 2 + 1 : 1, import_usage(), &bad);
				rep("memory.import.unaligned_size: %s (%s returned %d)", result == VK_SUCCESS ? "accepted" : "refused",
					import_stage, (int)result);
				import_free(&bad);
				expecting_errors = 0;
				guard_end();
			}
			host_low_unmap(cpu, size * 2);
		}
	}
	free_buffer(&readback);
}

/* a texture read from imported memory: BC1 if the device can sample and copy it, else RGBA8 */
static void memory_import_texture(void)
{
	size_t alignment = (size_t)P.host_properties.minImportedHostPointerAlignment;
	VkFormatProperties properties;
	VkFormat format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	uint32_t width = 256, height = 256, bytes;
	struct imported imported;
	struct image image;
	struct buffer readback;
	void *cpu;
	VkResult result;
	size_t size;
	int bc = 1;

	if (!P.host_import || !vkGetMemoryHostPointerPropertiesEXT)
		return;
	vkGetPhysicalDeviceFormatProperties(P.physical, format, &properties);
	if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ||
		!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
	{
		format = VK_FORMAT_R8G8B8A8_UNORM;
		bc = 0;
	}
	bytes = bc ? (width / 4) * (height / 4) * 8 : width * height * 4;
	size = ((size_t)bytes + alignment - 1) & ~(alignment - 1);
	cpu = low_range(size, alignment);
	if (!cpu)
		return;
	fill_pattern(cpu, bytes, 7);
	result = import_range(cpu, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &imported);
	if (result != VK_SUCCESS)
	{
		rep("memory.texture: not imported (%s returned %d)", import_stage, (int)result);
		import_free(&imported);
		host_low_unmap(cpu, size);
		return;
	}
	if (make_image(&image, width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
		make_buffer(&readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
	{
		VkCommandBuffer command = command_begin();
		VkBufferImageCopy copy;

		memset(&copy, 0, sizeof(copy));
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = width;
		copy.imageExtent.height = height;
		copy.imageExtent.depth = 1;
		image_barrier(command, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(command, imported.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		image_barrier(command, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkCmdCopyImageToBuffer(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
		host_barrier(command);
		if (command_submit_wait(command))
		{
			/* the image is tiled, so the copy back is the same bytes only if the driver keeps every texel */
			rep("memory.texture: %s %ux%u from imported memory, read back %s", bc ? "BC1" : "RGBA8", width, height,
				memcmp(readback.map, cpu, bytes) == 0 ? "identical" : "different");
		}
		free_buffer(&readback);
	}
	free_image(&image);
	import_free(&imported);
	host_low_unmap(cpu, size);
}


/* ---- the cost of copying instead: an upload ring, against the memory in place */

#define RING_BYTES (4u * 1024u * 1024u)
#define RING_FRAMES 300
#define GPU_SAMPLES 60

static uint32_t random_state = 12345;

static uint32_t next_random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* many small triangles scattered over the target: the draw is the vertex fetch's, not the pixels' */
static uint32_t fill_scatter(void *memory, size_t bytes)
{
	struct color_vertex *vertices = memory;
	size_t count = bytes / sizeof(struct color_vertex), index;

	count -= count % 3;
	for (index = 0; index < count; index += 3)
	{
		float x = (float)(next_random() % 2000) / 1000.0f - 1.0f;
		float y = (float)(next_random() % 2000) / 1000.0f - 1.0f;
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			vertices[index + corner].x = x + (corner == 1 ? 0.02f : 0.0f);
			vertices[index + corner].y = y + (corner == 2 ? 0.02f : 0.0f);
			vertices[index + corner].color[0] = 200;
			vertices[index + corner].color[1] = 100;
			vertices[index + corner].color[2] = 50;
			vertices[index + corner].color[3] = 255;
		}
	}
	return (uint32_t)count;
}

/* GPU time of one render pass (clear, one draw) from timestamps around it, microseconds */
struct gpu_timer
{
	VkQueryPool pool;
	uint64_t mask;
};

static int gpu_timer_make(struct gpu_timer *timer)
{
	VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };

	memset(timer, 0, sizeof(*timer));
	if (!P.timestamps)
		return 0;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = 2;
	timer->mask = P.family_properties.timestampValidBits >= 64 ? ~0ull : (1ull << P.family_properties.timestampValidBits) - 1;
	return CHECK_RESULT(vkCreateQueryPool(P.device, &info, NULL, &timer->pool));
}

static double gpu_timer_read(struct gpu_timer *timer)
{
	uint64_t stamps[2];

	if (vkGetQueryPoolResults(P.device, timer->pool, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
		return -1.0;
	return (double)((stamps[1] - stamps[0]) & timer->mask) * P.properties.limits.timestampPeriod / 1000.0;
}

struct ring_target
{
	struct target target;
	struct dynamic_set dynamic;
	int ready;
};

/* one pass: clear, bind vb, draw vertex_count vertices; `copy_from` first copies that buffer into vb (the
device-local case), inside the timed span, before the pass */
static double timed_draw(struct gpu_timer *timer, struct target *target, VkBuffer vb, uint32_t vertex_count, VkBuffer copy_from, VkDeviceSize copy_size)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	VkCommandBuffer command = command_begin();
	VkDeviceSize offset = 0;
	double result;

	if (timer->pool)
	{
		vkCmdResetQueryPool(command, timer->pool, 0, 2);
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timer->pool, 0);
	}
	if (copy_from)
	{
		VkBufferCopy region = { 0, 0, copy_size };

		vkCmdCopyBuffer(command, copy_from, vb, 1, &region);
		memory_barrier(command);
	}
	pass_begin(command, target, black);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipeline);
	dynamic_record(command, &S.dynamic, &layout_color, target->color.width, target->color.height, 0);
	vkCmdBindVertexBuffers(command, 0, 1, &vb, &offset);
	vkCmdDraw(command, vertex_count, 1, 0, 0);
	pass_end(command);
	if (timer->pool)
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timer->pool, 1);
	if (!command_submit_wait(command))
		return -1.0;
	result = timer->pool ? gpu_timer_read(timer) : -1.0;
	return result;
}

/* the median of GPU_SAMPLES passes after a few warm-ups, microseconds; negative if the device cannot time */
static double gpu_median(struct gpu_timer *timer, struct target *target, VkBuffer vb, uint32_t vertex_count, VkBuffer copy_from,
	VkDeviceSize copy_size, double *minimum)
{
	double samples[GPU_SAMPLES];
	int index;

	for (index = 0; index < 5; index++)
		timed_draw(timer, target, vb, vertex_count, copy_from, copy_size);
	for (index = 0; index < GPU_SAMPLES; index++)
		samples[index] = timed_draw(timer, target, vb, vertex_count, copy_from, copy_size);
	qsort(samples, GPU_SAMPLES, sizeof(double), compare_doubles);
	*minimum = samples[0];
	return samples[GPU_SAMPLES / 2];
}

static void ring_report_cpu(const char *key, struct buffer *ring, const void *source, int flush)
{
	double wall[RING_FRAMES], cpu[RING_FRAMES];
	int frame;

	for (frame = 0; frame < RING_FRAMES; frame++)
	{
		uint64_t wall_start = now_ns(), cpu_start = cpu_ns();

		memcpy(ring->map, source, RING_BYTES);
		if (flush)
		{
			VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };

			range.memory = ring->memory;
			range.size = VK_WHOLE_SIZE;
			vkFlushMappedMemoryRanges(P.device, 1, &range);
		}
		cpu[frame] = (double)(cpu_ns() - cpu_start) / 1e6;
		wall[frame] = (double)(now_ns() - wall_start) / 1e6;
	}
	{
		char text[96];

		snprintf(text, sizeof(text), "%s.cpu_copy_wall", key);
		rep_spread(text, wall, RING_FRAMES);
		snprintf(text, sizeof(text), "%s.cpu_copy_thread_cpu", key);
		rep_spread(text, cpu, RING_FRAMES);
	}
}

static void memory_ring_cost(void)
{
	struct buffer coherent, cached, local;
	struct gpu_timer timer;
	struct ring_target ring;
	void *source;
	uint32_t vertices;
	double minimum, baseline, median;
	int have_coherent, have_cached = 0, have_local;
	uint32_t type_cached;

	memset(&coherent, 0, sizeof(coherent));
	memset(&cached, 0, sizeof(cached));
	memset(&local, 0, sizeof(local));
	rep("memory.ring.bytes_per_frame: %u (the figure until step 6 measures the game's)", RING_BYTES);
	if (!simple_init())
		return;
	source = malloc(RING_BYTES);
	vertices = fill_scatter(source, RING_BYTES);
	rep("memory.ring.vertices: %u (%u triangles)", vertices, vertices / 3);

	have_coherent = make_buffer(&coherent, RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1);
	if (have_coherent)
		rep("memory.ring.coherent.type: %u, %s", coherent.type, memory_flag_text(P.memory.memoryTypes[coherent.type].propertyFlags));
	/* host-cached without coherence: needs explicit flushes */
	type_cached = UINT32_MAX;
	{
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		VkBuffer probe_buffer;
		VkMemoryRequirements requirements;

		info.size = RING_BYTES;
		info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (CHECK_RESULT(vkCreateBuffer(P.device, &info, NULL, &probe_buffer)))
		{
			vkGetBufferMemoryRequirements(P.device, probe_buffer, &requirements);
			type_cached = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			vkDestroyBuffer(P.device, probe_buffer, NULL);
		}
	}
	if (type_cached != UINT32_MAX)
	{
		have_cached = make_buffer(&cached, RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
		if (have_cached)
			rep("memory.ring.cached.type: %u, %s", cached.type, memory_flag_text(P.memory.memoryTypes[cached.type].propertyFlags));
	}
	else
	{
		rep("memory.ring.cached: no host-cached type without coherence exists for a vertex buffer on this device");
	}
	have_local = make_buffer(&local, RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
	if (have_local)
		rep("memory.ring.device_local.type: %u, %s", local.type, memory_flag_text(P.memory.memoryTypes[local.type].propertyFlags));

	/* what the CPU pays per frame for the copy */
	if (have_coherent)
		ring_report_cpu("memory.ring.coherent", &coherent, source, 0);
	if (have_cached)
		ring_report_cpu("memory.ring.cached", &cached, source, 1);

	/* what the GPU pays to draw from each */
	gpu_timer_make(&timer);
	memset(&ring, 0, sizeof(ring));
	if (target_make(&ring.target, 256, 256, PROBE_COLOR_FORMAT, VK_FORMAT_UNDEFINED))
	{
		if (have_coherent)
			memcpy(coherent.map, source, RING_BYTES);
		if (have_cached)
		{
			VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };

			memcpy(cached.map, source, RING_BYTES);
			range.memory = cached.memory;
			range.size = VK_WHOLE_SIZE;
			vkFlushMappedMemoryRanges(P.device, 1, &range);
		}
		if (!timer.pool)
		{
			rep("memory.ring.gpu: the queue has no timestamps (timestampValidBits is 0)");
		}
		else if (have_coherent)
		{
			/* the cost of the pass with almost nothing drawn, to take away */
			baseline = gpu_median(&timer, &ring.target, coherent.buffer, 3, NULL, 0, &minimum);
			rep("memory.ring.gpu.baseline_pass: median %.1f us, min %.1f us (a pass that draws one triangle)", baseline, minimum);
			median = gpu_median(&timer, &ring.target, coherent.buffer, vertices, NULL, 0, &minimum);
			rep("memory.ring.gpu.draw_from_host_coherent: median %.1f us, min %.1f us (%.1f us over the baseline)", median, minimum, median - baseline);
			if (have_cached)
			{
				median = gpu_median(&timer, &ring.target, cached.buffer, vertices, NULL, 0, &minimum);
				rep("memory.ring.gpu.draw_from_host_cached: median %.1f us, min %.1f us (%.1f us over the baseline)", median, minimum, median - baseline);
			}
			if (have_local)
			{
				median = gpu_median(&timer, &ring.target, local.buffer, vertices, coherent.buffer, RING_BYTES, &minimum);
				rep("memory.ring.gpu.copy_to_device_local_then_draw: median %.1f us, min %.1f us (%.1f us over the baseline)", median, minimum, median - baseline);
				/* the copy is in the buffer now: the draw alone, from memory only the GPU can see */
				median = gpu_median(&timer, &ring.target, local.buffer, vertices, NULL, 0, &minimum);
				rep("memory.ring.gpu.draw_from_device_local: median %.1f us, min %.1f us (%.1f us over the baseline)", median, minimum, median - baseline);
			}
		}
		target_free(&ring.target);
	}
	if (timer.pool)
		vkDestroyQueryPool(P.device, timer.pool, NULL);

	free_buffer(&local);
	free_buffer(&cached);
	free_buffer(&coherent);
	free(source);
}

/* The same draw, from imported memory (where the device imports), for the timing against the
ring's: a separate function so that a device without the extension never reaches it. */
static void memory_import_gpu_cost(void)
{
	size_t alignment = (size_t)P.host_properties.minImportedHostPointerAlignment;
	size_t size;
	void *cpu;
	struct imported imported;
	struct gpu_timer timer;
	struct target target;
	uint32_t vertices;
	double baseline, median, minimum;

	if (!P.host_import || !vkGetMemoryHostPointerPropertiesEXT || !alignment || !P.timestamps || !simple_init())
		return;
	size = ((size_t)RING_BYTES + alignment - 1) & ~(alignment - 1);
	cpu = low_range(size, alignment);
	if (!cpu)
		return;
	vertices = fill_scatter(cpu, RING_BYTES);
	if (import_range(cpu, size, import_usage(), &imported) == VK_SUCCESS && gpu_timer_make(&timer))
	{
		if (target_make(&target, 256, 256, PROBE_COLOR_FORMAT, VK_FORMAT_UNDEFINED))
		{
			baseline = gpu_median(&timer, &target, imported.buffer, 3, NULL, 0, &minimum);
			median = gpu_median(&timer, &target, imported.buffer, vertices, NULL, 0, &minimum);
			rep("memory.ring.gpu.draw_from_imported: median %.1f us, min %.1f us (%.1f us over its baseline of %.1f us)", median,
				minimum, median - baseline, baseline);
			target_free(&target);
		}
		vkDestroyQueryPool(P.device, timer.pool, NULL);
	}
	import_free(&imported);
	host_low_unmap(cpu, size);
}

/* ---- beyond the plan: where there is no host-pointer import, can the driver's memory be mapped by the
guest instead? An allocation exported as a file descriptor, then mapped with mmap, below 4 GB too. */

/* memcpy's speed in MB/s over a megabyte, best of a few tries */
static double copy_speed(void *to, const void *from, size_t size)
{
	double best = 0.0;
	int attempt;

	for (attempt = 0; attempt < 5; attempt++)
	{
		uint64_t start = now_ns();
		double speed;

		memcpy(to, from, size);
		speed = (double)size / 1048576.0 / ((double)(now_ns() - start) / 1e9);
		if (speed > best)
			best = speed;
	}
	return best;
}

/* one exported allocation of the memory type with these flags (and none of `avoid`) */
static void memory_export_try(const char *key, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoid)
{
	VkExternalMemoryBufferCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkExportMemoryAllocateInfo export_info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
	VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryGetFdInfoKHR get_fd = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
	VkPhysicalDeviceExternalBufferInfo query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
	VkExternalBufferProperties properties = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
	VkMemoryRequirements requirements;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	struct buffer readback;
	struct imported view;
	void *anywhere = MAP_FAILED, *low = NULL, *driver_map = NULL;
	const VkDeviceSize size = 8u << 20;
	const VkExternalMemoryHandleTypeFlagBits handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	int fd = -1, dedicated_only;
	uint32_t type;
	VkResult result;

	query.usage = usage;
	query.handleType = handle;
	vkGetPhysicalDeviceExternalBufferProperties(P.physical, &query, &properties);
	rep("%s.buffer_properties: features 0x%x (exportable %s, dedicated only %s), compatible handle types 0x%x", key,
		properties.externalMemoryProperties.externalMemoryFeatures,
		yesno(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
		yesno(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT),
		properties.externalMemoryProperties.compatibleHandleTypes);
	if (!(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
	{
		rep("%s: the driver says a buffer of this usage cannot be exported as an opaque fd", key);
		return;
	}
	dedicated_only = (properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
	external.handleTypes = handle;
	info.pNext = &external;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!CHECK_RESULT(vkCreateBuffer(P.device, &info, NULL, &buffer)))
		return;
	vkGetBufferMemoryRequirements(P.device, buffer, &requirements);
	type = find_memory_type(requirements.memoryTypeBits, required, avoid);
	if (type == UINT32_MAX)
	{
		rep("%s: no such memory type for the exported buffer (it may use 0x%x)", key, requirements.memoryTypeBits);
		vkDestroyBuffer(P.device, buffer, NULL);
		return;
	}
	export_info.handleTypes = handle;
	allocate.pNext = &export_info;
	if (dedicated_only)
	{
		dedicated.buffer = buffer;
		export_info.pNext = &dedicated;
	}
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(P.device, &allocate, NULL, &memory);
	if (result != VK_SUCCESS || !CHECK_RESULT(vkBindBufferMemory(P.device, buffer, memory, 0)))
	{
		rep("%s.allocated: no (%d)", key, (int)result);
		goto done;
	}
	get_fd.memory = memory;
	get_fd.handleType = handle;
	result = vkGetMemoryFdKHR(P.device, &get_fd, &fd);
	rep("%s.get_fd: result %d, fd %d (memory type %u, %s%s)", key, (int)result, fd, type,
		memory_flag_text(P.memory.memoryTypes[type].propertyFlags), dedicated_only ? ", dedicated allocation" : "");
	if (result != VK_SUCCESS)
		goto done;
	if (!CHECK_RESULT(vkMapMemory(P.device, memory, 0, VK_WHOLE_SIZE, 0, &driver_map)))
		driver_map = NULL;

	anywhere = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	rep("%s.mmap_anywhere: %s (errno %d)", key, anywhere == MAP_FAILED ? "failed" : "mapped", anywhere == MAP_FAILED ? errno : 0);
	/* below 4 GB, where the guest's memory lives: a range the host owns, mapped over */
	low = host_low_map((size_t)size, PROT_READ | PROT_WRITE);
	if (low)
	{
		void *fixed = mmap(low, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);

		rep("%s.mmap_below_4GB: %s at %p (errno %d)", key, fixed == MAP_FAILED ? "failed" : "mapped", fixed,
			fixed == MAP_FAILED ? errno : 0);
		if (fixed != MAP_FAILED)
		{
			if (make_buffer(&readback, 131072, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
			{
				void *plain = host_low_map((size_t)size, PROT_READ | PROT_WRITE);
				void *plain2 = host_low_map((size_t)size, PROT_READ | PROT_WRITE);

				fill_pattern(fixed, (size_t)size, 11);
				if (driver_map)
					rep("%s.same_pages_as_the_drivers_mapping: %s", key, yesno(!memcmp(fixed, driver_map, 65536)));
				memset(&view, 0, sizeof(view));
				view.buffer = buffer;
				view.size = size;
				rep("%s.gpu_reads_guest_writes: %s", key, yesno(copy_matches(&view, 11, &readback)));
				fill_pattern(fixed, (size_t)size, 12);
				rep("%s.gpu_reads_rewrite: %s", key, yesno(copy_matches(&view, 12, &readback)));
				/* what the game's CPU pays for living in it: the speed of writes and reads there, against ordinary memory */
				if (plain && plain2)
				{
					memset(plain, 1, (size_t)size);
					memset(plain2, 2, (size_t)size);
					rep("%s.cpu_write_MBps: %.0f here, %.0f in ordinary memory", key, copy_speed(fixed, plain, (size_t)size),
						copy_speed(plain2, plain, (size_t)size));
					rep("%s.cpu_read_MBps: %.0f here, %.0f in ordinary memory", key, copy_speed(plain, fixed, (size_t)size),
						copy_speed(plain, plain2, (size_t)size));
				}
				if (plain)
					host_low_unmap(plain, (size_t)size);
				if (plain2)
					host_low_unmap(plain2, (size_t)size);
				free_buffer(&readback);
			}
		}
		/* the range is the host's to give back, whatever is mapped there now */
		host_low_unmap(low, (size_t)size);
	}
	if (anywhere != MAP_FAILED)
		munmap(anywhere, (size_t)size);
done:
	if (fd >= 0)
		close(fd);
	if (driver_map)
		vkUnmapMemory(P.device, memory);
	if (buffer)
		vkDestroyBuffer(P.device, buffer, NULL);
	if (memory)
		vkFreeMemory(P.device, memory, NULL);
}

static void memory_export_case(void)
{
	if (!extension_enabled("VK_KHR_external_memory_fd") || !vkGetMemoryFdKHR)
	{
		rep("memory.export: not possible: VK_KHR_external_memory_fd is not offered by this device");
		return;
	}
	memory_export_try("memory.export.cached", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
	memory_export_try("memory.export.uncached", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
}

static void step_memory(void)
{
	rep("step.memory: start");
	rep("memory.window: the guest's window is the same kind of mapping (host_memory.c), anonymous and private");
	memory_import_cases();
	memory_import_texture();
	memory_ring_cost();
	memory_import_gpu_cost();
	memory_export_case();
	rep("step.memory: done");
}

/* ---------- step 4: the shader compiler (compile) */

/* the shaders of the probe: the pass-through for the fixed cost, then the game's, hand-converted
(port/android/probe/): the largest vertex shader and pixel shader of a GL ES session, and the median ones */
static const char *const shader_names[] = { "pass.vert", "pass.frag", "vs_typical.vert", "ps_typical.frag", "vs_large.vert", "ps_large.frag" };
#define SHADER_COUNT 6

/* the spirv of each shader, kept for step 5 (made by step 4 or, if that was not run, by step 5) */
static struct spirv shader_spirv[SHADER_COUNT];
static int shader_ready[SHADER_COUNT];

static void write_spirv_file(const char *name, const struct spirv *code)
{
	char path[700];
	FILE *file;

	snprintf(path, sizeof(path), "%s/vk_probe_spirv", P.data_root);
	mkdir(path, 0770);
	snprintf(path, sizeof(path), "%s/vk_probe_spirv/%s.spv", P.data_root, name);
	file = fopen(path, "wb");
	if (file)
	{
		fwrite(code->words, 4, code->count, file);
		fclose(file);
	}
}

/* the size of libhalo_glslang.so in memory, from /proc/self/maps (it is mapped from the APK) */
static void report_glslang_size(void)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	unsigned long long total = 0;

	if (!maps)
		return;
	while (fgets(line, sizeof(line), maps))
	{
		unsigned long long from, to;

		if (strstr(line, "libhalo_glslang.so") && sscanf(line, "%llx-%llx", &from, &to) == 2)
			total += to - from;
	}
	fclose(maps);
	rep("compile.library_mapped_bytes: %llu", total);
}

/* compiles every shader once, quietly, to have its SPIR-V; 1 if all are there */
static int ensure_shaders(void)
{
	int index, all = 1;

	for (index = 0; index < SHADER_COUNT; index++)
	{
		if (!shader_ready[index] && compile_asset(shader_names[index], &shader_spirv[index]))
			shader_ready[index] = 1;
		all &= shader_ready[index];
	}
	return all;
}

static void step_compile(void)
{
	enum { RUNS = 10 };
	int index, run;
	uint64_t start;

	rep("step.compile: start");
	rep("compile.glslang_library: libhalo_glslang.so (glslang 16.6.0, ENABLE_OPT off, ENABLE_HLSL off), Vulkan 1.0 / SPIR-V 1.0 target, one thread");
	start = now_ns();
	if (!glslang_load())
	{
		rep("step.compile: failed (glslang did not load)");
		return;
	}
	rep("compile.load_and_initialize_process: %.3f ms (dlopen, symbols, glslang_initialize_process)", (double)(now_ns() - start) / 1e6);
	report_glslang_size();
	for (index = 0; index < SHADER_COUNT; index++)
	{
		char *text = asset_text(shader_names[index]);
		double wall[RUNS], cpu[RUNS];
		struct spirv code;
		char key[96];

		if (!text)
			continue;
		for (run = 0; run < RUNS; run++)
		{
			uint64_t wall_ns = 0, cpu_ns_taken = 0;
			int ok = glslang_compile(text, stage_of(shader_names[index]), &code, &wall_ns, &cpu_ns_taken, 0);

			if (!ok)
			{
				rep("compile.%s: failed", shader_names[index]);
				break;
			}
			wall[run] = (double)wall_ns / 1e6;
			cpu[run] = (double)cpu_ns_taken / 1e6;
			if (run < RUNS - 1)
				free(code.words);
		}
		if (run == RUNS)
		{
			if (shader_ready[index])
				free(shader_spirv[index].words);
			shader_spirv[index] = code;
			shader_ready[index] = 1;
			write_spirv_file(shader_names[index], &code);
			rep("compile.%s.source_bytes: %zu", shader_names[index], strlen(text));
			rep("compile.%s.spirv_bytes: %zu", shader_names[index], code.count * 4);
			snprintf(key, sizeof(key), "compile.%s.wall", shader_names[index]);
			rep_times(key, wall, RUNS);
			snprintf(key, sizeof(key), "compile.%s.thread_cpu", shader_names[index]);
			rep_times(key, cpu, RUNS);
			if (index == 0)
				rep("compile.first_compile_of_the_first_shader: %.3f ms wall, %.3f ms thread cpu (it pays the front end's start)", wall[0], cpu[0]);
		}
		free(text);
	}
	rep("compile.spirv_files: %s/vk_probe_spirv/*.spv (check off the device with spirv-val and spirv-dis)", P.data_root);
	rep("step.compile: done");
}

/* ---------- step 5: pipelines (pipelines) */

struct pair
{
	const char *name;
	int vertex, fragment; /* indexes of shader_names */
	struct vertex_layout layout;
};

static struct pair pairs[3];

static void make_pairs(void)
{
	struct vertex_layout *typical = &pairs[1].layout, *large = &pairs[2].layout;
	int index;

	pairs[0].name = "pass";
	pairs[0].vertex = 0;
	pairs[0].fragment = 1;
	pairs[0].layout = layout_color;
	pairs[1].name = "typical";
	pairs[1].vertex = 2;
	pairs[1].fragment = 3;
	pairs[2].name = "large";
	pairs[2].vertex = 4;
	pairs[2].fragment = 5;
	/* the typical vertex shader reads 16 vec4 inputs */
	typical->stride = 256;
	typical->count = 16;
	for (index = 0; index < 16; index++)
	{
		typical->attributes[index].location = (uint32_t)index;
		typical->attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		typical->attributes[index].offset = (uint32_t)index * 16;
	}
	/* the large one: a vec4, three packed words, twelve more vec4 */
	large->count = 16;
	large->attributes[0].location = 0;
	large->attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	large->attributes[0].offset = 0;
	for (index = 1; index < 4; index++)
	{
		large->attributes[index].location = (uint32_t)index;
		large->attributes[index].format = VK_FORMAT_R32_UINT;
		large->attributes[index].offset = 16 + (uint32_t)(index - 1) * 4;
	}
	for (index = 4; index < 16; index++)
	{
		large->attributes[index].location = (uint32_t)index;
		large->attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		large->attributes[index].offset = 28 + (uint32_t)(index - 4) * 16;
	}
	large->stride = 28 + 12 * 16;
}

struct pipe_context
{
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkFormat depth;
	struct dynamic_set full, plain;
	int ready;
};

static struct pipe_context X;

static int pipe_context_init(void)
{
	VkDescriptorSetLayoutBinding bindings[6];
	VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	int index;

	if (X.ready)
		return 1;
	if (!ensure_shaders())
	{
		rep("error: the shaders did not all compile; no pipelines");
		return 0;
	}
	make_pairs();
	memset(bindings, 0, sizeof(bindings));
	/* the vertex shader's block, the pixel shader's block, then four samplers (the bindings the converted shaders use) */
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[1] = bindings[0];
	bindings[1].binding = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	for (index = 2; index < 6; index++)
	{
		bindings[index].binding = (uint32_t)index;
		bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	info.bindingCount = 6;
	info.pBindings = bindings;
	if (!CHECK_RESULT(vkCreateDescriptorSetLayout(P.device, &info, NULL, &X.set_layout)))
		return 0;
	X.layout = make_pipeline_layout(X.set_layout);
	if (!X.layout)
		return 0;
	X.depth = pick_depth_format();
	rep("pipelines.formats: colour %d, depth-stencil %d, dynamic rendering %s", (int)PROBE_COLOR_FORMAT, (int)X.depth,
		P.dynamic_rendering ? "yes" : "no (a render pass)");
	dynamic_choose(&X.full, 1);
	dynamic_choose(&X.plain, 0);
	dynamic_report("pipelines.dynamic_states_full", &X.full);
	dynamic_report("pipelines.dynamic_states_plain", &X.plain);
	rep("pipelines.dynamic_state_flags: counted viewport %s, stride %s, vertex input %s", yesno(X.full.counted_viewport),
		yesno(X.full.stride), yesno(X.full.vertex_input));
	X.ready = 1;
	return 1;
}

static void pipe_context_free(void)
{
	if (X.layout)
		vkDestroyPipelineLayout(P.device, X.layout, NULL);
	if (X.set_layout)
		vkDestroyDescriptorSetLayout(P.device, X.set_layout, NULL);
	memset(&X, 0, sizeof(X));
}

struct pipe_timing
{
	double module_ms, wall_ms, cpu_ms;
};

/* one pair's shader modules and pipeline, timed; the pipeline is returned (or NULL) */
static VkPipeline make_pair_pipeline(int pair, const struct dynamic_set *dynamic, VkPipelineCache cache, int32_t salt, struct pipe_timing *timing)
{
	struct pipeline_request request;
	uint64_t wall = 0, cpu = 0, start = now_ns();
	VkShaderModule vertex = make_shader_module(&shader_spirv[pairs[pair].vertex]);
	VkShaderModule fragment = make_shader_module(&shader_spirv[pairs[pair].fragment]);
	VkPipeline pipeline = VK_NULL_HANDLE;

	if (timing)
		timing->module_ms = (double)(now_ns() - start) / 1e6;
	if (vertex && fragment)
	{
		memset(&request, 0, sizeof(request));
		request.vertex = vertex;
		request.fragment = fragment;
		request.layout = &pairs[pair].layout;
		request.pipeline_layout = X.layout;
		request.color = PROBE_COLOR_FORMAT;
		request.depth = X.depth;
		request.depth_test = 1;
		request.blend = 1;
		request.dynamic = dynamic;
		request.cache = cache;
		request.salt = salt;
		pipeline = create_pipeline(&request, &wall, &cpu);
	}
	if (timing)
	{
		timing->wall_ms = (double)wall / 1e6;
		timing->cpu_ms = (double)cpu / 1e6;
	}
	if (vertex)
		vkDestroyShaderModule(P.device, vertex, NULL);
	if (fragment)
		vkDestroyShaderModule(P.device, fragment, NULL);
	return pipeline;
}

#define PIPELINE_RUNS 6

/* PIPELINE_RUNS creations of each pair with salts of their own, in a fresh cache (or `cache`) */
static void time_pairs(const char *key, const struct dynamic_set *dynamic, VkPipelineCache cache, int32_t salt_base)
{
	int pair, run;

	for (pair = 0; pair < 3; pair++)
	{
		double module[PIPELINE_RUNS], wall[PIPELINE_RUNS], cpu[PIPELINE_RUNS];
		char text[128];
		int made = 0;

		for (run = 0; run < PIPELINE_RUNS; run++)
		{
			struct pipe_timing timing;
			VkPipeline pipeline = make_pair_pipeline(pair, dynamic, cache, salt_base + run * 8 + pair, &timing);

			if (!pipeline)
				break;
			module[run] = timing.module_ms;
			wall[run] = timing.wall_ms;
			cpu[run] = timing.cpu_ms;
			vkDestroyPipeline(P.device, pipeline, NULL);
			made++;
		}
		if (made == PIPELINE_RUNS)
		{
			snprintf(text, sizeof(text), "%s.%s.shader_modules", key, pairs[pair].name);
			rep_times(text, module, PIPELINE_RUNS);
			snprintf(text, sizeof(text), "%s.%s.pipeline_wall", key, pairs[pair].name);
			rep_times(text, wall, PIPELINE_RUNS);
			snprintf(text, sizeof(text), "%s.%s.pipeline_thread_cpu", key, pairs[pair].name);
			rep_times(text, cpu, PIPELINE_RUNS);
		}
		else
		{
			rep("%s.%s: failed", key, pairs[pair].name);
		}
	}
}

/* ---- the second thread's test */

struct frame_stats
{
	double cpu[3][400], fence[3][400];
	int count[3];
};

struct thread_a
{
	pthread_t thread;
	volatile int phase, stop;
	struct frame_stats stats;
	struct target target;
	struct buffer vertices;
	VkPipeline pipeline;
	int ok;
};

static void *frames_thread(void *argument)
{
	struct thread_a *a = argument;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	uint64_t next = now_ns();

	if (!make_command_pool(&pool) || !allocate_command(pool, &command) || !make_fence(&fence, 0))
		return NULL;
	while (!a->stop)
	{
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		VkDeviceSize offset = 0;
		int phase = a->phase, draw;
		uint64_t cpu_start = cpu_ns(), wall_start, submitted;

		/* a frame: a clear and a hundred draws with the pipeline that is already made */
		vkResetCommandPool(P.device, pool, 0);
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(command, &begin);
		pass_begin(command, &a->target, black);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, a->pipeline);
		dynamic_record(command, &X.full, &pairs[0].layout, 256, 256, 1);
		bind_vertex_buffer(command, &X.full, &pairs[0].layout, a->vertices.buffer, offset);
		for (draw = 0; draw < 100; draw++)
			vkCmdDraw(command, 3, 1, 0, 0);
		pass_end(command);
		vkEndCommandBuffer(command);
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		vkResetFences(P.device, 1, &fence);
		wall_start = now_ns();
		vkQueueSubmit(P.queue, 1, &submit, fence);
		submitted = now_ns();
		if (a->stats.count[phase] < 400)
			a->stats.cpu[phase][a->stats.count[phase]] = (double)(cpu_ns() - cpu_start) / 1e6;
		(void)submitted;
		vkWaitForFences(P.device, 1, &fence, VK_TRUE, WAIT_FOREVER);
		if (a->stats.count[phase] < 400)
		{
			a->stats.fence[phase][a->stats.count[phase]] = (double)(now_ns() - wall_start) / 1e6;
			a->stats.count[phase]++;
		}
		next += 16000000ull;
		{
			uint64_t now = now_ns();

			if (next > now)
			{
				struct timespec wait = { (time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull) };

				nanosleep(&wait, NULL);
			}
			else
			{
				next = now; /* behind: no catching up in a burst */
			}
		}
	}
	vkDeviceWaitIdle(P.device);
	vkDestroyFence(P.device, fence, NULL);
	vkDestroyCommandPool(P.device, pool, NULL);
	a->ok = 1;
	return NULL;
}

static void report_phase(const char *name, const struct thread_a *a, int phase)
{
	char key[96];
	int index, slow = 0;
	double median;
	double sorted[400];
	int count = a->stats.count[phase];

	if (count < 2)
	{
		rep("pipelines.thread.%s: %d frames", name, count);
		return;
	}
	memcpy(sorted, a->stats.fence[phase], sizeof(double) * (size_t)count);
	qsort(sorted, (size_t)count, sizeof(double), compare_doubles);
	median = sorted[count / 2];
	for (index = 0; index < count; index++)
		if (a->stats.fence[phase][index] > 2.0 * median)
			slow++;
	snprintf(key, sizeof(key), "pipelines.thread.%s.frame_cpu", name);
	rep_spread(key, a->stats.cpu[phase], count);
	snprintf(key, sizeof(key), "pipelines.thread.%s.submit_to_fence", name);
	rep_spread(key, a->stats.fence[phase], count);
	rep("pipelines.thread.%s.frames_over_twice_the_median_submit_to_fence: %d of %d", name, slow, count);
}

static void thread_test(void)
{
	static struct thread_a A;
	struct buffer *vertices = &A.vertices;
	VkPipelineCache cache = VK_NULL_HANDLE;
	VkPipelineCacheCreateInfo cache_info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	int index;
	int32_t base = (int32_t)(now_ns() & 0x3fffffff);
	uint64_t b_start, b_end;
	double b_times[20];

	memset(&A, 0, sizeof(A));
	A.pipeline = make_pair_pipeline(0, &X.full, VK_NULL_HANDLE, base, NULL);
	if (!A.pipeline || !target_make(&A.target, 256, 256, PROBE_COLOR_FORMAT, X.depth) ||
		!make_buffer(vertices, 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1))
	{
		rep("pipelines.thread: setup failed");
		return;
	}
	fill_triangle(vertices->map, 255, 0, 0);
	if (pthread_create(&A.thread, NULL, frames_thread, &A) != 0)
	{
		rep("pipelines.thread: cannot start the frame thread");
		return;
	}
	A.phase = 0;
	sleep(3);
	A.phase = 1;
	CHECK_RESULT(vkCreatePipelineCache(P.device, &cache_info, NULL, &cache));
	b_start = now_ns();
	/* thread B (this one): twenty new pipelines, each with a salt of its own */
	for (index = 0; index < 20; index++)
	{
		struct pipe_timing timing;
		VkPipeline pipeline = make_pair_pipeline(1 + index % 2, &X.full, cache, base + 100 + index, &timing);

		b_times[index] = timing.wall_ms;
		if (pipeline)
			vkDestroyPipeline(P.device, pipeline, NULL);
	}
	b_end = now_ns();
	A.phase = 2;
	sleep(3);
	A.stop = 1;
	pthread_join(A.thread, NULL);
	rep("pipelines.thread.twenty_pipelines: %.1f ms in all, %.1f ms each on average (the typical and the large pair, alternating)",
		(double)(b_end - b_start) / 1e6, (double)(b_end - b_start) / 1e6 / 20.0);
	(void)b_times;
	report_phase("before", &A, 0);
	report_phase("during", &A, 1);
	report_phase("after", &A, 2);
	if (cache)
		vkDestroyPipelineCache(P.device, cache, NULL);
	vkDestroyPipeline(P.device, A.pipeline, NULL);
	free_buffer(vertices);
	target_free(&A.target);
}

/* ---- the saved cache and its salts */

struct cache_header
{
	uint32_t size, version, vendor, device;
	uint8_t uuid[16];
};

static void step_pipelines(void)
{
	char cache_path[700], salt_path[700];
	int32_t base = (int32_t)(now_ns() & 0x3fffffff), saved_base[3], have_saved = 0;
	void *saved_data = NULL;
	size_t saved_size = 0;
	VkPipelineCache cold_cache = VK_NULL_HANDLE, warm_cache = VK_NULL_HANDLE, empty_cache = VK_NULL_HANDLE;
	VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	FILE *file;
	int pair;

	rep("step.pipelines: start");
	if (!pipe_context_init())
	{
		rep("step.pipelines: failed (no shaders)");
		return;
	}
	snprintf(cache_path, sizeof(cache_path), "%s/vk_probe_pipeline_cache.bin", P.data_root);
	snprintf(salt_path, sizeof(salt_path), "%s/vk_probe_salts.txt", P.data_root);

	/* what the last run saved */
	file = fopen(salt_path, "r");
	if (file)
	{
		have_saved = fscanf(file, "%d %d %d", &saved_base[0], &saved_base[1], &saved_base[2]) == 3;
		fclose(file);
	}
	file = fopen(cache_path, "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		saved_size = (size_t)ftell(file);
		fseek(file, 0, SEEK_SET);
		saved_data = malloc(saved_size ? saved_size : 1);
		if (fread(saved_data, 1, saved_size, file) != saved_size)
		{
			free(saved_data);
			saved_data = NULL;
			saved_size = 0;
		}
		fclose(file);
	}
	if (saved_data && saved_size >= sizeof(struct cache_header))
	{
		const struct cache_header *header = saved_data;

		rep("pipelines.saved_cache: %zu bytes, header size %u version %u vendor 0x%04x device 0x%04x uuid %s", saved_size,
			header->size, header->version, header->vendor, header->device,
			!memcmp(header->uuid, P.properties.pipelineCacheUUID, 16) ? "matches this device" : "DIFFERENT from this device");
	}
	else
	{
		rep("pipelines.saved_cache: none (run the probe again for the warm figures)");
	}

	/* cold: salts of this run, a cache that starts empty */
	CHECK_RESULT(vkCreatePipelineCache(P.device, &info, NULL, &cold_cache));
	rep("pipelines.salts: base %d", base);
	time_pairs("pipelines.cold", &X.full, cold_cache, base);

	/* warm: last run's pipelines (its first run of each pair), from the saved cache, and without it */
	if (have_saved && saved_data)
	{
		VkPipelineCacheCreateInfo seeded = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
		size_t after = 0;

		seeded.initialDataSize = saved_size;
		seeded.pInitialData = saved_data;
		CHECK_RESULT(vkCreatePipelineCache(P.device, &seeded, NULL, &warm_cache));
		vkGetPipelineCacheData(P.device, warm_cache, &after, NULL);
		rep("pipelines.warm.cache_after_loading: %zu bytes (the file was %zu; the driver %s)", after, saved_size,
			after >= saved_size ? "kept it" : "dropped some or all of it");
		for (pair = 0; pair < 3; pair++)
		{
			struct pipe_timing timing;
			VkPipeline pipeline = make_pair_pipeline(pair, &X.full, warm_cache, saved_base[pair], &timing);

			rep("pipelines.warm.%s: pipeline %.3f ms wall, %.3f ms thread cpu (modules %.3f ms)", pairs[pair].name,
				timing.wall_ms, timing.cpu_ms, timing.module_ms);
			if (pipeline)
				vkDestroyPipeline(P.device, pipeline, NULL);
		}
		/* the driver's own cache, with ours empty */
		CHECK_RESULT(vkCreatePipelineCache(P.device, &info, NULL, &empty_cache));
		for (pair = 0; pair < 3; pair++)
		{
			struct pipe_timing timing;
			VkPipeline pipeline = make_pair_pipeline(pair, &X.full, empty_cache, saved_base[pair], &timing);

			rep("pipelines.driver_cache_only.%s: pipeline %.3f ms wall, %.3f ms thread cpu", pairs[pair].name,
				timing.wall_ms, timing.cpu_ms);
			if (pipeline)
				vkDestroyPipeline(P.device, pipeline, NULL);
		}
	}

	/* nothing dynamic but the viewport and the scissor */
	time_pairs("pipelines.no_dynamic_state", &X.plain, VK_NULL_HANDLE, base + 100000);

	/* the second thread */
	thread_test();

	/* saved for the next run: the cache holding this run's cold pipelines, and the salts of their first run */
	{
		size_t size = 0;

		vkGetPipelineCacheData(P.device, cold_cache, &size, NULL);
		if (size)
		{
			void *data = malloc(size);

			if (vkGetPipelineCacheData(P.device, cold_cache, &size, data) == VK_SUCCESS)
			{
				file = fopen(cache_path, "wb");
				if (file)
				{
					fwrite(data, 1, size, file);
					fclose(file);
				}
				rep("pipelines.saved_cache_now: %zu bytes", size);
			}
			free(data);
		}
		file = fopen(salt_path, "w");
		if (file)
		{
			fprintf(file, "%d %d %d\n", base, base + 1, base + 2);
			fclose(file);
		}
	}
	if (cold_cache)
		vkDestroyPipelineCache(P.device, cold_cache, NULL);
	if (warm_cache)
		vkDestroyPipelineCache(P.device, warm_cache, NULL);
	if (empty_cache)
		vkDestroyPipelineCache(P.device, empty_cache, NULL);
	free(saved_data);
	rep("step.pipelines: done");
}

/* ---------- step 5b: the game's shaders, drawn (draw) */

/* The draw the earlier attempt's Turnip crashed on, and the one part A never made: the converted
shaders of the game (the typical and the large pair, and the pass-through pair as the control), with the
descriptor set they declare (the vertex block at binding 0, the pixel block at 1, four combined image
samplers at 2 to 5), drawn into a 256x256 target and read back. Every sub-step that touches the driver in a
new way is guarded, so that a crash names itself on the next run: the descriptor update, each variant's
pipeline and draw, the rewrite of the set after a draw, push descriptors. */

#define DRAW_SIZE 256

/* the blocks as std140 lays them out (the shaders' declarations, in the shaders of port/android/probe): asserted, because
a C structure that disagrees with the block from some byte on draws wrong without a word */
struct vs_block
{
	float c[192][4];
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float screen_offset;
	float pad[2];
};

struct ps_block
{
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float alpha_reference;
	float pad[3];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float texture_lod_bias[4];
};

_Static_assert(offsetof(struct vs_block, viewport_scale) == 3072, "std140: viewport_scale");
_Static_assert(offsetof(struct vs_block, viewport_offset) == 3088, "std140: viewport_offset");
_Static_assert(offsetof(struct vs_block, point_size) == 3104, "std140: point_size");
_Static_assert(offsetof(struct vs_block, screen_offset) == 3108, "std140: screen_offset");
_Static_assert(offsetof(struct ps_block, ps_c1) == 128, "std140: ps_c1");
_Static_assert(offsetof(struct ps_block, ps_final_c0) == 256, "std140: ps_final_c0");
_Static_assert(offsetof(struct ps_block, fog_parameters) == 304, "std140: fog_parameters");
_Static_assert(offsetof(struct ps_block, alpha_reference) == 320, "std140: alpha_reference");
_Static_assert(offsetof(struct ps_block, bump_matrix) == 336, "std140: bump_matrix");
_Static_assert(offsetof(struct ps_block, bump_luminance) == 400, "std140: bump_luminance");
_Static_assert(offsetof(struct ps_block, texture_scale) == 464, "std140: texture_scale");
_Static_assert(offsetof(struct ps_block, texture_lod_bias) == 528, "std140: texture_lod_bias");

/* a 64x64 texture, or a cube of six, with its sampler */
struct texture
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkSampler sampler;
	VkFormat format;
	int cube;
};

#define TEXTURE_SIZE 64

static void texture_free(struct texture *texture)
{
	if (texture->sampler)
		vkDestroySampler(P.device, texture->sampler, NULL);
	if (texture->view)
		vkDestroyImageView(P.device, texture->view, NULL);
	if (texture->image)
		vkDestroyImage(P.device, texture->image, NULL);
	if (texture->memory)
		vkFreeMemory(P.device, texture->memory, NULL);
	memset(texture, 0, sizeof(*texture));
}

/* the pixels of one layer: a checker over a gradient, a colour of its own per layer */
static void texture_fill_rgba(uint8_t *out, int layer)
{
	int x, y;

	for (y = 0; y < TEXTURE_SIZE; y++)
	{
		for (x = 0; x < TEXTURE_SIZE; x++)
		{
			int checker = ((x >> 3) ^ (y >> 3)) & 1;

			out[(y * TEXTURE_SIZE + x) * 4 + 0] = (uint8_t)(x * 4);
			out[(y * TEXTURE_SIZE + x) * 4 + 1] = (uint8_t)(y * 4);
			out[(y * TEXTURE_SIZE + x) * 4 + 2] = (uint8_t)(layer * 40 + (checker ? 100 : 0));
			out[(y * TEXTURE_SIZE + x) * 4 + 3] = 255;
		}
	}
}

/* BC1 blocks (8 bytes each, 16x16 of them): two 5:6:5 colours that change with the block, four-colour mode */
static void texture_fill_bc1(uint8_t *out, int layer)
{
	int block;

	for (block = 0; block < 256; block++)
	{
		int bx = block % 16, by = block / 16;
		uint16_t c0 = (uint16_t)(((16 + bx) << 11) | ((by * 4) << 5) | (layer * 4 + 8));
		uint16_t c1 = (uint16_t)(((15 - bx) << 11) | (((15 - by) * 2) << 5) | (layer * 2));
		uint8_t *p = out + block * 8;

		p[0] = (uint8_t)c0;
		p[1] = (uint8_t)(c0 >> 8);
		p[2] = (uint8_t)c1;
		p[3] = (uint8_t)(c1 >> 8);
		p[4] = p[5] = p[6] = p[7] = (uint8_t)(block & 1 ? 0xE4 : 0x1B);
	}
}

static int format_samples(VkFormat format)
{
	VkFormatProperties properties;

	vkGetPhysicalDeviceFormatProperties(P.physical, format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
		(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
		(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
}

/* a texture of the given kind, filled and ready to be sampled; 0 (and a report) if it cannot be made */
static int texture_make(struct texture *texture, int cube, int compressed, VkSamplerAddressMode address)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkBufferImageCopy copies[6];
	struct buffer staging;
	VkCommandBuffer command;
	uint32_t layers = cube ? 6 : 1, layer;
	size_t layer_bytes = compressed ? 256 * 8 : TEXTURE_SIZE * TEXTURE_SIZE * 4;

	memset(texture, 0, sizeof(*texture));
	texture->format = compressed ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
	texture->cube = cube;
	if (!make_buffer(&staging, layer_bytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
		return 0;
	for (layer = 0; layer < layers; layer++)
	{
		if (compressed)
			texture_fill_bc1((uint8_t *)staging.map + layer * layer_bytes, (int)layer);
		else
			texture_fill_rgba((uint8_t *)staging.map + layer * layer_bytes, (int)layer);
	}
	info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = texture->format;
	info.extent.width = TEXTURE_SIZE;
	info.extent.height = TEXTURE_SIZE;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = layers;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!CHECK_RESULT(vkCreateImage(P.device, &info, NULL, &texture->image)))
		goto fail;
	vkGetImageMemoryRequirements(P.device, texture->image, &requirements);
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (allocate.memoryTypeIndex == UINT32_MAX)
		allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, 0, 0);
	if (!CHECK_RESULT(vkAllocateMemory(P.device, &allocate, NULL, &texture->memory)) ||
		!CHECK_RESULT(vkBindImageMemory(P.device, texture->image, texture->memory, 0)))
		goto fail;
	view.image = texture->image;
	view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
	view.format = texture->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = layers;
	if (!CHECK_RESULT(vkCreateImageView(P.device, &view, NULL, &texture->view)))
		goto fail;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = address;
	sampler.maxLod = 0.0f;
	sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	if (!CHECK_RESULT(vkCreateSampler(P.device, &sampler, NULL, &texture->sampler)))
		goto fail;

	command = command_begin();
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = layers;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	memset(copies, 0, sizeof(copies));
	for (layer = 0; layer < layers; layer++)
	{
		copies[layer].bufferOffset = layer * layer_bytes;
		copies[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copies[layer].imageSubresource.baseArrayLayer = layer;
		copies[layer].imageSubresource.layerCount = 1;
		copies[layer].imageExtent.width = TEXTURE_SIZE;
		copies[layer].imageExtent.height = TEXTURE_SIZE;
		copies[layer].imageExtent.depth = 1;
	}
	vkCmdCopyBufferToImage(command, staging.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, copies);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	if (!command_submit_wait(command))
		goto fail;
	free_buffer(&staging);
	return 1;
fail:
	free_buffer(&staging);
	texture_free(texture);
	return 0;
}

/* ---- the draw's resources, made once */

enum { _draw_pass, _draw_typical, _draw_large, _draw_pairs };

struct draw_context
{
	struct target target;
	struct buffer readback;
	struct buffer vs_block[2], ps_block;  /* the second vertex block moves the triangle (the rewrite test) */
	struct buffer vertices[_draw_pairs];
	/* rgba 2d, bc1 2d, rgba cube, bc1 cube (RGBA8 stands in for BC1 where the device cannot sample it) */
	struct texture textures[4];
	VkDescriptorPool pool;
	VkDescriptorSet set[_draw_pairs];
	int ready;
	uint32_t clear_word;
};

static struct draw_context W;

/* where the picture of the draw is: the pixels, and what to say of them */
struct draw_result
{
	int finished;
	uint32_t not_clear;
	uint64_t hash;
	uint32_t center;
};

static const float draw_clear[4] = { 0.2f, 0.4f, 0.6f, 1.0f };

static uint64_t fnv1a(const void *data, size_t size)
{
	const uint8_t *bytes = data;
	uint64_t hash = 1469598103934665603ull;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ull;
	return hash;
}

/* the picture as a portable pixmap in the data folder, for looking at off the device */
static void draw_write_picture(const char *name, const uint32_t *pixels)
{
	char path[760];
	FILE *file;
	int index;

	snprintf(path, sizeof(path), "%s/vk_probe_draw_%s.ppm", P.data_root, name);
	file = fopen(path, "wb");
	if (!file)
		return;
	fprintf(file, "P6\n%d %d\n255\n", DRAW_SIZE, DRAW_SIZE);
	for (index = 0; index < DRAW_SIZE * DRAW_SIZE; index++)
	{
		uint8_t rgb[3] = { (uint8_t)pixels[index], (uint8_t)(pixels[index] >> 8), (uint8_t)(pixels[index] >> 16) };

		fwrite(rgb, 1, 3, file);
	}
	fclose(file);
}

static void draw_analyze(const char *name, struct draw_result *result)
{
	const uint32_t *pixels = W.readback.map;
	int index;

	result->finished = 1;
	result->not_clear = 0;
	for (index = 0; index < DRAW_SIZE * DRAW_SIZE; index++)
		if (pixels[index] != W.clear_word)
			result->not_clear++;
	result->hash = fnv1a(pixels, DRAW_SIZE * DRAW_SIZE * 4);
	result->center = pixels[DRAW_SIZE / 2 * DRAW_SIZE + DRAW_SIZE / 2];
	if (name)
		draw_write_picture(name, pixels);
}

static void draw_context_free(void)
{
	int index;

	if (!P.device)
		return;
	for (index = 0; index < 4; index++)
		texture_free(&W.textures[index]);
	for (index = 0; index < _draw_pairs; index++)
		free_buffer(&W.vertices[index]);
	free_buffer(&W.vs_block[0]);
	free_buffer(&W.vs_block[1]);
	free_buffer(&W.ps_block);
	free_buffer(&W.readback);
	if (W.pool)
		vkDestroyDescriptorPool(P.device, W.pool, NULL);
	target_free(&W.target);
	memset(&W, 0, sizeof(W));
}

/* the constants that put the vertices on the screen: an identity transform where each shader reads its
matrix (the typical vertex shader's c[28] to c[31]; the large one's c[0] to c[3], with the skinning matrices at
c[60] to c[62] and the index scale c[7].w), c[58] and c[59] for the viewport, and viewport_scale and
viewport_offset that make the vertices' pixel coordinates cover a 256x256 target */
static void draw_fill_vs(struct vs_block *block, float scale)
{
	int index, k;

	memset(block, 0, sizeof(*block));
	for (index = 0; index < 192; index++)
		for (k = 0; k < 4; k++)
			block->c[index][k] = 0.5f;
	for (index = 0; index < 4; index++)
	{
		memset(block->c[index], 0, 16);
		memset(block->c[28 + index], 0, 16);
		block->c[index][index] = 1.0f;
		block->c[28 + index][index] = 1.0f;
	}
	for (index = 0; index < 3; index++)
	{
		memset(block->c[60 + index], 0, 16);
		block->c[60 + index][index] = 1.0f;
	}
	memset(block->c[7], 0, 16);
	block->c[7][3] = 1.0f;
	block->c[58][0] = block->c[58][1] = block->c[58][2] = 1.0f;
	block->c[58][3] = 0.0f;
	memset(block->c[59], 0, 16);
	block->viewport_scale[0] = scale;
	block->viewport_scale[1] = scale;
	block->viewport_scale[2] = 1.0f;
	block->viewport_scale[3] = 1.0f;
	block->viewport_offset[0] = (float)DRAW_SIZE / 2.0f;
	block->viewport_offset[1] = (float)DRAW_SIZE / 2.0f;
	block->point_size = 1.0f;
}

static void draw_fill_ps(struct ps_block *block)
{
	int index, k;

	memset(block, 0, sizeof(*block));
	for (index = 0; index < 8; index++)
	{
		for (k = 0; k < 4; k++)
		{
			block->ps_c0[index][k] = 0.5f;
			block->ps_c1[index][k] = 0.25f;
		}
	}
	for (k = 0; k < 4; k++)
	{
		block->ps_final_c0[k] = 1.0f;
		block->ps_final_c1[k] = 0.5f;
		block->fog_color[k] = 0.2f;
	}
	block->fog_parameters[0] = 0.0f;
	block->alpha_reference = 0.0f;
	for (index = 0; index < 4; index++)
	{
		block->bump_matrix[index][index] = 1.0f;
		block->bump_luminance[index][0] = 1.0f;
		for (k = 0; k < 4; k++)
			block->texture_scale[index][k] = 1.0f;
	}
}

/* three vertices of a triangle that covers the target, in pixel coordinates */
static const float draw_corners[3][2] = { { -64.0f, -64.0f }, { 512.0f, -64.0f }, { -64.0f, 512.0f } };

static void put_floats(uint8_t *at, float a, float b, float c, float d)
{
	float values[4] = { a, b, c, d };

	memcpy(at, values, sizeof(values));
}

static int draw_fill_vertices(int pair)
{
	uint8_t *map = W.vertices[pair].map;
	int vertex, attribute;

	if (pair == _draw_pass)
	{
		struct color_vertex vertices[3];

		fill_triangle(vertices, 255, 160, 32);
		memcpy(map, vertices, sizeof(vertices));
		return 1;
	}
	for (vertex = 0; vertex < 3; vertex++)
	{
		float x = draw_corners[vertex][0], y = draw_corners[vertex][1];
		uint8_t *at = map + vertex * pairs[pair].layout.stride;

		if (pair == _draw_typical)
		{
			for (attribute = 0; attribute < 16; attribute++)
				put_floats(at + attribute * 16, 0.5f, 0.5f, 0.5f, 1.0f);
			put_floats(at + 0 * 16, x, y, 0.5f, 1.0f);                         /* position */
			put_floats(at + 4 * 16, x / DRAW_SIZE, y / DRAW_SIZE, 0.0f, 1.0f); /* texture coordinates */
			put_floats(at + 9 * 16, 1.0f, 0.6f, 0.2f, 1.0f);                   /* diffuse */
		}
		else
		{
			uint32_t packed = 0x20000000u | 0x00000400u; /* a unit-ish normal in the 11:11:10 form */

			put_floats(at, x, y, 0.5f, 1.0f);
			memcpy(at + 16, &packed, 4);
			memcpy(at + 20, &packed, 4);
			memcpy(at + 24, &packed, 4);
			for (attribute = 4; attribute < 16; attribute++)
				put_floats(at + 28 + (attribute - 4) * 16, 0.5f, 0.5f, 0.5f, 1.0f);
			put_floats(at + 28 + 0 * 16, x / DRAW_SIZE, y / DRAW_SIZE, 0.0f, 1.0f); /* attribute 4: texture coordinates */
			put_floats(at + 28 + 1 * 16, 0.0f, 0.0f, 0.0f, 0.0f);                   /* attribute 5: blend indices */
			put_floats(at + 28 + 2 * 16, 1.0f, 0.0f, 0.0f, 1.0f);                   /* attribute 6: blend weights */
		}
	}
	return 1;
}

static int draw_context_init(void)
{
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	int bc = P.features2.features.textureCompressionBC && format_samples(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
	VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	int pair;

	if (W.ready)
		return 1;
	if (!pipe_context_init())
		return 0;
	if (!target_make(&W.target, DRAW_SIZE, DRAW_SIZE, PROBE_COLOR_FORMAT, X.depth) ||
		!make_buffer(&W.readback, DRAW_SIZE * DRAW_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, 0, 1))
		return 0;
	if (!make_buffer(&W.vs_block[0], 4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, 0, 1) ||
		!make_buffer(&W.vs_block[1], 4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, 0, 1) ||
		!make_buffer(&W.ps_block, 4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, 0, 1))
		return 0;
	/* the first block puts one pixel unit on one 128th of the half-width; the second halves the triangle's size */
	draw_fill_vs(W.vs_block[0].map, (float)DRAW_SIZE / 2.0f);
	draw_fill_vs(W.vs_block[1].map, (float)DRAW_SIZE);
	draw_fill_ps(W.ps_block.map);
	for (pair = 0; pair < _draw_pairs; pair++)
	{
		if (!make_buffer(&W.vertices[pair], 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, 0, 1))
			return 0;
		draw_fill_vertices(pair);
	}
	if (!texture_make(&W.textures[0], 0, 0, VK_SAMPLER_ADDRESS_MODE_REPEAT) ||
		!texture_make(&W.textures[1], 0, bc, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ||
		!texture_make(&W.textures[2], 1, 0, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ||
		!texture_make(&W.textures[3], 1, bc, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE))
		return 0;
	rep("draw.textures: 64x64 R8G8B8A8, %s, and cubes of both", bc ? "BC1" : "R8G8B8A8 standing in for BC1 (the device cannot sample it)");
	sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[0].descriptorCount = 16;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = 32;
	pool.maxSets = 8;
	pool.poolSizeCount = 2;
	pool.pPoolSizes = sizes;
	if (!CHECK_RESULT(vkCreateDescriptorPool(P.device, &pool, NULL, &W.pool)))
		return 0;
	W.ready = 1;
	return 1;
}

/* the descriptors of a pair: the vertex block at binding 0 (block 0 or 1), the pixel block at 1, four textures at 2 to 5.
The typical pixel shader's first sampler is a cube map and the large one's last is (the shaders say so). */
static void draw_descriptors(int pair, int vs_block, VkDescriptorBufferInfo buffers[2], VkDescriptorImageInfo images[4],
	VkWriteDescriptorSet writes[6], VkDescriptorSet set)
{
	/* typical: cube, bc1, rgba, bc1.  large: rgba, bc1, rgba, cube */
	static const int typical_textures[4] = { 2, 1, 0, 1 };
	static const int large_textures[4] = { 0, 1, 0, 3 };
	const int *which = pair == _draw_large ? large_textures : typical_textures;
	int index;

	buffers[0].buffer = W.vs_block[vs_block].buffer;
	buffers[0].offset = 0;
	buffers[0].range = sizeof(struct vs_block);
	buffers[1].buffer = W.ps_block.buffer;
	buffers[1].offset = 0;
	buffers[1].range = sizeof(struct ps_block);
	memset(writes, 0, sizeof(VkWriteDescriptorSet) * 6);
	for (index = 0; index < 2; index++)
	{
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = (uint32_t)index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[index].pBufferInfo = &buffers[index];
	}
	for (index = 0; index < 4; index++)
	{
		const struct texture *texture = &W.textures[which[index]];

		images[index].sampler = texture->sampler;
		images[index].imageView = texture->view;
		images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[2 + index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[2 + index].dstSet = set;
		writes[2 + index].dstBinding = (uint32_t)(2 + index);
		writes[2 + index].descriptorCount = 1;
		writes[2 + index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[2 + index].pImageInfo = &images[index];
	}
}

/* one draw of a pair into the cleared target and its readback; push is the layout of a push-descriptor
draw (the pair's descriptors are pushed instead of bound); set may be NULL (the pass-through pair uses none) */
static int draw_submit(int pair, VkPipeline pipeline, const struct dynamic_set *dynamic, int blend, VkDescriptorSet set,
	VkPipelineLayout push_layout, struct draw_result *result, const char *picture)
{
	VkCommandBuffer command = command_begin();
	VkBufferImageCopy copy;
	VkDeviceSize offset = 0;

	memset(result, 0, sizeof(*result));
	pass_begin(command, &W.target, draw_clear);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	dynamic_record(command, dynamic, &pairs[pair].layout, DRAW_SIZE, DRAW_SIZE, blend);
	bind_vertex_buffer(command, dynamic, &pairs[pair].layout, W.vertices[pair].buffer, offset);
	if (push_layout)
	{
		VkDescriptorBufferInfo buffers[2];
		VkDescriptorImageInfo images[4];
		VkWriteDescriptorSet writes[6];

		draw_descriptors(pair, 0, buffers, images, writes, VK_NULL_HANDLE);
		vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_GRAPHICS, push_layout, 0, 6, writes);
	}
	else if (set)
	{
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, X.layout, 0, 1, &set, 0, NULL);
	}
	vkCmdDraw(command, 3, 1, 0, 0);
	pass_end(command);
	image_barrier(command, W.target.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = DRAW_SIZE;
	copy.imageExtent.height = DRAW_SIZE;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(command, W.target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W.readback.buffer, 1, &copy);
	host_barrier(command);
	if (!command_submit_wait(command))
		return 0;
	draw_analyze(picture, result);
	return 1;
}

static void draw_report(const char *key, const struct draw_result *result)
{
	rep("%s.finished: %s", key, yesno(result->finished));
	if (result->finished)
	{
		rep("%s.pixels_not_clear: %u of %d", key, result->not_clear, DRAW_SIZE * DRAW_SIZE);
		rep("%s.hash: 0x%016llx", key, (unsigned long long)result->hash);
		rep("%s.center_pixel: 0x%08x (r %u g %u b %u a %u)", key, result->center, result->center & 255,
			(result->center >> 8) & 255, (result->center >> 16) & 255, result->center >> 24);
	}
}

/* a pipeline of a pair for the draw: no specialization, depth test on */
static VkPipeline draw_pipeline(int pair, const struct dynamic_set *dynamic, int blend, VkPipelineLayout layout)
{
	struct pipeline_request request;
	VkShaderModule vertex = make_shader_module(&shader_spirv[pairs[pair].vertex]);
	VkShaderModule fragment = make_shader_module(&shader_spirv[pairs[pair].fragment]);
	VkPipeline pipeline = VK_NULL_HANDLE;

	if (vertex && fragment)
	{
		memset(&request, 0, sizeof(request));
		request.vertex = vertex;
		request.fragment = fragment;
		request.layout = &pairs[pair].layout;
		request.pipeline_layout = layout;
		request.color = PROBE_COLOR_FORMAT;
		request.depth = X.depth;
		request.depth_test = 1;
		request.blend = blend;
		request.dynamic = dynamic;
		request.salt = -1;
		pipeline = create_pipeline(&request, NULL, NULL);
	}
	if (vertex)
		vkDestroyShaderModule(P.device, vertex, NULL);
	if (fragment)
		vkDestroyShaderModule(P.device, fragment, NULL);
	return pipeline;
}

static void step_draw(void)
{
	struct variant { const char *name; int pair; int blend; int full; };
	static const struct variant variants[] = {
		{ "pass.opaque", _draw_pass, 0, 0 },
		{ "pass.blend", _draw_pass, 1, 0 },
		{ "typical.opaque", _draw_typical, 0, 0 },
		{ "typical.blend", _draw_typical, 1, 0 },
		{ "typical.full_dynamic", _draw_typical, 0, 1 },
		{ "large.opaque", _draw_large, 0, 0 },
		{ "large.blend", _draw_large, 1, 0 },
		{ "large.full_dynamic", _draw_large, 0, 1 },
	};
	struct draw_result result, first[_draw_pairs];
	VkDescriptorSet *sets = W.set;
	int updated[_draw_pairs] = { 1, 0, 0 };
	char key[96];
	size_t index;
	int pair;

	rep("step.draw: start");
	if (!ensure_shaders())
	{
		rep("step.draw: failed (the shaders did not compile)");
		return;
	}
	if (!guard_begin("draw.setup"))
		return;
	if (!draw_context_init())
	{
		guard_end();
		rep("step.draw: failed (setup)");
		return;
	}
	guard_end();
	memset(first, 0, sizeof(first));

	/* the clear colour as the target holds it, to count what a draw changed */
	{
		VkCommandBuffer command = command_begin();
		VkBufferImageCopy copy;

		pass_begin(command, &W.target, draw_clear);
		pass_end(command);
		image_barrier(command, W.target.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		memset(&copy, 0, sizeof(copy));
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = DRAW_SIZE;
		copy.imageExtent.height = DRAW_SIZE;
		copy.imageExtent.depth = 1;
		vkCmdCopyImageToBuffer(command, W.target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W.readback.buffer, 1, &copy);
		host_barrier(command);
		if (!command_submit_wait(command))
		{
			rep("step.draw: failed (the clear)");
			return;
		}
		W.clear_word = ((const uint32_t *)W.readback.map)[0];
		rep("draw.clear_word: 0x%08x", W.clear_word);
	}

	/* the descriptor sets: allocated, then written with vkUpdateDescriptorSets, which the earlier attempt's Turnip died in */
	for (pair = _draw_typical; pair < _draw_pairs; pair++)
	{
		VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		VkDescriptorBufferInfo buffers[2];
		VkDescriptorImageInfo images[4];
		VkWriteDescriptorSet writes[6];

		snprintf(key, sizeof(key), "draw.update.%s", pairs[pair].name);
		allocate.descriptorPool = W.pool;
		allocate.descriptorSetCount = 1;
		allocate.pSetLayouts = &X.set_layout;
		if (!CHECK_RESULT(vkAllocateDescriptorSets(P.device, &allocate, &sets[pair])))
			continue;
		if (!guard_begin(key))
			continue;
		draw_descriptors(pair, 0, buffers, images, writes, sets[pair]);
		vkUpdateDescriptorSets(P.device, 6, writes, 0, NULL);
		guard_end();
		updated[pair] = 1;
		rep("draw.%s.descriptors_written: yes (2 uniform buffers, 4 combined image samplers)", pairs[pair].name);
	}

	/* each variant: its pipeline, the draw, the picture */
	for (index = 0; index < sizeof(variants) / sizeof(variants[0]); index++)
	{
		const struct variant *v = &variants[index];
		VkPipeline pipeline;
		int ok;

		snprintf(key, sizeof(key), "draw.%s", v->name);
		if (!updated[v->pair])
		{
			rep("%s: skipped (its descriptor set was not written)", key);
			continue;
		}
		if (!guard_begin(key))
			continue;
		pipeline = draw_pipeline(v->pair, v->full ? &X.full : &X.plain, v->blend, X.layout);
		if (!pipeline)
		{
			guard_end();
			rep("%s.finished: no (the pipeline was not made)", key);
			continue;
		}
		ok = draw_submit(v->pair, pipeline, v->full ? &X.full : &X.plain, v->blend, v->pair == _draw_pass ? VK_NULL_HANDLE : sets[v->pair],
			VK_NULL_HANDLE, &result, v->name);
		vkDestroyPipeline(P.device, pipeline, NULL);
		guard_end();
		if (!ok)
		{
			rep("%s.finished: no (the draw did not complete)", key);
			continue;
		}
		draw_report(key, &result);
		if (!strcmp(v->name + strlen(v->name) - 6, "opaque"))
			first[v->pair] = result;
	}

	/* the descriptor set written again, after the first draw was submitted and waited for: the vertex block
	swapped for one that halves the triangle (fewer pixels), then back (the first picture again) */
	for (pair = _draw_typical; pair < _draw_pairs; pair++)
	{
		VkDescriptorBufferInfo buffers[2];
		VkDescriptorImageInfo images[4];
		VkWriteDescriptorSet writes[6];
		struct draw_result moved, restored;
		VkPipeline pipeline;
		int ok;

		snprintf(key, sizeof(key), "draw.rewrite.%s", pairs[pair].name);
		if (!first[pair].finished)
		{
			rep("%s: skipped (no first draw to compare with)", key);
			continue;
		}
		if (!guard_begin(key))
			continue;
		pipeline = draw_pipeline(pair, &X.plain, 0, X.layout);
		draw_descriptors(pair, 1, buffers, images, writes, sets[pair]);
		vkUpdateDescriptorSets(P.device, 6, writes, 0, NULL);
		ok = pipeline && draw_submit(pair, pipeline, &X.plain, 0, sets[pair], VK_NULL_HANDLE, &moved, NULL);
		draw_descriptors(pair, 0, buffers, images, writes, sets[pair]);
		vkUpdateDescriptorSets(P.device, 6, writes, 0, NULL);
		ok = ok && draw_submit(pair, pipeline, &X.plain, 0, sets[pair], VK_NULL_HANDLE, &restored, NULL);
		if (pipeline)
			vkDestroyPipeline(P.device, pipeline, NULL);
		guard_end();
		if (!ok)
		{
			rep("%s.finished: no", key);
			continue;
		}
		rep("%s.finished: yes", key);
		rep("%s.second_block_pixels_not_clear: %u (first block %u)", key, moved.not_clear, first[pair].not_clear);
		rep("%s.second_block_changed_the_picture: %s", key, yesno(moved.hash != first[pair].hash));
		rep("%s.restored_matches_first: %s (0x%016llx against 0x%016llx)", key, yesno(restored.hash == first[pair].hash),
			(unsigned long long)restored.hash, (unsigned long long)first[pair].hash);
	}

	/* push descriptors: the same draw as the typical pair's first, with the descriptors pushed */
	if (!extension_enabled("VK_KHR_push_descriptor") || !vkCmdPushDescriptorSetKHR)
	{
		rep("draw.push: not possible (VK_KHR_push_descriptor is not offered by this device)");
	}
	else if (updated[_draw_typical] && guard_begin("draw.push"))
	{
		VkDescriptorSetLayoutBinding bindings[6];
		VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		VkDescriptorSetLayout push_set = VK_NULL_HANDLE;
		VkPipelineLayout push_layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		int index2;

		memset(bindings, 0, sizeof(bindings));
		for (index2 = 0; index2 < 6; index2++)
		{
			bindings[index2].binding = (uint32_t)index2;
			bindings[index2].descriptorType = index2 < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[index2].descriptorCount = 1;
			bindings[index2].stageFlags = index2 == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		layout_info.bindingCount = 6;
		layout_info.pBindings = bindings;
		if (CHECK_RESULT(vkCreateDescriptorSetLayout(P.device, &layout_info, NULL, &push_set)))
			push_layout = make_pipeline_layout(push_set);
		if (push_layout)
			pipeline = draw_pipeline(_draw_typical, &X.plain, 0, push_layout);
		if (pipeline && draw_submit(_draw_typical, pipeline, &X.plain, 0, VK_NULL_HANDLE, push_layout, &result, "typical.push"))
		{
			draw_report("draw.push.typical", &result);
			rep("draw.push.typical.matches_bound_set: %s", yesno(first[_draw_typical].finished && result.hash == first[_draw_typical].hash));
		}
		else
		{
			rep("draw.push.typical.finished: no");
		}
		if (pipeline)
			vkDestroyPipeline(P.device, pipeline, NULL);
		if (push_layout)
			vkDestroyPipelineLayout(P.device, push_layout, NULL);
		if (push_set)
			vkDestroyDescriptorSetLayout(P.device, push_set, NULL);
		guard_end();
	}
	rep("step.draw: done");
}

/* ---------- step 2: a frame on the screen (present) */

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAPCHAIN_IMAGES 16

struct swapchain
{
	VkSwapchainKHR handle;
	VkFormat format;
	VkExtent2D extent;
	VkSurfaceTransformFlagBitsKHR transform;
	uint32_t count;
	VkImage images[MAX_SWAPCHAIN_IMAGES];
	VkImageView views[MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES];
	VkSemaphore render_done[MAX_SWAPCHAIN_IMAGES];
	VkRenderPass render_pass;
};

struct present_state
{
	struct swapchain chain;
	VkCommandPool pool[FRAMES_IN_FLIGHT];
	VkCommandBuffer command[FRAMES_IN_FLIGHT];
	VkFence fence[FRAMES_IN_FLIGHT];
	VkSemaphore acquired[FRAMES_IN_FLIGHT];
	struct buffer marker;
	int mode, modes_available;
	int frame;
	/* what happened, for the report */
	int recreations[8];
	int presented;
	int suboptimal_seen;
	int orientation_ok, orientation_wrong;
	uint64_t last_input;
	/* a loss of the surface, from its detection until a surface and swapchain are made again */
	uint64_t lost_at;
	int lost_attempts;
};

static struct present_state V;

enum { _recreate_start, _recreate_out_of_date, _recreate_suboptimal, _recreate_surface_lost, _recreate_transform,
	_recreate_foreground, _recreate_extent };
static const char *const recreate_names[] = { "start", "out_of_date", "suboptimal", "surface_lost", "transform_changed",
	"foreground", "extent_changed" };

static const char *transform_name(VkSurfaceTransformFlagBitsKHR transform)
{
	switch (transform)
	{
	case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "identity";
	case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "rotate_90";
	case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "rotate_180";
	case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "rotate_270";
	default: return "other";
	}
}

static void swapchain_destroy(struct swapchain *chain)
{
	uint32_t index;

	for (index = 0; index < chain->count; index++)
	{
		if (chain->framebuffers[index])
			vkDestroyFramebuffer(P.device, chain->framebuffers[index], NULL);
		if (chain->views[index])
			vkDestroyImageView(P.device, chain->views[index], NULL);
		if (chain->render_done[index])
			vkDestroySemaphore(P.device, chain->render_done[index], NULL);
	}
	if (chain->render_pass)
		vkDestroyRenderPass(P.device, chain->render_pass, NULL);
	if (chain->handle)
		vkDestroySwapchainKHR(P.device, chain->handle, NULL);
	memset(chain, 0, sizeof(*chain));
}

/* makes the swapchain for the surface as it is now: FIFO, the surface's own transform, the first of
B8G8R8A8 or R8G8B8A8 it lists, one image more than the minimum. Returns 0 (and says why) if it cannot. */
static int swapchain_create(struct swapchain *chain, VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSurfaceFormatKHR formats[32];
	uint32_t format_count = 32, index, count;
	VkResult result;
	int chosen = -1;
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

	result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.physical, P.surface, &caps);
	if (result != VK_SUCCESS)
	{
		rep("present.swapchain: cannot read the surface (%d)", (int)result);
		return 0;
	}
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(P.physical, P.surface, &format_count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return 0;
	for (index = 0; index < format_count && chosen < 0; index++)
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM)
			chosen = (int)index;
	if (chosen < 0)
	{
		rep("present.swapchain: the surface lists neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM");
		return 0;
	}
	if (caps.currentExtent.width == 0xffffffffu)
	{
		int w = 0, h = 0;

		SDL_GetWindowSizeInPixels(P.window, &w, &h);
		caps.currentExtent.width = (uint32_t)w;
		caps.currentExtent.height = (uint32_t)h;
	}
	if (!caps.currentExtent.width || !caps.currentExtent.height)
	{
		rep("present.swapchain: the surface has no size (%ux%u)", caps.currentExtent.width, caps.currentExtent.height);
		return 0;
	}
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
	{
		rep("present.swapchain: the surface cannot take colour attachment and transfer destination usage");
		return 0;
	}
	count = caps.minImageCount + 1;
	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;
	if (count > MAX_SWAPCHAIN_IMAGES)
		count = MAX_SWAPCHAIN_IMAGES;
	if (!(caps.supportedCompositeAlpha & alpha))
		alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR :
			(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR :
			VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	info.surface = P.surface;
	info.minImageCount = count;
	info.imageFormat = formats[chosen].format;
	info.imageColorSpace = formats[chosen].colorSpace;
	info.imageExtent = caps.currentExtent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	memset(chain, 0, sizeof(*chain));
	result = vkCreateSwapchainKHR(P.device, &info, NULL, &chain->handle);
	if (result != VK_SUCCESS)
	{
		rep("present.swapchain: vkCreateSwapchainKHR returned %d", (int)result);
		chain->handle = VK_NULL_HANDLE;
		return 0;
	}
	chain->format = info.imageFormat;
	chain->extent = info.imageExtent;
	chain->transform = caps.currentTransform;
	count = MAX_SWAPCHAIN_IMAGES;
	vkGetSwapchainImagesKHR(P.device, chain->handle, &count, chain->images);
	chain->count = count;
	chain->render_pass = make_render_pass(chain->format, VK_FORMAT_UNDEFINED);
	for (index = 0; index < count; index++)
	{
		VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkFramebufferCreateInfo framebuffer = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

		view.image = chain->images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = chain->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1;
		view.subresourceRange.layerCount = 1;
		CHECK_RESULT(vkCreateImageView(P.device, &view, NULL, &chain->views[index]));
		CHECK_RESULT(vkCreateSemaphore(P.device, &semaphore, NULL, &chain->render_done[index]));
		framebuffer.renderPass = chain->render_pass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &chain->views[index];
		framebuffer.width = chain->extent.width;
		framebuffer.height = chain->extent.height;
		framebuffer.layers = 1;
		CHECK_RESULT(vkCreateFramebuffer(P.device, &framebuffer, NULL, &chain->framebuffers[index]));
	}
	return 1;
}

/* waits for the device and (re)makes the swapchain, logging why and how long it took */
static int present_recreate(int reason, int new_surface)
{
	uint64_t start = now_ns();
	VkSwapchainKHR old = V.chain.handle;
	struct swapchain made;
	/* while the app is away there is no window to make a surface on: the attempts are repeated by the
	loop, without a line each, and the loss is counted and logged once, when it is over */
	int lost = reason == _recreate_surface_lost;
	int ok;

	vkDeviceWaitIdle(P.device);
	if (new_surface)
	{
		swapchain_destroy(&V.chain);
		old = VK_NULL_HANDLE;
		if (P.surface)
			vkDestroySurfaceKHR(P.instance, P.surface, NULL);
		P.surface = VK_NULL_HANDLE;
		if (lost)
			V.lost_attempts++;
		if (!make_surface(lost))
		{
			if (!lost)
				rep("present.recreate: %s: no surface", recreate_names[reason]);
			return 0;
		}
	}
	ok = swapchain_create(&made, old);
	if (old)
	{
		/* the old one is retired by the new one's creation; its images are the application's to wait for, done above */
		struct swapchain retired = V.chain;

		memset(&V.chain, 0, sizeof(V.chain));
		retired.handle = old;
		swapchain_destroy(&retired);
	}
	if (ok)
		V.chain = made;
	V.recreations[reason]++;
	{
		char lost_text[96] = "";

		if (lost && V.lost_at)
		{
			snprintf(lost_text, sizeof(lost_text), ", %.1f ms after the loss, %d attempts", (double)(now_ns() - V.lost_at) / 1e6,
				V.lost_attempts);
			if (ok)
				V.lost_at = 0;
		}
		rep("present.recreate: %s%s, %s, %.2f ms%s (%ux%u, transform %s, %u images)", recreate_names[reason],
			new_surface ? " with a new surface" : "", ok ? "ok" : "failed", (double)(now_ns() - start) / 1e6, lost_text,
			ok ? V.chain.extent.width : 0, ok ? V.chain.extent.height : 0, ok ? transform_name(V.chain.transform) : "-",
			ok ? V.chain.count : 0);
	}
	return ok;
}

/* the surface is gone (found by acquire or by present): counted and logged once, however many tries it takes */
static void surface_lost(const char *where)
{
	if (!V.lost_at)
	{
		V.lost_at = now_ns();
		V.lost_attempts = 0;
		rep("present.surface_lost: %s", where);
	}
	present_recreate(_recreate_surface_lost, 1);
}

static void hue_to_color(double hue, float *rgb)
{
	double h = fmod(hue, 1.0) * 6.0;
	int sector = (int)h;
	double f = h - sector;
	float q = (float)(1.0 - f), t = (float)f;

	switch (sector)
	{
	case 0: rgb[0] = 1; rgb[1] = t; rgb[2] = 0; break;
	case 1: rgb[0] = q; rgb[1] = 1; rgb[2] = 0; break;
	case 2: rgb[0] = 0; rgb[1] = 1; rgb[2] = t; break;
	case 3: rgb[0] = 0; rgb[1] = q; rgb[2] = 1; break;
	case 4: rgb[0] = t; rgb[1] = 0; rgb[2] = 1; break;
	default: rgb[0] = 1; rgb[1] = 0; rgb[2] = q; break;
	}
}

#define MARKER_SIZE 160

/* the marker that shows orientation: a white square in the image's top-left corner with a black one in its own
top-left, so that a picture turned over or squashed shows */
static void marker_fill(uint32_t *pixels)
{
	int x, y;

	for (y = 0; y < MARKER_SIZE; y++)
		for (x = 0; x < MARKER_SIZE; x++)
			pixels[y * MARKER_SIZE + x] = (x < MARKER_SIZE / 2 && y < MARKER_SIZE / 2) ? 0xff000000u : 0xffffffffu;
}

static void present_record(VkCommandBuffer command, uint32_t image_index, const float rgb[3])
{
	struct swapchain *chain = &V.chain;
	VkImage image = chain->images[image_index];
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkClearValue clear;
	VkClearAttachment attachments[2];
	VkClearRect rects[2];
	uint32_t size = MARKER_SIZE < chain->extent.height ? MARKER_SIZE : chain->extent.height;

	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command, &begin);
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = rgb[0];
	clear.color.float32[1] = rgb[1];
	clear.color.float32[2] = rgb[2];
	clear.color.float32[3] = 1.0f;
	memset(attachments, 0, sizeof(attachments));
	memset(rects, 0, sizeof(rects));
	attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	attachments[0].clearValue.color.float32[0] = 1.0f;
	attachments[0].clearValue.color.float32[1] = 1.0f;
	attachments[0].clearValue.color.float32[2] = 1.0f;
	attachments[0].clearValue.color.float32[3] = 1.0f;
	attachments[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	attachments[1].clearValue.color.float32[3] = 1.0f;
	rects[0].rect.extent.width = size;
	rects[0].rect.extent.height = size;
	rects[0].layerCount = 1;
	rects[1].rect.extent.width = size / 2;
	rects[1].rect.extent.height = size / 2;
	rects[1].layerCount = 1;
	if (V.mode == 0)
	{
		/* no render pass: a whole-image clear, and the marker by a copy from a buffer */
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkBufferImageCopy copy;

		image_barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.color, 1, &range);
		memory_barrier(command);
		memset(&copy, 0, sizeof(copy));
		copy.bufferRowLength = MARKER_SIZE;
		copy.bufferImageHeight = MARKER_SIZE;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = size;
		copy.imageExtent.height = size;
		copy.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(command, V.marker.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		image_barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	else if (V.mode == 1 && P.dynamic_rendering)
	{
		VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };

		image_barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		color.imageView = chain->views[image_index];
		color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color.clearValue = clear;
		rendering.renderArea.extent = chain->extent;
		rendering.layerCount = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments = &color;
		vkCmdBeginRendering(command, &rendering);
		rects[0].rect.extent.width = size;
		vkCmdClearAttachments(command, 1, &attachments[0], 1, &rects[0]);
		vkCmdClearAttachments(command, 1, &attachments[1], 1, &rects[1]);
		vkCmdEndRendering(command);
		image_barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	else
	{
		VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };

		pass.renderPass = chain->render_pass;
		pass.framebuffer = chain->framebuffers[image_index];
		pass.renderArea.extent = chain->extent;
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdClearAttachments(command, 1, &attachments[0], 1, &rects[0]);
		vkCmdClearAttachments(command, 1, &attachments[1], 1, &rects[1]);
		vkCmdEndRenderPass(command);
		/* the pass's own final layout is colour attachment: to present */
		image_barrier(command, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	vkEndCommandBuffer(command);
}

static const char *mode_name(int mode)
{
	return mode == 0 ? "vkCmdClearColorImage (no render pass)" : mode == 1 && P.dynamic_rendering ? "dynamic rendering" : "render pass";
}

/* the pacing of one window of presents, from the time each present call returned */
static void report_pacing(const char *label, const double *intervals, int count)
{
	double sorted[1024], median;
	int index, slow = 0, n = count < 1024 ? count : 1024;

	if (n < 3)
		return;
	memcpy(sorted, intervals, sizeof(double) * (size_t)n);
	qsort(sorted, (size_t)n, sizeof(double), compare_doubles);
	median = sorted[n / 2];
	for (index = 0; index < n; index++)
		if (intervals[index] > 1.5 * median)
			slow++;
	rep("present.pacing.%s: %d intervals, min %.3f ms, median %.3f ms (%.1f Hz), max %.3f ms, %d over 1.5x the median", label, n,
		sorted[0], median, 1000.0 / median, sorted[n - 1], slow);
}

static void present_tone(SDL_AudioStream **out)
{
	SDL_AudioSpec spec;
	float samples[48000];
	int index;

	spec.format = SDL_AUDIO_F32;
	spec.channels = 1;
	spec.freq = 48000;
	*out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (!*out)
	{
		rep("present.audio: the stream did not open (%s)", SDL_GetError());
		return;
	}
	for (index = 0; index < 48000; index++)
		samples[index] = 0.2f * sinf(2.0f * 3.14159265f * 440.0f * (float)index / 48000.0f);
	SDL_PutAudioStreamData(*out, samples, sizeof(samples));
	SDL_ResumeAudioStreamDevice(*out);
	rep("present.audio: a one-second tone queued on the default device (the tester says if it was heard)");
}

static void step_present(void)
{
	SDL_AudioStream *audio = NULL;
	SDL_Gamepad *gamepad = NULL;
	double intervals[1024];
	int interval_count = 0, running = 1, background = 0, input_events = 0, index;
	uint64_t last_present = 0, window_start, last_caps_check, begun;
	double all_intervals_sum = 0.0;
	int all_intervals = 0;
	const SDL_DisplayMode *display;
	VkSurfaceTransformFlagBitsKHR seen_transforms = 0;
	int buttons[32] = { 0 };

	rep("step.present: start");
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
		rep("present.sdl: SDL_InitSubSystem(gamepad, audio) failed: %s", SDL_GetError());
	display = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(P.window));
	if (display)
		rep("present.display: %dx%d at %.2f Hz (SDL_GetCurrentDisplayMode)", display->w, display->h, display->refresh_rate);
	memset(&V, 0, sizeof(V));
	for (index = 0; index < FRAMES_IN_FLIGHT; index++)
	{
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		if (!make_command_pool(&V.pool[index]) || !allocate_command(V.pool[index], &V.command[index]) ||
			!make_fence(&V.fence[index], 1) || !CHECK_RESULT(vkCreateSemaphore(P.device, &semaphore, NULL, &V.acquired[index])))
		{
			rep("step.present: failed (frame objects)");
			return;
		}
	}
	if (!make_buffer(&V.marker, MARKER_SIZE * MARKER_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1))
		return;
	marker_fill(V.marker.map);
	V.modes_available = P.dynamic_rendering ? 3 : 2;
	if (!present_recreate(_recreate_start, 0))
	{
		rep("step.present: failed (no swapchain)");
		goto cleanup;
	}
	seen_transforms |= V.chain.transform;
	rep("present.script: Watch the colour cycle, with a white square (black inside) at the top-left corner.");
	rep("present.script: Press Home and come back, ten times. Turn the device over and back.");
	rep("present.script: Press A if the picture looks right, Y if upside down or squashed, X to change the way it is cleared, B to end.");
	rep("present.mode: %s", mode_name(V.mode));
	present_tone(&audio);
	{
		int count = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&count);

		rep("present.gamepads: %d", count);
		if (ids && count > 0)
		{
			gamepad = SDL_OpenGamepad(ids[0]);
			rep("present.gamepad_open: %s (%s)", gamepad ? "yes" : "no", gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetError());
		}
		SDL_free(ids);
	}
	begun = window_start = last_caps_check = V.last_input = now_ns();
	while (running)
	{
		SDL_Event event;
		VkSurfaceCapabilitiesKHR caps;
		uint32_t image_index = 0;
		VkResult result;
		int frame = V.frame % FRAMES_IN_FLIGHT;
		float rgb[3];

		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_EVENT_WILL_ENTER_BACKGROUND:
				rep("present.event: will_enter_background (t %.2f s)", (double)(now_ns() - begun) / 1e9);
				background = 1;
				{
					uint64_t start = now_ns();

					vkDeviceWaitIdle(P.device);
					swapchain_destroy(&V.chain);
					if (P.surface)
						vkDestroySurfaceKHR(P.instance, P.surface, NULL);
					P.surface = VK_NULL_HANDLE;
					rep("present.background: swapchain and surface destroyed in %.2f ms", (double)(now_ns() - start) / 1e6);
				}
				break;
			case SDL_EVENT_DID_ENTER_BACKGROUND: rep("present.event: did_enter_background"); break;
			case SDL_EVENT_WILL_ENTER_FOREGROUND: rep("present.event: will_enter_foreground"); break;
			case SDL_EVENT_DID_ENTER_FOREGROUND:
				rep("present.event: did_enter_foreground (t %.2f s)", (double)(now_ns() - begun) / 1e9);
				if (background)
				{
					background = 0;
					if (present_recreate(_recreate_foreground, 1))
						seen_transforms |= V.chain.transform;
				}
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: rep("present.event: window pixel size %dx%d", event.window.data1, event.window.data2); break;
			case SDL_EVENT_DISPLAY_ORIENTATION: rep("present.event: display orientation %d", (int)event.display.data1); break;
			case SDL_EVENT_WINDOW_EXPOSED: rep("present.event: window exposed"); break;
			case SDL_EVENT_WINDOW_HIDDEN: rep("present.event: window hidden"); break;
			case SDL_EVENT_WINDOW_SHOWN: rep("present.event: window shown"); break;
			case SDL_EVENT_TERMINATING: rep("present.event: terminating"); running = 0; break;
			case SDL_EVENT_GAMEPAD_ADDED:
				rep("present.event: gamepad added");
				if (!gamepad)
					gamepad = SDL_OpenGamepad(event.gdevice.which);
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_KEY_DOWN:
			{
				const char *name = "?";
				int action = 0; /* 1 right, 2 wrong, 3 mode, 4 end */

				if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
				{
					name = SDL_GetGamepadStringForButton((SDL_GamepadButton)event.gbutton.button);
					action = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH ? 2 :
						event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST ? 3 : event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ? 4 : 0;
					if (event.gbutton.button < 32)
						buttons[event.gbutton.button]++;
					rep("present.input: gamepad button %s", name ? name : "?");
				}
				else
				{
					name = SDL_GetScancodeName(event.key.scancode);
					action = event.key.scancode == SDL_SCANCODE_A ? 1 : event.key.scancode == SDL_SCANCODE_Y ? 2 :
						event.key.scancode == SDL_SCANCODE_X ? 3 : event.key.scancode == SDL_SCANCODE_B ? 4 : 0;
					rep("present.input: key %s (scancode %d)", name, (int)event.key.scancode);
				}
				input_events++;
				V.last_input = now_ns();
				if (action == 1)
				{
					V.orientation_ok++;
					rep("present.tester: picture looks right");
				}
				else if (action == 2)
				{
					V.orientation_wrong++;
					rep("present.tester: picture upside down or squashed (transform %s)", transform_name(V.chain.transform));
				}
				else if (action == 3)
				{
					V.mode = (V.mode + 1) % V.modes_available;
					rep("present.mode: %s", mode_name(V.mode));
				}
				else if (action == 4)
				{
					rep("present.tester: B, ending the step");
					running = 0;
				}
				break;
			}
			default: break;
			}
		}
		if (!running)
			break;
		if (now_ns() - V.last_input > 120ull * 1000000000ull)
		{
			rep("present.tester: no input for 120 s, ending the step on its own");
			break;
		}
		if (background || !V.chain.handle)
		{
			struct timespec wait = { 0, 50 * 1000000 };

			nanosleep(&wait, NULL);
			if (!background && !V.chain.handle)
				surface_lost("no swapchain");
			continue;
		}
		/* the surface as it is: a transform or a size that is not the swapchain's means a new one */
		if (now_ns() - last_caps_check > 250000000ull)
		{
			last_caps_check = now_ns();
			if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.physical, P.surface, &caps) == VK_SUCCESS)
			{
				if (caps.currentTransform != V.chain.transform)
				{
					rep("present.transform: %s -> %s (swapchain not yet re-made; suboptimal reported so far: %s)",
						transform_name(V.chain.transform), transform_name(caps.currentTransform), yesno(V.suboptimal_seen));
					seen_transforms |= caps.currentTransform;
					if (present_recreate(_recreate_transform, 0))
						seen_transforms |= V.chain.transform;
				}
				else if (caps.currentExtent.width != 0xffffffffu &&
					(caps.currentExtent.width != V.chain.extent.width || caps.currentExtent.height != V.chain.extent.height))
				{
					present_recreate(_recreate_extent, 0);
				}
			}
		}
		result = vkWaitForFences(P.device, 1, &V.fence[frame], VK_TRUE, WAIT_FOREVER);
		if (result != VK_SUCCESS)
		{
			rep("error: the frame fence did not signal (%d)", (int)result);
			break;
		}
		result = vkAcquireNextImageKHR(P.device, V.chain.handle, WAIT_FOREVER, V.acquired[frame], VK_NULL_HANDLE, &image_index);
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			present_recreate(_recreate_out_of_date, 0);
			continue;
		}
		if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			surface_lost("vkAcquireNextImageKHR");
			continue;
		}
		if (result == VK_SUBOPTIMAL_KHR)
			V.suboptimal_seen++;
		else if (result != VK_SUCCESS)
		{
			rep("error: vkAcquireNextImageKHR returned %d", (int)result);
			break;
		}
		{
			double hue = (double)(now_ns() - begun) / 6e9;

			hue_to_color(hue, rgb);
		}
		vkResetFences(P.device, 1, &V.fence[frame]);
		vkResetCommandPool(P.device, V.pool[frame], 0);
		present_record(V.command[frame], image_index, rgb);
		{
			VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };

			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &V.acquired[frame];
			submit.pWaitDstStageMask = &wait_stage;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &V.command[frame];
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &V.chain.render_done[image_index];
			if (!CHECK_RESULT(vkQueueSubmit(P.queue, 1, &submit, V.fence[frame])))
				break;
			present.waitSemaphoreCount = 1;
			present.pWaitSemaphores = &V.chain.render_done[image_index];
			present.swapchainCount = 1;
			present.pSwapchains = &V.chain.handle;
			present.pImageIndices = &image_index;
			result = vkQueuePresentKHR(P.queue, &present);
		}
		{
			uint64_t now = now_ns();

			if (last_present && interval_count < 1024)
			{
				intervals[interval_count++] = (double)(now - last_present) / 1e6;
				all_intervals_sum += (double)(now - last_present) / 1e6;
				all_intervals++;
			}
			last_present = now;
		}
		V.frame++;
		if (result == VK_SUCCESS)
			V.presented++;
		else if (result == VK_SUBOPTIMAL_KHR)
		{
			V.presented++;
			V.suboptimal_seen++;
			if (present_recreate(_recreate_suboptimal, 0))
				seen_transforms |= V.chain.transform;
			last_present = 0;
		}
		else if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			present_recreate(_recreate_out_of_date, 0);
			last_present = 0;
		}
		else if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			surface_lost("vkQueuePresentKHR");
			last_present = 0;
		}
		else
		{
			rep("error: vkQueuePresentKHR returned %d", (int)result);
			break;
		}
		if (now_ns() - window_start > 10ull * 1000000000ull)
		{
			char label[32];

			snprintf(label, sizeof(label), "t%.0fs", (double)(now_ns() - begun) / 1e9);
			report_pacing(label, intervals, interval_count);
			interval_count = 0;
			window_start = now_ns();
		}
	}
	report_pacing("last_window", intervals, interval_count);
	rep("present.presented_frames: %d", V.presented);
	for (index = 0; index < 7; index++)
		if (V.recreations[index])
			rep("present.recreations.%s: %d", recreate_names[index], V.recreations[index]);
	rep("present.transforms_seen: identity %s, rotate_90 %s, rotate_180 %s, rotate_270 %s",
		yesno(seen_transforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR), yesno(seen_transforms & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR),
		yesno(seen_transforms & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR), yesno(seen_transforms & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR));
	rep("present.suboptimal_reported: %d times", V.suboptimal_seen);
	rep("present.tester_answers: right %d, wrong %d; input events %d; sdl input arrived: %s", V.orientation_ok, V.orientation_wrong,
		input_events, yesno(input_events > 0));
	if (all_intervals)
		rep("present.mean_interval: %.3f ms over the whole step", all_intervals_sum / all_intervals);
cleanup:
	vkDeviceWaitIdle(P.device);
	swapchain_destroy(&V.chain);
	free_buffer(&V.marker);
	for (index = 0; index < FRAMES_IN_FLIGHT; index++)
	{
		if (V.acquired[index])
			vkDestroySemaphore(P.device, V.acquired[index], NULL);
		if (V.fence[index])
			vkDestroyFence(P.device, V.fence[index], NULL);
		if (V.pool[index])
			vkDestroyCommandPool(P.device, V.pool[index], NULL);
	}
	if (gamepad)
		SDL_CloseGamepad(gamepad);
	if (audio)
		SDL_DestroyAudioStream(audio);
	rep("step.present: done");
}


/* ---------- step 7: the game's shaders (shaders), phase 4 of port/android/VULKAN.md

Every .vert and .frag (not the .gl.* twins) in the folder debug.gpu_dump_shaders names, which the Vulkan image's
generators wrote while the game met them, is compiled with glslang and made a shader module. With the validation
layer on, which checks each module's SPIR-V, a warning or an error while a module is made is a failure. The report
says how many, the time of each stage, and the slowest ten of each kind. */

struct shader_time
{
	char name[96];
	double compile, module;
};

static void shader_slowest(const char *what, struct shader_time *times, int count, int by_module)
{
	int rank, index;

	for (rank = 0; rank < 10 && rank < count; rank++)
	{
		int best = -1;

		for (index = 0; index < count; index++)
		{
			double value = by_module ? times[index].module : times[index].compile;

			if (value < 0)
				continue;
			if (best < 0 || value > (by_module ? times[best].module : times[best].compile))
				best = index;
		}
		if (best < 0)
			break;
		rep("shaders.slowest.%s.%d: %s %.3f ms", what, rank + 1, times[best].name,
			by_module ? times[best].module : times[best].compile);
		if (by_module)
			times[best].module = -1;
		else
			times[best].compile = -1;
	}
}

static void step_shaders(void)
{
	char folder[512];
	DIR *directory;
	struct dirent *entry;
	struct shader_time *times[2] = { NULL, NULL };
	int count[2] = { 0, 0 }, capacity[2] = { 0, 0 }, failed[2] = { 0, 0 }, complained[2] = { 0, 0 };
	double compile_total[2] = { 0, 0 }, module_total[2] = { 0, 0 };
	int stage_index, reported = 0;
	uint64_t start = now_ns();

	rep("step.shaders: start");
	config_string_setting("debug.gpu_dump_shaders", folder, sizeof(folder));
	if (!folder[0])
	{
		rep("step.shaders: skipped (debug.gpu_dump_shaders names no folder)");
		return;
	}
	if (!glslang_load())
	{
		rep("step.shaders: failed (glslang did not load)");
		return;
	}
	directory = opendir(folder);
	if (!directory)
	{
		rep("step.shaders: failed (cannot open %s: %s)", folder, strerror(errno));
		return;
	}
	rep("shaders.folder: %s", folder);
	rep("shaders.validation_layer: %s", P.validation ? "on (it validates each module's SPIR-V)" : "off (modules are made, not validated)");
	while ((entry = readdir(directory)) != NULL)
	{
		const char *name = entry->d_name;
		size_t length = strlen(name);
		int fragment, complaints_before, file_failed = 0;
		char path[1024];
		char *text;
		FILE *file;
		long size;
		struct spirv code;
		uint64_t wall = 0, module_start;
		VkShaderModule module;
		struct shader_time *time;

		if (length < 6 || strstr(name, ".gl.") || (strcmp(name + length - 5, ".vert") && strcmp(name + length - 5, ".frag")))
			continue;
		fragment = name[length - 1] == 'g';
		snprintf(path, sizeof(path), "%s/%s", folder, name);
		file = fopen(path, "rb");
		if (!file)
			continue;
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		text = malloc((size_t)size + 1);
		if (!text || fread(text, 1, (size_t)size, file) != (size_t)size)
		{
			fclose(file);
			free(text);
			continue;
		}
		text[size] = 0;
		fclose(file);
		if (count[fragment] == capacity[fragment])
		{
			capacity[fragment] = capacity[fragment] ? capacity[fragment] * 2 : 256;
			times[fragment] = realloc(times[fragment], sizeof(*times[fragment]) * (size_t)capacity[fragment]);
		}
		time = &times[fragment][count[fragment]++];
		snprintf(time->name, sizeof(time->name), "%s", name);
		time->compile = time->module = 0;
		if (!glslang_compile(text, fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX, &code, &wall, NULL, 1))
		{
			/* said again, loudly, for the log */
			failed[fragment]++;
			rep("shaders.FAILED.%s: glslang did not compile it", name);
			glslang_compile(text, fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX, &code, &wall, NULL, 0);
			free(text);
			continue;
		}
		free(text);
		time->compile = (double)wall / 1e6;
		compile_total[fragment] += time->compile;
		complaints_before = validation_complaints;
		module_start = now_ns();
		module = make_shader_module(&code);
		time->module = (double)(now_ns() - module_start) / 1e6;
		module_total[fragment] += time->module;
		if (module == VK_NULL_HANDLE)
		{
			file_failed = 1;
			rep("shaders.FAILED.%s: vkCreateShaderModule failed", name);
		}
		else
		{
			vkDestroyShaderModule(P.device, module, NULL);
		}
		if (validation_complaints != complaints_before)
		{
			complained[fragment]++;
			file_failed = 1;
			rep("shaders.FAILED.%s: the validation layer complained while the module was made (see validation.message above)", name);
		}
		failed[fragment] += file_failed;
		free(code.words);
		reported++;
	}
	closedir(directory);
	for (stage_index = 0; stage_index < 2; stage_index++)
	{
		const char *what = stage_index ? "fragment" : "vertex";
		int n = count[stage_index];

		rep("shaders.%s.count: %d compiled and made without a complaint, %d failed (%d of them with a validation complaint)", what, n - failed[stage_index], failed[stage_index], complained[stage_index]);
		if (!n)
			continue;
		rep("shaders.%s.glslang_total: %.1f ms, average %.3f ms", what, compile_total[stage_index], compile_total[stage_index] / n);
		rep("shaders.%s.vkCreateShaderModule_total: %.1f ms, average %.3f ms", what, module_total[stage_index], module_total[stage_index] / n);
		shader_slowest(what, times[stage_index], n, 0);
		shader_slowest(what, times[stage_index], n, 1);
		free(times[stage_index]);
	}
	rep("shaders.wall: %.1f s for %d shaders", (double)(now_ns() - start) / 1e9, reported);
	rep("step.shaders: done");
}

/* ---------- running it */

static const struct { const char *name; void (*run)(void); } step_table[] = {
	{ "caps", step_caps },
	{ "memory", step_memory },
	{ "compile", step_compile },
	{ "pipelines", step_pipelines },
	{ "draw", step_draw },
	{ "present", step_present },
	{ "shaders", step_shaders },
};
#define STEP_COUNT ((int)(sizeof(step_table) / sizeof(step_table[0])))

/* the steps (or sub-steps) a run that died left behind, one name a line */
static char skip_path[640], running_path[640];

static int in_list(const char *list, const char *name)
{
	size_t length = strlen(name);
	const char *at = list;

	while (list && (at = strstr(at, name)))
	{
		int start = at == list || at[-1] == ',' || at[-1] == '\n' || at[-1] == ' ';
		int end = at[length] == 0 || at[length] == ',' || at[length] == '\n' || at[length] == ' ';

		if (start && end)
			return 1;
		at += length;
	}
	return 0;
}

static char *read_file_text(const char *path)
{
	FILE *file = fopen(path, "rb");
	char *text;
	long size;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = calloc(1, (size_t)size + 1);
	if (size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size)
		text[0] = 0;
	fclose(file);
	return text;
}

/* the step being run, which the running marker goes back to when a sub-step's guard ends: a crash
later in the same step must still name the step */
static char current_step[64];

/* what is being done, for a run that dies in it; empty when nothing is */
static void mark_running(const char *name)
{
	if (name)
	{
		FILE *file = fopen(running_path, "wb");

		if (file)
		{
			fputs(name, file);
			fclose(file);
		}
	}
	else
	{
		remove(running_path);
	}
}

/* a risky sub-step: true if it may run (it did not kill the probe before) */
static int guard_begin(const char *name)
{
	char *skipped = read_file_text(skip_path);
	int skip = in_list(skipped, name);

	free(skipped);
	if (skip)
	{
		rep("guard.%s: skipped (it killed the probe once; delete vk_probe_skip.txt to try again)", name);
		return 0;
	}
	mark_running(name);
	return 1;
}

static void guard_end(void)
{
	mark_running(current_step[0] ? current_step : NULL);
}

/* which GPU-related libraries the process has loaded, to show that only the chosen driver is in it (the system
loader is one only with the phone's own driver, or through libadrenotools' private copy of it) */
static int report_library(struct dl_phdr_info *info, size_t size, void *data)
{
	static const char *const interesting[] = { "vulkan", "adreno", "vk_", "EGL", "GLES", "adrenotools", "_hook",
		"hook_impl", "VkLayer", "turnip", "freedreno", "gsl", "kgsl", "libmain", "libglslang" };
	size_t index;

	(void)size;
	(void)data;
	if (!info->dlpi_name || !info->dlpi_name[0])
		return 0;
	for (index = 0; index < sizeof(interesting) / sizeof(interesting[0]); index++)
	{
		if (strstr(info->dlpi_name, interesting[index]))
		{
			rep("driver.loaded_library: %s", info->dlpi_name);
			break;
		}
	}
	return 0;
}

static void probe_teardown(void)
{
	if (P.device)
	{
		int index;

		vkDeviceWaitIdle(P.device);
		draw_check_free();
		draw_context_free();
		simple_free();
		pipe_context_free();
		for (index = 0; index < 4; index++)
			if (pipeline_passes[index].pass)
				vkDestroyRenderPass(P.device, pipeline_passes[index].pass, NULL);
		if (main_fence)
			vkDestroyFence(P.device, main_fence, NULL);
		if (main_pool)
			vkDestroyCommandPool(P.device, main_pool, NULL);
		vkDestroyDevice(P.device, NULL);
	}
	if (P.surface)
		vkDestroySurfaceKHR(P.instance, P.surface, NULL);
	if (P.messenger && vkDestroyDebugUtilsMessengerEXT)
		vkDestroyDebugUtilsMessengerEXT(P.instance, P.messenger, NULL);
	if (P.instance)
		vkDestroyInstance(P.instance, NULL);
	if (P.window)
		SDL_DestroyWindow(P.window);
	glslang_unload();
}

void host_vk_probe_run(const char *steps, const char *data_root, const char *vk_driver)
{
	char *crashed, *skipped;
	int index, setup = 0;

	snprintf(P.data_root, sizeof(P.data_root), "%s", data_root);
	snprintf(P.driver_setting, sizeof(P.driver_setting), "%s", vk_driver ? vk_driver : "");
	snprintf(report_path, sizeof(report_path), "%s/vk_probe.txt", data_root);
	snprintf(skip_path, sizeof(skip_path), "%s/vk_probe_skip.txt", data_root);
	snprintf(running_path, sizeof(running_path), "%s/vk_probe_running.txt", data_root);
	report_file = fopen(report_path, "wb");
	if (!report_file)
		host_logf(HOST_LOG_ERROR, "vk probe: cannot write %s: %s", report_path, strerror(errno));
	rep("probe.start: %s", steps);
	report_device();
	rep("probe.steps_asked: %s", steps);

	/* a step that died last time is left out this time */
	crashed = read_file_text(running_path);
	if (crashed && crashed[0])
	{
		FILE *file = fopen(skip_path, "ab");

		rep("probe.crashed_last_time: %s (left out from now on)", crashed);
		if (file)
		{
			fprintf(file, "%s\n", crashed);
			fclose(file);
		}
	}
	free(crashed);
	mark_running(NULL);

	if (create_instance() && create_surface() && select_device())
	{
		query_extensions_and_features();
		if (create_device() && make_command_pool(&main_pool) && allocate_command(main_pool, &main_command) &&
			make_fence(&main_fence, 0))
			setup = 1;
	}
	{
		/* the system loader loads the driver only at the instance; a custom one that the hook refused is silently
		the phone's, and a file that is no driver leaves the loader with none */
		char check[256];
		int check_ok = host_vk_driver_verify(check, sizeof(check));

		rep("driver.check: %s (%s)", check_ok ? "ok" : "FAILED", check);
		dl_iterate_phdr(report_library, NULL);
	}
	if (!setup)
	{
		rep("probe.setup: failed");
	}
	else
	{
		skipped = read_file_text(skip_path);
		rep("probe.setup: ok");
		for (index = 0; index < STEP_COUNT; index++)
		{
			int asked = !strcmp(steps, "all") || in_list(steps, step_table[index].name);

			/* caps is the report's base: it is run whatever is asked for, and costs nothing */
			if (!asked && index != 0)
				continue;
			if (in_list(skipped, step_table[index].name))
			{
				rep("step.%s: skipped (it killed the probe once; delete vk_probe_skip.txt to try again)",
					step_table[index].name);
				continue;
			}
			snprintf(current_step, sizeof(current_step), "%s", step_table[index].name);
			mark_running(current_step);
			step_table[index].run();
			current_step[0] = 0;
			mark_running(NULL);
		}
		free(skipped);
	}
	probe_teardown();
	rep("validation.summary: %d messages, %d errors", validation_messages, validation_errors);
	rep("probe.done: %s", setup ? "yes" : "no");
	/* last, and guarded: unloading a driver is one more thing that can go wrong */
	if (guard_begin("driver_close"))
	{
		host_vk_driver_close();
		guard_end();
		rep("driver.closed: yes");
	}
	mark_running(NULL);
	if (report_file)
		fclose(report_file);
	host_exit(0);
}

