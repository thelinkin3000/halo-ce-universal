/*
HOST_VK_DRAW.C

The Vulkan renderer's host half, phase 6 (port/android/VULKAN.md): the game's draws. Each VK_COMMAND_DRAW
(port/android/guest/vk_commands.h) is one self-contained record, and this file turns it into Vulkan commands: the
pipeline for its shaders and state (host_vk_shaders.c; a draw whose pipeline is not ready is skipped, counted), the
dynamic state, a descriptor set of its own (the three uniform blocks and the four textures), the vertex and index
buffers (in the upload ring, where the guest's copies were placed), and the draw.

Nothing here keeps draw state between draws but what is bound. Descriptor sets are allocated one a draw from pools that
belong to the frame and are reset when the frame's fence has passed, so a set a recorded command uses is never rewritten.
A stage with no texture gets a dummy: an opaque black 1x1 image of the type the pixel shader declares for it (2D, 3D or
cube), so the view's type always matches the shader's sampler.
*/

#include "host.h"
#include "host_vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B host_vkb

/* ---------- barriers */

/* a full barrier (all commands to all commands, memory read and write) from one layout to another, outside a rendering */
static void image_barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, uint32_t levels,
	uint32_t layers, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

	barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = levels;
	barrier.subresourceRange.layerCount = layers;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
		1, &barrier);
}

/* ---------- the dummies */

/* by the shader's sampler type minus 1: 2D, 3D, cube */
static struct
{
	VkImage image[3];
	VkDeviceMemory memory[3];
	VkImageView view[3];
	int made, failed;
} dummies;

