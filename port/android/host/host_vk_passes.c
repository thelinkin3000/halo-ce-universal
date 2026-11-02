/*
HOST_VK_PASSES.C

Render passes for a driver without dynamic rendering (host_vk.render_passes: a Mali's own Vulkan 1.1, which lacks
VK_KHR_dynamic_rendering). The backend begins and ends its renderings with vkCmdBeginRendering's VkRenderingInfo
(host_vk_cmd_begin_rendering and host_vk_cmd_end_rendering), and makes its pipelines for the formats a rendering has
(VkPipelineRenderingCreateInfo). Here a rendering becomes a render pass of one subpass and a framebuffer of its
views, both made the first time and kept, and a pipeline is made with a render pass compatible with the renderings it
is drawn in. The backend's code is the same either way.

A render pass is the rendering as it is: its colour (and the resolve attachment, when it resolves its samples into
another view), its depth and stencil (one attachment: the rendering names one view for both), their loads, stores and
layouts. Each attachment is in the same layout before, during and after the subpass, so the render pass makes no
transitions and no dependencies of its own: the backend's barriers, recorded outside the renderings, make them as
they do for dynamic rendering. Render passes are compatible when their attachments' formats and sample counts are
(the loads, stores and layouts do not matter), so a pipeline is given one of the same formats and samples.

A view carries neither its format nor its sample count, which a render pass needs: the backend notes them as it makes
each view that can be an attachment (host_vk_pass_view_note), and before it destroys one the framebuffers made of it
go (host_vk_pass_view_forget), which is when nothing can still be using them: a view is destroyed only once the frames
that drew with it are done.

B.lock is held by every caller (host_vk_submit's), as for the rest of the backend.
*/

#include "host.h"
#include "host_vk.h"

#include <stdlib.h>
#include <string.h>

#define B host_vkb

/* the views that can be attachments, the render passes and the framebuffers made: a frame names a few targets, and
the game a few dozen in all */
#define VIEW_SLOTS 256
#define PASS_SLOTS 128
#define FRAMEBUFFER_SLOTS 256

struct view_note
{
	VkImageView view;
	VkFormat format;
	VkSampleCountFlagBits samples;
};

/* what a render pass is made of: VK_FORMAT_UNDEFINED where it has no such attachment */
struct pass_key
{
	VkFormat color_format, resolve_format, depth_format;
	VkSampleCountFlagBits color_samples, depth_samples;
	VkAttachmentLoadOp color_load, depth_load, stencil_load;
	VkAttachmentStoreOp color_store, depth_store, stencil_store;
	VkImageLayout color_layout, resolve_layout, depth_layout;
};

struct pass_entry
{
	struct pass_key key;
	VkRenderPass pass;
};

struct framebuffer_entry
{
	VkRenderPass pass;
	VkImageView views[3];
	uint32_t count, width, height;
	VkFramebuffer framebuffer;
	/* the submission of the frame that last began a rendering with it (B.submission + 1 while it is recorded) */
	uint64_t used;
};

static struct view_note views[VIEW_SLOTS];
static unsigned view_count;
static struct pass_entry passes[PASS_SLOTS];
static unsigned pass_count;
static struct framebuffer_entry framebuffers[FRAMEBUFFER_SLOTS];
static unsigned framebuffer_count;
static int said_full;

void host_vk_pass_view_note(VkImageView view, VkFormat format, VkSampleCountFlagBits samples)
{
	unsigned index;

	if (!host_vk.render_passes || view == VK_NULL_HANDLE)
		return;
	for (index = 0; index < view_count; index++)
	{
		if (views[index].view == view)
			break;
	}
	if (index == view_count)
	{
		if (view_count == VIEW_SLOTS)
		{
			host_logf(HOST_LOG_ERROR, "vk: more than %d attachment views; a rendering on another cannot be begun",
				VIEW_SLOTS);
			return;
		}
		view_count++;
	}
	views[index].view = view;
	views[index].format = format;
	views[index].samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
}

