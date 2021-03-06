#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/SysCalls/SysCalls.h"

#include "Emu/Cell/PPUThread.h"
#include "Emu/Event.h"
#include "sleep_queue.h"
#include "sys_time.h"
#include "sys_process.h"
#include "sys_event.h"

SysCallBase sys_event("sys_event");

u32 event_queue_create(u32 protocol, s32 type, u64 name_u64, u64 event_queue_key, s32 size)
{
	std::shared_ptr<event_queue_t> queue(new event_queue_t(protocol, type, name_u64, event_queue_key, size));

	Emu.GetEventManager().RegisterKey(queue, event_queue_key);

	return Emu.GetIdManager().GetNewID(queue, TYPE_EVENT_QUEUE);
}

s32 sys_event_queue_create(vm::ptr<u32> equeue_id, vm::ptr<sys_event_queue_attr> attr, u64 event_queue_key, s32 size)
{
	sys_event.Warning("sys_event_queue_create(equeue_id=*0x%x, attr=*0x%x, event_queue_key=0x%llx, size=%d)", equeue_id, attr, event_queue_key, size);

	if (size <= 0 || size > 127)
	{
		return CELL_EINVAL;
	}

	const u32 protocol = attr->protocol;

	switch (protocol)
	{
	case SYS_SYNC_FIFO: break;
	case SYS_SYNC_PRIORITY: break;
	default: sys_event.Error("sys_event_queue_create(): unknown protocol (0x%x)", protocol); return CELL_EINVAL;
	}

	const u32 type = attr->type;

	switch (type)
	{
	case SYS_PPU_QUEUE: break;
	case SYS_SPU_QUEUE: break;
	default: sys_event.Error("sys_event_queue_create(): unknown type (0x%x)", type); return CELL_EINVAL;
	}

	std::shared_ptr<event_queue_t> queue(new event_queue_t(protocol, type, attr->name_u64, event_queue_key, size));

	if (!Emu.GetEventManager().RegisterKey(queue, event_queue_key))
	{
		return CELL_EEXIST;
	}

	*equeue_id = Emu.GetIdManager().GetNewID(queue, TYPE_EVENT_QUEUE);
	
	return CELL_OK;
}

s32 sys_event_queue_destroy(u32 equeue_id, s32 mode)
{
	sys_event.Warning("sys_event_queue_destroy(equeue_id=%d, mode=%d)", equeue_id, mode);

	LV2_LOCK;

	std::shared_ptr<event_queue_t> queue;

	if (!Emu.GetIdManager().GetIDData(equeue_id, queue))
	{
		return CELL_ESRCH;
	}

	if (mode && mode != SYS_EVENT_QUEUE_DESTROY_FORCE)
	{
		return CELL_EINVAL;
	}

	if (!mode && queue->waiters)
	{
		return CELL_EBUSY;
	}

	if (queue->cancelled.exchange(true))
	{
		throw __FUNCTION__;
	}

	if (queue->waiters)
	{
		queue->cv.notify_all();
	}

	Emu.GetEventManager().UnregisterKey(queue->key);
	Emu.GetIdManager().RemoveID<event_queue_t>(equeue_id);

	return CELL_OK;
}

s32 sys_event_queue_tryreceive(u32 equeue_id, vm::ptr<sys_event_t> event_array, s32 size, vm::ptr<u32> number)
{
	sys_event.Log("sys_event_queue_tryreceive(equeue_id=%d, event_array=*0x%x, size=%d, number=*0x%x)", equeue_id, event_array, size, number);

	LV2_LOCK;

	std::shared_ptr<event_queue_t> queue;

	if (!Emu.GetIdManager().GetIDData(equeue_id, queue))
	{
		return CELL_ESRCH;
	}

	if (size < 0)
	{
		throw __FUNCTION__;
	}

	if (queue->type != SYS_PPU_QUEUE)
	{
		return CELL_EINVAL;
	}

	s32 count = 0;

	while (!queue->waiters && count < size && queue->events.size())
	{
		auto& event = queue->events.front();
		event_array[count++] = { be_t<u64>::make(event.source), be_t<u64>::make(event.data1), be_t<u64>::make(event.data2), be_t<u64>::make(event.data3) };

		queue->events.pop_front();
	}

	*number = count;

	return CELL_OK;
}

s32 sys_event_queue_receive(PPUThread& CPU, u32 equeue_id, vm::ptr<sys_event_t> dummy_event, u64 timeout)
{
	sys_event.Log("sys_event_queue_receive(equeue_id=%d, event=*0x%x, timeout=0x%llx)", equeue_id, dummy_event, timeout);

	const u64 start_time = get_system_time();

	LV2_LOCK;

	std::shared_ptr<event_queue_t> queue;

	if (!Emu.GetIdManager().GetIDData(equeue_id, queue))
	{
		return CELL_ESRCH;
	}

	if (queue->type != SYS_PPU_QUEUE)
	{
		return CELL_EINVAL;
	}

	// protocol is ignored in current implementation
	queue->waiters++;

	while (queue->events.empty())
	{
		if (queue->cancelled)
		{
			queue->waiters--;
			return CELL_ECANCELED;
		}

		if (timeout && get_system_time() - start_time > timeout)
		{
			queue->waiters--;
			return CELL_ETIMEDOUT;
		}

		if (Emu.IsStopped())
		{
			sys_event.Warning("sys_event_queue_receive(equeue_id=%d) aborted", equeue_id);
			return CELL_OK;
		}

		queue->cv.wait_for(lv2_lock, std::chrono::milliseconds(1));
	}

	// event data is returned in registers (second arg is not used)
	auto& event = queue->events.front();
	CPU.GPR[4] = event.source;
	CPU.GPR[5] = event.data1;
	CPU.GPR[6] = event.data2;
	CPU.GPR[7] = event.data3;

	queue->events.pop_front();
	queue->waiters--;

	return CELL_OK;
}