static int dummy_make(int which)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkClearColorValue black;
	VkImageSubresourceRange range;
	VkCommandBuffer command;
	uint32_t layers = which == 2 ? 6 : 1;

	info.imageType = which == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	info.flags = which == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent.width = info.extent.height = info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = layers;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!HOST_VK_CHECK(vkCreateImage(B.device, &info, NULL, &dummies.image[which])))
		return 0;
	vkGetImageMemoryRequirements(B.device, dummies.image[which], &requirements);
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = host_vk_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (allocation.memoryTypeIndex == UINT32_MAX || !HOST_VK_CHECK(vkAllocateMemory(B.device, &allocation, NULL,
		&dummies.memory[which])) || !HOST_VK_CHECK(vkBindImageMemory(B.device, dummies.image[which], dummies.memory[which], 0)))
		return 0;
	view.image = dummies.image[which];
	view.viewType = which == 1 ? VK_IMAGE_VIEW_TYPE_3D : which == 2 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
	view.format = info.format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = layers;
	if (!HOST_VK_CHECK(vkCreateImageView(B.device, &view, NULL, &dummies.view[which])))
		return 0;
	command = host_vk_frame_command();
	image_barrier(command, dummies.image[which], VK_IMAGE_ASPECT_COLOR_BIT, 1, layers, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&black, 0, sizeof(black));
	black.float32[3] = 1.0f;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = layers;
	vkCmdClearColorImage(command, dummies.image[which], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	image_barrier(command, dummies.image[which], VK_IMAGE_ASPECT_COLOR_BIT, 1, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return 1;
}

/* the three dummies, made when the first draw needs one (outside a rendering: the one open is ended) */
static int dummies_ensure(void)
{
	int which;

	if (dummies.made)
		return 1;
	if (dummies.failed)
		return 0;
	host_vk_rendering_end();
	for (which = 0; which < 3; which++)
	{
		if (!dummy_make(which))
		{
			dummies.failed = 1;
			host_logf(HOST_LOG_ERROR, "vk: the dummy textures could not be made; draws are not made");
			return 0;
		}
	}
	dummies.made = 1;
	return 1;
}

/* ---------- samplers: one VkSampler for each distinct state */

struct sampler_entry
{
	struct vk_sampler_state state;
	VkSampler sampler;
	struct sampler_entry *next;
};

#define SAMPLER_BUCKETS 256

static struct sampler_entry *sampler_buckets[SAMPLER_BUCKETS];
static unsigned sampler_count;

static VkSampler sampler_get(const struct vk_sampler_state *state)
{
	unsigned bucket = (unsigned)(vk_hash_mix(vk_hash_init(), state, sizeof(*state)) % SAMPLER_BUCKETS);
	struct sampler_entry *entry;
	VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	unsigned alpha = state->border_color >> 24;
	unsigned luminance = (((state->border_color >> 16) & 0xff) + ((state->border_color >> 8) & 0xff) +
		(state->border_color & 0xff)) / 3;

	for (entry = sampler_buckets[bucket]; entry; entry = entry->next)
	{
		if (!memcmp(&entry->state, state, sizeof(*state)))
			return entry->sampler;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return VK_NULL_HANDLE;
	entry->state = *state;
	info.magFilter = state->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter = state->min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = state->mip_mode == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = (VkSamplerAddressMode)state->address[0];
	info.addressModeV = (VkSamplerAddressMode)state->address[1];
	info.addressModeW = (VkSamplerAddressMode)state->address[2];
	info.mipLodBias = state->lod_bias;
	if (state->mip_mode)
	{
		info.minLod = state->min_lod;
		info.maxLod = 1000.0f;
	}
	else
	{
		/* no mipmapping: the base level only */
		info.minLod = 0.0f;
		info.maxLod = 0.25f;
	}
	if (B.sampler_anisotropy && state->anisotropy > 1.0f)
	{
		float limit = host_vk.properties.limits.maxSamplerAnisotropy;

		info.anisotropyEnable = VK_TRUE;
		info.maxAnisotropy = state->anisotropy < limit ? state->anisotropy : limit;
	}
	/* Vulkan's three fixed border colours: the nearest of them (the game sets a border colour rarely) */
	info.borderColor = alpha < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
		luminance > 127 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	if (!HOST_VK_CHECK(vkCreateSampler(B.device, &info, NULL, &entry->sampler)))
	{
		free(entry);
		return VK_NULL_HANDLE;
	}
	entry->next = sampler_buckets[bucket];
	sampler_buckets[bucket] = entry;
	sampler_count++;
	return entry->sampler;
}

/* ---------- descriptor sets */

#define POOL_SETS 512

void host_vk_draw_frame_reset(struct host_vk_frame *frame)
{
	unsigned index;

	for (index = 0; index < frame->pool_count; index++)
		HOST_VK_CHECK(vkResetDescriptorPool(B.device, frame->pools[index], 0));
	host_vk_texture_frame_reset((unsigned)(frame - B.frames));
	frame->pool_current = 0;
}

/* a descriptor set of the draws' layout, from the frame's pools (a new one when they are full) */
static VkDescriptorSet set_allocate(struct host_vk_frame *frame)
{
	for (;;)
	{
		if (frame->pool_current < frame->pool_count)
		{
			VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			VkDescriptorSet set = VK_NULL_HANDLE;
			VkResult result;

			info.descriptorPool = frame->pools[frame->pool_current];
			info.descriptorSetCount = 1;
			info.pSetLayouts = &B.draw_set_layout;
			result = vkAllocateDescriptorSets(B.device, &info, &set);
			if (result == VK_SUCCESS)
				return set;
			if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
			{
				HOST_VK_CHECK(result);
				return VK_NULL_HANDLE;
			}
			frame->pool_current++;
		}
		else
		{
			VkDescriptorPoolSize sizes[2];
			VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			VkDescriptorPool pool;

			sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			sizes[0].descriptorCount = 3 * POOL_SETS;
			sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			sizes[1].descriptorCount = 4 * POOL_SETS;
			info.maxSets = POOL_SETS;
			info.poolSizeCount = 2;
			info.pPoolSizes = sizes;
			if (!HOST_VK_CHECK(vkCreateDescriptorPool(B.device, &info, NULL, &pool)))
				return VK_NULL_HANDLE;
			if (frame->pool_count == frame->pool_capacity)
			{
				unsigned capacity = frame->pool_capacity ? frame->pool_capacity * 2 : 8;
				VkDescriptorPool *pools = realloc(frame->pools, sizeof(*pools) * capacity);

				if (!pools)
				{
					vkDestroyDescriptorPool(B.device, pool, NULL);
					return VK_NULL_HANDLE;
				}
				frame->pools = pools;
				frame->pool_capacity = capacity;
			}
			frame->pools[frame->pool_count++] = pool;
		}
	}
}

/* ---------- a draw */

static void skipped(unsigned *counter, const char *why)
{
	static unsigned said;

	(*counter)++;
	if (said++ < 4)
		host_logf(HOST_LOG_WARN, "vk: a draw is skipped: %s", why);
}

void host_vk_command_draw(const struct vk_command_draw *draw, uint32_t size)
{
	struct host_vk_frame *frame = &B.frames[B.frame];
	VkPipeline pipeline;
	VkCommandBuffer command;
	VkBuffer buffers[VK_DRAW_STREAMS], uniform_buffers[3], index_buffer = VK_NULL_HANDLE;
	VkDeviceSize offsets[VK_DRAW_STREAMS], uniform_offsets[3], index_offset = 0;
	VkDescriptorSet set;
	VkDescriptorBufferInfo buffer_infos[3];
	VkDescriptorImageInfo image_infos[VK_DRAW_TEXTURE_STAGES];
	VkWriteDescriptorSet writes[7];
	VkViewport viewport;
	VkRect2D scissor;
	int64_t x0, y0, x1, y1;
	unsigned index;
	float depth_min, depth_max;

	(void)size;
	if (draw->state.binding_count > VK_DRAW_STREAMS)
	{
		skipped(&B.counts.draws_skipped_other, "more bindings than a draw can have");
		return;
	}
	pipeline = host_vk_pipeline_find(draw->vertex_shader, draw->pixel_shader, &draw->state);
	if (!pipeline)
		return;
	if (!B.color && !B.depth)
	{
		B.counts.draws_skipped_target++;
		return;
	}
	/* a pipeline must match the rendering it is used in: the attachments it has are the ones bound */
	if ((draw->state.color_format != 0) != (B.color != NULL) || (draw->state.depth_format != 0) != (B.depth != NULL))
	{
		skipped(&B.counts.draws_skipped_other, "its attachments are not the ones bound");
		return;
	}
	if (draw->viewport[2] <= 0.0f || draw->viewport[3] <= 0.0f)
	{
		B.counts.draws_skipped_other++;
		return;
	}
	if (!host_vk_data_find(draw->vertex_constants.id, draw->vertex_constants.offset, sizeof(struct vk_vertex_constants),
			&uniform_buffers[0], &uniform_offsets[0]) ||
		!host_vk_data_find(draw->vertex_parameters.id, draw->vertex_parameters.offset, sizeof(struct vk_vertex_parameters),
			&uniform_buffers[1], &uniform_offsets[1]) ||
		!host_vk_data_find(draw->pixel_parameters.id, draw->pixel_parameters.offset, sizeof(struct vk_pixel_parameters),
			&uniform_buffers[2], &uniform_offsets[2]))
	{
		B.counts.draws_skipped_data++;
		return;
	}
	for (index = 0; index < draw->state.binding_count; index++)
	{
		if (!host_vk_data_find(draw->vertex_buffers[index].id, draw->vertex_buffers[index].offset, 1, &buffers[index],
			&offsets[index]))
		{
			B.counts.draws_skipped_data++;
			return;
		}
	}
	if (draw->indexed && !host_vk_data_find(draw->index_data.id, draw->index_data.offset, draw->count * 2, &index_buffer,
		&index_offset))
	{
		B.counts.draws_skipped_data++;
		return;
	}
	/* the textures written since are made readable first: outside a rendering */
	host_vk_textures_flush();
	if (!dummies_ensure())
	{
		B.counts.draws_skipped_other++;
		return;
	}
	if (!host_vk_rendering_begin())
	{
		B.counts.draws_skipped_target++;
		return;
	}
	command = host_vk_frame_command();
	set = set_allocate(frame);
	if (!set)
	{
		B.counts.draws_skipped_other++;
		return;
	}

	memset(writes, 0, sizeof(writes));
	for (index = 0; index < 3; index++)
	{
		buffer_infos[index].buffer = uniform_buffers[index];
		buffer_infos[index].offset = uniform_offsets[index];
		buffer_infos[index].range = index == 0 ? sizeof(struct vk_vertex_constants) :
			index == 1 ? sizeof(struct vk_vertex_parameters) : sizeof(struct vk_pixel_parameters);
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = VK_BINDING_VERTEX_CONSTANTS + index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[index].pBufferInfo = &buffer_infos[index];
	}
	for (index = 0; index < VK_DRAW_TEXTURE_STAGES; index++)
	{
		const struct vk_draw_texture *texture = &draw->textures[index];
		int type = texture->sampler_type >= 1 && texture->sampler_type <= 3 ? (int)texture->sampler_type - 1 : 0;
		struct vk_sampler_state none;

		const struct host_vk_image *image = texture->kind == VK_TEXTURE_IMAGE ? host_vk_image_get(texture->id) : NULL;

		/* a stage with no texture, or one whose image is not there or is not the type the shader samples, is a dummy of
		the shader's type */
		memset(&none, 0, sizeof(none));
		if (texture->kind != VK_TEXTURE_NONE && (!image || image->kind != texture->sampler_type))
		{
			B.counts.draws_texture_missing++;
			image = NULL;
		}
		image_infos[index].sampler = sampler_get(image ? &texture->sampler : &none);
		image_infos[index].imageView = image ? image->view : dummies.view[type];
		image_infos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[3 + index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[3 + index].dstSet = set;
		writes[3 + index].dstBinding = VK_BINDING_TEXTURE0 + index;
		writes[3 + index].descriptorCount = 1;
		writes[3 + index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[3 + index].pImageInfo = &image_infos[index];
		if (!image_infos[index].sampler)
		{
			B.counts.draws_skipped_other++;
			return;
		}
	}
	vkUpdateDescriptorSets(B.device, 7, writes, 0, NULL);

	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, B.draw_layout, 0, 1, &set, 0, NULL);

	/* the viewport in the target's pixels with a negative height (y from the bottom of its rectangle, the height negated:
	Vulkan 1.1's rule): the one place the picture is turned, since the shaders make Direct3D's clip space. Set with the
	scissor at every draw. Direct3D's half pixel is the vertex shaders', not added here. */
	depth_min = draw->viewport[4] < 0.0f ? 0.0f : draw->viewport[4] > 1.0f ? 1.0f : draw->viewport[4];
	depth_max = draw->viewport[5] < 0.0f ? 0.0f : draw->viewport[5] > 1.0f ? 1.0f : draw->viewport[5];
	viewport.x = draw->viewport[0];
	viewport.y = draw->viewport[1] + draw->viewport[3];
	viewport.width = draw->viewport[2];
	viewport.height = -draw->viewport[3];
	viewport.minDepth = depth_min;
	viewport.maxDepth = depth_max;
	vkCmdSetViewport(command, 0, 1, &viewport);
	/* the scissor follows the viewport, within the render area; one with no area is as GL has it, off */
	x0 = draw->scissor[0];
	y0 = draw->scissor[1];
	x1 = x0 + draw->scissor[2];
	y1 = y0 + draw->scissor[3];
	if (draw->scissor[2] <= 0 || draw->scissor[3] <= 0)
	{
		x0 = y0 = 0;
		x1 = B.area_width;
		y1 = B.area_height;
	}
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int64_t)B.area_width)
		x1 = B.area_width;
	if (y1 > (int64_t)B.area_height)
		y1 = B.area_height;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	scissor.offset.x = (int32_t)x0;
	scissor.offset.y = (int32_t)y0;
	scissor.extent.width = (uint32_t)(x1 - x0);
	scissor.extent.height = (uint32_t)(y1 - y0);
	vkCmdSetScissor(command, 0, 1, &scissor);
	vkCmdSetDepthBias(command, draw->depth_bias_constant, 0.0f, draw->depth_bias_slope);
	vkCmdSetBlendConstants(command, draw->blend_constants);
	vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, draw->stencil_compare_mask);
	vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, draw->stencil_write_mask);
	vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, draw->stencil_reference);

	vkCmdBindVertexBuffers(command, 0, draw->state.binding_count, buffers, offsets);
	if (draw->indexed)
	{
		vkCmdBindIndexBuffer(command, index_buffer, index_offset, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(command, draw->count, 1, 0, draw->vertex_offset, 0);
	}
	else
	{
		vkCmdDraw(command, draw->count, 1, 0, 0);
	}
	B.counts.draws_made++;
}
