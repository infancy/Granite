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

#include "deferred_lights.hpp"
#include "renderer.hpp"
#include "unstable_remove_if.hpp"
#include "lights.hpp"
#include <algorithm>
#include <float.h>

namespace Granite
{
void DeferredLights::refresh(RenderContext &context)
{
	if (!enable_clustered_stencil)
		return;

	visible.clear();
	scene->gather_visible_positional_lights(context.get_visibility_frustum(), visible,
	                                        max_spot_lights, max_point_lights);

	clips.clear();
	for (auto &cluster : clusters)
		cluster.clear();

	auto &params = context.get_render_parameters();

	// Lights which clip either near or far don't need double-sided testing.
	auto itr = Util::unstable_remove_if(begin(visible), end(visible), [&params, &context](const RenderableInfo &light) -> bool {
		vec2 range = static_cast<const PositionalLight *>(light.renderable)->get_z_range(context, light.transform->transform->world_transform);
		return range.x < params.z_near || range.y > params.z_far;
	});

	clips.insert(end(clips), itr, end(visible));
	visible.erase(itr, end(visible));

	if (visible.empty())
		return;

	// Find Z-range of all lights.
	float cluster_min = FLT_MAX;
	float cluster_max = 0.0f;
	for (auto &light : visible)
	{
		auto &aabb = light.transform->world_aabb;
		float to_center = dot(aabb.get_center() - params.camera_position, params.camera_front);
		cluster_min = min(to_center, cluster_min);
		cluster_max = max(to_center, cluster_max);
	}

	float cluster_range = cluster_max - cluster_min;
	cluster_range = max(cluster_range, 0.001f);
	float cluster_inv_range = float(NumClusters) / cluster_range;

	// Assign each renderable to a cluster index based on their position.
	for (auto &light : visible)
	{
		auto &aabb = light.transform->world_aabb;
		float to_center = dot(aabb.get_center() - params.camera_position, params.camera_front);
		int cluster_index = clamp(int((to_center - cluster_min) * cluster_inv_range), 0, NumClusters - 1);
		clusters[cluster_index].push_back(light);
	}
}

void DeferredLights::set_scene(Scene *scene_)
{
	scene = scene_;
}

void DeferredLights::set_renderers(Renderer *depth_renderer_, Renderer *deferred_renderer_)
{
	depth_renderer = depth_renderer_;
	deferred_renderer = deferred_renderer_;
}

void DeferredLights::render_prepass_lights(Vulkan::CommandBuffer &cmd, RenderContext &context)
{
	if (!enable_clustered_stencil)
		return;

	for (unsigned cluster = 0; cluster < NumClusters; cluster++)
	{
		depth_renderer->begin();
		depth_renderer->push_depth_renderables(context, clusters[cluster]);
		depth_renderer->set_stencil_reference(0xff, 2 << cluster, 2 << cluster);
		depth_renderer->flush(cmd, context,
		                      Renderer::NO_COLOR_BIT |
		                      Renderer::BACKFACE_BIT |
		                      Renderer::DEPTH_STENCIL_READ_ONLY_BIT |
		                      Renderer::STENCIL_WRITE_REFERENCE_BIT);
	}
}

void DeferredLights::render_lights(Vulkan::CommandBuffer &cmd, RenderContext &context,
                                   Renderer::RendererOptionFlags flags)
{
	deferred_renderer->set_mesh_renderer_options(flags);

	if (enable_clustered_stencil)
	{
		deferred_renderer->begin();
		deferred_renderer->push_renderables(context, clips);
		deferred_renderer->set_stencil_reference(1, 0, 0);
		deferred_renderer->flush(cmd, context, Renderer::STENCIL_COMPARE_REFERENCE_BIT);

		for (unsigned cluster = 0; cluster < NumClusters; cluster++)
		{
			deferred_renderer->begin();
			deferred_renderer->push_renderables(context, clusters[cluster]);
			deferred_renderer->set_stencil_reference((2 << cluster) | 1, 0, 2 << cluster);
			deferred_renderer->flush(cmd, context, Renderer::STENCIL_COMPARE_REFERENCE_BIT);
		}
	}
	else
	{
		visible.clear();
		scene->gather_visible_positional_lights(context.get_visibility_frustum(), visible,
		                                        max_spot_lights, max_point_lights);
		deferred_renderer->begin();
		deferred_renderer->push_renderables(context, visible);
		deferred_renderer->flush(cmd, context);
	}
}
}