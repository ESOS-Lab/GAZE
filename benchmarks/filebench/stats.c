/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdarg.h>
#include <limits.h>

#include "filebench.h"
#include "flowop.h"
#include "vars.h"
#include "stats.h"
#include "fbtime.h"

/*
 * Second at which the GC-window statistics (the gc_fs_* counters behind
 * the "GC IO Summary" / "GC <flowop>" output lines) start accumulating,
 * so the reported numbers cover only the under-GC steady state.
 *
 * Read from the FILEBENCH_GC_START_SEC environment variable (seconds),
 * which experiment_script feeds from experiment_params.sh so
 * set_config.sh can scale it to the machine.  Unset/invalid -> the
 * paper's setting (100 s: GC onset of the fileserver workload on the
 * paper's 256G emulated SSD).
 */
static int
gc_start_time_sec(void)
{
	static int cached = -1;

	if (cached < 0) {
		const char *s = getenv("FILEBENCH_GC_START_SEC");
		cached = (s && *s && atoi(s) > 0) ? atoi(s) : 100;
	}
	return cached;
}
#define GC_START_TIME (gc_start_time_sec())

/*
 * A set of routines for collecting and dumping various filebench
 * run statistics.
 */

/* Global statistics */
static struct flowstats *globalstats = NULL;

/*
 * Add a flowstat b to a, leave sum in a.
 */
static void
stats_add(struct flowstats *a, struct flowstats *b)
{
	int i;

	a->fs_count += b->fs_count;
	a->fs_rcount += b->fs_rcount;
	a->fs_wcount += b->fs_wcount;
	a->fs_bytes += b->fs_bytes;
	a->fs_rbytes += b->fs_rbytes;
	a->fs_wbytes += b->fs_wbytes;
	a->fs_total_lat += b->fs_total_lat;

	if (b->fs_maxlat > a->fs_maxlat)
		a->fs_maxlat = b->fs_maxlat;

	if (b->fs_minlat < a->fs_minlat)
		a->fs_minlat = b->fs_minlat;

	for (i = 0; i < OSPROF_BUCKET_NUMBER; i++)
		a->fs_distribution[i] += b->fs_distribution[i];

//#ifdef JW_GC_IOPS
	a->gc_fs_count += b->gc_fs_count;
	a->gc_fs_rcount += b->gc_fs_rcount;
	a->gc_fs_wcount += b->gc_fs_wcount;
	a->gc_fs_bytes += b->gc_fs_bytes;
	a->gc_fs_rbytes += b->gc_fs_rbytes;
	a->gc_fs_wbytes += b->gc_fs_wbytes;
	a->gc_fs_total_lat += b->gc_fs_total_lat;

	if (b->gc_fs_maxlat > a->gc_fs_maxlat)
		a->gc_fs_maxlat = b->gc_fs_maxlat;

	if (b->gc_fs_minlat < a->gc_fs_minlat)
		a->gc_fs_minlat = b->gc_fs_minlat;

	for (i = 0; i < OSPROF_BUCKET_NUMBER; i++)
		a->gc_fs_distribution[i] += b->gc_fs_distribution[i];
//#endif
}



#define JW_GC_IOPS

//#ifdef JW_GC_IOPS
int
gc_started(void)
{
	hrtime_t orig_starttime, cur_time;
	double total_time_sec;
	static int is_first = 1;
	static hrtime_t first_time = 0;
	if (is_first) {
		is_first = 0;
		first_time = gethrtime();
		return 0;
	}
	//if (!globalstats)
	//filebench_log(LOG_INFO, "%s", "let's try 1");
	//orig_starttime = globalstats->fs_stime;
	cur_time = gethrtime();
	total_time_sec = (cur_time - first_time) / SEC2NS_FLOAT;

	if (total_time_sec > GC_START_TIME) {
		//printf("%s: GC Start!!!!!!!!\n", __func__);
		//filebench_log(LOG_INFO, "%s", "gc started!!!!!!!!!!!!!!!!!!");
		return 1;
	} else {
		//filebench_log(LOG_INFO, "%s", "gc not started!!");
		return 0;
	}
}
//#endif

/*
 * Takes a "snapshot" of the global statistics. Actually, it calculates
 * them from the local statistics maintained by each flowop.
 * First the routine pauses filebench, then rolls the statistics for
 * each flowop into its associated FLOW_MASTER flowop.
 * Next all the FLOW_MASTER flowops' statistics are written
 * to the log file followed by the global totals. Then filebench
 * operation is allowed to resume.
 */
