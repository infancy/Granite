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

#include "renderer.hpp"
#include "device.hpp"
#include "render_context.hpp"
#include "sprite.hpp"
#include "lights/clusterer.hpp"
#include "lights/volumetric_fog.hpp"
#include "render_parameters.hpp"
#include <string.h>

using namespace Vulkan;
using namespace Util;
using namespace std;

namespace Granite
{

Renderer::Renderer(RendererType type_, const ShaderSuiteResolver *resolver_)
	: type(type_), resolver(resolver_)
{
	EVENT_MANAGER_REGISTER_LATCH(Renderer, on_device_created, on_device_destroyed, DeviceCreatedEvent);

	if (type == RendererType::GeneralDeferred || type == RendererType::GeneralForward)
		set_mesh_renderer_options(SHADOW_CASCADE_ENABLE_BIT | SHADOW_ENABLE_BIT | FOG_ENABLE_BIT | ENVIRONMENT_ENABLE_BIT);
	else
		set_mesh_renderer_options(0);
}

static const char *renderer_to_define(RendererType type)
{
	switch (type)
	{
	case RendererType::GeneralForward:
		return "RENDERER_FORWARD";

	case RendererType::GeneralDeferred:
		return "RENDERER_DEFERRED";

	case RendererType::DepthOnly:
		return "RENDERER_DEPTH";

	default:
		break;
	}

	return "";
}

void Renderer::set_mesh_renderer_options_internal(RendererOptionFlags flags)
{
	auto global_defines = build_defines_from_renderer_options(type, flags);

	if (device)
	{
		// Safe early-discard.
		if (device->get_device_features().demote_to_helper_invocation_features.shaderDemoteToHelperInvocation)
			global_defines.emplace_back("DEMOTE", 1);

		// Used for early-kill alpha testing if demote_to_helper isn't available.
		auto &subgroup = device->get_device_features().subgroup_properties;
		if ((subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 &&
		    !ImplementationQuirks::get().force_no_subgroups &&
		    subgroup.subgroupSize >= 4)
		{
			if ((subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0)
				global_defines.emplace_back("SUBGROUP_BASIC", 1);

			if ((subgroup.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0)
				global_defines.emplace_back("SUBGROUP_CLUSTERED", 1);
			if ((subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0)
				global_defines.emplace_back("SUBGROUP_QUAD", 1);
			if ((subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0)
				global_defines.emplace_back("SUBGROUP_BALLOT", 1);
			if ((subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0)
				global_defines.emplace_back("SUBGROUP_VOTE", 1);

			if (flags & POSITIONAL_LIGHT_ENABLE_BIT)
			{
				// Try to enable wave-optimizations.
				static const VkSubgroupFeatureFlags required_subgroup =
						VK_SUBGROUP_FEATURE_BALLOT_BIT |
						VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;

				if ((subgroup.supportedOperations & required_subgroup) == required_subgroup)
					global_defines.emplace_back("CLUSTERING_WAVE_UNIFORM", 1);
			}
		}
	}

	auto &meshes = suite[ecast(RenderableType::Mesh)];
	meshes.get_base_defines() = global_defines;
	meshes.bake_base_defines();
	auto &ground = suite[ecast(RenderableType::Ground)];
	ground.get_base_defines() = global_defines;
	ground.bake_base_defines();
	auto &ocean = suite[ecast(RenderableType::Ocean)];
	ocean.get_base_defines() = global_defines;
	ocean.bake_base_defines();
	auto &plane = suite[ecast(RenderableType::TexturePlane)];
	plane.get_base_defines() = global_defines;
	plane.bake_base_defines();
	auto &spot = suite[ecast(RenderableType::SpotLight)];
	spot.get_base_defines() = global_defines;
	spot.bake_base_defines();
	auto &point = suite[ecast(RenderableType::PointLight)];
	point.get_base_defines() = global_defines;
	point.bake_base_defines();

	// Skybox renderers only depend on VOLUMETRIC_FOG.
	ShaderSuite *suites[] = {
		&suite[ecast(RenderableType::Skybox)],
		&suite[ecast(RenderableType::SkyCylinder)],
	};

	for (auto *shader_suite : suites)
	{
		shader_suite->get_base_defines().clear();
		if (flags & VOLUMETRIC_FOG_ENABLE_BIT)
			shader_suite->get_base_defines().emplace_back("VOLUMETRIC_FOG", 1);
		shader_suite->get_base_defines().emplace_back(renderer_to_define(type), 1);
		shader_suite->bake_base_defines();
	}

	renderer_options = flags;
}

Renderer::RendererOptionFlags Renderer::get_mesh_renderer_options() const
{
	return renderer_options;
}

void Renderer::set_mesh_renderer_options(RendererOptionFlags flags)
{
	if (renderer_options != flags)
		set_mesh_renderer_options_internal(flags);
}

vector<pair<string, int>> Renderer::build_defines_from_renderer_options(RendererType type, RendererOptionFlags flags)
{
	vector<pair<string, int>> global_defines;
	if (flags & SHADOW_ENABLE_BIT)
		global_defines.emplace_back("SHADOWS", 1);
	if (flags & SHADOW_CASCADE_ENABLE_BIT)
		global_defines.emplace_back("SHADOW_CASCADES", 1);
	if (flags & FOG_ENABLE_BIT)
		global_defines.emplace_back("FOG", 1);
	if (flags & VOLUMETRIC_FOG_ENABLE_BIT)
		global_defines.emplace_back("VOLUMETRIC_FOG", 1);
	if (flags & ENVIRONMENT_ENABLE_BIT)
		global_defines.emplace_back("ENVIRONMENT", 1);
	if (flags & REFRACTION_ENABLE_BIT)
		global_defines.emplace_back("REFRACTION", 1);
	if (flags & POSITIONAL_LIGHT_ENABLE_BIT)
		global_defines.emplace_back("POSITIONAL_LIGHTS", 1);
	if (flags & POSITIONAL_LIGHT_SHADOW_ENABLE_BIT)
		global_defines.emplace_back("POSITIONAL_LIGHTS_SHADOW", 1);
	if (flags & POSITIONAL_LIGHT_CLUSTER_LIST_BIT)
		global_defines.emplace_back("CLUSTER_LIST", 1);

	if (flags & SHADOW_VSM_BIT)
		global_defines.emplace_back("DIRECTIONAL_SHADOW_VSM", 1);
	if (flags & POSITIONAL_LIGHT_SHADOW_VSM_BIT)
		global_defines.emplace_back("POSITIONAL_SHADOW_VSM", 1);
	if (flags & (POSITIONAL_LIGHT_SHADOW_VSM_BIT | SHADOW_VSM_BIT))
		global_defines.emplace_back("SHADOW_RESOLVE_VSM", 1);

	if (flags & SHADOW_PCF_KERNEL_WIDTH_5_BIT)
		global_defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 5);
	else if (flags & SHADOW_PCF_KERNEL_WIDTH_3_BIT)
		global_defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 3);

	if (flags & ALPHA_TEST_DISABLE_BIT)
		global_defines.emplace_back("ALPHA_TEST_DISABLE", 1);

	global_defines.emplace_back(renderer_to_define(type), 1);

	return global_defines;
}

Renderer::RendererOptionFlags Renderer::get_mesh_renderer_options_from_lighting(const LightingParameters &lighting)
{
	uint32_t flags = 0;
	if (lighting.environment_irradiance && lighting.environment_radiance)
		flags |= ENVIRONMENT_ENABLE_BIT;
	if (lighting.shadow_far)
	{
		flags |= SHADOW_ENABLE_BIT;
		if (!Vulkan::format_has_depth_or_stencil_aspect(lighting.shadow_far->get_format()))
			flags |= SHADOW_VSM_BIT;
	}
	if (lighting.shadow_near && lighting.shadow_far)
		flags |= SHADOW_CASCADE_ENABLE_BIT;

	if (lighting.volumetric_fog)
		flags |= VOLUMETRIC_FOG_ENABLE_BIT;
	else if (lighting.fog.falloff > 0.0f)
		flags |= FOG_ENABLE_BIT;

	if (lighting.cluster && lighting.cluster->get_cluster_image())
	{
		flags |= POSITIONAL_LIGHT_ENABLE_BIT;
		if (lighting.cluster->get_spot_light_shadows() && lighting.cluster->get_point_light_shadows())
		{
			flags |= POSITIONAL_LIGHT_SHADOW_ENABLE_BIT;
			if (!format_has_depth_or_stencil_aspect(lighting.cluster->get_spot_light_shadows()->get_format()))
				flags |= POSITIONAL_LIGHT_SHADOW_VSM_BIT;
		}
		if (lighting.cluster->get_cluster_list_buffer())
			flags |= POSITIONAL_LIGHT_CLUSTER_LIST_BIT;
	}

	return flags;
}

void Renderer::set_mesh_renderer_options_from_lighting(const LightingParameters &lighting)
{
	auto flags = get_mesh_renderer_options_from_lighting(lighting);
	set_mesh_renderer_options(flags);
}

void Renderer::setup_shader_suite(Device &device_, RendererType renderer_type)
{
	ShaderSuiteResolver default_resolver;
	auto *res = resolver ? resolver : &default_resolver;
	for (int i = 0; i < ecast(RenderableType::Count); i++)
		res->init_shader_suite(device_, suite[i], renderer_type, static_cast<RenderableType>(i));
}

void Renderer::on_device_created(const DeviceCreatedEvent &created)
{
	device = &created.get_device();
	setup_shader_suite(*device, type);
	set_mesh_renderer_options_internal(renderer_options);
	for (auto &s : suite)
		s.bake_base_defines();
}

void Renderer::on_device_destroyed(const DeviceCreatedEvent &)
{
}

void Renderer::begin()
{
	queue.reset();
	queue.set_shader_suites(suite);
}

static void set_cluster_parameters(Vulkan::CommandBuffer &cmd, const LightClusterer &cluster)
{
	auto &params = *cmd.allocate_typed_constant_data<ClustererParameters>(0, 2, 1);
	memset(&params, 0, sizeof(params));

	cmd.set_texture(1, 6, *cluster.get_cluster_image(), StockSampler::NearestClamp);

	params.transform = cluster.get_cluster_transform();
	memcpy(params.spots, cluster.get_active_spot_lights(),
	       cluster.get_active_spot_light_count() * sizeof(PositionalFragmentInfo));
	memcpy(params.points, cluster.get_active_point_lights(),
	       cluster.get_active_point_light_count() * sizeof(PositionalFragmentInfo));

	if (cluster.get_spot_light_shadows() && cluster.get_point_light_shadows())
	{
		auto spot_sampler = format_has_depth_or_stencil_aspect(cluster.get_spot_light_shadows()->get_format()) ?
		                    StockSampler::LinearShadow : StockSampler::LinearClamp;
		auto point_sampler = format_has_depth_or_stencil_aspect(cluster.get_point_light_shadows()->get_format()) ?
		                     StockSampler::LinearShadow : StockSampler::LinearClamp;

		cmd.set_texture(1, 7, *cluster.get_spot_light_shadows(), spot_sampler);
		cmd.set_texture(1, 8, *cluster.get_point_light_shadows(), point_sampler);

		memcpy(params.spot_shadow_transforms, cluster.get_active_spot_light_shadow_matrices(),
		       cluster.get_active_spot_light_count() * sizeof(mat4));

		memcpy(params.point_shadow, cluster.get_active_point_light_shadow_transform(),
		       cluster.get_active_point_light_count() * sizeof(PointTransform));
	}

	if (cluster.get_cluster_list_buffer())
		cmd.set_storage_buffer(1, 9, *cluster.get_cluster_list_buffer());
}

void Renderer::bind_lighting_parameters(Vulkan::CommandBuffer &cmd, const RenderContext &context)
{
	auto *lighting = context.get_lighting_parameters();
	assert(lighting);

	auto *combined = cmd.allocate_typed_constant_data<CombinedRenderParameters>(0, 1, 1);
	memset(combined, 0, sizeof(*combined));

	combined->environment.intensity = lighting->environment.intensity;
	if (lighting->environment_radiance)
		combined->environment.mipscale = float(lighting->environment_radiance->get_create_info().levels - 1);

	if (lighting->volumetric_fog)
	{
		cmd.set_texture(1, 5, lighting->volumetric_fog->get_view(), StockSampler::LinearClamp);
		combined->volumetric_fog.slice_z_log2_scale = lighting->volumetric_fog->get_slice_z_log2_scale();
	}
	else
		combined->fog = lighting->fog;

	combined->shadow = lighting->shadow;
	combined->directional = lighting->directional;
	combined->refraction = lighting->refraction;

	combined->resolution.resolution = vec2(cmd.get_viewport().width, cmd.get_viewport().height);
	combined->resolution.inv_resolution = vec2(1.0f / cmd.get_viewport().width, 1.0f / cmd.get_viewport().height);

	cmd.set_texture(1, 2,
	                cmd.get_device().get_texture_manager().request_texture("builtin://textures/ibl_brdf_lut.gtx")->get_image()->get_view(),
	                Vulkan::StockSampler::LinearClamp);

	if (lighting->environment_radiance != nullptr)
		cmd.set_texture(1, 0, *lighting->environment_radiance, Vulkan::StockSampler::TrilinearClamp);
	if (lighting->environment_irradiance != nullptr)
		cmd.set_texture(1, 1, *lighting->environment_irradiance, Vulkan::StockSampler::LinearClamp);

	if (lighting->shadow_far != nullptr)
	{
		auto sampler = format_has_depth_or_stencil_aspect(lighting->shadow_far->get_format()) ? StockSampler::LinearShadow
		                                                                           : StockSampler::LinearClamp;
		cmd.set_texture(1, 3, *lighting->shadow_far, sampler);
	}

	if (lighting->shadow_near != nullptr)
	{
		auto sampler = format_has_depth_or_stencil_aspect(lighting->shadow_near->get_format()) ? StockSampler::LinearShadow
		                                                                            : StockSampler::LinearClamp;
		cmd.set_texture(1, 4, *lighting->shadow_near, sampler);
	}

	if (lighting->cluster && lighting->cluster->get_cluster_image())
		set_cluster_parameters(cmd, *lighting->cluster);

	if (lighting->ambient_occlusion)
		cmd.set_texture(1, 10, *lighting->ambient_occlusion, StockSampler::LinearClamp);
}

void Renderer::set_stencil_reference(uint8_t compare_mask, uint8_t write_mask, uint8_t ref)
{
	stencil_compare_mask = compare_mask;
	stencil_write_mask = write_mask;
	stencil_reference = ref;
}

void Renderer::bind_global_parameters(Vulkan::CommandBuffer &cmd, const RenderContext &context)
{
	auto *global = cmd.allocate_typed_constant_data<RenderParameters>(0, 0, 1);
	*global = context.get_render_parameters();
}

void Renderer::set_render_context_parameter_binder(RenderContextParameterBinder *binder)
{
	render_context_parameter_binder = binder;
}

void Renderer::flush(Vulkan::CommandBuffer &cmd, RenderContext &context, RendererFlushFlags options)
{
	if (render_context_parameter_binder)
	{
		render_context_parameter_binder->bind_render_context_parameters(cmd, context);
	}
	else
	{
		bind_global_parameters(cmd, context);
		if (type == RendererType::GeneralForward)
			bind_lighting_parameters(cmd, context);
	}

	if ((options & SKIP_SORTING_BIT) == 0)
		queue.sort();

	cmd.set_opaque_state();

	if (options & FRONT_FACE_CLOCKWISE_BIT)
		cmd.set_front_face(VK_FRONT_FACE_CLOCKWISE);

	if (options & NO_COLOR_BIT)
		cmd.set_color_write_mask(0);

	if (options & DEPTH_STENCIL_READ_ONLY_BIT)
		cmd.set_depth_test(true, false);

	if (options & DEPTH_BIAS_BIT)
	{
		cmd.set_depth_bias(true);
		cmd.set_depth_bias(+4.0f, +3.0f);
	}

	if (options & BACKFACE_BIT)
	{
		cmd.set_cull_mode(VK_CULL_MODE_FRONT_BIT);
		cmd.set_depth_compare(VK_COMPARE_OP_GREATER);
	}

	if (options & DEPTH_TEST_EQUAL_BIT)
		cmd.set_depth_compare(VK_COMPARE_OP_EQUAL);
	else if (options & DEPTH_TEST_INVERT_BIT)
		cmd.set_depth_compare(VK_COMPARE_OP_GREATER);

	if (options & STENCIL_WRITE_REFERENCE_BIT)
	{
		cmd.set_stencil_test(true);
		cmd.set_stencil_ops(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
		cmd.set_stencil_reference(stencil_compare_mask, stencil_write_mask, stencil_reference);
	}

	CommandBufferSavedState state;
	cmd.save_state(COMMAND_BUFFER_SAVED_SCISSOR_BIT | COMMAND_BUFFER_SAVED_VIEWPORT_BIT | COMMAND_BUFFER_SAVED_RENDER_STATE_BIT, state);
	// No need to spend write bandwidth on writing 0 to light buffer, render opaque emissive on top.
	queue.dispatch(Queue::Opaque, cmd, &state);
	queue.dispatch(Queue::OpaqueEmissive, cmd, &state);

	if (type == RendererType::GeneralDeferred)
	{
		// General deferred renderers can render light volumes.
		cmd.restore_state(state);
		cmd.set_input_attachments(3, 0);
		cmd.set_depth_test(true, false);
		cmd.set_blend_enable(true);
		cmd.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
		cmd.set_blend_op(VK_BLEND_OP_ADD);

		cmd.set_stencil_test(true);
		if (options & STENCIL_COMPARE_REFERENCE_BIT)
			cmd.set_stencil_reference(stencil_compare_mask, 0, stencil_reference);
		else
			cmd.set_stencil_reference(0xff, 0, 0);

		cmd.set_stencil_front_ops(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
		cmd.set_stencil_back_ops(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
		cmd.save_state(COMMAND_BUFFER_SAVED_SCISSOR_BIT | COMMAND_BUFFER_SAVED_VIEWPORT_BIT | COMMAND_BUFFER_SAVED_RENDER_STATE_BIT, state);
		queue.dispatch(Queue::Light, cmd, &state);
	}
	else if (type == RendererType::GeneralForward)
	{
		// Forward renderers can also render transparent objects.
		cmd.restore_state(state);
		cmd.set_blend_enable(true);
		cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		cmd.set_blend_op(VK_BLEND_OP_ADD);
		cmd.set_depth_test(true, false);
		cmd.save_state(COMMAND_BUFFER_SAVED_SCISSOR_BIT | COMMAND_BUFFER_SAVED_VIEWPORT_BIT | COMMAND_BUFFER_SAVED_RENDER_STATE_BIT, state);
		queue.dispatch(Queue::Transparent, cmd, &state);
	}
}

DebugMeshInstanceInfo &Renderer::render_debug(RenderContext &context, unsigned count)
{
	DebugMeshInfo debug;

	auto *instance_data = queue.allocate_one<DebugMeshInstanceInfo>();
	instance_data->count = count;
	instance_data->colors = queue.allocate_many<vec4>(count);
	instance_data->positions = queue.allocate_many<vec3>(count);

	Hasher hasher;
	hasher.string("debug");
	auto instance_key = hasher.get();
	auto sorting_key = RenderInfo::get_sort_key(context, Queue::Opaque, hasher.get(), hasher.get(), vec3(0.0f));
	debug.MVP = context.get_render_parameters().view_projection;

	auto *debug_info = queue.push<DebugMeshInfo>(Queue::Opaque, instance_key, sorting_key,
	                                             RenderFunctions::debug_mesh_render,
	                                             instance_data);

	if (debug_info)
	{
		debug.program = suite[ecast(RenderableType::DebugMesh)].get_program(DrawPipeline::Opaque,
		                                                                    MESH_ATTRIBUTE_POSITION_BIT |
		                                                                    MESH_ATTRIBUTE_VERTEX_COLOR_BIT, 0);
		*debug_info = debug;
	}

	return *instance_data;
}

template <typename T>
inline void dump_debug_coords(vec3 *pos, const T &t)
{
	*pos++ = t.get_coord(0.0f, 0.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 0.0f, 0.0f);

	*pos++ = t.get_coord(0.0f, 1.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 1.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 1.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 1.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 1.0f, 0.0f);

	*pos++ = t.get_coord(0.0f, 0.0f, 0.0f);
	*pos++ = t.get_coord(0.0f, 1.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 0.0f);
	*pos++ = t.get_coord(1.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(1.0f, 1.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 0.0f, 1.0f);
	*pos++ = t.get_coord(0.0f, 1.0f, 1.0f);
}

void Renderer::render_debug_frustum(RenderContext &context, const Frustum &frustum, const vec4 &color)
{
	auto &debug = render_debug(context, 12 * 2);
	for (unsigned i = 0; i < debug.count; i++)
		debug.colors[i] = color;
	dump_debug_coords(debug.positions, frustum);
}

void Renderer::render_debug_aabb(RenderContext &context, const AABB &aabb, const vec4 &color)
{
	auto &debug = render_debug(context, 12 * 2);
	for (unsigned i = 0; i < debug.count; i++)
		debug.colors[i] = color;
	dump_debug_coords(debug.positions, aabb);
}

void Renderer::push_renderables(RenderContext &context, const VisibilityList &visible)
{
	for (auto &vis : visible)
		vis.renderable->get_render_info(context, vis.transform, queue);
}

void Renderer::push_depth_renderables(RenderContext &context, const VisibilityList &visible)
{
	for (auto &vis : visible)
		vis.renderable->get_depth_render_info(context, vis.transform, queue);
}

void DeferredLightRenderer::render_light(Vulkan::CommandBuffer &cmd, RenderContext &context,
                                         Renderer::RendererOptionFlags flags)
{
	cmd.set_quad_state();
	cmd.set_input_attachments(3, 0);
	cmd.set_blend_enable(true);
	cmd.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
	cmd.set_blend_op(VK_BLEND_OP_ADD);
	CommandBufferUtil::set_fullscreen_quad_vertex_state(cmd);

	auto &device = cmd.get_device();
	auto *program = device.get_shader_manager().register_graphics("builtin://shaders/lights/directional.vert",
	                                                              "builtin://shaders/lights/directional.frag");

	auto &light = *context.get_lighting_parameters();

	vector<pair<string, int>> defines;
	if (light.shadow_far && light.shadow_near)
		defines.emplace_back("SHADOW_CASCADES", 1);
	if (light.environment_radiance && light.environment_irradiance)
		defines.emplace_back("ENVIRONMENT", 1);
	if (light.shadow_far)
	{
		defines.emplace_back("SHADOWS", 1);
		if (!format_has_depth_or_stencil_aspect(light.shadow_far->get_format()))
			defines.emplace_back("DIRECTIONAL_SHADOW_VSM", 1);
		else
		{
			if (flags & Renderer::SHADOW_PCF_KERNEL_WIDTH_5_BIT)
				defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 5);
			else if (flags & Renderer::SHADOW_PCF_KERNEL_WIDTH_3_BIT)
				defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 3);
		}
	}
	if (light.ambient_occlusion)
		defines.emplace_back("AMBIENT_OCCLUSION", 1);

	unsigned variant = program->register_variant(defines);
	cmd.set_program(program->get_program(variant));
	cmd.set_depth_test(true, false);
	cmd.set_depth_compare(VK_COMPARE_OP_GREATER);

	if (light.environment_radiance)
		cmd.set_texture(1, 0, *light.environment_radiance, Vulkan::StockSampler::LinearClamp);
	if (light.environment_irradiance)
		cmd.set_texture(1, 1, *light.environment_irradiance, Vulkan::StockSampler::LinearClamp);

	cmd.set_texture(1, 2,
	                cmd.get_device().get_texture_manager().request_texture("builtin://textures/ibl_brdf_lut.gtx")->get_image()->get_view(),
	                Vulkan::StockSampler::LinearClamp);

	if (light.shadow_far)
	{
		auto sampler = format_has_depth_or_stencil_aspect(light.shadow_far->get_format()) ? StockSampler::LinearShadow
		                                                                       : StockSampler::LinearClamp;
		cmd.set_texture(1, 3, *light.shadow_far, sampler);
	}

	if (light.shadow_near)
	{
		auto sampler = format_has_depth_or_stencil_aspect(light.shadow_near->get_format()) ? StockSampler::LinearShadow
		                                                                        : StockSampler::LinearClamp;
		cmd.set_texture(1, 4, *light.shadow_near, sampler);
	}

	if (light.ambient_occlusion)
		cmd.set_texture(1, 10, *light.ambient_occlusion, Vulkan::StockSampler::LinearClamp);

	struct DirectionalLightPush
	{
		alignas(16) vec4 inv_view_proj_col2;
		alignas(16) vec4 shadow_col2;
		alignas(16) vec4 shadow_near_col2;
		alignas(16) vec4 direction_inv_cutoff;
		alignas(16) vec4 color_env_intensity;
		alignas(16) vec4 camera_pos_mipscale;
		alignas(16) vec3 camera_front;
		alignas(8) vec2 inv_resolution;
	} push;

	mat4 total_shadow_transform = light.shadow.far_transform * context.get_render_parameters().inv_view_projection;
	mat4 total_shadow_transform_near = light.shadow.near_transform * context.get_render_parameters().inv_view_projection;

	struct DirectionalLightUBO
	{
		mat4 inv_view_projection;
		mat4 shadow_transform;
		mat4 shadow_transform_near;
	};
	auto *ubo = static_cast<DirectionalLightUBO *>(cmd.allocate_constant_data(0, 0, sizeof(DirectionalLightUBO)));
	ubo->inv_view_projection = context.get_render_parameters().inv_view_projection;
	ubo->shadow_transform = total_shadow_transform;
	ubo->shadow_transform_near = total_shadow_transform_near;

	push.inv_view_proj_col2 = context.get_render_parameters().inv_view_projection[2];
	push.shadow_col2 = total_shadow_transform[2];
	push.shadow_near_col2 = total_shadow_transform_near[2];
	push.color_env_intensity = vec4(light.directional.color, light.environment.intensity);
	push.direction_inv_cutoff = vec4(light.directional.direction, light.shadow.inv_cutoff_distance);

	float mipscale = 0.0f;
	if (light.environment_radiance)
		mipscale = float(light.environment_radiance->get_create_info().levels - 1);

	push.camera_pos_mipscale = vec4(context.get_render_parameters().camera_position, mipscale);
	push.camera_front = context.get_render_parameters().camera_front;
	push.inv_resolution.x = 1.0f / cmd.get_viewport().width;
	push.inv_resolution.y = 1.0f / cmd.get_viewport().height;
	cmd.push_constants(&push, 0, sizeof(push));

	CommandBufferUtil::draw_fullscreen_quad(cmd);

	// Clustered lighting.
	if (light.cluster && light.cluster->get_cluster_image())
	{
		struct ClusterPush
		{
			vec4 inv_view_proj_col2;
			vec3 camera_pos;
		};

		ClusterPush cluster_push = {
			context.get_render_parameters().inv_view_projection[2],
			context.get_render_parameters().camera_position,
		};

		vector<pair<string, int>> cluster_defines;
		if (light.cluster->get_spot_light_shadows())
		{
			cluster_defines.emplace_back("POSITIONAL_LIGHTS_SHADOW", 1);
			if (!format_has_depth_or_stencil_aspect(light.cluster->get_spot_light_shadows()->get_format()))
				cluster_defines.emplace_back("POSITIONAL_SHADOW_VSM", 1);
			else
			{
				if (flags & Renderer::SHADOW_PCF_KERNEL_WIDTH_5_BIT)
					cluster_defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 5);
				else if (flags & Renderer::SHADOW_PCF_KERNEL_WIDTH_3_BIT)
					cluster_defines.emplace_back("SHADOW_MAP_PCF_KERNEL_WIDTH", 3);
			}
		}

		if (light.cluster->get_cluster_list_buffer())
			cluster_defines.emplace_back("CLUSTER_LIST", 1);

		// Try to enable wave-optimizations.
		static const VkSubgroupFeatureFlags required_subgroup = VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
		auto &subgroup = device.get_device_features().subgroup_properties;
		if ((subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 &&
		    !ImplementationQuirks::get().force_no_subgroups &&
		    (subgroup.supportedOperations & required_subgroup) == required_subgroup)
		{
			cluster_defines.emplace_back("CLUSTERING_WAVE_UNIFORM", 1);
		}

		cmd.set_program("builtin://shaders/lights/clustering.vert",
		                "builtin://shaders/lights/clustering.frag",
		                cluster_defines);

		cmd.push_constants(&cluster_push, 0, sizeof(cluster_push));
		set_cluster_parameters(cmd, *light.cluster);
		CommandBufferUtil::draw_fullscreen_quad(cmd);
	}

	// Skip fog for non-reflection passes.
	if (light.volumetric_fog != nullptr)
	{
		struct Fog
		{
			vec4 inv_z;
			float slice_z_log2_scale;
		} fog;

		fog.inv_z = vec4(context.get_render_parameters().inv_projection[2].zw(),
		                 context.get_render_parameters().inv_projection[3].zw());
		fog.slice_z_log2_scale = light.volumetric_fog->get_slice_z_log2_scale();
		cmd.push_constants(&fog, 0, sizeof(fog));

		cmd.set_texture(2, 0, light.volumetric_fog->get_view(), StockSampler::LinearClamp);
		cmd.set_program("builtin://shaders/lights/volumetric_fog.vert", "builtin://shaders/lights/volumetric_fog.frag");
		cmd.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA);
		// Always render volumetric fog.
		cmd.set_depth_test(false, false);
		cmd.set_stencil_test(false);
		CommandBufferUtil::draw_fullscreen_quad(cmd);
	}
	else if (light.fog.falloff > 0.0f)
	{
		struct Fog
		{
			mat4 inv_view_proj;
			vec4 camera_pos;
			vec4 color_falloff;
		} fog;

		fog.inv_view_proj = context.get_render_parameters().inv_view_projection;
		fog.camera_pos = vec4(context.get_render_parameters().camera_position, 0.0f);
		fog.color_falloff = vec4(light.fog.color, light.fog.falloff);
		cmd.push_constants(&fog, 0, sizeof(fog));

		cmd.set_blend_factors(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA);
		cmd.set_program("builtin://shaders/lights/fog.vert", "builtin://shaders/lights/fog.frag");
		CommandBufferUtil::draw_fullscreen_quad(cmd);
	}
}

void ShaderSuiteResolver::init_shader_suite(Device &device, ShaderSuite &suite,
                                            RendererType renderer,
                                            RenderableType drawable) const
{
	if (renderer == RendererType::GeneralDeferred ||
	    renderer == RendererType::GeneralForward)
	{
		switch (drawable)
		{
		case RenderableType::Mesh:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/static_mesh.vert", "builtin://shaders/static_mesh.frag");
			break;

		case RenderableType::DebugMesh:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/debug_mesh.vert", "builtin://shaders/debug_mesh.frag");
			break;

		case RenderableType::Skybox:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/skybox.vert", "builtin://shaders/skybox.frag");
			break;

		case RenderableType::SkyCylinder:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/skycylinder.vert", "builtin://shaders/skycylinder.frag");
			break;

		case RenderableType::Ground:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/ground.vert", "builtin://shaders/ground.frag");
			break;

		case RenderableType::Ocean:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/ocean/ocean.vert", "builtin://shaders/ocean/ocean.frag");
			break;

		case RenderableType::TexturePlane:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/texture_plane.vert", "builtin://shaders/texture_plane.frag");
			break;

		default:
			break;
		}
	}
	else if (renderer == RendererType::DepthOnly)
	{
		switch (drawable)
		{
		case RenderableType::Mesh:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/static_mesh.vert", "builtin://shaders/static_mesh_depth.frag");
			break;

		case RenderableType::Ground:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/ground.vert", "builtin://shaders/dummy_depth.frag");
			break;

		case RenderableType::TexturePlane:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/texture_plane.vert", "builtin://shaders/dummy_depth.frag");
			break;

		case RenderableType::SpotLight:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/lights/spot.vert", "builtin://shaders/dummy.frag");
			break;

		case RenderableType::PointLight:
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/lights/point.vert", "builtin://shaders/dummy.frag");
			break;

		default:
			break;
		}
	}
	else if (renderer == RendererType::Flat)
	{
		if (drawable == RenderableType::Sprite)
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/sprite.vert", "builtin://shaders/sprite.frag");
		else if (drawable == RenderableType::LineUI)
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/line_ui.vert", "builtin://shaders/debug_mesh.frag");
	}

	if (renderer == RendererType::GeneralDeferred)
	{
		if (drawable == RenderableType::SpotLight)
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/lights/spot.vert", "builtin://shaders/lights/spot.frag");
		else if (drawable == RenderableType::PointLight)
			suite.init_graphics(&device.get_shader_manager(), "builtin://shaders/lights/point.vert", "builtin://shaders/lights/point.frag");
	}
}

}
