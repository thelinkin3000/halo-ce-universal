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

#include "../guest/vk_commands.h"

#include <pthread.h>

/* the instance-level entry points, loaded through the driver's
vkGetInstanceProcAddr; each is the core function, or the extension's where the
instance is 1.0-level and the extension stands in for it */
#define HOST_VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
	X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceProperties2) X(vkGetDeviceProcAddr) \
	X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkCreateDevice) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR)

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
	/* the log line of the decision: which driver and device, for the device-lost report */
	char line[600];

#define X(name) PFN_##name name;
	HOST_VK_INSTANCE_FUNCTIONS(X)
#undef X
	PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
	PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
};

/* valid after host_vk_startup returned 1; zeroed otherwise */
extern struct host_vk host_vk;

/* ---------- the backend (host_vk_render.c, host_vk_present.c)

Made at the first host_vk_submit, from what host_vk_startup kept. */

/* the device-level entry points: hidden, so that the library exports no symbol named
as a Vulkan function */
#define HOST_VK_DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueuePresentKHR) \
	X(vkAllocateMemory) X(vkFreeMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
	X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdPipelineBarrier) \
	X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdBlitImage) X(vkCmdSetViewport) X(vkCmdSetScissor) \
	X(vkCmdBindPipeline) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
	X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
	X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) \
	X(vkUnmapMemory) X(vkCmdCopyImageToBuffer)

#define X(name) extern PFN_##name name __attribute__((visibility("hidden")));
HOST_VK_DEVICE_FUNCTIONS(X)
#undef X
/* dynamic rendering: core in 1.3, the extension's before */
extern PFN_vkCmdBeginRenderingKHR host_vk_cmd_begin_rendering __attribute__((visibility("hidden")));
extern PFN_vkCmdEndRenderingKHR host_vk_cmd_end_rendering __attribute__((visibility("hidden")));

#define HOST_VK_FRAMES 2
#define HOST_VK_SWAPCHAIN_IMAGES 16
#define HOST_VK_TARGET_BUCKETS 64

/* a render target or depth buffer of the game, as an image */
struct host_vk_target
{
	struct vk_surface key;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkImageLayout layout;
	VkImageAspectFlags aspect;
	struct host_vk_target *next_in_bucket;
};

struct host_vk_frame
{
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	int recording; /* the command buffer is open */
	int submitted; /* the fence is (to be) signalled by submission number */
	uint64_t number;
};

struct host_vk_swapchain
{
	VkSwapchainKHR handle;
	VkFormat format;
	VkExtent2D extent;
	uint32_t count;
	VkImage images[HOST_VK_SWAPCHAIN_IMAGES];
	VkSemaphore render_done[HOST_VK_SWAPCHAIN_IMAGES];
	VkImageView views[HOST_VK_SWAPCHAIN_IMAGES]; /* only with debug.vk_present_marker */
};

struct host_vk_backend
{
	int state; /* 0 not made yet, 1 ready, 2 not available (said why) */
	int dead; /* the device was lost: nothing more is recorded */
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkSurfaceKHR surface;
	void *native_window; /* the ANativeWindow the surface was made on */
	VkPhysicalDeviceMemoryProperties memory;
	VkFormat color_format, depth_format;
	VkFilter blit_filter;
	VkFormatFeatureFlags swapchain_features;

	struct host_vk_frame frames[HOST_VK_FRAMES];
	int frame;
	uint64_t submission; /* the number of the latest submission */
	uint64_t retired; /* the highest number seen retired */
	pthread_mutex_t lock;

	struct host_vk_target *buckets[HOST_VK_TARGET_BUCKETS];
	unsigned images;
	struct host_vk_target *color, *depth; /* bound now (NULL: none) */
	struct host_vk_target *last_color, *last_depth; /* the pair a command last named, for the statistics */
	int rendering; /* a rendering is open on them */
	uint32_t area_width, area_height; /* its render area */

	struct host_vk_swapchain chain;
	uint64_t lost_at; /* from the loss of the surface until it is made again */
	unsigned lost_attempts;
	unsigned presented_since_lost;

	/* the built-in pipeline of clears of some channels only */
	VkPipelineLayout clear_layout;
	VkShaderModule clear_vertex, clear_fragment;
	VkPipeline clear_pipelines[16][2]; /* by colour write mask, and whether a depth buffer is bound */

	/* counted for the log line every 60 frames, and in total */
	struct host_vk_counts
	{
		unsigned frames, hand_overs, targets, clears, presents, clears_drawn, clears_attachments, target_changes;
		unsigned recreations;
	} counts;
};

extern struct host_vk_backend host_vkb;

/* host_vk_render.c: records a command buffer and logs; check() reports a failed call and
stops the backend if the device was lost */
int host_vk_check(VkResult result, const char *call);
#define HOST_VK_CHECK(call) host_vk_check((call), #call)
void host_vk_wait_fence(VkFence fence, const char *what, uint64_t number);
/* the transition of a target to a layout (outside a rendering) */
void host_vk_target_transition(VkCommandBuffer command, struct host_vk_target *target, VkImageLayout layout);
/* ends the open rendering, if any */
void host_vk_rendering_end(void);
/* the target for a surface of a command, made (and cleared) the first time; NULL if there is none */
struct host_vk_target *host_vk_target_get(const struct vk_surface *surface);
/* the current frame's command buffer, open for recording */
VkCommandBuffer host_vk_frame_command(void);

/* host_vk_present.c */
/* the surface and swapchain for the window as it is now; 0 if there is no window to make one on */
int host_vk_surface_make(int quiet);
/* the loss of the surface or of the swapchain's use: counted and logged once, however many tries it takes */
void host_vk_surface_lost(const char *where);
/* called with the frame's command buffer open, after the game's rendering: acquires a swapchain image
and records the blit of the back buffer into it. Returns the image's index, or UINT32_MAX when there is
nothing to present to (no swapchain, surface lost) */
uint32_t host_vk_present_record(VkCommandBuffer command, struct host_vk_target *back_buffer);
/* after the frame was submitted: presents the image acquired by host_vk_present_record */
void host_vk_present_queue(uint32_t image_index);
void host_vk_present_destroy(void);

#endif
