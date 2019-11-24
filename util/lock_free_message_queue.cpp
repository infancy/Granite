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

#include "lock_free_message_queue.hpp"
#include "util.hpp"
#include "aligned_alloc.hpp"
#include <stdlib.h>
#include <algorithm>

namespace Util
{
void MessageQueuePayloadDeleter::operator()(void *ptr)
{
	memalign_free(ptr);
}

LockFreeMessageQueue::LockFreeMessageQueue()
{
	for (unsigned i = 0; i < 8; i++)
		payload_capacity[i] = 256u << i;
	for (unsigned i = 0; i < 8; i++)
		write_ring[i].reset((16u * 1024u) >> i);
	read_ring.reset(32 * 1024);

	// Pre-fill the rings.
	for (unsigned i = 0; i < 8; i++)
	{
		unsigned count = 512u >> i;
		for (unsigned j = 0; j < count; j++)
		{
			MessageQueuePayload payload;
			payload.set_payload_data(memalign_calloc(64, payload_capacity[i]), payload_capacity[i]);
			recycle_payload(std::move(payload));
		}
	}
}

size_t LockFreeMessageQueue::available_read_messages() const noexcept
{
	return read_ring.read_avail();
}

MessageQueuePayload LockFreeMessageQueue::read_message() noexcept
{
	MessageQueuePayload payload;
	read_ring.read_and_move(payload);
	return payload;
}

bool LockFreeMessageQueue::push_written_payload(MessageQueuePayload payload) noexcept
{
	return read_ring.write_and_move(std::move(payload));
}

void LockFreeMessageQueue::recycle_payload(MessageQueuePayload payload) noexcept
{
	for (unsigned i = 0; i < 8; i++)
	{
		if (payload.get_capacity() == payload_capacity[i])
		{
			if (!write_ring[i].write_and_move(std::move(payload)))
				LOGE("Failed to recycle write payload. Increase sizes!\n");
			return;
		}
	}
}

MessageQueuePayload LockFreeMessageQueue::allocate_write_payload(size_t size) noexcept
{
	MessageQueuePayload payload;
	for (unsigned i = 0; i < 8; i++)
	{
		if (size <= payload_capacity[i])
		{
			if (!write_ring[i].read_and_move(payload))
			{
				LOGE("Failed to allocate write payload. Have to allocate a new one on the fly. Increase sizes!\n");
				payload.set_payload_data(memalign_calloc(64, payload_capacity[i]), payload_capacity[i]);
			}
			return payload;
		}
	}

	LOGE("Failed to allocate any payload. Making a temporary one. Increase max payload size!\n");
	payload.set_payload_data(memalign_calloc(64, size), size);
	return payload;
}
}
