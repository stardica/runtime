/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <lib/util/list.h>
#include <lib/util/misc.h>
#include <lib/mhandle/mhandle.h>

#include <runtime/opencl/command.h>
#include <runtime/opencl/command-queue.h>
#include <runtime/opencl/debug.h>
#include <runtime/opencl/device.h>
#include <runtime/opencl/event.h>
#include <runtime/opencl/mem.h>
#include <runtime/opencl/x86-kernel.h>



/* Memory read */
static void opencl_command_run_mem_read(struct opencl_command_t *command)
{
	struct opencl_device_t *device = command->device;

	assert(device->arch_device_mem_read_func);
	device->arch_device_mem_read_func(device->arch_device, command->mem_read.host_ptr, command->mem_read.device_ptr, command->mem_read.size);

}


/* Memory write */
static void opencl_command_run_mem_write(struct opencl_command_t *command)
{
	struct opencl_device_t *device = command->device;

	assert(device->arch_device_mem_write_func);
	device->arch_device_mem_write_func(device->arch_device, command->mem_write.device_ptr, command->mem_write.host_ptr, command->mem_write.size);
}


/* Memory read */
static void opencl_command_run_mem_copy(struct opencl_command_t *command)
{
	struct opencl_device_t *device = command->device;

	assert(device->arch_device_mem_copy_func);
	device->arch_device_mem_copy_func(
			device->arch_device,
			command->mem_copy.device_dest_ptr,
			command->mem_copy.device_src_ptr,
			command->mem_copy.size);
}


/* Map a buffer */
static void opencl_command_run_map_buffer(struct opencl_command_t *command)
{
	struct opencl_mem_t *mem = command->map_buffer.mem;
	struct opencl_device_t *device = command->device;

	/* Check that host pointer is still valid. This may not be true
	 * if there was a map/unmap race condition between command queues. */
	if (!mem->mapped)
		fatal("%s: cl_mem no longer mapped - race condition?",
				__FUNCTION__);

	/* If the CL_MAP_READ flag was set, copy buffer from device to host */
	assert(mem->host_ptr);
	if (mem->map_flags & 1)
	{
		device->arch_device_mem_read_func(
				device->arch_device,
				mem->host_ptr + mem->map_offset,
				mem->device_ptr + mem->map_offset,
				mem->map_size);
		opencl_debug("\t%d bytes copied from device [%p+%u] to host [%p+%u]",
				mem->map_size, mem->device_ptr, mem->map_offset,
				mem->host_ptr, mem->map_offset);
	}
}


/* Unmap a buffer */
static void opencl_command_run_unmap_buffer(struct opencl_command_t *command)
{
	struct opencl_mem_t *mem = command->unmap_buffer.mem;
	struct opencl_device_t *device = command->device;

	/* Check that host pointer is still valid. This may not be true if
	 * there was a map/unmap race condition between command queues. */
	if (!mem->mapped)
		fatal("%s: cl_mem no longer mapped - race condition?",
				__FUNCTION__);

	/* If the CL_MAP_WRITE flag was set, copy buffer from host to device */
	assert(mem->host_ptr);
	if (mem->map_flags & 2)
	{
		device->arch_device_mem_write_func(
				device->arch_device,
				mem->device_ptr + mem->map_offset,
				mem->host_ptr + mem->map_offset,
				mem->map_size);
		opencl_debug("\t%d bytes copied from host [%p+%u] to devicem [%p+%u]",
				mem->map_size, mem->host_ptr, mem->map_offset,
				mem->device_ptr, mem->map_offset);
	}

	/* Free host memory, but only if the host buffer was allocated with a call
	 * to clMapBuffer. This is not the case when the buffer used a host
	 * pointer given by the user in clCreateBuffer. */
	assert(mem->host_ptr);
	if (!mem->use_host_ptr)
	{
		free(mem->host_ptr);
		mem->host_ptr = NULL;
	}
}

