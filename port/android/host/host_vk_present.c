/*
HOST_VK_PRESENT.C

The Vulkan renderer's surface and swapchain (port/android/VULKAN.md, phase 2),
on the game's window: the surface is made with vkCreateAndroidSurfaceKHR on the
window's ANativeWindow (no SDL_Vulkan_* call: SDL would load the phone's driver
beside the chosen one), the swapchain is FIFO with the surface's own images'
count plus two, and a frame is shown by drawing the game's back buffer,
letterboxed, into the acquired image: one rendering, cleared black as it begins
(the bars), with one triangle over the picture's rectangle that samples the
back buffer. On a tiler that is one write of the image, where clearing it and
blitting into it, as was done before, wrote it twice and read it once more; a
driver that cannot make that pass, or a back buffer that cannot be sampled,
has the clear and the blit still.

Android destroys the window's ANativeWindow when the app goes to the background
and makes a new one on return; SDL's lifecycle events do not reach the game's
loop (phase 0), so the loss is found from what the calls return and from the
window's ANativeWindow changing. While there is no swapchain a frame's work is
still submitted (its fence and number move on) and nothing goes to the screen.
*/

#include "host.h"
#include "host_vk.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define B host_vkb

#define ACQUIRE_TIMEOUT_NS 1000000000ull
/* how often (in presented frames) the surface's size is looked at */
#define EXTENT_CHECK_FRAMES 60

static uint64_t now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

/* the window's ANativeWindow as it is now (a new one after the app was away), or NULL */
static void *native_window(void)
{
#ifdef HALO_PS5
	/* (the console has no window: the surface is VideoOut's display plane, below) */
	return NULL;
#else
	if (!host_vk_window)
		return NULL;
	return SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)host_vk_window),
		SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
#endif
}

#ifdef HALO_PS5
/* The PS5's surface (port/ps5/PS5.md, phase 4): a plane on VideoOut's one display, in its one mode (3840x2160 at
59.94 Hz), as phase 0 B's host probe made it. Nothing here waits for a window, so a surface made again after a loss
is made at once; a failure is said the first time, quiet or not, since there is no window to wait for. */
static int plane_failed(VkResult result, const char *call)
{
	static int said;

	if (!said++)
		host_logf(HOST_LOG_ERROR, "vk: no display plane surface: %s returned %d", call, (int)result);
	return 0;
}

