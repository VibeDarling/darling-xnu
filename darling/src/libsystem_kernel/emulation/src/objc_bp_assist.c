/* xnu's objc breakpoint helper installs a per-process breakpoint assist; Darling has no kernel to
 * assist, so the call is recorded once and ignored. Nothing in objc4 calls it, so the arity follows
 * the kernel entry (adr, ctl) and the return value is success. */

#include <stdint.h>

#include <darling/emulation/common/simple.h>

__attribute__ ((visibility ("default")))
int
objc_bp_assist_cfg_np(uint64_t adr, uint64_t ctl)
{
	static int logged;

	if (!logged) {
		logged = 1;
		__simple_printf("objc_bp_assist_cfg_np(%llx, %llx): no breakpoint assist, call ignored\n",
				(unsigned long long)adr, (unsigned long long)ctl);
	}
	return 0;
}
