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

#include "query_pool.hpp"
#include "device.hpp"
#include <utility>

using namespace std;

namespace Vulkan
{

QueryPool::QueryPool(Device *device_)
	: device(device_)
	, table(device_->get_device_table())
{
	query_period = 1e-9 * device->get_gpu_properties().limits.timestampPeriod;
	supports_timestamp = device->get_gpu_properties().limits.timestampComputeAndGraphics;

	// Ignore timestampValidBits and friends for now.
	if (supports_timestamp)
		add_pool();
}

QueryPool::~QueryPool()
{
	for (auto &pool : pools)
		table.vkDestroyQueryPool(device->get_device(), pool.pool, nullptr);
}

void QueryPool::begin()
{
	for (unsigned i = 0; i <= pool_index; i++)
	{
		if (i >= pools.size())
			continue;

		auto &pool = pools[i];
		if (pool.index == 0)
			continue;

		table.vkGetQueryPoolResults(device->get_device(), pool.pool,
		                            0, pool.index,
		                            pool.index * sizeof(uint64_t),
		                            pool.query_results.data(),
		                            sizeof(uint64_t),
		                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

		for (unsigned j = 0; j < pool.index; j++)
			pool.cookies[j]->signal_timestamp(double(pool.query_results[j]) * query_period);

		if (device->get_device_features().host_query_reset_features.hostQueryReset)
			table.vkResetQueryPoolEXT(device->get_device(), pool.pool, 0, pool.index);
	}

	pool_index = 0;
	for (auto &pool : pools)
		pool.index = 0;
}

void QueryPool::add_pool()
{
	VkQueryPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	pool_info.queryCount = 64;

	Pool pool;
	table.vkCreateQueryPool(device->get_device(), &pool_info, nullptr, &pool.pool);
	pool.size = pool_info.queryCount;
	pool.index = 0;
	pool.query_results.resize(pool.size);
	pool.cookies.resize(pool.size);

	if (device->get_device_features().host_query_reset_features.hostQueryReset)
		table.vkResetQueryPoolEXT(device->get_device(), pool.pool, 0, pool.size);

	pools.push_back(move(pool));
}

QueryPoolHandle QueryPool::write_timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage)
{
	if (!supports_timestamp)
	{
		LOGI("Timestamps are not supported on this implementation.\n");
		return {};
	}

	if (pools[pool_index].index >= pools[pool_index].size)
		pool_index++;

	if (pool_index >= pools.size())
		add_pool();

	auto &pool = pools[pool_index];

	auto cookie = QueryPoolHandle(device->handle_pool.query.allocate(device));
	pool.cookies[pool.index] = cookie;

	if (!device->get_device_features().host_query_reset_features.hostQueryReset)
		table.vkCmdResetQueryPool(cmd, pool.pool, pool.index, 1);
	table.vkCmdWriteTimestamp(cmd, stage, pool.pool, pool.index);

	pool.index++;
	return cookie;
}

void QueryPoolResultDeleter::operator()(QueryPoolResult *query)
{
	query->device->handle_pool.query.free(query);
}

void TimestampInterval::mark_end_of_frame_context()
{
	if (total_time > 0.0)
		total_frame_iterations++;
}

uint64_t TimestampInterval::get_total_accumulations() const
{
	return total_accumulations;
}

uint64_t TimestampInterval::get_total_frame_iterations() const
{
	return total_frame_iterations;
}

double TimestampInterval::get_total_time() const
{
	return total_time;
}

void TimestampInterval::accumulate_time(double t)
{
	total_time += t;
	total_accumulations++;
}

double TimestampInterval::get_time_per_iteration() const
{
	if (total_frame_iterations)
		return total_time / double(total_frame_iterations);
	else
		return 0.0;
}

const string &TimestampInterval::get_tag() const
{
	return tag;
}

TimestampInterval::TimestampInterval(string tag_)
	: tag(move(tag_))
{
}

TimestampInterval *TimestampIntervalManager::get_timestamp_tag(const char *tag)
{
	Util::Hasher h;
	h.string(tag);
	return timestamps.emplace_yield(h.get(), tag);
}

void TimestampIntervalManager::mark_end_of_frame_context()
{
	for (auto &timestamp : timestamps)
		timestamp.mark_end_of_frame_context();
}

void TimestampIntervalManager::log_simple()
{
	for (auto &timestamp : timestamps)
	{
		LOGI("Timestamp tag report: %s\n", timestamp.get_tag().c_str());
		if (timestamp.get_total_frame_iterations())
		{
			LOGI("  %.3f ms / frame context\n", 1000.0 * timestamp.get_time_per_iteration());
			LOGI("  %.3f iterations / frame context\n",
			     double(timestamp.get_total_accumulations()) / double(timestamp.get_total_frame_iterations()));
		}
	}
}
}