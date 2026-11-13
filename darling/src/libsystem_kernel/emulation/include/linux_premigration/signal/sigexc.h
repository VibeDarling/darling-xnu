#ifndef _SIGEXC_H
#define _SIGEXC_H

#include <stdbool.h>

#include "rtsig.h"
#include <darling/emulation/xnu_syscall/bsd/impl/signal/sigaction.h>

// NOTE: Keep these definitions up to date with lkm/darling/binfmt.c!
// Uses one of the below magic values to toggle the debugging state
#define SIGNAL_SIGEXC_SUSPEND	LINUX_SIGRTMIN
#define SIGNAL_S2C (LINUX_SIGRTMIN + 1)
// task_restartable_ranges_synchronize: the handler makes no server call and only edits the
// interrupted PC (see restartable.c), so unlike the two above it needs no interrupt_enter.
#define SIGNAL_RESTARTABLE_SYNC (LINUX_SIGRTMIN + 2)

void sigexc_setup(void);

// for PT_SIGEXC to handle this operation synchronously
void darling_sigexc_self(void);
void sigexc_handler(int linux_signum, struct linux_siginfo* info, struct linux_ucontext* ctxt);

struct linux_ucontext;
// xnu forces the restartable-ranges check on every sigreturn: call it with the frame a handler returns to.
void restart_if_in_restartable_range(struct linux_ucontext* ctxt);

void sigexc_thread_setup(void);
void sigexc_thread_exit(void);

#endif // _SIGEXC_H