void host_vk_pass_view_forget(VkImageView view)
{
	unsigned index, kept = 0;

	if (!host_vk.render_passes || view == VK_NULL_HANDLE)
		return;
	for (index = 0; index < framebuffer_count; index++)
	{
		struct framebuffer_entry *entry = &framebuffers[index];
		uint32_t attachment;
		int uses = 0;

		for (attachment = 0; attachment < entry->count; attachment++)
			uses |= entry->views[attachment] == view;
		if (uses)
			vkDestroyFramebuffer(B.device, entry->framebuffer, NULL);
		else
			framebuffers[kept++] = *entry;
	}
	framebuffer_count = kept;
	for (index = 0; index < view_count; index++)
	{
		if (views[index].view == view)
		{
			views[index] = views[--view_count];
			break;
		}
	}
}

static const struct view_note *view_find(VkImageView view)
{
	unsigned index;

	for (index = 0; index < view_count; index++)
	{
		if (views[index].view == view)
			return &views[index];
	}
	return NULL;
}

/* one attachment, in one layout throughout */
static void attachment_describe(VkAttachmentDescription *description, VkFormat format, VkSampleCountFlagBits samples,
	VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkAttachmentLoadOp stencil_load,
	VkAttachmentStoreOp stencil_store, VkImageLayout layout)
{
	memset(description, 0, sizeof(*description));
	description->format = format;
	description->samples = samples;
	description->loadOp = load;
	description->storeOp = store;
	description->stencilLoadOp = stencil_load;
	description->stencilStoreOp = stencil_store;
	description->initialLayout = layout;
	description->finalLayout = layout;
}

/* the render pass of a key, made the first time; VK_NULL_HANDLE if it cannot be */
static VkRenderPass pass_get(const struct pass_key *key)
{
	VkAttachmentDescription attachments[3];
	VkAttachmentReference color = { 0 }, resolve = { 0 }, depth = { 0 };
	VkSubpassDescription subpass = { 0 };
	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	VkRenderPass pass = VK_NULL_HANDLE;
	unsigned index;
	uint32_t count = 0;

	for (index = 0; index < pass_count; index++)
	{
		if (!memcmp(&passes[index].key, key, sizeof(*key)))
			return passes[index].pass;
	}
	if (pass_count == PASS_SLOTS)
	{
		if (!said_full)
			host_logf(HOST_LOG_ERROR, "vk: more than %d render passes; a rendering of another kind is not drawn",
				PASS_SLOTS);
		said_full = 1;
		return VK_NULL_HANDLE;
	}
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	if (key->color_format != VK_FORMAT_UNDEFINED)
	{
		attachment_describe(&attachments[count], key->color_format, key->color_samples, key->color_load,
			key->color_store, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, key->color_layout);
		color.attachment = count++;
		color.layout = key->color_layout;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color;
		if (key->resolve_format != VK_FORMAT_UNDEFINED)
		{
			/* (resolved as the subpass ends: what it held before is not read) */
			attachment_describe(&attachments[count], key->resolve_format, VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE, key->resolve_layout);
			resolve.attachment = count++;
			resolve.layout = key->resolve_layout;
			subpass.pResolveAttachments = &resolve;
		}
	}
	if (key->depth_format != VK_FORMAT_UNDEFINED)
	{
		attachment_describe(&attachments[count], key->depth_format, key->depth_samples, key->depth_load,
			key->depth_store, key->stencil_load, key->stencil_store, key->depth_layout);
		depth.attachment = count++;
		depth.layout = key->depth_layout;
		subpass.pDepthStencilAttachment = &depth;
	}
	info.attachmentCount = count;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	if (!HOST_VK_CHECK(vkCreateRenderPass(B.device, &info, NULL, &pass)))
		return VK_NULL_HANDLE;
	passes[pass_count].key = *key;
	passes[pass_count].pass = pass;
	pass_count++;
	return pass;
}

