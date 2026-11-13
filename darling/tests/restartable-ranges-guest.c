/* Darwin arm64 or arm64e executable for a Darling guest: task_restartable_ranges_synchronize with
 * several threads (see RESTARTABLE-RANGES.md). It registers one range around a spin loop, runs
 * threads inside it and checks that synchronize, and a signal return, move their PCs to the range's
 * recovery address with the rest of the register state intact. Prints PASS and exits 0, or FAIL. */
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "the spin routine is written for arm64"
#endif

/* xnu osfmk/kern/restartable.h; no Darwin SDK header carries these. */
typedef struct {
	uint64_t location;
	unsigned short length;
	unsigned short recovery_offs;
	unsigned int flags;
} range_t;
extern kern_return_t task_restartable_ranges_register(task_t, range_t *, mach_msg_type_number_t);
extern kern_return_t task_restartable_ranges_synchronize(task_t);

/* One per spinning thread; the routine below owns the layout. */
struct spin {
	volatile uint64_t restarts;  /* +0  incremented by the recovery path */
	volatile uint64_t progress;  /* +8  incremented by every loop pass */
	volatile uint64_t stop;      /* +16 nonzero ends the loop */
	volatile uint64_t damaged;   /* +24 recovery saw a register or flag that changed */
};

extern void rr_spin(struct spin *);
extern void rr_addrs(uint64_t out[3]);

/* Range = Lstart..Lend. The first instruction is the range's first byte, which xnu never resets;
 * the loop sits after it. X13 and D8 hold a constant and Z is set across the loop, all untouched by
 * it: the recovery code checks them, so a stop that loses register state is counted. */
__asm__(
	".text\n"
	".p2align 4\n"
	".globl _rr_spin\n"
	"_rr_spin:\n"
	"	mov	x9, x0\n"
	"	movz	x13, #0x5a5a, lsl #48\n"
	"	movk	x13, #0x1234, lsl #32\n"
	"	movk	x13, #0xbeef, lsl #16\n"
	"	movk	x13, #0x0042\n"
	"	fmov	d8, x13\n"
	"	cmp	xzr, xzr\n"
	"Lstart:\n"
	"	nop\n"
	"Lloop:\n"
	"	ldr	x10, [x9, #16]\n"
	"	cbnz	x10, Lexit\n"
	"	ldr	x11, [x9, #8]\n"
	"	add	x11, x11, #1\n"
	"	str	x11, [x9, #8]\n"
	"	b	Lloop\n"
	"Lend:\n"
	"Lexit:\n"
	"	ret\n"
	"Lrecover:\n"
	"	b.ne	Lbad\n"
	"	movz	x14, #0x5a5a, lsl #48\n"
	"	movk	x14, #0x1234, lsl #32\n"
	"	movk	x14, #0xbeef, lsl #16\n"
	"	movk	x14, #0x0042\n"
	"	cmp	x13, x14\n"
	"	b.ne	Lbad\n"
	"	fmov	x15, d8\n"
	"	cmp	x15, x14\n"
	"	b.ne	Lbad\n"
	"	b	Lcount\n"
	"Lbad:\n"
	"	ldr	x11, [x9, #24]\n"
	"	add	x11, x11, #1\n"
	"	str	x11, [x9, #24]\n"
	"Lcount:\n"
	"	ldr	x11, [x9, #0]\n"
	"	add	x11, x11, #1\n"
	"	str	x11, [x9, #0]\n"
	"	cmp	xzr, xzr\n"
	"	b	Lloop\n"
	"\n"
	".p2align 2\n"
	".globl _rr_addrs\n"
	"_rr_addrs:\n"
	"	adr	x1, Lstart\n"
	"	str	x1, [x0]\n"
	"	adr	x1, Lend\n"
	"	str	x1, [x0, #8]\n"
	"	adr	x1, Lrecover\n"
	"	str	x1, [x0, #16]\n"
	"	ret\n");

static int failures;
#define CHECK(cond, ...) do { \
	int ok_ = (cond); \
	printf("%s %s: ", ok_ ? "CHECK" : "FAIL ", #cond); printf(__VA_ARGS__); printf("\n"); \
	if (!ok_) failures++; \
} while (0)

static uint64_t nowNs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

volatile const char *phaseName = "start";
volatile unsigned phaseCount;

static void onAlarm(int sig)
{
	static const char msg[] = "FAIL watchdog: the test did not finish in time (hang) in phase ";
	(void)sig;
	write(1, msg, sizeof(msg) - 1);
	write(1, (const char *)phaseName, strlen((const char *)phaseName));
	{
		char buf[64];
		int n = snprintf(buf, sizeof(buf), " after %u calls\n", phaseCount);
		write(1, buf, n);
	}
	_exit(124);
}

