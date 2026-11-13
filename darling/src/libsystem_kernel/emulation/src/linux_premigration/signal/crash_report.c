#include <darling/emulation/linux_premigration/signal/crash_report.h>

#include <stddef.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <mach-o/dyld_images.h>

#include <darling/emulation/common/base.h>
#include <darling/emulation/common/simple.h>
#include <darling/emulation/linux_premigration/linux-syscalls/linux.h>

// Crash report for guest processes. It runs in the fatal-signal path of sigexc_handler and prints, to fd 2:
// the signal, why it was raised, what kind of address faulted, and the image that holds pc and lr.
//
// Clean-room limits, deliberate and checked by tests/crash-report: the report names images and nothing
// else about them (no address, offset or symbol), never reads the instruction at pc, and walks no stack.
// An address is matched to an image only through the memory map (/proc/self/maps) and the public
// dyld_all_image_infos table (<mach-o/dyld_images.h>): the map bounds the mapping that holds the address,
// and the image is the one whose header address is the greatest at or below it inside that mapping. Nothing
// inside an image is read.

extern int getpid(void);

bool crash_report_on = false;
// 0 untouched, 1 a crashing thread is writing the report, 2 it is complete
static volatile int g_state = 0;

// all_image_info_addr from task_info(TASK_DYLD_INFO), read once at init: at crash time darlingserver
// blocks in that call until dyld's info has been registered.
static unsigned long long g_all_image_infos = 0;

#define PATH_MAX_SHOWN 300
#define VA_MASK 0x0000ffffffffffffULL
#define NULL_RELATIVE_LIMIT 0x10000ULL
#define REPORT_WAIT_MS 2000

struct region {
	unsigned long long lo, hi;
	char perms[5];
	char file[PATH_MAX_SHOWN];
};

