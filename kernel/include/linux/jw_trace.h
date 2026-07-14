/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_JW_TRACE_H
#define _LINUX_JW_TRACE_H

//#define JW_COPY_COMMAND

struct lat_profiler
{
	unsigned long long filemap_get_folio_lat;
		unsigned long long filemap_get_entry_lat;
			unsigned long long __xas_load_lat;
			unsigned long long __trial_cnt;
			unsigned long long __repeat_cnt;
		unsigned long long lock_folio_lat;
		unsigned long long mark_folio_flag_lat;
		unsigned long long wait_folio_stable_lat;
		/* no_page section */
		unsigned long long prepare_alloc_folio_lat;
		unsigned long long alloc_folio_lat;
			unsigned long long __folio_alloc_lat;
				unsigned long long ____alloc_pages_lat;
					unsigned long long ______prepare_alloc_pages_lat;
					unsigned long long ______get_page_from_freelist_lat;
						unsigned long long ________cpuset_zone_allowed_lat;
						unsigned long long ________consider_node_dirty_pages_lat;
						unsigned long long ________watermark_fast_lat; //
						unsigned long long ________watermark_check_lat; //
						unsigned long long ________node_reclaim_lat; //
						unsigned long long ________watermark_ok_lat; //
						unsigned long long ________rmqueue_lat;
						unsigned long long ________prep_new_page_lat; //
						unsigned long long __________post_alloc_hook_lat; //
						unsigned long long __________prep_compound_page_lat; //
						unsigned long long ________check_highatomic_lat; //
						unsigned long long c1_cnt; //
						unsigned long long c2_cnt; //
						unsigned long long c3_cnt; //
						unsigned long long c4_cnt; //
						unsigned long long c5_cnt; //
						unsigned long long c6_cnt; //
						unsigned long long c7_cnt; //
						unsigned long long c8_cnt; //
						unsigned long long c9_cnt; //
						unsigned long long c10_cnt; //
							unsigned long long __________rmqueue_pcplist_lat;
								unsigned long long rmqueue_pcplist_bulk_lat;
								unsigned long long rmqueue_bulk_smallest_get_buddy_lat;
								unsigned long long rmqueue_bulk_smallest_update_buddy_lat;
								unsigned long long rmqueue_pcplist_cnt;
								unsigned long long rmqueue_pcplist_list_empty_cnt;
								unsigned long long rmqueue_bulk_batch;
								unsigned long long rmqueue_pcplist_loop_cnt;
							unsigned long long __________rmqueue_buddy_lat;
							unsigned long long __________rmqueue_out_lat; //
					unsigned long long ______alloc_pages_slowpath_lat;
					unsigned long long ______post_process_lat;
		unsigned long long add_folio_lat;
		unsigned long long put_folio_lat;
//		unsigned long long alloc_and_add_folio_lat;
	unsigned long long folio_file_page_lat;
};

//#define GC_IO_PROFILE_TIMESERIAL
//#define GC_PROFILE_TIMESERIAL

/* Emit the per-GC profiler prints (print_cur_gc_profiler) only for GCs whose
 * victim-section utilization equals this value. The paper's breakdown figures
 * are all taken at victim zone utilization 40% and every figure parser
 * (parse_lat.sh, parse_gc_breakdown.sh, the copy/pipe variants) filters on
 * "util: 40" anyway; skipping the other GCs keeps profile-run dmesg small
 * enough that the ring buffer never wraps. */
#define GC_PROFILE_UTIL_ONLY 40

#define REQ_GC_FLAG

/*On-going. Not yet verified optimization*/
//#define ASYNC_GC_READ_DISPATCH	/* let the kworker to dispatch GC read req. Getting Worse Currently!! */
//#define GC_READ_NO_SCHEDULER_MERGE /* disable bio merge with reqs on scheduler for gc read bio. Getting Worse Currently!!*/
//#define GC_WRITE_FUNC_PATH
//#define SINGLE_READ_PLUG


/* 2nd comment */
//#define GC_SCHEDULER

///* last comment*/
//#ifdef GC_IO_PROFILE_TIMESERIAL
#define DISABLE_GC_WRITE_THROTTLING // Not only for GC Write, but also for GC Read. 
//#undef DISABLE_GC_WRITE_SPLIT
//#endif


//#define APP_WRITE_PROFILE /* buffered write temporal profiling (dirty-page
// samples for the paper's memory-utilization figures; no longer part of the
// AE profile flow -- the 50ms samples were ~97% of profile-run dmesg) */