/* Run a a native kernel ND-Range */
static void opencl_command_run_native_kernel_ndrange(struct opencl_command_t *command)
{

	//printf("opencl_command_run_ndrange()\n");

	struct timespec start, end;
	cl_ulong cltime;

	if (command->done_event)
	{
		clock_gettime(CLOCK_MONOTONIC, &start);
	}

	opencl_native_kernel_args **arg = NULL;
	opencl_cpu_native_func_t kernel_func = NULL;
	unsigned int num_args;
	int i = 0;

	//get the kernel function
	kernel_func = (opencl_cpu_native_func_t)command->ndrange;

	//get the kernel args
	num_args = list_count(command->arg_list);

	//build the arg list
	arg = (opencl_native_kernel_args **)xmalloc(num_args*sizeof(opencl_native_kernel_args));

	for(i = 0; i < num_args; i++)
		arg[i] = list_get(command->arg_list, i);

	kernel_func(num_args, (void *)arg, (void*)command->work_space);

	if (command->done_event)
	{
		clock_gettime(CLOCK_MONOTONIC, &end);

		cltime = (cl_ulong)start.tv_sec;
		cltime *= 1000000000;
		cltime += (cl_ulong)start.tv_nsec;
		command->done_event->time_start = cltime;

		cltime = (cl_ulong)end.tv_sec;
		cltime *= 1000000000;
		cltime += (cl_ulong)end.tv_nsec;
		command->done_event->time_end = cltime;
	}
}

/* Run an ND-Range */
static void opencl_command_run_ndrange(struct opencl_command_t *command)
{

	//printf("opencl_command_run_ndrange()\n");

	assert(command->ndrange);
	assert(command->device->arch_ndrange_run_func);

	struct opencl_ndrange_t *ndrange;

	struct timespec start, end;

	cl_ulong cltime;

	ndrange = command->ndrange;

	if (command->done_event)
	{
		clock_gettime(CLOCK_MONOTONIC, &start);
	}

	//printf("Running the NDRange\n");
	command->device->arch_ndrange_run_func(ndrange->arch_ndrange);

	if (command->done_event)
	{
		clock_gettime(CLOCK_MONOTONIC, &end);

		cltime = (cl_ulong)start.tv_sec;
		cltime *= 1000000000;
		cltime += (cl_ulong)start.tv_nsec;
		command->done_event->time_start = cltime;

		cltime = (cl_ulong)end.tv_sec;
		cltime *= 1000000000;
		cltime += (cl_ulong)end.tv_nsec;
		command->done_event->time_end = cltime;
	}
}



/*
 * Public Functions
 */

/*extern struct opencl_command_t *opencl_create_test_command(opencl_command_test_func_t func,
		cl_command_queue *command_queue){

	struct opencl_command_t *command;

	command = opencl_command_create(opencl_command_test,
		(void *)func,
		(struct opencl_command_queue_t *) command_queue,
		NULL,
		0,
		NULL);

	return command;
}*/