/* the framebuffer of a render pass's views, made the first time; VK_NULL_HANDLE if it cannot be */
static VkFramebuffer framebuffer_get(VkRenderPass pass, const VkImageView *attachment_views, uint32_t count,
	uint32_t width, uint32_t height)
{
	VkFramebufferCreateInfo info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	struct framebuffer_entry *entry;
	unsigned index;

	for (index = 0; index < framebuffer_count; index++)
	{
		entry = &framebuffers[index];
		if (entry->pass == pass && entry->count == count && entry->width == width && entry->height == height &&
			!memcmp(entry->views, attachment_views, count * sizeof(*attachment_views)))
		{
			entry->used = B.submission + 1;
			return entry->framebuffer;
		}
	}
	if (framebuffer_count == FRAMEBUFFER_SLOTS)
	{
		/* (full, which the game's few dozen targets do not make it: the one used longest ago goes, once the device has
		finished every frame submitted; never one the frame being recorded uses) */
		unsigned oldest = FRAMEBUFFER_SLOTS;

		for (index = 0; index < framebuffer_count; index++)
		{
			if (framebuffers[index].used <= B.submission &&
				(oldest == FRAMEBUFFER_SLOTS || framebuffers[index].used < framebuffers[oldest].used))
				oldest = index;
		}
		if (oldest == FRAMEBUFFER_SLOTS)
		{
			if (!said_full)
				host_logf(HOST_LOG_ERROR, "vk: %d framebuffers in one frame; a rendering past them is not drawn",
					FRAMEBUFFER_SLOTS);
			said_full = 1;
			return VK_NULL_HANDLE;
		}
		vkDeviceWaitIdle(B.device);
		vkDestroyFramebuffer(B.device, framebuffers[oldest].framebuffer, NULL);
		framebuffers[oldest] = framebuffers[--framebuffer_count];
	}
	info.renderPass = pass;
	info.attachmentCount = count;
	info.pAttachments = attachment_views;
	info.width = width;
	info.height = height;
	info.layers = 1;
	if (!HOST_VK_CHECK(vkCreateFramebuffer(B.device, &info, NULL, &framebuffer)))
		return VK_NULL_HANDLE;
	entry = &framebuffers[framebuffer_count++];
	entry->pass = pass;
	memset(entry->views, 0, sizeof(entry->views));
	memcpy(entry->views, attachment_views, count * sizeof(*attachment_views));
	entry->count = count;
	entry->width = width;
	entry->height = height;
	entry->framebuffer = framebuffer;
	entry->used = B.submission + 1;
	return framebuffer;
}

/* (whether a rendering was begun as a render pass, so that its end ends one) */
static int pass_open;