static void *spinner(void *arg)
{
	rr_spin(arg);
	return NULL;
}

#define MAX_SPIN 8
static struct spin spins[MAX_SPIN];
static pthread_t spinThreads[MAX_SPIN];

static void startSpinners(unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		memset(&spins[i], 0, sizeof(spins[i]));
		pthread_create(&spinThreads[i], NULL, spinner, &spins[i]);
	}
	for (unsigned i = 0; i < n; i++)
		while (spins[i].progress == 0)
			usleep(1000);
}

static void stopSpinners(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		spins[i].stop = 1;
	for (unsigned i = 0; i < n; i++)
		pthread_join(spinThreads[i], NULL);
}

static uint64_t totalRestarts(unsigned n)
{
	uint64_t sum = 0;
	for (unsigned i = 0; i < n; i++)
		sum += spins[i].restarts;
	return sum;
}

static uint64_t totalDamaged(unsigned n)
{
	uint64_t sum = 0;
	for (unsigned i = 0; i < n; i++)
		sum += spins[i].damaged;
	return sum;
}

/* Threads blocked in four different ways while synchronize runs, each released afterwards. */
static pthread_mutex_t condLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condVar = PTHREAD_COND_INITIALIZER;
static volatile int condGo, sleeperDone, condDone, readDone, machDone;
static int pipeFds[2];
static mach_port_t recvPort;

static void *sleeper(void *arg) { (void)arg; usleep(300000); sleeperDone = 1; return NULL; }
static void *condWaiter(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&condLock);
	while (!condGo)
		pthread_cond_wait(&condVar, &condLock);
	pthread_mutex_unlock(&condLock);
	condDone = 1;
	return NULL;
}
static void *pipeReader(void *arg)
{
	char c;
	(void)arg;
	readDone = read(pipeFds[0], &c, 1) == 1 && c == 'x';
	return NULL;
}
static void *machReceiver(void *arg)
{
	struct { mach_msg_header_t h; char pad[64]; } m;
	(void)arg;
	machDone = mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof(m), recvPort, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) == KERN_SUCCESS;
	return NULL;
}

static void waitFlag(volatile int *flag)
{
	for (int i = 0; i < 5000 && !*flag; i++)
		usleep(1000);
}

static void blockedThreadsPhase(unsigned syncs)
{
	pthread_t t[4];
	mach_msg_header_t msg;
	unsigned ok = 0;

	pipe(pipeFds);
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recvPort);
	mach_port_insert_right(mach_task_self(), recvPort, recvPort, MACH_MSG_TYPE_MAKE_SEND);
	pthread_create(&t[0], NULL, sleeper, NULL);
	pthread_create(&t[1], NULL, condWaiter, NULL);
	pthread_create(&t[2], NULL, pipeReader, NULL);
	pthread_create(&t[3], NULL, machReceiver, NULL);
	usleep(100000);
	for (unsigned i = 0; i < syncs; i++)
		ok += task_restartable_ranges_synchronize(mach_task_self()) == KERN_SUCCESS;
	CHECK(ok == syncs, "%u syncs with a sleeping, a condvar-, a read- and a mach_msg-blocked thread", ok);

	pthread_mutex_lock(&condLock);
	condGo = 1;
	pthread_cond_signal(&condVar);
	pthread_mutex_unlock(&condLock);
	write(pipeFds[1], "x", 1);
	memset(&msg, 0, sizeof(msg));
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof(msg);
	msg.msgh_remote_port = recvPort;
	mach_msg(&msg, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	waitFlag(&sleeperDone); waitFlag(&condDone); waitFlag(&readDone); waitFlag(&machDone);
	CHECK(sleeperDone && condDone && readDone && machDone,
		"all four blocked threads were released afterwards (sleep %d cond %d read %d mach %d)",
		sleeperDone, condDone, readDone, machDone);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
}

/* Threads that exit while synchronize runs. */
static volatile int churnStop;
static volatile unsigned churnCount;
static void *shortLived(void *arg) { (void)arg; return NULL; }
static void *churn(void *arg)
{
	(void)arg;
	while (!churnStop) {
		pthread_t t;
		if (pthread_create(&t, NULL, shortLived, NULL) == 0) {
			pthread_join(t, NULL);
			churnCount++;
		}
	}
	return NULL;
}