int host_vk_surface_make(int quiet)
{
	VkDisplayPropertiesKHR display;
	VkDisplayModePropertiesKHR mode;
	VkDisplayPlanePropertiesKHR plane;
	VkDisplayPlaneCapabilitiesKHR capabilities;
	VkDisplaySurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
	uint32_t count = 1;
	VkResult result;

	(void)quiet;
	if (B.surface)
		return 1;
	result = host_vk.vkGetPhysicalDeviceDisplayPropertiesKHR(host_vk.physical, &count, &display);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
		return plane_failed(result, "vkGetPhysicalDeviceDisplayPropertiesKHR (or no display)");
	count = 1;
	result = host_vk.vkGetDisplayModePropertiesKHR(host_vk.physical, display.display, &count, &mode);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
		return plane_failed(result, "vkGetDisplayModePropertiesKHR (or no mode)");
	count = 1;
	result = host_vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(host_vk.physical, &count, &plane);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
		return plane_failed(result, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR (or no plane)");
	memset(&capabilities, 0, sizeof(capabilities));
	host_vk.vkGetDisplayPlaneCapabilitiesKHR(host_vk.physical, mode.displayMode, 0, &capabilities);
	info.displayMode = mode.displayMode;
	info.planeIndex = 0;
	info.planeStackIndex = 0;
	info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.globalAlpha = 1.0f;
	info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	info.imageExtent = mode.parameters.visibleRegion;
	result = host_vk.vkCreateDisplayPlaneSurfaceKHR(host_vk.instance, &info, NULL, &B.surface);
	if (result != VK_SUCCESS)
	{
		B.surface = VK_NULL_HANDLE;
		return plane_failed(result, "vkCreateDisplayPlaneSurfaceKHR");
	}
	host_logf(HOST_LOG_INFO, "vk: surface: display \"%s\", %ux%u at %u mHz, plane 0 (on %s, stack 0, its alpha 0x%x)",
		display.displayName ? display.displayName : "(no name)", mode.parameters.visibleRegion.width,
		mode.parameters.visibleRegion.height, mode.parameters.refreshRate, plane.currentDisplay == display.display ?
		"that display" : "no display yet", (unsigned)capabilities.supportedAlpha);
	return 1;
}
#else
int host_vk_surface_make(int quiet)
{
	VkAndroidSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
	void *native = native_window();
	VkResult result;

	if (B.surface)
		return 1;
	if (!native)
		return 0;
	info.window = (struct ANativeWindow *)native;
	result = host_vk.vkCreateAndroidSurfaceKHR(host_vk.instance, &info, NULL, &B.surface);
	if (result != VK_SUCCESS)
	{
		B.surface = VK_NULL_HANDLE;
		if (!quiet)
			host_vk_check(result, "vkCreateAndroidSurfaceKHR");
		return 0;
	}
	B.native_window = native;
	return 1;
}
#endif

/* ---------- the swapchain */

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

static void swapchain_destroy(struct host_vk_swapchain *chain)
{
	uint32_t index;

	for (index = 0; index < chain->count; index++)
	{
		if (chain->render_done[index])
			vkDestroySemaphore(B.device, chain->render_done[index], NULL);
		if (chain->views[index])
		{
			host_vk_pass_view_forget(chain->views[index]);
			vkDestroyImageView(B.device, chain->views[index], NULL);
		}
	}
	if (chain->handle)
		vkDestroySwapchainKHR(B.device, chain->handle, NULL);
	memset(chain, 0, sizeof(*chain));
}

/* the size the surface says its images are: its current extent, or the window's pixels where the surface leaves
it to the swapchain */
static int surface_extent(const VkSurfaceCapabilitiesKHR *caps, VkExtent2D *extent)
{
	*extent = caps->currentExtent;
#ifndef HALO_PS5
	if (extent->width == 0xffffffffu)
	{
		int width = 0, height = 0;

		if (host_vk_window)
			SDL_GetWindowSizeInPixels((SDL_Window *)host_vk_window, &width, &height);
		extent->width = (uint32_t)width;
		extent->height = (uint32_t)height;
	}
#endif
	return extent->width && extent->height;
}

/* makes a swapchain for the surface as it is now: FIFO, the surface's own transform left to the compositor
(preTransform identity: Android turns the picture), the first of B8G8R8A8_UNORM or R8G8B8A8_UNORM it lists in the
sRGB-nonlinear colour space, one image more than the minimum. Returns 0 (and says why, unless quiet) if it cannot. */
static int swapchain_create(struct host_vk_swapchain *chain, VkSwapchainKHR old, int quiet, VkSurfaceTransformFlagBitsKHR *transform_out)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSurfaceFormatKHR formats[32];
	uint32_t format_count = 32, index, count;
	VkResult result;
	int chosen = -1;
	VkCompositeAlphaFlagBitsKHR alpha;
	VkExtent2D extent;
	VkFormatProperties properties;

	memset(chain, 0, sizeof(*chain));
	result = host_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(host_vk.physical, B.surface, &caps);
	if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		return 0;
	}
	if (result != VK_SUCCESS)
		return quiet ? 0 : host_vk_check(result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	result = host_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(host_vk.physical, B.surface, &format_count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return quiet ? 0 : host_vk_check(result, "vkGetPhysicalDeviceSurfaceFormatsKHR");
	for (index = 0; index < format_count && chosen < 0; index++)
	{
		if ((formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) &&
			formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			chosen = (int)index;
	}
	if (chosen < 0)
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the surface lists neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM (sRGB nonlinear)");
		return 0;
	}
	if (!surface_extent(&caps, &extent))
	{
		if (!quiet)
			host_logf(HOST_LOG_WARN, "vk: the surface has no size yet");
		return 0;
	}
	if ((caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) !=
		(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the surface cannot take colour attachment and transfer destination usage");
		return 0;
	}
	/* the picture is blitted into the swapchain's images, so their format has to be a blit destination */
	host_vk.vkGetPhysicalDeviceFormatProperties(host_vk.physical, formats[chosen].format, &properties);
	if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the swapchain format %d is not a blit destination here", (int)formats[chosen].format);
		return 0;
	}
	/* two more than the surface needs: with only one, a phone whose surface asks for two (a Nokia 8: 3 images, one on
	the screen and one queued) had its render thread wait for an image to come back a third of the time */
	count = caps.minImageCount + 2;
	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;
	/* opaque if the surface offers it, else the first of the others (this device's surface offers only inherit) */
	alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
		(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR :
		(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR :
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	info.surface = B.surface;
	info.minImageCount = count;
	info.imageFormat = formats[chosen].format;
	info.imageColorSpace = formats[chosen].colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
#ifdef HALO_PS5
	/* (the screenshot copies the presented image: host_vk_screenshot_ps5.c) */
	if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
		VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	result = vkCreateSwapchainKHR(B.device, &info, NULL, &chain->handle);
	if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
	{
		chain->handle = VK_NULL_HANDLE;
		host_vk_surface_lost("vkCreateSwapchainKHR");
		return 0;
	}
	if (result != VK_SUCCESS)
	{
		chain->handle = VK_NULL_HANDLE;
		return quiet ? 0 : host_vk_check(result, "vkCreateSwapchainKHR");
	}
	chain->format = info.imageFormat;
	chain->extent = info.imageExtent;
	count = HOST_VK_SWAPCHAIN_IMAGES;
	result = vkGetSwapchainImagesKHR(B.device, chain->handle, &count, chain->images);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
	{
		host_vk_check(result, "vkGetSwapchainImagesKHR");
		swapchain_destroy(chain);
		return 0;
	}
	chain->count = count;
	for (index = 0; index < count; index++)
	{
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		if (!host_vk_check(vkCreateSemaphore(B.device, &semaphore, NULL, &chain->render_done[index]), "vkCreateSemaphore"))
		{
			/* the handle and the semaphores made so far */
			swapchain_destroy(chain);
			return 0;
		}
		{
			/* the picture is drawn in a rendering on the image (and the marker with it) */
			VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };

			view.image = chain->images[index];
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = chain->format;
			view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view.subresourceRange.levelCount = 1;
			view.subresourceRange.layerCount = 1;
			if (!host_vk_check(vkCreateImageView(B.device, &view, NULL, &chain->views[index]), "vkCreateImageView"))
			{
				swapchain_destroy(chain);
				return 0;
			}
			host_vk_pass_view_note(chain->views[index], chain->format, VK_SAMPLE_COUNT_1_BIT);
		}
	}
	if (transform_out)
		*transform_out = info.preTransform;
	return 1;
}

/* the swapchain and the surface are gone: waits for the device, then destroys them */
static void present_teardown(void)
{
	if (B.device)
		vkDeviceWaitIdle(B.device);
	swapchain_destroy(&B.chain);
	if (B.surface)
		host_vk.vkDestroySurfaceKHR(host_vk.instance, B.surface, NULL);
	B.surface = VK_NULL_HANDLE;
	B.native_window = NULL;
	host_vk_presenting = 0;
}

/* the surface is gone (found by acquire, by present, or by the window's ANativeWindow changing): counted and
logged once, however many tries it takes to make another */
void host_vk_surface_lost(const char *where)
{
	if (!B.lost_at)
	{
		B.lost_at = now_ns();
		B.lost_attempts = 0;
		host_logf(HOST_LOG_INFO, "vk: the surface is lost (%s)", where);
		/* the app is going to the background, which Android may follow with the process's end */
		host_vk_pipeline_cache_save("the surface was lost");
	}
	present_teardown();
}

/* a swapchain, if there is none: on the surface there is, or on one made now. Logs the return from a loss once. */
static int ensure_swapchain(void)
{
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	void *native = native_window();

	/* the window has a new ANativeWindow: the surface is on the old one */
	if (B.surface && native != B.native_window)
		host_vk_surface_lost(native ? "the window's ANativeWindow changed" : "the window has no ANativeWindow");
	if (B.chain.handle)
		return 1;
	if (B.lost_at)
		B.lost_attempts++;
	if (!B.surface && !host_vk_surface_make(1))
		return 0;
	if (!swapchain_create(&B.chain, VK_NULL_HANDLE, B.lost_at != 0, &transform))
		return 0;
	if (B.lost_at)
	{
		host_logf(HOST_LOG_INFO, "vk: the surface is back after %.1f ms, %u attempts: swapchain %ux%u, %u images, "
			"transform %s", (double)(now_ns() - B.lost_at) / 1e6, B.lost_attempts, B.chain.extent.width,
			B.chain.extent.height, B.chain.count, transform_name(transform));
		B.lost_at = 0;
	}
	else
	{
		host_logf(HOST_LOG_INFO, "vk: swapchain %ux%u, %u images, format %s, FIFO, preTransform %s", B.chain.extent.width,
			B.chain.extent.height, B.chain.count, B.chain.format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" :
			"R8G8B8A8_UNORM", transform_name(transform));
	}
	return 1;
}

/* makes the swapchain again from the old one, with the device idle first; the reason and the time are logged */
static void swapchain_recreate(const char *reason)
{
	uint64_t start = now_ns();
	struct host_vk_swapchain made, retired;
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	int ok;

	vkDeviceWaitIdle(B.device);
	ok = swapchain_create(&made, B.chain.handle, 0, &transform);
	/* the old swapchain is retired by the new one's creation; the device is idle, so its images are not in use */
	/* (if the creation found the surface lost, host_vk_surface_lost has destroyed the old swapchain and the surface
	already, and B.chain is empty here) */
	retired = B.chain;
	memset(&B.chain, 0, sizeof(B.chain));
	swapchain_destroy(&retired);
	if (ok)
	{
		B.chain = made;
		B.counts.recreations++;
		host_logf(HOST_LOG_INFO, "vk: swapchain made again (%s): %ux%u, %u images, %.2f ms", reason, B.chain.extent.width,
			B.chain.extent.height, B.chain.count, (double)(now_ns() - start) / 1e6);
	}
	else
	{
		host_vk_presenting = 0;
		host_logf(HOST_LOG_WARN, "vk: the swapchain could not be made again (%s)", reason);
	}
}

/* does the surface's size differ from the swapchain's? Looked at when the driver says SUBOPTIMAL (Android says so
for the rotation of the display, which is not a reason to make the swapchain again) and every so often */
static void extent_check(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkExtent2D extent;
	VkResult result;

	result = host_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(host_vk.physical, B.surface, &caps);
	if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		return;
	}
	if (result != VK_SUCCESS || !surface_extent(&caps, &extent))
		return;
	if (extent.width != B.chain.extent.width || extent.height != B.chain.extent.height)
		swapchain_recreate("the surface's size changed");
}

/* ---------- the drawn present */

static struct
{
	/* made: for the swapchain format `format`; failed: it cannot be (said once), and the blit is used */
	int made, failed;
	VkFormat format;
	VkShaderModule vertex, fragment;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkSampler sampler;
	VkDescriptorPool pool;
	/* a set for each frame's command buffer, written as it is recorded: the frame before may still be reading its own */
	VkDescriptorSet sets[HOST_VK_FRAMES];
} drawn;

/* one triangle over the whole viewport (the picture's rectangle) */
static const char drawn_vertex_source[] =
	"#version 450\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(float((gl_VertexIndex & 1) << 2) - 1.0, float((gl_VertexIndex & 2) << 1) - 1.0, 0.0, 1.0);\n"
	"}\n";

/* the back buffer's texel under the pixel: its rows are the picture's from the top, as the swapchain image's are */
static const char drawn_fragment_source[] =
	"#version 450\n"
	"layout(set = 0, binding = 0) uniform sampler2D picture;\n"
	"layout(push_constant) uniform pass\n"
	"{\n"
	"\t/* the rectangle's top left in the image, and 1 / its width and height */\n"
	"\tvec4 rectangle;\n"
	"};\n"
	"layout(location = 0) out vec4 result;\n"
	"void main()\n"
	"{\n"
	"\tresult = vec4(texture(picture, (gl_FragCoord.xy - rectangle.xy) * rectangle.zw).rgb, 1.0);\n"
	"}\n";

static void drawn_destroy_pipeline(void)
{
	if (drawn.pipeline)
		vkDestroyPipeline(B.device, drawn.pipeline, NULL);
	drawn.pipeline = VK_NULL_HANDLE;
	drawn.made = 0;
}

/* the pass's pipeline for the swapchain's format, made the first time (and again for another format), and what it
binds; 0 if it cannot be made (said once) */
static int drawn_make(void)
{
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorSetLayout layouts[HOST_VK_FRAMES];
	VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo depth_stencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	uint32_t index;

	if (drawn.made && drawn.format == B.chain.format)
		return 1;
	if (drawn.failed || !B.color_sampled)
		return 0;
	/* (another format: a new swapchain, made with the device idle) */
	drawn_destroy_pipeline();
	drawn.failed = 1;
	if (!drawn.layout)
	{
		drawn.vertex = host_vk_shader_module(drawn_vertex_source, 0, "present pass's vertex shader");
		drawn.fragment = host_vk_shader_module(drawn_fragment_source, 1, "present pass's fragment shader");
		if (!drawn.vertex || !drawn.fragment)
			goto failed;
		memset(&binding, 0, sizeof(binding));
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		set_layout.bindingCount = 1;
		set_layout.pBindings = &binding;
		if (!HOST_VK_CHECK(vkCreateDescriptorSetLayout(B.device, &set_layout, NULL, &drawn.set_layout)))
			goto failed;
		/* the blit's filter: linear where the format can be filtered so */
		sampler.magFilter = B.blit_filter;
		sampler.minFilter = B.blit_filter;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.maxLod = 0.25f;
		if (!HOST_VK_CHECK(vkCreateSampler(B.device, &sampler, NULL, &drawn.sampler)))
			goto failed;
		size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		size.descriptorCount = HOST_VK_FRAMES;
		pool.maxSets = HOST_VK_FRAMES;
		pool.poolSizeCount = 1;
		pool.pPoolSizes = &size;
		if (!HOST_VK_CHECK(vkCreateDescriptorPool(B.device, &pool, NULL, &drawn.pool)))
			goto failed;
		for (index = 0; index < HOST_VK_FRAMES; index++)
			layouts[index] = drawn.set_layout;
		allocate.descriptorPool = drawn.pool;
		allocate.descriptorSetCount = HOST_VK_FRAMES;
		allocate.pSetLayouts = layouts;
		if (!HOST_VK_CHECK(vkAllocateDescriptorSets(B.device, &allocate, drawn.sets)))
			goto failed;
		range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		range.offset = 0;
		range.size = 16;
		layout.setLayoutCount = 1;
		layout.pSetLayouts = &drawn.set_layout;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges = &range;
		if (!HOST_VK_CHECK(vkCreatePipelineLayout(B.device, &layout, NULL, &drawn.layout)))
			goto failed;
	}

	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = drawn.vertex;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = drawn.fragment;
	stages[1].pName = "main";
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &B.chain.format;
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth_stencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = drawn.layout;
	info.basePipelineIndex = -1;
	/* (a driver without dynamic rendering: a render pass of the rendering's format, host_vk_passes.c) */
	info.renderPass = host_vk_pass_for_pipeline(&rendering, multisample.rasterizationSamples, 0);
	if (info.renderPass)
		info.pNext = rendering.pNext;
	if (!HOST_VK_CHECK(vkCreateGraphicsPipelines(B.device, B.pipeline_cache, 1, &info, NULL, &drawn.pipeline)))
		goto failed;
	drawn.made = 1;
	drawn.failed = 0;
	drawn.format = B.chain.format;
	host_logf(HOST_LOG_INFO, "vk: the picture is presented by a draw");
	return 1;
failed:
	host_logf(HOST_LOG_WARN, "vk: the present pass cannot be made; the picture is presented by a blit");
	return 0;
}

/* the back buffer drawn into the rectangle (x, y, width, height) of swapchain image `index`, the rest black, and the
marker's squares with it; the image is left in COLOR_ATTACHMENT_OPTIMAL. 0 (nothing recorded) where the pass cannot be
made: the clear and the blit present it then. */
static int drawn_present(VkCommandBuffer command, struct host_vk_target *back_buffer, uint32_t index, int64_t x, int64_t y,
	int64_t width, int64_t height)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkPipelineStageFlags source_stage, destination_stage;
	VkDescriptorImageInfo image;
	VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	VkDescriptorSet set;
	VkViewport viewport;
	VkRect2D rectangle;
	float constants[4];

	if (!B.chain.views[index] || !back_buffer->view || !drawn_make())
		return 0;
	set = drawn.sets[B.frame];
	host_vk_target_transition(command, back_buffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	image.sampler = drawn.sampler;
	image.imageView = back_buffer->view;
	image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(B.device, 1, &write, 0, NULL);

	/* (the layout change must follow the acquire semaphore's wait, which is at the transfer stage: frame_submit) */
	host_vk_barrier_scope(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &source_stage,
		&barrier.srcAccessMask, &destination_stage, &barrier.dstAccessMask);
	source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = B.chain.images[index];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

	attachment.imageView = B.chain.views[index];
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.clearValue.color.float32[3] = 1.0f;
	rendering.renderArea.extent = B.chain.extent;
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &attachment;
	host_vk_profile_rendering_begin(command, "present");
	host_vk_cmd_begin_rendering(command, &rendering);
	if (width > 0 && height > 0)
	{
		viewport.x = (float)x;
		viewport.y = (float)y;
		viewport.width = (float)width;
		viewport.height = (float)height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		rectangle.offset.x = (int32_t)x;
		rectangle.offset.y = (int32_t)y;
		rectangle.extent.width = (uint32_t)width;
		rectangle.extent.height = (uint32_t)height;
		vkCmdSetViewport(command, 0, 1, &viewport);
		vkCmdSetScissor(command, 0, 1, &rectangle);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawn.pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawn.layout, 0, 1, &set, 0, NULL);
		constants[0] = (float)x;
		constants[1] = (float)y;
		constants[2] = 1.0f / (float)width;
		constants[3] = 1.0f / (float)height;
		vkCmdPushConstants(command, drawn.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
		vkCmdDraw(command, 3, 1, 0, 0);
	}
	if (host_vk_present_marker && width > 0 && height > 0)
	{
		/* a red square at the picture's top left and a green one at its top right, so that a screenshot shows which
		way up the picture is and where its corners land */
		VkClearAttachment clear;
		VkClearRect rect;
		uint32_t square_size = (uint32_t)(width / 16 > 16 ? width / 16 : 16);
		int square;

		if (square_size > (uint32_t)height)
			square_size = (uint32_t)height;
		memset(&clear, 0, sizeof(clear));
		clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		memset(&rect, 0, sizeof(rect));
		rect.layerCount = 1;
		rect.rect.extent.width = square_size;
		rect.rect.extent.height = square_size;
		rect.rect.offset.y = (int32_t)y;
		for (square = 0; square < 2; square++)
		{
			clear.clearValue.color.float32[0] = square == 0 ? 1.0f : 0.0f;
			clear.clearValue.color.float32[1] = square == 0 ? 0.0f : 1.0f;
			clear.clearValue.color.float32[3] = 1.0f;
			rect.rect.offset.x = square == 0 ? (int32_t)x : (int32_t)(x + width - square_size);
			vkCmdClearAttachments(command, 1, &clear, 1, &rect);
		}
	}
	host_vk_cmd_end_rendering(command);
	host_vk_profile_rendering_end(command);
	return 1;
}

/* ---------- presenting */

uint32_t host_vk_present_record(VkCommandBuffer command, struct host_vk_target *back_buffer)
{
	static unsigned frames_since_check;
	VkResult result;
	uint32_t index = 0;
	struct host_vk_frame *frame = &B.frames[B.frame];
	VkImage image;
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkPipelineStageFlags source_stage, destination_stage;
	VkImageBlit blit;
	int attempt, acquired = 0;
	uint64_t acquire_start;
	int64_t width, height, x, y;

	if (!ensure_swapchain())
	{
		host_vk_presenting = 0;
		return UINT32_MAX;
	}
	if (++frames_since_check >= EXTENT_CHECK_FRAMES)
	{
		frames_since_check = 0;
		extent_check();
		if (!B.chain.handle)
			return UINT32_MAX;
	}
	acquire_start = now_ns();
	for (attempt = 0; attempt < 2 && !acquired; attempt++)
	{
		uint64_t waited = now_ns() - acquire_start;

		result = vkAcquireNextImageKHR(B.device, B.chain.handle, waited < ACQUIRE_TIMEOUT_NS ? ACQUIRE_TIMEOUT_NS - waited : 0,
			frame->acquired, VK_NULL_HANDLE, &index);
		/* (a timeout before its time is waited out again: host_vk_wait_fence) */
		if (result == VK_TIMEOUT && now_ns() - acquire_start < ACQUIRE_TIMEOUT_NS)
		{
			attempt--;
			continue;
		}
		if (result == VK_SUCCESS)
		{
			acquired = 1;
		}
		else if (result == VK_SUBOPTIMAL_KHR)
		{
			/* the image is acquired and its semaphore signalled: it is used; the size is looked at after the present */
			acquired = 1;
			frames_since_check = EXTENT_CHECK_FRAMES;
		}
		else if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			swapchain_recreate("out of date");
			if (!B.chain.handle)
				return UINT32_MAX;
		}
		else if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			host_vk_surface_lost("vkAcquireNextImageKHR");
			return UINT32_MAX;
		}
		else if (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			/* with the app away nothing frees an image: the surface is as good as lost */
			host_vk_surface_lost("vkAcquireNextImageKHR timed out");
			return UINT32_MAX;
		}
		else
		{
			host_vk_check(result, "vkAcquireNextImageKHR");
			return UINT32_MAX;
		}
	}
	if (!acquired)
		return UINT32_MAX;
	image = B.chain.images[index];

	/* the picture's rectangle: the letterbox as d3d8_gl.c's Present works it out, the window's width, or its height,
	whichever the picture's shape fits */
	width = B.chain.extent.width;
	height = width * (int64_t)back_buffer->key.pixel_height / (int64_t)back_buffer->key.pixel_width;
	if (height > (int64_t)B.chain.extent.height)
	{
		height = B.chain.extent.height;
		width = height * (int64_t)back_buffer->key.pixel_width / (int64_t)back_buffer->key.pixel_height;
	}
	x = ((int64_t)B.chain.extent.width - width) / 2;
	y = ((int64_t)B.chain.extent.height - height) / 2;
	if (drawn_present(command, back_buffer, index, x, y, width, height))
	{
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.image = image;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		goto presentable;
	}

	/* (no present pass) the back buffer is read by the blit; the image is cleared black (the bars) and the picture blitted into it, the
	letterbox as d3d8_gl.c's Present works it out: the window's width, or its height, whichever the picture's shape fits */
	host_vk_target_transition(command, back_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	host_vk_barrier_scope(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &source_stage,
		&barrier.srcAccessMask, &destination_stage, &barrier.dstAccessMask);
	/* (the layout change must follow the acquire semaphore's wait, which is at the transfer stage: frame_submit) */
	source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
	{
		VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	}
	/* (the clear is a transfer write and the blit another: a barrier between them) */
	{
		VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

		memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, NULL,
			0, NULL);
	}
	if (width > 0 && height > 0)
	{
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)back_buffer->key.pixel_width;
		blit.srcOffsets[1].y = (int32_t)back_buffer->key.pixel_height;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[0].x = (int32_t)x;
		blit.dstOffsets[0].y = (int32_t)y;
		blit.dstOffsets[1].x = (int32_t)(x + width);
		blit.dstOffsets[1].y = (int32_t)(y + height);
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(command, back_buffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, B.blit_filter);
	}
	if (host_vk_present_marker && width > 0 && height > 0 && B.chain.views[index])
	{
		/* a red square at the picture's top left and a green one at its top right, so that a screenshot shows
		which way up the picture is and where its corners land. vkCmdClearColorImage clears whole subresources only,
		so the squares are cleared by vkCmdClearAttachments in a rendering on the swapchain image. */
		VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		VkClearAttachment clear;
		VkClearRect rect;
		uint32_t size = (uint32_t)(width / 16 > 16 ? width / 16 : 16);
		int square;

		if (size > (uint32_t)height)
			size = (uint32_t)height;
		host_vk_barrier_scope(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &source_stage,
			&barrier.srcAccessMask, &destination_stage, &barrier.dstAccessMask);
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
		attachment.imageView = B.chain.views[index];
		attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		rendering.renderArea.extent = B.chain.extent;
		rendering.layerCount = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments = &attachment;
		host_vk_cmd_begin_rendering(command, &rendering);
		memset(&clear, 0, sizeof(clear));
		clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		memset(&rect, 0, sizeof(rect));
		rect.layerCount = 1;
		rect.rect.extent.width = size;
		rect.rect.extent.height = size;
		rect.rect.offset.y = (int32_t)y;
		for (square = 0; square < 2; square++)
		{
			clear.clearValue.color.float32[0] = square == 0 ? 1.0f : 0.0f;
			clear.clearValue.color.float32[1] = square == 0 ? 0.0f : 1.0f;
			clear.clearValue.color.float32[3] = 1.0f;
			rect.rect.offset.x = square == 0 ? (int32_t)x : (int32_t)(x + width - size);
			vkCmdClearAttachments(command, 1, &clear, 1, &rect);
		}
		host_vk_cmd_end_rendering(command);
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	else
	{
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
presentable:
#ifdef HALO_PS5
	/* (the picture as it is presented, copied when a screenshot is due; the image is then in barrier.oldLayout) */
	host_vk_screenshot_record(command, image, &barrier.oldLayout);
#endif
	host_vk_barrier_scope(barrier.oldLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &source_stage, &barrier.srcAccessMask,
		&destination_stage, &barrier.dstAccessMask);
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
	return index;
}

void host_vk_present_queue(uint32_t image_index)
{
	VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	VkResult result;
	static int first = 1;

	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &B.chain.render_done[image_index];
	present.swapchainCount = 1;
	present.pSwapchains = &B.chain.handle;
	present.pImageIndices = &image_index;
	result = vkQueuePresentKHR(B.queue, &present);
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
	{
		if (first)
			host_logf(HOST_LOG_INFO, "vk: first frame presented");
		first = 0;
		host_vk_presenting = 1;
		if (result == VK_SUBOPTIMAL_KHR)
			extent_check();
	}
	else if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		swapchain_recreate("out of date");
	}
	else if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkQueuePresentKHR");
	}
	else
	{
		host_vk_check(result, "vkQueuePresentKHR");
		host_vk_presenting = 0;
	}
}