static void VKAPI_CALL begin_rendering(VkCommandBuffer command, const VkRenderingInfo *rendering)
{
	const VkRenderingAttachmentInfo *color = rendering->colorAttachmentCount ? &rendering->pColorAttachments[0] : NULL;
	const VkRenderingAttachmentInfo *depth = rendering->pDepthAttachment && rendering->pDepthAttachment->imageView ?
		rendering->pDepthAttachment : rendering->pStencilAttachment;
	const VkRenderingAttachmentInfo *stencil = rendering->pStencilAttachment;
	struct pass_key key;
	VkImageView attachment_views[3];
	VkClearValue clears[3];
	VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	uint32_t count = 0;
	VkRenderPass pass;
	VkFramebuffer framebuffer;

	pass_open = 0;
	memset(&key, 0, sizeof(key));
	if (color && color->imageView)
	{
		const struct view_note *note = view_find(color->imageView);

		if (!note)
		{
			host_logf(HOST_LOG_ERROR, "vk: a rendering's colour view was not noted; it is not drawn");
			return;
		}
		key.color_format = note->format;
		key.color_samples = note->samples;
		key.color_load = color->loadOp;
		key.color_store = color->storeOp;
		key.color_layout = color->imageLayout;
		attachment_views[count] = color->imageView;
		clears[count++] = color->clearValue;
		if (color->resolveMode != VK_RESOLVE_MODE_NONE && color->resolveImageView)
		{
			const struct view_note *resolve = view_find(color->resolveImageView);

			if (!resolve)
			{
				host_logf(HOST_LOG_ERROR, "vk: a rendering's resolve view was not noted; it is not drawn");
				return;
			}
			key.resolve_format = resolve->format;
			key.resolve_layout = color->resolveImageLayout;
			attachment_views[count] = color->resolveImageView;
			memset(&clears[count++], 0, sizeof(clears[0]));
		}
	}
	if (depth && depth->imageView)
	{
		const struct view_note *note = view_find(depth->imageView);

		if (!note)
		{
			host_logf(HOST_LOG_ERROR, "vk: a rendering's depth view was not noted; it is not drawn");
			return;
		}
		key.depth_format = note->format;
		key.depth_samples = note->samples;
		key.depth_load = rendering->pDepthAttachment ? rendering->pDepthAttachment->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		key.depth_store = rendering->pDepthAttachment ? rendering->pDepthAttachment->storeOp :
			VK_ATTACHMENT_STORE_OP_DONT_CARE;
		key.stencil_load = stencil ? stencil->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		key.stencil_store = stencil ? stencil->storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		key.depth_layout = depth->imageLayout;
		attachment_views[count] = depth->imageView;
		clears[count].depthStencil.depth = rendering->pDepthAttachment ?
			rendering->pDepthAttachment->clearValue.depthStencil.depth : 0.0f;
		clears[count++].depthStencil.stencil = stencil ? stencil->clearValue.depthStencil.stencil : 0;
	}
	if (!count)
		return;
	pass = pass_get(&key);
	framebuffer = pass ? framebuffer_get(pass, attachment_views, count, rendering->renderArea.offset.x +
		rendering->renderArea.extent.width, rendering->renderArea.offset.y + rendering->renderArea.extent.height) :
		VK_NULL_HANDLE;
	if (!framebuffer)
		return;
	begin.renderPass = pass;
	begin.framebuffer = framebuffer;
	begin.renderArea = rendering->renderArea;
	begin.clearValueCount = count;
	begin.pClearValues = clears;
	vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
	pass_open = 1;
}

static void VKAPI_CALL end_rendering(VkCommandBuffer command)
{
	if (pass_open)
		vkCmdEndRenderPass(command);
	pass_open = 0;
}

void host_vk_passes_install(void)
{
	host_vk_cmd_begin_rendering = (PFN_vkCmdBeginRenderingKHR)begin_rendering;
	host_vk_cmd_end_rendering = (PFN_vkCmdEndRenderingKHR)end_rendering;
	host_logf(HOST_LOG_INFO, "vk: no dynamic rendering: each rendering is a render pass (host_vk_passes.c)");
}

VkRenderPass host_vk_pass_for_pipeline(const VkPipelineRenderingCreateInfo *rendering, VkSampleCountFlagBits samples,
	int resolves)
{
	struct pass_key key;

	if (!host_vk.render_passes)
		return VK_NULL_HANDLE;
	memset(&key, 0, sizeof(key));
	/* (compatible with every rendering of these formats and samples: the loads, stores and layouts are any) */
	if (rendering->colorAttachmentCount && rendering->pColorAttachmentFormats[0] != VK_FORMAT_UNDEFINED)
	{
		key.color_format = rendering->pColorAttachmentFormats[0];
		key.color_samples = samples;
		key.color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
		key.color_store = VK_ATTACHMENT_STORE_OP_STORE;
		key.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		if (resolves && samples > VK_SAMPLE_COUNT_1_BIT)
		{
			key.resolve_format = key.color_format;
			key.resolve_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
	}
	if (rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED || rendering->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
	{
		key.depth_format = rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED ? rendering->depthAttachmentFormat :
			rendering->stencilAttachmentFormat;
		key.depth_samples = samples;
		key.depth_load = key.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
		key.depth_store = key.stencil_store = VK_ATTACHMENT_STORE_OP_STORE;
		key.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}
	return pass_get(&key);
}