static volatile unsigned concurrentOk;
static void *syncMany(void *arg)
{
	unsigned n = (unsigned)(uintptr_t)arg;
	for (unsigned i = 0; i < n; i++)
		if (task_restartable_ranges_synchronize(mach_task_self()) == KERN_SUCCESS)
			__atomic_fetch_add(&concurrentOk, 1, __ATOMIC_RELAXED);
	return NULL;
}

/* A signal that interrupts a thread inside the range, with a handler that outlives a synchronize. */
static volatile int inHandler, releaseHandler, handlerDone;
static void onSignal(int sig)
{
	(void)sig;
	inHandler = 1;
	while (!releaseHandler)
		usleep(1000);
	handlerDone = 1;
}

/* A thread whose Mach calls return out-of-line data (task_threads) while others synchronize. */
static volatile int oolStop;
static volatile unsigned oolCalls, oolFailures, oolLastKr;
static void *oolCaller(void *arg)
{
	(void)arg;
	while (!oolStop) {
		thread_act_array_t threads;
		mach_msg_type_number_t count;
		kern_return_t kr = task_threads(mach_task_self(), &threads, &count);
		if (kr == KERN_SUCCESS) {
			for (mach_msg_type_number_t i = 0; i < count; i++)
				mach_port_deallocate(mach_task_self(), threads[i]);
			vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
		} else {
			oolFailures++;
			oolLastKr = kr;
		}
		oolCalls++;
	}
	return NULL;
}


static int wanted(const char *phases, char phase)
{
	return phases == NULL || strchr(phases, phase) != NULL;
}

