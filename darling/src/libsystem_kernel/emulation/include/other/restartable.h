#ifndef EMULATION_OTHER_RESTARTABLE_H
#define EMULATION_OTHER_RESTARTABLE_H

#include <stdint.h>

#include <darling/emulation/common/base.h>

CPP_EXTERN_BEGIN

/* Called from the SIGNAL_RESTARTABLE_SYNC handler with the interrupted PC of the thread it runs
 * on. Acknowledges to the thread inside task_restartable_ranges_synchronize and returns the PC the
 * thread must resume at: the range's recovery address when the PC is inside a range, else the PC. */
uint64_t darling_restartable_sync_interrupted(uint64_t pc);

CPP_EXTERN_END

#endif
