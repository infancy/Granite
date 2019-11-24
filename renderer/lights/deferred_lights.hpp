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

#pragma once

#include "scene.hpp"
#include "render_context.hpp"
#include "renderer.hpp"
#include <vector>

namespace Granite
{
class DeferredLights : public PerFrameRefreshable
{
public:
	void set_scene(Scene *scene);
	void set_renderers(Renderer *depth_renderer, Renderer *deferred_renderer);
	void set_enable_clustered_stencil_culling(bool state)
	{
		enable_clustered_stencil = state;
	}

	void render_prepass_lights(Vulkan::CommandBuffer &cmd, RenderContext &context);
	void render_lights(Vulkan::CommandBuffer &cmd, RenderContext &context, Renderer::RendererOptionFlags flags);

	void set_max_spot_lights(unsigned count)
	{
		max_spot_lights = count;
	}

	void set_max_point_lights(unsigned count)
	{
		max_point_lights = count;
	}

private:
	VisibilityList visible;
	Scene *scene = nullptr;
	Renderer *depth_renderer = nullptr;
	Renderer *deferred_renderer = nullptr;

	enum { NumClusters = 7 };

	VisibilityList clips;
	VisibilityList clusters[NumClusters];
	bool enable_clustered_stencil = false;

	unsigned max_spot_lights = std::numeric_limits<unsigned>::max();
	unsigned max_point_lights = std::numeric_limits<unsigned>::max();

	void refresh(RenderContext &context) override;
};
}