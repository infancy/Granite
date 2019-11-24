/* Copyright (c) 2017-2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "clusterer.hpp"
#include "render_graph.hpp"
#include "scene.hpp"
#include "render_context.hpp"
#include "renderer.hpp"
#include "application_wsi_events.hpp"
#include "quirks.hpp"
#include "muglm/matrix_helper.hpp"
#include "thread_group.hpp"
#include <string.h>

using namespace Vulkan;
using namespace std;

namespace Granite
{
LightClusterer::LightClusterer()
{
	EVENT_MANAGER_REGISTER_LATCH(LightClusterer, on_device_created, on_device_destroyed, DeviceCreatedEvent);
	for (unsigned i = 0; i < MaxLights; i++)
	{
		points.index_remap[i] = i;
		spots.index_remap[i] = i;
	}
}

void LightClusterer::on_device_created(const Vulkan::DeviceCreatedEvent &e)
{
	auto &shader_manager = e.get_device().get_shader_manager();
	program = shader_manager.register_compute("builtin://shaders/lights/clustering.comp");
	inherit_variant = program->register_variant({{ "INHERIT", 1 }});
	cull_variant = program->register_variant({});
}

void LightClusterer::on_device_destroyed(const Vulkan::DeviceCreatedEvent &)
{
	program = nullptr;
	inherit_variant = 0;
	cull_variant = 0;

	spots.atlas.reset();
	points.atlas.reset();
	scratch_vsm_rt.reset();
	scratch_vsm_down.reset();
	for (auto &rt : shadow_atlas_rt)
		rt.reset();

	fill(begin(spots.cookie), end(spots.cookie), 0);
	fill(begin(points.cookie), end(points.cookie), 0);
}

void LightClusterer::set_scene(Scene *scene_)
{
	scene = scene_;
	lights = &scene->get_entity_pool().get_component_group<PositionalLightComponent, RenderInfoComponent>();
}

void LightClusterer::set_resolution(unsigned x, unsigned y, unsigned z)
{
	resolution_x = x;
	resolution_y = y;
	resolution_z = z;
}

void LightClusterer::set_shadow_resolution(unsigned res)
{
	shadow_resolution = res;
}

void LightClusterer::setup_render_pass_dependencies(RenderGraph &, RenderPass &target_)
{
	// TODO: Other passes might want this?
	target_.add_texture_input("light-cluster");
}

void LightClusterer::set_base_render_context(const RenderContext *context_)
{
	context = context_;
}

void LightClusterer::setup_render_pass_resources(RenderGraph &graph)
{
	target = &graph.get_physical_texture_resource(graph.get_texture_resource("light-cluster").get_physical_index());
	if (!ImplementationQuirks::get().clustering_list_iteration && !ImplementationQuirks::get().clustering_force_cpu)
		pre_cull_target = &graph.get_physical_texture_resource(graph.get_texture_resource("light-cluster-prepass").get_physical_index());
}

unsigned LightClusterer::get_active_point_light_count() const
{
	return points.count;
}

unsigned LightClusterer::get_active_spot_light_count() const
{
	return spots.count;
}

const PositionalFragmentInfo *LightClusterer::get_active_point_lights() const
{
	return points.lights;
}

const mat4 *LightClusterer::get_active_spot_light_shadow_matrices() const
{
	return spots.transforms;
}

const PointTransform *LightClusterer::get_active_point_light_shadow_transform() const
{
	return points.transforms;
}

const PositionalFragmentInfo *LightClusterer::get_active_spot_lights() const
{
	return spots.lights;
}

void LightClusterer::set_enable_clustering(bool enable)
{
	enable_clustering = enable;
}

void LightClusterer::set_shadow_type(ShadowType shadow_type_)
{
	shadow_type = shadow_type_;
}

void LightClusterer::set_enable_shadows(bool enable)
{
	enable_shadows = enable;
}

void LightClusterer::set_force_update_shadows(bool enable)
{
	force_update_shadows = enable;
}

const Vulkan::ImageView *LightClusterer::get_cluster_image() const
{
	return enable_clustering ? target : nullptr;
}

const Vulkan::Buffer *LightClusterer::get_cluster_list_buffer() const
{
	return enable_clustering && cluster_list ? cluster_list.get() : nullptr;
}

const Vulkan::ImageView *LightClusterer::get_spot_light_shadows() const
{
	return (enable_shadows && spots.atlas) ? &spots.atlas->get_view() : nullptr;
}

const Vulkan::ImageView *LightClusterer::get_point_light_shadows() const
{
	return (enable_shadows && points.atlas) ? &points.atlas->get_view() : nullptr;
}

const mat4 &LightClusterer::get_cluster_transform() const
{
	return cluster_transform;
}

template <typename T>
static uint32_t reassign_indices(T &type)
{
	uint32_t partial_mask = 0;

	for (unsigned i = 0; i < type.count; i++)
	{
		// Try to inherit shadow information from some other index.
		auto itr = find_if(begin(type.cookie), end(type.cookie), [=](unsigned cookie) {
			return cookie == type.handles[i]->get_cookie();
		});

		if (itr != end(type.cookie))
		{
			auto index = std::distance(begin(type.cookie), itr);
			if (i != unsigned(index))
			{
				// Reuse the shadow data from the atlas.
				swap(type.cookie[i], type.cookie[index]);
				swap(type.transforms[i], type.transforms[index]);
				swap(type.index_remap[i], type.index_remap[index]);
			}
		}

		// Try to find an atlas slot which has never been used.
		if (type.handles[i]->get_cookie() != type.cookie[i] && type.cookie[i] != 0)
		{
			auto cookie_itr = find(begin(type.cookie), end(type.cookie), 0);

			if (cookie_itr != end(type.cookie))
			{
				auto index = std::distance(begin(type.cookie), cookie_itr);
				if (i != unsigned(index))
				{
					// Reuse the shadow data from the atlas.
					swap(type.cookie[i], type.cookie[index]);
					swap(type.transforms[i], type.transforms[index]);
					swap(type.index_remap[i], type.index_remap[index]);
				}
			}
		}

		if (type.handles[i]->get_cookie() != type.cookie[i])
			partial_mask |= 1u << i;
		else
			type.handles[i]->set_shadow_info(&type.atlas->get_view(), type.transforms[i]);

		type.cookie[i] = type.handles[i]->get_cookie();
	}

	return partial_mask;
}

void LightClusterer::render_shadow(Vulkan::CommandBuffer &cmd, RenderContext &depth_context, VisibilityList &visible,
                                   unsigned off_x, unsigned off_y, unsigned res_x, unsigned res_y,
                                   Vulkan::ImageView &rt, Renderer::RendererFlushFlags flags)
{
	bool vsm = shadow_type == ShadowType::VSM;
	visible.clear();
	scene->gather_visible_static_shadow_renderables(depth_context.get_visibility_frustum(), visible);

	depth_renderer->set_mesh_renderer_options(vsm ? Renderer::POSITIONAL_LIGHT_SHADOW_VSM_BIT : 0);
	depth_renderer->begin();
	depth_renderer->push_depth_renderables(depth_context, visible);

	if (vsm)
	{
		auto image_info = ImageCreateInfo::render_target(shadow_resolution, shadow_resolution, VK_FORMAT_R32G32_SFLOAT);
		image_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (!scratch_vsm_rt)
			scratch_vsm_rt = cmd.get_device().create_image(image_info, nullptr);
		if (!scratch_vsm_down)
		{
			image_info.width >>= 1;
			image_info.height >>= 1;
			scratch_vsm_down = cmd.get_device().create_image(image_info, nullptr);
		}

		RenderPassInfo rp;
		rp.op_flags = RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT;
		rp.clear_attachments = 1 << 0;
		rp.store_attachments = 1 << 1;
		rp.color_attachments[0] = &cmd.get_device().get_transient_attachment(shadow_resolution, shadow_resolution, VK_FORMAT_R32G32_SFLOAT, 0, 4);
		rp.color_attachments[1] = &scratch_vsm_rt->get_view();
		rp.num_color_attachments = 2;
		rp.depth_stencil = &cmd.get_device().get_transient_attachment(shadow_resolution, shadow_resolution, VK_FORMAT_D16_UNORM, 0, 4);
		rp.clear_depth_stencil.depth = 1.0f;
		rp.clear_depth_stencil.stencil = 0;

		float z_far = depth_context.get_render_parameters().z_far;
		rp.clear_color[0].float32[0] = z_far;
		rp.clear_color[0].float32[1] = z_far * z_far;

		RenderPassInfo::Subpass subpass = {};
		subpass.num_color_attachments = 1;
		subpass.num_resolve_attachments = 1;
		subpass.depth_stencil_mode = RenderPassInfo::DepthStencil::ReadWrite;
		subpass.color_attachments[0] = 0;
		subpass.resolve_attachments[0] = 1;
		rp.num_subpasses = 1;
		rp.subpasses = &subpass;

		cmd.image_barrier(*scratch_vsm_rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		cmd.image_barrier(*scratch_vsm_down, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		cmd.begin_render_pass(rp);
		depth_renderer->flush(cmd, depth_context, flags);
		cmd.end_render_pass();

		cmd.image_barrier(*scratch_vsm_rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

		{
			RenderPassInfo rp_vert = {};
			rp_vert.num_color_attachments = 1;
			rp_vert.store_attachments = 1 << 0;
			rp_vert.color_attachments[0] = &scratch_vsm_down->get_view();
			cmd.begin_render_pass(rp_vert);
			cmd.set_texture(0, 0, scratch_vsm_rt->get_view(), StockSampler::LinearClamp);
			vec2 inv_size(1.0f / scratch_vsm_rt->get_create_info().width,
			              1.0f / scratch_vsm_rt->get_create_info().height);
			cmd.push_constants(&inv_size, 0, sizeof(inv_size));
			CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                        "builtin://shaders/post/vsm_down_blur.frag");
			cmd.end_render_pass();
		}

		cmd.image_barrier(*scratch_vsm_down, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

		{
			RenderPassInfo rp_horiz = {};
			rp_horiz.num_color_attachments = 1;
			rp_horiz.store_attachments = 1 << 0;
			rp_horiz.color_attachments[0] = &rt;
			rp_horiz.render_area.offset.x = off_x;
			rp_horiz.render_area.offset.y = off_y;
			rp_horiz.render_area.extent.width = res_x;
			rp_horiz.render_area.extent.height = res_y;

			cmd.begin_render_pass(rp_horiz);
			cmd.set_viewport({ float(off_x), float(off_y), float(res_x), float(res_y), 0.0f, 1.0f });
			cmd.set_scissor({{ int(off_x), int(off_y) }, { res_x, res_y }});
			vec2 inv_size(1.0f / scratch_vsm_down->get_create_info().width,
			              1.0f / scratch_vsm_down->get_create_info().height);
			cmd.push_constants(&inv_size, 0, sizeof(inv_size));
			cmd.set_texture(0, 0, scratch_vsm_down->get_view(), StockSampler::LinearClamp);
			CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                        "builtin://shaders/post/vsm_up_blur.frag");
			cmd.end_render_pass();
		}
	}
	else
	{
		RenderPassInfo rp;
		rp.op_flags = RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT |
		              RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
		rp.num_color_attachments = 0;
		rp.depth_stencil = &rt;
		rp.clear_depth_stencil.depth = 1.0f;
		rp.render_area.offset.x = off_x;
		rp.render_area.offset.y = off_y;
		rp.render_area.extent.width = res_x;
		rp.render_area.extent.height = res_y;

		cmd.begin_render_pass(rp);
		cmd.set_viewport({ float(off_x), float(off_y), float(res_x), float(res_y), 0.0f, 1.0f });
		cmd.set_scissor({{ int(off_x), int(off_y) }, { res_x, res_y }});
		depth_renderer->flush(cmd, depth_context, flags);
		cmd.end_render_pass();
	}
}

void LightClusterer::render_atlas_point(RenderContext &context_)
{
	bool vsm = shadow_type == ShadowType::VSM;
	uint32_t partial_mask = reassign_indices(points);

	if (!points.atlas || force_update_shadows)
		partial_mask = ~0u;

	if (partial_mask == 0 && points.atlas && !force_update_shadows)
		return;

	bool partial_update = partial_mask != ~0u;
	auto &device = context_.get_device();
	auto cmd = device.request_command_buffer();

	if (!points.atlas)
	{
		auto format = vsm ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_D16_UNORM;
		ImageCreateInfo info = ImageCreateInfo::render_target(shadow_resolution, shadow_resolution, format);
		info.layers = 6 * MaxLights;
		info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		info.initial_layout = vsm ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

		if (vsm)
			info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		else
			info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

		points.atlas = device.create_image(info, nullptr);

		for (unsigned i = 0; i < 6 * MaxLights; i++)
		{
			ImageViewCreateInfo view;
			view.image = points.atlas.get();
			view.layers = 1;
			view.base_layer = i;
			shadow_atlas_rt[i] = device.create_image_view(view);
		}
	}
	else if (partial_update)
	{
		VkImageMemoryBarrier barriers[32];
		unsigned barrier_count = 0;

		Util::for_each_bit(partial_mask, [&](unsigned bit) {
			auto &b = barriers[barrier_count++];
			b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.image = points.atlas->get_image();
			b.srcAccessMask = 0;

			if (vsm)
			{
				b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
				                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			}
			else
			{
				b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
				                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			b.subresourceRange.baseArrayLayer = 6u * points.index_remap[bit];
			b.subresourceRange.layerCount = 6;
			b.subresourceRange.levelCount = 1;
		});

		if (vsm)
		{
			cmd->barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			             0, nullptr, 0, nullptr, barrier_count, barriers);
		}
		else
		{
			cmd->barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			             0, nullptr, 0, nullptr, barrier_count, barriers);
		}
	}
	else if (vsm)
	{
		cmd->image_barrier(*points.atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	}
	else
	{
		cmd->image_barrier(*points.atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	}

	RenderContext depth_context;
	VisibilityList visible;

	for (unsigned i = 0; i < points.count; i++)
	{
		if ((partial_mask & (1u << i)) == 0)
			continue;

		LOGI("Rendering shadow for point light %u (%p)\n", i, static_cast<void *>(points.handles[i]));

		unsigned remapped = points.index_remap[i];

		for (unsigned face = 0; face < 6; face++)
		{
			mat4 view, proj;
			compute_cube_render_transform(points.lights[i].position, face, proj, view,
			                              0.005f / points.lights[i].inv_radius,
			                              1.0f / points.lights[i].inv_radius);
			depth_context.set_camera(proj, view);

			if (face == 0)
			{
				points.transforms[i].transform = vec4(proj[2].zw(), proj[3].zw());
				points.transforms[i].slice.x = float(remapped);
				points.handles[i]->set_shadow_info(&points.atlas->get_view(), points.transforms[i]);
			}

			render_shadow(*cmd, depth_context, visible,
			              0, 0, shadow_resolution, shadow_resolution,
			              *shadow_atlas_rt[6 * remapped + face],
			              Renderer::FRONT_FACE_CLOCKWISE_BIT | Renderer::DEPTH_BIAS_BIT);
		}
	}

	if (partial_update)
	{
		VkImageMemoryBarrier barriers[32];
		unsigned barrier_count = 0;

		Util::for_each_bit(partial_mask, [&](unsigned bit) {
			auto &b = barriers[barrier_count++];
			b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.image = points.atlas->get_image();

			if (vsm)
			{
				b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			}
			else
			{
				b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.subresourceRange.baseArrayLayer = 6u * points.index_remap[bit];
			b.subresourceRange.layerCount = 6;
			b.subresourceRange.levelCount = 1;
		});

		cmd->barrier(vsm ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		             0, nullptr, 0, nullptr, barrier_count, barriers);
	}
	else if (vsm)
	{
		cmd->image_barrier(*points.atlas, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	else
	{
		cmd->image_barrier(*points.atlas, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}

	device.submit(cmd);
}

void LightClusterer::render_atlas_spot(RenderContext &context_)
{
	bool vsm = shadow_type == ShadowType::VSM;
	uint32_t partial_mask = reassign_indices(spots);

	if (!spots.atlas || force_update_shadows)
		partial_mask = ~0u;

	if (partial_mask == 0 && spots.atlas && !force_update_shadows)
		return;

	auto &device = context_.get_device();
	auto cmd = device.request_command_buffer();

	if (!spots.atlas)
	{
		auto format = vsm ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_D16_UNORM;
		ImageCreateInfo info = ImageCreateInfo::render_target(shadow_resolution * 8, shadow_resolution * 4, format);
		info.initial_layout = vsm ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

		if (vsm)
			info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		else
			info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		spots.atlas = device.create_image(info, nullptr);

		// Make sure we have a cleared atlas so we don't spuriously filter against NaN.
		if (vsm)
		{
			cmd->image_barrier(*spots.atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
			                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			cmd->clear_image(*spots.atlas, {});
			cmd->image_barrier(*spots.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		}
	}
	else
	{
		// Preserve data if we're not overwriting the entire shadow atlas.
		auto access = vsm ?
		              (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) :
		              (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
		auto stages = vsm ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT :
		              (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

		VkImageLayout layout = vsm ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		cmd->image_barrier(*spots.atlas,
		                   partial_mask != ~0u ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, layout,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                   stages, access);
	}

	RenderContext depth_context;
	VisibilityList visible;

	for (unsigned i = 0; i < spots.count; i++)
	{
		if ((partial_mask & (1u << i)) == 0)
			continue;

		LOGI("Rendering shadow for spot light %u (%p)\n", i, static_cast<void *>(spots.handles[i]));

		float range = tan(spots.handles[i]->get_xy_range());
		mat4 view = mat4_cast(look_at_arbitrary_up(spots.lights[i].direction)) *
		            translate(-spots.lights[i].position);
		mat4 proj = projection(range * 2.0f, 1.0f,
		                       0.005f / spots.lights[i].inv_radius,
		                       1.0f / spots.lights[i].inv_radius);

		unsigned remapped = spots.index_remap[i];

		// Carve out the atlas region where the spot light shadows live.
		spots.transforms[i] =
				translate(vec3(float(remapped & 7) / 8.0f, float(remapped >> 3) / 4.0f, 0.0f)) *
				scale(vec3(1.0f / 8.0f, 1.0f / 4.0f, 1.0f)) *
				translate(vec3(0.5f, 0.5f, 0.0f)) *
				scale(vec3(0.5f, 0.5f, 1.0f)) *
				proj * view;

		spots.handles[i]->set_shadow_info(&spots.atlas->get_view(), spots.transforms[i]);

		depth_context.set_camera(proj, view);

		render_shadow(*cmd, depth_context, visible,
		              shadow_resolution * (remapped & 7), shadow_resolution * (remapped >> 3),
		              shadow_resolution, shadow_resolution,
		              spots.atlas->get_view(), Renderer::DEPTH_BIAS_BIT);
	}

	if (vsm)
	{
		cmd->image_barrier(*spots.atlas, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	else
	{
		cmd->image_barrier(*spots.atlas, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}

	device.submit(cmd);
}

void LightClusterer::refresh(RenderContext &context_)
{
	points.count = 0;
	spots.count = 0;
	auto &frustum = context_.get_visibility_frustum();

	for (auto &light : *lights)
	{
		auto &l = *get_component<PositionalLightComponent>(light)->light;
		auto *transform = get_component<RenderInfoComponent>(light);

		// Frustum cull lights here.
		if (!frustum.intersects(transform->world_aabb))
			continue;

		if (l.get_type() == PositionalLight::Type::Spot)
		{
			auto &spot = static_cast<SpotLight &>(l);
			spot.set_shadow_info(nullptr, {});
			if (spots.count < max_spot_lights)
			{
				spots.lights[spots.count] = spot.get_shader_info(transform->transform->world_transform);
				spots.handles[spots.count] = &spot;
				spots.count++;
			}
		}
		else if (l.get_type() == PositionalLight::Type::Point)
		{
			auto &point = static_cast<PointLight &>(l);
			point.set_shadow_info(nullptr, {});
			if (points.count < max_point_lights)
			{
				points.lights[points.count] = point.get_shader_info(transform->transform->world_transform);
				points.handles[points.count] = &point;
				points.count++;
			}
		}
	}

	// Figure out aabb bounds in view space.
	auto &inv_proj = context_.get_render_parameters().inv_projection;
	const auto project = [](const vec4 &v) -> vec3 {
		return v.xyz() / v.w;
	};

	vec3 ul = project(inv_proj * vec4(-1.0f, -1.0f, 1.0f, 1.0f));
	vec3 ll = project(inv_proj * vec4(-1.0f, +1.0f, 1.0f, 1.0f));
	vec3 ur = project(inv_proj * vec4(+1.0f, -1.0f, 1.0f, 1.0f));
	vec3 lr = project(inv_proj * vec4(+1.0f, +1.0f, 1.0f, 1.0f));

	vec3 min_view = min(min(ul, ll), min(ur, lr));
	vec3 max_view = max(max(ul, ll), max(ur, lr));
	// Make sure scaling the box does not move the near plane.
	max_view.z = 0.0f;

	mat4 ortho_box = ortho(AABB(min_view, max_view));

	if (points.count || spots.count)
		cluster_transform = scale(vec3(1 << (ClusterHierarchies - 1))) * ortho_box * context_.get_render_parameters().view;
	else
		cluster_transform = scale(vec3(0.0f, 0.0f, 0.0f));

	if (enable_shadows)
	{
		render_atlas_spot(context_);
		render_atlas_point(context_);
	}
	else
	{
		spots.atlas.reset();
		points.atlas.reset();
	}
}

uvec2 LightClusterer::cluster_lights_cpu(int x, int y, int z, const CPUGlobalAccelState &state,
                                         const CPULocalAccelState &local_state, float scale, uvec2 pre_mask)
{
	uint32_t spot_mask = 0;
	uint32_t point_mask = 0;

	vec3 view_space = vec3(2.0f, 2.0f, 0.5f) *
	                  (vec3(x, y, z) + vec3(0.5f * scale)) *
	                  state.inv_res +
	                  vec3(-1.0f, -1.0f, local_state.z_bias);
	view_space *= local_state.world_scale_factor;
	vec3 cube_center = (state.inverse_cluster_transform * vec4(view_space, 1.0f)).xyz();
	float cube_radius = local_state.cube_radius * scale;

	while (pre_mask.x)
	{
		unsigned i = trailing_zeroes(pre_mask.x);
		pre_mask.x &= ~(1u << i);

		// Sphere/cone culling from https://bartwronski.com/2017/04/13/cull-that-cone/.
		vec3 V = cube_center - state.spot_position[i];
		float V_sq = dot(V, V);
		float V1_len  = dot(V, state.spot_direction[i]);

		if (V1_len > cube_radius + state.spot_size[i])
			continue;
		if (-V1_len > cube_radius)
			continue;

		float V2_len = sqrtf(std::max(V_sq - V1_len * V1_len, 0.0f));
		float distance_closest_point = state.spot_angle_cos[i] * V2_len - state.spot_angle_sin[i] * V1_len;

		if (distance_closest_point > cube_radius)
			continue;

		spot_mask |= 1u << i;
	}

	while (pre_mask.y)
	{
		unsigned i = trailing_zeroes(pre_mask.y);
		pre_mask.y &= ~(1u << i);

		vec3 cube_center_dist = cube_center - state.point_position[i];
		float radial_dist_sqr = dot(cube_center_dist, cube_center_dist);

		float cutoff = state.point_size[i] + cube_radius;
		cutoff *= cutoff;
		bool radial_inside = radial_dist_sqr <= cutoff;
		if (radial_inside)
			point_mask |= 1u << i;
	}

	return uvec2(spot_mask, point_mask);
}

void LightClusterer::build_cluster_cpu(Vulkan::CommandBuffer &cmd, Vulkan::ImageView &view)
{
	unsigned res_x = resolution_x;
	unsigned res_y = resolution_y;
	unsigned res_z = resolution_z;

#ifdef CLUSTERER_FORCE_TRANSFER_UPDATE
	auto &image = view.get_image();
	auto *image_data = static_cast<uvec4 *>(cmd.update_image(image, 0, 0));
#else
	// Copy to image using a compute pipeline so we know how it's implemented.
	BufferCreateInfo compute_staging_info = {};
	compute_staging_info.domain = BufferDomain::Host;
	compute_staging_info.size = res_x * res_y * res_z * (ClusterHierarchies + 1) * sizeof(uvec4);
	compute_staging_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	auto compute_staging = cmd.get_device().create_buffer(compute_staging_info, nullptr);
	auto *image_data = static_cast<uvec4 *>(cmd.get_device().map_host_buffer(*compute_staging, MEMORY_ACCESS_WRITE_BIT));

	{
		auto *copy_program = cmd.get_device().get_shader_manager().register_compute(
				"builtin://shaders/util/copy_buffer_to_image_3d.comp");
		auto variant = copy_program->register_variant({});
		cmd.set_program(copy_program->get_program(variant));
		cmd.set_storage_texture(0, 0, view);
		cmd.set_storage_buffer(0, 1, *compute_staging);

		struct Push
		{
			uint dim_x;
			uint dim_y;
			uint row_stride;
			uint height_stride;
		};

		const Push push = {
			res_x, res_y,
			res_x, res_x * res_y,
		};

		cmd.push_constants(&push, 0, sizeof(push));
		cmd.dispatch((res_x + 7) / 8, (res_y + 7) / 8, res_z * (ClusterHierarchies + 1));
	}
#endif

	cluster_list_buffer.clear();

	auto &workers = *Global::thread_group();
	auto task = workers.create_task();

	// Naive and simple multithreading :)
	// Pre-compute useful data structures before we go wide ...
	CPUGlobalAccelState state;
	state.inverse_cluster_transform = inverse(cluster_transform);
	state.inv_res = vec3(1.0f / res_x, 1.0f / res_y, 1.0f / res_z);
	state.radius = 0.5f * length(mat3(state.inverse_cluster_transform) * (vec3(2.0f, 2.0f, 0.5f) * state.inv_res));

	for (unsigned i = 0; i < spots.count; i++)
	{
		state.spot_position[i] = spots.lights[i].position;
		state.spot_direction[i] = spots.lights[i].direction;
		state.spot_size[i] = 1.0f / spots.lights[i].inv_radius;
		state.spot_angle_cos[i] = cosf(spots.handles[i]->get_xy_range());
		state.spot_angle_sin[i] = sinf(spots.handles[i]->get_xy_range());
	}

	for (unsigned i = 0; i < points.count; i++)
	{
		state.point_position[i] = points.lights[i].position;
		state.point_size[i] = 1.0f / points.lights[i].inv_radius;
	}

	for (unsigned slice = 0; slice < ClusterHierarchies + 1; slice++)
	{
		float world_scale_factor;
		float z_bias;

		if (slice == 0)
		{
			world_scale_factor = 1.0f;
			z_bias = 0.0f;
		}
		else
		{
			world_scale_factor = exp2(float(slice - 1));
			z_bias = 0.5f;
		}

		for (unsigned cz = 0; cz < res_z; cz += ClusterPrepassDownsample)
		{
			// Four slices per task.
			task->enqueue_task([&, z_bias, world_scale_factor, slice, cz]() {
				CPULocalAccelState local_state;
				local_state.world_scale_factor = world_scale_factor;
				local_state.z_bias = z_bias;
				local_state.cube_radius = state.radius * world_scale_factor;

				uint32_t cached_spot_mask = 0;
				uint32_t cached_point_mask = 0;
				uvec4 cached_node = uvec4(0);

				vector<uint32_t> tmp_list_buffer;
				vector<uvec4> image_base;
				if (ImplementationQuirks::get().clustering_list_iteration)
					image_base.resize(ClusterPrepassDownsample * res_x * res_y);

				auto *image_output_base = &image_data[slice * res_z * res_y * res_x + cz * res_y * res_x];

				// Add a small guard band for safety.
				float range_z = z_bias + (0.5f * (cz + ClusterPrepassDownsample + 0.5f)) / res_z;
				int min_x = int(std::floor((0.5f - 0.5f * range_z) * res_x));
				int max_x = int(std::ceil((0.5f + 0.5f * range_z) * res_x));
				int min_y = int(std::floor((0.5f - 0.5f * range_z) * res_y));
				int max_y = int(std::ceil((0.5f + 0.5f * range_z) * res_y));

				min_x = clamp(min_x, 0, int(res_x));
				max_x = clamp(max_x, 0, int(res_x));
				min_y = clamp(min_y, 0, int(res_y));
				max_y = clamp(max_y, 0, int(res_y));

				uvec2 pre_mask((1ull << spots.count) - 1,
				               (1ull << points.count) - 1);

				for (int cy = min_y; cy < max_y; cy += ClusterPrepassDownsample)
				{
					for (int cx = min_x; cx < max_x; cx += ClusterPrepassDownsample)
					{
						int target_x = std::min(cx + ClusterPrepassDownsample, max_x);
						int target_y = std::min(cy + ClusterPrepassDownsample, max_y);

						auto res = cluster_lights_cpu(cx, cy, cz, state, local_state,
						                              float(ClusterPrepassDownsample),
						                              pre_mask);

						// No lights in large block? Quick eliminate.
						if (!res.x && !res.y)
						{
							if (!ImplementationQuirks::get().clustering_list_iteration)
							{
								for (int sz = 0; sz < 4; sz++)
									for (int sy = cy; sy < target_y; sy++)
										for (int sx = cx; sx < target_x; sx++)
											image_output_base[sz * res_y * res_x + sy * res_x + sx] = uvec4(0u);
							}
							continue;
						}

						for (int sz = 0; sz < 4; sz++)
						{
							for (int sy = cy; sy < target_y; sy++)
							{
								for (int sx = cx; sx < target_x; sx++)
								{
									auto final_res = cluster_lights_cpu(sx, sy, sz + int(cz), state, local_state, 1.0f, res);

									if (!ImplementationQuirks::get().clustering_list_iteration)
									{
										image_output_base[sz * res_y * res_x + sy * res_x + sx] = uvec4(final_res, 0u, 0u);
									}
									else if (cached_spot_mask == final_res.x && cached_point_mask == final_res.y)
									{
										// Neighbor blocks have a high likelihood of sharing the same lights,
										// try to conserve memory.
										image_base[sz * res_y * res_x + sy * res_x + sx] = cached_node;
									}
									else
									{
										uint32_t spot_count = 0;
										uint32_t point_count = 0;
										uint32_t spot_start = tmp_list_buffer.size();

										Util::for_each_bit(final_res.x, [&](uint32_t bit) {
											tmp_list_buffer.push_back(bit);
											spot_count++;
										});

										uint32_t point_start = tmp_list_buffer.size();

										Util::for_each_bit(final_res.y, [&](uint32_t bit) {
											tmp_list_buffer.push_back(bit);
											point_count++;
										});

										uvec4 node(spot_start, spot_count, point_start, point_count);
										image_base[sz * res_y * res_x + sy * res_x + sx] = node;
										cached_spot_mask = final_res.x;
										cached_point_mask = final_res.y;
										cached_node = node;
									}
								}
							}
						}
					}
				}

				if (ImplementationQuirks::get().clustering_list_iteration)
				{
					size_t cluster_offset = 0;
					{
						lock_guard<mutex> holder{cluster_list_lock};
						cluster_offset = cluster_list_buffer.size();
						cluster_list_buffer.resize(cluster_offset + tmp_list_buffer.size());
						memcpy(cluster_list_buffer.data() + cluster_offset, tmp_list_buffer.data(),
						       tmp_list_buffer.size() * sizeof(uint32_t));
					}

					unsigned elems = ClusterPrepassDownsample * res_x * res_y;
					for (unsigned i = 0; i < elems; i++)
						image_output_base[i] = image_base[i] + uvec4(cluster_offset, 0, cluster_offset, 0);
				}
			});
		}
	}

	task->flush();
	task->wait();

	if (!cluster_list_buffer.empty())
	{
		// Just allocate a fresh buffer every frame.
		BufferCreateInfo info = {};
		info.domain = BufferDomain::Device;
		info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		info.size = cluster_list_buffer.size() * sizeof(cluster_list_buffer[0]);
		cluster_list = cmd.get_device().create_buffer(info, cluster_list_buffer.data());
		//LOGI("Cluster list has %u elements.\n", unsigned(cluster_list_buffer.size()));
	}
	else if (ImplementationQuirks::get().clustering_list_iteration)
	{
		BufferCreateInfo info = {};
		info.domain = BufferDomain::Device;
		info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		info.size = sizeof(uvec4);
		static const uvec4 dummy(0u);
		cluster_list = cmd.get_device().create_buffer(info, &dummy);
	}
	else
		cluster_list.reset();
}

void LightClusterer::build_cluster(Vulkan::CommandBuffer &cmd, Vulkan::ImageView &view, const Vulkan::ImageView *pre_culled)
{
	unsigned res_x = resolution_x;
	unsigned res_y = resolution_y;
	unsigned res_z = resolution_z;
	if (!pre_culled)
	{
		res_x /= ClusterPrepassDownsample;
		res_y /= ClusterPrepassDownsample;
		res_z /= ClusterPrepassDownsample;
	}

	cmd.set_program(program->get_program(pre_culled ? inherit_variant : cull_variant));
	cmd.set_storage_texture(0, 0, view);
	if (pre_culled)
		cmd.set_texture(0, 1, *pre_culled, StockSampler::NearestWrap);

	auto *spot_buffer = cmd.allocate_typed_constant_data<PositionalFragmentInfo>(1, 0, MaxLights);
	auto *point_buffer = cmd.allocate_typed_constant_data<PositionalFragmentInfo>(1, 1, MaxLights);
	memcpy(spot_buffer, spots.lights, spots.count * sizeof(PositionalFragmentInfo));
	memcpy(point_buffer, points.lights, points.count * sizeof(PositionalFragmentInfo));

	auto *spot_lut_buffer = cmd.allocate_typed_constant_data<vec4>(1, 2, MaxLights);
	for (unsigned i = 0; i < spots.count; i++)
	{
		spot_lut_buffer[i] = vec4(cosf(spots.handles[i]->get_xy_range()),
		                          sinf(spots.handles[i]->get_xy_range()),
		                          1.0f / spots.lights[i].inv_radius,
		                          0.0f);
	}

	struct Push
	{
		mat4 inverse_cluster_transform;
		uvec4 size_z_log2;
		vec4 inv_texture_size;
		vec4 inv_size_radius;
		uint32_t spot_count;
		uint32_t point_count;
	};

	auto inverse_cluster_transform = inverse(cluster_transform);

	vec3 inv_res = vec3(1.0f / res_x, 1.0f / res_y, 1.0f / res_z);
	float radius = 0.5f * length(mat3(inverse_cluster_transform) * (vec3(2.0f, 2.0f, 0.5f) * inv_res));

	Push push = {
			inverse_cluster_transform,
			uvec4(res_x, res_y, res_z, trailing_zeroes(res_z)),
			vec4(1.0f / res_x, 1.0f / res_y, 1.0f / ((ClusterHierarchies + 1) * res_z), 1.0f),
			vec4(inv_res, radius),
			spots.count, points.count,
	};
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.dispatch((res_x + 3) / 4, (res_y + 3) / 4, (ClusterHierarchies + 1) * ((res_z + 3) / 4));
}

void LightClusterer::add_render_passes(RenderGraph &graph)
{
	AttachmentInfo att;
	att.levels = 1;
	att.layers = 1;
	att.format = VK_FORMAT_R32G32B32A32_UINT;
	att.samples = 1;
	att.size_class = SizeClass::Absolute;
	att.size_x = resolution_x;
	att.size_y = resolution_y;
	att.size_z = resolution_z * (ClusterHierarchies + 1);
	att.aux_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	att.persistent = true;

	if (ImplementationQuirks::get().clustering_list_iteration || ImplementationQuirks::get().clustering_force_cpu)
	{
#ifdef CLUSTERER_FORCE_TRANSFER_UPDATE
		auto &pass = graph.add_pass("clustering", RENDER_GRAPH_QUEUE_COMPUTE_BIT);
		pass.add_blit_texture_output("light-cluster", att);
#else
		auto &pass = graph.add_pass("clustering", RENDER_GRAPH_QUEUE_COMPUTE_BIT);
		pass.add_storage_texture_output("light-cluster", att);
#endif
		pass.set_build_render_pass([this](Vulkan::CommandBuffer &cmd) {
			build_cluster_cpu(cmd, *target);
		});

		pass.set_need_render_pass([this]() {
			return enable_clustering;
		});
	}
	else
	{
		att.format = VK_FORMAT_R32G32_UINT;

		AttachmentInfo att_prepass = att;
		assert((resolution_x % ClusterPrepassDownsample) == 0);
		assert((resolution_y % ClusterPrepassDownsample) == 0);
		assert((resolution_z % ClusterPrepassDownsample) == 0);
		assert((resolution_z & (resolution_z - 1)) == 0);
		att_prepass.size_x /= ClusterPrepassDownsample;
		att_prepass.size_y /= ClusterPrepassDownsample;
		att_prepass.size_z /= ClusterPrepassDownsample;

		auto &pass = graph.add_pass("clustering", RENDER_GRAPH_QUEUE_COMPUTE_BIT);
		pass.add_storage_texture_output("light-cluster", att);
		pass.add_storage_texture_output("light-cluster-prepass", att_prepass);
		pass.set_build_render_pass([this](Vulkan::CommandBuffer &cmd) {
			build_cluster(cmd, *pre_cull_target, nullptr);
			cmd.image_barrier(pre_cull_target->get_image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			                  VK_ACCESS_SHADER_WRITE_BIT,
			                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
			build_cluster(cmd, *target, pre_cull_target);
		});

		pass.set_need_render_pass([this]() {
			return enable_clustering;
		});
	}
}

void LightClusterer::set_base_renderer(Renderer *, Renderer *, Renderer *depth)
{
	depth_renderer = depth;
}
}
