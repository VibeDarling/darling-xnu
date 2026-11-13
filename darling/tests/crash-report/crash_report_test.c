// Crashing programs for the Darling crash report (DARLING_CRASH_REPORT=1). Each case prints what the report
// must say, computed with dladdr() (dyld's own image search, independent of the reporter), then crashes.
//
//   crash_report_test <case>
//
// Lines on stdout:  EXPECT <key> <value>
//   pc-image   path of the image holding the crashing instruction
//   lr-image   path of the image lr points into
//   pc         expected pc line when no image holds it
//   signal     expected signal name
//   fault      expected fault line
//   tid        "differs": the report's tid must not be the pid
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int __pthread_kill(unsigned int, int);

static void expect(const char* key, const char* value)
{
	printf("EXPECT %s %s\n", key, value);
	fflush(stdout);
}

static void expect_image_of(const char* key, const void* addr)
{
	Dl_info info;
	if (dladdr(addr, &info) && info.dli_fname)
		expect(key, info.dli_fname);
	else
		expect(key, "dladdr-failed");
}

static void __attribute__((noinline)) null_store(void)
{
	expect_image_of("pc-image", (const void*)null_store);
	expect_image_of("lr-image", (const void*)null_store);
	expect("signal", "SIGSEGV");
	expect("fault", "null-relative, offset 8");
	*(volatile int*)8 = 1;
}

static void __attribute__((noinline)) do_abort(void)
{
	expect("signal", "SIGABRT");
	expect("fault", "n/a");
	expect_image_of("pc-image", (const void*)__pthread_kill);
	expect_image_of("lr-image", (const void*)__pthread_kill);
	abort();
}

static void __attribute__((noinline)) do_trap(void)
{
	expect_image_of("pc-image", (const void*)do_trap);
	expect_image_of("lr-image", (const void*)do_trap);
	expect("signal", "SIGTRAP");
	expect("fault", "n/a");
	__builtin_trap();
}

static void __attribute__((noinline)) wild_jump(void)
{
	expect("signal", "SIGSEGV");
	expect("pc", "not mapped");
	expect_image_of("lr-image", (const void*)wild_jump);
	expect("fault", "not mapped");
	void (*fn)(void) = (void (*)(void))(uintptr_t)0x300000000000ULL;
	fn();
}

static void __attribute__((noinline)) high_bits(void)
{
	expect_image_of("pc-image", (const void*)high_bits);
	expect_image_of("lr-image", (const void*)high_bits);
	expect("signal", "SIGSEGV");
	expect("fault", "high bits set (not a canonical address)");
	volatile int* p = (volatile int*)(uintptr_t)0xdead000000001000ULL;
	*p = 1;
}

static void __attribute__((noinline)) library_crash(void)
{
	expect("signal", "SIGSEGV");
	expect("fault", "null-relative, offset 0");
	expect_image_of("pc-image", (const void*)strlen);
	expect_image_of("lr-image", (const void*)library_crash);
	volatile const char* volatile p = NULL;
	volatile size_t n = strlen((const char*)p);
	(void)n;
}

// An executable anonymous page holding one 32-bit instruction word we wrote ourselves (brk #0 or the
// permanently undefined word 0): the pc is in no image, so the report must say so instead of naming the
// nearest image header below it.
static void __attribute__((noinline)) anon_exec(const char* signal_name, uint32_t word)
{
	expect("signal", signal_name);
	expect("fault", "n/a");
	expect("pc", "no image (anonymous region)");
	expect_image_of("lr-image", (const void*)anon_exec);
	void* page = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	memcpy(page, &word, sizeof(word));
	mprotect(page, 16384, PROT_READ | PROT_EXEC);
	__builtin___clear_cache((char*)page, (char*)page + sizeof(word));
	((void (*)(void))page)();
}

// A store to read-only constant data of this program: the address is mapped and the access is refused.
static const char read_only_data[64] = "constant data";

static void __attribute__((noinline)) store_to_read_only(void)
{
	expect("signal", "SIGSEGV");
	expect("fault", "mapped, access not permitted");
	expect_image_of("pc-image", (const void*)store_to_read_only);
	expect_image_of("lr-image", (const void*)store_to_read_only);
	*(volatile char*)read_only_data = 1;
}

static void* thread_body(void* arg)
{
	(void)arg;
	expect("tid", "differs");
	null_store();
	return NULL;
}

static void thread_crash(void)
{
	pthread_t t;
	pthread_create(&t, NULL, thread_body, NULL);
	pthread_join(t, NULL);
}

// Two threads fault at the same moment: exactly one complete report must come out, the loser waits for it.
static volatile int race_go = 0;
static volatile int race_ready = 0;

static void* race_body(void* arg)
{
	(void)arg;
	__sync_fetch_and_add(&race_ready, 1);
	while (!race_go)
		;
	*(volatile int*)8 = 1;
	return NULL;
}

static void race_crash(void)
{
	expect("signal", "SIGSEGV");
	expect("fault", "null-relative, offset 8");
	expect_image_of("pc-image", (const void*)race_body);
	expect_image_of("lr-image", (const void*)pthread_create);   // race_body is a leaf: lr is in the thread start code
	pthread_t a, b;
	pthread_create(&a, NULL, race_body, NULL);
	pthread_create(&b, NULL, race_body, NULL);
	while (race_ready < 2)
		;
	race_go = 1;
	pthread_join(a, NULL);
	pthread_join(b, NULL);
}

// A process killed by SIGTERM did not crash: no report.
static void term_self(void)
{
	kill(getpid(), SIGTERM);
	sleep(5);
}

static void own_handler(int sig)
{
	(void)sig;
	printf("handled\n");
	fflush(stdout);
	_exit(0);
}

static void handler_installed(void)
{
	signal(SIGSEGV, own_handler);
	*(volatile int*)8 = 1;
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s null|abort|trap|wild|highbits|library|anon|ill|rodata|thread|race|term|handler\n", argv[0]);
		return 2;
	}
	const char* c = argv[1];
	if (!strcmp(c, "null"))
		null_store();
	else if (!strcmp(c, "abort"))
		do_abort();
	else if (!strcmp(c, "trap"))
		do_trap();
	else if (!strcmp(c, "wild"))
		wild_jump();
	else if (!strcmp(c, "highbits"))
		high_bits();
	else if (!strcmp(c, "library"))
		library_crash();
	else if (!strcmp(c, "anon"))
		anon_exec("SIGTRAP", 0xd4200000);
	else if (!strcmp(c, "ill"))
		anon_exec("SIGILL", 0x00000000);
	else if (!strcmp(c, "rodata"))
		store_to_read_only();
	else if (!strcmp(c, "thread"))
		thread_crash();
	else if (!strcmp(c, "race"))
		race_crash();
	else if (!strcmp(c, "term"))
		term_self();
	else if (!strcmp(c, "handler"))
		handler_installed();
	else {
		fprintf(stderr, "unknown case %s\n", c);
		return 2;
	}
	return 0;
}