static bool str_eq(const char* a, const char* b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void str_copy(char* dst, const char* src, int max)
{
	int i = 0;
	while (src[i] && i < max - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static unsigned long long parse_hex(const char** p)
{
	unsigned long long v = 0;
	int d;
	while ((d = hex_digit(**p)) >= 0) {
		v = (v << 4) | (unsigned long long)d;
		(*p)++;
	}
	return v;
}

// "lo-hi perms offset dev inode   path"
static void parse_map_line(const char* line, struct region* r)
{
	const char* p = line;
	r->lo = parse_hex(&p);
	if (*p == '-')
		p++;
	r->hi = parse_hex(&p);
	while (*p == ' ')
		p++;
	for (int i = 0; i < 4 && *p && *p != ' '; i++, p++)
		r->perms[i] = *p;
	r->perms[4] = '\0';
	for (int field = 0; field < 4; field++) {
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}
	str_copy(r->file, p, sizeof(r->file));
}

// 1 found, 0 not mapped, -1 the map could not be read.
static int find_region(unsigned long long addr, struct region* out)
{
	long fd = LINUX_SYSCALL(__NR_openat, -100 /* AT_FDCWD */, "/proc/self/maps", 0 /* O_RDONLY */, 0);
	if (fd < 0)
		return -1;

	char chunk[1024];
	char line[PATH_MAX_SHOWN + 96];
	int len = 0;
	int result = 0;
	bool done = false;

	while (!done) {
		long n = LINUX_SYSCALL(__NR_read, fd, chunk, sizeof(chunk));
		if (n < 0) {
			result = -1;
			break;
		}
		if (n == 0)
			break;
		for (long i = 0; i < n && !done; i++) {
			if (chunk[i] != '\n') {
				if (len < (int)sizeof(line) - 1)
					line[len++] = chunk[i];
				continue;
			}
			line[len] = '\0';
			len = 0;
			struct region r;
			parse_map_line(line, &r);
			if (addr >= r.lo && addr < r.hi) {
				*out = r;
				result = 1;
				done = true;
			}
		}
	}

	LINUX_SYSCALL(__NR_close, fd);
	return result;
}

static bool region_covers(const struct region* r, unsigned long long addr, unsigned long long size)
{
	return r->perms[0] == 'r' && addr >= r->lo && addr + size >= addr && addr + size <= r->hi;
}

// The path dyld lists for the image whose header is the greatest at or below addr inside region r. False when
// the table cannot be trusted or no image header lies in the mapping.
static bool image_path_for(unsigned long long addr, const struct region* r, char* out, int max)
{
	struct region tr;
	if (g_all_image_infos == 0)
		return false;

	const struct dyld_all_image_infos* infos = (const struct dyld_all_image_infos*)g_all_image_infos;
	if (find_region(g_all_image_infos, &tr) != 1
		|| !region_covers(&tr, g_all_image_infos, offsetof(struct dyld_all_image_infos, infoArray) + sizeof(void*)))
		return false;

	unsigned long long count = infos->infoArrayCount;
	unsigned long long array = (unsigned long long)infos->infoArray;
	if (array == 0 || count == 0 || count > 16384)
		return false;
	if (find_region(array, &tr) != 1 || !region_covers(&tr, array, count * sizeof(struct dyld_image_info)))
		return false;

	// the array validated above, not a second read of infos->infoArray: dyld may swap it in between
	const struct dyld_image_info* images = (const struct dyld_image_info*)array;
	unsigned long long best = 0;
	const char* best_path = NULL;
	for (unsigned long long i = 0; i < count; i++) {
		unsigned long long hdr = (unsigned long long)images[i].imageLoadAddress;
		if (hdr >= r->lo && hdr <= addr && hdr >= best) {
			best = hdr;
			best_path = images[i].imageFilePath;
		}
	}
	if (best_path == NULL)
		return false;

	unsigned long long p = (unsigned long long)best_path;
	if (find_region(p, &tr) != 1 || tr.perms[0] != 'r')
		return false;
	int i = 0;
	while (i < max - 1 && p + (unsigned long long)i < tr.hi && best_path[i]) {
		out[i] = best_path[i];
		i++;
	}
	out[i] = '\0';
	return i > 0;
}

// One line of text naming what holds addr: an image path, or why there is none.
static void describe_address(unsigned long long addr, char* out, int max)
{
	struct region r;
	int found = find_region(addr, &r);
	if (found < 0) {
		str_copy(out, "unknown (memory map unavailable)", max);
	} else if (found == 0) {
		str_copy(out, "not mapped", max);
	} else if (!image_path_for(addr, &r, out, max)) {
		str_copy(out, r.file[0] ? "no image (file-backed region)" : "no image (anonymous region)", max);
	}
}

static const char* signal_name(int bsd_signum)
{
	switch (bsd_signum) {
		case 1: return "SIGHUP";
		case 2: return "SIGINT";
		case 3: return "SIGQUIT";
		case 4: return "SIGILL";
		case 5: return "SIGTRAP";
		case 6: return "SIGABRT";
		case 7: return "SIGEMT";
		case 8: return "SIGFPE";
		case 9: return "SIGKILL";
		case 10: return "SIGBUS";
		case 11: return "SIGSEGV";
		case 12: return "SIGSYS";
		case 13: return "SIGPIPE";
		case 14: return "SIGALRM";
		case 15: return "SIGTERM";
		default: return "signal";
	}
}

// Linux si_code values: this runs on the raw Linux siginfo.
static const char* code_name(int bsd_signum, int code)
{
	switch (code) {
		case 0: return "SI_USER";
		case -1: return "SI_QUEUE";
		case -6: return "SI_TKILL";
		case 0x80: return "SI_KERNEL";
	}
	switch (bsd_signum) {
		case 11:
			if (code == 1) return "SEGV_MAPERR";
			if (code == 2) return "SEGV_ACCERR";
			break;
		case 10:
			if (code == 1) return "BUS_ADRALN";
			if (code == 2) return "BUS_ADRERR";
			if (code == 3) return "BUS_OBJERR";
			break;
		case 5:
			if (code == 1) return "TRAP_BRKPT";
			if (code == 2) return "TRAP_TRACE";
			break;
		case 4:
			if (code == 1) return "ILL_ILLOPC";
			if (code == 2) return "ILL_ILLOPN";
			if (code == 4) return "ILL_ILLTRP";
			break;
		case 8:
			if (code == 1) return "FPE_INTDIV";
			if (code == 2) return "FPE_INTOVF";
			break;
	}
	return "";
}

// What kind of address the fault was on. A small offset from NULL is the one number printed: it names a
// field of a missing object and is not an address inside any image. For an address that is mapped, only
// whether the access was refused is said: no image name, permission string or location.
static void describe_fault(int bsd_signum, const struct linux_siginfo* info, char* out, int max)
{
	// Linux fault codes are 1 to 4; for other codes si_addr aliases si_pid and means nothing
	if ((bsd_signum != 11 && bsd_signum != 10) || info->si_code < 1 || info->si_code > 4) {
		str_copy(out, "n/a", max);
		return;
	}

	unsigned long long addr = (unsigned long long)info->si_addr;
	if (addr >> 48) {
		str_copy(out, "high bits set (not a canonical address)", max);
	} else if (addr < NULL_RELATIVE_LIMIT) {
		__simple_snprintf(out, max, "null-relative, offset %d", (int)addr);
	} else {
		struct region r;
		int found = find_region(addr, &r);
		if (found < 0)
			str_copy(out, "unknown (memory map unavailable)", max);
		else if (found == 0)
			str_copy(out, "not mapped", max);
		else
			str_copy(out, info->si_code == 2 ? "mapped, access not permitted" : "mapped", max);
	}
}

void crash_report_enable(const char* envp[])
{
	for (int i = 0; envp && envp[i]; i++) {
		if (str_eq(envp[i], "DARLING_CRASH_REPORT=1")) {
			task_dyld_info_data_t di;
			mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
			if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&di, &count) == KERN_SUCCESS)
				g_all_image_infos = di.all_image_info_addr;
			crash_report_on = true;
			return;
		}
	}
}

static long long monotonic_ms(void)
{
	struct { long long sec; long long nsec; } ts = { 0, 0 };
	LINUX_SYSCALL(__NR_clock_gettime, 1 /* CLOCK_MONOTONIC */, &ts);
	return ts.sec * 1000 + ts.nsec / 1000000;
}

// The signals a crash raises. A process killed by SIGTERM or SIGINT did not crash.
static bool is_crash_signal(int bsd_signum)
{
	return bsd_signum == 4 || bsd_signum == 5 || bsd_signum == 6 || bsd_signum == 7 || bsd_signum == 8
		|| bsd_signum == 10 || bsd_signum == 11 || bsd_signum == 12;
}

void crash_report_print(int bsd_signum, const struct linux_siginfo* info, const struct linux_ucontext* ctxt)
{
	if (!info || !ctxt || !is_crash_signal(bsd_signum))
		return;
	// First crash of the process only. A thread that loses the race waits for the report to finish: the caller
	// goes on to kill the process, which would cut the winner's lines short. The wait is bounded by time, not by
	// a spin count, so a slow reader of /proc cannot be outrun.
	if (!__sync_bool_compare_and_swap(&g_state, 0, 1)) {
		long long deadline = monotonic_ms() + REPORT_WAIT_MS;
		while (g_state != 2 && monotonic_ms() < deadline)
			LINUX_SYSCALL(__NR_sched_yield);
		return;
	}

	char text[PATH_MAX_SHOWN + 64];
	const char* name = code_name(bsd_signum, info->si_code);

	__simple_fprintf(2, "=== darling crash report v1 ===\n");
	__simple_fprintf(2, "process: pid %d tid %d\n", getpid(), (int)LINUX_SYSCALL(__NR_gettid));
	__simple_fprintf(2, "signal: %s (%d) si_code %d%s%s%s\n", signal_name(bsd_signum), bsd_signum, info->si_code,
		name[0] ? " (" : "", name, name[0] ? ")" : "");

	describe_fault(bsd_signum, info, text, sizeof(text));
	__simple_fprintf(2, "fault: %s\n", text);

#if defined(__aarch64__)
	describe_address(ctxt->uc_mcontext.gregs.pc & VA_MASK, text, sizeof(text));
	__simple_fprintf(2, "pc: %s\n", text);
	unsigned long long lr = ctxt->uc_mcontext.gregs.regs[30] & VA_MASK;
	if (lr == 0) {
		__simple_fprintf(2, "lr: none\n");
	} else {
		describe_address(lr, text, sizeof(text));
		__simple_fprintf(2, "lr: %s\n", text);
	}
#else
	__simple_fprintf(2, "pc: not supported on this architecture\n");
#endif
	__simple_fprintf(2, "=== end darling crash report ===\n");
	__sync_synchronize();
	g_state = 2;
}