void
stats_snap(void)
{
	struct flowstats *iostat = &globalstats[FLOW_TYPE_IO];
	struct flowstats *aiostat = &globalstats[FLOW_TYPE_AIO];
	hrtime_t orig_starttime;
	flowop_t *flowop;
	char *str;
	double total_time_sec;
	uint64_t p95 = 0, p99 = 0, p99_9 = 0, p99_95 = 0, p99_99 = 0,
			p99_999 = 0, p99_9999 = 0, p99_99999 = 0, cnt = 0;
	
	if (!globalstats) {
		filebench_log(LOG_ERROR,
		    "'stats snap' called before 'stats clear'");
		return;
	}

	/* don't print out if run ended in error */
	if (filebench_shm->shm_f_abort == FILEBENCH_ABORT_ERROR) {
		filebench_log(LOG_ERROR,
		    "NO VALID RESULTS! Filebench run terminated prematurely");
		return;
	}

	/* Freeze statistics during update */
	filebench_shm->shm_bequiet = 1;

	/* We want to have blank global statistics each
	 * time we start the summation process, but the
	 * statistics collection start time must remain
	 * unchanged (it's a snapshot compared to the original
	 * start time). */
	orig_starttime = globalstats->fs_stime;
	(void) memset(globalstats, 0, FLOW_TYPES * sizeof(struct flowstats));
	globalstats->fs_stime = orig_starttime;
	globalstats->fs_etime = gethrtime();

	total_time_sec = (globalstats->fs_etime -
			globalstats->fs_stime) / SEC2NS_FLOAT;
#ifdef JW_GC_IOPS
	double gc_total_time_sec;
	gc_total_time_sec = total_time_sec - GC_START_TIME;
	int gc_run = (total_time_sec > GC_START_TIME)? 1: 0;
#endif
	filebench_log(LOG_DEBUG_SCRIPT, "Stats period = %.0f sec",
			total_time_sec);

	/* Similarly we blank the master flowop statistics */
	flowop = filebench_shm->shm_flowoplist;
	while (flowop) {
		if (flowop->fo_instance == FLOW_MASTER) {
			(void) memset(&flowop->fo_stats, 0, sizeof(struct flowstats));
			flowop->fo_stats.fs_minlat = ULLONG_MAX;
		}
		flowop = flowop->fo_next;
	}

	/* Roll up per-flowop statistics in globalstats and master flowops */
	flowop = filebench_shm->shm_flowoplist;
	while (flowop) {
		flowop_t *flowop_master;

		if (flowop->fo_instance <= FLOW_DEFINITION) {
			flowop = flowop->fo_next;
			continue;
		}

		/* Roll up per-flowop into global stats */
		stats_add(&globalstats[flowop->fo_type], &flowop->fo_stats);
		stats_add(&globalstats[FLOW_TYPE_GLOBAL], &flowop->fo_stats);

		flowop_master = flowop_find_one(flowop->fo_name, FLOW_MASTER);
		if (flowop_master) {
			/* Roll up per-flowop stats into master */
			stats_add(&flowop_master->fo_stats, &flowop->fo_stats);
		} else {
			filebench_log(LOG_DEBUG_NEVER,
			    "flowop_stats could not find %s",
			    flowop->fo_name);
		}

		filebench_log(LOG_DEBUG_SCRIPT,
		    "flowop %-20s-%4d  - %5d ops %5.1lf ops/sec %5.1lfmb/s "
		    "%8.3fms/op",
		    flowop->fo_name,
		    flowop->fo_instance,
		    flowop->fo_stats.fs_count,
		    flowop->fo_stats.fs_count / total_time_sec,
		    (flowop->fo_stats.fs_bytes / MB_FLOAT) / total_time_sec,
		    flowop->fo_stats.fs_count ?
		    flowop->fo_stats.fs_total_lat /
		    (flowop->fo_stats.fs_count * SEC2MS_FLOAT) : 0);

		flowop = flowop->fo_next;

	}

	flowop = filebench_shm->shm_flowoplist;
	str = malloc(1048576);
	*str = '\0';
	(void) strcpy(str, "Per-Operation Breakdown\n");
	while (flowop) {
		char line[1024];
		char histogram[1024];
		char hist_reading[20];
		int i = 0;

		if (flowop->fo_instance != FLOW_MASTER) {
			flowop = flowop->fo_next;
			continue;
		}

		(void) snprintf(line, sizeof(line), "%-20s %dops %8.0lfops/s "
		    "%5.1lfmb/s %8.3fms/op",
		    flowop->fo_name,
		    flowop->fo_stats.fs_count,
		    flowop->fo_stats.fs_count / total_time_sec,
		    (flowop->fo_stats.fs_bytes / MB_FLOAT) / total_time_sec,
		    flowop->fo_stats.fs_count ?
		    flowop->fo_stats.fs_total_lat /
		    (flowop->fo_stats.fs_count * SEC2MS_FLOAT) : 0);
		(void) strcat(str, line);

		(void) snprintf(line, sizeof(line)," [%.3fms - %5.3fms]",
			flowop->fo_stats.fs_minlat / SEC2MS_FLOAT,
			flowop->fo_stats.fs_maxlat / SEC2MS_FLOAT);
		(void) strcat(str, line);

		if (filebench_shm->lathist_enabled) {
			cnt = 0;
			p95 = 0; 
			p99 = 0; 
			p99_9 = 0; 
			p99_95 = 0; 
			p99_99 = 0;
			p99_999 = 0; 
			p99_9999 = 0; 
			p99_99999 = 0;
			for (i = 0; i < OSPROF_BUCKET_NUMBER; i++) {
				cnt += flowop->fo_stats.fs_distribution[i];
				if(!p95 && (cnt * 100 > flowop->fo_stats.fs_count * 95))
					p95 = i*100;
				
				if(!p99 && (cnt * 100 > flowop->fo_stats.fs_count * 99))
					p99 = i*100;
				
				if(!p99_9 && (cnt * 1000 > flowop->fo_stats.fs_count * 999))
					p99_9 = i*100;
				
				if(!p99_95 && (cnt * 10000 > flowop->fo_stats.fs_count * 9995))
					p99_95 = i*100;
				
				if(!p99_99 && (cnt * 10000 > flowop->fo_stats.fs_count * 9999))
					p99_99 = i*100;

				if(!p99_999 && (cnt * 100000 > flowop->fo_stats.fs_count * 99999))
					p99_999 = i*100;

				if(!p99_9999 && (cnt * 1000000 > flowop->fo_stats.fs_count * 999999))
					p99_9999 = i*100;

				if(cnt * 10000000 > flowop->fo_stats.fs_count * 9999999) {
					p99_99999 = i*100;
					break;
				}
				
				//if (cnt * 10000 > flowop->fo_stats.fs_count * 9999){
				//	p99_99 = i;
				//	break;
				//}
			}
			(void) sprintf(histogram, "[p95: %lu , p99: %lu , p99.9: %lu , p99.95: %lu , p99.99: %lu , p99.999: %lu , p99.9999: %lu , p99.99999: %lu (us)]\n",
			p95, p99, p99_9, p99_95, p99_99, p99_999, p99_9999, p99_99999);
			(void) strcat(str, histogram);

		} else
			(void) strcat(str, "\n");

#ifdef JW_GC_IOPS
		if (gc_run) {
			(void) snprintf(line, sizeof(line), "GC %-20s %dops %8.0lfops/s "
			    "%5.1lfmb/s %8.3fms/op",
			    flowop->fo_name,
			    flowop->fo_stats.gc_fs_count,
			    flowop->fo_stats.gc_fs_count / gc_total_time_sec,
			    (flowop->fo_stats.gc_fs_bytes / MB_FLOAT) / gc_total_time_sec,
			    flowop->fo_stats.gc_fs_count ?
			    flowop->fo_stats.gc_fs_total_lat /
			    (flowop->fo_stats.gc_fs_count * SEC2MS_FLOAT) : 0);
			(void) strcat(str, line);

			(void) snprintf(line, sizeof(line)," [%.3fms - %5.3fms]",
				flowop->fo_stats.gc_fs_minlat / SEC2MS_FLOAT,
				flowop->fo_stats.gc_fs_maxlat / SEC2MS_FLOAT);
			(void) strcat(str, line);

			if (filebench_shm->lathist_enabled) {
				cnt = 0;
				p95 = 0; 
				p99 = 0; 
				p99_9 = 0; 
				p99_95 = 0; 
				p99_99 = 0;
				p99_999 = 0; 
				p99_9999 = 0; 
				p99_99999 = 0;
				for (i = 0; i < OSPROF_BUCKET_NUMBER; i++) {
					cnt += flowop->fo_stats.gc_fs_distribution[i];
					if(!p95 && (cnt * 100 > flowop->fo_stats.gc_fs_count * 95))
						p95 = i*100;
					
					if(!p99 && (cnt * 100 > flowop->fo_stats.gc_fs_count * 99))
						p99 = i*100;
					
					if(!p99_9 && (cnt * 1000 > flowop->fo_stats.gc_fs_count * 999))
						p99_9 = i*100;
					
					if(!p99_95 && (cnt * 10000 > flowop->fo_stats.gc_fs_count * 9995))
						p99_95 = i*100;
					
					if(!p99_99 && (cnt * 10000 > flowop->fo_stats.gc_fs_count * 9999))
						p99_99 = i*100;

					if(!p99_999 && (cnt * 100000 > flowop->fo_stats.gc_fs_count * 99999))
						p99_999 = i*100;

					if(!p99_9999 && (cnt * 1000000 > flowop->fo_stats.gc_fs_count * 999999))
						p99_9999 = i*100;

					if(cnt * 10000000 > flowop->fo_stats.gc_fs_count * 9999999) {
						p99_99999 = i*100;
						break;
					}
				}
				(void) sprintf(histogram, "[p95: %lu , p99: %lu , p99.9: %lu , p99.95: %lu , p99.99: %lu , p99.999: %lu , p99.9999: %lu , p99.99999: %lu (us)]\n",
				p95, p99, p99_9, p99_95, p99_99, p99_999, p99_9999, p99_99999);
				(void) strcat(str, histogram);

			} else
				(void) strcat(str, "\n");
		}
#endif

		flowop = flowop->fo_next;
	}

	/* removing last \n  */
	str[strlen(str) - 1] = '\0';

	filebench_log(LOG_INFO, "%s", str);
	free(str);

		
	/* log all latencies per operation */
	/*
	flowop = filebench_shm->shm_flowoplist;
	filebench_log(LOG_INFO, "[JW] Latencies!!");
	while (flowop) {
		unsigned long long *lat_log;
		lat_log = flowop->fo_stats.lat_log;
		for (int i = 0; i < flowop->fo_stats.lat_idx; i ++){
			filebench_log(LOG_INFO, 
				"%s %d: %llu", 
				&flowop->fo_name, i, lat_log[i]);
		}
		flowop->fo_stats.lat_idx= 0;
		flowop = flowop->fo_next;
	}
	*/

	filebench_log(LOG_INFO,
	    "IO Summary: %5d ops %5.3lf ops/s %0.0lf/%0.0lf rd/wr "
	    "%5.1lfmb/s %5.3fms/op",
	    iostat->fs_count + aiostat->fs_count,
	    (iostat->fs_count + aiostat->fs_count) / total_time_sec,
	    (iostat->fs_rcount + aiostat->fs_rcount) / total_time_sec,
	    (iostat->fs_wcount + aiostat->fs_wcount) / total_time_sec,
	    ((iostat->fs_bytes + aiostat->fs_bytes) / MB_FLOAT)
						/ total_time_sec,
	    (iostat->fs_count + aiostat->fs_count) ?
	    (iostat->fs_total_lat + aiostat->fs_total_lat) /
	    ((iostat->fs_count + aiostat->fs_count) * SEC2MS_FLOAT) : 0);

#ifdef JW_GC_IOPS
	if (gc_run) {	
		filebench_log(LOG_INFO,
		    "GC IO Summary: %5d ops %5.3lf ops/s %0.0lf/%0.0lf rd/wr "
		    "%5.1lfmb/s %5.3fms/op",
		    iostat->gc_fs_count + aiostat->gc_fs_count,
		    (iostat->gc_fs_count + aiostat->gc_fs_count) / gc_total_time_sec,
		    (iostat->gc_fs_rcount + aiostat->gc_fs_rcount) / gc_total_time_sec,
		    (iostat->gc_fs_wcount + aiostat->gc_fs_wcount) / gc_total_time_sec,
		    ((iostat->gc_fs_bytes + aiostat->gc_fs_bytes) / MB_FLOAT)
							/ gc_total_time_sec,
		    (iostat->gc_fs_count + aiostat->gc_fs_count) ?
		    (iostat->gc_fs_total_lat + aiostat->gc_fs_total_lat) /
		    ((iostat->gc_fs_count + aiostat->gc_fs_count) * SEC2MS_FLOAT) : 0);
	}
#endif

	filebench_shm->shm_bequiet = 0;
}

/*
 * Clears all the statistics variables (fo_stats) for every defined flowop.
 * It also creates a global flowstat table if one doesn't already exist and
 * clears it.
 */
void
stats_clear(void)
{
	flowop_t *flowop;
	static int is_first = 0;

	if (globalstats == NULL)
		globalstats = malloc(FLOW_TYPES * sizeof (struct flowstats));

	(void) memset(globalstats, 0, FLOW_TYPES * sizeof (struct flowstats));

	flowop = filebench_shm->shm_flowoplist;

	while (flowop) {
		filebench_log(LOG_DEBUG_IMPL, "Clearing stats for %s-%d",
		    flowop->fo_name,
		    flowop->fo_instance);
		(void) memset(&flowop->fo_stats, 0, sizeof (struct flowstats));
		flowop = flowop->fo_next;
	}

	(void) memset(globalstats, 0, sizeof(struct flowstats));
	globalstats->fs_stime = gethrtime();
}
