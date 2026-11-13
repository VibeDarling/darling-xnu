/* XNU serves the restartable-range interface through MIG, backed by per-task state in the kernel.
 * Darling has no kernel task to interrupt, so the guest implementations validate and record the
 * caller's ranges. task_restartable_ranges_synchronize stops no thread: it sends every other
 * thread of the process a real-time signal (SIGNAL_RESTARTABLE_SYNC, handled in sigexc.c), and
 * the handler, running on the interrupted thread, rewrites the interrupted PC to the range's
 * recovery address when it is inside a range, then acknowledges. */

#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_types.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <stdint.h>

#include <darling/emulation/common/simple.h>
#include <errno.h>
#include <darling/emulation/conversion/duct_errno.h>
#include <darling/emulation/conversion/common_at.h>
#include <darling/emulation/conversion/fcntl/open.h>
#include <darling/emulation/linux_premigration/ext/futex.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>
#include <darling/emulation/linux_premigration/signal/sigexc.h>
#include <darling/emulation/other/restartable.h>

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

/* xnu's thread_reset_pcs_ast(): a PC strictly inside a range, not at its first byte and not at
 * its end, is replaced by the range's recovery address. Ranges are registered once, before the
 * task has a second thread, so readers need no lock. Returns 0 when the PC is not inside a range. */
static uint64_t
recoveryPC(uint64_t pc)
{
	if (!gRegistered)
		return 0;
	for (uint32_t i = 0; i < gRangeCount; i++) {
		if (pc > gRanges[i].location && pc < gRanges[i].location + gRanges[i].length)
			return gRanges[i].location + gRanges[i].recovery_offs;
	}
	return 0;
}

/* One synchronize call at a time: its rounds share the slot tables the handler reads. */
#define SYNC_THREAD_MAX 4096
#define SYNC_PASSES 4
#define SYNC_WAITING 0
#define SYNC_HANDLED 1
#define SYNC_GONE 2
#define SYNC_TIMEOUT_MS 10000

static int gSyncTid[SYNC_THREAD_MAX];   /* the slots of the round in progress */
static int gSyncState[SYNC_THREAD_MAX];
static int gSyncCount;
static int gSyncOutstanding;
static int gSyncSeen[SYNC_THREAD_MAX];  /* every thread id signalled by this call */
static int gSyncListed[SYNC_THREAD_MAX];
static int gSyncParked[64];             /* threads that blocked the signal with it pending */
static int gSyncParkedCount;
static int gSyncLock; /* 0 free, 1 held, 2 held and waited on */

