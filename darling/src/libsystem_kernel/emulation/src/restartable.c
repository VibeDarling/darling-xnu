/* XNU serves the restartable-range interface through MIG, backed by per-task state in the kernel.
 * Darling has no kernel task to interrupt, so the guest implementations validate and record the
 * caller's ranges and keep the single-threaded semantics the objc runtime relies on. */

#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_types.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <stdint.h>

#include <darling/emulation/common/simple.h>

/* xnu osfmk/kern/restartable.h defines this; no Darwin SDK header carries it. */
typedef struct {
	uint64_t location;
	unsigned short length;
	unsigned short recovery_offs;
	unsigned int flags;
} darling_restartable_range_t;

#define RESTARTABLE_OFFSET_MAX 4096u
#define RESTARTABLE_RANGE_MAX 1024u

static darling_restartable_range_t gRanges[RESTARTABLE_RANGE_MAX];
static uint32_t gRangeCount;
static int gRegistered;

static kern_return_t
callingTaskThreadCount(mach_msg_type_number_t *count)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t threadCount = 0;
	kern_return_t kr = task_threads(mach_task_self(), &threads, &threadCount);

	if (kr != KERN_SUCCESS)
		return kr;
	for (mach_msg_type_number_t i = 0; i < threadCount; i++)
		mach_port_deallocate(mach_task_self(), threads[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)threads, threadCount * sizeof(thread_t));
	*count = threadCount;
	return KERN_SUCCESS;
}

/* Same rules as xnu's _ranges_validate: no wrap, offsets within a page, flags clear, and no
 * overlap. xnu detects overlap after sorting its MIG copy; a read-only pairwise scan gives the
 * same answers and leaves the caller's array untouched. */
static kern_return_t
validateRanges(const darling_restartable_range_t *ranges, uint32_t count)
{
	uint64_t end, recovery;

	for (uint32_t i = 0; i < count; i++) {
		if (ranges[i].length > RESTARTABLE_OFFSET_MAX
				|| ranges[i].recovery_offs > RESTARTABLE_OFFSET_MAX)
			return KERN_INVALID_ARGUMENT;
		if (ranges[i].flags)
			return KERN_INVALID_ARGUMENT;
		if (__builtin_add_overflow(ranges[i].location, (uint64_t)ranges[i].length, &end))
			return KERN_INVALID_ARGUMENT;
		if (__builtin_add_overflow(ranges[i].location, (uint64_t)ranges[i].recovery_offs, &recovery))
			return KERN_INVALID_ARGUMENT;
	}
	/* No addition below can wrap: every range passed the checks above. */
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t j = i + 1; j < count; j++) {
			if (ranges[i].location < ranges[j].location + ranges[j].length
					&& ranges[j].location < ranges[i].location + ranges[i].length)
				return KERN_INVALID_ARGUMENT;
		}
	}
	return KERN_SUCCESS;
}

__attribute__ ((visibility ("default")))
kern_return_t
task_restartable_ranges_register(task_t task, darling_restartable_range_t *ranges,
		mach_msg_type_number_t count)
{
	mach_msg_type_number_t threadCount;
	kern_return_t kr;

	if (task != mach_task_self())
		return KERN_FAILURE;
	if (ranges == NULL || count == 0)
		return KERN_INVALID_ARGUMENT;

	/* xnu validates the received array before the count limit and the task-state checks; mirror
	 * that order so rejection codes match. */
	kr = validateRanges(ranges, count);
	if (kr != KERN_SUCCESS)
		return kr;
	if (count > RESTARTABLE_RANGE_MAX)
		return KERN_RESOURCE_SHORTAGE;

	if (gRegistered) {
		__simple_printf("restartable ranges: already registered, rejecting second registration\n");
		return KERN_NOT_SUPPORTED;
	}

	kr = callingTaskThreadCount(&threadCount);
	if (kr != KERN_SUCCESS) {
		__simple_printf("restartable ranges: cannot count threads: %d\n", kr);
		return KERN_FAILURE;
	}
	if (threadCount != 1) {
		__simple_printf("restartable ranges: registration with %u threads is not supported\n", threadCount);
		return KERN_NOT_SUPPORTED;
	}

	for (mach_msg_type_number_t i = 0; i < count; i++)
		gRanges[i] = ranges[i];
	gRangeCount = count;
	gRegistered = 1;
	__simple_printf("restartable ranges: registered %u ranges\n", gRangeCount);
	return KERN_SUCCESS;
}

__attribute__ ((visibility ("default")))
kern_return_t
task_restartable_ranges_synchronize(task_t task)
{
	mach_msg_type_number_t threadCount;
	kern_return_t kr;

	if (task != mach_task_self())
		return KERN_FAILURE;

	/* xnu resets the PC of every other thread inside a range; with one thread there is none. */
	kr = callingTaskThreadCount(&threadCount);
	if (kr != KERN_SUCCESS) {
		__simple_printf("restartable ranges: cannot count threads: %d\n", kr);
		return KERN_FAILURE;
	}
	if (threadCount != 1) {
		__simple_printf("restartable ranges: synchronize with %u threads is not implemented\n", threadCount);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}
