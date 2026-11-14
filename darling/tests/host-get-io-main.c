// host_get_io_main is the current name of host_get_io_master (same request, same reply). Current macOS libraries import
// it from libsystem_kernel; without the export Darling's libsystem_kernel is bypassed by the cache's own copy, which
// runs against Darling's kernel. Darling's kernel half has no IO master port of its own (it answers success with
// MACH_PORT_NULL), so libsystem_kernel answers with iokitd's port, the one Darling's IOKit looks up by name through
// the bootstrap server. This test pins that: both names return the same valid port, IOMasterPort and IOMainPort agree
// with it, and when iokitd cannot be reached the routines fail loudly (never success with a null port).
// Symbols are looked up at run time so the same binary reports a missing export instead of failing to load.
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>

#define IO_KIT "/System/Library/Frameworks/IOKit.framework/IOKit"

static void expect(int condition, const char* what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		exit(1);
	}
}

int main(void)
{
	kern_return_t (*get_io_main)(host_t, mach_port_t*) = dlsym(RTLD_DEFAULT, "host_get_io_main");
	expect(get_io_main != NULL, "libsystem_kernel exports host_get_io_main");

	mach_port_t main_port = 0x7777, master_port = 0x7777;
	kern_return_t main_kr = get_io_main(mach_host_self(), &main_port);
	kern_return_t master_kr = host_get_io_master(mach_host_self(), &master_port);
	expect(main_kr == KERN_SUCCESS && MACH_PORT_VALID(main_port), "host_get_io_main returns a port");
	expect(master_kr == KERN_SUCCESS && MACH_PORT_VALID(master_port), "host_get_io_master returns a port");
	expect(main_port == master_port, "host_get_io_main and host_get_io_master return the same port");

	mach_port_t unused = 0x7777;
	kern_return_t null_main = get_io_main(MACH_PORT_NULL, &unused);
	kern_return_t null_master = host_get_io_master(MACH_PORT_NULL, &unused);
	expect(null_main == null_master && null_main != KERN_SUCCESS, "a null host is refused the same way");

	void* iokit = dlopen(IO_KIT, RTLD_NOW);
	expect(iokit != NULL, "IOKit loads");
	kern_return_t (*io_master_port)(mach_port_t, mach_port_t*) = dlsym(iokit, "IOMasterPort");
	kern_return_t (*io_main_port)(mach_port_t, mach_port_t*) = dlsym(iokit, "IOMainPort");
	expect(io_master_port != NULL && io_main_port != NULL, "IOKit exports IOMasterPort and IOMainPort");
	mach_port_t via_master = MACH_PORT_NULL, via_main = MACH_PORT_NULL;
	expect(io_master_port(MACH_PORT_NULL, &via_master) == KERN_SUCCESS && MACH_PORT_VALID(via_master), "IOMasterPort returns a port");
	expect(io_main_port(MACH_PORT_NULL, &via_main) == KERN_SUCCESS && MACH_PORT_VALID(via_main), "IOMainPort returns a port");
	expect(via_master == via_main, "IOMasterPort and IOMainPort return the same port");
	expect(via_master == main_port, "the host's IO main port is the port IOMasterPort hands out (iokitd)");

	// iokitd unreachable: no bootstrap port, then a bootstrap port whose receiver is gone.
	mach_port_t real_bootstrap = bootstrap_port;
	mach_port_t port = 0x7777;
	bootstrap_port = MACH_PORT_NULL;
	expect(get_io_main(mach_host_self(), &port) != KERN_SUCCESS && port == MACH_PORT_NULL, "no bootstrap port: an error and a null port");

	mach_port_t dead = MACH_PORT_NULL;
	expect(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &dead) == KERN_SUCCESS, "allocate a port");
	expect(mach_port_insert_right(mach_task_self(), dead, dead, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS, "make a send right");
	expect(mach_port_mod_refs(mach_task_self(), dead, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS, "drop the receive right");
	bootstrap_port = dead;
	port = 0x7777;
	expect(get_io_main(mach_host_self(), &port) != KERN_SUCCESS && port == MACH_PORT_NULL, "dead bootstrap port: an error and a null port");
	port = 0x7777;
	expect(host_get_io_master(mach_host_self(), &port) != KERN_SUCCESS && port == MACH_PORT_NULL, "host_get_io_master fails the same way");
	bootstrap_port = real_bootstrap;

	printf("PASS: host_get_io_main and host_get_io_master return iokitd's port %u, IOMasterPort and IOMainPort agree, unreachable iokitd is an error\n", main_port);
	return 0;
}