static void
syncLock(void)
{
	int expected = 0;

	if (__atomic_compare_exchange_n(&gSyncLock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	while (__atomic_exchange_n(&gSyncLock, 2, __ATOMIC_ACQUIRE) != 0)
		__linux_futex_reterr(&gSyncLock, FUTEX_WAIT, 2, NULL, NULL, 0);
}

static void
syncUnlock(void)
{
	if (__atomic_exchange_n(&gSyncLock, 0, __ATOMIC_RELEASE) == 2)
		__linux_futex_reterr(&gSyncLock, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static void
syncSlotDone(int slot, int state)
{
	int expected = SYNC_WAITING;

	if (!__atomic_compare_exchange_n(&gSyncState[slot], &expected, state, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	if (__atomic_sub_fetch(&gSyncOutstanding, 1, __ATOMIC_SEQ_CST) == 0)
		__linux_futex_reterr(&gSyncOutstanding, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* Runs in the signal handler of the interrupted thread, which may be any thread of the process.
 * Async-signal-safe: raw system calls and atomics only, no thread-local storage. Returns the PC
 * the thread resumes at. */
uint64_t
darling_restartable_sync_interrupted(uint64_t pc)
{
	int tid = (int)LINUX_SYSCALL(__NR_gettid);
	int count = __atomic_load_n(&gSyncCount, __ATOMIC_ACQUIRE);

	for (int i = 0; i < count; i++) {
		if (gSyncTid[i] != tid)
			continue;
		uint64_t recovery = recoveryPC(pc);

		syncSlotDone(i, SYNC_HANDLED);
		return recovery ? recovery : pc;
	}
	return pc;
}

/* The thread ids of the process, from /proc/self/task, without the caller's. */
static int
listThreads(int *tids, int max, int self)
{
	char buf[2048];
	int count = 0;
	long fd = LINUX_SYSCALL(__NR_openat, LINUX_AT_FDCWD, "/proc/self/task",
			LINUX_O_RDONLY | LINUX_O_DIRECTORY | LINUX_O_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	for (;;) {
		long n = LINUX_SYSCALL(__NR_getdents64, fd, buf, sizeof(buf));

		if (n <= 0) {
			LINUX_SYSCALL(__NR_close, fd);
			return n < 0 ? -1 : count;
		}
		for (long off = 0; off < n;) {
			/* struct linux_dirent64: u64 ino, s64 off, u16 reclen, u8 type, char name[] */
			unsigned short reclen = *(unsigned short *)(buf + off + 16);
			const char *name = buf + off + 19;
			int tid = 0, digits = 0;

			for (; *name >= '0' && *name <= '9'; name++, digits++)
				tid = tid * 10 + (*name - '0');
			if (digits && !*name && tid != self) {
				if (count == max) {
					LINUX_SYSCALL(__NR_close, fd);
					return -2;
				}
				tids[count++] = tid;
			}
			off += reclen;
		}
	}
}

/* The hex mask of a "Name:" line of a status file, or 0. */
static unsigned long long
statusMask(const char *buf, long n, const char *name, int nameLen)
{
	for (long i = 0; i + nameLen < n; i++) {
		int match = 1;

		for (int j = 0; j < nameLen && match; j++)
			match = buf[i + j] == name[j];
		if (!match || (i && buf[i - 1] != '\n'))
			continue;

		unsigned long long mask = 0;

		for (i += nameLen; buf[i] == ' ' || buf[i] == '\t'; i++)
			;
		for (;; i++) {
			char c = buf[i];
			int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

			if (d < 0)
				break;
			mask = mask << 4 | (unsigned)d;
		}
		return mask;
	}
	return 0;
}

/* Whether the thread blocks the synchronize signal and has it pending (SigBlk and SigPnd in its
 * status file). Only native loader code blocks it for long: the guest's RPC wrappers block signals
 * 1 to 31, and a thread parked with every signal blocked, for instance the main thread after
 * dispatch_main, never runs guest code, so it cannot be inside a range. A thread that is inside
 * the handler also blocks the signal, but there it is no longer pending, and it must be waited
 * for: its handler is about to rewrite the PC. */
static int
threadBlocksSyncSignal(int tid)
{
	char path[48] = "/proc/self/task/", buf[4096];
	int len = 16, digits = 0;
	char rev[12];
	long fd, n;
	unsigned long long bit = 1ull << (SIGNAL_RESTARTABLE_SYNC - 1);

	for (int v = tid; v > 0; v /= 10)
		rev[digits++] = '0' + v % 10;
	while (digits)
		path[len++] = rev[--digits];
	for (const char *t = "/status"; *t; t++)
		path[len++] = *t;
	path[len] = 0;

	fd = LINUX_SYSCALL(__NR_openat, LINUX_AT_FDCWD, path, LINUX_O_RDONLY | LINUX_O_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	n = LINUX_SYSCALL(__NR_read, fd, buf, sizeof(buf) - 1);
	LINUX_SYSCALL(__NR_close, fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	return (statusMask(buf, n, "SigBlk:", 7) & bit) && (statusMask(buf, n, "SigPnd:", 7) & bit);
}

/* A thread that exits after the signal was sent, or that blocks it for good, never runs the
 * handler. */
static void
reapUnreachableThreads(int pid)
{
	for (int i = 0; i < gSyncCount; i++) {
		if (__atomic_load_n(&gSyncState[i], __ATOMIC_ACQUIRE) != SYNC_WAITING)
			continue;
		if (LINUX_SYSCALL(__NR_tgkill, pid, gSyncTid[i], 0) == -LINUX_ESRCH) {
			syncSlotDone(i, SYNC_GONE);
		} else if (threadBlocksSyncSignal(gSyncTid[i])) {
			if (gSyncParkedCount < (int)(sizeof(gSyncParked) / sizeof(gSyncParked[0])))
				gSyncParked[gSyncParkedCount++] = gSyncTid[i];
			syncSlotDone(i, SYNC_GONE);
		}
	}
}

/* Whether the thread was found parked with the signal blocked and pending by an earlier round and
 * still is. Signalling it again would queue one more real-time signal per call, never consumed,
 * against the user's pending-signal limit. */
static int
stillParked(int tid)
{
	for (int i = 0; i < gSyncParkedCount; i++) {
		if (gSyncParked[i] != tid)
			continue;
		if (threadBlocksSyncSignal(tid))
			return 1;
		gSyncParked[i] = gSyncParked[--gSyncParkedCount];
		return 0;
	}
	return 0;
}

/* Signal the n threads in gSyncTid and wait for all of them. */
static kern_return_t
syncRound(int n, int pid)
{
	struct { long tv_sec; long tv_nsec; } oneMs = { 0, 1000000 };
	kern_return_t result = KERN_SUCCESS;

	for (int i = 0; i < n; i++)
		gSyncState[i] = SYNC_WAITING;
	gSyncOutstanding = n;
	__atomic_store_n(&gSyncCount, n, __ATOMIC_RELEASE);

	for (int i = 0; i < n; i++) {
		long rv;

		if (stillParked(gSyncTid[i])) {
			syncSlotDone(i, SYNC_GONE);
			continue;
		}
		rv = LINUX_SYSCALL(__NR_tgkill, pid, gSyncTid[i], SIGNAL_RESTARTABLE_SYNC);
		if (rv == 0)
			continue;
		if (rv != -LINUX_ESRCH) {
			__simple_printf("restartable ranges: tgkill of thread %d failed: %ld\n", gSyncTid[i], rv);
			result = KERN_FAILURE;
		}
		syncSlotDone(i, SYNC_GONE);
	}

	for (int waited = 0; __atomic_load_n(&gSyncOutstanding, __ATOMIC_SEQ_CST) > 0;) {
		int outstanding = __atomic_load_n(&gSyncOutstanding, __ATOMIC_SEQ_CST);
		int rv;

		if (waited >= SYNC_TIMEOUT_MS) {
			__simple_printf("restartable ranges: %d threads did not acknowledge in %d ms\n", outstanding, SYNC_TIMEOUT_MS);
			result = KERN_FAILURE;
			break;
		}
		rv = __linux_futex_reterr(&gSyncOutstanding, FUTEX_WAIT, outstanding, (const struct timespec *)&oneMs, NULL, 0);
		if (rv == -ETIMEDOUT) /* the converted errno keeps the futex call's negative sign */
			waited++;
		if (__atomic_load_n(&gSyncOutstanding, __ATOMIC_SEQ_CST) > 0)
			reapUnreachableThreads(pid);
	}

	/* No late handler of this round may match a slot of the next one. */
	for (int i = 0; i < n; i++)
		__atomic_store_n(&gSyncState[i], SYNC_GONE, __ATOMIC_RELEASE);
	__atomic_store_n(&gSyncCount, 0, __ATOMIC_RELEASE);
	return result;
}

__attribute__ ((visibility ("default")))
kern_return_t
task_restartable_ranges_synchronize(task_t task)
{
	int pid, self, seen = 0;
	kern_return_t result = KERN_SUCCESS;

	if (task != mach_task_self())
		return KERN_FAILURE;

	/* xnu: without registered ranges there is nothing to reset. */
	if (!gRegistered)
		return KERN_SUCCESS;

	/* Pairs with the handler: the caller's update of whatever the ranges protect is visible to a
	 * thread before the thread is inspected (xnu: release fence, then the AST). */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	syncLock();
	pid = (int)LINUX_SYSCALL(__NR_getpid);
	self = (int)LINUX_SYSCALL(__NR_gettid);

	/* Reading a directory while a thread exits can skip the thread that follows it, so list again
	 * and signal what an earlier listing missed. Threads created meanwhile are signalled too,
	 * which is harmless; the passes are bounded so a thread-creating storm cannot keep the call
	 * from returning. A thread skipped by every listing is still missed; that takes the same
	 * unlucky exit race in each pass. */
	for (int pass = 0; pass < SYNC_PASSES && result == KERN_SUCCESS; pass++) {
		int count = listThreads(gSyncListed, SYNC_THREAD_MAX, self), n = 0;

		if (count < 0) {
			__simple_printf("restartable ranges: cannot list the threads of the process (%d)\n", count);
			result = KERN_FAILURE;
			break;
		}
		for (int i = 0; i < count; i++) {
			int known = 0;

			for (int j = 0; j < seen && !known; j++)
				known = gSyncSeen[j] == gSyncListed[i];
			if (known)
				continue;
			if (seen == SYNC_THREAD_MAX) {
				__simple_printf("restartable ranges: more than %d threads\n", SYNC_THREAD_MAX);
				result = KERN_FAILURE;
				break;
			}
			gSyncSeen[seen] = gSyncListed[i];
			gSyncTid[n++] = gSyncListed[i];
			seen++;
		}
		if (n == 0)
			break;
		if (result == KERN_SUCCESS)
			result = syncRound(n, pid);
	}

	syncUnlock();
	return result;
}
