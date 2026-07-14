#ifndef FIO_JW_GC_H
#define FIO_JW_GC_H

#include <stdlib.h>

/*
 * GC-window measurement start (GAZE paper experiments).
 *
 * This fio is patched so that NO latency/iops/bw samples are recorded
 * until this much time has passed since the job epoch (io_u.c gates the
 * add_*_sample calls on td->jw_gcstart, which backend.c sets once
 * jw_gc_start_time elapses).  The reported numbers therefore cover only
 * the under-GC steady state.
 *
 * The value comes from the FIO_GC_START_SEC environment variable
 * (seconds), which experiment_script feeds from experiment_params.sh so
 * set_config.sh can scale it to the machine.  Unset/invalid -> the
 * paper's setting (500 s: GC onset of the 4K randwrite job on the
 * paper's 256G emulated SSD).
 */
#define JW_GC_START_USEC_DEFAULT 500000000ULL

static inline unsigned long long jw_gc_start_time_usec(void)
{
	const char *s = getenv("FIO_GC_START_SEC");
	char *end;
	unsigned long long sec;

	if (!s || !*s)
		return JW_GC_START_USEC_DEFAULT;
	sec = strtoull(s, &end, 10);
	if (end == s || *end != '\0')
		return JW_GC_START_USEC_DEFAULT;
	return sec * 1000000ULL;
}

#endif