/* usage: restartable-ranges-guest [PHASES]; PHASES is a subset of "nspcxbgo", default all. */
int main(int argc, char **argv)
{
	const char *phases = argc > 1 ? argv[1] : NULL;
	uint64_t a[3];
	range_t range;
	kern_return_t kr;
	const unsigned N = 4, SYNCS = 200;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, onAlarm);
	alarm(argc > 2 ? (unsigned)atoi(argv[2]) : 120);

	rr_addrs(a);
	range.location = a[0];
	range.length = (unsigned short)(a[1] - a[0]);
	range.recovery_offs = (unsigned short)(a[2] - a[0]);
	range.flags = 0;

	phaseName = "register";
	kr = task_restartable_ranges_synchronize(mach_task_self());
	CHECK(kr == KERN_SUCCESS, "synchronize before any registration: %d", kr);
	kr = task_restartable_ranges_register(mach_task_self(), &range, 1);
	CHECK(kr == KERN_SUCCESS, "register one range: %d", kr);
	kr = task_restartable_ranges_register(mach_task_self(), &range, 1);
	CHECK(kr == KERN_NOT_SUPPORTED, "second registration is refused: %d", kr);

	if (wanted(phases, 'n')) {
		unsigned fails = 0;
		uint64_t t0 = nowNs();
		phaseName = "n: N=1";
		for (unsigned s = 0; s < SYNCS; s++)
			if (task_restartable_ranges_synchronize(mach_task_self()) != KERN_SUCCESS)
				fails++;
		CHECK(fails == 0, "N=1: %u synchronizes with only the main thread, %.1f us each", SYNCS,
			(double)(nowNs() - t0) / SYNCS / 1000.0);
	}

	/* Everything below runs with N threads spinning inside the range. */
	startSpinners(N);

	if (wanted(phases, 's')) {
		/* Control: no synchronize, so the recovery path never runs. */
		phaseName = "s: control";
		usleep(300000);
		CHECK(totalRestarts(N) == 0, "control, no synchronize: %llu restarts", (unsigned long long)totalRestarts(N));
	}

	if (wanted(phases, 'p')) {
		/* Each thread is inside the range for all but a few instructions of its time, so each
		 * should be restarted once per call. */
		uint64_t before[MAX_SPIN], progress[MAX_SPIN], t0;
		unsigned fails = 0, moved = 0;
		phaseName = "p: spinners";
		for (unsigned i = 0; i < N; i++) { before[i] = spins[i].restarts; progress[i] = spins[i].progress; }
		t0 = nowNs();
		uint64_t waitNs = 0;
		for (phaseCount = 0; phaseCount < SYNCS; phaseCount++) {
			uint64_t seen[MAX_SPIN], w0;
			if (task_restartable_ranges_synchronize(mach_task_self()) != KERN_SUCCESS)
				fails++;
			/* A thread restarted by this call is back in the loop only once it has made progress;
			 * a call that arrives sooner finds it at the recovery address, outside the range, and
			 * rightly does not restart it again. */
			w0 = nowNs();
			for (unsigned i = 0; i < N; i++) seen[i] = spins[i].progress;
			for (unsigned i = 0; i < N; i++)
				while (spins[i].progress == seen[i] && nowNs() - w0 < 2000000000u)
					;
			waitNs += nowNs() - w0;
		}
		printf("INFO  N=%u (%u spinners and the caller): %.1f us per synchronize (%.1f us per call waiting for the spinners)\n", N + 1, N,
			(double)(nowNs() - t0 - waitNs) / SYNCS / 1000.0, (double)waitNs / SYNCS / 1000.0);
		/* The spinners store the restart count and the progress counter without a barrier between
		 * them, so a reader may see the second before the first: let both settle. */
		usleep(50000);
		for (unsigned i = 0; i < N; i++) {
			uint64_t d = spins[i].restarts - before[i];
			CHECK(d == SYNCS, "spinner %u was restarted by every call (%llu of %u)",
				i, (unsigned long long)d, SYNCS);
		}
		for (unsigned i = 0; i < N; i++)
			moved += spins[i].progress > progress[i];
		CHECK(fails == 0, "no synchronize failed (%u failures)", fails);
		CHECK(moved == N, "every spinner kept making progress afterwards (%u of %u)", moved, N);
		CHECK(totalDamaged(N) == 0, "registers, D8 and flags survived every restart (%llu damaged)",
			(unsigned long long)totalDamaged(N));
	}

	if (wanted(phases, 'c')) {
		pthread_t t[3];
		unsigned expect = 3 * 100;
		phaseName = "c: concurrent synchronizers";
		for (int i = 0; i < 3; i++)
			pthread_create(&t[i], NULL, syncMany, (void *)(uintptr_t)100);
		for (int i = 0; i < 3; i++)
			pthread_join(t[i], NULL);
		CHECK(concurrentOk == expect, "3 threads x 100 concurrent synchronizes all succeeded (%u of %u)",
			concurrentOk, expect);
	}

	if (wanted(phases, 'x')) {
		pthread_t c;
		unsigned fails = 0;
		phaseName = "x: threads exiting during synchronize";
		pthread_create(&c, NULL, churn, NULL);
		for (unsigned s = 0; s < SYNCS; s++)
			if (task_restartable_ranges_synchronize(mach_task_self()) != KERN_SUCCESS)
				fails++;
		churnStop = 1;
		pthread_join(c, NULL);
		CHECK(fails == 0, "%u synchronizes while %u threads were created and exited: %u failures",
			SYNCS, churnCount, fails);
	}

	if (wanted(phases, 'b')) {
		phaseName = "b: blocked threads";
		blockedThreadsPhase(20);
	}

	if (wanted(phases, 'o')) {
		pthread_t c;
		unsigned fails = 0;
		phaseName = "o: out-of-line replies";
		pthread_create(&c, NULL, oolCaller, NULL);
		for (unsigned s = 0; s < SYNCS; s++)
			if (task_restartable_ranges_synchronize(mach_task_self()) != KERN_SUCCESS)
				fails++;
		oolStop = 1;
		pthread_join(c, NULL);
		CHECK(fails == 0 && oolFailures == 0,
			"%u synchronizes while another thread made %u task_threads calls: %u synchronize failures, %u call failures (last %#x)",
			SYNCS, oolCalls, fails, oolFailures, oolLastKr);
	}

	if (wanted(phases, 'g')) {
		/* Signal return: the interrupted PC, saved in the signal frame, is inside the range
		 * while the handler runs. */
		uint64_t r0, r1;
		phaseName = "g: signal return";
		signal(SIGUSR1, onSignal);
		pthread_kill(spinThreads[0], SIGUSR1);
		for (int i = 0; i < 5000 && !inHandler; i++)
			usleep(1000);
		CHECK(inHandler, "the signal handler ran on the spinning thread");
		task_restartable_ranges_synchronize(mach_task_self());
		r0 = spins[0].restarts;
		releaseHandler = 1;
		waitFlag(&handlerDone);
		usleep(50000);
		r1 = spins[0].restarts;
		CHECK(r1 > r0, "returning from the handler restarted the interrupted thread (%llu -> %llu)",
			(unsigned long long)r0, (unsigned long long)r1);
		CHECK(totalDamaged(N) == 0, "registers survived the signal return (%llu damaged)",
			(unsigned long long)totalDamaged(N));
	}

	phaseName = "stop";
	stopSpinners(N);
	if (failures) {
		printf("FAIL restartable-ranges-guest: %d checks failed\n", failures);
		return 1;
	}
	printf("PASS restartable-ranges-guest\n");
	return 0;
}