#ifdef GC_IO_PROFILE_TIMESERIAL
struct io_profiler
{
	/* called by jwgc_submit_bio() */
	/* called by submit_bio_noacct_nocheck_lat(). one of two following funcs are called. */
	/* __submit_bio_noacct_mq() repeats __submit_bio->blk_mq_submit_bio() */
	//unsigned long long submit_bio_noacct_nocheck_lat;
	/* in blk_mq_submit_bio() */
	unsigned long long  ____bio_split_to_limits_lat;
		unsigned long long  ______bio_split_rw_lat;
			unsigned long long  split_trial_cnt;
			unsigned long long  split_loop_cnt;
			unsigned long long  ________bio_split_rw_loop_fin_lat;
			unsigned long long  ________bio_split_rw_do_split_lat;
			unsigned long long  ________bio_split_rw_bio_split_lat;
				unsigned long long  __________bio_split_alloc_clone_lat;
					unsigned long long  ____________bio_split_alloc_bioset_lat;
						unsigned long long  ______________mempool_alloc_lat;
						unsigned long long  ______________mempool_alloc2_lat;
		unsigned long long  ______bio_split_submit_bio_lat;
	/* in blk_mq_get_cached_request() */
	unsigned long long  ____blk_get_new_requests_lat;
	/* in blk_get_new_requests() */
	unsigned long long  ______blk_mq_attempt_bio_merge_lat;
	unsigned long long  ______rq_qos_throttle_lat;
	unsigned long long  ______blk_mq_alloc_requests_lat;
	/* in blk_mq_alloc_requests() */
	unsigned long long  ________blk_mq_get_tag_lat;
	unsigned long long  __________io_schedule_lat;

	unsigned long long  ____blk_mq_sched_insert_request_lat;
};

struct read_io_profiler
{
	/* called by jwgc_submit_bio() */
	/* called by submit_bio_noacct_nocheck_lat(). one of two following funcs are called. */
	/* __submit_bio_noacct_mq() repeats __submit_bio->blk_mq_submit_bio() */
	unsigned long long blk_throtl_bio_lat;
	unsigned long long submit_bio_noacct_nocheck_lat;
	/* in blk_mq_submit_bio() */
	unsigned long long  ____bio_split_to_limits_lat;
	/* in blk_mq_get_cached_request() */
	unsigned long long ____blk_mq_get_cached_requests_lat;
	unsigned long long  ____blk_get_new_requests_no_rq_lat;
	unsigned long long  ____blk_get_new_requests_lat;
	/* in blk_get_new_requests() */
	unsigned long long  ______blk_mq_attempt_bio_merge_lat;
		unsigned long long  bio_merge_lat;
			unsigned long long  bio_merge_dd_lock_lat;
			unsigned long long  bik_mq_sched_try_merge_lat;
			unsigned long long  free_request_lat;
				unsigned long long  cache_merge;
				unsigned long long  cache_merge_lat;
				unsigned long long  hash_merge;
				unsigned long long  hash_merge_lat;
				unsigned long long  dd_merge;
				unsigned long long  dd_merge_lat;
	unsigned long long  ______rq_qos_throttle_lat;
	unsigned long long  ______blk_mq_alloc_requests_lat;
	/* in blk_mq_alloc_requests() */
	unsigned long long  ________blk_mq_get_tag_lat;
	unsigned long long  __________io_schedule_lat;
	unsigned long long  ____others_lat;
	unsigned long long  ____blk_add_rq_to_plug_lat;
		unsigned long long  ______plug_flush_lat;
			unsigned long long ________blk_mq_sched_insert_request_lat;
				unsigned long long __________dd_insert_request_lat;
				unsigned long long __________blk_mq_run_hw_queue_lat;
					unsigned long long __blk_mq_sched_dipspatch_requests1_lat;
						unsigned long long dd_dispatch_request_lat;
							unsigned long long dd_lock_lat;
							unsigned long long dispatch_prio_aged_request_lat;
							unsigned long long __dd_dispatch_request_lat;
								unsigned long long dd_next_read_request1_lat;
								unsigned long long dd_next_write_request1_lat;
								unsigned long long dd_fifo_write_request_lat;
									unsigned long long dd_fifo_wrt_zone_select_lock_lat;
									unsigned long long dd_fifo_wrt_loop_lat;
									unsigned long long dd_fifo_wrt_loop_cnt;
									unsigned long long dd_fifo_wrt_loop_trial_cnt;
								unsigned long long dd_next_read_request2_lat;
								unsigned long long dd_next_write_request2_lat;
								unsigned long long dd_fifo_read_request2_lat;
								unsigned long long dd_fifo_write_request2_lat;
								unsigned long long blk_req_zone_write_lock_lat;
						unsigned long long blk_mq_delay_run_hw_queues_lat;
						unsigned long long dispatch_multi_hctx_lat;
						unsigned long long blk_mq_dispatch_rq_list_lat;
						unsigned long long dispatch_rq_cnt;
						unsigned long long dispatch_trial_cnt;
					//unsigned long long __blk_mq_sched_dipspatch_requests2_lat;
					//unsigned long long __blk_mq_run_hw_queue_again_lat;
	//unsigned long long  ____blk_mq_sched_insert_request_lat;
	//unsigned long long  ____blk_mq_run_dispatch_ops_lat;
	unsigned long long gc_read_no_tag1_cnt;
	unsigned long long gc_read_no_tag2_cnt;
	unsigned long long gc_read_try_get_tag_cnt;
};
#endif



#endif /* _LINUX_JW_TRACE_H */