struct opencl_command_t *opencl_command_create(
		enum opencl_command_type_t type,
		opencl_command_func_t func,
		struct opencl_command_queue_t *command_queue,
		cl_event *done_event_ptr,
		int num_wait_events,
		cl_event *wait_events)
{
	struct opencl_command_t *command;
	int i;

	/* Initialize */
	command = xcalloc(1, sizeof(struct opencl_command_t));
	command->type = type;
	command->func = func;
	command->command_queue = command_queue;
	command->device = command_queue->device;
	command->num_wait_events = num_wait_events;
	command->wait_events = wait_events;

	/* The command has a reference to all the prerequisite events */
	for (i = 0; i < num_wait_events; i++)
		if (clRetainEvent(wait_events[i]) != CL_SUCCESS)
			fatal("%s: clRetainEvent failed on prerequisite event", __FUNCTION__);

	/* Completion event */
	if (done_event_ptr)
	{
		command->done_event = opencl_event_create(command_queue);
		*done_event_ptr = command->done_event;
		if (clRetainEvent(*done_event_ptr) != CL_SUCCESS)
			fatal("%s: clRetainEvent failed on done event", __FUNCTION__);
	}

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_create_nop(
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	return opencl_command_create(opencl_command_nop, NULL, command_queue,
			done_event_ptr, num_wait_events, wait_events);
}


struct opencl_command_t *opencl_command_create_end(
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	return opencl_command_create(opencl_command_end, NULL, command_queue,
			done_event_ptr, num_wait_events, wait_events);
}


struct opencl_command_t *opencl_command_create_mem_read(
		void *host_ptr,
		void *device_ptr,
		unsigned int size,
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_mem_read, opencl_command_run_mem_read, command_queue, done_event_ptr, num_wait_events, wait_events);
	command->mem_read.host_ptr = host_ptr;
	command->mem_read.device_ptr = device_ptr;
	command->mem_read.size = size;

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_create_mem_write(
		void *device_ptr,
		void *host_ptr,
		unsigned int size,
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_mem_write, opencl_command_run_mem_write, command_queue, done_event_ptr, num_wait_events, wait_events);
	command->mem_write.device_ptr = device_ptr;
	command->mem_write.host_ptr = host_ptr;
	command->mem_write.size = size;

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_create_mem_copy(
		void *device_dest_ptr,
		void *device_src_ptr,
		unsigned int size,
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_mem_copy,
			opencl_command_run_mem_copy,
			command_queue, done_event_ptr, num_wait_events,
			wait_events);
	command->mem_copy.device_dest_ptr = device_dest_ptr;
	command->mem_copy.device_src_ptr = device_src_ptr;
	command->mem_copy.size = size;

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_create_map_buffer(
		struct opencl_mem_t *mem,
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_map_buffer,
			opencl_command_run_map_buffer, command_queue,
			done_event_ptr, num_wait_events, wait_events);
	command->map_buffer.mem = mem;

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_create_unmap_buffer(
		struct opencl_mem_t *mem,
		struct opencl_command_queue_t *command_queue,
		struct opencl_event_t **done_event_ptr,
		int num_wait_events,
		struct opencl_event_t **wait_events)
{
	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_unmap_buffer,
			opencl_command_run_unmap_buffer, command_queue,
			done_event_ptr, num_wait_events, wait_events);
	command->unmap_buffer.mem = mem;

	/* Return */
	return command;
}

struct opencl_command_t *opencl_command_run_native_kernel_init(
	struct opencl_command_queue_t *command_queue,
	struct opencl_event_t **done_event_ptr,
	int num_wait_events,
	struct opencl_event_t **wait_events)
{

	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_launch_ndrange, opencl_command_run_native_kernel_ndrange, command_queue, done_event_ptr, num_wait_events, wait_events);

	/* Return */
	return command;
}


struct opencl_command_t *opencl_command_run_ndrage_init(
	struct opencl_command_queue_t *command_queue,
	struct opencl_event_t **done_event_ptr,
	int num_wait_events,
	struct opencl_event_t **wait_events)
{

	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_launch_ndrange, opencl_command_run_ndrange, command_queue, done_event_ptr, num_wait_events, wait_events);

	/* Return */
	return command;
}

/*struct opencl_ndrange_t * opencl_create_ndrange_multi(
	struct opencl_kernel_t *kernel,
	int work_dim,
	unsigned int *global_work_offset,
	unsigned int *global_work_size,
	unsigned int *local_work_size)
{
	assert(kernel);
	assert(global_work_size);
	assert(global_work_offset == 0);
	assert(IN_RANGE(work_dim, 1, 3));

	//struct opencl_device_t *device;
	struct opencl_ndrange_t *ndrange;
	struct opencl_kernel_entry_t *kernel_entry;
	int i = 0;

	ndrange = xcalloc(1, sizeof(struct opencl_ndrange_t));
	ndrange->num_devices = list_count(kernel->entry_list);
	//ndrange->device = device;
	ndrange->kernel = kernel;

	//get each kernel entry
	for(i=0;i<ndrange->num_devices;i++)
	{

		kernel_entry = list_get(kernel->entry_list, i);

		//for binary entries that need driver interventions
		if(kernel_entry->kernel_type == kernel_cpu_native)
		{

			star FIXME make this a true callback func
			//printf("ARGs to CPU work_dim %d, GWS %d, LWS %d\n", work_dim, *global_work_size, *local_work_size);
			kernel_entry->device->device_ndrange = (void *)opencl_x86_ndrange_create_native(ndrange,
																					work_dim,
																					global_work_offset,
																					global_work_size,
																					local_work_size);

			printf("NDRange (native) setup name %s type %d\n", kernel_entry->device->name, (int) kernel_entry->device->type);

		}
		else if (kernel_entry->kernel_type == kernel_gpu_binary)
		{
			//printf("ARGs to GPU work_dim %d, GWS %d, LWS %d\n", work_dim, *global_work_size, *local_work_size);
			kernel_entry->device->device_ndrange = kernel_entry->device->arch_ndrange_create_func( ndrange,
																					kernel_entry->arch_kernel,
																					work_dim,
																					global_work_offset,
																					global_work_size,
																					local_work_size,
																					0);

			printf("NDRange setup name %s type %d\n", kernel_entry->device->name, (int) kernel_entry->device->type);
		}

	}

	return ndrange;
}*/