s32 sys_event_queue_drain(u32 equeue_id)
{
	sys_event.Log("sys_event_queue_drain(equeue_id=%d)", equeue_id);

	LV2_LOCK;

	std::shared_ptr<event_queue_t> queue;

	if (!Emu.GetIdManager().GetIDData(equeue_id, queue))
	{
		return CELL_ESRCH;
	}

	queue->events = {};

	return CELL_OK;
}

u32 event_port_create(u64 name)
{
	std::shared_ptr<event_port_t> eport(new event_port_t(SYS_EVENT_PORT_LOCAL, name));
	
	return Emu.GetIdManager().GetNewID(eport, TYPE_EVENT_PORT);
}

s32 sys_event_port_create(vm::ptr<u32> eport_id, s32 port_type, u64 name)
{
	sys_event.Warning("sys_event_port_create(eport_id=*0x%x, port_type=%d, name=0x%llx)", eport_id, port_type, name);

	if (port_type != SYS_EVENT_PORT_LOCAL)
	{
		sys_event.Error("sys_event_port_create(): invalid port_type (%d)", port_type);
		return CELL_EINVAL;
	}

	std::shared_ptr<event_port_t> eport(new event_port_t(port_type, name));

	*eport_id = Emu.GetIdManager().GetNewID(eport, TYPE_EVENT_PORT);

	return CELL_OK;
}

s32 sys_event_port_destroy(u32 eport_id)
{
	sys_event.Warning("sys_event_port_destroy(eport_id=%d)", eport_id);

	LV2_LOCK;

	std::shared_ptr<event_port_t> port;

	if (!Emu.GetIdManager().GetIDData(eport_id, port))
	{
		return CELL_ESRCH;
	}

	if (!port->queue.expired())
	{
		return CELL_EISCONN;
	}

	Emu.GetIdManager().RemoveID<event_port_t>(eport_id);

	return CELL_OK;
}

s32 sys_event_port_connect_local(u32 eport_id, u32 equeue_id)
{
	sys_event.Warning("sys_event_port_connect_local(eport_id=%d, equeue_id=%d)", eport_id, equeue_id);

	LV2_LOCK;

	std::shared_ptr<event_port_t> port;
	std::shared_ptr<event_queue_t> queue;

	if (!Emu.GetIdManager().GetIDData(eport_id, port) || !Emu.GetIdManager().GetIDData(equeue_id, queue))
	{
		return CELL_ESRCH;
	}

	if (port->type != SYS_EVENT_PORT_LOCAL)
	{
		return CELL_EINVAL;
	}

	if (!port->queue.expired())
	{
		return CELL_EISCONN;
	}

	port->queue = queue;

	return CELL_OK;
}

s32 sys_event_port_disconnect(u32 eport_id)
{
	sys_event.Warning("sys_event_port_disconnect(eport_id=%d)", eport_id);

	LV2_LOCK;

	std::shared_ptr<event_port_t> port;

	if (!Emu.GetIdManager().GetIDData(eport_id, port))
	{
		return CELL_ESRCH;
	}

	std::shared_ptr<event_queue_t> queue = port->queue.lock();

	if (!queue)
	{
		return CELL_ENOTCONN;
	}

	// CELL_EBUSY is not returned

	//const u64 source = port->name ? port->name : ((u64)process_getpid() << 32) | (u64)eport_id;

	//for (auto& event : queue->events)
	//{
	//	if (event.source == source)
	//	{
	//		return CELL_EBUSY; // ???
	//	}
	//}

	port->queue.reset();

	return CELL_OK;
}

s32 sys_event_port_send(u32 eport_id, u64 data1, u64 data2, u64 data3)
{
	sys_event.Log("sys_event_port_send(eport_id=%d, data1=0x%llx, data2=0x%llx, data3=0x%llx)", eport_id, data1, data2, data3);

	LV2_LOCK;

	std::shared_ptr<event_port_t> port;

	if (!Emu.GetIdManager().GetIDData(eport_id, port))
	{
		return CELL_ESRCH;
	}

	std::shared_ptr<event_queue_t> queue = port->queue.lock();

	if (!queue)
	{
		return CELL_ENOTCONN;
	}

	if (queue->events.size() >= queue->size)
	{
		return CELL_EBUSY;
	}

	const u64 source = port->name ? port->name : ((u64)process_getpid() << 32) | (u64)eport_id;

	queue->push(source, data1, data2, data3);

	return CELL_OK;
}
