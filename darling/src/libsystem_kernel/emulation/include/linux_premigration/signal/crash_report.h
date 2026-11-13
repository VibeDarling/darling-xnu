#ifndef _CRASH_REPORT_H
#define _CRASH_REPORT_H

#include <stdbool.h>
#include <darling/emulation/xnu_syscall/bsd/impl/signal/sigaction.h>

// Set once from the guest environment (DARLING_CRASH_REPORT=1) by crash_report_enable.
extern bool crash_report_on;

// Reads the environment given to __libkernel_init; when DARLING_CRASH_REPORT=1 it turns the report on and
// caches the address of dyld's all-image-infos table (task_info is not safe to call at crash time).
void crash_report_enable(const char* envp[]);

// Prints the report to fd 2. Called from sigexc_handler when the process is about to take the default
// action for a fatal signal.
void crash_report_print(int bsd_signum, const struct linux_siginfo* info, const struct linux_ucontext* ctxt);

#endif // _CRASH_REPORT_H