struct opencl_command_t *opencl_command_create_ndrange(
	struct opencl_device_t *device,
	struct opencl_kernel_t *kernel,
	int work_dim,
	unsigned int *global_work_offset,
	unsigned int *global_work_size,
	unsigned int *local_work_size,
	struct opencl_command_queue_t *command_queue,
	struct opencl_event_t **done_event_ptr,
	int num_wait_events,
	struct opencl_event_t **wait_events)
{

	struct opencl_command_t *command;

	/* Initialize */
	command = opencl_command_create(opencl_command_launch_ndrange, opencl_command_run_ndrange, command_queue, done_event_ptr, num_wait_events, wait_events);

	command->ndrange = opencl_ndrange_create(device, kernel, work_dim, global_work_offset, global_work_size, local_work_size);

	/* Return */
	return command;
}


void opencl_command_free(struct opencl_command_t *command)
{
	int i;

	/* Release events */
	for (i = 0; i < command->num_wait_events; i++)
		if (clReleaseEvent(command->wait_events[i]) != CL_SUCCESS)
			fatal("%s: clReleaseEvent failed on prerequisite event", __FUNCTION__);

	/* Completion event */
	if (command->done_event)
	{
		if (clReleaseEvent(command->done_event) != CL_SUCCESS)
			fatal("%s: clReleaseEvent failed on done event", __FUNCTION__);
	}

	/* Free */
	free(command);
}


/*void opencl_command_run_multi(struct opencl_command_t *command)
{
	int i = 0;
	struct opencl_kernel_entry_t *kernel_entry;

	if (command->num_wait_events > 0)
	{
		printf("Command queue wait for event\n");
		clWaitForEvents(command->num_wait_events, command->wait_events);
	}

	//how many devices are there
	for(i=0;i<command->num_devices;i++)
	{
		kernel_entry = list_get(command->ndrange_multi->kernel->entry_list, i);

		if(kernel_entry->device->type == CL_DEVICE_TYPE_CPU)
		{
			printf("found CPU device type %d\n", (int)kernel_entry->device->type);
		}
		else if(kernel_entry->device->type == CL_DEVICE_TYPE_GPU)
		{
			printf("found GPU device type %d\n", (int)kernel_entry->device->type);

			printf("running GPU device type %d\n", (int)kernel_entry->device->type);
			command->ndrange = kernel_entry->device->device_ndrange;
			command->device = kernel_entry->device;
			command->func(command);
		}
	}

	fatal("STOP END\n");

	if (command->func)
	{
		printf("Command queue run the function\n");
		command->func(command);
	}

	if (command->done_event)
	{
		printf("Command queue done event\n");
		opencl_event_set_status(command->done_event, CL_COMPLETE);
	}
}*/



void opencl_command_run(struct opencl_command_t *command)
{
	if (command->num_wait_events > 0)
	{
		printf("Command queue wait for event\n");
		clWaitForEvents(command->num_wait_events, command->wait_events);
	}

	if (command->func)
	{
		//printf("Command queue run the function\n");
		command->func(command);
	}

	if (command->done_event)
	{
		//printf("Command queue done event\n");
		opencl_event_set_status(command->done_event, CL_COMPLETE);
	}
}


