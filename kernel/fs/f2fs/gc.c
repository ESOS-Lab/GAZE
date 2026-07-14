// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/random.h>
#include <linux/sched/mm.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include "iostat.h"
#include <trace/events/f2fs.h>

static struct kmem_cache *victim_entry_slab;

#ifdef JW_COPY_COMMAND
static struct kmem_cache *endio_copy_meta_slab;
#endif

#ifdef GC_PAGE_POOL
static mempool_t *gc_page_pool;
#endif

static unsigned int count_bits(const unsigned long *addr,
				unsigned int offset, unsigned int len);
#ifdef PGC
static int p_gc_func(void *data);
#endif


#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
static inline unsigned long long OS_TimeGetNS_( void )
{
    //struct timespec64 lTime;
    ////ktime_get_coarse_real_ts64(&lTime);
    //ktime_get_real_ts64(&lTime);
    //return (lTime.tv_sec * 1000000000 + lTime.tv_nsec );
    return ktime_get_ns();
}
#endif

#ifdef GC_PROFILE_TIMESERIAL	
unsigned long long OS_TimeGetUS_( void )
{
    struct timespec64 lTime;
    //ktime_get_coarse_real_ts64(&lTime);
    ktime_get_real_ts64(&lTime);
    return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

}
#endif

//#ifndef NONBLOCKING_GC
static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	wait_queue_head_t *fggc_wq = &sbi->gc_thread->fggc_wq;
	unsigned int wait_ms;
	struct f2fs_gc_control gc_control = {
		.victim_segno = NULL_SEGNO,
		.should_migrate_blocks = false,
		.err_gc_skipped = false };

	wait_ms = gc_th->min_sleep_time;

	set_freezable();
	do {
		bool sync_mode, foreground = false;

		wait_event_freezable_timeout(*wq,
				kthread_should_stop() ||
				waitqueue_active(fggc_wq) ||
				gc_th->gc_wake,
				msecs_to_jiffies(wait_ms));

		if (test_opt(sbi, GC_MERGE) && waitqueue_active(fggc_wq))
			foreground = true;

		/* give it a try one time */
		if (gc_th->gc_wake)
			gc_th->gc_wake = false;

		if (f2fs_readonly(sbi->sb)) {
			stat_other_skip_bggc_count(sbi);
			continue;
		}
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			increase_sleep_time(gc_th, &wait_ms);
			stat_other_skip_bggc_count(sbi);
			continue;
		}

		if (time_to_inject(sbi, FAULT_CHECKPOINT))
			f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_FAULT_INJECT);

		if (!sb_start_write_trylock(sbi->sb)) {
			stat_other_skip_bggc_count(sbi);
			continue;
		}

		/*
		 * [GC triggering condition]
		 * 0. GC is not conducted currently.
		 * 1. There are enough dirty segments.
		 * 2. IO subsystem is idle by checking the # of writeback pages.
		 * 3. IO subsystem is idle by checking the # of requests in
		 *    bdev's request list.
		 *
		 * Note) We have to avoid triggering GCs frequently.
		 * Because it is possible that some segments can be
		 * invalidated soon after by user update or deletion.
		 * So, I'd like to wait some time to collect dirty segments.
		 */
		if (sbi->gc_mode == GC_URGENT_HIGH ||
				sbi->gc_mode == GC_URGENT_MID) {
			wait_ms = gc_th->urgent_sleep_time;
			f2fs_down_write(&sbi->gc_lock);
			goto do_gc;
		}

		if (foreground) {
			f2fs_down_write(&sbi->gc_lock);
			goto do_gc;
		} else if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
			stat_other_skip_bggc_count(sbi);
			goto next;
		}

#ifndef ASYNC_GC
		if (!is_idle(sbi, GC_TIME)) {
			increase_sleep_time(gc_th, &wait_ms);
			f2fs_up_write(&sbi->gc_lock);
			stat_io_skip_bggc_count(sbi);
			goto next;
		}
#endif

		if (has_enough_invalid_blocks(sbi))
			decrease_sleep_time(gc_th, &wait_ms);
		else
			increase_sleep_time(gc_th, &wait_ms);
do_gc:
		stat_inc_gc_call_count(sbi, foreground ?
					FOREGROUND : BACKGROUND);

		sync_mode = F2FS_OPTION(sbi).bggc_mode == BGGC_MODE_SYNC;

		/* foreground GC was been triggered via f2fs_balance_fs() */
		if (foreground)
			sync_mode = false;

		gc_control.init_gc_type = sync_mode ? FG_GC : BG_GC;
		gc_control.no_bg_gc = foreground;
		gc_control.nr_free_secs = foreground ? 1 : 0;
		
#ifdef DISABLE_BG_GC_THREAD
		if (foreground) {
			if (f2fs_gc(sbi, &gc_control)) {
				/* don't bother wait_ms by foreground gc */
				if (!foreground)
					wait_ms = gc_th->no_gc_sleep_time;
				} else {
					/* reset wait_ms to default sleep time */
					if (wait_ms == gc_th->no_gc_sleep_time)
						wait_ms = gc_th->min_sleep_time;
				}
			} else {
				wait_ms = gc_th->no_gc_sleep_time;
			}
#else
		/* if return value is not zero, no victim was selected */
		if (f2fs_gc(sbi, &gc_control)) {
			/* don't bother wait_ms by foreground gc */
			if (!foreground)
				wait_ms = gc_th->no_gc_sleep_time;
		} else {
			/* reset wait_ms to default sleep time */
			if (wait_ms == gc_th->no_gc_sleep_time)
				wait_ms = gc_th->min_sleep_time;
		}
#endif

		if (foreground)
			wake_up_all(&gc_th->fggc_wq);

		trace_f2fs_background_gc(sbi->sb, wait_ms,
				prefree_segments(sbi), free_segments(sbi));

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi, true);
next:
		if (sbi->gc_mode != GC_NORMAL) {
			spin_lock(&sbi->gc_remaining_trials_lock);
			if (sbi->gc_remaining_trials) {
				sbi->gc_remaining_trials--;
				if (!sbi->gc_remaining_trials)
					sbi->gc_mode = GC_NORMAL;
			}
			spin_unlock(&sbi->gc_remaining_trials_lock);
		}
		sb_end_write(sbi->sb);

	} while (!kthread_should_stop());
	return 0;
}
//#else
//static int gc_thread_func(void *data)
//{
//	struct f2fs_sb_info *sbi = data;
//	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
//	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
//	wait_queue_head_t *fggc_wq = &sbi->gc_thread->fggc_wq;
//	unsigned int wait_ms;
//	struct f2fs_gc_control gc_control = {
//		.victim_segno = NULL_SEGNO,
//		.should_migrate_blocks = false,
//		.err_gc_skipped = false };
//
//	wait_ms = gc_th->min_sleep_time;
//
//	set_freezable();
//	do {
//		bool sync_mode, foreground = false, foreground_by_wb_thread = false, 
//			 	foreground_by_app_thread = false;
//
//		wait_event_freezable_timeout(*wq,
//				kthread_should_stop() ||
//				waitqueue_active(fggc_wq) ||
//				gc_th->gc_wake_nonblock || 
//				gc_th->gc_wake,
//				msecs_to_jiffies(wait_ms));
//		//printk("%s: woke up!!!!!", __func__);
//		//if ((test_opt(sbi, GC_MERGE) && waitqueue_active(fggc_wq))
//		if ((waitqueue_active(fggc_wq))
//				|| gc_th->gc_wake_nonblock) {
//			foreground = true;
//			//if (test_opt(sbi, GC_MERGE) && waitqueue_active(fggc_wq)) {
//			if (waitqueue_active(fggc_wq)) {
//				foreground_by_wb_thread = true;
//			} else {
//				foreground_by_app_thread = true;
//			}
//		} 
//		//if ((test_opt(sbi, GC_MERGE) && waitqueue_active(fggc_wq))
//		if ((waitqueue_active(fggc_wq))
//				|| gc_th->gc_wake_nonblock) {
//			foreground = true;
//		} 
//		//else if (!waitqueue_active(fggc_wq)) {
//		//	printk("%s: not expected!!!!!!!!!!!!!!", __func__);
//		//}
//
//		/* give it a try one time */
//		if (gc_th->gc_wake)
//			gc_th->gc_wake = false;
//		
//		if (gc_th->gc_wake_nonblock)
//			gc_th->gc_wake_nonblock = false;
//
//		if (f2fs_readonly(sbi->sb)) {
//			stat_other_skip_bggc_count(sbi);
//			continue;
//		}
//		if (kthread_should_stop())
//			break;
//
//		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
//			increase_sleep_time(gc_th, &wait_ms);
//			stat_other_skip_bggc_count(sbi);
//			continue;
//		}
//
//		if (time_to_inject(sbi, FAULT_CHECKPOINT))
//			f2fs_stop_checkpoint(sbi, false,
//					STOP_CP_REASON_FAULT_INJECT);
//
//		if (!sb_start_write_trylock(sbi->sb)) {
//			stat_other_skip_bggc_count(sbi);
//			continue;
//		}
//
//		/*
//		 * [GC triggering condition]
//		 * 0. GC is not conducted currently.
//		 * 1. There are enough dirty segments.
//		 * 2. IO subsystem is idle by checking the # of writeback pages.
//		 * 3. IO subsystem is idle by checking the # of requests in
//		 *    bdev's request list.
//		 *
//		 * Note) We have to avoid triggering GCs frequently.
//		 * Because it is possible that some segments can be
//		 * invalidated soon after by user update or deletion.
//		 * So, I'd like to wait some time to collect dirty segments.
//		 */
//		if (sbi->gc_mode == GC_URGENT_HIGH ||
//				sbi->gc_mode == GC_URGENT_MID) {
//			wait_ms = gc_th->urgent_sleep_time;
//			f2fs_down_write(&sbi->gc_lock);
//			goto do_gc;
//		}
//
//		if (foreground) {
//			f2fs_down_write(&sbi->gc_lock);
//			goto do_gc;
//		} else if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
//			stat_other_skip_bggc_count(sbi);
//			goto next;
//		}
//
//#ifndef ASYNC_GC
//		if (!is_idle(sbi, GC_TIME)) {
//			increase_sleep_time(gc_th, &wait_ms);
//			f2fs_up_write(&sbi->gc_lock);
//			stat_io_skip_bggc_count(sbi);
//			goto next;
//		}
//#endif
//
//		if (has_enough_invalid_blocks(sbi))
//			decrease_sleep_time(gc_th, &wait_ms);
//		else
//			increase_sleep_time(gc_th, &wait_ms);
//do_gc:
//		stat_inc_gc_call_count(sbi, foreground ?
//					FOREGROUND : BACKGROUND);
//
//		sync_mode = F2FS_OPTION(sbi).bggc_mode == BGGC_MODE_SYNC;
//
//		/* foreground GC was been triggered via f2fs_balance_fs() */
//		if (foreground)
//			sync_mode = false;
//
//		gc_control.init_gc_type = sync_mode ? FG_GC : BG_GC;
//		gc_control.no_bg_gc = foreground;
//		gc_control.nr_free_secs = foreground ? 1 : 0;
//
//		/* if return value is not zero, no victim was selected */
//		printk("%s: start GC!!", __func__);
//		if (f2fs_gc(sbi, &gc_control)) {
//			/* don't bother wait_ms by foreground gc */
//			if (!foreground)
//				wait_ms = gc_th->no_gc_sleep_time;
//		} else {
//			/* reset wait_ms to default sleep time */
//			if (wait_ms == gc_th->no_gc_sleep_time)
//				wait_ms = gc_th->min_sleep_time;
//		}
//
//		//if (foreground) {
//		if (foreground_by_wb_thread) {
//			wake_up_all(&gc_th->fggc_wq);
//		}
//
//		trace_f2fs_background_gc(sbi->sb, wait_ms,
//				prefree_segments(sbi), free_segments(sbi));
//
//		/* balancing f2fs's metadata periodically */
//		f2fs_balance_fs_bg(sbi, true);
//next:
//		if (sbi->gc_mode != GC_NORMAL) {
//			spin_lock(&sbi->gc_remaining_trials_lock);
//			if (sbi->gc_remaining_trials) {
//				sbi->gc_remaining_trials--;
//				if (!sbi->gc_remaining_trials)
//					sbi->gc_mode = GC_NORMAL;
//			}
//			spin_unlock(&sbi->gc_remaining_trials_lock);
//		}
//		sb_end_write(sbi->sb);
//
//	} while (!kthread_should_stop());
//	return 0;
//}
//#endif

int f2fs_start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th)
		return -ENOMEM;

	gc_th->urgent_sleep_time = DEF_GC_THREAD_URGENT_SLEEP_TIME;
	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_wake = false;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	init_waitqueue_head(&sbi->gc_thread->fggc_wq);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
			"f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		int err = PTR_ERR(gc_th->f2fs_gc_task);

		kfree(gc_th);
		sbi->gc_thread = NULL;
		return err;
	}

#ifdef PGC
	int err_ = 0;
	struct f2fs_p_gc_kthread* p_gc;
  	for (int i = 0; i < NUM_PGC_THREADS ; i++) {
		p_gc = &gc_th->p_gc[i];
		p_gc->sbi = sbi;
		init_waitqueue_head(&p_gc->gc_wait_queue_head);
		init_waitqueue_head(&p_gc->caller_wq);
		p_gc->done = true;
		p_gc->p_gc_idx = i;
		p_gc->f2fs_gc_task = kthread_run(p_gc_func, p_gc, "p_gc_thread-%u", i);
		if (IS_ERR(p_gc->f2fs_gc_task)) {
			err_ = PTR_ERR(p_gc->f2fs_gc_task);
			kfree(gc_th);
			sbi->gc_thread = NULL;
			return err_;
		}
  	}
#endif

	return 0;
}

void f2fs_stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;

	if (!gc_th)
		return;
#ifdef PGC
	for (int i = 0 ; i < NUM_PGC_THREADS ; i++) {
		struct f2fs_p_gc_kthread *p_gc = &gc_th->p_gc[i];
		if (p_gc) {
			kthread_stop(p_gc->f2fs_gc_task);
			wake_up_all(&p_gc->caller_wq);
		}
	}
#endif
	kthread_stop(gc_th->f2fs_gc_task);
	wake_up_all(&gc_th->fggc_wq);
	kfree(gc_th);
	sbi->gc_thread = NULL;
}

#ifdef GC_PIPELINE
static int do_garbage_collect_pipelined_writing(struct f2fs_sb_info *sbi);

static int pipelined_gc_write_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	wait_queue_head_t *prereader_wq = &sbi->pipelined_gc_write_thread->prereader_wq;
	set_freezable();
	do {

		wait_event_interruptible(*wq, kthread_should_stop() || sbi->pipelined_gc_write_thread_wake);
		sbi->pipelined_gc_write_thread_wake = false;

		if (kthread_should_stop())
			break;

		//printk("%s start do_garbage_collect_pipelined_writing", __func__);
		do_garbage_collect_pipelined_writing(sbi);
		
		//printk("%s wake up prereader_wq", __func__);
		/* wakeup gc pre-reader thread */
		sbi->pipelined_gc_write_done = 1;
		//printk("%s set pipelined_gc_write_done to 1", __func__);
		wake_up_all(&gc_th->prereader_wq);
		//printk("%s wakeup done. ", __func__);

	} while (!kthread_should_stop());
	return 0;
}

#ifdef GC_PIPELINE_BATCHED 
static int do_garbage_collect_batched_pipelined_writing(struct f2fs_sb_info *sbi);
#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
static int do_garbage_collect_batched_block_granularity_pipelined_writing(struct f2fs_sb_info *sbi);
#endif

#ifdef GC_DEEP_PIPELINE_BATCHED
static int do_garbage_collect_batched_deep_pipelined_reading_second_step(struct f2fs_sb_info *sbi);

static int batched_deep_pipelined_gc_read_second_step_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	//struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	wait_queue_head_t *wq = &sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head;
	set_freezable();
	do {

		wait_event_interruptible(*wq, kthread_should_stop() || 
					sbi->pipelined_gc_read_second_step_thread_wake);
		sbi->pipelined_gc_read_second_step_thread_wake = false;

		if (kthread_should_stop())
			break;

		//printk("%s do_gc start", __func__);
		do_garbage_collect_batched_deep_pipelined_reading_second_step(sbi);
		//printk("%s do_gc end", __func__);
		/* wakeup gc pre-reader thread */
		sbi->pipelined_gc_read_second_step_done = 1;
		/* FIX (reader stranded across rounds): the round orchestrator now
		 * waits for this flag before it may return (and thus before the
		 * next round's init can reset the shared pacing counters), so it
		 * must be woken here. Without this handshake the round could end
		 * with this thread still batches behind (its blocks all dead under
		 * rm -rf, writer passing on stale done_blk_seq); the next round
		 * then reset read_step2_credit_seq under it and stranded it
		 * mid-round forever (observed live: reader at batch 28/32 waiting
		 * credit 29 while credit was 1 of the NEXT round). */
		smp_mb();
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
		wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_wq);
#else
		wake_up_all(&sbi->pipelined_gc_write_thread->prereader_wq);
#endif

	} while (!kthread_should_stop());
	return 0;
}

int f2fs_start_pipelined_gc_read_second_step_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_read_second_step_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_pipelined_gc_read_second_step_kthread), GFP_KERNEL);
	if (!gc_th)
		return -ENOMEM;

	//gc_th->urgent_sleep_time = DEF_GC_THREAD_URGENT_SLEEP_TIME;
	//gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	//gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	//gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	//gc_th->gc_wake = false;

	sbi->pipelined_gc_read_second_step_thread = gc_th;
	
	sbi->pipelined_gc_read_second_step_done = 0;

	sbi->pipelined_gc_read_second_step_wake = false;
	init_waitqueue_head(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
	sbi->pipelined_gc_read_second_step_thread->f2fs_gc_task 
			= kthread_run(batched_deep_pipelined_gc_read_second_step_thread_func, sbi,
				"f2fs_pipelined_gc_read_second_step-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		int err = PTR_ERR(gc_th->f2fs_gc_task);

		kfree(gc_th);
		sbi->pipelined_gc_read_second_step_thread = NULL;
		return err;
	}

	return 0;
}

void f2fs_stop_pipelined_gc_read_second_step_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_read_second_step_kthread *gc_th = sbi->pipelined_gc_read_second_step_thread;

	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->pipelined_gc_read_second_step_thread = NULL;
}

static int do_garbage_collect_batched_deep_pipelined_writing(struct f2fs_sb_info *sbi);

#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
static int do_garbage_collect_batched_deep_pipelined_writing_first_step(struct f2fs_sb_info *sbi);
static int do_garbage_collect_batched_deep_pipelined_writing_second_step(struct f2fs_sb_info *sbi);

static int batched_pipelined_gc_write_second_step_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_pipelined_gc_write_second_step_kthread *gc_th = sbi->pipelined_gc_write_second_step_thread;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head;
	wait_queue_head_t *prereader_wq = &sbi->pipelined_gc_write_second_step_thread->prereader_wq;
#ifdef GC_DEEP_PIPELINE_BATCHED
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
#endif
	set_freezable();
	do {

		wait_event_interruptible(*wq, kthread_should_stop() || sbi->pipelined_gc_write_second_step_thread_wake);
		sbi->pipelined_gc_write_second_step_thread_wake = false;

		if (kthread_should_stop())
			break;
		//printk("%s do_gc start. segno: %llu", __func__, pgci->start_segno);
		if (IS_DATASEG(get_seg_entry(sbi, pgci->start_segno)->type)) {
			do_garbage_collect_batched_deep_pipelined_writing_second_step(sbi);
		} else {
			f2fs_bug_on(sbi, 1);
		}
		//printk("%s do_gc done. ", __func__);
		//printk("%s wake up prereader_wq", __func__);

		/* wakeup gc pre-reader thread */
		sbi->pipelined_gc_write_done = 1;
		//printk("%s set pipelined_gc_write_done to 1", __func__);
		wake_up_all(&gc_th->prereader_wq);
		//printk("%s wakeup done. ", __func__);

	} while (!kthread_should_stop());
	return 0;
}

int f2fs_start_pipelined_gc_write_second_step_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_write_second_step_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_pipelined_gc_write_second_step_kthread), GFP_KERNEL);
	if (!gc_th)
		return -ENOMEM;

	//gc_th->urgent_sleep_time = DEF_GC_THREAD_URGENT_SLEEP_TIME;
	//gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	//gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	//gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	//gc_th->gc_wake = false;

	sbi->pipelined_gc_write_second_step_thread = gc_th;
	
	sbi->pipelined_gc_write_done = 0;

	sbi->pipelined_gc_write_second_step_wake = false;
	init_waitqueue_head(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
	init_waitqueue_head(&sbi->pipelined_gc_write_second_step_thread->prereader_wq);
	init_waitqueue_head(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
	init_waitqueue_head(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
	printk("%s: init all wqs. \n", __func__);	
	sbi->pipelined_gc_write_second_step_thread->f2fs_gc_task = kthread_run(batched_pipelined_gc_write_second_step_thread_func, sbi,
			"f2fs_pipelined_write_second_step_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		int err = PTR_ERR(gc_th->f2fs_gc_task);

		kfree(gc_th);
		sbi->pipelined_gc_write_second_step_thread = NULL;
		return err;
	}

	return 0;
}

void f2fs_stop_pipelined_gc_write_second_step_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_write_second_step_kthread *gc_th = sbi->pipelined_gc_write_second_step_thread;

	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	wake_up_all(&gc_th->prereader_wq);
	wake_up_all(&gc_th->prereader_step2_wq);
	wake_up_all(&gc_th->prewriter_step1_wq);
	kfree(gc_th);
	sbi->pipelined_gc_write_second_step_thread = NULL;
}

#endif /* end of GC_DEEP_PIPELINE_WRITE_BATCHED */
#endif /* end of GC_DEEP_PIPELINE_BATCHED */

static int batched_pipelined_gc_write_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	wait_queue_head_t *prereader_wq = &sbi->pipelined_gc_write_thread->prereader_wq;
	wait_queue_head_t *prereader_step2_wq = &sbi->pipelined_gc_write_thread->prereader_step2_wq;
#ifdef GC_DEEP_PIPELINE_BATCHED
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
#endif
	bool need_to_wake_prereader = true;
	set_freezable();
	do {

		wait_event_interruptible(*wq, kthread_should_stop() || sbi->pipelined_gc_write_thread_wake);
		sbi->pipelined_gc_write_thread_wake = false;

		if (kthread_should_stop())
			break;
		need_to_wake_prereader = true;
		//printk("%s do_gc start. segno: %llu", __func__, pgci->start_segno);
#ifdef GC_DEEP_PIPELINE_BATCHED
		if (IS_DATASEG(get_seg_entry(sbi, pgci->start_segno)->type)) {
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
			do_garbage_collect_batched_deep_pipelined_writing_first_step(sbi);
			need_to_wake_prereader = false;
#else
			do_garbage_collect_batched_deep_pipelined_writing(sbi);
#endif
		} else {
			do_garbage_collect_batched_block_granularity_pipelined_writing(sbi);
		}
#elif defined (GC_BLOCK_GRANULARITY_PIPELINE_BATCHED)
		do_garbage_collect_batched_block_granularity_pipelined_writing(sbi);
#else
		do_garbage_collect_batched_pipelined_writing(sbi);
#endif		
		//printk("%s do_gc done. ", __func__);
		//printk("%s wake up prereader_wq", __func__);

		if (need_to_wake_prereader) { 
			/* wakeup gc pre-reader thread */
			sbi->pipelined_gc_write_done = 1;
			//printk("%s set pipelined_gc_write_done to 1", __func__);
			wake_up_all(&gc_th->prereader_wq);
			//printk("%s wakeup done. ", __func__);
		}

	} while (!kthread_should_stop());
	return 0;
}
#endif

int f2fs_start_pipelined_gc_write_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_write_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_pipelined_gc_write_kthread), GFP_KERNEL);
	if (!gc_th)
		return -ENOMEM;

	//gc_th->urgent_sleep_time = DEF_GC_THREAD_URGENT_SLEEP_TIME;
	//gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	//gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	//gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	//gc_th->gc_wake = false;

	sbi->pipelined_gc_write_thread = gc_th;
	
	sbi->pipelined_gc_write_done = 0;

	sbi->pipelined_gc_write_wake = false;
	init_waitqueue_head(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	init_waitqueue_head(&sbi->pipelined_gc_write_thread->prereader_wq);
	init_waitqueue_head(&sbi->pipelined_gc_write_thread->prereader_step2_wq);
#ifdef GC_PIPELINE_BATCHED 
	sbi->pipelined_gc_write_thread->f2fs_gc_task = kthread_run(batched_pipelined_gc_write_thread_func, sbi,
			"f2fs_pipelined_write_gc-%u:%u", MAJOR(dev), MINOR(dev));
#else
	sbi->pipelined_gc_write_thread->f2fs_gc_task = kthread_run(pipelined_gc_write_thread_func, sbi,
			"f2fs_pipelined_write_gc-%u:%u", MAJOR(dev), MINOR(dev));
#endif
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		int err = PTR_ERR(gc_th->f2fs_gc_task);

		kfree(gc_th);
		sbi->pipelined_gc_write_thread = NULL;
		return err;
	}

	return 0;
}

void f2fs_stop_pipelined_gc_write_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;

	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	wake_up_all(&gc_th->prereader_wq);
	wake_up_all(&gc_th->prereader_step2_wq);
	kfree(gc_th);
	sbi->pipelined_gc_write_thread = NULL;
}
#endif

static int select_gc_type(struct f2fs_sb_info *sbi, int gc_type)
{
	int gc_mode;

	if (gc_type == BG_GC) {
		if (sbi->am.atgc_enabled)
			gc_mode = GC_AT;
		else
			gc_mode = GC_CB;
	} else {
		gc_mode = GC_GREEDY;
	}

	switch (sbi->gc_mode) {
	case GC_IDLE_CB:
		gc_mode = GC_CB;
		break;
	case GC_IDLE_GREEDY:
	case GC_URGENT_HIGH:
		gc_mode = GC_GREEDY;
		break;
	case GC_IDLE_AT:
		gc_mode = GC_AT;
		break;
	}

	return gc_mode;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
			int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (p->alloc_mode == SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_bitmap = dirty_i->dirty_segmap[type];
		p->max_search = dirty_i->nr_dirty[type];
		p->ofs_unit = 1;
	} else if (p->alloc_mode == AT_SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_bitmap = dirty_i->dirty_segmap[type];
		p->max_search = dirty_i->nr_dirty[type];
		p->ofs_unit = 1;
	} else {
		p->gc_mode = select_gc_type(sbi, gc_type);
		p->ofs_unit = SEGS_PER_SEC(sbi);
		if (__is_large_section(sbi)) {
			p->dirty_bitmap = dirty_i->dirty_secmap;
			p->max_search = count_bits(p->dirty_bitmap,
						0, MAIN_SECS(sbi));
		} else {
			p->dirty_bitmap = dirty_i->dirty_segmap[DIRTY];
			p->max_search = dirty_i->nr_dirty[DIRTY];
		}
	}

	/*
	 * adjust candidates range, should select all dirty segments for
	 * foreground GC and urgent GC cases.
	 */
	if (gc_type != FG_GC &&
			(sbi->gc_mode != GC_URGENT_HIGH) &&
			(p->gc_mode != GC_AT && p->alloc_mode != AT_SSR) &&
			p->max_search > sbi->max_victim_search)
		p->max_search = sbi->max_victim_search;

	/* let's select beginning hot/small space first. */
	if (f2fs_need_rand_seg(sbi))
		p->offset = prandom_u32_max(MAIN_SECS(sbi) *
						SEGS_PER_SEC(sbi));
	else if (type == CURSEG_HOT_DATA || IS_NODESEG(type))
		p->offset = 0;
	else
		p->offset = SIT_I(sbi)->last_victim[p->gc_mode];
}

static unsigned int get_max_cost(struct f2fs_sb_info *sbi,
				struct victim_sel_policy *p)
{
	/* SSR allocates in a segment unit */
	if (p->alloc_mode == SSR)
		return BLKS_PER_SEG(sbi);
	else if (p->alloc_mode == AT_SSR)
		return UINT_MAX;

	/* LFS */
	if (p->gc_mode == GC_GREEDY)
		return SEGS_TO_BLKS(sbi, 2 * p->ofs_unit);
	else if (p->gc_mode == GC_CB)
		return UINT_MAX;
	else if (p->gc_mode == GC_AT)
		return UINT_MAX;
	else /* No other gc_mode */
		return 0;
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int secno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
	for_each_set_bit(secno, dirty_i->victim_secmap, MAIN_SECS(sbi)) {
		if (sec_usage_check(sbi, secno))
			continue;
		clear_bit(secno, dirty_i->victim_secmap);
		return GET_SEG_FROM_SEC(sbi, secno);
	}
	return NULL_SEGNO;
}

static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int start = GET_SEG_FROM_SEC(sbi, secno);
	unsigned long long mtime = 0;
	unsigned int vblocks;
	unsigned char age = 0;
	unsigned char u;
	unsigned int i;
	unsigned int usable_segs_per_sec = f2fs_usable_segs_in_sec(sbi, segno);

	for (i = 0; i < usable_segs_per_sec; i++)
		mtime += get_seg_entry(sbi, start + i)->mtime;
	vblocks = get_valid_blocks(sbi, segno, true);

	mtime = div_u64(mtime, usable_segs_per_sec);
	vblocks = div_u64(vblocks, usable_segs_per_sec);

	u = BLKS_TO_SEGS(sbi, vblocks * 100);

	/* Handle if the system time has changed by the user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		age = 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
}

static inline unsigned int get_gc_cost(struct f2fs_sb_info *sbi,
			unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
		return get_seg_entry(sbi, segno)->ckpt_valid_blocks;

	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
		return get_valid_blocks(sbi, segno, true);
	else if (p->gc_mode == GC_CB)
		return get_cb_cost(sbi, segno);

	f2fs_bug_on(sbi, 1);
	return 0;
}

static unsigned int count_bits(const unsigned long *addr,
				unsigned int offset, unsigned int len)
{
	unsigned int end = offset + len, sum = 0;

	while (offset < end) {
		if (test_bit(offset++, addr))
			++sum;
	}
	return sum;
}

static bool f2fs_check_victim_tree(struct f2fs_sb_info *sbi,
				struct rb_root_cached *root)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct rb_node *cur = rb_first_cached(root), *next;
	struct victim_entry *cur_ve, *next_ve;

	while (cur) {
		next = rb_next(cur);
		if (!next)
			return true;

		cur_ve = rb_entry(cur, struct victim_entry, rb_node);
		next_ve = rb_entry(next, struct victim_entry, rb_node);

		if (cur_ve->mtime > next_ve->mtime) {
			f2fs_info(sbi, "broken victim_rbtree, "
				"cur_mtime(%llu) next_mtime(%llu)",
				cur_ve->mtime, next_ve->mtime);
			return false;
		}
		cur = next;
	}
#endif
	return true;
}

static struct victim_entry *__lookup_victim_entry(struct f2fs_sb_info *sbi,
					unsigned long long mtime)
{
	struct atgc_management *am = &sbi->am;
	struct rb_node *node = am->root.rb_root.rb_node;
	struct victim_entry *ve = NULL;

	while (node) {
		ve = rb_entry(node, struct victim_entry, rb_node);

		if (mtime < ve->mtime)
			node = node->rb_left;
		else
			node = node->rb_right;
	}
	return ve;
}

static struct victim_entry *__create_victim_entry(struct f2fs_sb_info *sbi,
		unsigned long long mtime, unsigned int segno)
{
	struct atgc_management *am = &sbi->am;
	struct victim_entry *ve;

	ve =  f2fs_kmem_cache_alloc(victim_entry_slab, GFP_NOFS, true, NULL);

	ve->mtime = mtime;
	ve->segno = segno;

	list_add_tail(&ve->list, &am->victim_list);
	am->victim_count++;

	return ve;
}

static void __insert_victim_entry(struct f2fs_sb_info *sbi,
				unsigned long long mtime, unsigned int segno)
{
	struct atgc_management *am = &sbi->am;
	struct rb_root_cached *root = &am->root;
	struct rb_node **p = &root->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct victim_entry *ve;
	bool left_most = true;

	/* look up rb tree to find parent node */
	while (*p) {
		parent = *p;
		ve = rb_entry(parent, struct victim_entry, rb_node);

		if (mtime < ve->mtime) {
			p = &(*p)->rb_left;
		} else {
			p = &(*p)->rb_right;
			left_most = false;
		}
	}

	ve = __create_victim_entry(sbi, mtime, segno);

	rb_link_node(&ve->rb_node, parent, p);
	rb_insert_color_cached(&ve->rb_node, root, left_most);
}

static void add_victim_entry(struct f2fs_sb_info *sbi,
				struct victim_sel_policy *p, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int start = GET_SEG_FROM_SEC(sbi, secno);
	unsigned long long mtime = 0;
	unsigned int i;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
		if (p->gc_mode == GC_AT &&
			get_valid_blocks(sbi, segno, true) == 0)
			return;
	}

	for (i = 0; i < SEGS_PER_SEC(sbi); i++)
		mtime += get_seg_entry(sbi, start + i)->mtime;
	mtime = div_u64(mtime, SEGS_PER_SEC(sbi));

	/* Handle if the system time has changed by the user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (mtime < sit_i->dirty_min_mtime)
		sit_i->dirty_min_mtime = mtime;
	if (mtime > sit_i->dirty_max_mtime)
		sit_i->dirty_max_mtime = mtime;

	/* don't choose young section as candidate */
	if (sit_i->dirty_max_mtime - mtime < p->age_threshold)
		return;

	__insert_victim_entry(sbi, mtime, segno);
}

static void atgc_lookup_victim(struct f2fs_sb_info *sbi,
						struct victim_sel_policy *p)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct atgc_management *am = &sbi->am;
	struct rb_root_cached *root = &am->root;
	struct rb_node *node;
	struct victim_entry *ve;
	unsigned long long total_time;
	unsigned long long age, u, accu;
	unsigned long long max_mtime = sit_i->dirty_max_mtime;
	unsigned long long min_mtime = sit_i->dirty_min_mtime;
	unsigned int sec_blocks = CAP_BLKS_PER_SEC(sbi);
	unsigned int vblocks;
	unsigned int dirty_threshold = max(am->max_candidate_count,
					am->candidate_ratio *
					am->victim_count / 100);
	unsigned int age_weight = am->age_weight;
	unsigned int cost;
	unsigned int iter = 0;

	if (max_mtime < min_mtime)
		return;

	max_mtime += 1;
	total_time = max_mtime - min_mtime;

	accu = div64_u64(ULLONG_MAX, total_time);
	accu = min_t(unsigned long long, div_u64(accu, 100),
					DEFAULT_ACCURACY_CLASS);

	node = rb_first_cached(root);
next:
	ve = rb_entry_safe(node, struct victim_entry, rb_node);
	if (!ve)
		return;

	if (ve->mtime >= max_mtime || ve->mtime < min_mtime)
		goto skip;

	/* age = 10000 * x% * 60 */
	age = div64_u64(accu * (max_mtime - ve->mtime), total_time) *
								age_weight;

	vblocks = get_valid_blocks(sbi, ve->segno, true);
	f2fs_bug_on(sbi, !vblocks || vblocks == sec_blocks);

	/* u = 10000 * x% * 40 */
	u = div64_u64(accu * (sec_blocks - vblocks), sec_blocks) *
							(100 - age_weight);

	f2fs_bug_on(sbi, age + u >= UINT_MAX);

	cost = UINT_MAX - (age + u);
	iter++;

	if (cost < p->min_cost ||
			(cost == p->min_cost && age > p->oldest_age)) {
		p->min_cost = cost;
		p->oldest_age = age;
		p->min_segno = ve->segno;
	}
skip:
	if (iter < dirty_threshold) {
		node = rb_next(node);
		goto next;
	}
}

/*
 * select candidates around source section in range of
 * [target - dirty_threshold, target + dirty_threshold]
 */
static void atssr_lookup_victim(struct f2fs_sb_info *sbi,
						struct victim_sel_policy *p)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct atgc_management *am = &sbi->am;
	struct victim_entry *ve;
	unsigned long long age;
	unsigned long long max_mtime = sit_i->dirty_max_mtime;
	unsigned long long min_mtime = sit_i->dirty_min_mtime;
	unsigned int vblocks;
	unsigned int dirty_threshold = max(am->max_candidate_count,
					am->candidate_ratio *
					am->victim_count / 100);
	unsigned int cost, iter;
	int stage = 0;

	if (max_mtime < min_mtime)
		return;
	max_mtime += 1;
next_stage:
	iter = 0;
	ve = __lookup_victim_entry(sbi, p->age);
next_node:
	if (!ve) {
		if (stage++ == 0)
			goto next_stage;
		return;
	}

	if (ve->mtime >= max_mtime || ve->mtime < min_mtime)
		goto skip_node;

	age = max_mtime - ve->mtime;

	vblocks = get_seg_entry(sbi, ve->segno)->ckpt_valid_blocks;
	f2fs_bug_on(sbi, !vblocks);

	/* rare case */
	if (vblocks == BLKS_PER_SEG(sbi))
		goto skip_node;

	iter++;

	age = max_mtime - abs(p->age - age);
	cost = UINT_MAX - vblocks;

	if (cost < p->min_cost ||
			(cost == p->min_cost && age > p->oldest_age)) {
		p->min_cost = cost;
		p->oldest_age = age;
		p->min_segno = ve->segno;
	}
skip_node:
	if (iter < dirty_threshold) {
		ve = rb_entry(stage == 0 ? rb_prev(&ve->rb_node) :
					rb_next(&ve->rb_node),
					struct victim_entry, rb_node);
		goto next_node;
	}

	if (stage++ == 0)
		goto next_stage;
}

static void lookup_victim_by_age(struct f2fs_sb_info *sbi,
						struct victim_sel_policy *p)
{
	f2fs_bug_on(sbi, !f2fs_check_victim_tree(sbi, &sbi->am.root));

	if (p->gc_mode == GC_AT)
		atgc_lookup_victim(sbi, p);
	else if (p->alloc_mode == AT_SSR)
		atssr_lookup_victim(sbi, p);
	else
		f2fs_bug_on(sbi, 1);
}

static void release_victim_entry(struct f2fs_sb_info *sbi)
{
	struct atgc_management *am = &sbi->am;
	struct victim_entry *ve, *tmp;

	list_for_each_entry_safe(ve, tmp, &am->victim_list, list) {
		list_del(&ve->list);
		kmem_cache_free(victim_entry_slab, ve);
		am->victim_count--;
	}

	am->root = RB_ROOT_CACHED;

	f2fs_bug_on(sbi, am->victim_count);
	f2fs_bug_on(sbi, !list_empty(&am->victim_list));
}

static bool f2fs_pin_section(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);

	if (!dirty_i->enable_pin_section)
		return false;
	if (!test_and_set_bit(secno, dirty_i->pinned_secmap))
		dirty_i->pinned_secmap_cnt++;
	return true;
}

static bool f2fs_pinned_section_exists(struct dirty_seglist_info *dirty_i)
{
	return dirty_i->pinned_secmap_cnt;
}

static bool f2fs_section_is_pinned(struct dirty_seglist_info *dirty_i,
						unsigned int secno)
{
	return dirty_i->enable_pin_section &&
		f2fs_pinned_section_exists(dirty_i) &&
		test_bit(secno, dirty_i->pinned_secmap);
}

static void f2fs_unpin_all_sections(struct f2fs_sb_info *sbi, bool enable)
{
	unsigned int bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));

	if (f2fs_pinned_section_exists(DIRTY_I(sbi))) {
		memset(DIRTY_I(sbi)->pinned_secmap, 0, bitmap_size);
		DIRTY_I(sbi)->pinned_secmap_cnt = 0;
	}
	DIRTY_I(sbi)->enable_pin_section = enable;
}

static int f2fs_gc_pinned_control(struct inode *inode, int gc_type,
							unsigned int segno)
{
	if (!f2fs_is_pinned_file(inode))
		return 0;
	if (gc_type != FG_GC)
		return -EBUSY;
	if (!f2fs_pin_section(F2FS_I_SB(inode), segno))
		f2fs_pin_file_control(inode, true);
	return -EAGAIN;
}

/*
 * This function is called from two paths.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
int f2fs_get_victim(struct f2fs_sb_info *sbi, unsigned int *result,
			int gc_type, int type, char alloc_mode,
			unsigned long long age)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct sit_info *sm = SIT_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, last_victim;
	unsigned int last_segment;
	unsigned int nsearched;
	bool is_atgc;
	int ret = 0;

	mutex_lock(&dirty_i->seglist_lock);
	last_segment = MAIN_SECS(sbi) * SEGS_PER_SEC(sbi);

	p.alloc_mode = alloc_mode;
	p.age = age;
	p.age_threshold = sbi->am.age_threshold;

retry:
	select_policy(sbi, gc_type, type, &p);
	p.min_segno = NULL_SEGNO;
	p.oldest_age = 0;
	p.min_cost = get_max_cost(sbi, &p);

	is_atgc = (p.gc_mode == GC_AT || p.alloc_mode == AT_SSR);
	nsearched = 0;

	if (is_atgc)
		SIT_I(sbi)->dirty_min_mtime = ULLONG_MAX;

	if (*result != NULL_SEGNO) {
		if (!get_valid_blocks(sbi, *result, false)) {
			ret = -ENODATA;
			goto out;
		}

		if (sec_usage_check(sbi, GET_SEC_FROM_SEG(sbi, *result)))
			ret = -EBUSY;
		else
			p.min_segno = *result;
		goto out;
	}

	ret = -ENODATA;
	if (p.max_search == 0)
		goto out;

	if (__is_large_section(sbi) && p.alloc_mode == LFS) {
		if (sbi->next_victim_seg[BG_GC] != NULL_SEGNO) {
			p.min_segno = sbi->next_victim_seg[BG_GC];
			*result = p.min_segno;
			sbi->next_victim_seg[BG_GC] = NULL_SEGNO;
			goto got_result;
		}
		if (gc_type == FG_GC &&
				sbi->next_victim_seg[FG_GC] != NULL_SEGNO) {
			p.min_segno = sbi->next_victim_seg[FG_GC];
			*result = p.min_segno;
			sbi->next_victim_seg[FG_GC] = NULL_SEGNO;
			goto got_result;
		}
	}

	last_victim = sm->last_victim[p.gc_mode];
	if (p.alloc_mode == LFS && gc_type == FG_GC) {
		p.min_segno = check_bg_victims(sbi);
		if (p.min_segno != NULL_SEGNO)
			goto got_it;
	}

	while (1) {
		unsigned long cost, *dirty_bitmap;
		unsigned int unit_no, segno;

		dirty_bitmap = p.dirty_bitmap;
		unit_no = find_next_bit(dirty_bitmap,
				last_segment / p.ofs_unit,
				p.offset / p.ofs_unit);
		segno = unit_no * p.ofs_unit;
		if (segno >= last_segment) {
			if (sm->last_victim[p.gc_mode]) {
				last_segment =
					sm->last_victim[p.gc_mode];
				sm->last_victim[p.gc_mode] = 0;
				p.offset = 0;
				continue;
			}
			break;
		}

		p.offset = segno + p.ofs_unit;
		nsearched++;

#ifdef CONFIG_F2FS_CHECK_FS
		/*
		 * skip selecting the invalid segno (that is failed due to block
		 * validity check failure during GC) to avoid endless GC loop in
		 * such cases.
		 */
		if (test_bit(segno, sm->invalid_segmap))
			goto next;
#endif

		secno = GET_SEC_FROM_SEG(sbi, segno);

		if (sec_usage_check(sbi, secno))
			goto next;

		/* Don't touch checkpointed data */
		if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
			if (p.alloc_mode == LFS) {
				/*
				 * LFS is set to find source section during GC.
				 * The victim should have no checkpointed data.
				 */
				if (get_ckpt_valid_blocks(sbi, segno, true))
					goto next;
			} else {
				/*
				 * SSR | AT_SSR are set to find target segment
				 * for writes which can be full by checkpointed
				 * and newly written blocks.
				 */
				if (!f2fs_segment_has_free_slot(sbi, segno))
					goto next;
			}
		}

		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			goto next;

		if (gc_type == FG_GC && f2fs_section_is_pinned(dirty_i, secno))
			goto next;

		if (is_atgc) {
			add_victim_entry(sbi, &p, segno);
			goto next;
		}

		cost = get_gc_cost(sbi, segno, &p);

		if (p.min_cost > cost) {
			p.min_segno = segno;
			p.min_cost = cost;
		}
next:
		if (nsearched >= p.max_search) {
			if (!sm->last_victim[p.gc_mode] && segno <= last_victim)
				sm->last_victim[p.gc_mode] =
					last_victim + p.ofs_unit;
			else
				sm->last_victim[p.gc_mode] = segno + p.ofs_unit;
			sm->last_victim[p.gc_mode] %=
				(MAIN_SECS(sbi) * SEGS_PER_SEC(sbi));
			break;
		}
	}

	/* get victim for GC_AT/AT_SSR */
	if (is_atgc) {
		lookup_victim_by_age(sbi, &p);
		release_victim_entry(sbi);
	}

	if (is_atgc && p.min_segno == NULL_SEGNO &&
			sm->elapsed_time < p.age_threshold) {
		p.age_threshold = 0;
		goto retry;
	}

	if (p.min_segno != NULL_SEGNO) {
got_it:
		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;
got_result:
		if (p.alloc_mode == LFS) {
			secno = GET_SEC_FROM_SEG(sbi, p.min_segno);
			if (gc_type == FG_GC)
				sbi->cur_victim_sec = secno;
			else
				set_bit(secno, dirty_i->victim_secmap);
		}
		ret = 0;

	}
out:
	if (p.min_segno != NULL_SEGNO)
		trace_f2fs_get_victim(sbi->sb, type, gc_type, &p,
				sbi->cur_victim_sec,
				prefree_segments(sbi), free_segments(sbi));
	mutex_unlock(&dirty_i->seglist_lock);

	return ret;
}

static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino)
{
	struct inode_entry *ie;

	ie = radix_tree_lookup(&gc_list->iroot, ino);
	if (ie)
		return ie->inode;
	return NULL;
}

static void add_gc_inode(struct gc_inode_list *gc_list, struct inode *inode)
{
	struct inode_entry *new_ie;

	if (inode == find_gc_inode(gc_list, inode->i_ino)) {
		iput(inode);
		return;
	}
	new_ie = f2fs_kmem_cache_alloc(f2fs_inode_entry_slab,
					GFP_NOFS, true, NULL);
	new_ie->inode = inode;

	f2fs_radix_tree_insert(&gc_list->iroot, inode->i_ino, new_ie);
	list_add_tail(&new_ie->list, &gc_list->ilist);
}

static void put_gc_inode(struct gc_inode_list *gc_list)
{
	struct inode_entry *ie, *next_ie;

	list_for_each_entry_safe(ie, next_ie, &gc_list->ilist, list) {
		radix_tree_delete(&gc_list->iroot, ie->inode->i_ino);
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(f2fs_inode_entry_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	down_read(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	up_read(&sit_i->sentry_lock);
	return ret;
}

static int jw_check_valid_map(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	down_read(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	up_read(&sit_i->sentry_lock);
	return ret;
}

//#ifdef GC_PIPELINE_LOCKLESS
static int jw_check_valid_map_lockless(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	//down_read(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	//up_read(&sit_i->sentry_lock);
	return ret;
}
//#endif


/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static int gc_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);
//#ifdef GC_LATENCY
//	unsigned long long read_start_t, read_end_t, write_start_t, write_end_t, tmp_write_time_sum = 0;
//	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5];
//	read_start_t = OS_TimeGetNS_();
//#endif


next_step:
	entry = sum;
//#ifdef GC_LATENCY_DETAIL
//	p_start_t[phase] = OS_TimeGetNS_();
//#endif

	if (fggc && phase == 2)
		atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			continue;
		}

		/* phase == 2 */
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during f2fs_get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (f2fs_get_node_info(sbi, nid, &ni, false)) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
//		write_start_t = OS_TimeGetNS_();
#endif
		err = f2fs_move_node_page(node_page, gc_type);
		if (!err && gc_type == FG_GC)
			submitted++;
//#ifdef GC_LATENCY_DETAIL
//		write_end_t = OS_TimeGetNS_();
//		sbi->gc_total_write_time_sum += write_end_t - write_start_t;
//		sbi->gc_total_write_blk_cnt_sum ++;
//		tmp_write_time_sum += write_end_t - write_start_t ;
//#endif
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}
//#ifdef GC_LATENCY_DETAIL
//	p_end_t[phase] = OS_TimeGetNS_();
//	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
//#endif

	if (++phase < 3)
		goto next_step;

	if (fggc)
		atomic_dec(&sbi->wb_sync_req[NODE]);
//#ifdef GC_LATENCY_DETAIL
//	read_end_t = OS_TimeGetNS_();
//	sbi->gc_total_read_time_sum += read_end_t - read_start_t - tmp_write_time_sum
//					- p_lat_t[0]
//					- p_lat_t[1];
//	sbi->gc_node_p0_total_time_sum += p_lat_t[0];
//	sbi->gc_node_p1_total_time_sum += p_lat_t[1];
//	sbi->gc_node_p2_total_time_sum += p_lat_t[2];
//#endif
	return submitted;
}

#ifdef PGC
static int gc_node_segment_pgc(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type, int mode)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
  	int start, end;

	start_addr = START_BLOCK(sbi, segno);
//#ifdef GC_LATENCY
//	unsigned long long read_start_t, read_end_t, write_start_t, write_end_t, tmp_write_time_sum = 0;
//	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5];
//	read_start_t = OS_TimeGetNS_();
//#endif
	/* PGC added */
  	if (mode == NORMAL_GC) {
  	  	start = 0;
  	  	end = usable_blks_in_seg;
  	} else {
  	  	start = mode * usable_blks_in_seg / NUM_PGC_THREADS;
  	  	end = (mode + 1) * usable_blks_in_seg / NUM_PGC_THREADS;
  	  	if (mode == NUM_PGC_THREADS - 1) {
  	  	 	 end = usable_blks_in_seg;
  	  	}
  	}


next_step:
	//entry = sum;
	/* PGC added */
	entry = sum + start;
//#ifdef GC_LATENCY_DETAIL
//	p_start_t[phase] = OS_TimeGetNS_();
//#endif

	if (fggc && phase == 2)
		atomic_inc(&sbi->wb_sync_req[NODE]);

	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
	for (off = start; off < end; off++, entry++) { /* PGC added */
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			continue;
		}

		/* phase == 2 */
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during f2fs_get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (f2fs_get_node_info(sbi, nid, &ni, false)) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
//		write_start_t = OS_TimeGetNS_();
#endif
		err = f2fs_move_node_page(node_page, gc_type);
		if (!err && gc_type == FG_GC)
			submitted++;
//#ifdef GC_LATENCY_DETAIL
//		write_end_t = OS_TimeGetNS_();
//		sbi->gc_total_write_time_sum += write_end_t - write_start_t;
//		sbi->gc_total_write_blk_cnt_sum ++;
//		tmp_write_time_sum += write_end_t - write_start_t ;
//#endif
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}
//#ifdef GC_LATENCY_DETAIL
//	p_end_t[phase] = OS_TimeGetNS_();
//	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
//#endif

	if (++phase < 3)
		goto next_step;

	if (fggc)
		atomic_dec(&sbi->wb_sync_req[NODE]);
//#ifdef GC_LATENCY_DETAIL
//	read_end_t = OS_TimeGetNS_();
//	sbi->gc_total_read_time_sum += read_end_t - read_start_t - tmp_write_time_sum
//					- p_lat_t[0]
//					- p_lat_t[1];
//	sbi->gc_node_p0_total_time_sum += p_lat_t[0];
//	sbi->gc_node_p1_total_time_sum += p_lat_t[1];
//	sbi->gc_node_p2_total_time_sum += p_lat_t[2];
//#endif
	return submitted;
}

#endif

/*
 * Calculate start block index indicating the given node offset.
 * Be careful, caller should give this node offset only indicating direct node
 * blocks. If any node offsets, which point the other types of node blocks such
 * as indirect or double indirect node blocks, are given, it must be a caller's
 * bug.
 */
block_t f2fs_start_bidx_of_node(unsigned int node_ofs, struct inode *inode)
{
	unsigned int indirect_blks = 2 * NIDS_PER_BLOCK + 4;
	unsigned int bidx;

	if (node_ofs == 0)
		return 0;

	if (node_ofs <= 2) {
		bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
		int dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);

		bidx = node_ofs - 2 - dec;
	} else {
		int dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);

		bidx = node_ofs - 5 - dec;
	}
	return bidx * ADDRS_PER_BLOCK(inode) + ADDRS_PER_INODE(inode);
}

static bool is_alive_jw_gc_isalive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node, max_addrs, base;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	//node_page = f2fs_get_node_page(sbi, nid);
	node_page = f2fs_get_node_page_jw_gc_isalive(sbi, nid);
	if (IS_ERR(node_page))
		return false;

	if (f2fs_get_node_info(sbi, nid, dni, false)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (sum->version != dni->version) {
		f2fs_warn(sbi, "%s: valid data with mismatched node version.",
			  __func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	if (f2fs_check_nid_range(sbi, dni->ino)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (IS_INODE(node_page)) {
		base = offset_in_addr(F2FS_INODE(node_page));
		max_addrs = DEF_ADDRS_PER_INODE;
	} else {
		base = 0;
		max_addrs = DEF_ADDRS_PER_BLOCK;
	}

	if (base + ofs_in_node >= max_addrs) {
		f2fs_err(sbi, "Inconsistent blkaddr offset: base:%u, ofs_in_node:%u, max:%u, ino:%u, nid:%u",
			base, ofs_in_node, max_addrs, dni->ino, dni->nid);
		f2fs_put_page(node_page, 1);
		return false;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr) {
#ifdef CONFIG_F2FS_CHECK_FS
		unsigned int segno = GET_SEGNO(sbi, blkaddr);
		unsigned long offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

		if (unlikely(check_valid_map(sbi, segno, offset))) {
			if (!test_and_set_bit(segno, SIT_I(sbi)->invalid_segmap)) {
				f2fs_err(sbi, "mismatched blkaddr %u (source_blkaddr %u) in seg %u",
					 blkaddr, source_blkaddr, segno);
				set_sbi_flag(sbi, SBI_NEED_FSCK);
			}
		}
#endif
		return false;
	}
	return true;
}

static bool is_alive_pipewrt(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node, max_addrs, base;
	block_t source_blkaddr;
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);
	
#ifdef GC_PROFILE_TIMESERIAL	
	read_start_t = OS_TimeGetNS_();
#endif

	node_page = f2fs_get_node_page_pipewrt(sbi, nid);

#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t = OS_TimeGetNS_();
	cur_gcp->p4_is_alive__get_node_page += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif
	
	if (IS_ERR(node_page))
		return false;

	if (f2fs_get_node_info(sbi, nid, dni, false)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t = OS_TimeGetNS_();
	cur_gcp->p4_is_alive__get_node_info += read_end_t - read_start_t;
#endif

	if (sum->version != dni->version) {
		f2fs_warn(sbi, "%s: valid data with mismatched node version.",
			  __func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	if (f2fs_check_nid_range(sbi, dni->ino)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (IS_INODE(node_page)) {
		base = offset_in_addr(F2FS_INODE(node_page));
		max_addrs = DEF_ADDRS_PER_INODE;
	} else {
		base = 0;
		max_addrs = DEF_ADDRS_PER_BLOCK;
	}

	if (base + ofs_in_node >= max_addrs) {
		f2fs_err(sbi, "Inconsistent blkaddr offset: base:%u, ofs_in_node:%u, max:%u, ino:%u, nid:%u",
			base, ofs_in_node, max_addrs, dni->ino, dni->nid);
		f2fs_put_page(node_page, 1);
		return false;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr) {
#ifdef CONFIG_F2FS_CHECK_FS
		unsigned int segno = GET_SEGNO(sbi, blkaddr);
		unsigned long offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

		if (unlikely(check_valid_map(sbi, segno, offset))) {
			if (!test_and_set_bit(segno, SIT_I(sbi)->invalid_segmap)) {
				f2fs_err(sbi, "mismatched blkaddr %u (source_blkaddr %u) in seg %u",
					 blkaddr, source_blkaddr, segno);
				set_sbi_flag(sbi, SBI_NEED_FSCK);
			}
		}
#endif
		return false;
	}
	return true;
}


static bool is_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node, max_addrs, base;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = f2fs_get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return false;

	if (f2fs_get_node_info(sbi, nid, dni, false)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (sum->version != dni->version) {
		f2fs_warn(sbi, "%s: valid data with mismatched node version.",
			  __func__);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
	}

	if (f2fs_check_nid_range(sbi, dni->ino)) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	if (IS_INODE(node_page)) {
		base = offset_in_addr(F2FS_INODE(node_page));
		max_addrs = DEF_ADDRS_PER_INODE;
	} else {
		base = 0;
		max_addrs = DEF_ADDRS_PER_BLOCK;
	}

	if (base + ofs_in_node >= max_addrs) {
		f2fs_err(sbi, "Inconsistent blkaddr offset: base:%u, ofs_in_node:%u, max:%u, ino:%u, nid:%u",
			base, ofs_in_node, max_addrs, dni->ino, dni->nid);
		f2fs_put_page(node_page, 1);
		return false;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = data_blkaddr(NULL, node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr) {
#ifdef CONFIG_F2FS_CHECK_FS
		unsigned int segno = GET_SEGNO(sbi, blkaddr);
		unsigned long offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

		if (unlikely(check_valid_map(sbi, segno, offset))) {
			if (!test_and_set_bit(segno, SIT_I(sbi)->invalid_segmap)) {
				f2fs_err(sbi, "mismatched blkaddr %u (source_blkaddr %u) in seg %u",
					 blkaddr, source_blkaddr, segno);
				set_sbi_flag(sbi, SBI_NEED_FSCK);
			}
		}
#endif
		return false;
	}
	return true;
}

static int ra_data_block(struct inode *inode, pgoff_t index)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	struct dnode_of_data dn;
	struct page *page;
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.ino = inode->i_ino,
		.type = DATA,
		.temp = COLD,
		.op = REQ_OP_READ,
		.op_flags = 0,
		.encrypted_page = NULL,
		.in_list = 0,
	};
	int err;

	page = f2fs_grab_cache_page(mapping, index, true);
	if (!page)
		return -ENOMEM;

	if (f2fs_lookup_read_extent_cache_block(inode, index,
						&dn.data_blkaddr)) {
		if (unlikely(!f2fs_is_valid_blkaddr(sbi, dn.data_blkaddr,
						DATA_GENERIC_ENHANCE_READ))) {
			err = -EFSCORRUPTED;
			goto put_page;
		}
		goto got_it;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
	if (err)
		goto put_page;
	f2fs_put_dnode(&dn);

	if (!__is_valid_data_blkaddr(dn.data_blkaddr)) {
		err = -ENOENT;
		goto put_page;
	}
	if (unlikely(!f2fs_is_valid_blkaddr(sbi, dn.data_blkaddr,
						DATA_GENERIC_ENHANCE))) {
		err = -EFSCORRUPTED;
		goto put_page;
	}
got_it:
	/* read page */
	fio.page = page;
	fio.new_blkaddr = fio.old_blkaddr = dn.data_blkaddr;

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA, true, true);

	f2fs_wait_on_block_writeback(inode, dn.data_blkaddr);

	fio.encrypted_page = f2fs_pagecache_get_page(META_MAPPING(sbi),
					dn.data_blkaddr,
					FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (!fio.encrypted_page) {
		err = -ENOMEM;
		goto put_page;
	}

	err = f2fs_submit_page_bio(&fio);
	if (err)
		goto put_encrypted_page;
	f2fs_put_page(fio.encrypted_page, 0);
	f2fs_put_page(page, 1);

	f2fs_update_iostat(sbi, inode, FS_DATA_READ_IO, F2FS_BLKSIZE);
	f2fs_update_iostat(sbi, NULL, FS_GDATA_READ_IO, F2FS_BLKSIZE);

	return 0;
put_encrypted_page:
	f2fs_put_page(fio.encrypted_page, 1);
put_page:
	f2fs_put_page(page, 1);
	return err;
}

/*
 * Move data block via META_MAPPING while keeping locked data page.
 * This can be used to move blocks, aka LBAs, directly on disk.
 */
static int move_data_block(struct inode *inode, block_t bidx,
				int gc_type, unsigned int segno, int off)
{
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.ino = inode->i_ino,
		.type = DATA,
		.temp = COLD,
		.op = REQ_OP_READ,
		.op_flags = 0,
		.encrypted_page = NULL,
		.in_list = 0,
	};
	struct dnode_of_data dn;
	struct f2fs_summary sum;
	struct node_info ni;
	struct page *page, *mpage;
	block_t newaddr;
	int err = 0;
	bool lfs_mode = f2fs_lfs_mode(fio.sbi);
	int type = fio.sbi->am.atgc_enabled && (gc_type == BG_GC) &&
				(fio.sbi->gc_mode != GC_URGENT_HIGH) ?
				CURSEG_ALL_DATA_ATGC : CURSEG_COLD_DATA;

	/* do not read out */
	page = f2fs_grab_cache_page(inode->i_mapping, bidx, false);
	if (!page)
		return -ENOMEM;

	if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
		err = -ENOENT;
		goto out;
	}

	err = f2fs_gc_pinned_control(inode, gc_type, segno);
	if (err)
		goto out;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
	if (err)
		goto out;

	if (unlikely(dn.data_blkaddr == NULL_ADDR)) {
		ClearPageUptodate(page);
		err = -ENOENT;
		goto put_out;
	}

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA, true, true);

	f2fs_wait_on_block_writeback(inode, dn.data_blkaddr);

	err = f2fs_get_node_info(fio.sbi, dn.nid, &ni, false);
	if (err)
		goto put_out;

	/* read page */
	fio.page = page;
	fio.new_blkaddr = fio.old_blkaddr = dn.data_blkaddr;

	if (lfs_mode)
		f2fs_down_write(&fio.sbi->io_order_lock);

	mpage = f2fs_grab_cache_page(META_MAPPING(fio.sbi),
					fio.old_blkaddr, false);
	if (!mpage) {
		err = -ENOMEM;
		goto up_out;
	}

	fio.encrypted_page = mpage;

	/* read source block in mpage */
	if (!PageUptodate(mpage)) {
		err = f2fs_submit_page_bio(&fio);
		if (err) {
			f2fs_put_page(mpage, 1);
			goto up_out;
		}

		f2fs_update_iostat(fio.sbi, inode, FS_DATA_READ_IO,
							F2FS_BLKSIZE);
		f2fs_update_iostat(fio.sbi, NULL, FS_GDATA_READ_IO,
							F2FS_BLKSIZE);

		lock_page(mpage);
		if (unlikely(mpage->mapping != META_MAPPING(fio.sbi) ||
						!PageUptodate(mpage))) {
			err = -EIO;
			f2fs_put_page(mpage, 1);
			goto up_out;
		}
	}

	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* allocate block address */
	err = f2fs_allocate_data_block(fio.sbi, NULL, fio.old_blkaddr, &newaddr,
				&sum, type, NULL);
	if (err) {
		f2fs_put_page(mpage, 1);
		/* filesystem should shutdown, no need to recovery block */
		goto up_out;
	}

	fio.encrypted_page = f2fs_pagecache_get_page(META_MAPPING(fio.sbi),
				newaddr, FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (!fio.encrypted_page) {
		err = -ENOMEM;
		f2fs_put_page(mpage, 1);
		goto recover_block;
	}

	/* write target block */
	f2fs_wait_on_page_writeback(fio.encrypted_page, DATA, true, true);
	memcpy(page_address(fio.encrypted_page),
				page_address(mpage), PAGE_SIZE);
	f2fs_put_page(mpage, 1);

	f2fs_invalidate_internal_cache(fio.sbi, fio.old_blkaddr);

	set_page_dirty(fio.encrypted_page);
	if (clear_page_dirty_for_io(fio.encrypted_page))
		dec_page_count(fio.sbi, F2FS_DIRTY_META);

	set_page_writeback(fio.encrypted_page);

	fio.op = REQ_OP_WRITE;
	fio.op_flags = REQ_SYNC;
	fio.new_blkaddr = newaddr;
	f2fs_submit_page_write(&fio);

	f2fs_update_iostat(fio.sbi, NULL, FS_GC_DATA_IO, F2FS_BLKSIZE);

	f2fs_update_data_blkaddr(&dn, newaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);

	f2fs_put_page(fio.encrypted_page, 1);
recover_block:
	if (err)
		f2fs_do_replace_block(fio.sbi, &sum, newaddr, fio.old_blkaddr,
							true, true, true);
up_out:
	if (lfs_mode)
		f2fs_up_write(&fio.sbi->io_order_lock);
put_out:
	f2fs_put_dnode(&dn);
out:
	f2fs_put_page(page, 1);
	return err;
}

static int move_data_page(struct inode *inode, block_t bidx, int gc_type,
							unsigned int segno, int off)
{
	struct page *page;
	int err = 0;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
#ifdef GC_PROFILE_TIMESERIAL	
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	unsigned long long read_start_t, read_end_t;
	unsigned long long read_start_t_, read_end_t_;
	read_start_t = OS_TimeGetNS_();
#endif

	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_end) {
			op_flags |= REQ_GC_END;
			sbi->gc_end = false;
			printk("%s: REQ_GC_END!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

	block_t start_addr;
	start_addr = START_BLOCK(sbi, segno);
	bool page_pool_used = false;
#ifndef SKIP_FMAP_TRAVERSE
	page = f2fs_get_lock_data_page_jw_gc(inode, bidx, true);
#else
#ifdef GC_PAGE_POOL
	page = f2fs_get_lock_data_page_jw_gc_without_filemap(inode, bidx, true, start_addr + off, 
									&page_pool_used);
#else
	page = f2fs_get_lock_data_page_jw_gc_without_filemap(inode, bidx, true, start_addr + off);
#endif
#endif
	if (IS_ERR(page)) {
		printk("%s: page err. segno: %lu blkaddr: 0x%lx page_pool_used: %d, page NULL: %d page: %p", __func__, segno, start_addr + off, page_pool_used, page == NULL, page);
		return PTR_ERR(page);
	}

#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t = OS_TimeGetNS_();
	cur_gcp->bef_write_lock_page_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif

#ifdef GC_PIPELINE_LOCKLESS
	if (!jw_check_valid_map_lockless(F2FS_I_SB(inode), segno, off)){
#else
	if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
#endif
		printk("%s: bitmap error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
		err = -ENOENT;
		goto out;
	}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
		cur_gcp->bef_write_1_lat += read_end_t_ - read_start_t;
		read_start_t_ = read_end_t_;
#endif

	err = f2fs_gc_pinned_control(inode, gc_type, segno);
	if (err){
		printk("%s: gcpin error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
		goto out;
	}

	if (gc_type == BG_GC) {
		printk("%s: BG_GC unexpected!!!!!!!!!!", __func__);
		f2fs_bug_on(sbi, 1);
		if (folio_test_writeback(page_folio(page))) {
			err = -EAGAIN;
			goto out;
		}
		set_page_dirty(page);
		set_page_private_gcing(page);
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.ino = inode->i_ino,
			.type = DATA,
			.temp = COLD,
			.op = REQ_OP_WRITE,
			.op_flags = op_flags,
			.old_blkaddr = NULL_ADDR,
			.page = page,
			.encrypted_page = NULL,
			.need_lock = LOCK_REQ,
			.io_type = FS_GC_DATA_IO,
		};
		bool is_dirty = PageDirty(page);
#ifdef GC_PAGE_POOL
		if (page_pool_used && is_dirty) {
			printk("%s: pool page is dirty. unexpected!!!!!!!!!!!!!!!!!!!", __func__);
		}
#endif

retry:
#ifdef GC_PAGE_POOL
		//if (!page_pool_used){
		    //printk("%s: wait_on_page_writeback page: %p idx: %lu", __func__, page, page->index);	
		f2fs_wait_on_page_writeback(page, DATA, true, true);
		//}
#else
		f2fs_wait_on_page_writeback(page, DATA, true, true);
#endif

//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_wait_wb_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif

#ifdef GC_PAGE_POOL
		if (!page_pool_used) {
			set_page_dirty(page);
			//SetPageDirty(page);
			if (clear_page_dirty_for_io(page)) {
				inode_dec_dirty_pages(inode);
				f2fs_remove_dirty_inode(inode);
			}
		}
#else
		set_page_dirty(page);
		//SetPageDirty(page);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			f2fs_remove_dirty_inode(inode);
		}
#endif
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_2_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif

		set_page_private_gcing(page);

#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
//		cur_gcp->bef_write_3_lat += read_end_t - read_start_t_;
		cur_gcp->bef_write_lat += read_end_t - read_start_t;
		read_start_t = read_end_t;
#endif
		//err = f2fs_do_write_data_page(&fio);
#ifdef GC_PAGE_POOL
		if (!page_pool_used) {
			err = jw_f2fs_do_write_data_page(&fio);
		} else {
			err = jw_f2fs_do_write_data_page_gc_pool(&fio);
		}
#else
		err = jw_f2fs_do_write_data_page(&fio);
#endif
		if (err) {
			printk("%s: write error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
			clear_page_private_gcing(page);
			if (err == -ENOMEM) {
				memalloc_retry_wait(GFP_NOFS);
				goto retry;
			}
			if (is_dirty)
				set_page_dirty(page);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		cur_gcp->write_lat += read_end_t - read_start_t;
#endif
	}
out:

#ifdef GC_PAGE_POOL
	if (!page_pool_used) {
#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
		f2fs_put_page(page, 0);	/* DEEP added */
#endif
		f2fs_put_page(page, 1);
	} else {
		jw_f2fs_put_page_gc_pool(page, 1);
	}
#else
	f2fs_put_page(page, 1);
#endif
	return err;
}

static int move_data_page_vanilla(struct inode *inode, block_t bidx, int gc_type,
							unsigned int segno, int off)
{
	struct page *page;
	int err = 0;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	unsigned long long read_start_t_, read_end_t_;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif

	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_end) {
			op_flags |= REQ_GC_END;
			sbi->gc_end = false;
			printk("%s: REQ_GC_END!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

	page = f2fs_get_lock_data_page_jw_gc(inode, bidx, true);
	if (IS_ERR(page))
		return PTR_ERR(page);

#ifdef GC_PROFILE_TIMESERIAL	
	read_start_t = OS_TimeGetNS_();
#endif

	if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
		err = -ENOENT;
		goto out;
	}
#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t_ = OS_TimeGetNS_();
	cur_gcp->bef_write_1_lat += read_end_t_ - read_start_t;
//	read_start_t_ = read_end_t_;
#endif

	err = f2fs_gc_pinned_control(inode, gc_type, segno);
	if (err)
		goto out;
//#ifdef GC_PROFILE_TIMESERIAL	
//	read_end_t_ = OS_TimeGetNS_();
//	cur_gcp->bef_write_2_lat += read_end_t_ - read_start_t_;
//	read_start_t_ = read_end_t_;
//#endif

	if (gc_type == BG_GC) {
		if (folio_test_writeback(page_folio(page))) {
			err = -EAGAIN;
			goto out;
		}
		set_page_dirty(page);
		set_page_private_gcing(page);
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.ino = inode->i_ino,
			.type = DATA,
			.temp = COLD,
			.op = REQ_OP_WRITE,
			.op_flags = op_flags,
			.old_blkaddr = NULL_ADDR,
			.page = page,
			.encrypted_page = NULL,
			.need_lock = LOCK_REQ,
			.io_type = FS_GC_DATA_IO,
		};
		bool is_dirty = PageDirty(page);
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_3_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif
retry:
		f2fs_wait_on_page_writeback(page, DATA, true, true);
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_4_lat += read_end_t_ - read_start_t_;
		read_start_t_ = read_end_t_;
#endif
		set_page_dirty(page);
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
		cur_gcp->bef_write_5_lat += read_end_t_ - read_start_t_;
		read_start_t_ = read_end_t_;
#endif
		//SetPageDirty(page);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			f2fs_remove_dirty_inode(inode);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
		cur_gcp->bef_write_6_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
#endif

		set_page_private_gcing(page);
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_7_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif

#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		unsigned long long lat_nsec = read_end_t - read_start_t;
		cur_gcp->bef_write_lat += lat_nsec;
		read_start_t = OS_TimeGetNS_();
#endif
		//err = f2fs_do_write_data_page(&fio);
		err = jw_f2fs_do_write_data_page(&fio);
		if (err) {
			clear_page_private_gcing(page);
			if (err == -ENOMEM) {
				memalloc_retry_wait(GFP_NOFS);
				goto retry;
			}
			if (is_dirty)
				set_page_dirty(page);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		lat_nsec = read_end_t - read_start_t;
		cur_gcp->write_lat += lat_nsec;
#endif
	}
out:

	f2fs_put_page(page, 1);
	return err;
}

#ifdef PGC
static int move_data_page_vanilla_pgc(struct inode *inode, block_t bidx, int gc_type,
							unsigned int segno, int off, 
							unsigned int pgc_idx)
{
	struct page *page;
	int err = 0;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	unsigned long long read_start_t_, read_end_t_;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_pgc_profiler[pgc_idx];
#endif

	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_end) {
			op_flags |= REQ_GC_END;
			sbi->gc_end = false;
			printk("%s: REQ_GC_END!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

	page = f2fs_get_lock_data_page_jw_pgc(inode, bidx, true, pgc_idx);
	if (IS_ERR(page))
		return PTR_ERR(page);

#ifdef GC_PROFILE_TIMESERIAL	
	read_start_t = OS_TimeGetNS_();
#endif

	if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
		err = -ENOENT;
		goto out;
	}
#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t_ = OS_TimeGetNS_();
	cur_gcp->bef_write_1_lat += read_end_t_ - read_start_t;
//	read_start_t_ = read_end_t_;
#endif

	err = f2fs_gc_pinned_control(inode, gc_type, segno);
	if (err)
		goto out;
//#ifdef GC_PROFILE_TIMESERIAL	
//	read_end_t_ = OS_TimeGetNS_();
//	cur_gcp->bef_write_2_lat += read_end_t_ - read_start_t_;
//	read_start_t_ = read_end_t_;
//#endif

	if (gc_type == BG_GC) {
		if (folio_test_writeback(page_folio(page))) {
			err = -EAGAIN;
			goto out;
		}
		set_page_dirty(page);
		set_page_private_gcing(page);
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.ino = inode->i_ino,
			.type = DATA,
			.temp = COLD,
			.op = REQ_OP_WRITE,
			.op_flags = op_flags,
			.old_blkaddr = NULL_ADDR,
			.page = page,
			.encrypted_page = NULL,
			.need_lock = LOCK_REQ,
			.io_type = FS_GC_DATA_IO,
		};
		bool is_dirty = PageDirty(page);
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_3_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif
retry:
		f2fs_wait_on_page_writeback(page, DATA, true, true);
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_4_lat += read_end_t_ - read_start_t_;
		read_start_t_ = read_end_t_;
#endif
		set_page_dirty(page);
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
		cur_gcp->bef_write_5_lat += read_end_t_ - read_start_t_;
		read_start_t_ = read_end_t_;
#endif
		//SetPageDirty(page);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			f2fs_remove_dirty_inode(inode);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t_ = OS_TimeGetNS_();
		cur_gcp->bef_write_6_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
#endif

		set_page_private_gcing(page);
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_7_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif

#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		unsigned long long lat_nsec = read_end_t - read_start_t;
		cur_gcp->bef_write_lat += lat_nsec;
		read_start_t = OS_TimeGetNS_();
#endif
		//err = f2fs_do_write_data_page(&fio);
		err = jw_f2fs_do_write_data_page_pgc(&fio, pgc_idx);
		if (err) {
			clear_page_private_gcing(page);
			if (err == -ENOMEM) {
				memalloc_retry_wait(GFP_NOFS);
				goto retry;
			}
			if (is_dirty)
				set_page_dirty(page);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		lat_nsec = read_end_t - read_start_t;
		cur_gcp->write_lat += lat_nsec;
#endif
	}
out:

	f2fs_put_page(page, 1);
	return err;
}
#endif

#ifdef JW_COPY_COMMAND
#ifdef COPY_ASYNC_META_UPDATE
int copy_map_update_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	wait_queue_head_t *q = &sbi->copy_map_update_wait_queue;
	//unsigned int wait_ms = DEF_MIGRATION_WAIT_TIME_USEC;
	//unsigned int wait_ms = 100;
	struct endio_copy_meta *ecm, *next_ecm;
	struct llist_node *tmp_list;

	//set_freezable();

	do {
		wait_event_interruptible(*q,
				kthread_should_stop() || 
				sbi->copy_map_update_wake || 
				!llist_empty(&sbi->ecm_entry_list));
		//wait_event_interruptible_timeout(*q,
		//		kthread_should_stop() || 
		//		sbi->copy_map_update_wake,
		//		msecs_to_jiffies(wait_ms));

		if (sbi->copy_map_update_wake)
			sbi->copy_map_update_wake = 0;

handle_ecm:
		ecm = NULL;
		/* read sbi->entry_list */
		//printk("%s: try lock", __func__);
		//spin_lock(&sbi->ecm_entry_list_lock);
		//printk("%s: lock acquired", __func__);
		//if (!list_empty(&sbi->ecm_entry_list)) {
		//	ecm = list_first_entry(&sbi->ecm_entry_list, struct endio_copy_meta, list);
		//	ecm = 	list_del(&ecm->list);
		//	ecm = }

		//printk("%s: try dispatch", __func__);
		//spin_unlock(&sbi->ecm_entry_list_lock);
		tmp_list = llist_del_all(&sbi->ecm_entry_list);
		tmp_list = llist_reverse_order(tmp_list);
		if (tmp_list) {
			llist_for_each_entry_safe(ecm, next_ecm, tmp_list, llnode) {
				if (ecm) {
					struct dnode_of_data dn;
					dn.data_blkaddr = ecm->old_blkaddr;
					dn.node_page = ecm->node_page;
					dn.ofs_in_node = ecm->ofs_in_node;
					dn.inode = ecm->inode;
					dn.inode_page = ecm->inode_page;
					lock_page((dn.node_page));

					f2fs_wait_on_page_writeback(dn.node_page, NODE, true, true);
					if (!__data_blkaddr_is_changed(&dn)) {
						f2fs_invalidate_data_block_async_meta_update(sbi, ecm->old_blkaddr);
						f2fs_update_data_blkaddr_for_copy_endio(&dn, ecm->new_blkaddr);
					} else{
						unsigned int segno, offset, blkaddr;
						struct seg_entry *se;
						blkaddr = ecm->old_blkaddr;
						segno = GET_SEGNO(sbi, blkaddr);
						offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);
						se = get_seg_entry(sbi, segno);
						if (check_valid_map(sbi, segno, offset) != 0){
							f2fs_invalidate_data_block_async_meta_update(sbi, ecm->old_blkaddr);
							printk("%s: invalidated!!! segno: %u secno: %u ofs: %u. vblk: %u",
								 __func__, segno, GET_SEC_FROM_SEG(sbi, segno), 
								 offset, se->valid_blocks);
						}
						printk("%s: datablk addr changed. segno: %u secno: %u vblks: %u", __func__, segno, GET_SEC_FROM_SEG(sbi, segno), se->valid_blocks);
					}
					//f2fs_update_data_blkaddr(&dn, ecm->new_blkaddr);
					
					f2fs_put_dnode(&dn);

					dec_page_count_copied(sbi);
					kmem_cache_free(endio_copy_meta_slab, ecm);

					//goto handle_ecm;
				}
			}
		}
		//printk("%s: handling done", __func__);
		if (!llist_empty(&sbi->ecm_entry_list)) {
			goto handle_ecm;
		}
		//printk("%s: list empty. go sleep", __func__);

		if (f2fs_readonly(sbi->sb)) {
			printk("%s: continue.", __func__);
			continue;
		}
		if (kthread_should_stop()){
			printk("%s: stop thread.", __func__);
			return 0;
		}
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
			printk("%s: unexpected!! SBI_NEED_FSCK", __func__);
			continue;
		}

	} while (!kthread_should_stop());
	return 0;
}
#endif

static void f2fs_submit_copy_endio(struct bio *bio)
{
	struct f2fs_sb_info *sbi = (struct f2fs_sb_info *)bio->bi_private;
	
	dec_page_count_copied(sbi);
	bio_put(bio);
}

#ifdef COPY_ASYNC_META_UPDATE
static void f2fs_submit_copy_endio_async_meta_update(struct bio *bio)
{
	struct endio_copy_meta *ecm = (struct endio_copy_meta *)bio->bi_private;
	struct f2fs_sb_info *sbi = ecm->sbi;
	//struct f2fs_sb_info *sbi = (struct f2fs_sb_info *)bio->bi_private;
	
	/*
	struct dnode_of_data dn;
	dn.data_blkaddr = ecm->old_blkaddr;
	dn.node_page = ecm->node_page;
	dn.ofs_in_node = ecm->ofs_in_node;
	dn.inode = ecm->inode;
	dn.inode_page = ecm->inode_page;
	lock_page((dn.node_page));

	f2fs_update_data_blkaddr_for_copy_endio(&dn, ecm->new_blkaddr);
	//f2fs_update_data_blkaddr(&dn, ecm->new_blkaddr);
	
	f2fs_put_dnode(&dn);

	dec_page_count_copied(sbi);
	kmem_cache_free(endio_copy_meta_slab, ecm);
	
	*/
	//printk("%s: try lock", __func__);
	//spin_lock(&sbi->ecm_entry_list_lock);
	//printk("%s: lock acquired", __func__);
	//list_add_tail(&ecm->list, &sbi->ecm_entry_list);
	llist_add(&ecm->llnode, &sbi->ecm_entry_list);
	//printk("%s: try unlock", __func__);
	//spin_unlock(&sbi->ecm_entry_list_lock);
	//printk("%s: unlock done", __func__);
	sbi->copy_map_update_wake = 1;
	wake_up_interruptible_all(&sbi->copy_map_update_wait_queue);
	//printk("%s get_page_copied: %u", __func__, (unsigned int ) get_page_copied(sbi));
	bio_put(bio);
}
#endif

#ifdef COPY_ASYNC_META_UPDATE
static struct bio* copy_data_block_async_meta_update(struct inode *inode, block_t start_bidx, 
							unsigned int segno, int off, 
							struct bio *prev_bio, bool *is_end_zone_addr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	*is_end_zone_addr = false;
	struct f2fs_summary sum;
	struct node_info ni;
	struct dnode_of_data dn;
	int err = 0;
	int type = CURSEG_COLD_DATA;
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.op = REQ_OP_COPY,
		.op_flags = 0,
		.page = NULL,
		.encrypted_page = NULL,
		//.op_flags = REQ_SYNC,
		//.temp = COLD,
		//.io_type = FS_GC_DATA_IO,
		//.ino = inode->i_ino,
		//.old_blkaddr = NULL_ADDR,
	};
	block_t saddr, eaddr;
	
	bool lfs_mode = f2fs_lfs_mode(sbi);

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
	{
		return NULL;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, start_bidx, LOOKUP_NODE);
	BUG_ON(err);

	fio.old_blkaddr = dn.data_blkaddr;

	/* This page is already truncated */
	if (fio.old_blkaddr == NULL_ADDR) {
		f2fs_put_dnode(&dn);
		return NULL;
	}

	err = f2fs_get_node_info(sbi, dn.nid, &ni, false);
	if (err)
		goto put_out;
	
	if (lfs_mode)
		f2fs_down_write(&fio.sbi->io_order_lock);
	
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	//err = f2fs_allocate_data_block(fio.sbi, NULL, fio.old_blkaddr, &(fio.new_blkaddr),
	//			&sum, type, NULL);
	err = f2fs_allocate_data_block_async_meta_update(fio.sbi, NULL, fio.old_blkaddr, &(fio.new_blkaddr),
				&sum, type, NULL);
	if (err) {
		goto up_out;
	}
	
	if (GET_SEGNO(sbi, fio.old_blkaddr) != NULL_SEGNO)
		f2fs_invalidate_internal_cache(sbi, fio.old_blkaddr);
	

	struct bio *bio = NULL;
	static struct block_device *past_bdev = NULL;
	//struct block_device *bdev = f2fs_target_device(sbi, fio.old_blkaddr, NULL);
	sector_t s_sector, e_sector;
	struct block_device *s_bdev = f2fs_target_device(sbi, fio.old_blkaddr, &s_sector);
	struct block_device *e_bdev = f2fs_target_device(sbi, fio.new_blkaddr, &e_sector);
	if (s_bdev != e_bdev) {
		printk("%s: old_blkaddr: 0x%llx src segno: %u (real_segno: %u) src secno: %u (read_secno: %u) segoff_in_sec: %u blkoff_in_seg: %u inode: %p start_bidx: %u", 
				__func__, 
				fio.old_blkaddr, GET_SEGNO(sbi, fio.old_blkaddr), segno,  
				GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.old_blkaddr))), 
				GET_SEC_FROM_SEG(sbi, segno), 
				GET_SEGNO(sbi, fio.old_blkaddr)	- 
					GET_SEG_FROM_SEC(sbi, (GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.old_blkaddr))))), 
					fio.old_blkaddr - START_BLOCK(sbi, GET_SEGNO(sbi, fio.old_blkaddr)), 
					inode, start_bidx
		);
		printk("%s: new_blkaddr: 0x%llx dst segno: %u dst secno: %u segoff_in_sec: %u blkoff_in_seg: %u inode: %p start_bidx: %u", 
				__func__, 
				fio.new_blkaddr, GET_SEGNO(sbi, fio.new_blkaddr), 
				GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.new_blkaddr))), 
				GET_SEGNO(sbi, fio.new_blkaddr)	- 
					GET_SEG_FROM_SEC(sbi, (GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.new_blkaddr))))), 
					fio.new_blkaddr - START_BLOCK(sbi, GET_SEGNO(sbi, fio.new_blkaddr)), 
					inode, start_bidx
		);
		printk("%s: old_blkaddr: 0x%lx new_blkaddr: 0x%lx", 
		__func__, fio.old_blkaddr, fio.new_blkaddr);
		f2fs_target_device_dbg(sbi, fio.old_blkaddr, &s_sector);
		f2fs_target_device_dbg(sbi, fio.new_blkaddr, &e_sector);
		f2fs_bug_on(sbi, 1);
		while(1);
	}

	
	saddr = fio.old_blkaddr;
	eaddr = fio.new_blkaddr;

	if (is_end_zone_blkaddr(sbi, fio.new_blkaddr)) {
		*is_end_zone_addr = true;
	}
	
	//if (f2fs_is_multi_device(sbi)) {
	//	//int devi = f2fs_target_device_index(sbi, fio.old_blkaddr);
	//	int devi = f2fs_target_device_index(sbi, fio.new_blkaddr);
	//	//printk("%s: multi device", __func__);
	//	saddr -= FDEV(devi).start_blk;
	//	eaddr -= FDEV(devi).start_blk;
	//}
	
	//if ( (bdev != past_bdev ||  f2fs_target_device(sbi, fio.old_blkaddr, NULL) != bdev) && past_bdev != NULL) {
	//	printk("%s: bdev: %p past_bdev: %p", __func__, bdev, past_bdev);
	//	printk("%s: WARN!!! s_sector: %lx d_sector: %lx saddr: 0x%x eaddr: 0x%x ori_saddr: 0x%x ori_eaddr: 0x%x", 
	//			__func__, 
	//			SECTOR_FROM_BLOCK(saddr),
	//			SECTOR_FROM_BLOCK(eaddr),
	//			saddr, eaddr, 
	//			fio.old_blkaddr, fio.new_blkaddr
	//			);
	//	past_bdev = bdev;
	//}
	err = __blkdev_issue_copy(e_bdev,
			s_sector,
			e_sector,
			GFP_NOFS, &bio);
	
	//printk("%s: s_sector: %lx d_sector: %lx saddr: 0x%x eaddr: 0x%x ori_saddr: 0x%x ori_eaddr: 0x%x", 
	//		__func__, 
	//		s_sector,
	//		e_sector,
	//		saddr, eaddr, 
	//		fio.old_blkaddr, fio.new_blkaddr
	//		);
	
	if (err) {
		printk("[JWDBG] %s __blkdev_issue_copy failed %d ", 
				__func__, err);
		f2fs_bug_on(sbi, 1);
	}
	inc_page_count_copied(sbi);
	//printk("%s get_page_copied: %u", __func__, (unsigned int ) get_page_copied(sbi));
	
	f2fs_bug_on(sbi, !bio);
	
	struct endio_copy_meta *ecm = f2fs_kmem_cache_alloc(endio_copy_meta_slab,
					GFP_NOFS, true, NULL);
	f2fs_bug_on(sbi, !ecm);
	ecm->sbi = sbi;
	ecm->new_blkaddr = fio.new_blkaddr;	
	ecm->old_blkaddr = dn.data_blkaddr;
	ecm->node_page = dn.node_page;
	ecm->ofs_in_node = dn.ofs_in_node;
	ecm->inode = dn.inode;
	ecm->inode_page = dn.inode_page;
	//INIT_LIST_HEAD(&ecm->list);
	//printk("%s: register endio handler: old: %u new: %u inode: %u node_page: %u", __func__, 
	//	ecm->old_blkaddr, ecm->new_blkaddr, ecm->inode, ecm->node_page);
	
	//bio->bi_private = sbi;
	bio->bi_private = ecm;
	bio->bi_end_io = f2fs_submit_copy_endio_async_meta_update;
	bio->bi_opf |= REQ_SYNC & REQ_NOMERGE;
	//bio->bi_opf = 0;
	//printk("%s [JWDBG] submit bio for copy command", __func__);
	if (bio->bi_next != NULL) {
		printk("%s: bi_next is not NULL!!", __func__);

	}
	if (prev_bio != NULL) {
		prev_bio->bi_next = bio;
	}
	//submit_bio(bio);
	//printk("%s [JWDBG] submit bio DONE for copy command", __func__);
	
	//f2fs_update_data_blkaddr(&dn, fio.new_blkaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);
	//if (start_bidx == 0)
	//	set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
up_out:
	if (lfs_mode)
		f2fs_up_write(&fio.sbi->io_order_lock);
put_out:
	//f2fs_put_dnode(&dn);
	f2fs_bug_on(F2FS_P_SB((dn.node_page)), !PageLocked((dn.node_page)));
	unlock_page((dn.node_page));

	return bio;
}
#endif

static struct bio* copy_data_block(struct inode *inode, block_t start_bidx, 
							unsigned int segno, int off, 
							struct bio *prev_bio, bool *is_end_zone_addr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	*is_end_zone_addr = false;
	struct f2fs_summary sum;
	struct node_info ni;
	struct dnode_of_data dn;
	int err = 0;
	int type = CURSEG_COLD_DATA;
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.op = REQ_OP_COPY,
		.op_flags = 0,
		.page = NULL,
		.encrypted_page = NULL,
		//.op_flags = REQ_SYNC,
		//.temp = COLD,
		//.io_type = FS_GC_DATA_IO,
		//.ino = inode->i_ino,
		//.old_blkaddr = NULL_ADDR,
	};
	block_t saddr, eaddr;

#ifdef GC_PROFILE_TIMESERIAL
	struct gc_profiler_snapshot *cur_gcp = fio.sbi->cur_gc_profiler;
	unsigned long long read_start_t, read_end_t;
	//read_start_t = OS_TimeGetNS_();
	read_start_t = OS_TimeGetNS_();
#endif
	
	bool lfs_mode = f2fs_lfs_mode(sbi);

	if (!check_valid_map(F2FS_I_SB(inode), segno, off))
	{
		return NULL;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
#ifdef GC_PROFILE_TIMESERIAL	
	read_start_t = OS_TimeGetNS_();
#endif
	err = f2fs_get_dnode_of_data(&dn, start_bidx, LOOKUP_NODE);
	BUG_ON(err);
#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t = OS_TimeGetNS_();
	cur_gcp->fmap_write_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif

	fio.old_blkaddr = dn.data_blkaddr;

	/* This page is already truncated */
	if (fio.old_blkaddr == NULL_ADDR) {
		f2fs_put_dnode(&dn);
		return NULL;
	}

	err = f2fs_get_node_info(sbi, dn.nid, &ni, false);
	if (err)
		goto put_out;

#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->write_get_node_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif
	
	if (lfs_mode)
		f2fs_down_write(&fio.sbi->io_order_lock);
	
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->____acquire_io_order_lock_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif

	err = f2fs_allocate_data_block(fio.sbi, NULL, fio.old_blkaddr, &(fio.new_blkaddr),
				&sum, type, NULL);
	if (err) {
		goto up_out;
	}
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->____allocate_data_block_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif
	
	if (GET_SEGNO(sbi, fio.old_blkaddr) != NULL_SEGNO)
		f2fs_invalidate_internal_cache(sbi, fio.old_blkaddr);
	
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->____invalidate_internal_cache_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif

	struct bio *bio = NULL;
	static struct block_device *past_bdev = NULL;
	//struct block_device *bdev = f2fs_target_device(sbi, fio.old_blkaddr, NULL);
	sector_t s_sector, e_sector;
	struct block_device *s_bdev = f2fs_target_device(sbi, fio.old_blkaddr, &s_sector);
	struct block_device *e_bdev = f2fs_target_device(sbi, fio.new_blkaddr, &e_sector);
	if (s_bdev != e_bdev) {
		printk("%s: old_blkaddr: 0x%llx src segno: %u (real_segno: %u) src secno: %u (read_secno: %u) segoff_in_sec: %u blkoff_in_seg: %u inode: %p start_bidx: %u", 
				__func__, 
				fio.old_blkaddr, GET_SEGNO(sbi, fio.old_blkaddr), segno,  
				GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.old_blkaddr))), 
				GET_SEC_FROM_SEG(sbi, segno), 
				GET_SEGNO(sbi, fio.old_blkaddr)	- 
					GET_SEG_FROM_SEC(sbi, (GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.old_blkaddr))))), 
					fio.old_blkaddr - START_BLOCK(sbi, GET_SEGNO(sbi, fio.old_blkaddr)), 
					inode, start_bidx
		);
		printk("%s: new_blkaddr: 0x%llx dst segno: %u dst secno: %u segoff_in_sec: %u blkoff_in_seg: %u inode: %p start_bidx: %u", 
				__func__, 
				fio.new_blkaddr, GET_SEGNO(sbi, fio.new_blkaddr), 
				GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.new_blkaddr))), 
				GET_SEGNO(sbi, fio.new_blkaddr)	- 
					GET_SEG_FROM_SEC(sbi, (GET_SEC_FROM_SEG(sbi, (GET_SEGNO(sbi, fio.new_blkaddr))))), 
					fio.new_blkaddr - START_BLOCK(sbi, GET_SEGNO(sbi, fio.new_blkaddr)), 
					inode, start_bidx
		);
		printk("%s: old_blkaddr: 0x%lx new_blkaddr: 0x%lx", 
		__func__, fio.old_blkaddr, fio.new_blkaddr);
		f2fs_target_device_dbg(sbi, fio.old_blkaddr, &s_sector);
		f2fs_target_device_dbg(sbi, fio.new_blkaddr, &e_sector);
		f2fs_bug_on(sbi, 1);
		while(1);
	}

	
	saddr = fio.old_blkaddr;
	eaddr = fio.new_blkaddr;

	if (is_end_zone_blkaddr(sbi, fio.new_blkaddr)) {
		*is_end_zone_addr = true;
	}
	
	//if (f2fs_is_multi_device(sbi)) {
	//	//int devi = f2fs_target_device_index(sbi, fio.old_blkaddr);
	//	int devi = f2fs_target_device_index(sbi, fio.new_blkaddr);
	//	//printk("%s: multi device", __func__);
	//	saddr -= FDEV(devi).start_blk;
	//	eaddr -= FDEV(devi).start_blk;
	//}
	
	//if ( (bdev != past_bdev ||  f2fs_target_device(sbi, fio.old_blkaddr, NULL) != bdev) && past_bdev != NULL) {
	//	printk("%s: bdev: %p past_bdev: %p", __func__, bdev, past_bdev);
	//	printk("%s: WARN!!! s_sector: %lx d_sector: %lx saddr: 0x%x eaddr: 0x%x ori_saddr: 0x%x ori_eaddr: 0x%x", 
	//			__func__, 
	//			SECTOR_FROM_BLOCK(saddr),
	//			SECTOR_FROM_BLOCK(eaddr),
	//			saddr, eaddr, 
	//			fio.old_blkaddr, fio.new_blkaddr
	//			);
	//	past_bdev = bdev;
	//}
	err = __blkdev_issue_copy(e_bdev,
			s_sector,
			e_sector,
			GFP_NOFS, &bio);
	
	//printk("%s: s_sector: %lx d_sector: %lx saddr: 0x%x eaddr: 0x%x ori_saddr: 0x%x ori_eaddr: 0x%x", 
	//		__func__, 
	//		s_sector,
	//		e_sector,
	//		saddr, eaddr, 
	//		fio.old_blkaddr, fio.new_blkaddr
	//		);
	
	if (err) {
		printk("[JWDBG] %s __blkdev_issue_copy failed %d ", 
				__func__, err);
		f2fs_bug_on(sbi, 1);
	}
	inc_page_count_copied(sbi);
	//printk("%s get_page_copied: %u", __func__, (unsigned int ) get_page_copied(sbi));
	
	f2fs_bug_on(sbi, !bio);
	
	bio->bi_private = sbi;
	bio->bi_end_io = f2fs_submit_copy_endio;
	bio->bi_opf |= REQ_SYNC & REQ_NOMERGE;
	//bio->bi_opf = 0;
	//printk("%s [JWDBG] submit bio for copy command", __func__);
	if (bio->bi_next != NULL) {
		printk("%s: bi_next is not NULL!!", __func__);

	}
	if (prev_bio != NULL) {
		prev_bio->bi_next = bio;
	}
	//submit_bio(bio);
	//printk("%s [JWDBG] submit bio DONE for copy command", __func__);
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->create_copy_bio_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
#endif
	
	f2fs_update_data_blkaddr(&dn, fio.new_blkaddr);
	set_inode_flag(inode, FI_APPEND_WRITE);
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->__update_data_blkaddr_lat += read_end_t - read_start_t;
#endif
	//if (start_bidx == 0)
	//	set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
up_out:
	if (lfs_mode)
		f2fs_up_write(&fio.sbi->io_order_lock);
put_out:
	f2fs_put_dnode(&dn);

	return bio;
}

static int gc_data_segment_copy(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type, int *copy_submitted, 
		bool force_migrate)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	int copy_cnt = 0, clean_pg_cnt = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	int phase_4_start = 1;
	struct blk_plug plug;
	struct bio *prev_bio = NULL, *cur_bio = NULL, *first_bio = NULL;
	bool prev_was_normal_write = false;
	start_addr = START_BLOCK(sbi, segno);
		
	static int jw_cnt  = 0;


	//printk("%s: copy start!!! sector_t size: %llu unsigned int size: %llu", 
	//		__func__, sizeof(sector_t), sizeof(unsigned));
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0,  
						p3_is_alive_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p4_move_lat = 0; 
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	read_start_t = OS_TimeGetNS_();
#endif
next_step:
	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif
	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		if (phase == 2) {
			f2fs_ra_node_page(sbi, dni.ino);
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			int err;
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode) || is_bad_inode(inode) || 
					special_file(inode->i_mode)) {
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}
			
			if (!f2fs_down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				iput(inode);
				sbi->skipped_gc_rwsem++;
				//printk("%s: SKIP 4 !!", __func__);
				continue;
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					//printk("%s: SKIP 5 !!", __func__);
					continue;
				}
				add_gc_inode(gc_list, inode);
				//printk("%s: SKIP 6 !!", __func__);
				continue;
			}
#ifndef JW_COPY_COMMAND
			data_page = f2fs_get_read_gc_data_page(inode, start_bidx, 
						REQ_RAHEAD, true, NULL);
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				continue;
			}

			f2fs_put_page(data_page, 0);
#else
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
#endif
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;

			if (S_ISREG(inode->i_mode)) {
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
					sbi->skipped_gc_rwsem++;
					//printk("%s: SKIP _2 !!", __func__);
					continue;
				}
				if (!f2fs_down_write_trylock(
						&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP _3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef JW_COPY_COMMAND
			//if (data_page = jw_f2fs_try_grab_cache_page(sbi, 
			//		inode->i_mapping, start_bidx, true)) {
			////if (data_page = find_get_page(inode->i_mapping, start_bidx)) {
			//	if(!PageDirty(data_page)) {
			//		clean_pg_cnt ++;
			//	}
			//	if (PageUptodate(data_page)) {
			//		unlock_page(data_page);
			//	}
			//		
			//	/* flush copy command due to the write ordering of ZNS */
			//	if (first_bio != NULL) {
			//		printk("%s: submit bio! merge_bio_cnt: %llu", __func__, first_bio->bi_iter.merge_bio_cnt);
			//		submit_bio(first_bio);
			//		first_bio = NULL;
			//		cur_bio = NULL;
			//	}

			//	//if(PageDirty(data_page)) {
			//		/* need to do host GC */
			//		if (f2fs_post_read_required(inode))
			//			err = move_data_block(inode, start_bidx,
			//						gc_type, segno, off);
			//		else
			//			err = move_data_page_vanilla(inode, start_bidx, gc_type,
			//							segno, off);

			//		if (err){
			//			printk("[JWDBG] %s something wrong! %d", 
			//					__func__, err);
			//			f2fs_bug_on(sbi, 1);
			//		}
			//		prev_was_normal_write = true;

			//		if (!err && (gc_type == FG_GC ||
			//				f2fs_post_read_required(inode)))
			//			submitted++;
			//		//printk("%s: move_data_page, err: %d", __func__, err);
			//		
			//		
			//		goto end_phase4;
			////	} else {
			////		clean_pg_cnt ++;
			////	}
			//}
			
			if (prev_was_normal_write == true) {
				//printk("%s: submit merged nornal 1", __func__);
				f2fs_submit_merged_write(sbi, DATA);
				prev_was_normal_write = false;
			}
			
			if (phase_4_start) {
				phase_4_start = 0;
				blk_start_plug(&plug);
			}
			/* copy command */
			//printk("%s: copy_data_block start", __func__);
			bool is_end_zone_addr;
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
#ifdef COPY_ASYNC_META_UPDATE
			cur_bio = copy_data_block_async_meta_update(inode, start_bidx, segno, off, prev_bio, &is_end_zone_addr);
#else
			cur_bio = copy_data_block(inode, start_bidx, segno, off, prev_bio, &is_end_zone_addr);
#endif
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			cur_gcp->copy_data_block_lat += tmp_end_t - tmp_start_t;
#endif
			//printk("%s: %d th copy_data_block done", __func__, copy_cnt);
			f2fs_bug_on(sbi, cur_bio == NULL);
			*copy_submitted = *copy_submitted + 1;
			if (first_bio == NULL) {
				first_bio = cur_bio;
				first_bio->bi_iter.merge_bio_cnt = 1;
				//printk("%s: first bio ! bio: %p", __func__, cur_bio);
			} else {
				first_bio->bi_iter.merge_bio_cnt ++;
				//printk("%s: merge_bio_cnt: %llu bio: %p", __func__, 
				//		first_bio->bi_iter.merge_bio_cnt, cur_bio);
			}
			
			if (first_bio->bi_iter.merge_bio_cnt == 32 || is_end_zone_addr) {
				jw_cnt ++;
				//if (jw_cnt % 100 == 0) {
					//printk("%s: submit bio! 2. merge_bio_cnt: %llu jw_cnt: %llu is_end_zone_addr: %d", 
					//			__func__, first_bio->bi_iter.merge_bio_cnt, jw_cnt, 
					//			is_end_zone_addr);
				//}
#ifdef GC_PROFILE_TIMESERIAL
				tmp_start_t = OS_TimeGetNS_(); 
#endif
				submit_bio(first_bio);
#ifdef GC_PROFILE_TIMESERIAL
				tmp_end_t = OS_TimeGetNS_(); 
				cur_gcp->copy_submit_bio_lat += tmp_end_t - tmp_start_t;
#endif
				first_bio = NULL;
				cur_bio = NULL;
			}
			
			prev_bio = cur_bio;
			copy_cnt ++;
			//printk("%s: copy_data_block end", __func__);
			

end_phase4:
			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
#else

			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page(inode, start_bidx, gc_type,
								segno, off);
			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;

			if (locked) {
				up_write(&fi->i_gc_rwsem[WRITE]);
				up_write(&fi->i_gc_rwsem[READ]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
#endif
		} else {
			printk("%s: no inode! ", __func__);
		}
	}
#ifdef GC_PROFILE_TIMESERIAL
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;
	
	if (first_bio != NULL) {
		//printk("%s: submit bio! last! merge_bio_cnt: %llu", __func__, 
		//		first_bio->bi_iter.merge_bio_cnt);
		jw_cnt ++;
		//if (jw_cnt % 100 == 0) {
		//	printk("%s: submit bio 2! merge_bio_cnt: %llu jw_cnt: %llu", 
		//				__func__, first_bio->bi_iter.merge_bio_cnt, jw_cnt);
		//}
#ifdef GC_PROFILE_TIMESERIAL
		tmp_start_t = OS_TimeGetNS_(); 
#endif
		submit_bio(first_bio);
#ifdef GC_PROFILE_TIMESERIAL
		tmp_end_t = OS_TimeGetNS_(); 
		cur_gcp->copy_submit_bio_lat += tmp_end_t - tmp_start_t;
#endif
	}
	if (prev_was_normal_write == true) {
		//printk("%s: submit last merged nornal 1", __func__);
		f2fs_submit_merged_write(sbi, DATA);
	}
	if (!phase_4_start)
		blk_finish_plug(&plug);
	//printk("%s: copy_cnt: %u submitted: %u clean_pg_cnt: %u", __func__, 
	//		copy_cnt, submitted, clean_pg_cnt);
#ifdef GC_PROFILE_TIMESERIAL
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
	cur_gcp->gc_p4_lat += p_lat_t[4];

	//cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	//cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	//cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	//cur_gcp->p3_check1_lat += p3_check1_lat;
	//cur_gcp->p3_check2_lat += p3_check2_lat;
	//cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	//cur_gcp->p3_iget_lat += p3_iget_lat;
	//cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	//cur_gcp->p3_read_lat += p3_read_lat;
	//cur_gcp->p3_put_page_lat = p3_put_page_lat;
	//cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	//cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}
#endif

#ifdef GC_PREREAD
/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static int preread_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	//if (fggc && phase == 2)
	//	atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			continue;
		}
	}

	if (++phase < 2)
		goto next_step;

	return submitted;
}

#ifdef GC_DEEP_PIPELINE_BATCHED 
static int preread_node_segment_first_step(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type, unsigned batch_ofs)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);
	
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;

next_step:
	entry = sum;

	//if (fggc && phase == 2)
	//	atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			
			/* BLOCK_GRANULARITY add */
			sbi->pipelined_gc_read_second_step_wake = true;
			atomic_set(&pgci->preread_blk_seq, 
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
			smp_mb();
			wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
			//printk("%s: preread_blk_seq: %llu . wake writer", __func__, 
			//		atomic_read(&pgci->preread_blk_seq));
			/* BLOCK_GRANULARITY add end */
			continue;
		}
	}

	if (++phase < 2)
		goto next_step;
	
	/* BLOCK_GRANULARITY add */
	/* block-granularity pipelining. let reader-second-step know block read done. */
	sbi->pipelined_gc_read_second_step_wake = true;
	atomic_set(&pgci->preread_blk_seq, 
		batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
	smp_mb();
	wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
	//printk("%s: preread_blk_seq: %llu . wake writer again. last seg. ", __func__, 
	//		atomic_read(&pgci->preread_blk_seq));
	/* BLOCK_GRANULARITY add end */

	return submitted;
}

static int clean_node_segment_first_step(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type, unsigned batch_ofs)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 2;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);
	/* BLOCK_GRANULARITY added */
	unsigned int target_blk_seq;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;

next_step:
	target_blk_seq = batch_ofs * BLKS_PER_SEG(sbi);
	entry = sum;

	if (fggc && phase == 2)
		atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* BLOCK_GRANULARITY added */
		target_blk_seq ++;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		//if (phase == 0) {
		//	f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
		//					META_NAT, true);
		//	continue;
		//}

		//if (phase == 1) {
		//	f2fs_ra_node_page(sbi, nid);
		//	continue;
		//}

		/* BLOCK_GRANULARITY added */
		/* Block-granularity pipelining. wait for reader block completion. */
		if (atomic_read(&sbi->pipe_gc_i->preread_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_wake && 
					atomic_read(&sbi->pipe_gc_i->preread_blk_seq) >= target_blk_seq));
		}
		//printk("%s: target_blk_seq: %llu preread_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->preread_blk_seq));

		/* phase == 2 */
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during f2fs_get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (f2fs_get_node_info(sbi, nid, &ni, false)) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		err = f2fs_move_node_page(node_page, gc_type);
		if (!err && gc_type == FG_GC)
			submitted++;
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}

	if (++phase < 3)
		goto next_step;

	if (fggc)
		atomic_dec(&sbi->wb_sync_req[NODE]);
	return submitted;
}
#endif

#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
static int preread_node_segment_blk_gra(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type, unsigned batch_ofs)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);
	
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;

next_step:
	entry = sum;

	//if (fggc && phase == 2)
	//	atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
			f2fs_ra_node_page(sbi, nid);
			
			/* BLOCK_GRANULARITY add */
			sbi->pipelined_gc_write_wake = true;
			atomic_set(&pgci->preread_blk_seq, 
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
			//printk("%s: preread_blk_seq: %llu . wake writer", __func__, 
			//		atomic_read(&pgci->preread_blk_seq));
			/* BLOCK_GRANULARITY add end */
			continue;
		}
	}

	if (++phase < 2)
		goto next_step;
	
	/* BLOCK_GRANULARITY add */
	/* block-granularity pipelining. let writer know block read done. */
	sbi->pipelined_gc_write_wake = true;
	atomic_set(&pgci->preread_blk_seq, 
		batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
	smp_mb();
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	//printk("%s: preread_blk_seq: %llu . wake writer again. last seg. ", __func__, 
	//		atomic_read(&pgci->preread_blk_seq));
	/* BLOCK_GRANULARITY add end */

	return submitted;
}

static int clean_node_segment_blk_gra(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type, unsigned batch_ofs)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 2;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);
	/* BLOCK_GRANULARITY added */
	unsigned int target_blk_seq;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;

next_step:
	target_blk_seq = batch_ofs * BLKS_PER_SEG(sbi);
	entry = sum;

	if (fggc && phase == 2)
		atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* BLOCK_GRANULARITY added */
		target_blk_seq ++;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		//if (phase == 0) {
		//	f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
		//					META_NAT, true);
		//	continue;
		//}

		//if (phase == 1) {
		//	f2fs_ra_node_page(sbi, nid);
		//	continue;
		//}

		/* BLOCK_GRANULARITY added */
		/* Block-granularity pipelining. wait for reader block completion. */
		if (atomic_read(&sbi->pipe_gc_i->preread_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_wake && 
					atomic_read(&sbi->pipe_gc_i->preread_blk_seq) >= target_blk_seq));
		}
		//printk("%s: target_blk_seq: %llu preread_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->preread_blk_seq));

		/* phase == 2 */
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during f2fs_get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (f2fs_get_node_info(sbi, nid, &ni, false)) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		err = f2fs_move_node_page(node_page, gc_type);
		if (!err && gc_type == FG_GC)
			submitted++;
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}

	if (++phase < 3)
		goto next_step;

	if (fggc)
		atomic_dec(&sbi->wb_sync_req[NODE]);
	return submitted;
}
#endif

/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static int clean_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 2;
	bool fggc = (gc_type == FG_GC);
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	if (fggc && phase == 2)
		atomic_inc(&sbi->wb_sync_req[NODE]);

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;
		struct node_info ni;
		int err;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0))
			return submitted;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		//if (phase == 0) {
		//	f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
		//					META_NAT, true);
		//	continue;
		//}

		//if (phase == 1) {
		//	f2fs_ra_node_page(sbi, nid);
		//	continue;
		//}
		
		/* phase == 2 */
		node_page = f2fs_get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* block may become invalid during f2fs_get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (f2fs_get_node_info(sbi, nid, &ni, false)) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}

		err = f2fs_move_node_page(node_page, gc_type);
		if (!err && gc_type == FG_GC)
			submitted++;
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}

	if (++phase < 3)
		goto next_step;

	if (fggc)
		atomic_dec(&sbi->wb_sync_req[NODE]);
	return submitted;
}

#ifdef GC_DEEP_PIPELINE_BATCHED
static int preread_data_segment_first_step(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned int batch_ofs)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p3_is_alive_lat = 0, 
						gc_p3_step1_write_buf_lat = 0; 
	unsigned long long out_phase3 = 0, in_phase3 = 0, 
				  phase3_start_t, phase3_end_t;
#endif
	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

//#ifdef GC_PROFILE_TIMESERIAL
//	read_start_t = OS_TimeGetNS_();
//#endif
#ifdef GC_PAGE_POOL
	bool page_pool_used;
#endif
	start_addr = START_BLOCK(sbi, segno);
	
	unsigned int read_pass_buffer_head = atomic_read(&pgci->reader_pass_buffer_head);
	unsigned int next_read_pass_buffer_head;
	struct reader_pass_entry *read_pass_entry;
	bool is_first = false;

next_step:

	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL
	sbi->intra_seg_blkseq_of_prereader = -1;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			phase3_start_t = OS_TimeGetNS_();
			tmp_start_t = phase3_start_t; 
		}
#endif
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif

#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_prereader ++;
#endif
		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif
		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_is_alive_lat += tmp_end_t - tmp_start_t;
#endif

		//if (phase == 2) {
		//	f2fs_ra_node_page(sbi, dni.ino);
		//	continue;
		//}
		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			out_phase3 += phase3_end_t - phase3_start_t;
			phase3_start_t = phase3_end_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			int err;
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				//printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			/* Prevent a head pass a tail. */
			next_read_pass_buffer_head = (read_pass_buffer_head + 1) % N_READ_PASS_ENTRY;
			//is_first = true;
			while(next_read_pass_buffer_head == atomic_read(&pgci->reader_pass_buffer_tail)){
				/* FIX (soft lockup): reader_pass ring full — yield instead of
				 * naked-spinning so this CPU is not pinned (watchdog soft
				 * lockup). Loop re-reads the tail, so it proceeds as soon as
				 * reading_second_step frees a slot. */
				cond_resched();
				//if (is_first) {
				//	printk("%s: waiting. next_buf_head: %lu buf_tail: %lu", __func__, 
				//			next_read_pass_buffer_head, atomic_read(&pgci->reader_pass_buffer_tail));
				//	is_first = false;
				//}
			}
			//if (!is_first) {
			//	printk("%s: waiting done. next_buf_head: %lu buf_tail: %lu", __func__, 
			//			next_read_pass_buffer_head, atomic_read(&pgci->reader_pass_buffer_tail));
			//}


			/* fill the read pass buffer */
			read_pass_entry = &pgci->reader_pass_buffer[read_pass_buffer_head];
			read_pass_entry->nofs = nofs;
			read_pass_entry->ofs_in_node = ofs_in_node;
			read_pass_entry->inode = inode;
			read_pass_entry->blkaddr = start_addr + off;
			//printk("%s: head: %lu segno: %lu blkaddr: 0x%lx", __func__, 
			//		read_pass_buffer_head, segno, read_pass_entry->blkaddr);
			
			/* update reader_pass_buffer_head */
			read_pass_buffer_head = next_read_pass_buffer_head;

			smp_mb();
			atomic_set(&pgci->reader_pass_buffer_head, read_pass_buffer_head);
			
			/* BLOCK_GRANULARITY add */
			/* block-granularity pipelining. 
			let reader-second-step know block read done. */
			sbi->pipelined_gc_read_second_step_wake = true;
			atomic_set(&pgci->preread_blk_seq, 
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
			smp_mb();
			wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
			//printk("%s: preread_blk_seq: %llu . wake 2nd reader", __func__, 
			//		atomic_read(&pgci->preread_blk_seq));
			/* BLOCK_GRANULARITY add end */
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			gc_p3_step1_write_buf_lat += phase3_end_t - tmp_start_t; 
			in_phase3 += phase3_end_t - phase3_start_t;
#endif 
			continue; 
		}

	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 4)
		goto next_step;
	
	/* BLOCK_GRANULARITY add */
	/* block-granularity pipelining. let reader-second-step know block read done. */
	sbi->pipelined_gc_read_second_step_wake = true;
	atomic_set(&pgci->preread_blk_seq, 
		batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
	smp_mb();
	wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
	//printk("%s: preread_blk_seq: %llu . wake 2nd reader again. last seg. ", __func__, 
	//		atomic_read(&pgci->preread_blk_seq));
	/* BLOCK_GRANULARITY add end */

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
#endif
#ifdef GC_PROFILE_TIMESERIAL	

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->gc_p3_step1_write_buf_lat += gc_p3_step1_write_buf_lat;
	cur_gcp->in_phase3 += in_phase3;
	cur_gcp->out_phase3 += out_phase3;
#endif

	return submitted;
}

/* page alloc and submit read IO */
static int preread_data_segment_second_step(struct f2fs_sb_info *sbi, 
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned int batch_no, unsigned int batch_ofs, 
		unsigned int batch_start_blkoff, unsigned int sec_start_blkaddr)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p3_is_alive_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0; 
	unsigned long long out_phase3 = 0, in_phase3 = 0, 
				  phase3_start_t, phase3_end_t;
#endif
	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head;

#ifdef GC_PROFILE_TIMESERIAL
	read_start_t = OS_TimeGetNS_();
#endif
#ifdef GC_PAGE_POOL
	bool page_pool_used;
#endif
	start_addr = START_BLOCK(sbi, segno);
	
	struct page *data_page;
	struct inode *inode;
	unsigned int ofs_in_node, nofs;
	block_t start_bidx;
	
	unsigned int target_blk_seq, step2_processed_blk_seq;
	target_blk_seq = batch_ofs * BLKS_PER_SEG(sbi);
	target_blk_seq ++;
	unsigned int read_pass_buffer_tail = atomic_read(&pgci->reader_pass_buffer_tail);
	unsigned int read_pass_buffer_head, next_read_pass_buffer_tail;
	struct reader_pass_entry *read_pass_entry;
	unsigned int blkaddr;
	unsigned int blkoff_in_sec, blkoff_in_batch; 
	bool is_first = false;

#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long gc_p3_step2_except_wait_lat = 0;
	unsigned long long p_step2_except_wait_end_t, p_step2_except_wait_start_t;
	unsigned long long p3_step2_read_buf_lat = 0, p3_step2_wakeup_writer_lat = 0;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long gc_p3_step2_lat = 0;
	unsigned long long p_step2_end_t, p_step2_start_t;
	p_step2_start_t = OS_TimeGetNS_();
#endif

	while (1) {
		/* deep pipelining. wait for reader-first-step block completion. */
		/* FIX (reader stranded on per-batch watermark): preread_blk_seq is
		 * batch-relative and reset by the pre-reader at every batch start,
		 * so a reader lagging one or more batches behind can wait here on a
		 * target the counter will never reach again (observed live: reader
		 * at batch 0 target 4609 vs counter 4097 republished by batch 12).
		 * Once the pre-reader has finished this whole batch
		 * (preread_batch_seq > batch_no) no further per-block pacing is
		 * needed: fall through to the ring wait below, which already
		 * handles leftover entries and has the same batch-done escape. */
		if (atomic_read(&sbi->pipe_gc_i->preread_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() ||
				batch_no < atomic_read(&pgci->preread_batch_seq) ||
				(sbi->pipelined_gc_read_second_step_wake &&
					atomic_read(&sbi->pipe_gc_i->preread_blk_seq) >= target_blk_seq));
		}
		
		/* Prevent a tail pass a head. */
		next_read_pass_buffer_tail = (read_pass_buffer_tail + 1) % N_READ_PASS_ENTRY;
		//is_first = true;
		while(next_read_pass_buffer_tail == atomic_read(&pgci->reader_pass_buffer_head)) {
			/* FIX (soft lockup): reader_pass ring empty — yield instead of
			 * naked-spinning so this CPU is not pinned (watchdog soft lockup).
			 * Loop re-reads head, so it proceeds when the pre-reader adds an
			 * entry or the batch-done check below fires. */
			cond_resched();
			/* check if read-first-step batch done. */
			if (batch_no < atomic_read(&pgci->preread_batch_seq)) {
				/* Here, read_first_step batch is done */
				/* Check again for corner case.  */
				if (next_read_pass_buffer_tail == atomic_read(&pgci->reader_pass_buffer_head)) {
					goto finish;
				} else {
					/* unprocessed read buffer entry still left. */ 
					break;
				}
			}
			//if (is_first) {
			//	printk("%s: waiting. next_buf_tail: %lu buf_head: %lu", __func__, 
			//				next_read_pass_buffer_tail, atomic_read(&pgci->reader_pass_buffer_head));
			//	is_first = false;
			//}
		}
		//if (!is_first) {
		//	printk("%s: waiting done. next_buf_tail: %lu buf_head: %lu", __func__, 
		//					next_read_pass_buffer_tail, atomic_read(&pgci->reader_pass_buffer_head));
		//}
#ifdef GC_PROFILE_TIMESERIAL	
		p_step2_except_wait_start_t = OS_TimeGetNS_();
		tmp_start_t = p_step2_except_wait_start_t; 
#endif
		
		/* get the read pass buffer content */
		read_pass_entry = &pgci->reader_pass_buffer[next_read_pass_buffer_tail];
		blkaddr = read_pass_entry->blkaddr;
		if (segno != GET_SEGNO(sbi, blkaddr)) {
			/* not taget segment. finsh this func and move to the next batch_ofs */
			goto finish;
		}
		//printk("%s: tail: %lu segno: %lu blkaddr: 0x%lx", __func__, 
		//		next_read_pass_buffer_tail, segno, read_pass_entry->blkaddr);
		nofs = read_pass_entry->nofs;
		ofs_in_node = read_pass_entry->ofs_in_node;
		inode = read_pass_entry->inode;
			
		/* update reader_pass_buffer_tail */
		read_pass_buffer_tail = next_read_pass_buffer_tail;
		atomic_set(&pgci->reader_pass_buffer_tail, read_pass_buffer_tail);

		/* need nofs, inode, ofs_in_node, start_addr+off, end_flag */
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_step2_read_buf_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t; 
#endif

		if (!f2fs_down_write_trylock(
			&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
			iput(inode);
			sbi->skipped_gc_rwsem++;
			printk("%s: SKIP 4 !!", __func__);
			continue;
		}

		start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
							ofs_in_node;

		if (f2fs_post_read_required(inode)) {
			int err = ra_data_block(inode, start_bidx);

			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (err) {
				iput(inode);
				printk("%s: SKIP 5 !!", __func__);
				continue;
			}
#ifdef GC_PIPELINE
			spin_lock(&sbi->gc_list_lock);
#endif
			add_gc_inode(gc_list, inode);
#ifdef GC_PIPELINE
			spin_unlock(&sbi->gc_list_lock);
#endif
			printk("%s: SKIP 6 !!", __func__);
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_bef_read_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t; 
#endif
		
		
#ifndef SKIP_FMAP_TRAVERSE
		data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
						op_flags, true, NULL);
#else
#ifdef GC_PAGE_POOL
		data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
						op_flags, true, NULL, blkaddr, &page_pool_used);
#else
		data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
						op_flags, true, NULL, blkaddr);
#endif
#endif
		//if (!page_pool_used) {
		//	printk("%s: tail: %lu segno: %lu blkaddr: 0x%lx pcache. page NULL: %d ERR: %d page: %p", 
		//			__func__, 
		//			next_read_pass_buffer_tail, segno, read_pass_entry->blkaddr, 
		//			data_page == NULL, IS_ERR(data_page), data_page);
		//}

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
		op_flags &= ~REQ_GC_START;
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_read_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t; 
#endif
		f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		if (IS_ERR(data_page)) {
			iput(inode);
			printk("%s: SKIP 7 !!", __func__);
			continue;
		}
#ifdef GC_PAGE_POOL
		if (!page_pool_used) {
			//f2fs_put_page(data_page, 0); /* DEEP commented */
		} else {
			//printk("%s: bef put!!!!!!!!!!!!!!!!!!!!", __func__);
			//f2fs_put_page(data_page, 0);
			//printk("%s: aft put!!!!!!!!!!!!!!!!!!!!", __func__);
		}
#else
		f2fs_put_page(data_page, 0);
#endif
		
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_put_page_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t; 
#endif

#ifdef GC_PIPELINE
		spin_lock(&sbi->gc_list_lock);
#endif

		add_gc_inode(gc_list, inode);


#ifdef GC_PIPELINE
		spin_unlock(&sbi->gc_list_lock);
#endif
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t;
#endif
		blkoff_in_sec = blkaddr - sec_start_blkaddr;
		blkoff_in_batch = blkoff_in_sec - batch_start_blkoff;
		step2_processed_blk_seq = blkoff_in_batch + 1;
		target_blk_seq = step2_processed_blk_seq + 1;
		/* BLOCK_GRANULARITY add */
		/* block-granularity pipelining. let writer know block read done. */
		/* FIX (watermark clobber): publish SECTION-absolute block offsets,
		 * not per-batch ones. Per-batch numbering forced every stage to
		 * reset this counter at its own batch boundary, and any such reset
		 * can land after this thread already published into the next batch
		 * (or, across rounds, strand the writer on a zeroed watermark).
		 * Section-absolute values are monotonic for the whole round, so the
		 * only reset left is the round-init one, and skipped/dead blocks
		 * are covered by the next publication or the per-segment watermark
		 * below. Consumers (clean/single first_step target_blk_seq) use the
		 * same numbering. */
		/* FIX (monotonicity): with the batch-done escape above, a reader
		 * catching up can consume ring entries belonging to later batches
		 * and then publish its own (lower) positions; never move the
		 * section-absolute watermark backwards. Single publisher (this
		 * thread), so check-then-set is race-free. */
		if (atomic_read(&pgci->read_step2_done_blk_seq) < blkoff_in_sec + 1)
			atomic_set(&pgci->read_step2_done_blk_seq,
				blkoff_in_sec + 1);
		sbi->pipelined_gc_write_wake = true;
		smp_mb();
		wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
		//printk("%s: read_step2_done_blk_seq: %llu . wake writer", __func__,
		//		atomic_read(&pgci->read_step2_done_blk_seq));
		/* BLOCK_GRANULARITY add end */
#ifdef GC_PROFILE_TIMESERIAL	
		p_step2_except_wait_end_t = OS_TimeGetNS_();
		gc_p3_step2_except_wait_lat += p_step2_except_wait_end_t - p_step2_except_wait_start_t;
		p3_step2_wakeup_writer_lat += p_step2_except_wait_end_t - tmp_start_t;
#endif
//		

	}
finish:
//#ifdef GC_PROFILE_TIMESERIAL	
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_step2_end_t = OS_TimeGetNS_();
	gc_p3_step2_lat = p_step2_end_t - p_step2_start_t;
#endif
//#endif

	/* BLOCK_GRANULARITY add */
	/* block-granularity pipelining. let writer know block read done. */
	/* per-segment watermark, SECTION-absolute (see FIX above): covers every
	 * block of this segment including ones skipped without a per-block
	 * publication (dead inode, ra/lock failures), so the writer's per-block
	 * wait cannot strand on a segment full of deleted blocks. */
	/* FIX (monotonicity): see the per-block publication above — never move
	 * the section-absolute watermark backwards when this reader lagged
	 * behind entries already consumed from later batches. */
	if (atomic_read(&pgci->read_step2_done_blk_seq) <
			batch_start_blkoff + batch_ofs * BLKS_PER_SEG(sbi) + usable_blks_in_seg)
		atomic_set(&pgci->read_step2_done_blk_seq,
			batch_start_blkoff + batch_ofs * BLKS_PER_SEG(sbi) + usable_blks_in_seg);
	sbi->pipelined_gc_write_wake = true;
	smp_mb();
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	//printk("%s: read_step2_done_blk_seq: %llu . wake writer again. last seg. ", __func__,
	//		atomic_read(&pgci->read_step2_done_blk_seq));
	/* BLOCK_GRANULARITY add end */

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
//	cur_gcp->gc_p0_lat += p_lat_t[0];
//	cur_gcp->gc_p1_lat += p_lat_t[1];
//	cur_gcp->gc_p2_lat += p_lat_t[2];
//	cur_gcp->gc_p3_lat += p_lat_t[3];

//	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
//	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
//	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
//	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
//	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->gc_p3_step2_lat += gc_p3_step2_lat;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->gc_p3_step2_except_wait_lat += gc_p3_step2_except_wait_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->p3_step2_read_buf_lat += p3_step2_read_buf_lat;
	cur_gcp->p3_step2_wakeup_writer_lat += p3_step2_wakeup_writer_lat;
//	cur_gcp->p3_check1_lat += p3_check1_lat;
//	cur_gcp->p3_check2_lat += p3_check2_lat;
//	cur_gcp->in_phase3 += in_phase3;
//	cur_gcp->out_phase3 += out_phase3;
#endif

	return submitted;
}

static int clean_data_segment_single_step(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned batch_ofs)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 4;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	unsigned long long p4_move_lat = 0, p4_s1_sleep_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif

#ifdef FILTER_GC_REQ
	unsigned int moved_pgs = 0, vblks_in_seg = get_valid_blocks(sbi, segno, false);
#endif

	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	start_addr = START_BLOCK(sbi, segno);
	
	/* BLOCK_GRANULARITY added */
	unsigned int target_blk_seq;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;


next_step:
	
	/* BLOCK_GRANULARITY added */
	/* FIX (watermark clobber): SECTION-absolute numbering, matching the
	 * reading_second_step publications (see preread_data_segment_second_step). */
	target_blk_seq = (segno - pgci->start_segno) * BLKS_PER_SEG(sbi);

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/* BLOCK_GRANULARITY added */
		target_blk_seq ++;
		
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif
			continue;
		} 
#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_writer ++;
#endif
		
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_();
#endif
		/* BLOCK_GRANULARITY added */
		/* Block-granularity pipelining. wait for reader block completion. */
		if (atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_wake && 
					atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq) >= target_blk_seq));
		}
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_();
		p4_s1_sleep_lat += tmp_end_t - tmp_start_t;
#endif
		//printk("%s: target_blk_seq: %llu read_step2_done_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->read_step2_done_blk_seq));


		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs)) {
			printk("%s: unexpected!!!!!!!!", __func__);
			continue;
		}
		
		//printk("%s: try move blkaddr: 0x%lx read_step2_done_blk_seq: %lu target_blk_seq: %lu", __func__, 
		//		start_addr + off, atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq), 
		//		target_blk_seq
		//		);

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		/* phase 4 */
#ifdef GC_PIPELINE
		spin_lock(&sbi->gc_list_lock);
#endif
		inode = find_gc_inode(gc_list, dni.ino);
#ifdef GC_PIPELINE
		spin_unlock(&sbi->gc_list_lock);
#endif
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;
			if (S_ISREG(inode->i_mode)) {
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[WRITE])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#endif
					printk("%s: unexpected SKIP 2 !!", __func__);
					sbi->skipped_gc_rwsem++;
					continue;
				}
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[READ])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					printk("%s: unexpected SKIP 3 !!", __func__);
					continue;
				}
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					printk("%s: unexpected SKIP 4 !!", __func__);
					continue;
				}
#endif
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
#ifdef FILTER_GC_REQ
			moved_pgs ++;
			if (moved_pgs == vblks_in_seg && sbi->gc_last_seg) {
				sbi->gc_end = true;
				sbi->gc_last_seg = false;
				//printk("%s: REQ_GC_END. set gc_end!!!", __func__);
			}
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page(inode, start_bidx, gc_type,
								segno, off);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif
			if (err) {
				printk("%s: move_data_page error!! segno: %lu blkaddr: 0x%lx", __func__, 
						segno, start_addr + off);
			}

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			if (locked) {
#ifdef GC_PIPELINE
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
				//f2fs_up_write(&fi->i_gc_rwsem[READ]);
				//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#else
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#endif
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		} else {
			printk("%s: unexpected inode NULL ", __func__);
		}
	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p4_lat += p_lat_t[4];
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->p4_move_lat += p4_move_lat;
	cur_gcp->p4_s1_sleep_lat += p4_s1_sleep_lat;
#endif

	return submitted;
}

#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
static int move_data_page_first_step(struct inode *inode, block_t bidx, int gc_type,
							unsigned int segno, int off, struct f2fs_io_info *fio, 
							struct dnode_of_data *dn, bool *page_pool_used_arg)
{
	struct page *page;
	int err = 0;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
#ifdef GC_PROFILE_TIMESERIAL	
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	unsigned long long read_start_t, read_end_t;
	unsigned long long read_start_t_, read_end_t_;
	read_start_t = OS_TimeGetNS_();
#endif

	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_end) {
			op_flags |= REQ_GC_END;
			sbi->gc_end = false;
			printk("%s: REQ_GC_END!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

#ifndef SKIP_FMAP_TRAVERSE
	page = f2fs_get_lock_data_page_jw_gc(inode, bidx, true);
#else
	block_t start_addr;
	start_addr = START_BLOCK(sbi, segno);
#ifdef GC_PAGE_POOL
	bool page_pool_used;
	page = f2fs_get_lock_data_page_jw_gc_without_filemap(inode, bidx, true, start_addr + off, 
									&page_pool_used);
#else
	page = f2fs_get_lock_data_page_jw_gc_without_filemap(inode, bidx, true, start_addr + off);
#endif
#endif
	if (IS_ERR(page)) {
#ifdef GC_PAGE_POOL
		printk("%s: page err. segno: %lu blkaddr: 0x%lx page_pool_used: %d, page NULL: %d page: %p", __func__, segno, start_addr + off, page_pool_used, page == NULL, page);
		/* FIX (UBSAN invalid-load gc.c:5641): this early return skips the
		 * out: label, leaving the caller's page_pool_used uninitialized. */
		*page_pool_used_arg = page_pool_used;
#else
		printk("%s: page err. segno: %lu blkaddr: 0x%lx  page NULL: %d page: %p", __func__, segno, start_addr + off, page == NULL, page);
#endif
		return PTR_ERR(page);
	}

#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t = OS_TimeGetNS_();
	cur_gcp->bef_write_lock_page_lat += read_end_t - read_start_t;
	read_start_t = read_end_t;
	//read_start_t = OS_TimeGetNS_();
#endif

#ifdef GC_PIPELINE_LOCKLESS
	if (!jw_check_valid_map_lockless(F2FS_I_SB(inode), segno, off)){
#else
	if (!check_valid_map(F2FS_I_SB(inode), segno, off)) {
#endif
		printk("%s: bitmap error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
		err = -ENOENT;
		goto out;
	}
#ifdef GC_PROFILE_TIMESERIAL	
	read_end_t_ = OS_TimeGetNS_();
	cur_gcp->bef_write_1_lat += read_end_t_ - read_start_t;
	read_start_t_ = read_end_t_;
#endif

	err = f2fs_gc_pinned_control(inode, gc_type, segno);
	if (err){
		printk("%s: gcpin error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
		goto out;
	}

	if (gc_type == BG_GC) {
		printk("%s: BG_GC unexpected!!!!!!!!!!", __func__);
		f2fs_bug_on(sbi, 1);
		if (folio_test_writeback(page_folio(page))) {
			err = -EAGAIN;
			goto out;
		}
		set_page_dirty(page);
		set_page_private_gcing(page);
	} else {
		fio->op_flags = op_flags;
		fio->page = page;
		//struct f2fs_io_info fio = {
		//	.sbi = F2FS_I_SB(inode),
		//	.ino = inode->i_ino,
		//	.type = DATA,
		//	.temp = COLD,
		//	.op = REQ_OP_WRITE,
		//	.op_flags = op_flags,
		//	.old_blkaddr = NULL_ADDR,
		//	.page = page,
		//	.encrypted_page = NULL,
		//	.need_lock = LOCK_REQ,
		//	.io_type = FS_GC_DATA_IO,
		//};
		bool is_dirty = PageDirty(page);
#ifdef GC_PAGE_POOL
		if (page_pool_used && is_dirty) {
			printk("%s: pool page is dirty. unexpected!!!!!!!!!!!!!!!!!!!", __func__);
		}
#endif

retry:
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_1_lat += read_end_t_ - read_start_t;
//		read_start_t_ = read_end_t_;
//#endif
#ifdef GC_PAGE_POOL
		//if (!page_pool_used){
		    //printk("%s: wait_on_page_writeback page: %p idx: %lu", __func__, page, page->index);	
		f2fs_wait_on_page_writeback(page, DATA, true, true);
		//}
#else
		f2fs_wait_on_page_writeback(page, DATA, true, true);
#endif
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_wait_wb_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif
#ifdef GC_PAGE_POOL
		if (!page_pool_used) {
			set_page_dirty(page);
			//SetPageDirty(page);
			if (clear_page_dirty_for_io(page)) {
				inode_dec_dirty_pages(inode);
				f2fs_remove_dirty_inode(inode);
			}
		}
#else
		set_page_dirty(page);
		//SetPageDirty(page);
		if (clear_page_dirty_for_io(page)) {
			inode_dec_dirty_pages(inode);
			f2fs_remove_dirty_inode(inode);
		}
#endif
//#ifdef GC_PROFILE_TIMESERIAL	
//		read_end_t_ = OS_TimeGetNS_();
//		cur_gcp->bef_write_2_lat += read_end_t_ - read_start_t_;
//		read_start_t_ = read_end_t_;
//#endif

		set_page_private_gcing(page);

#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
//		cur_gcp->bef_write_3_lat += read_end_t - read_start_t_;
		cur_gcp->bef_write_lat += read_end_t - read_start_t;
		read_start_t = read_end_t;
#endif
		//err = f2fs_do_write_data_page(&fio);
#ifdef GC_PAGE_POOL
		if (!page_pool_used) {
			err = jw_f2fs_do_write_data_page_first_step(fio, dn);
		} else {
			err = jw_f2fs_do_write_data_page_gc_pool_first_step(fio, dn);
		}
#else
		err = jw_f2fs_do_write_data_page_first_step(fio, dn);
#endif
		if (err) {
#ifdef GC_PAGE_POOL
			printk("%s: write error. segno: %lu blkaddr: 0x%lx page_pool_used: %d", __func__, segno, start_addr + off, page_pool_used);
#else
			printk("%s: write error. segno: %lu blkaddr: 0x%lx ", __func__, segno, start_addr + off);
#endif
			clear_page_private_gcing(page);
			if (err == -ENOMEM) {
				memalloc_retry_wait(GFP_NOFS);
				goto retry;
			}
			if (is_dirty)
				set_page_dirty(page);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		read_end_t = OS_TimeGetNS_();
		cur_gcp->write_lat += read_end_t - read_start_t;
#endif
	}
out:
#ifdef GC_PAGE_POOL
	*page_pool_used_arg = page_pool_used;
#endif
	/* FIX (NULL deref in second step): on error the caller no longer
	 * enqueues this block to the write-pass ring, so second step will never
	 * unlock/put this page — release it here. Only the lookup reference is
	 * dropped; the reader-side reference (the extra put(page, 0) second step
	 * does for non-pool pages) may leak on this rare path, which is
	 * preferable to an over-put. */
	if (err) {
#ifdef GC_PAGE_POOL
		if (page_pool_used)
			jw_f2fs_put_page_gc_pool(page, 1);
		else
			f2fs_put_page(page, 1);
#else
		f2fs_put_page(page, 1);
#endif
	}
//#ifdef GC_PAGE_POOL
//	if (!page_pool_used) {
//#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
//		f2fs_put_page(page, 0);	/* DEEP added */
//#endif
//		f2fs_put_page(page, 1);
//	} else {
//		jw_f2fs_put_page_gc_pool(page, 1);
//	}
//#else
//	f2fs_put_page(page, 1);
//#endif
	return err;
}

static int move_data_page_second_step(struct inode *inode, struct f2fs_io_info *fio, 
							bool page_pool_used, struct dnode_of_data *dn)
{
	int err = 0;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
#endif

	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_end) {
			op_flags |= REQ_GC_END;
			sbi->gc_end = false;
			printk("%s: REQ_GC_END!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif
	struct page *page = fio->page;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif

	bool is_dirty = PageDirty(page);

#ifdef GC_PROFILE_TIMESERIAL	
	read_start_t = OS_TimeGetNS_();
#endif
	//err = f2fs_do_write_data_page(&fio);
#ifdef GC_PAGE_POOL
	if (!page_pool_used) {
		err = jw_f2fs_do_write_data_page_second_step(fio, dn);
	} else {
		err = jw_f2fs_do_write_data_page_gc_pool_second_step(fio, dn);
	}
#else
	err = jw_f2fs_do_write_data_page_second_step(fio, dn);
#endif
	if (err == -EALREADY) {
		/* a concurrent user write retargeted this block after step1;
		 * the stale copy was orphaned and invalidated — expected under
		 * overwrite-heavy workloads (YCSB-A/F), not an error */
		err = 0;
	} else if (err) {
		f2fs_bug_on(sbi, 1);
		//printk("%s: write error. segno: %lu blkaddr: 0x%lx", __func__, segno, start_addr + off);
		clear_page_private_gcing(page);
		if (err == -ENOMEM) {
			memalloc_retry_wait(GFP_NOFS);
			f2fs_bug_on(sbi, 1);
			//goto retry;
		}
		if (is_dirty)
			set_page_dirty(page);
	}
#ifdef GC_PROFILE_TIMESERIAL
	read_end_t = OS_TimeGetNS_();
	cur_gcp->write_step2_lat += read_end_t - read_start_t;
#endif
out:

#ifdef GC_PAGE_POOL
	if (!page_pool_used) {
#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
		f2fs_put_page(page, 0);	/* DEEP added */
#endif
		f2fs_put_page(page, 1);
	} else {
		jw_f2fs_put_page_gc_pool(page, 1);
	}
#else
	f2fs_put_page(page, 1);
#endif
	return err;
}

static int clean_data_segment_first_step(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned batch_ofs)//, bool is_last_seg)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long //p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p4_move_lat = 0; 
#endif

#ifdef FILTER_GC_REQ
	unsigned int moved_pgs = 0, vblks_in_seg = get_valid_blocks(sbi, segno, false);
#endif

	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	start_addr = START_BLOCK(sbi, segno);
	
	/* BLOCK_GRANULARITY added */
	unsigned int target_blk_seq;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	unsigned int write_pass_buffer_head = atomic_read(&pgci->writer_pass_buffer_head);
	unsigned int next_write_pass_buffer_head;
	struct writer_pass_entry *write_pass_entry;
	bool is_first = false;

	/* BLOCK_GRANULARITY added */
	/* FIX (watermark clobber): SECTION-absolute numbering, matching the
	 * reading_second_step publications (see preread_data_segment_second_step). */
	target_blk_seq = (segno - pgci->start_segno) * BLKS_PER_SEG(sbi);

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_step1_end_t, p_step1_start_t;
	p_step1_start_t = OS_TimeGetNS_();
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long gc_p4_step1_except_wait_lat = 0, p4_is_alive_lat = 0, p4_s1_write_buf_and_wake_lat = 0;
	unsigned long long p_step1_except_wait_end_t, p_step1_except_wait_start_t;
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/* BLOCK_GRANULARITY added */
		target_blk_seq ++;
		
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif
			continue;
		} 
#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_writer ++;
#endif
		
		/* BLOCK_GRANULARITY added */
		/* Block-granularity pipelining. wait for reader block completion. */
		if (atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_wake && 
					atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq) >= target_blk_seq));
		}
		//printk("%s: target_blk_seq: %llu read_step2_done_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->read_step2_done_blk_seq));


		//printk("%s: bef is_alive. target_blk_seq: %llu read_step2_done_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->read_step2_done_blk_seq));
#ifdef GC_PROFILE_TIMESERIAL	
		p_step1_except_wait_start_t = OS_TimeGetNS_();
		tmp_start_t = p_step1_except_wait_start_t; 
#endif
		//printk("%s: aft is_alive. target_blk_seq: %llu read_step2_done_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->read_step2_done_blk_seq));

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs)) {
		//if (!is_alive_pipewrt(sbi, entry, &dni, start_addr + off, &nofs)) {
			printk("%s: unexpected!!!!!!!!", __func__);
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p4_is_alive_lat += tmp_end_t - tmp_start_t;
		//tmp_start_t = tmp_end_t;
#endif
		
		//printk("%s: try move blkaddr: 0x%lx read_step2_done_blk_seq: %lu target_blk_seq: %lu", __func__, 
		//		start_addr + off, atomic_read(&sbi->pipe_gc_i->read_step2_done_blk_seq), 
		//		target_blk_seq
		//		);

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		/* phase 4 */
#ifdef GC_PIPELINE
		spin_lock(&sbi->gc_list_lock);
#endif
		inode = find_gc_inode(gc_list, dni.ino);
#ifdef GC_PIPELINE
		spin_unlock(&sbi->gc_list_lock);
#endif
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;
			//printk("%s: try rwsem. ", __func__);
			if (S_ISREG(inode->i_mode)) {
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[WRITE])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
					printk("%s: unexpected SKIP 2 !!", __func__);
					sbi->skipped_gc_rwsem++;
					continue;
				}
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[READ])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					printk("%s: unexpected SKIP 3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
				atomic_inc(&fi->n_gc_pages);
				//printk("%s: finish rwsem. ", __func__);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
#ifdef FILTER_GC_REQ
			//moved_pgs ++;
			//if (moved_pgs == vblks_in_seg && sbi->gc_last_seg) {
			//	sbi->gc_end = true;
			//	sbi->gc_last_seg = false;
			//	//printk("%s: REQ_GC_END. set gc_end!!!", __func__);
			//}
#endif
		
			struct f2fs_io_info fio = {
				.sbi = F2FS_I_SB(inode),
				.ino = inode->i_ino,
				.type = DATA,
				.temp = COLD,
				.op = REQ_OP_WRITE,
				//.op_flags = op_flags,
				.old_blkaddr = NULL_ADDR,
				//.page = page,
				.encrypted_page = NULL,
				.need_lock = LOCK_REQ,
				.io_type = FS_GC_DATA_IO,
			};
			struct dnode_of_data dn;
			bool page_pool_used;
			if (f2fs_post_read_required(inode)) {
				f2fs_bug_on(sbi, 1);
				//err = move_data_block(inode, start_bidx,
				//			gc_type, segno, off);
			}
			else {
				//printk("%s: try move_data_page_fist_step. ", __func__);
				err = move_data_page_first_step(inode, start_bidx, gc_type,
								segno, off, &fio, &dn, &page_pool_used);
				//printk("%s: finish move_data_page_fist_step. ", __func__);
			}
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (err) {
				printk("%s: move_data_page error!! segno: %lu blkaddr: 0x%lx", __func__,
						segno, start_addr + off);
				/* FIX (NULL deref in second step): do NOT enqueue this
				 * block. On most error paths fio->page was never set, so
				 * second step would crash in PageDirty(NULL) (observed
				 * Oops at clean_data_segment_second_step, CR2=0x8). The
				 * page itself is released inside move_data_page_first_step.
				 * Still advance prewrite_blk_seq so second step's per-block
				 * wait cannot strand on a trailing skipped block. */
				if (locked) {
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					atomic_dec(&fi->n_gc_pages);
				}
				sbi->pipelined_gc_write_second_step_wake = true;
				atomic_set(&pgci->prewrite_blk_seq,
					batch_ofs * BLKS_PER_SEG(sbi) + off + 1);
				smp_mb();
				wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
				continue;
			}

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode))) {
				/* TODO: This should be modified. write second step should increase submitted. */
				submitted++;
			}
			
			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
				//f2fs_up_write(&fi->i_gc_rwsem[READ]);
				//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			}
			
			/* Prevent a head pass a tail. */
			next_write_pass_buffer_head = (write_pass_buffer_head + 1) % N_WRITE_PASS_ENTRY;
			is_first = true;
			while(next_write_pass_buffer_head == atomic_read(&pgci->writer_pass_buffer_tail)){
				/* FIX (soft lockup): writer_pass ring full — yield instead of
				 * naked-spinning so this CPU is not pinned (watchdog soft
				 * lockup). Loop re-reads the tail, so it proceeds as soon as
				 * writing_second_step frees a slot. */
				cond_resched();
				if (is_first) {
					/* keep quiet on the hot path: a full ring under GC
					 * pressure would turn this into a printk storm that
					 * throttles step1 exactly when GC is busiest */
					//printk("%s: waiting. next_buf_head: %lu buf_tail: %lu", __func__,
					//		next_write_pass_buffer_head, atomic_read(&pgci->writer_pass_buffer_tail));
					is_first = false;
				}
			}
			if (!is_first) {
				//printk("%s: waiting done. next_buf_head: %lu buf_tail: %lu", __func__,
				//		next_write_pass_buffer_head, atomic_read(&pgci->writer_pass_buffer_tail));
			}


			/* fill the write pass buffer */
			write_pass_entry = &pgci->writer_pass_buffer[write_pass_buffer_head];
			write_pass_entry->fio = fio;
			write_pass_entry->dn = dn;
			write_pass_entry->inode = inode;
			write_pass_entry->page_pool_used = page_pool_used;
			write_pass_entry->blkaddr = start_addr + off;
			//printk("%s: head: %lu segno: %lu blkaddr: 0x%lx", __func__, 
			//		write_pass_buffer_head, segno, write_pass_entry->blkaddr);
			
			/* update writer_pass_buffer_head */
			write_pass_buffer_head = next_write_pass_buffer_head;

			smp_mb();
			atomic_set(&pgci->writer_pass_buffer_head, write_pass_buffer_head);

			//stat_inc_data_blk_count(sbi, 1, gc_type);
			
			/* block-granularity pipelining. let writer-second-step know block write done. */
			sbi->pipelined_gc_write_second_step_wake = true;
			atomic_set(&pgci->prewrite_blk_seq, 
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
			//printk("%s: prewrite_blk_seq: %llu . wake 2nd writer", __func__, 
			//		atomic_read(&pgci->prewrite_blk_seq));
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_();
			p4_s1_write_buf_and_wake_lat += tmp_end_t - tmp_start_t;
			p_step1_except_wait_end_t = tmp_end_t;
			gc_p4_step1_except_wait_lat += p_step1_except_wait_end_t - p_step1_except_wait_start_t;
		//p4_step2_wakeup_writer_lat += p_step2_except_wait_end_t - tmp_start_t;
#endif
		} else {
			printk("%s: unexpected inode NULL ", __func__);
			/* skipped block (inode deleted under us, e.g. rm -rf during
			 * GC): no ring entry to enqueue, but still advance
			 * prewrite_blk_seq so second step's per-block wait cannot
			 * strand on it (same accounting as the move_data_page error
			 * path above). */
			sbi->pipelined_gc_write_second_step_wake = true;
			atomic_set(&pgci->prewrite_blk_seq,
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1);
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
		}
	}

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	p_step1_end_t = OS_TimeGetNS_();
	cur_gcp->gc_p4_lat += p_step1_end_t - p_step1_start_t;
	cur_gcp->p4_s1_write_buf_and_wake_lat += p4_s1_write_buf_and_wake_lat;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->gc_p4_step1_except_wait_lat += gc_p4_step1_except_wait_lat;
	cur_gcp->p4_move_lat += p4_move_lat;
	cur_gcp->p4_is_alive_lat += p4_is_alive_lat;
#endif

	return submitted;
}

static int clean_data_segment_second_step(struct f2fs_sb_info *sbi, 
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned int batch_no, unsigned batch_ofs, 
		unsigned int batch_start_blkoff, unsigned int sec_start_blkaddr)
{
	struct super_block *sb = sbi->sb;
	block_t start_addr;
	int off;
	//int phase = 4;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	unsigned long long //p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p4_move_step2_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif

#ifdef FILTER_GC_REQ
	unsigned int moved_pgs = 0, vblks_in_seg = get_valid_blocks(sbi, segno, false);
#endif

	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	start_addr = START_BLOCK(sbi, segno);
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head;

#ifdef GC_PROFILE_TIMESERIAL	
	//sbi->intra_seg_blkseq_of_writer = -1;
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	//p_start_t[phase] = OS_TimeGetNS_();
#endif
	struct inode *inode;
	struct f2fs_io_info *fio;
	bool page_pool_used;
	struct dnode_of_data *dn;

	unsigned int target_blk_seq, step2_processed_blk_seq;
	target_blk_seq = batch_ofs * BLKS_PER_SEG(sbi);
	target_blk_seq ++;
	unsigned int write_pass_buffer_tail = atomic_read(&pgci->writer_pass_buffer_tail);
	unsigned int write_pass_buffer_head, next_write_pass_buffer_tail;
	struct writer_pass_entry *write_pass_entry;
	unsigned int blkaddr;
	unsigned int blkoff_in_sec, blkoff_in_batch; 
	bool is_first = false;

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_step2_end_t, p_step2_start_t;
	unsigned long long gc_p4_step2_lat = 0;
	p_step2_start_t = OS_TimeGetNS_();
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long gc_p4_step2_except_wait_lat = 0;
	unsigned long long p_step2_except_wait_end_t, p_step2_except_wait_start_t;
	unsigned long long p4_step2_read_buf_lat = 0, p4_step2_wakeup_writer_lat = 0;
#endif

	while (1) {
		/* deep pipelining. wait for reader-first-step block completion. */
		if (atomic_read(&sbi->pipe_gc_i->prewrite_blk_seq) < target_blk_seq) {
			//printk("%s: start wait. target_blk_seq: %lu prewrite_blk_seq: %lu",
			//	   	__func__, target_blk_seq, atomic_read(&sbi->pipe_gc_i->prewrite_blk_seq));
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_second_step_wake && 
					atomic_read(&sbi->pipe_gc_i->prewrite_blk_seq) >= target_blk_seq));
			//printk("%s: finish wait. target_blk_seq: %lu prewrite_blk_seq: %lu", 
			//		__func__, target_blk_seq, 
			//		atomic_read(&sbi->pipe_gc_i->prewrite_blk_seq));
		}
		
		/* Prevent a tail pass a head. */
		next_write_pass_buffer_tail = (write_pass_buffer_tail + 1) % N_WRITE_PASS_ENTRY;
		is_first = true;
		while(next_write_pass_buffer_tail == atomic_read(&pgci->writer_pass_buffer_head)) {
			/* check if write-first-step batch done. */
			if (batch_no < atomic_read(&pgci->prewrite_batch_seq)) {
				/* Here, write_first_step batch is done */
				/* Check again for corner case.  */
				//printk("%s: batch_no: %lu may finish. ", __func__, batch_no);
				if (next_write_pass_buffer_tail == atomic_read(&pgci->writer_pass_buffer_head)) {
					//printk("%s: batch_no: %lu finish. ", __func__, batch_no);
					goto finish;
				} else {
					/* unprocessed write buffer entry still left. */ 
					break;
				}
			}
			if (is_first) {
				//printk("%s: waiting. next_buf_tail: %lu buf_head: %lu", __func__,
				//			next_write_pass_buffer_tail, atomic_read(&pgci->writer_pass_buffer_head));
				is_first = false;
			}
			/* FIX (soft lockup): ring empty and batch not yet done — sleep on
			 * the second-step wq instead of the old naked busy-spin (which had
			 * no scheduling point, pinned the CPU -> watchdog soft lockup, and
			 * could never bail on teardown). Woken by writing_first_step when it
			 * advances writer_pass_buffer_head (block produced) or increments
			 * prewrite_batch_seq (batch done). On wake we loop back and re-run
			 * the checks above, preserving the goto finish / break semantics. */
			wait_event_interruptible(*wq,
				kthread_should_stop() ||
				next_write_pass_buffer_tail != atomic_read(&pgci->writer_pass_buffer_head) ||
				batch_no < atomic_read(&pgci->prewrite_batch_seq));
			if (kthread_should_stop())
				goto finish;
		}
		if (!is_first) {
			//printk("%s: waiting done. next_buf_tail: %lu buf_head: %lu", __func__, 
			//				next_write_pass_buffer_tail, atomic_read(&pgci->writer_pass_buffer_head));
		}
#ifdef GC_PROFILE_TIMESERIAL	
		p_step2_except_wait_start_t = OS_TimeGetNS_();
		tmp_start_t = p_step2_except_wait_start_t; 
#endif
		
		/* get the write pass buffer content */
		write_pass_entry = &pgci->writer_pass_buffer[next_write_pass_buffer_tail];
		blkaddr = write_pass_entry->blkaddr;
		if (segno != GET_SEGNO(sbi, blkaddr)) {
			/* not taget segment. finsh this func and move to the next batch_ofs */
			blkoff_in_sec = blkaddr - sec_start_blkaddr;
			blkoff_in_batch = blkoff_in_sec - batch_start_blkoff;
			step2_processed_blk_seq = blkoff_in_batch + 1; 
			target_blk_seq = step2_processed_blk_seq + 1;
			//printk("%s: finish. target_blk_seq: %lu", __func__, target_blk_seq);
			goto finish;
		}
		//printk("%s: tail: %lu segno: %lu blkaddr: 0x%lx", __func__, 
		//		next_write_pass_buffer_tail, segno, write_pass_entry->blkaddr);
		/* inode, */
		fio = &write_pass_entry->fio;
		dn = &write_pass_entry->dn;
		inode = write_pass_entry->inode;
		page_pool_used = write_pass_entry->page_pool_used;
			
		/* update reader_pass_buffer_tail */
		write_pass_buffer_tail = next_write_pass_buffer_tail;
		//atomic_set(&pgci->writer_pass_buffer_tail, write_pass_buffer_tail);

		/* need nofs, inode, ofs_in_node, start_addr+off, end_flag */
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p4_step2_read_buf_lat += tmp_end_t - tmp_start_t;
		tmp_start_t = tmp_end_t; 
#endif

		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			//bool locked = false;
			int err;
			if (S_ISREG(inode->i_mode)) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write_second_step[WRITE])) {
				//	printk("%s: unexpected SKIP 2 !!", __func__);
				//	sbi->skipped_gc_rwsem++;
				//	continue;
				//}
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write_second_step[READ])) {
				////if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
				//	sbi->skipped_gc_rwsem++;
				//	f2fs_up_write(&fi->i_gc_rwsem_gc_write_second_step[WRITE]);
				//	//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
				//	printk("%s: unexpected SKIP 3 !!", __func__);
				//	continue;
				//}
				//locked = true;

				/* wait for all inflight aio data */
				//inode_dio_wait(inode);
			}

//			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
//								+ ofs_in_node;
//#ifdef GC_PROFILE_TIMESERIAL	
//			tmp_start_t = OS_TimeGetNS_();
//#endif
#ifdef FILTER_GC_REQ
			moved_pgs ++;
			if (moved_pgs == vblks_in_seg && sbi->gc_last_seg) {
				sbi->gc_end = true;
				sbi->gc_last_seg = false;
				//printk("%s: REQ_GC_END. set gc_end!!!", __func__);
			}
#endif
			if (f2fs_post_read_required(inode)) {
				f2fs_bug_on(sbi, 1);
				//err = move_data_block(inode, start_bidx,
				//			gc_type, segno, off);
			}
			else {
				blkoff_in_sec = blkaddr - sec_start_blkaddr;
				blkoff_in_batch = blkoff_in_sec - batch_start_blkoff;
				step2_processed_blk_seq = blkoff_in_batch + 1;
				target_blk_seq = step2_processed_blk_seq + 1;
				//printk("%s: target_blk_seq: %u start move_data_page_second_step. ", __func__, target_blk_seq);
				/* defensive: first step must not enqueue entries without a
				 * page; crash observed when an errored entry slipped in. */
				if (unlikely(!fio->page)) {
					printk("%s: unexpected NULL fio->page. blkaddr: 0x%x", __func__, blkaddr);
					err = -EINVAL;
				} else
					err = move_data_page_second_step(inode, fio, page_pool_used, dn);
				//printk("%s: target_blk_seq: %u end move_data_page_second_step. ", __func__, target_blk_seq);
			}
			if (err) {
				printk("%s: move_data_page error!! segno: %lu blkaddr: 0x%lx", __func__, 
						segno, start_addr + off);
			}

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			//if (locked) {
			//	f2fs_up_write(&fi->i_gc_rwsem_gc_write_second_step[READ]);
			//	f2fs_up_write(&fi->i_gc_rwsem_gc_write_second_step[WRITE]);
			//}
			
			atomic_dec(&fi->n_gc_pages);

			stat_inc_data_blk_count(sbi, 1, gc_type);
		} else {
			printk("%s: unexpected inode NULL ", __func__);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_();
		p4_move_step2_lat += tmp_end_t - tmp_start_t;
		//tmp_start_t = tmp_end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
//		tmp_end_t = OS_TimeGetNS_(); 
//		p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
//		tmp_start_t = tmp_end_t;
//		tmp_start_t = OS_TimeGetNS_(); 
#endif
		/* update reader_pass_buffer_tail. Lazy manner due to fio and dn. */
		atomic_set(&pgci->writer_pass_buffer_tail, write_pass_buffer_tail);
		
		blkoff_in_sec = blkaddr - sec_start_blkaddr;
		blkoff_in_batch = blkoff_in_sec - batch_start_blkoff;
		step2_processed_blk_seq = blkoff_in_batch + 1; 
		target_blk_seq = step2_processed_blk_seq + 1;
		/* BLOCK_GRANULARITY add */
		/* block-granularity pipelining. let writer know block read done. */
//		atomic_set(&pgci->read_step2_done_blk_seq, 
//			step2_processed_blk_seq); 
//		sbi->pipelined_gc_write_wake = true;
//		smp_mb();
//		wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
		//printk("%s: read_step2_done_blk_seq: %llu . wake writer", __func__, 
		//		atomic_read(&pgci->read_step2_done_blk_seq));
		/* BLOCK_GRANULARITY add end */
#ifdef GC_PROFILE_TIMESERIAL	
		p_step2_except_wait_end_t = OS_TimeGetNS_();
		gc_p4_step2_except_wait_lat += p_step2_except_wait_end_t - p_step2_except_wait_start_t;
		//p4_step2_wakeup_writer_lat += p_step2_except_wait_end_t - tmp_start_t;
#endif
	}
finish:


#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_step2_end_t = OS_TimeGetNS_();
	gc_p4_step2_lat = p_step2_end_t - p_step2_start_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p4_step2_lat += gc_p4_step2_lat;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->gc_p4_step2_except_wait_lat += gc_p4_step2_except_wait_lat;
	cur_gcp->p4_move_step2_lat += p4_move_step2_lat;
	cur_gcp->p4_step2_read_buf_lat += p4_step2_read_buf_lat;
#endif

	return submitted;
}
#endif
#endif

#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
static int preread_data_segment_blk_gra(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned int batch_ofs)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p3_is_alive_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0; 
	unsigned long long out_phase3 = 0, in_phase3 = 0, 
				  phase3_start_t, phase3_end_t;
#endif
	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL
	read_start_t = OS_TimeGetNS_();
#endif
#ifdef GC_PAGE_POOL
	bool page_pool_used;
#endif
	start_addr = START_BLOCK(sbi, segno);

next_step:

	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL
	sbi->intra_seg_blkseq_of_prereader = -1;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			phase3_start_t = OS_TimeGetNS_();
			tmp_start_t = phase3_start_t; 
		}
#endif
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif

#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_prereader ++;
#endif
		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif
		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_is_alive_lat += tmp_end_t - tmp_start_t;
#endif

		//if (phase == 2) {
		//	f2fs_ra_node_page(sbi, dni.ino);
		//	continue;
		//}
		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			in_phase3 += phase3_end_t - phase3_start_t;
			phase3_start_t = phase3_end_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			int err;
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				//printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			if (!f2fs_down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				iput(inode);
				sbi->skipped_gc_rwsem++;
				//printk("%s: SKIP 4 !!", __func__);
				continue;
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					//printk("%s: SKIP 5 !!", __func__);
					continue;
				}
#ifdef GC_PIPELINE
				spin_lock(&sbi->gc_list_lock);
#endif
				add_gc_inode(gc_list, inode);
#ifdef GC_PIPELINE
				spin_unlock(&sbi->gc_list_lock);
#endif
				//printk("%s: SKIP 6 !!", __func__);
				continue;
			}

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			
		
#ifndef SKIP_FMAP_TRAVERSE
			data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
							op_flags, true, NULL);
#else
#ifdef GC_PAGE_POOL
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off, &page_pool_used);
#else
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off);
#endif
#endif

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
			op_flags &= ~REQ_GC_START;
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				//printk("%s: SKIP 7 !!", __func__);
				continue;
			}
#ifdef GC_PAGE_POOL
			if (!page_pool_used) {
				//f2fs_put_page(data_page, 0); /* DEEP commented */
			} else {
				//printk("%s: bef put!!!!!!!!!!!!!!!!!!!!", __func__);
				//f2fs_put_page(data_page, 0);
				//printk("%s: aft put!!!!!!!!!!!!!!!!!!!!", __func__);
			}
#else
			f2fs_put_page(data_page, 0);
#endif
		
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_put_page_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif

#ifdef GC_PIPELINE
			spin_lock(&sbi->gc_list_lock);
#endif

			add_gc_inode(gc_list, inode);

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
#endif

#ifdef GC_PIPELINE
			spin_unlock(&sbi->gc_list_lock);
#endif
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			out_phase3 += phase3_end_t - phase3_start_t;
#endif 
			/* BLOCK_GRANULARITY add */
			/* block-granularity pipelining. let writer know block read done. */
			sbi->pipelined_gc_write_wake = true;
			atomic_set(&pgci->preread_blk_seq, 
				batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
			//printk("%s: preread_blk_seq: %llu . wake writer", __func__, 
			//		atomic_read(&pgci->preread_blk_seq));
			/* BLOCK_GRANULARITY add end */
			continue; 
		}

	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 4)
		goto next_step;
	
	/* BLOCK_GRANULARITY add */
	/* block-granularity pipelining. let writer know block read done. */
	sbi->pipelined_gc_write_wake = true;
	atomic_set(&pgci->preread_blk_seq, 
		batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
	smp_mb();
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	//printk("%s: preread_blk_seq: %llu . wake writer again. last seg. ", __func__, 
	//		atomic_read(&pgci->preread_blk_seq));
	/* BLOCK_GRANULARITY add end */

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
#endif
#ifdef GC_PROFILE_TIMESERIAL	

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	//cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->in_phase3 += in_phase3;
	cur_gcp->out_phase3 += out_phase3;
#endif

	return submitted;
}

static int clean_data_segment_blk_gra(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, unsigned batch_ofs)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 4;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p4_move_lat = 0; 
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	//read_start_t = OS_TimeGetNS_();
#endif

#ifdef FILTER_GC_REQ
	unsigned int moved_pgs = 0, vblks_in_seg = get_valid_blocks(sbi, segno, false);
#endif

	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	start_addr = START_BLOCK(sbi, segno);
	
	/* BLOCK_GRANULARITY added */
	unsigned int target_blk_seq;
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;


next_step:
	
	/* BLOCK_GRANULARITY added */
	target_blk_seq = batch_ofs * BLKS_PER_SEG(sbi);

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/* BLOCK_GRANULARITY added */
		target_blk_seq ++;
		
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif
			continue;
		} 
#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_writer ++;
#endif
		
		/* BLOCK_GRANULARITY added */
		/* Block-granularity pipelining. wait for reader block completion. */
		if (atomic_read(&sbi->pipe_gc_i->preread_blk_seq) < target_blk_seq) {
			wait_event_interruptible(*wq, kthread_should_stop() || 
				(sbi->pipelined_gc_write_wake && 
					atomic_read(&sbi->pipe_gc_i->preread_blk_seq) >= target_blk_seq));
		}
		//printk("%s: target_blk_seq: %llu preread_blk_seq: %llu . writer passed.", __func__, 
		//		target_blk_seq, atomic_read(&pgci->preread_blk_seq));


		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		/* phase 4 */
#ifdef GC_PIPELINE
		spin_lock(&sbi->gc_list_lock);
#endif
		inode = find_gc_inode(gc_list, dni.ino);
#ifdef GC_PIPELINE
		spin_unlock(&sbi->gc_list_lock);
#endif
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;
			if (S_ISREG(inode->i_mode)) {
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[WRITE])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#endif
					//printk("%s: SKIP 2 !!", __func__);
					sbi->skipped_gc_rwsem++;
					continue;
				}
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[READ])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP 3 !!", __func__);
					continue;
				}
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP 4 !!", __func__);
					continue;
				}
#endif
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
#ifdef FILTER_GC_REQ
			moved_pgs ++;
			if (moved_pgs == vblks_in_seg && sbi->gc_last_seg) {
				sbi->gc_end = true;
				sbi->gc_last_seg = false;
				//printk("%s: REQ_GC_END. set gc_end!!!", __func__);
			}
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page(inode, start_bidx, gc_type,
								segno, off);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			if (locked) {
#ifdef GC_PIPELINE
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
				//f2fs_up_write(&fi->i_gc_rwsem[READ]);
				//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#else
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#endif
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p4_lat += p_lat_t[4];
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}
#endif

static int preread_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0,  
						p3_is_alive_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0; 
	unsigned long long out_phase3 = 0, in_phase3 = 0, 
				  phase3_start_t, phase3_end_t;
#endif
	blk_opf_t op_flags = REQ_RAHEAD;
#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req) {
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL
	read_start_t = OS_TimeGetNS_();
#endif
#ifdef GC_PAGE_POOL
	bool page_pool_used;
#endif
	start_addr = START_BLOCK(sbi, segno);

next_step:

	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL
	sbi->intra_seg_blkseq_of_prereader = -1;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			phase3_start_t = OS_TimeGetNS_();
			tmp_start_t = phase3_start_t; 
		}
#endif
		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}
		
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif

#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_prereader ++;
#endif
		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif
		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		tmp_end_t = OS_TimeGetNS_(); 
		p3_is_alive_lat += tmp_end_t - tmp_start_t;
#endif

		//if (phase == 2) {
		//	f2fs_ra_node_page(sbi, dni.ino);
		//	continue;
		//}
		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			in_phase3 += phase3_end_t - phase3_start_t;
			phase3_start_t = phase3_end_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			int err;
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				//printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			if (!f2fs_down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				iput(inode);
				sbi->skipped_gc_rwsem++;
				//printk("%s: SKIP 4 !!", __func__);
				continue;
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					//printk("%s: SKIP 5 !!", __func__);
					continue;
				}
#ifdef GC_PIPELINE
				spin_lock(&sbi->gc_list_lock);
#endif
				add_gc_inode(gc_list, inode);
#ifdef GC_PIPELINE
				spin_unlock(&sbi->gc_list_lock);
#endif
				//printk("%s: SKIP 6 !!", __func__);
				continue;
			}

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			
		
#ifndef SKIP_FMAP_TRAVERSE
			data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
							op_flags, true, NULL);
#else
#ifdef GC_PAGE_POOL
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off, &page_pool_used);
#else
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off);
#endif
#endif

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
			op_flags &= ~REQ_GC_START;
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				//printk("%s: SKIP 7 !!", __func__);
				continue;
			}
#ifdef GC_PAGE_POOL
			if (!page_pool_used) {
				f2fs_put_page(data_page, 0);
			} else {
				//printk("%s: bef put!!!!!!!!!!!!!!!!!!!!", __func__);
				//f2fs_put_page(data_page, 0);
				//printk("%s: aft put!!!!!!!!!!!!!!!!!!!!", __func__);
			}
#else
			f2fs_put_page(data_page, 0);
#endif
		
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_put_page_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif

#ifdef GC_PIPELINE
			spin_lock(&sbi->gc_list_lock);
#endif

			add_gc_inode(gc_list, inode);

#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
#endif

#ifdef GC_PIPELINE
			spin_unlock(&sbi->gc_list_lock);
#endif
#ifdef GC_PROFILE_TIMESERIAL	
			phase3_end_t = OS_TimeGetNS_();
			out_phase3 += phase3_end_t - phase3_start_t;
#endif
			continue;
		}

	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 4)
		goto next_step;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->in_phase3 += in_phase3;
	cur_gcp->out_phase3 += out_phase3;
#endif

	return submitted;
}

static int clean_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 4;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t;
	unsigned long long p4_move_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif

#ifdef FILTER_GC_REQ
	unsigned int moved_pgs = 0, vblks_in_seg = get_valid_blocks(sbi, segno, false);
#endif

	start_addr = START_BLOCK(sbi, segno);

next_step:
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

#ifdef GC_PIPELINE_LOCKLESS
		if (jw_check_valid_map_lockless(sbi, segno, off) == 0){
#else
		if (check_valid_map(sbi, segno, off) == 0){
#endif
			continue;
		} 
#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_writer ++;
#endif

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		/* phase 4 */
#ifdef GC_PIPELINE
		spin_lock(&sbi->gc_list_lock);
#endif
		inode = find_gc_inode(gc_list, dni.ino);
#ifdef GC_PIPELINE
		spin_unlock(&sbi->gc_list_lock);
#endif
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;
			if (S_ISREG(inode->i_mode)) {
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[WRITE])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
#endif
					printk("%s: SKIP 2 !!", __func__);
					sbi->skipped_gc_rwsem++;
					continue;
				}
#ifdef GC_PIPELINE
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem_gc_write[READ])) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					printk("%s: SKIP 3 !!", __func__);
					continue;
				}
#else
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP 4 !!", __func__);
					continue;
				}
#endif
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
#ifdef FILTER_GC_REQ
			moved_pgs ++;
			if (moved_pgs == vblks_in_seg && sbi->gc_last_seg) {
				sbi->gc_end = true;
				sbi->gc_last_seg = false;
				//printk("%s: REQ_GC_END. set gc_end!!!", __func__);
			}
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page(inode, start_bidx, gc_type,
								segno, off);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			if (locked) {
#ifdef GC_PIPELINE
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
				//f2fs_up_write(&fi->i_gc_rwsem[READ]);
				//f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#else
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
#endif
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p4_lat += p_lat_t[4];
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}


static int do_garbage_collect_early_read(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
#ifdef CHECK_GC_RACE
	printk("%s start: %s %u", __func__, current->comm, current->pid);
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	static int do_gc_cnt = 0;
	struct gc_profiler *gcp;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
	unsigned long long start_t_ssa, end_t_ssa, start_phase_t, end_phase_t;
#endif

#ifdef GC_PAGE_POOL
	f2fs_gc_pool_set_victim(sbi, START_BLOCK(sbi, start_segno));
#endif

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
#endif

	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error!!", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;
		sbi->gc_start_ = true;	
	}
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
	sbi->intra_zone_segidx_of_writer = 0;
#endif

	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	struct page *sum_page_array[N_BATCHED_SEGS];
	bool is_terminated_seg[N_BATCHED_SEGS];
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* Preread valid filesystem blocks within segemnt batch */

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

#ifdef GC_PROFILE_TIMESERIAL
		start_phase_t = OS_TimeGetNS_();
#endif
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;

		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;

#ifdef GC_PROFILE_TIMESERIAL
			start_t_ssa = OS_TimeGetNS_();
#endif
	
			/* find segment summary of victim */
			sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
			f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
			sum_page_array[batch_ofs] = sum_page;
			
			if (get_valid_blocks(sbi, segno, false) == 0)
				goto freed_early;
			if (gc_type == BG_GC && __is_large_section(sbi) &&
					migrated >= sbi->migration_granularity)
				goto skip_early;
			if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
				goto skip_early;
	
			sum = page_address(sum_page);
			if (type != GET_SUM_TYPE((&sum->footer))) {
				f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
					 segno, type, GET_SUM_TYPE((&sum->footer)));
				set_sbi_flag(sbi, SBI_NEED_FSCK);
				f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_CORRUPTED_SUMMARY);
				goto skip_early;
			}
			/* Preread valid filesystem blocks in the segment */
			is_terminated_seg[batch_ofs] = false;

			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */

#ifdef GC_PROFILE_TIMESERIAL	
			sbi->intra_zone_segidx_of_prereader = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif

			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
				start_t_ssa = OS_TimeGetNS_();
#endif
				//printk("%s: preread_node_segment start", __func__);
				preread_node_segment(sbi, sum->entries, segno,
									gc_type);
				//printk("%s: preread_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
#endif
			} else {
				//printk("%s: preread_data_segment start", __func__);
				preread_data_segment(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate);
				//printk("%s: preread_data_segment end", __func__);
			}
			continue;

freed_early:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
	
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip_early:
			f2fs_put_page(sum_page, 0);
			is_terminated_seg[batch_ofs] = true;

		}

#ifdef GC_PROFILE_TIMESERIAL
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->preread_phase_lat += end_phase_t - start_phase_t;
		start_phase_t = end_phase_t;
#endif
	
		/* GC valid filesystem blocks within segemnt batch */
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
#ifdef FILTER_GC_REQ
			sbi->gc_last_seg = (batch_no == n_seg_batch-1 && batch_ofs == n_batched_segs-1);
#endif
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
					start_t_ssa = OS_TimeGetNS_();
#endif
					//printk("%s: clean_node_segment start", __func__);
					submitted += clean_node_segment(sbi, sum->entries, segno,
									gc_type);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
					end_t_ssa = OS_TimeGetNS_();
					cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
					cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					submitted += clean_data_segment(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				stat_inc_gc_seg_count(sbi, data_type, gc_type);
				sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				migrated++;
	
freed:
				if (gc_type == FG_GC &&
						get_valid_blocks(sbi, segno, false) == 0)
					seg_freed++;
	
				if (__is_large_section(sbi))
					sbi->next_victim_seg[gc_type] =
						(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
	
		}

#ifdef GC_PROFILE_TIMESERIAL	
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
#endif
	}

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = end_t_ssa;
#endif

	blk_finish_plug(&plug);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->blk_finish_lat += end_t_ssa - start_t_ssa;
#endif

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

//#ifdef GC_PROFILE_TIMESERIAL	
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
//#endif
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif
	return seg_freed;
}

#ifdef GC_PIPELINE
static int do_garbage_collect_pipelined_writing(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	struct blk_plug plug;
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	//printk("%s start: %s %u. start_segno %u. sum_type: %s sec_vblks: %u", __func__, current->comm, current->pid, 
	//	start_segno, 
	//	(type == SUM_TYPE_NODE)? "NODE" : "DATA", 
	//		get_valid_blocks(sbi, start_segno, true)
	//	);
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	unsigned int target_seg_seq = 1;

	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
			/* Waiting for segment preread. */
			//printk("%s: sleep: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			
			//if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	wait_event_interruptible(*wq, kthread_should_stop() || 
			//		(sbi->pipelined_gc_write_wake && 
			//			atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
			//}
			//printk("Bef Start. wake: %d preread_seg_seq: %lu target_seg_seq: %lu n_batched_segs: %lu", 
			//	sbi->pipelined_gc_write_wake, 
			//	atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq, 
			//	n_batched_segs);
			if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
				//printk("Bef Sleep. wake: %d preread_seg_seq: %lu target_seg_seq: %lu n_batched_segs: %lu", 
				//sbi->pipelined_gc_write_wake, 
				//atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq, 
				//n_batched_segs);
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
				//printk("Aft Sleep. wake: %d preread_seg_seq: %lu target_seg_seq: %lu n_batched_segs: %lu", 
				//sbi->pipelined_gc_write_wake, 
				//atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq, 
				//n_batched_segs);
			}
			//printk("%s: wokeup: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);

			//spin_lock(&sbi->pipe_gc_write_wake_lck);
			//if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq + 1) {
			//	sbi->pipelined_gc_write_wake = false;
			//} else {
			//	sbi->pipelined_gc_write_wake = true;
			//}
			//spin_unlock(&sbi->pipe_gc_write_wake_lck);
			

			//printk("Among Start. wake: %d preread_seg_seq: %lu target_seg_seq: %lu n_batched_segs: %lu", 
			//	sbi->pipelined_gc_write_wake, 
			//	atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq, 
			//	n_batched_segs);
			//while(atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	/* Busy waiting. */
			//}
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				//printk("%s: cpuid: %d", __func__, smp_processor_id());
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
					start_t = OS_TimeGetNS_();
#endif
					//printk("%s: clean_node_segment start", __func__);
					submitted += clean_node_segment(sbi, sum->entries, segno,
									gc_type);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
					end_t = OS_TimeGetNS_();
					cur_gcp->node_seg_lat += end_t - start_t;
					cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					submitted += clean_data_segment(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				stat_inc_gc_seg_count(sbi, data_type, gc_type);
				sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				migrated++;
	
freed:
				if (gc_type == FG_GC &&
						get_valid_blocks(sbi, segno, false) == 0) {
					seg_freed++;
				} 
				//else if (gc_type == FG_GC && get_valid_blocks(sbi, segno, false) != 0) {
					//printk("%s: WEIRD!!!!!!!!!!!!!!!!!!!!! vblks: %u", __func__, 
					//get_valid_blocks(sbi, segno, false));
				//}
	
				if (__is_large_section(sbi))
					sbi->next_victim_seg[gc_type] =
						(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
			target_seg_seq ++;
			//printk("%s: Aft Start. wake: %d preread_seg_seq: %lu target_seg_seq: %lu n_batched_segs: %lu", 
			//	__func__, sbi->pipelined_gc_write_wake, 
			//	atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq, 
			//	n_batched_segs);
		}

	}
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
	//printk("%s end soon", __func__);
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL
	end_t = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t - start_t;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
}

#ifdef GC_PIPELINE_BATCHED 

#ifdef GC_DEEP_PIPELINE_BATCHED 
static int do_garbage_collect_batched_deep_pipelined_reading_second_step(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	struct blk_plug plug;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	//struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;
	/* BLOCK_GRANULARITY add */
	unsigned int target_blk_seq = 1; 
	/* BLOCK_GRANULARITY add */
	unsigned int check_target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif

	unsigned int sec_start_blkaddr = START_BLOCK(sbi, start_segno);
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;
	unsigned int batch_start_blkoff;
	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head;
	atomic_set(&sbi->pipe_gc_i->read_step2_done_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
	
	//unsigned int target_batch_seq = 1;

	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("%s: batch_no: %d", __func__, batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		batch_start_blkoff = batch_no * N_BATCHED_SEGS * BLKS_PER_SEG(sbi);
		
		//printk("%s: batch %d start. ", __func__, batch_no);
		
		///* BLOCK_GRANULARITY added */
		///* wait for terminated segment check finished. */
		//if (atomic_read(&sbi->pipe_gc_i->read_step_one_started_batch_seq) < check_target_batch_seq) {
		//	wait_event_interruptible(*wq, kthread_should_stop() || 
		//		(sbi->pipelined_gc_write_wake && 
		//			atomic_read(&sbi->pipe_gc_i->checked_seg_seq) >= check_target_batch_seq));
		//}
		/* FIX (watermark clobber): no per-batch reset of
		 * read_step2_done_blk_seq. The watermark is SECTION-absolute and
		 * monotonic for the whole round now; zeroing it here raced with the
		 * writing_first_step consumer still waiting on the previous batch's
		 * value. Reset happens once, at round init. */
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			if (type == SUM_TYPE_NODE){
//#ifdef GC_PROFILE_TIMESERIAL
//				start_t = OS_TimeGetNS_();
//#endif
				//printk("%s: clean_node_segment start", __func__);
				f2fs_bug_on(sbi, 1);
				//preread_node_segment_second_step(sbi, sum->entries, segno,
				//				gc_type, batch_ofs);
				//printk("%s: clean_node_segment end", __func__);
//#ifdef GC_PROFILE_TIMESERIAL
//				end_t = OS_TimeGetNS_();
//				cur_gcp->node_seg_lat += end_t - start_t;
//				cur_gcp->node_seg_cnt ++;
//#endif
			} else {
				//printk("%s: clean_data_segment start", __func__);
				preread_data_segment_second_step(sbi, gc_list,
								segno, gc_type,
								force_migrate, batch_no, batch_ofs, batch_start_blkoff, sec_start_blkaddr);
				//printk("%s: clean_data_segment end", __func__);
			}
			
			/* BLOCK_GRANULARITY commented */
			//target_seg_seq ++;
			/* BLOCK_GRANULARITY added */
			//check_target_seg_seq ++;
		}
		
		//printk(KERN_EMERG "%s: batch %d end. go sleep\n", __func__, batch_no);
//#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED 
		//printk("%s: thread: %p wq: %p value: %d", __func__, 
		//		sbi->pipelined_gc_write_second_step_thread,
		//		&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq,
		//					sbi->pipelined_gc_write_batch_done_for_read_step2);
		//wait_event_interruptible(sbi->pipelined_gc_write_second_step_thread->prereader_wq, 
		/* Wait for the writing_second_step producer's monotonic batch credit
		 * before advancing to the next batch. Replaces the lost-update-prone
		 * single boolean batch_done_for_read_step2 (see producer hand-off).
		 *
		 * FIX (round-boundary stranding): never wait on the LAST batch.
		 * There is no next batch to pace, and the round handshake only
		 * waits for writing_second_step (pipelined_gc_write_done) — this
		 * thread's completion is not awaited. If we park here on the last
		 * batch, the next round's do_garbage_collect_batched_deep_pipelined
		 * resets read_step2_credit_seq to 0 before we re-check the
		 * condition, stranding this thread forever inside the old round
		 * (observed live: ring untouched at init values, whole pipeline
		 * deadlocked, filebench/kworker D-state in f2fs_balance_fs).
		 * Intermediate batches cannot strand: write step2 cannot finish the
		 * round without this thread producing the remaining batches. */
		if (batch_no < n_seg_batch - 1)
			wait_event_interruptible(sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq,
							atomic_read(&sbi->pipe_gc_i->read_step2_credit_seq) >= batch_no + 1 ||
							kthread_should_stop());
							//sbi->pipelined_gc_write_batch_done_for_read_step2);
//#else
//		wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_step2_wq,
//							sbi->pipelined_gc_write_batch_done_for_read_step2);
//#endif
		//while(!sbi->pipelined_gc_write_batch_done_for_read_step2) {
		//}
		/* teardown: if the writer bailed on kthread_stop() without setting
		 * the flag, exit the batch loop so this thread's kthread_stop() can
		 * complete instead of wedging umount/rm. */
		if (kthread_should_stop())
			break;
		smp_mb();
		/* credit is monotonic; nothing to clear */
		//printk(KERN_EMERG "%s: batch %d end. woke up.\n", __func__, batch_no);

	}

	blk_finish_plug(&plug);

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: let the gc pre-reader run */
	return seg_freed;
} /* End of do_garbage_collect_batched_deep_pipelined_reading_second_step(struct f2fs_sb_info *sbi) */

static int do_garbage_collect_batched_deep_pipelined_writing(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	struct blk_plug plug;
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;
	/* BLOCK_GRANULARITY add */
	unsigned int target_blk_seq = 1; 
	/* BLOCK_GRANULARITY add */
	unsigned int check_target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		//printk("%s: batch %d start. ", __func__, batch_no);
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
			/* Waiting for segment preread. */
			//printk("%s: sleep: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			/* BLOCK_GRANULARITY commented */
			/*
			if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
			}
			*/
			/* BLOCK_GRANULARITY added */
			/* wait for terminated segment check finished. */
			if (atomic_read(&sbi->pipe_gc_i->checked_seg_seq) < check_target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->checked_seg_seq) >= check_target_seg_seq));
			}
			//printk("%s: check_target_seg_seq: %llu checked_seg_seq: %llu . terminated checked by writer.", __func__, 
			//	check_target_seg_seq, atomic_read(&pgci->checked_seg_seq));
			//printk("%s: wokeup: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			//sbi->pipelined_gc_write_wake = false;
			//while(atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	/* Busy waiting. */
			//}
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
					start_t = OS_TimeGetNS_();
#endif
					//printk("%s: clean_node_segment start", __func__);
					submitted += clean_node_segment_first_step(sbi, sum->entries, segno,
									gc_type, batch_ofs);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL
					end_t = OS_TimeGetNS_();
					cur_gcp->node_seg_lat += end_t - start_t;
					cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					submitted += clean_data_segment_single_step(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate, batch_ofs);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				stat_inc_gc_seg_count(sbi, data_type, gc_type);
				sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				migrated++;
	
freed:
				if (gc_type == FG_GC &&
						get_valid_blocks(sbi, segno, false) == 0) {
					seg_freed++;
				} else if (gc_type == FG_GC &&                   
	                       	get_valid_blocks(sbi, segno, false) != 0) {
					printk("%s: not reclaimed!!!!! segno: %lu vblks: %lu", __func__, segno, 
							get_valid_blocks(sbi, segno, false));
				}
				if (__is_large_section(sbi))
					sbi->next_victim_seg[gc_type] =
						(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
			/* BLOCK_GRANULARITY commented */
			//target_seg_seq ++;
			/* BLOCK_GRANULARITY added */
			check_target_seg_seq ++;
		}
		//printk("%s: batch %d end. ", __func__, batch_no);
		sbi->pipelined_gc_write_batch_done = true;
		wake_up_all(&gc_th->prereader_wq);
		/* FIX (watermark clobber): do not reset read_step2_done_blk_seq
		 * here — this thread is a CONSUMER of that watermark; zeroing it
		 * from the write side raced with the reader's publications
		 * (SECTION-absolute, reset only at round init). */
		sbi->pipelined_gc_write_batch_done_for_read_step2 = true;
		wake_up_all(&gc_th->prereader_step2_wq);

	}
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t - start_t;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
}

#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED  
static int do_garbage_collect_batched_deep_pipelined_writing_first_step(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	//struct blk_plug plug;
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;
	/* BLOCK_GRANULARITY add */
	unsigned int target_blk_seq = 1; 
	/* BLOCK_GRANULARITY add */
	unsigned int check_target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	//blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("%s: batch_no: %d", __func__, batch_no);
		atomic_set(&sbi->pipe_gc_i->prewrite_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		//printk("%s: batch %d start. ", __func__, batch_no);
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
			/* Waiting for segment preread. */
			//printk("%s: sleep: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			/* BLOCK_GRANULARITY commented */
			/*
			if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
			}
			*/
			/* BLOCK_GRANULARITY added */
			/* wait for terminated segment check finished. */
			if (atomic_read(&sbi->pipe_gc_i->checked_seg_seq) < check_target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->checked_seg_seq) >= check_target_seg_seq));
			}
			//printk("%s: batch_no: %d batch_ofs: %d", __func__, batch_no, batch_ofs);
			//printk("%s: check_target_seg_seq: %llu checked_seg_seq: %llu . terminated checked by writer.", __func__, 
			//	check_target_seg_seq, atomic_read(&pgci->checked_seg_seq));
			//printk("%s: wokeup: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			//sbi->pipelined_gc_write_wake = false;
			//while(atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	/* Busy waiting. */
			//}
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
					//start_t = OS_TimeGetNS_();
#endif
					f2fs_bug_on(sbi, 1);
					//printk("%s: clean_node_segment start", __func__);
					//submitted += clean_node_segment_first_step(sbi, sum->entries, segno,
					//				gc_type, batch_ofs);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL
					//end_t = OS_TimeGetNS_();
					//cur_gcp->node_seg_lat += end_t - start_t;
					//cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					//bool is_last_seg = (batch_no == n_seg_batch - 1) && 
					//				(batch_ofs == n_batched_segs - 1);
					submitted += clean_data_segment_first_step(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate, batch_ofs);//, is_last_seg);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				//stat_inc_gc_seg_count(sbi, data_type, gc_type);
				//sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				//migrated++;
	
freed:
				//if (gc_type == FG_GC &&
				//		get_valid_blocks(sbi, segno, false) == 0) {
				//	seg_freed++;
				//} else if (gc_type == FG_GC &&                   
	            //           	get_valid_blocks(sbi, segno, false) != 0) {
				//	printk("%s: not reclaimed!!!!! segno: %lu vblks: %lu", __func__, segno, 
				//			get_valid_blocks(sbi, segno, false));
				//}
				//if (__is_large_section(sbi))
				//	sbi->next_victim_seg[gc_type] =
				//		(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
			/* BLOCK_GRANULARITY commented */
			//target_seg_seq ++;
			/* BLOCK_GRANULARITY added */
			check_target_seg_seq ++;
		}
		//printk("%s: batch %d end. ", __func__, batch_no);
	
		sbi->pipelined_gc_write_second_step_wake = true;
		atomic_set(&pgci->prewrite_blk_seq, 
			batch_ofs * BLKS_PER_SEG(sbi) + 1); 
			//batch_ofs * BLKS_PER_SEG(sbi) + off + 1); 
		smp_mb();
		wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
		//printk("%s: prewrite_blk_seq: %llu . wake 2nd writer", __func__, 
		//		atomic_read(&pgci->prewrite_blk_seq));
		
		//sbi->pipelined_gc_write_batch_done = true;
		//wake_up_all(&gc_th->prereader_wq);
		//
		//atomic_set(&sbi->pipe_gc_i->read_step2_done_blk_seq, 0);
		//sbi->pipelined_gc_write_batch_done_for_read_step2 = true;
		//wake_up_all(&gc_th->prereader_step2_wq);
		
		/* Let second-step writer know that batch is finished
			so do not stuck in write pass buffer. */
		atomic_inc(&sbi->pipe_gc_i->prewrite_batch_seq);
		/* FIX (lost wakeup): wake the second-step writer, which may be parked
		 * in the writer_pass_buffer wait (clean_data_segment_second_step) on
		 * exactly this batch-done signal. Advancing head already wakes it, but
		 * the batch-done inc did not -- without this, a drained ring + batch
		 * done would deadlock (D-state hang) instead of the old soft lockup. */
		smp_mb();
		wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);

		/* Wait for gc write second-step thread to finish */
		//printk(KERN_EMERG "%s: batch %d end. go sleep. value: %d\n", __func__, batch_no, 
		//			sbi->pipelined_gc_write_batch_done_for_write_step1);
		//while(!sbi->pipelined_gc_write_batch_done_for_write_step1) {
		//}
		//printk("%s: thread: %p wq: %p value: %d", __func__, 
		//			sbi->pipelined_gc_write_second_step_thread, 	
		//		&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq,
		//					sbi->pipelined_gc_write_batch_done_for_write_step1);
		//wait_event_interruptible(sbi->pipelined_gc_write_second_step_thread->prereader_wq,	
		/* Wait for the writing_second_step producer's monotonic batch credit
		 * (replaces the lost-update-prone single boolean).
		 *
		 * FIX (round-boundary stranding): never wait on the LAST batch —
		 * same reason as the reading_second_step credit wait: this thread's
		 * completion is not part of the round handshake, so parking here on
		 * the last batch races with the next round's credit reset and
		 * strands this thread inside the old round forever. */
		if (batch_no < n_seg_batch - 1)
			wait_event_interruptible(sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq,
							atomic_read(&sbi->pipe_gc_i->write_step1_credit_seq) >= batch_no + 1 ||
							kthread_should_stop());
		//					sbi->pipelined_gc_write_batch_done_for_write_step1);
		//printk(KERN_EMERG "%s: batch %d end. woke up. value: %d\n", __func__, batch_no, sbi->pipelined_gc_write_batch_done_for_write_step1);
		/* teardown: exit the batch loop on kthread_stop() so the writer
		 * bailing without setting the flag cannot wedge umount/rm. */
		if (kthread_should_stop())
			break;
		smp_mb();
		/* credit is monotonic; nothing to clear */
		//printk("%s: batch %d end. woke up.", __func__, batch_no);

	}
		
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
//	if (submitted)
//		f2fs_submit_merged_write(sbi, data_type);
//#ifdef GC_PROFILE_TIMESERIAL	
//	end_t = OS_TimeGetNS_();
//	cur_gcp->merged_write_lat += end_t - start_t;
//#endif
//
//	//blk_finish_plug(&plug);
//
//	if (migrated)
//		stat_inc_gc_sec_count(sbi, data_type, gc_type);
//#ifdef WAF
//	sbi->gc_written_blk += submitted;
//	sbi->total_gc_written_blk += submitted;
//#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

//	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
//	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
} /* End of do_garbage_collect_batched_deep_pipelined_writing_first_step(struct f2fs_sb_info *sbi) */

static int do_garbage_collect_batched_deep_pipelined_writing_second_step(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	struct blk_plug plug;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	struct f2fs_pipelined_gc_write_second_step_kthread *gc_th 
											= sbi->pipelined_gc_write_second_step_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;
	/* BLOCK_GRANULARITY add */
	unsigned int target_blk_seq = 1; 
	/* BLOCK_GRANULARITY add */
	unsigned int check_target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	unsigned int sec_start_blkaddr = START_BLOCK(sbi, start_segno);
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;
	unsigned int batch_start_blkoff;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("%s: batch_no: %d", __func__, batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		batch_start_blkoff = batch_no * N_BATCHED_SEGS * BLKS_PER_SEG(sbi);
		//printk("%s: batch %d start. ", __func__, batch_no);
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
//#ifdef GC_PROFILE_TIMESERIAL	
//			sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
//#endif
			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */
			if (type == SUM_TYPE_NODE){
//#ifdef GC_PROFILE_TIMESERIAL
//				start_t = OS_TimeGetNS_();
//#endif
				f2fs_bug_on(sbi, 1);
				//printk("%s: clean_node_segment start", __func__);
//				submitted += clean_node_segment_second_step(sbi, sum->entries, segno,
//								gc_type, batch_ofs);
				//printk("%s: clean_node_segment end", __func__);
//#ifdef GC_PROFILE_TIMESERIAL
//				end_t = OS_TimeGetNS_();
//				cur_gcp->node_seg_lat += end_t - start_t;
//				cur_gcp->node_seg_cnt ++;
//#endif
			} else {
				//printk("%s: clean_data_segment start batch_no: %lu batch_ofs: %lu", __func__, batch_no, batch_ofs);
				submitted += clean_data_segment_second_step(sbi, gc_list,
								segno, gc_type,
								force_migrate, batch_no, batch_ofs, 
								batch_start_blkoff, sec_start_blkaddr);
				//printk("%s: clean_data_segment end batch_no: %lu batch_ofs: %lu", __func__, batch_no, batch_ofs);
			}
	
			stat_inc_gc_seg_count(sbi, data_type, gc_type);
			sbi->gc_reclaimed_segs[sbi->gc_mode]++;
			migrated++;
	
freed:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0) {
				seg_freed++;
			} else if (gc_type == FG_GC &&                   
	                   	get_valid_blocks(sbi, segno, false) != 0) {
				printk("%s: not reclaimed!!!!! segno: %lu vblks: %lu", __func__, segno, 
						get_valid_blocks(sbi, segno, false));
			}
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
			/* BLOCK_GRANULARITY commented */
			//target_seg_seq ++;
			/* BLOCK_GRANULARITY added */
			check_target_seg_seq ++;
		}
		//printk(KERN_EMERG "%s: batch %d end wake up all. \n", __func__, batch_no);
		sbi->pipelined_gc_write_batch_done = true;
		wake_up(&gc_th->prereader_wq);

		/* FIX (watermark clobber): do not reset read_step2_done_blk_seq at
		 * this batch boundary. Under delete-heavy load (rm -rf) the reader
		 * runs ahead (all victim blocks dead, no reads to issue) and has
		 * already published watermarks the writing_first_step consumer is
		 * about to wait on; zeroing them here stranded first_step at its
		 * done_blk_seq wait and deadlocked the round (observed live:
		 * done=0, first_step parked on batch 1 block 0, ckpt/rm D-state).
		 * The watermark is SECTION-absolute now; round init resets it. */
		//printk("%s: thread: %p wq: %p value: %d", __func__,
		//		sbi->pipelined_gc_write_second_step_thread,
		//		&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq,
		//					sbi->pipelined_gc_write_batch_done_for_read_step2);
		/*
		 * Batch-credit hand-off to the reading_second_step consumer.
		 *
		 * The reader is bounded to at most one batch ahead of this writer
		 * (depth-1 shared batch buffer). Publish a *monotonic* credit
		 * counter instead of the old single boolean: the reader advances
		 * to batch N+1 once read_step2_credit_seq >= N+1. A monotonic
		 * counter cannot lose an update the way the boolean did, so the
		 * racy waitqueue_active() pre-spin that waited for the reader to
		 * park is no longer needed. That spin was the hang: when the reader
		 * was mid-work (not parked) it never terminated, pinning the CPU
		 * (soft lockup) or, with cond_resched, livelocking GC so filebench
		 * blocked in f2fs_balance_fs and unlink/umount wedged. This publish
		 * never blocks, so teardown (kthread_stop) is never wedged either.
		 */
		atomic_set(&sbi->pipe_gc_i->read_step2_credit_seq, batch_no + 1);
		smp_mb();
		wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		//sbi->pipelined_gc_write_batch_done_for_read_step2 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		////sbi->pipelined_gc_write_batch_done_for_read_step2 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		////sbi->pipelined_gc_write_batch_done_for_read_step2 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		//printk("%s: wake up done. read_step2 value: %d", __func__, 
		//			sbi->pipelined_gc_write_batch_done_for_read_step2
		//			);
		//printk("%s: thread: %p wq: %p value: %d", __func__,
		//			sbi->pipelined_gc_write_second_step_thread,
		//		&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq,
		//					sbi->pipelined_gc_write_batch_done_for_write_step1);
		/* Same monotonic batch-credit hand-off for the writing_first_step
		 * consumer (see the read_step2 hand-off above for rationale). */
		atomic_set(&sbi->pipe_gc_i->write_step1_credit_seq, batch_no + 1);
		smp_mb();
		wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		////sbi->pipelined_gc_write_batch_done_for_write_step1 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		////sbi->pipelined_gc_write_batch_done_for_write_step1 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		////sbi->pipelined_gc_write_batch_done_for_write_step1 = true;
		//smp_mb();
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		//printk("%s: wake up done. write_step1 value: %d", __func__, 
		//			sbi->pipelined_gc_write_batch_done_for_write_step1 
		//			);
		
		//smp_mb();
		//wake_up_all(&gc_th->prereader_wq);
		//wake_up_interruptible_all(&gc_th->prereader_wq);
		
		
		//if (waitqueue_active(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq)) {
		//	printk("%s: bef wake up. write_step1 value: %d", __func__, 
		//					sbi->pipelined_gc_write_batch_done_for_write_step1 
		//					);
		//	printk("%s: bef wake up. write_step1 thread: %p wq: %p value: %d wq_: %p", __func__, 
		//					gc_th,
		//					&gc_th->prewriter_step1_wq,
		//					sbi->pipelined_gc_write_batch_done_for_write_step1, 
		//					&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq
		//					);
		//	sbi->pipelined_gc_write_batch_done_for_write_step1 = true;
		//	smp_mb();
		//	wake_up(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		//	printk("%s: wake up done. write_step1 value: %d", __func__, 
		//					sbi->pipelined_gc_write_batch_done_for_write_step1 
		//					);
		//	//printk("%s: wake up done. write_step1 thread: %p wq: %p value: %d wq_: %p", __func__, 
		//	//				gc_th,
		//	//				&gc_th->prewriter_step1_wq,
		//	//				sbi->pipelined_gc_write_batch_done_for_write_step1, 
		//	//				&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq
		//	//				);
		//} else {
		//	//printk("%s: not wake up. write_step1 thread: %p wq: %p value: %d wq_: %p", __func__, 
		//	//				gc_th,
		//	//				&gc_th->prewriter_step1_wq,
		//	//				sbi->pipelined_gc_write_batch_done_for_write_step1, 
		//	//				&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq
		//	//				);
		//	printk("%s: not wake up. write_step1 value: %d", __func__, 
		//					sbi->pipelined_gc_write_batch_done_for_write_step1
		//					);
		//	sbi->pipelined_gc_write_batch_done_for_write_step1 = true;
		//	smp_mb();
		//	wake_up(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		//}

		
		//smp_mb();
		//printk("%s: read_step2 thead: %p wq: %p value: %d wq_: %p", __func__, 
		//					gc_th,
		//					&gc_th->prereader_step2_wq,
		//					sbi->pipelined_gc_write_batch_done_for_read_step2,
		//					&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq
		//					);
		//wake_up_all(&gc_th->prereader_step2_wq);
		//wake_up_interruptible_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		//if (waitqueue_active(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq)) {
		//	wake_up(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		//	//printk("%s: wake up done. read_step2 thead: %p wq: %p value: %d wq_: %p", __func__, 
		//	//				gc_th,
		//	//				&gc_th->prereader_step2_wq,
		//	//				sbi->pipelined_gc_write_batch_done_for_read_step2,
		//	//				&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq
		//	//				);
		//} else {
		//	//printk("%s: not wake up. read_step2 thead: %p wq: %p value: %d wq_: %p", __func__, 
		//	//				gc_th,
		//	//				&gc_th->prereader_step2_wq,
		//	//				sbi->pipelined_gc_write_batch_done_for_read_step2,
		//	//				&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq
		//	//				);
		//}
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		
		//atomic_set(&sbi->pipe_gc_i->read_step2_done_blk_seq, 0);
		//printk("%s: write_step1 thread: %p wq: %p value: %d wq_: %p", __func__, 
		//					gc_th,
		//					&gc_th->prewriter_step1_wq,
		//					sbi->pipelined_gc_write_batch_done_for_write_step1, 
		//					&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq
		//					);
		//smp_mb();
		//wake_up_all(&gc_th->prewriter_step1_wq);
		//wake_up_interruptible_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		//wake_up_all(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		
		//wake_up(&gc_th->prereader_wq);
		//smp_mb();
		//wake_up(&sbi->pipelined_gc_write_second_step_thread->prereader_step2_wq);
		//smp_mb();
		//wake_up(&sbi->pipelined_gc_write_second_step_thread->prewriter_step1_wq);
		//printk(KERN_EMERG "%s: batch %d end. wake up all done.\n", __func__, batch_no);

	}
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t - start_t;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
} /* End of do_garbage_collect_batched_deep_pipelined_writing_second_step(struct f2fs_sb_info *sbi) */
#endif
#endif

#if defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED ) || defined(GC_DEEP_PIPELINE_BATCHED)
static int do_garbage_collect_batched_block_granularity_pipelined_writing(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	struct blk_plug plug;
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;
	/* BLOCK_GRANULARITY add */
	unsigned int target_blk_seq = 1; 
	/* BLOCK_GRANULARITY add */
	unsigned int check_target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
			/* Waiting for segment preread. */
			//printk("%s: sleep: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			/* BLOCK_GRANULARITY commented */
			/*
			if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
			}
			*/
			/* BLOCK_GRANULARITY added */
			/* wait for terminated segment check finished. */
			if (atomic_read(&sbi->pipe_gc_i->checked_seg_seq) < check_target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->checked_seg_seq) >= check_target_seg_seq));
			}
			//printk("%s: check_target_seg_seq: %llu checked_seg_seq: %llu . terminated checked by writer.", __func__, 
			//	check_target_seg_seq, atomic_read(&pgci->checked_seg_seq));
			//printk("%s: wokeup: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			//sbi->pipelined_gc_write_wake = false;
			//while(atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	/* Busy waiting. */
			//}
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
					start_t = OS_TimeGetNS_();
#endif
					//printk("%s: clean_node_segment start", __func__);
					submitted += clean_node_segment_blk_gra(sbi, sum->entries, segno,
									gc_type, batch_ofs);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL
					end_t = OS_TimeGetNS_();
					cur_gcp->node_seg_lat += end_t - start_t;
					cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					submitted += clean_data_segment_blk_gra(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate, batch_ofs);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				stat_inc_gc_seg_count(sbi, data_type, gc_type);
				sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				migrated++;
	
freed:
				if (gc_type == FG_GC &&
						get_valid_blocks(sbi, segno, false) == 0)
					seg_freed++;
	
				if (__is_large_section(sbi))
					sbi->next_victim_seg[gc_type] =
						(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
			/* BLOCK_GRANULARITY commented */
			//target_seg_seq ++;
			/* BLOCK_GRANULARITY added */
			check_target_seg_seq ++;
		}
		sbi->pipelined_gc_write_batch_done = true;
		wake_up_all(&gc_th->prereader_wq);

	}
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t - start_t;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
}
#endif

static int do_garbage_collect_batched_pipelined_writing(struct f2fs_sb_info *sbi)
{
	/* Get pipelined gc info from gc pre-reader */
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	//struct blk_plug *plug = pgci->plug;
	struct blk_plug plug;
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	unsigned int start_segno = pgci->start_segno;
	unsigned int end_segno = pgci->end_segno;
	struct gc_inode_list *gc_list = pgci->gc_list;
	int gc_type = pgci->gc_type;
	bool force_migrate = pgci->force_migrate;

	struct page *sum_page;
	struct f2fs_summary_block *sum;
	unsigned int segno = start_segno;
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	
	struct f2fs_pipelined_gc_write_kthread *gc_th = sbi->pipelined_gc_write_thread;
	
	static int do_gc_cnt = 0;
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_phase_t, end_phase_t;
	unsigned long long start_t, end_t;
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
	unsigned int target_seg_seq = 1;

#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_writer = 0;
#endif
	
	//printk("%s: cpuid: %d", __func__, smp_processor_id());
	
	blk_start_plug(&plug);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
#ifdef GC_PROFILE_TIMESERIAL
	start_phase_t = OS_TimeGetNS_();
#endif
	
	wait_queue_head_t *wq = &sbi->pipelined_gc_write_thread->gc_wait_queue_head;
	
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* GC valid filesystem blocks within segemnt batch */
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("batch_no: %d batch_ofs: %d", batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;
			
			/* Waiting for segment preread. */
			//printk("%s: sleep: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			if (atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
				wait_event_interruptible(*wq, kthread_should_stop() || 
					(sbi->pipelined_gc_write_wake && 
						atomic_read(&sbi->pipe_gc_i->preread_seg_seq) >= target_seg_seq));
			}
			//printk("%s: wokeup: wake: %d preread_seg_seq: %d target_seg_seq: %d ", 
			//		__func__, sbi->pipelined_gc_write_wake, 
			//		atomic_read(&sbi->pipe_gc_i->preread_seg_seq), target_seg_seq
			//		);
			//sbi->pipelined_gc_write_wake = false;
			//while(atomic_read(&sbi->pipe_gc_i->preread_seg_seq) < target_seg_seq) {
			//	/* Busy waiting. */
			//}
			if (!is_terminated_seg[batch_ofs]) {
#ifdef GC_PROFILE_TIMESERIAL	
				sbi->intra_zone_segidx_of_writer = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
				sum_page = sum_page_array[batch_ofs];
				sum = page_address(sum_page);
				/*
				 * this is to avoid deadlock:
				 * - lock_page(sum_page)         - f2fs_replace_block
				 *  - check_valid_map()            - down_write(sentry_lock)
				 *   - down_read(sentry_lock)     - change_curseg()
				 *                                  - lock_page(sum_page)
				 */
				if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
					start_t = OS_TimeGetNS_();
#endif
					//printk("%s: clean_node_segment start", __func__);
					submitted += clean_node_segment(sbi, sum->entries, segno,
									gc_type);
					//printk("%s: clean_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL
					end_t = OS_TimeGetNS_();
					cur_gcp->node_seg_lat += end_t - start_t;
					cur_gcp->node_seg_cnt ++;
#endif
				} else {
					//printk("%s: clean_data_segment start", __func__);
					submitted += clean_data_segment(sbi, sum->entries, gc_list,
									segno, gc_type,
									force_migrate);
					//printk("%s: clean_data_segment end", __func__);
				}
	
				stat_inc_gc_seg_count(sbi, data_type, gc_type);
				sbi->gc_reclaimed_segs[sbi->gc_mode]++;
				migrated++;
	
freed:
				if (gc_type == FG_GC &&
						get_valid_blocks(sbi, segno, false) == 0)
					seg_freed++;
	
				if (__is_large_section(sbi))
					sbi->next_victim_seg[gc_type] =
						(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
				f2fs_put_page(sum_page, 0);
			}
			target_seg_seq ++;
		}
		sbi->pipelined_gc_write_batch_done = true;
		wake_up_all(&gc_th->prereader_wq);

	}
#ifdef GC_PROFILE_TIMESERIAL
	end_phase_t = OS_TimeGetNS_();
	cur_gcp->move_phase_lat += end_phase_t - start_phase_t;
	start_t = end_phase_t;
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t - start_t;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif

	/* TODO: pipeliend gc writer post-processing. pass info to gc pre-reader */
	pgci->seg_freed_by_writer = seg_freed;
	
	/* TODO: let the gc pre-reader run */
	return seg_freed;
}
#endif


static int do_garbage_collect_pipelined(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
#ifdef CHECK_GC_RACE
	printk("%s start: %s %u", __func__, current->comm, current->pid);
#endif
	//printk("%s start: %s %u. start_segno %u. sum_type: %s sec_vblks: %u", __func__, current->comm, current->pid, 
	//		start_segno, 
	//		(type == SUM_TYPE_NODE)? "NODE" : "DATA",
	//		get_valid_blocks(sbi, start_segno, true)
	//		);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long start_t_ssa, end_t_ssa, start_phase_t, end_phase_t;
#endif

#ifdef GC_PAGE_POOL
	f2fs_gc_pool_set_victim(sbi, START_BLOCK(sbi, start_segno));
#endif

	//printk("%s: cpuid: %d", __func__, smp_processor_id());

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));
	
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
#endif


	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error!!", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
#endif

	blk_start_plug(&plug);

	/* TODO: prepare gc_pipeline info for writer thread */
	pgci->plug = &plug;
	pgci->start_segno = start_segno;
	pgci->end_segno = end_segno;
	pgci->gc_list = gc_list;
	pgci->gc_type = gc_type;
	pgci->force_migrate = force_migrate;

	/* TODO: wake up writer thread */	
	sbi->pipelined_gc_write_done = 0;
	//sbi->pipelined_gc_write_wake = true;
	sbi->pipelined_gc_write_thread_wake = true;
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* Preread valid filesystem blocks within segemnt batch */
#ifdef GC_PROFILE_TIMESERIAL
		start_phase_t = OS_TimeGetNS_();
#endif
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		//printk("%s: n_batched_segs: %lu", __func__, n_batched_segs);

		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("%s: batch_no: %d batch_ofs: %d", __func__, batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;

#ifdef GC_PROFILE_TIMESERIAL
			start_t_ssa = OS_TimeGetNS_();
#endif
			/* find segment summary of victim */
			sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
			f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
			sum_page_array[batch_ofs] = sum_page;
			
			if (get_valid_blocks(sbi, segno, false) == 0) {
				//printk("%s: SKIP 1", __func__);
				goto freed_early_pipe;
			}
			if (gc_type == BG_GC && __is_large_section(sbi) &&
					migrated >= sbi->migration_granularity) {
				/* TODO: migrated should be considered. In this function, migrated is zero. */
				//printk("%s: SKIP 2. migrated: %lu mg_gra: %lu", __func__, migrated, sbi->migration_granularity);
				goto skip_early_pipe;
			}
			if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi))) {
				//printk("%s: SKIP 3. sum: %d cp: %d", __func__, !PageUptodate(sum_page), f2fs_cp_error(sbi));
				goto skip_early_pipe;
			}
	
			sum = page_address(sum_page);
			if (type != GET_SUM_TYPE((&sum->footer))) {
				f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
					 segno, type, GET_SUM_TYPE((&sum->footer)));
				set_sbi_flag(sbi, SBI_NEED_FSCK);
				f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_CORRUPTED_SUMMARY);
				//printk("%s: SKIP 4. ", __func__);
				goto skip_early_pipe;
			}
			/* Preread valid filesystem blocks in the segment */
			is_terminated_seg[batch_ofs] = false;

			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */

#ifdef GC_PROFILE_TIMESERIAL	
			sbi->intra_zone_segidx_of_prereader = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif
	
			//printk("%s: cpuid: %d", __func__, smp_processor_id());

			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL
				start_t_ssa = OS_TimeGetNS_();
#endif
				//printk("%s: preread_node_segment start", __func__);
				preread_node_segment(sbi, sum->entries, segno,
									gc_type);
				//printk("%s: preread_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
#endif
			} else {
				//printk("%s: preread_data_segment start", __func__);
				preread_data_segment(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate);
				//printk("%s: preread_data_segment end", __func__);
			}
			//sbi->pipe_gc_i->preread_seg_seq ++;
			atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			/* TODO: wake up writer thread */
			//spin_lock(&sbi->pipe_gc_write_wake_lck);
			sbi->pipelined_gc_write_wake = true;
			//spin_unlock(&sbi->pipe_gc_write_wake_lck);
			smp_mb();
			//printk("%s: 1. batch_no: %lu preread_seg_seq: %d ", 
			//		__func__, batch_no, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
			//		);

			//printk("%s: wake up. preread_seg_seq: %d ", __func__, 
			//	atomic_read(&sbi->pipe_gc_i->preread_seg_seq));
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			continue;

freed_early_pipe:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
	
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip_early_pipe:
			f2fs_put_page(sum_page, 0);
			is_terminated_seg[batch_ofs] = true;
			//printk("%s: 2. batch_no: %lu preread_seg_seq: %d ", 
			//		__func__, batch_no, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
			//		);
			//printk("%s: SKIP!!! preread_seg_seq: %d ", 
			//		__func__, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
			//		);
			atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			//spin_lock(&sbi->pipe_gc_write_wake_lck);
			sbi->pipelined_gc_write_wake = true;
			//spin_unlock(&sbi->pipe_gc_write_wake_lck);
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

		}

#ifdef GC_PROFILE_TIMESERIAL
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->preread_phase_lat += end_phase_t - start_phase_t;
		start_phase_t = end_phase_t;
#endif
		/* Let the gc write thread to perform move GCed pages. */
	}
	//if (seg_freed == end_segno - start_segno && get_valid_blocks(sbi, start_segno, true) != 0) {
	//	unsigned int tmp_vblks_ = 0;
	//	int tmp__i = 0;
	//	for (tmp__i; tmp__i < end_segno - start_segno; tmp__i ++) {
	//		tmp_vblks_ += get_valid_blocks(sbi, start_segno + tmp__i, false);
	//	}
	//	printk("%s: ERROR! sec_vblks: %lu segs_vblk_sum: %lu ", 
	//	__func__, get_valid_blocks(sbi, start_segno, true), tmp_vblks_);
	//}
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	blk_finish_plug(&plug);
	
	//printk("%s: bef WAIT", __func__);
	wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_wq, sbi->pipelined_gc_write_done);
	//printk("%s: aft WAIT", __func__);
//	printk("%s: bef DEFINE_WAIT", __func__);
//	/* TODO: Wait for gc write thread to finish */
//	DEFINE_WAIT(wait);
//	printk("%s: bef prepare_to_wait", __func__);
//	prepare_to_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait,
//				TASK_UNINTERRUPTIBLE);
//	printk("%s: bef io_schedule", __func__);
//	io_schedule();
//	printk("%s: bef finish_wait", __func__);
//	finish_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait);
//	printk("%s: aft finish_wait", __func__);

	/* pipelined GC post processing */
	seg_freed += pgci->seg_freed_by_writer;
	//sbi->pipe_gc_i->preread_seg_seq = 0;
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif
	return seg_freed;
}

#ifdef GC_PIPELINE_BATCHED 
#ifdef GC_DEEP_PIPELINE_BATCHED 
//#elif defined(GC_BLOCK_GRANULARITY_PIPELINE_BATCHED)
static int do_garbage_collect_batched_deep_pipelined(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
#ifdef CHECK_GC_RACE
	printk("%s start: %s %u", __func__, current->comm, current->pid);
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long start_t_ssa, end_t_ssa, start_phase_t, end_phase_t;
#endif

#ifdef GC_PAGE_POOL
	f2fs_gc_pool_set_victim(sbi, START_BLOCK(sbi, start_segno));
#endif

	//printk("%s: cpuid: %d", __func__, smp_processor_id());

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));
	

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
#endif


	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error!!", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
#endif

	//blk_start_plug(&plug);

	/* prepare gc_pipeline info for other thread */
	atomic_set(&pgci->preread_seg_seq, 0);
	atomic_set(&pgci->checked_seg_seq, 0);
	atomic_set(&pgci->preread_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
	atomic_set(&pgci->preread_batch_seq, 0);
	atomic_set(&pgci->read_step2_credit_seq, 0);	/* reset batch credit for new round */
	/* FIX (stale block credit across rounds): reset the read->write block
	 * completion seq BEFORE waking the pipeline threads. The reader thread
	 * resets it only once it starts the new round (it is woken async), so
	 * without this, writing_first_step can consume the PREVIOUS round's
	 * final value and sail past blocks of the new victim section that were
	 * never read ("unexpected inode NULL" flood, segments not reclaimed,
	 * "freed 0/512 segs"). */
	atomic_set(&pgci->read_step2_done_blk_seq, 0);
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
	atomic_set(&pgci->prewrite_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
	atomic_set(&pgci->prewrite_batch_seq, 0);
	atomic_set(&pgci->write_step1_credit_seq, 0);	/* reset batch credit for new round */
#endif
	pgci->plug = &plug;
	pgci->start_segno = start_segno;
	pgci->end_segno = end_segno;
	pgci->gc_list = gc_list;
	pgci->gc_type = gc_type;
	pgci->force_migrate = force_migrate;
	atomic_set(&pgci->reader_pass_buffer_head, 0);
	atomic_set(&pgci->reader_pass_buffer_tail, N_READ_PASS_ENTRY - 1);
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED 
	atomic_set(&pgci->writer_pass_buffer_head, 0);
	atomic_set(&pgci->writer_pass_buffer_tail, N_WRITE_PASS_ENTRY - 1);
#endif

	/* Setup interaction between gc first step reader and second step reader thread */	
	/* will be used to wake up reader after 2nd step reader thread all batch completion. */
	sbi->pipelined_gc_read_second_step_done = 0;	/* round-end handshake: orchestrator waits on this */
	/* will be used to wake up reader after writer thread single batch completion. */
	sbi->pipelined_gc_write_batch_done_for_read_step2 = 0; /*in write pipelined, step 2 writer will update this.*/
	/* wake up second step reader thread */
	sbi->pipelined_gc_read_second_step_thread_wake = true;
	wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
	
	/* Setup interaction between gc first step reader and writer thread */	
	/* will be used to wake up reader after writer thread all batch completion. */
	sbi->pipelined_gc_write_done = 0;	/* in write pipelined, step 2 writer will update this. */
	/* will be used to wake up reader after writer thread single batch completion. */
	sbi->pipelined_gc_write_batch_done = 0; /*in write pipelined, step 2 writer will update this.*/
	/* wake up writer thread */
	sbi->pipelined_gc_write_thread_wake = true;
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED 
	/* Setup interaction between gc first step reader and second step reader thread */	
	/* will be used to wake up writer_step1 after writer_step2 thread single batch completion. */
	sbi->pipelined_gc_write_batch_done_for_write_step1 = 0;
	/* wake up second step writer thread */
	sbi->pipelined_gc_write_second_step_thread_wake = true;
	wake_up(&sbi->pipelined_gc_write_second_step_thread->gc_wait_queue_head);
#endif

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("%s: batch_no: %d", __func__, batch_no);
		atomic_set(&sbi->pipe_gc_i->preread_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
		/* Preread valid filesystem blocks within segemnt batch */
#ifdef GC_PROFILE_TIMESERIAL
		start_phase_t = OS_TimeGetNS_();
#endif
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif
		//printk("%s: batch %d start. ", __func__, batch_no);
		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("%s: batch_no: %d batch_ofs: %d", __func__, batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;

#ifdef GC_PROFILE_TIMESERIAL	
			start_t_ssa = OS_TimeGetNS_();
#endif
			/* find segment summary of victim */
			sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
			f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
			sum_page_array[batch_ofs] = sum_page;
			
			if (get_valid_blocks(sbi, segno, false) == 0)
				goto freed_early;
			if (gc_type == BG_GC && __is_large_section(sbi) &&
					migrated >= sbi->migration_granularity) {
				/* TODO: migrated should be considered. In this function, migrated is zero. */
				goto skip_early;
			}
			if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
				goto skip_early;
	
			sum = page_address(sum_page);
			if (type != GET_SUM_TYPE((&sum->footer))) {
				f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
					 segno, type, GET_SUM_TYPE((&sum->footer)));
				set_sbi_flag(sbi, SBI_NEED_FSCK);
				f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_CORRUPTED_SUMMARY);
				goto skip_early;
			}
			
			/* Preread valid filesystem blocks in the segment */
			is_terminated_seg[batch_ofs] = false;
			/* BLOCK_GRANULARITY add */
			atomic_inc(&sbi->pipe_gc_i->checked_seg_seq);
			/* BLOCK_GRANULARITY add */
			smp_mb();
			/* BLOCK_GRANULARITY add */
			/* after terminated segment check, wake up writer to process seg. */
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */

#ifdef GC_PROFILE_TIMESERIAL	
			sbi->intra_zone_segidx_of_prereader = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif

			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
				start_t_ssa = OS_TimeGetNS_();
#endif
				//printk("%s: preread_node_segment start", __func__);
				/* BLOCK_GRANULARITY add */ 
				f2fs_bug_on(sbi, 1);
				//preread_node_segment_first_step(sbi, sum->entries, segno,
				//					gc_type, batch_ofs);
				//printk("%s: preread_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
#endif
			} else {
				//printk("%s: preread_data_segment start", __func__);
				/* BLOCK_GRANULARITY add */ 
				preread_data_segment_first_step(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate, batch_ofs); 
				//printk("%s: preread_data_segment end", __func__);
			}
			//sbi->pipe_gc_i->preread_seg_seq ++;
			/* BLOCK_GRANULARITY commented */
			//atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
		
			/* BLOCK_GRANULARITY commented */
		//	/* TODO: wake up writer thread */
		//	sbi->pipelined_gc_write_wake = true;
		//	smp_mb();
		//	//printk("%s: wake up. preread_seg_seq: %d ", 
		//	//		__func__, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
		//	//		);
		//	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			continue;

freed_early:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
	
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip_early:
			f2fs_put_page(sum_page, 0);
			is_terminated_seg[batch_ofs] = true;
			/* BLOCK_GRANULARITY commented */
			//atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			sbi->pipelined_gc_write_wake = true;
			/* BLOCK_GRANULARITY added */
			atomic_inc(&sbi->pipe_gc_i->checked_seg_seq);
			/* FIX (skipped segment not accounted in preread watermark):
			 * preread_data_segment_first_step never runs for a freed/
			 * skipped segment, so preread_blk_seq stays short of this
			 * segment's range and the second-step reader's per-block wait
			 * can strand on it (delete-heavy loads skip most segments).
			 * Advance the watermark over the whole segment and wake the
			 * reader. Single publisher (this thread), in-order walk, so
			 * the check-then-set stays monotonic within the batch. */
			if (atomic_read(&sbi->pipe_gc_i->preread_blk_seq) <
					(batch_ofs + 1) * BLKS_PER_SEG(sbi))
				atomic_set(&sbi->pipe_gc_i->preread_blk_seq,
					(batch_ofs + 1) * BLKS_PER_SEG(sbi));
			sbi->pipelined_gc_read_second_step_wake = true;
			/* BLOCK_GRANULARITY added */
			smp_mb();
			/* after terminated segment check, wake up writer to skip seg. */
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
			wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);

		}

#ifdef GC_PROFILE_TIMESERIAL
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->preread_phase_lat += end_phase_t - start_phase_t;
		start_phase_t = end_phase_t;
#endif
		//printk(KERN_EMERG "%s: batch %d end. \n", __func__, batch_no);
		
		/* Let second-step reader know that batch is finished
			so do not stuck in read pass buffer. */
		atomic_inc(&sbi->pipe_gc_i->preread_batch_seq);
		/* FIX (lost wakeup for the batch-done escape): the reader's
		 * per-block wait now also exits on preread_batch_seq advancing,
		 * but nothing woke its waitqueue on that transition — only the
		 * per-block publications did, and those stop once this batch's
		 * preread loop is over. Wake it explicitly so a reader parked on
		 * a stale per-batch target re-evaluates and escapes. */
		smp_mb();
		wake_up(&sbi->pipelined_gc_read_second_step_thread->gc_wait_queue_head);
		/* Let the gc write thread to perform move GCed pages. */

		/* Wait for gc write thread to finish.
		 *
		 * FIX (signal-pending busy spin): this driver runs in the
		 * calling task's context (FG GC via f2fs_balance_fs), so with
		 * a pending signal (Ctrl-C on the benchmark, JVM/MySQL signal
		 * traffic) wait_event_interruptible returns immediately without
		 * waiting and nobody checks the return value. The driver then
		 * blasts through every handshake, corrupts the batch ordering
		 * (mass is_alive failures, unreclaimed segments) and finally
		 * spins forever on a condition its own racing made unreachable,
		 * wedging the FS while holding gc_lock. Driver-side waits must
		 * be uninterruptible, like mainline FG GC; the kthread-side
		 * waits keep interruptible + kthread_should_stop. */
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
		wait_event(sbi->pipelined_gc_write_second_step_thread->prereader_wq,
				sbi->pipelined_gc_write_batch_done);
#else
		wait_event(sbi->pipelined_gc_write_thread->prereader_wq,
				sbi->pipelined_gc_write_batch_done);
#endif
		sbi->pipelined_gc_write_batch_done = 0;
		//printk(KERN_EMERG "%s: batch %d end. woke up.\n", __func__, batch_no);
	}

	
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	//blk_finish_plug(&plug);


	/* driver context: must not be interruptible (see batch wait above) */
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
	wait_event(sbi->pipelined_gc_write_second_step_thread->prereader_wq,
									sbi->pipelined_gc_write_done);
#else
	wait_event(sbi->pipelined_gc_write_thread->prereader_wq,
									sbi->pipelined_gc_write_done);
#endif
	/* FIX (reader stranded across rounds): also wait for the
	 * reading_second_step thread to finish THIS round. The write-side
	 * completion above does not imply the reader is done: under delete-heavy
	 * load (rm -rf) every victim block is dead, the writer sails past on
	 * done_blk_seq watermarks the lagging reader published earlier, and the
	 * round "completes" with the reader still batches behind. Returning here
	 * lets the next round's init reset read_step2_credit_seq/done_blk_seq
	 * under the still-running reader, stranding it mid-round and deadlocking
	 * the next round (ckpt/rm D-state). The reader always terminates once
	 * write2 has finished: its remaining credit waits pass (credit ==
	 * n_seg_batch, monotonic within the round) and its ring waits escape via
	 * preread_batch_seq. */
	/* driver context: must not be interruptible (see batch wait above) */
#ifdef GC_DEEP_PIPELINE_WRITE_BATCHED
	wait_event(sbi->pipelined_gc_write_second_step_thread->prereader_wq,
									sbi->pipelined_gc_read_second_step_done);
#else
	wait_event(sbi->pipelined_gc_write_thread->prereader_wq,
									sbi->pipelined_gc_read_second_step_done);
#endif
	/* pipelined GC post processing */
	/*
	 * Authoritative freed-segment count. By this point every pipeline
	 * stage has drained (we are past the pipelined_gc_write_done wait
	 * above), so a segment is freed iff it now holds no valid blocks.
	 * Recomputing from live section state avoids a double-count race
	 * between the reader thread's freed_early tally (seg_freed++ earlier)
	 * and the writer threads' tally (pgci->seg_freed_by_writer): under
	 * delete-heavy workloads (e.g. fileserver) the same segment could be
	 * counted on both sides, pushing seg_freed past the segs-per-section.
	 */
	seg_freed = 0;
	for (segno = start_segno; segno < end_segno; segno++)
		if (get_valid_blocks(sbi, segno, false) == 0)
			seg_freed++;
	//sbi->pipe_gc_i->preread_seg_seq = 0;
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);
	atomic_set(&sbi->pipe_gc_i->checked_seg_seq, 0);

//#ifdef GC_PROFILE_TIMESERIAL	
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
//#endif
#endif
	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif
	return seg_freed;
}
#endif
#if defined(GC_DEEP_PIPELINE_BATCHED) || defined (GC_BLOCK_GRANULARITY_PIPELINE_BATCHED)
static int do_garbage_collect_batched_block_granularity_pipelined(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
#ifdef CHECK_GC_RACE
	printk("%s start: %s %u", __func__, current->comm, current->pid);
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long start_t_ssa, end_t_ssa, start_phase_t, end_phase_t;
#endif

#ifdef GC_PAGE_POOL
	f2fs_gc_pool_set_victim(sbi, START_BLOCK(sbi, start_segno));
#endif

	//printk("%s: cpuid: %d", __func__, smp_processor_id());

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));
	
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);
	atomic_set(&sbi->pipe_gc_i->checked_seg_seq, 0);
	atomic_set(&sbi->pipe_gc_i->preread_blk_seq, 0); 	/* BLOCK_GRANULARITY add */

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
#endif


	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error!!", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
#endif

	blk_start_plug(&plug);

	/* TODO: prepare gc_pipeline info for writer thread */
	pgci->plug = &plug;
	pgci->start_segno = start_segno;
	pgci->end_segno = end_segno;
	pgci->gc_list = gc_list;
	pgci->gc_type = gc_type;
	pgci->force_migrate = force_migrate;

	/* TODO: wake up writer thread */
	sbi->pipelined_gc_write_done = 0;
	sbi->pipelined_gc_write_batch_done = 0;
	//sbi->pipelined_gc_write_wake = true;
	sbi->pipelined_gc_write_thread_wake = true;
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		atomic_set(&sbi->pipe_gc_i->preread_blk_seq, 0); 	/* BLOCK_GRANULARITY add */
		/* Preread valid filesystem blocks within segemnt batch */
#ifdef GC_PROFILE_TIMESERIAL
		start_phase_t = OS_TimeGetNS_();
#endif
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("%s: batch_no: %d batch_ofs: %d", __func__, batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;

#ifdef GC_PROFILE_TIMESERIAL	
			start_t_ssa = OS_TimeGetNS_();
#endif
			/* find segment summary of victim */
			sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
			f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
			sum_page_array[batch_ofs] = sum_page;
			
			if (get_valid_blocks(sbi, segno, false) == 0)
				goto freed_early;
			if (gc_type == BG_GC && __is_large_section(sbi) &&
					migrated >= sbi->migration_granularity) {
				/* TODO: migrated should be considered. In this function, migrated is zero. */
				goto skip_early;
			}
			if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
				goto skip_early;
	
			sum = page_address(sum_page);
			if (type != GET_SUM_TYPE((&sum->footer))) {
				f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
					 segno, type, GET_SUM_TYPE((&sum->footer)));
				set_sbi_flag(sbi, SBI_NEED_FSCK);
				f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_CORRUPTED_SUMMARY);
				goto skip_early;
			}
			
			/* Preread valid filesystem blocks in the segment */
			is_terminated_seg[batch_ofs] = false;
			/* BLOCK_GRANULARITY add */
			atomic_inc(&sbi->pipe_gc_i->checked_seg_seq);
			/* BLOCK_GRANULARITY add */
			smp_mb();
			/* BLOCK_GRANULARITY add */
			/* after terminated segmetn check, wake up writer to process seg. */
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */

#ifdef GC_PROFILE_TIMESERIAL	
			sbi->intra_zone_segidx_of_prereader = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif

			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
				start_t_ssa = OS_TimeGetNS_();
#endif
				//printk("%s: preread_node_segment start", __func__);
				/* BLOCK_GRANULARITY add */ 
				preread_node_segment_blk_gra(sbi, sum->entries, segno,
									gc_type, batch_ofs);
				//printk("%s: preread_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
#endif
			} else {
				//printk("%s: preread_data_segment start", __func__);
				/* BLOCK_GRANULARITY add */ 
				preread_data_segment_blk_gra(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate, batch_ofs); 
				//printk("%s: preread_data_segment end", __func__);
			}
			//sbi->pipe_gc_i->preread_seg_seq ++;
			/* BLOCK_GRANULARITY commented */
			//atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
		
			/* BLOCK_GRANULARITY commented */
		//	/* TODO: wake up writer thread */
		//	sbi->pipelined_gc_write_wake = true;
		//	smp_mb();
		//	//printk("%s: wake up. preread_seg_seq: %d ", 
		//	//		__func__, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
		//	//		);
		//	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			continue;

freed_early:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
	
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip_early:
			f2fs_put_page(sum_page, 0);
			is_terminated_seg[batch_ofs] = true;
			/* BLOCK_GRANULARITY commented */
			//atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			sbi->pipelined_gc_write_wake = true;
			/* BLOCK_GRANULARITY added */
			atomic_inc(&sbi->pipe_gc_i->checked_seg_seq);
			/* BLOCK_GRANULARITY added */
			smp_mb();
			/* after terminated segmetn check, wake up writer to skip seg. */
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

		}

#ifdef GC_PROFILE_TIMESERIAL
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->preread_phase_lat += end_phase_t - start_phase_t;
		start_phase_t = end_phase_t;
#endif
		/* Let the gc write thread to perform move GCed pages. */

		/* Wait for gc write thread to finish */
		/* TODO. */
		/* TODO. Need to recheck */
		//DEFINE_WAIT(wait);
		//prepare_to_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait,
		//			TASK_UNINTERRUPTIBLE);
		//io_schedule();
		//finish_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait);
		wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_wq, 
							sbi->pipelined_gc_write_batch_done);
		sbi->pipelined_gc_write_batch_done = 0;
	}

	
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	blk_finish_plug(&plug);


	wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_wq, sbi->pipelined_gc_write_done);
	/* pipelined GC post processing */
	seg_freed += pgci->seg_freed_by_writer;
	//sbi->pipe_gc_i->preread_seg_seq = 0;
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);
	atomic_set(&sbi->pipe_gc_i->checked_seg_seq, 0);

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif
	return seg_freed;
}
#else	/* else of GC_BLOCK_GRANULARITY_PIPELINE_BATCHED*/
static int do_garbage_collect_batched_pipelined(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct pipelined_gc_info *pgci = sbi->pipe_gc_i;
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
#ifdef CHECK_GC_RACE
	printk("%s start: %s %u", __func__, current->comm, current->pid);
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long start_t_ssa, end_t_ssa, start_phase_t, end_phase_t;
#endif

#ifdef GC_PAGE_POOL
	f2fs_gc_pool_set_victim(sbi, START_BLOCK(sbi, start_segno));
#endif

	//printk("%s: cpuid: %d", __func__, smp_processor_id());

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));
	
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
#endif


	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error!!", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
#endif

	blk_start_plug(&plug);

	/* TODO: prepare gc_pipeline info for writer thread */
	pgci->plug = &plug;
	pgci->start_segno = start_segno;
	pgci->end_segno = end_segno;
	pgci->gc_list = gc_list;
	pgci->gc_type = gc_type;
	pgci->force_migrate = force_migrate;

	/* TODO: wake up writer thread */
	sbi->pipelined_gc_write_done = 0;
	sbi->pipelined_gc_write_batch_done = 0;
	sbi->pipelined_gc_write_thread_wake = true;
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

	int batch_no, batch_ofs, n_batched_segs, n_seg_batch, last_n_batched_segs;

	if ((end_segno - start_segno) % N_BATCHED_SEGS) {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS + 1;
		last_n_batched_segs = (end_segno - start_segno) % N_BATCHED_SEGS;
	} else {
		n_seg_batch = (end_segno - start_segno) / N_BATCHED_SEGS;
		last_n_batched_segs = N_BATCHED_SEGS;
	}
	//if ((end_segno - start_segno) % N_BATCHED_SEGS) {
	//	printk("%s: end_segno: %llu start_segno: %llu N_BATCHED_SEGS: %llu", 
	//			__func__, end_segno, start_segno, N_BATCHED_SEGS);
	//}
	//f2fs_bug_on(sbi, (end_segno - start_segno) % N_BATCHED_SEGS);
	struct page **sum_page_array = pgci->sum_page_array;
	bool *is_terminated_seg = pgci->is_terminated_seg;
	//printk("%s: n_seg_batch %d end_segno: %u start_segno: %u", __func__, n_seg_batch);
	for (batch_no = 0; batch_no < n_seg_batch; batch_no ++) {
		//printk("batch_no: %d", batch_no);
		/* Preread valid filesystem blocks within segemnt batch */
#ifdef GC_PROFILE_TIMESERIAL
		start_phase_t = OS_TimeGetNS_();
#endif
		n_batched_segs = (batch_no == n_seg_batch - 1)?
			last_n_batched_segs: N_BATCHED_SEGS;

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		for (batch_ofs = 0; batch_ofs < n_batched_segs; batch_ofs ++) {
			//printk("%s: batch_no: %d batch_ofs: %d", __func__, batch_no, batch_ofs);
			segno = start_segno + batch_ofs + batch_no * N_BATCHED_SEGS;

#ifdef GC_PROFILE_TIMESERIAL	
			start_t_ssa = OS_TimeGetNS_();
#endif
			/* find segment summary of victim */
			sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
			f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
			sum_page_array[batch_ofs] = sum_page;
			
			if (get_valid_blocks(sbi, segno, false) == 0)
				goto freed_early;
			if (gc_type == BG_GC && __is_large_section(sbi) &&
					migrated >= sbi->migration_granularity) {
				/* TODO: migrated should be considered. In this function, migrated is zero. */
				goto skip_early;
			}
			if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
				goto skip_early;
	
			sum = page_address(sum_page);
			if (type != GET_SUM_TYPE((&sum->footer))) {
				f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
					 segno, type, GET_SUM_TYPE((&sum->footer)));
				set_sbi_flag(sbi, SBI_NEED_FSCK);
				f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_CORRUPTED_SUMMARY);
				goto skip_early;
			}
			/* Preread valid filesystem blocks in the segment */
			is_terminated_seg[batch_ofs] = false;

			/*
			 * this is to avoid deadlock:
			 * - lock_page(sum_page)         - f2fs_replace_block
			 *  - check_valid_map()            - down_write(sentry_lock)
			 *   - down_read(sentry_lock)     - change_curseg()
			 *                                  - lock_page(sum_page)
			 */

#ifdef GC_PROFILE_TIMESERIAL	
			sbi->intra_zone_segidx_of_prereader = batch_ofs + batch_no * N_BATCHED_SEGS;
#endif

			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
				start_t_ssa = OS_TimeGetNS_();
#endif
				//printk("%s: preread_node_segment start", __func__);
				preread_node_segment(sbi, sum->entries, segno,
									gc_type);
				//printk("%s: preread_node_segment end", __func__);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
#endif
			} else {
				//printk("%s: preread_data_segment start", __func__);
				preread_data_segment(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate);
				//printk("%s: preread_data_segment end", __func__);
			}
			//sbi->pipe_gc_i->preread_seg_seq ++;
			atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			/* TODO: wake up writer thread */
			sbi->pipelined_gc_write_wake = true;
			smp_mb();
			//printk("%s: wake up. preread_seg_seq: %d ", 
			//		__func__, atomic_read(&sbi->pipe_gc_i->preread_seg_seq)
			//		);
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

			continue;

freed_early:
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
	
			if (__is_large_section(sbi))
				sbi->next_victim_seg[gc_type] =
					(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip_early:
			f2fs_put_page(sum_page, 0);
			is_terminated_seg[batch_ofs] = true;
			atomic_inc(&sbi->pipe_gc_i->preread_seg_seq);
			sbi->pipelined_gc_write_wake = true;
			smp_mb();
			wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);

		}

#ifdef GC_PROFILE_TIMESERIAL
		end_phase_t = OS_TimeGetNS_();
		cur_gcp->preread_phase_lat += end_phase_t - start_phase_t;
		start_phase_t = end_phase_t;
#endif
		/* Let the gc write thread to perform move GCed pages. */

		/* Wait for gc write thread to finish */
		/* TODO. */
		/* TODO. Need to recheck */
		//DEFINE_WAIT(wait);
		//prepare_to_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait,
		//			TASK_UNINTERRUPTIBLE);
		//io_schedule();
		//finish_wait(&sbi->pipelined_gc_write_thread->prereader_wq, &wait);
		wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_wq, 
							sbi->pipelined_gc_write_batch_done);
		sbi->pipelined_gc_write_batch_done = 0;
	}

	
	wake_up(&sbi->pipelined_gc_write_thread->gc_wait_queue_head);
	blk_finish_plug(&plug);


	wait_event_interruptible(sbi->pipelined_gc_write_thread->prereader_wq, sbi->pipelined_gc_write_done);
	/* pipelined GC post processing */
	seg_freed += pgci->seg_freed_by_writer;
	//sbi->pipe_gc_i->preread_seg_seq = 0;
	atomic_set(&sbi->pipe_gc_i->preread_seg_seq, 0);


#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s: end", __func__);
#ifdef CHECK_GC_RACE
	printk("%s end: %s %u", __func__, current->comm, current->pid);
#endif
	return seg_freed;
}
#endif	/* End of GC_BLOCK_GRANULARITY_PIPELINE_BATCHED*/
#endif /* End of GC_PIPELINE_BATCHED */

#endif /* End of GC_PIPELINE */
#endif /* End of GC_PREREAD */


/*
 * This function tries to get parent node of victim data block, and identifies
 * data block validity. If the block is valid, copy that with cold status and
 * modify parent node.
 * If the parent node is not valid or the data block address is different,
 * the victim data block is ignored.
 */
static int gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	blk_opf_t op_flags = REQ_RAHEAD;

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req){
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else 
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif


#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0,  
						p3_is_alive_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p4_move_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif

	start_addr = START_BLOCK(sbi, segno);

next_step:
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_prereader = -1;
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_start_t = OS_TimeGetNS_();
			//phase3_start_t = OS_TimeGetNS_();
			//tmp_start_t = phase3_start_t; 
		}
#endif

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

		if (check_valid_map(sbi, segno, off) == 0){
#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_prereader ++;
		sbi->intra_seg_blkseq_of_writer ++;
#endif

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_(); 
			p3_is_alive_lat += tmp_end_t - tmp_start_t;
		}
#endif

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			int err;

#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				//printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			if (!f2fs_down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				iput(inode);
				sbi->skipped_gc_rwsem++;
				//printk("%s: SKIP 4 !!", __func__);
				continue;
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					//printk("%s: SKIP 5 !!", __func__);
					continue;
				}
				add_gc_inode(gc_list, inode);
				//printk("%s: SKIP 6 !!", __func__);
				continue;
			}
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif

			data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
							op_flags, true, NULL);
							//REQ_RAHEAD, true, NULL);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				continue;
			}

			f2fs_put_page(data_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_put_page_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			add_gc_inode(gc_list, inode);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;

			if (S_ISREG(inode->i_mode)) {
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
					sbi->skipped_gc_rwsem++;
					//printk("%s: SKIP _2 !!", __func__);
					continue;
				}
				if (!f2fs_down_write_trylock(
						&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP _3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page_vanilla(inode, start_bidx, gc_type,
								segno, off);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			//if (err)
				//printk("%s: SKIP _5 !!", __func__);

			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#ifdef GC_PROFILE_TIMESERIAL
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;
#ifdef GC_PROFILE_TIMESERIAL
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
	cur_gcp->gc_p4_lat += p_lat_t[4];

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}

#ifdef PGC
static int gc_data_segment_pgc(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, int mode)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	blk_opf_t op_flags = REQ_RAHEAD;

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req){
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else 
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif


#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0,  
						p3_is_alive_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p4_move_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif
	int start, end;
	start_addr = START_BLOCK(sbi, segno);
	/* PGC added */
	if (mode == NORMAL_GC) {
		start = 0;
		end = usable_blks_in_seg;
	} else {
		start = mode * usable_blks_in_seg / NUM_PGC_THREADS;
		end = (mode + 1) * usable_blks_in_seg / NUM_PGC_THREADS;
		if (mode == NUM_PGC_THREADS - 1) {
			end = usable_blks_in_seg;
		}
	}

next_step:
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_prereader = 0;
	sbi->intra_seg_blkseq_of_writer = 0;
	//sbi->intra_seg_blkseq_of_prereader = -1;
	//sbi->intra_seg_blkseq_of_writer = -1;
#endif

	//entry = sum;
	/* PGC added */
	entry = sum + start;
#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = start; off < end; off++, entry++) { /* PGC added */
	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_start_t = OS_TimeGetNS_();
			//phase3_start_t = OS_TimeGetNS_();
			//tmp_start_t = phase3_start_t; 
		}
#endif

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

		if (check_valid_map(sbi, segno, off) == 0){
#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		//sbi->intra_seg_blkseq_of_prereader ++;
		//sbi->intra_seg_blkseq_of_writer ++;
#endif

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_(); 
			p3_is_alive_lat += tmp_end_t - tmp_start_t;
		}
#endif

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			int err;

retry_gc_write_lock_p3:
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			//if (!f2fs_down_write_trylock(
			//	&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
			//	iput(inode);
			//	sbi->skipped_gc_rwsem++;
			//	printk("%s: SKIP 4 !!", __func__);
			//	continue;
			//}
			f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			//if (!f2fs_down_write_trylock(
			//	&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
			//	iput(inode);
			//	goto retry_gc_write_lock_p3;
			//}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					printk("%s: SKIP 5 !!", __func__);
					continue;
				}
				add_gc_inode(gc_list, inode);
				printk("%s: SKIP 6 !!", __func__);
				continue;
			}
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif

			data_page = f2fs_get_read_pgc_data_page(inode, start_bidx,
							op_flags, true, NULL, mode);
							//REQ_RAHEAD, true, NULL);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				printk("%s: SKIP 7 !!", __func__);
				continue;
			}

			f2fs_put_page(data_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_put_page_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			add_gc_inode(gc_list, inode);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;

			if (S_ISREG(inode->i_mode)) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
				//	sbi->skipped_gc_rwsem++;
				//	printk("%s: SKIP _2 !!", __func__);
				//	continue;
				//}
retry_gc_write_lock_p4:
				f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
				//	//sbi->skipped_gc_rwsem++;
				//	//printk("%s: SKIP _2 !!", __func__);
				//	//continue;
				//	goto retry_gc_write_lock_p4;
				//}
				if (!f2fs_down_write_trylock(
						&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					printk("%s: SKIP _3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page_vanilla_pgc(inode, start_bidx, gc_type,
								segno, off, mode);
				//err = move_data_page_vanilla(inode, start_bidx, gc_type,
				//				segno, off);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			//if (err)
				//printk("%s: SKIP _5 !!", __func__);

			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#ifdef GC_PROFILE_TIMESERIAL
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;
#ifdef GC_PROFILE_TIMESERIAL
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_pgc_profiler[mode];
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
	cur_gcp->gc_p4_lat += p_lat_t[4];
	
	//printk("%s: pgc_idx: %d cur_gcp: %p p0: %llu (%llu) p1: %llu (%llu) p2: %llu (%llu) p3: %llu (%llu) p4: %llu (%llu) ", __func__, mode,  
	//	cur_gcp, 
	//	cur_gcp->gc_p0_lat, 
	//	p_lat_t[0], 
	//	cur_gcp->gc_p1_lat, 
	//	p_lat_t[1], 
	//	cur_gcp->gc_p2_lat, 
	//	p_lat_t[2], 
	//	cur_gcp->gc_p3_lat, 
	//	p_lat_t[3], 
	//	cur_gcp->gc_p4_lat, 
	//	p_lat_t[4] 
	//	);

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}

static int gc_data_segment_pgc_second_thread(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate, int mode)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	blk_opf_t op_flags = REQ_RAHEAD;

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req){
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else 
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif


#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_check1_lat = 0, p3_check2_lat = 0,  
						p3_is_alive_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p3_add_gc_inode_lat = 0, 
						p3_put_page_lat = 0, 
						p4_move_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif
	int start, end;
	start_addr = START_BLOCK(sbi, segno);
	/* PGC added */
	if (mode == NORMAL_GC) {
		start = 0;
		end = usable_blks_in_seg;
	} else {
		start = mode * usable_blks_in_seg / NUM_PGC_THREADS;
		end = (mode + 1) * usable_blks_in_seg / NUM_PGC_THREADS;
		if (mode == NUM_PGC_THREADS - 1) {
			end = usable_blks_in_seg;
		}
	}

next_step:
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_prereader = 0;
	sbi->intra_seg_blkseq_of_writer = 0;
	//sbi->intra_seg_blkseq_of_prereader = -1;
	//sbi->intra_seg_blkseq_of_writer = -1;
#endif

	//entry = sum;
	/* PGC added */
	entry = sum + start;
#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = start; off < end; off++, entry++) { /* PGC added */
	//for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);
#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_start_t = OS_TimeGetNS_();
			//phase3_start_t = OS_TimeGetNS_();
			//tmp_start_t = phase3_start_t; 
		}
#endif

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check1_lat += tmp_end_t - tmp_start_t; 
			tmp_start_t = tmp_end_t;
		}
#endif

		if (check_valid_map(sbi, segno, off) == 0){
#ifdef GC_PROFILE_TIMESERIAL
			if (phase == 3){
				tmp_end_t = OS_TimeGetNS_();
				p3_check2_lat += tmp_end_t - tmp_start_t; 
			}
#endif
			continue;
		} 

#ifdef GC_PROFILE_TIMESERIAL
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_();
			p3_check2_lat += tmp_end_t - tmp_start_t; 
		}
#endif

#ifdef GC_PROFILE_TIMESERIAL
		//sbi->intra_seg_blkseq_of_prereader ++;
		//sbi->intra_seg_blkseq_of_writer ++;
#endif

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

#ifdef GC_PROFILE_TIMESERIAL	
		tmp_start_t = OS_TimeGetNS_(); 
#endif

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;
#ifdef GC_PROFILE_TIMESERIAL	
		if (phase == 3){
			tmp_end_t = OS_TimeGetNS_(); 
			p3_is_alive_lat += tmp_end_t - tmp_start_t;
		}
#endif

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			int err;

retry_gc_write_lock_p3:
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			//if (!f2fs_down_write_trylock(
			//	&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
			//	iput(inode);
			//	sbi->skipped_gc_rwsem++;
			//	printk("%s: SKIP 4 !!", __func__);
			//	continue;
			//}
			f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem_gc_write[WRITE]);
			//if (!f2fs_down_write_trylock(
			//	&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
			//	iput(inode);
			//	goto retry_gc_write_lock_p3;
			//}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem_gc_write[WRITE]);
				if (err) {
					iput(inode);
					printk("%s: SKIP 5 !!", __func__);
					continue;
				}
				add_gc_inode(gc_list, inode);
				printk("%s: SKIP 6 !!", __func__);
				continue;
			}
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif

			data_page = f2fs_get_read_pgc_data_page(inode, start_bidx,
							op_flags, true, NULL, mode);
							//REQ_RAHEAD, true, NULL);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem_gc_write[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				printk("%s: SKIP 7 !!", __func__);
				continue;
			}

			f2fs_put_page(data_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_put_page_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			add_gc_inode(gc_list, inode);
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_end_t = OS_TimeGetNS_(); 
			p3_add_gc_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;

			if (S_ISREG(inode->i_mode)) {
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
				//	sbi->skipped_gc_rwsem++;
				//	printk("%s: SKIP _2 !!", __func__);
				//	continue;
				//}
retry_gc_write_lock_p4:
				f2fs_down_write(&fi->i_gc_rwsem_gc_write[WRITE]);
				//if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
				//	//sbi->skipped_gc_rwsem++;
				//	//printk("%s: SKIP _2 !!", __func__);
				//	//continue;
				//	goto retry_gc_write_lock_p4;
				//}
				if (!f2fs_down_write_trylock(
						&fi->i_gc_rwsem_gc_write[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
					printk("%s: SKIP _3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page_vanilla_pgc(inode, start_bidx, gc_type,
								segno, off, mode);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			//if (err)
				//printk("%s: SKIP _5 !!", __func__);

			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[READ]);
				f2fs_up_write(&fi->i_gc_rwsem_gc_write[WRITE]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#ifdef GC_PROFILE_TIMESERIAL
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;
#ifdef GC_PROFILE_TIMESERIAL
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_pgc_profiler[mode];
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
	cur_gcp->gc_p4_lat += p_lat_t[4];
	//printk("%s: pgc_idx: %d cur_gcp: %p p0: %llu (%llu) p1: %llu (%llu) p2: %llu (%llu) p3: %llu (%llu) p4: %llu (%llu) ", __func__, mode,  
	//	cur_gcp, 
	//	cur_gcp->gc_p0_lat, 
	//	p_lat_t[0], 
	//	cur_gcp->gc_p1_lat, 
	//	p_lat_t[1], 
	//	cur_gcp->gc_p2_lat, 
	//	p_lat_t[2], 
	//	cur_gcp->gc_p3_lat, 
	//	p_lat_t[3], 
	//	cur_gcp->gc_p4_lat, 
	//	p_lat_t[4] 
	//	);

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_check1_lat += p3_check1_lat;
	cur_gcp->p3_check2_lat += p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p3_put_page_lat = p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p3_add_gc_inode_lat;
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}

static int p_gc_func(void *data) {
	struct f2fs_p_gc_kthread* p_gc = data;
	struct f2fs_sb_info *sbi = p_gc->sbi;
	do {
		wait_event_interruptible_timeout(p_gc->gc_wait_queue_head,
			kthread_should_stop() || p_gc->done == false,
			msecs_to_jiffies(300));
		if (p_gc->done == false) {
			if (p_gc->type == SUM_TYPE_NODE) {
				//printk("%s: START pgc %dth thread gc_node_segment", 
				//			__func__, p_gc->p_gc_idx);
				p_gc->submitted = gc_node_segment_pgc(sbi, p_gc->sum->entries, p_gc->segno,
									FG_GC, p_gc->p_gc_idx);
				//printk("%s: END pgc %dth thread gc_node_segment", 
				//			__func__, p_gc->p_gc_idx);

			} else {
				//printk("%s: START pgc %dth thread gc_data_segment", 
				//			__func__, p_gc->p_gc_idx);
				if (p_gc->p_gc_idx % 2 == 0) {
					p_gc->submitted = gc_data_segment_pgc(sbi, p_gc->sum->entries, 
								p_gc->gc_list,
								p_gc->segno, FG_GC,
								p_gc->should_migrate_blocks, 
								p_gc->p_gc_idx);
				} else {
					p_gc->submitted = gc_data_segment_pgc_second_thread(sbi, p_gc->sum->entries, 
								p_gc->gc_list,
								p_gc->segno, FG_GC,
								p_gc->should_migrate_blocks, 
								p_gc->p_gc_idx);
				}
				//printk("%s: END pgc %dth thread gc_data_segment", 
				//			__func__, p_gc->p_gc_idx);
			}
    			//do_garbage_collect_pgc(sbi, p_gc->segno, p_gc->gc_list, FG_GC,
    			//	p_gc->should_migrate_blocks, p_gc->p_gc_idx);
			p_gc->done = true;
			wake_up(&p_gc->caller_wq);
		}
	} while (!kthread_should_stop());

  return 0;
}

#endif /* end of PGC */


#ifdef GC_PAGE_POOL
static int gc_data_segment_page_pool(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type,
		bool force_migrate)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	int submitted = 0;
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	blk_opf_t op_flags = REQ_RAHEAD;

#ifdef REQ_GC_FLAG
#ifdef FILTER_GC_REQ
	if (sbi->set_gc_req){
		if (sbi->gc_start_) {
			op_flags |= REQ_GC_START;
			sbi->gc_start_ = false;
			printk("%s: REQ_GC_START!!!", __func__);
		} else 
			op_flags |= REQ_GC;
	}
#else
	op_flags |= REQ_GC;
#endif
#endif

#ifdef GC_PAGE_POOL
	bool page_pool_used;
#endif

#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long read_start_t, read_end_t, tmp_write_time_sum = 0;
	unsigned long long p_start_t[5], p_end_t[5], p_lat_t[5], 
				  tmp_start_t, tmp_end_t; 
	unsigned long long p1_ra_node_lat = 0, 
						p2_check_inode_lat = 0, p2_ra_inode_lat = 0, 
						p3_iget_lat = 0, p3_bef_read_lat = 0, 
						p3_read_lat = 0, 
						p4_move_lat = 0; 
	read_start_t = OS_TimeGetNS_();
#endif

	start_addr = START_BLOCK(sbi, segno);

next_step:
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_seg_blkseq_of_prereader = -1;
	sbi->intra_seg_blkseq_of_writer = -1;
#endif

	entry = sum;
#ifdef GC_PROFILE_TIMESERIAL	
	p_start_t[phase] = OS_TimeGetNS_();
#endif

	for (off = 0; off < usable_blks_in_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;
		nid_t nid = le32_to_cpu(entry->nid);

		/*
		 * stop BG_GC if there is not enough free sections.
		 * Or, stop GC if the segment becomes fully valid caused by
		 * race condition along with SSR block allocation.
		 */
		if ((gc_type == BG_GC && has_not_enough_free_secs(sbi, 0, 0)) ||
			(!force_migrate && get_valid_blocks(sbi, segno, true) ==
							CAP_BLKS_PER_SEC(sbi))) {
			//printk("%s: SKIP 1 !!", __func__);
			return submitted;
		}

		if (check_valid_map(sbi, segno, off) == 0){
			continue;
		} 
#ifdef GC_PROFILE_TIMESERIAL
		sbi->intra_seg_blkseq_of_prereader ++;
		sbi->intra_seg_blkseq_of_writer ++;
#endif

		if (phase == 0) {
			f2fs_ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), 1,
							META_NAT, true);
			continue;
		}

		if (phase == 1) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			f2fs_ra_node_page(sbi, nid);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p1_ra_node_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		if (phase == 2) {
#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			if (!is_alive_jw_gc_isalive(sbi, entry, &dni, start_addr + off, &nofs))
				continue;
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_check_inode_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = tmp_end_t; 
#endif
			f2fs_ra_node_page(sbi, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p2_ra_inode_lat += tmp_end_t - tmp_start_t;
#endif
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs))
			continue;

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 3) {
			int err;

#ifdef GC_PROFILE_TIMESERIAL
			tmp_start_t = OS_TimeGetNS_(); 
#endif
			inode = f2fs_iget(sb, dni.ino);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_iget_lat += tmp_end_t - tmp_start_t;
#endif
			if (IS_ERR(inode) || is_bad_inode(inode) ||
					special_file(inode->i_mode)) {
				//printk("%s: SKIP 2 !!", __func__);
				continue;
			}

			err = f2fs_gc_pinned_control(inode, gc_type, segno);
			if (err == -EAGAIN) {
				iput(inode);
				//printk("%s: SKIP 3 !!", __func__);
				return submitted;
			}

			if (!f2fs_down_write_trylock(
				&F2FS_I(inode)->i_gc_rwsem[WRITE])) {
				iput(inode);
				sbi->skipped_gc_rwsem++;
				//printk("%s: SKIP 4 !!", __func__);
				continue;
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode) +
								ofs_in_node;

			if (f2fs_post_read_required(inode)) {
				int err = ra_data_block(inode, start_bidx);

				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				if (err) {
					iput(inode);
					//printk("%s: SKIP 5 !!", __func__);
					continue;
				}
				add_gc_inode(gc_list, inode);
				//printk("%s: SKIP 6 !!", __func__);
				continue;
			}
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_bef_read_lat += tmp_end_t - tmp_start_t;
			tmp_start_t = OS_TimeGetNS_(); 
#endif

			//data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
			//				op_flags, true, NULL);
			//				//REQ_RAHEAD, true, NULL);
#ifndef SKIP_FMAP_TRAVERSE
			data_page = f2fs_get_read_gc_data_page(inode, start_bidx,
							op_flags, true, NULL);
#else
#ifdef GC_PAGE_POOL
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off, &page_pool_used);
#else
			data_page = f2fs_get_read_gc_data_page_without_filemap(inode, start_bidx,
							op_flags, true, NULL, start_addr + off);
#endif
#endif

#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_(); 
			p3_read_lat += tmp_end_t - tmp_start_t;
#endif
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			if (IS_ERR(data_page)) {
				iput(inode);
				continue;
			}

			//f2fs_put_page(data_page, 0);
#ifdef GC_PAGE_POOL
			if (!page_pool_used) {
				f2fs_put_page(data_page, 0);
			} else {
				//printk("%s: bef put!!!!!!!!!!!!!!!!!!!!", __func__);
				//f2fs_put_page(data_page, 0);
				//printk("%s: aft put!!!!!!!!!!!!!!!!!!!!", __func__);
			}
#else
			f2fs_put_page(data_page, 0);
#endif
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 4 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			struct f2fs_inode_info *fi = F2FS_I(inode);
			bool locked = false;
			int err;

			if (S_ISREG(inode->i_mode)) {
				if (!f2fs_down_write_trylock(&fi->i_gc_rwsem[WRITE])) {
					sbi->skipped_gc_rwsem++;
					//printk("%s: SKIP _2 !!", __func__);
					continue;
				}
				if (!f2fs_down_write_trylock(
						&fi->i_gc_rwsem[READ])) {
					sbi->skipped_gc_rwsem++;
					f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
					//printk("%s: SKIP _3 !!", __func__);
					continue;
				}
				locked = true;

				/* wait for all inflight aio data */
				inode_dio_wait(inode);
			}

			start_bidx = f2fs_start_bidx_of_node(nofs, inode)
								+ ofs_in_node;
#ifdef GC_PROFILE_TIMESERIAL	
			tmp_start_t = OS_TimeGetNS_();
#endif
			if (f2fs_post_read_required(inode))
				err = move_data_block(inode, start_bidx,
							gc_type, segno, off);
			else
				err = move_data_page(inode, start_bidx, gc_type,
								segno, off);
#ifdef GC_PROFILE_TIMESERIAL
			tmp_end_t = OS_TimeGetNS_();
			p4_move_lat += tmp_end_t - tmp_start_t;
#endif

			if (!err && (gc_type == FG_GC ||
					f2fs_post_read_required(inode)))
				submitted++;
			
			//if (err)
				//printk("%s: SKIP _5 !!", __func__);

			if (locked) {
				f2fs_up_write(&fi->i_gc_rwsem[READ]);
				f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			}

			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}
#ifdef GC_PROFILE_TIMESERIAL
	p_end_t[phase] = OS_TimeGetNS_();
	p_lat_t[phase] = p_end_t[phase] - p_start_t[phase];
#endif

	if (++phase < 5)
		goto next_step;
#ifdef GC_PROFILE_TIMESERIAL
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->gc_p0_lat += p_lat_t[0];
	cur_gcp->gc_p1_lat += p_lat_t[1];
	cur_gcp->gc_p2_lat += p_lat_t[2];
	cur_gcp->gc_p3_lat += p_lat_t[3];
	cur_gcp->gc_p4_lat += p_lat_t[4];

	cur_gcp->p1_ra_node_lat += p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p2_check_inode_lat;
	cur_gcp->p3_iget_lat += p3_iget_lat;
	//cur_gcp->p3_bef_read_lat += p3_bef_read_lat;
	cur_gcp->p3_read_lat += p3_read_lat;
	cur_gcp->p4_move_lat += p4_move_lat;
#endif

	return submitted;
}
#endif

static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *victim,
			int gc_type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;

	down_write(&sit_i->sentry_lock);
	ret = f2fs_get_victim(sbi, victim, gc_type, NO_CHECK_TYPE, LFS, 0);
	up_write(&sit_i->sentry_lock);
	return ret;
}

#ifdef GC_PAGE_POOL
static int do_garbage_collect_gc_pool(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;

	//printk("%s start: %s %u", __func__, current->comm, current->pid);

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_t_ssa, end_t_ssa;
#endif

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL
	end_t_ssa = OS_TimeGetNS_();
#endif

	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
	sbi->intra_zone_segidx_of_writer = 0;
#endif

	blk_start_plug(&plug);

	for (segno = start_segno; segno < end_segno; segno++) {

#ifdef GC_PROFILE_TIMESERIAL	
		start_t_ssa = OS_TimeGetNS_();
#endif

		/* find segment summary of victim */
		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, segno));
		f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
		end_t_ssa = OS_TimeGetNS_();
		cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif

		if (get_valid_blocks(sbi, segno, false) == 0) {
			//printk("%s: SKIP 1", __func__);
			goto freed;
		}
		if (gc_type == BG_GC && __is_large_section(sbi) &&
				migrated >= sbi->migration_granularity)
			goto skip;
		if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
			goto skip;

		sum = page_address(sum_page);
		if (type != GET_SUM_TYPE((&sum->footer))) {
			f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
				 segno, type, GET_SUM_TYPE((&sum->footer)));
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			f2fs_stop_checkpoint(sbi, false,
				STOP_CP_REASON_CORRUPTED_SUMMARY);
			goto skip;
		}

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		/*
		 * this is to avoid deadlock:
		 * - lock_page(sum_page)         - f2fs_replace_block
		 *  - check_valid_map()            - down_write(sentry_lock)
		 *   - down_read(sentry_lock)     - change_curseg()
		 *                                  - lock_page(sum_page)
		 */
		if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
			start_t_ssa = OS_TimeGetNS_();
#endif
			submitted += gc_node_segment(sbi, sum->entries, segno,
								gc_type);
#ifdef GC_PROFILE_TIMESERIAL	
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
			cur_gcp->node_seg_cnt ++;
#endif
		}
		else {
			submitted += gc_data_segment_page_pool(sbi, sum->entries, gc_list,
							segno, gc_type,
							force_migrate);
		}
#ifdef GC_PROFILE_TIMESERIAL	
		sbi->intra_zone_segidx_of_prereader ++;
		sbi->intra_zone_segidx_of_writer ++;
#endif

		stat_inc_gc_seg_count(sbi, data_type, gc_type);
		sbi->gc_reclaimed_segs[sbi->gc_mode]++;
		migrated++;

freed:
		if (gc_type == FG_GC &&
				get_valid_blocks(sbi, segno, false) == 0)
			seg_freed++;

		if (__is_large_section(sbi))
			sbi->next_victim_seg[gc_type] =
				(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
		f2fs_put_page(sum_page, 0);
	}

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t_ssa - start_t_ssa;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s end: %s %u", __func__, current->comm, current->pid);
	return seg_freed;
}


#endif

#ifdef PGC
bool parallel_gc(struct f2fs_sb_info* sbi, int gc_type, unsigned int segno);

/* The three profiler helpers below touch cur_(p)gc_profiler /
 * struct gc_profiler_snapshot, which only exist when profiling is compiled
 * in -- guard them the same way as their only call site so the pgc mode
 * also builds with profiling disabled. */
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
static inline void reset_pgc_profiler(struct f2fs_sb_info *sbi)
{
  	struct gc_profiler_snapshot *cur_gcp;
	int i;
  	for (i = 0; i < NUM_PGC_THREADS; i++) {
  	  	cur_gcp = sbi->cur_pgc_profiler[i];
		cur_gcp->gc_p0_lat = 0;
		cur_gcp->gc_p1_lat = 0;
		cur_gcp->gc_p2_lat = 0;
		cur_gcp->gc_p3_lat = 0;
		cur_gcp->gc_p4_lat = 0;

		cur_gcp->p1_ra_node_lat = 0; 
		cur_gcp->p2_ra_inode_lat = 0;
		cur_gcp->p2_check_inode_lat = 0;
		cur_gcp->p3_check1_lat = 0;
		cur_gcp->p3_check2_lat = 0;
		cur_gcp->p3_is_alive_lat = 0;
		cur_gcp->p3_iget_lat = 0;
		cur_gcp->p3_bef_read_lat = 0;
		cur_gcp->p3_read_lat = 0;
		cur_gcp->p3_put_page_lat = 0;
		cur_gcp->p3_add_gc_inode_lat = 0;
		cur_gcp->p4_move_lat = 0;
						 
		cur_gcp->__pcache_alloc_filemap_get_entry_lat = 0;
		cur_gcp->__pcache_alloc___________rmqueue_pcplist_lat = 0;
		cur_gcp->__pcache_alloc________prep_new_page_lat = 0;
		cur_gcp->__pcache_alloc_add_folio_lat = 0;
		cur_gcp->pcache_alloc_lat = 0;
		cur_gcp->fmap_preread_lat = 0;
		cur_gcp->read_lat = 0;
		cur_gcp->pcache_grab_lat = 0;
		cur_gcp->__pcache_grab_filemap_get_entry_lat = 0;
		cur_gcp->__pcache_grab_lock_folio_lat = 0;
		cur_gcp->bef_write_lat = 0;
		cur_gcp->bef_write_1_lat = 0;
		cur_gcp->bef_write_5_lat = 0;
		cur_gcp->bef_write_6_lat = 0;
		cur_gcp->write_lat = 0;
		cur_gcp->fmap_write_lat = 0;
		cur_gcp->write_get_node_lat = 0;
		cur_gcp->set_pg_wb_lat = 0;
		cur_gcp->____allocate_data_block_lat = 0;
		cur_gcp->____invalidate_internal_cache_lat = 0;
		cur_gcp->____submit_page_write_lat = 0;
		cur_gcp->__update_data_blkaddr_lat = 0;
  	}

}


static inline unsigned int get_max_pgc_idx(struct f2fs_sb_info *sbi)
{
  	struct gc_profiler_snapshot *tmp_gcp;
	int max_gc_idx = 0, i;
	unsigned long long max_gc_time = 0, cur_gc_time;
  	for (i = 0 ; i < NUM_PGC_THREADS ; i++) {
  	  	tmp_gcp = sbi->cur_pgc_profiler[i];
		cur_gc_time = tmp_gcp->gc_p0_lat + 
				tmp_gcp->gc_p1_lat +
				tmp_gcp->gc_p2_lat +
				tmp_gcp->gc_p3_lat +
				tmp_gcp->gc_p4_lat;
		//printk("%s: pgc_idx: %d gcp: %p cur_gc_time_: %llu max_gc_time: %llu max_gc_idx: %d (p0: %llu p1: %llu p2: %llu p3: %llu p4: %llu)", __func__, i, 
		//	tmp_gcp, 
		//	cur_gc_time, max_gc_time, 
		//	max_gc_idx, 
		//	tmp_gcp->gc_p0_lat, tmp_gcp->gc_p1_lat, tmp_gcp->gc_p2_lat, tmp_gcp->gc_p3_lat, 
		//	tmp_gcp->gc_p4_lat
		//);
		if (cur_gc_time > max_gc_time) {
			max_gc_time = cur_gc_time;
			max_gc_idx = i;
		}
  	}
	return max_gc_idx;

}

static inline void reflect_slowest_gc_to_total(struct f2fs_sb_info *sbi, unsigned int max_gc_idx)
{
	struct gc_profiler_snapshot *p_gcp = sbi->cur_pgc_profiler[max_gc_idx];
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	
	cur_gcp->gc_p0_lat += p_gcp->gc_p0_lat;
	cur_gcp->gc_p1_lat += p_gcp->gc_p1_lat;
	cur_gcp->gc_p2_lat += p_gcp->gc_p2_lat;
	cur_gcp->gc_p3_lat += p_gcp->gc_p3_lat;
	cur_gcp->gc_p4_lat += p_gcp->gc_p4_lat;

	cur_gcp->p1_ra_node_lat += p_gcp->p1_ra_node_lat;
	cur_gcp->p2_ra_inode_lat += p_gcp->p2_ra_inode_lat;
	cur_gcp->p2_check_inode_lat += p_gcp->p2_check_inode_lat;
	cur_gcp->p3_check1_lat += p_gcp->p3_check1_lat;
	cur_gcp->p3_check2_lat += p_gcp->p3_check2_lat;
	cur_gcp->p3_is_alive_lat += p_gcp->p3_is_alive_lat;
	cur_gcp->p3_iget_lat += p_gcp->p3_iget_lat;
	cur_gcp->p3_bef_read_lat += p_gcp->p3_bef_read_lat;
	cur_gcp->p3_read_lat += p_gcp->p3_read_lat;
	cur_gcp->p3_put_page_lat += p_gcp->p3_put_page_lat;
	cur_gcp->p3_add_gc_inode_lat += p_gcp->p3_add_gc_inode_lat;
	cur_gcp->p4_move_lat += p_gcp->p4_move_lat;
					 
	cur_gcp->__pcache_alloc_filemap_get_entry_lat += p_gcp->__pcache_alloc_filemap_get_entry_lat;
	cur_gcp->__pcache_alloc___________rmqueue_pcplist_lat += p_gcp->__pcache_alloc___________rmqueue_pcplist_lat;
	cur_gcp->__pcache_alloc________prep_new_page_lat += p_gcp->__pcache_alloc________prep_new_page_lat;
	cur_gcp->__pcache_alloc_add_folio_lat += p_gcp->__pcache_alloc_add_folio_lat;
	cur_gcp->pcache_alloc_lat += p_gcp->pcache_alloc_lat;
	cur_gcp->fmap_preread_lat += p_gcp->fmap_preread_lat;
	cur_gcp->read_lat += p_gcp->read_lat;
	cur_gcp->pcache_grab_lat += p_gcp->pcache_grab_lat;
	cur_gcp->__pcache_grab_filemap_get_entry_lat += p_gcp->__pcache_grab_filemap_get_entry_lat;
	cur_gcp->__pcache_grab_lock_folio_lat += p_gcp->__pcache_grab_lock_folio_lat;
	cur_gcp->bef_write_lat += p_gcp->bef_write_lat;
	cur_gcp->bef_write_1_lat += p_gcp->bef_write_1_lat;
	cur_gcp->bef_write_5_lat += p_gcp->bef_write_5_lat;
	cur_gcp->bef_write_6_lat += p_gcp->bef_write_6_lat;
	cur_gcp->write_lat += p_gcp->write_lat;
	cur_gcp->fmap_write_lat += p_gcp->fmap_write_lat;
	cur_gcp->write_get_node_lat += p_gcp->write_get_node_lat;
	cur_gcp->set_pg_wb_lat += p_gcp->set_pg_wb_lat;
	cur_gcp->____allocate_data_block_lat += p_gcp->____allocate_data_block_lat;
	cur_gcp->____invalidate_internal_cache_lat += p_gcp->____invalidate_internal_cache_lat;
	cur_gcp->____submit_page_write_lat += p_gcp->____submit_page_write_lat;
	cur_gcp->__update_data_blkaddr_lat += p_gcp->__update_data_blkaddr_lat;
}
#endif /* GC_PROFILE_TIMESERIAL || LIGHT_PROFILE */

static int do_garbage_collect_pgc(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;
	/* used by the (unconditional) parallel-GC wake/wait loops below, so it
	 * can't live inside the profiling guard */
	int i;

	//printk("%s start: %s %u", __func__, current->comm, current->pid);
	bool prev_was_normal = false;
	/* fires once per GC section — hundreds of lines per pgc benchmark run,
	 * drowning dmesg (and paying printk cost on the GC path) */
	//printk("%s: start_segno: %llu type: %s", __func__,
	//	start_segno, (type == SUM_TYPE_DATA)? "DATA" : "NODE" );

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_t_ssa, end_t_ssa;
#endif
#ifdef JW_COPY_COMMAND
	int copy_submitted = 0;
#endif

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL
	end_t_ssa = OS_TimeGetNS_();
#endif

	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
	sbi->intra_zone_segidx_of_writer = 0;
#endif

	blk_start_plug(&plug);
	unsigned int pgc_cnt = 0, is_pgc = 0;
	for (segno = start_segno; segno < end_segno; segno++) {
		is_pgc = 0;

#ifdef GC_PROFILE_TIMESERIAL	
		start_t_ssa = OS_TimeGetNS_();
#endif

		/* find segment summary of victim */
		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, segno));
		f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
		end_t_ssa = OS_TimeGetNS_();
		cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif

		if (get_valid_blocks(sbi, segno, false) == 0) {
			//printk("%s: SKIP 1", __func__);
			goto freed;
		}
		if (gc_type == BG_GC && __is_large_section(sbi) &&
				migrated >= sbi->migration_granularity)
			goto skip;
		if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi)))
			goto skip;

		sum = page_address(sum_page);
		if (type != GET_SUM_TYPE((&sum->footer))) {
			f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
				 segno, type, GET_SUM_TYPE((&sum->footer)));
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			f2fs_stop_checkpoint(sbi, false,
				STOP_CP_REASON_CORRUPTED_SUMMARY);
			goto skip;
		}

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		/*
		 * this is to avoid deadlock:
		 * - lock_page(sum_page)         - f2fs_replace_block
		 *  - check_valid_map()            - down_write(sentry_lock)
		 *   - down_read(sentry_lock)     - change_curseg()
		 *                                  - lock_page(sum_page)
		 */


  		if (parallel_gc(sbi, gc_type, segno)) {
			pgc_cnt ++;
			is_pgc = 1;
			//printk("%s: start parallel GC!!", __func__);
  		  	for (i = 0 ; i < NUM_PGC_THREADS ; i++) {
  		  	  	struct f2fs_p_gc_kthread* p_gc = &sbi->gc_thread->p_gc[i];
  		  	  	p_gc->done = false;
  		  	  	p_gc->segno = segno;
				p_gc->type = type;
				p_gc->sum = sum;
				p_gc->submitted = 0;
  		  	  	//p_gc->seg_freed = 0;
  		  	  	//p_gc->should_migrate_blocks = gc_control->should_migrate_blocks;
  		  	  	wake_up(&p_gc->gc_wait_queue_head);
  		  	}
			//printk("%s: wait parallel GC!!", __func__);
  		  	for (i = 0 ; i < NUM_PGC_THREADS ; i++) {
  		  	  	struct f2fs_p_gc_kthread* p_gc = &sbi->gc_thread->p_gc[i];
  		  	  	while(p_gc->done == false) {
  		  	  	 	 wait_event_interruptible_timeout(p_gc->caller_wq,
  		  	  	 	     p_gc->done == true, msecs_to_jiffies(300));
  		  	  	}
				submitted += p_gc->submitted;
  		  	}
#ifdef GC_PROFILE_TIMESERIAL	
			if (type == SUM_TYPE_DATA) {
				unsigned int max_gc_idx = get_max_pgc_idx(sbi);
				//printk("%s: max_pgc_idx: %u", __func__, max_gc_idx);
				reflect_slowest_gc_to_total(sbi, max_gc_idx);
				reset_pgc_profiler(sbi);
			}
#endif
			//printk("%s: finish parallel GC!!", __func__);

  		} else {
			if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
				start_t_ssa = OS_TimeGetNS_();
#endif
				submitted += gc_node_segment(sbi, sum->entries, segno,
									gc_type);
#ifdef GC_PROFILE_TIMESERIAL	
				end_t_ssa = OS_TimeGetNS_();
				cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
				cur_gcp->node_seg_cnt ++;
#endif

			}
			else {
				submitted += gc_data_segment(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate);
			}
		}
#ifdef GC_PROFILE_TIMESERIAL	
		sbi->intra_zone_segidx_of_prereader ++;
		sbi->intra_zone_segidx_of_writer ++;
#endif

		stat_inc_gc_seg_count(sbi, data_type, gc_type);
		sbi->gc_reclaimed_segs[sbi->gc_mode]++;
		migrated++;

freed:
		if (gc_type == FG_GC &&
				get_valid_blocks(sbi, segno, false) == 0) {
			seg_freed++;
		} else if (gc_type == FG_GC && get_valid_blocks(sbi, segno, false) != 0) {
			printk("%s: seg not cleaned !! is_pgc: %u, vblk: %llu", __func__, is_pgc, 
				get_valid_blocks(sbi, segno, false));
		}

		if (__is_large_section(sbi))
			sbi->next_victim_seg[gc_type] =
				(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
		f2fs_put_page(sum_page, 0);
	}

	/* JW PGC added */
	if (get_sec_entry(sbi, start_segno)->valid_blocks != 0) {
		printk("%s: not fully cleaned !!! vblks: %llu . pgc_cnt: %llu ", __func__, 
			get_sec_entry(sbi, start_segno)->valid_blocks, pgc_cnt);
	}

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t_ssa - start_t_ssa;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
#ifdef JW_COPY_COMMAND
	sbi->gc_written_blk += submitted + copy_submitted;
	sbi->total_gc_written_blk += submitted + copy_submitted;
#else
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif
#endif

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s end: %s %u", __func__, current->comm, current->pid);
	return seg_freed;
}

bool parallel_gc(struct f2fs_sb_info* sbi, int gc_type, unsigned int segno) {
  if (gc_type != FG_GC) {
    return false;
  }

  //if (get_valid_blocks(sbi, segno, false) >= 256) {
  //if (get_valid_blocks(sbi, segno, false) >= 128) {
  if (get_valid_blocks(sbi, segno, false) >= 190) {
    return true;
  } else {
    return false;
  }
}

#endif

static int do_garbage_collect(struct f2fs_sb_info *sbi,
				unsigned int start_segno,
				struct gc_inode_list *gc_list, int gc_type,
				bool force_migrate)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	unsigned int segno = start_segno;
	unsigned int end_segno = start_segno + SEGS_PER_SEC(sbi);
	int seg_freed = 0, migrated = 0;
	unsigned char type = IS_DATASEG(get_seg_entry(sbi, segno)->type) ?
						SUM_TYPE_DATA : SUM_TYPE_NODE;
	unsigned char data_type = (type == SUM_TYPE_DATA) ? DATA : NODE;
	int submitted = 0;

	//printk("%s start: %s %u", __func__, current->comm, current->pid);
	//printk("%s start: %s %u", __func__, current->comm, current->pid);
	bool prev_was_normal = false;
	bool already_invalid_seg = false;

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	static int do_gc_cnt = 0;
	unsigned long long start_t = OS_TimeGetNS_();
	unsigned long long end_t;
#endif
#ifdef GC_PROFILE_TIMESERIAL
	unsigned long long start_t_ssa, end_t_ssa;
#endif
#ifdef JW_COPY_COMMAND
	int copy_submitted = 0;
	static unsigned int prev_victim_secno = NULL_SEGNO;
#endif

	if (__is_large_section(sbi))
		end_segno = rounddown(end_segno, SEGS_PER_SEC(sbi));

	/*
	 * zone-capacity can be less than zone-size in zoned devices,
	 * resulting in less than expected usable segments in the zone,
	 * calculate the end segno in the zone which can be garbage collected
	 */
	if (f2fs_sb_has_blkzoned(sbi))
		end_segno -= SEGS_PER_SEC(sbi) -
					f2fs_usable_segs_in_sec(sbi, segno);

	sanity_check_seg_type(sbi, get_seg_entry(sbi, segno)->type);

#ifdef GC_PROFILE_TIMESERIAL
	start_t_ssa = OS_TimeGetNS_();
#endif
	/* readahead multi ssa blocks those have contiguous address */
	if (__is_large_section(sbi))
		f2fs_ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno),
					end_segno - segno, META_SSA, true);
#ifdef GC_PROFILE_TIMESERIAL
	end_t_ssa = OS_TimeGetNS_();
#endif

	/* reference all summary page */
	unsigned int vblks_of_victim_sec = 0;
	unsigned int victim_secno = GET_SEC_FROM_SEG(sbi, segno);
	unsigned int vblks_of_victim_sec_check = get_sec_entry(sbi, segno)->valid_blocks;

	unsigned int nblks = BLKS_PER_SEC(sbi);
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
	cur_gcp->do_gc_cnt += 1;
#ifdef JW_COPY_COMMAND
	cur_gcp->include_normal = false;
#endif

#endif
#ifdef GC_PROFILE_TIMESERIAL	
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
	start_t_ssa = OS_TimeGetNS_();
#endif
	

	while (segno < end_segno) {
		vblks_of_victim_sec += get_seg_entry(sbi, segno)->valid_blocks;
		sum_page = f2fs_get_sum_page(sbi, segno++);
		if (IS_ERR(sum_page)) {
			int err = PTR_ERR(sum_page);

			end_segno = segno - 1;
			for (segno = start_segno; segno < end_segno; segno++) {
				sum_page = find_get_page(META_MAPPING(sbi),
						GET_SUM_BLOCK(sbi, segno));
				f2fs_put_page(sum_page, 0);
				f2fs_put_page(sum_page, 0);
			}
			printk("%s: sum error return", __func__);
			return err;
		}
		unlock_page(sum_page);
	}
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	sbi->tmp_cur_sec_util = vblks_of_victim_sec * 100 / nblks;
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;	
	if (vblks_of_victim_sec == vblks_of_victim_sec_check
			&&  vblks_of_victim_sec_check * 100 / nblks == GC_REQ_TARGET_SEC_UTIL) {
		sbi->set_gc_req = true;	
		sbi->gc_start_ = true;	
	}
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	sbi->intra_zone_segidx_of_prereader = 0;
	sbi->intra_zone_segidx_of_writer = 0;
#endif

//	printk("%s start: secno: %u vblks: %u type: %s process: %s %u", __func__, 
//	victim_secno, vblks_of_victim_sec, (type == SUM_TYPE_NODE)? "NODE" : "DATA", current->comm, current->pid);
	
	blk_start_plug(&plug);

	for (segno = start_segno; segno < end_segno; segno++) {

#ifdef GC_PROFILE_TIMESERIAL	
		start_t_ssa = OS_TimeGetNS_();
#endif

		/* find segment summary of victim */
		sum_page = find_get_page(META_MAPPING(sbi),
					GET_SUM_BLOCK(sbi, segno));
		f2fs_put_page(sum_page, 0);
#ifdef GC_PROFILE_TIMESERIAL	
		end_t_ssa = OS_TimeGetNS_();
		cur_gcp->read_ssa_lat += end_t_ssa - start_t_ssa;
#endif

		if (get_valid_blocks(sbi, segno, false) == 0) {
			//printk("%s: SKIP 1", __func__);
			already_invalid_seg = true;
			goto freed;
		}
		if (gc_type == BG_GC && __is_large_section(sbi) &&
				migrated >= sbi->migration_granularity)
			goto skip;
		if (!PageUptodate(sum_page) || unlikely(f2fs_cp_error(sbi))) {
			printk("%s skip due to meta error", __func__);
			goto skip;
		}

		sum = page_address(sum_page);
		if (type != GET_SUM_TYPE((&sum->footer))) {
			f2fs_err(sbi, "Inconsistent segment (%u) type [%d, %d] in SSA and SIT",
				 segno, type, GET_SUM_TYPE((&sum->footer)));
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			f2fs_stop_checkpoint(sbi, false,
				STOP_CP_REASON_CORRUPTED_SUMMARY);
			goto skip;
		}

#ifdef GC_PAGE_POOL
		//sbi->gc_page_holder_next_idx = 0;
#endif

		/*
		 * this is to avoid deadlock:
		 * - lock_page(sum_page)         - f2fs_replace_block
		 *  - check_valid_map()            - down_write(sentry_lock)
		 *   - down_read(sentry_lock)     - change_curseg()
		 *                                  - lock_page(sum_page)
		 */
		if (type == SUM_TYPE_NODE){
#ifdef GC_PROFILE_TIMESERIAL	
			start_t_ssa = OS_TimeGetNS_();
#endif
			submitted += gc_node_segment(sbi, sum->entries, segno,
								gc_type);
#ifdef GC_PROFILE_TIMESERIAL	
			end_t_ssa = OS_TimeGetNS_();
			cur_gcp->node_seg_lat += end_t_ssa - start_t_ssa;
			cur_gcp->node_seg_cnt ++;
#endif
#if defined(JW_COPY_COMMAND) && (defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE))
			cur_gcp->include_normal = true;
#endif
		}
		else {
#ifdef JW_COPY_COMMAND
			unsigned int first_zoned_segno_ = first_zoned_segno(sbi);
			if (first_zoned_segno_ > segno || 
				CURSEG_I(sbi, CURSEG_COLD_DATA)->segno < first_zoned_segno_ || 
				CURSEG_I(sbi, CURSEG_COLD_DATA)->next_segno < first_zoned_segno_
				|| prev_victim_secno == victim_secno
				) {
			//if (f2fs_target_device_idx(sbi, START_BLOCK(sbi, segno)) != ZONED_DEV_IDX) {
				submitted += gc_data_segment(sbi, sum->entries, gc_list,
								segno, gc_type,
								force_migrate);
				prev_was_normal = true;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
				cur_gcp->include_normal = true;
#endif
			} else {
				if (prev_was_normal) {
					f2fs_submit_merged_write(sbi, DATA);
				}
				submitted += gc_data_segment_copy(sbi, sum->entries, gc_list,
								segno, gc_type, &copy_submitted, force_migrate);
				prev_was_normal = false;
			}
#else
			submitted += gc_data_segment(sbi, sum->entries, gc_list,
							segno, gc_type,
							force_migrate);
#endif
		}
#ifdef GC_PROFILE_TIMESERIAL	
		sbi->intra_zone_segidx_of_prereader ++;
		sbi->intra_zone_segidx_of_writer ++;
#endif

		stat_inc_gc_seg_count(sbi, data_type, gc_type);
		sbi->gc_reclaimed_segs[sbi->gc_mode]++;
		migrated++;

freed:
#ifdef COPY_ASYNC_META_UPDATE
		if (type != SUM_TYPE_NODE){
			if (gc_type == FG_GC) {
				if (already_invalid_seg) {
					printk("%s already invalid segno %u done. secno: %u type: %s vblks: %u process: %s %u", __func__, 
						segno, victim_secno, (type == SUM_TYPE_NODE)? "NODE" : "DATA", get_valid_blocks(sbi, segno, false), current->comm, current->pid);
					seg_freed++;
				}
				else if (prev_was_normal) {
					if (get_valid_blocks(sbi, segno, false) == 0) {
						printk("%s normal segno %u done. secno: %u type: %s vblks: %u process: %s %u", __func__, 
						segno, victim_secno, (type == SUM_TYPE_NODE)? "NODE" : "DATA", get_valid_blocks(sbi, segno, false), current->comm, current->pid);
						seg_freed++;
					} else {
						unsigned int real_vblk = 0, tmp_vblk;
						struct sit_info *sit_i = SIT_I(sbi);
						for (unsigned int iii = 0; iii < 512; iii++) {

							if (check_valid_map(sbi, segno, iii) == 1){
								real_vblk ++;
								printk("%s: vblk_off: %u segno: %u secno: %u", __func__, iii, segno, victim_secno);
							}
						}
						tmp_vblk = get_valid_blocks(sbi, segno, false);
						printk("%s: prob_segno: %u secno: %u vblk: %u real_vblk: %u", 
						__func__, segno, victim_secno, tmp_vblk, real_vblk);
						down_read(&sit_i->sentry_lock);
						tmp_vblk = get_valid_blocks(sbi, segno, false);
						struct seg_entry *se;
						se = get_seg_entry(sbi, segno);
						se->valid_blocks = 0;
						get_sec_entry(sbi, segno)->valid_blocks -= tmp_vblk;
						up_read(&sit_i->sentry_lock);
						
						//f2fs_bug_on(sbi, 1);
					}
				} else {
					seg_freed++;
					printk("%s copied segno %u done. secno: %u type: %s vblks: %u process: %s %u", __func__, 
					segno, victim_secno, (type == SUM_TYPE_NODE)? "NODE" : "DATA", get_valid_blocks(sbi, segno, false), current->comm, current->pid);
				}
			}
		} else {
			if (gc_type == FG_GC &&
					get_valid_blocks(sbi, segno, false) == 0)
				seg_freed++;
		}
		already_invalid_seg = false;
#else
		if (gc_type == FG_GC &&
				get_valid_blocks(sbi, segno, false) == 0)
			seg_freed++;
#endif
		if (__is_large_section(sbi))
			sbi->next_victim_seg[gc_type] =
				(segno + 1 < end_segno) ? segno + 1 : NULL_SEGNO;
skip:
		f2fs_put_page(sum_page, 0);
	}

#ifdef GC_PROFILE_TIMESERIAL	
	start_t_ssa = OS_TimeGetNS_();
#endif
	if (submitted)
		f2fs_submit_merged_write(sbi, data_type);
#ifdef GC_PROFILE_TIMESERIAL	
	end_t_ssa = OS_TimeGetNS_();
	cur_gcp->merged_write_lat += end_t_ssa - start_t_ssa;
#endif

	blk_finish_plug(&plug);

	if (migrated)
		stat_inc_gc_sec_count(sbi, data_type, gc_type);
#ifdef WAF
#ifdef JW_COPY_COMMAND
	sbi->gc_written_blk += submitted + copy_submitted;
	sbi->total_gc_written_blk += submitted + copy_submitted;
	prev_victim_secno = victim_secno;
#else
	sbi->gc_written_blk += submitted;
	sbi->total_gc_written_blk += submitted;
#endif
#endif

#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)	
	end_t = OS_TimeGetNS_();
	cur_gcp->do_gc_lat += end_t - start_t;
#endif
	//printk("%s end: secno: %u vblks: %u type: %s process: %s %u", __func__, 
	//victim_secno, vblks_of_victim_sec, (type == SUM_TYPE_NODE)? "NODE" : "DATA", current->comm, current->pid);
	//printk("%s end: %s %u", __func__, current->comm, current->pid);
	return seg_freed;
}


static inline void print_WAF_(struct f2fs_sb_info *sbi)
{
	uint64_t host_written_blk = atomic_read(&sbi->host_written_blk);
	uint64_t total_host_written_blk = atomic_read(&sbi->total_host_written_blk);
	if (host_written_blk - sbi->gc_written_blk > 0) {
		unsigned int waf = 
			(100* (host_written_blk)) / (host_written_blk - sbi->gc_written_blk);
		unsigned int total_waf = 
			(100* (total_host_written_blk)) 
			/ (total_host_written_blk - sbi->total_gc_written_blk);
		printk("%s: FSWAF: %u percent gc: %llu KB write_req: %llu KB total: %llu total WAF: %u", 
			__func__, waf, sbi->gc_written_blk*4, (host_written_blk - sbi->gc_written_blk)*4, 
			total_host_written_blk*4, total_waf);
		//printk("%s: total transfer volumes by FSGC: write: %llu KB read: %llu KB", __func__, 
		//		sbi->total_gc_written_blk*4, sbi->total_gc_read_blk*4);
	}
}

static inline void try_print_WAF_(struct f2fs_sb_info *sbi) {
#ifdef GC_PROFILE_TIMESERIAL	
	unsigned long long cur_t = OS_TimeGetUS_();
#endif
//if (cur_t - sbi->last_t > WAF_TIME_INTERVAL) {
		//print_WAF_(sbi);
		
#ifdef GC_PROFILE_TIMESERIAL	
		sbi->last_t = cur_t;
#endif
		sbi->gc_written_blk = 0;
		atomic_set(&sbi->host_written_blk, 0);
	//}
}


int f2fs_gc(struct f2fs_sb_info *sbi, struct f2fs_gc_control *gc_control)
{
	//printk("%s start: thread: %s %u", __func__, current->comm, current->pid);
#ifdef CHECK_GC_RACE
	printk("%s start: thread: %s %u", __func__, current->comm, current->pid);
	//dump_stack();
	atomic_set(&sbi->gcing, 1);
#endif
	
#ifdef JW_COPY_COMMAND
	unsigned int tmptmp;
#endif
	int gc_type = gc_control->init_gc_type;
	unsigned int segno = gc_control->victim_segno;
	int sec_freed = 0, seg_freed = 0, total_freed = 0, total_sec_freed = 0;
	int ret = 0;
	struct cp_control cpc;
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(gc_list.iroot, GFP_NOFS),
	};
#ifdef PGC
	struct gc_inode_list p_gc_list[MAX_GC_THREAD];
	for (int i = 0 ; i < NUM_PGC_THREADS ; i++) {
	 	 p_gc_list[i].ilist.next = &p_gc_list[i].ilist;
	 	 p_gc_list[i].ilist.prev = &p_gc_list[i].ilist;
	 	 p_gc_list[i].iroot.xa_lock = __SPIN_LOCK_UNLOCKED(p_gc_list[i].iroot.xa_lock);
	 	 p_gc_list[i].iroot.xa_flags = GFP_NOFS;
	 	 p_gc_list[i].iroot.xa_head = NULL;
	};
	/* JW PGC debug */
  	for (int i = 0 ; i < NUM_PGC_THREADS ; i++) {
  	  	struct f2fs_p_gc_kthread* p_gc = &sbi->gc_thread->p_gc[i];
  	  	p_gc->gc_list = &p_gc_list[i];
  	  	p_gc->should_migrate_blocks = gc_control->should_migrate_blocks;
  	}

#endif
	unsigned int skipped_round = 0, round = 0;
	unsigned int upper_secs;
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	struct gc_profiler_snapshot *cur_gcp = sbi->cur_gc_profiler;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	static bool is_first_gc = true;
	unsigned long long cp_start_t, cp_end_t, tmp_cp_latency = 0;
	bool cp_done = false;
	unsigned long long victim_select_start_t, victim_select_end_t;

	unsigned long long whole_gc_start_t, whole_gc_end_t;
	whole_gc_start_t = OS_TimeGetNS_();
	int do_gc_cnt = 0;
#endif
	trace_f2fs_gc_begin(sbi->sb, gc_type, gc_control->no_bg_gc,
				gc_control->nr_free_secs,
				get_pages(sbi, F2FS_DIRTY_NODES),
				get_pages(sbi, F2FS_DIRTY_DENTS),
				get_pages(sbi, F2FS_DIRTY_IMETA),
				free_sections(sbi),
				free_segments(sbi),
				reserved_segments(sbi),
				prefree_segments(sbi));

	cpc.reason = __get_cp_reason(sbi);
gc_more:
	sbi->skipped_gc_rwsem = 0;
	if (unlikely(!(sbi->sb->s_flags & SB_ACTIVE))) {
		ret = -EINVAL;
		goto stop;
	}
	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		goto stop;
	}

	/* Let's run FG_GC, if we don't have enough space. */
	if (has_not_enough_free_secs_gc_trigger(sbi, 0, 0)) {
		gc_type = FG_GC;
		//dump_stack();
		//printk("%s: bef gc cp. free_secs: %u total_secs: %u ", 
		//		__func__, 
		//		free_sections(sbi),  
		//		sbi->total_sections
		//		);
		/*
		 * For example, if there are many prefree_segments below given
		 * threshold, we can make them free by checkpoint. Then, we
		 * secure free segments which doesn't need fggc any more.
		 */
		if (prefree_segments(sbi)) {
			stat_inc_cp_call_count(sbi, TOTAL_CALL);
#ifdef GC_PROFILE_TIMESERIAL	
			cp_done = true;
			cp_start_t = OS_TimeGetNS_();
#endif
//#ifdef GC_CDF
#ifdef GC_CP
			ret = f2fs_write_checkpoint_gc(sbi, &cpc);
#else
			ret = f2fs_write_checkpoint(sbi, &cpc);
#endif
#ifdef GC_PROFILE_TIMESERIAL	
			cp_end_t = OS_TimeGetNS_();
			tmp_cp_latency = cp_end_t - cp_start_t;
#endif
			if (ret)
				goto stop;
			/* Reset due to checkpoint */
			sec_freed = 0;
		}
	}

	/* f2fs_balance_fs doesn't need to do BG_GC in critical path. */
	if (gc_type == BG_GC && gc_control->no_bg_gc) {
		ret = -EINVAL;
		goto stop;
	}
retry:
	ret = __get_victim(sbi, &segno, gc_type);
	if (ret) {
		/* allow to search victim from sections has pinned data */
		if (ret == -ENODATA && gc_type == FG_GC &&
				f2fs_pinned_section_exists(DIRTY_I(sbi))) {
			f2fs_unpin_all_sections(sbi, false);
			goto retry;
		}
		goto stop;
	}
#ifdef GC_PROFILE_TIMESERIAL	
	if (is_first_gc && gc_type == FG_GC){
		printk("[JWDBG] %s: do %s!!", __func__, gc_type == FG_GC? "FG_GC" : "BG_GC");
		is_first_gc = false;
		sbi->gc_start = true;
	}
#endif

	/* sbi->tmp_cur_sec_util is set after do_garbage_collect */
#ifdef GC_PAGE_POOL
	sbi->gc_page_holder_next_idx = 0;
#endif


#ifdef GC_PREREAD
#ifdef GC_PIPELINE
#ifdef GC_PIPELINE_BATCHED 
	if (gc_type == FG_GC) {
#ifdef GC_DEEP_PIPELINE_BATCHED 
		if (IS_DATASEG(get_seg_entry(sbi, segno)->type)) {
			/* 4 step deep pipelinig for data type sec */
			//printk("%s: FG_GC data sec GC start, segno: %lu", __func__, segno);
			seg_freed = do_garbage_collect_batched_deep_pipelined(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
			//printk("%s: FG_GC data sec GC end. seg_free: %lu", __func__, seg_freed);
			/*
			 * A full-section FG_GC should reclaim every usable segment
			 * in the section. Compare against the actual usable count
			 * (which can be < segs-per-section on zoned devices, where
			 * the hardcoded 512 was wrong) and only warn: this is a
			 * diagnostic, not a corruption, so avoid f2fs_bug_on's
			 * WARN splat and SBI_NEED_FSCK flag.
			 */
			if (seg_freed != f2fs_usable_segs_in_sec(sbi, segno))
				f2fs_warn(sbi,
					"deep-pipelined data GC freed %d/%u segs in sec %u",
					seg_freed, f2fs_usable_segs_in_sec(sbi, segno),
					GET_SEC_FROM_SEG(sbi, segno));
		} else {
			/* 2 step pipelinig for node type sec */
			//printk("%s: FG_GC node sec GC start", __func__);
			seg_freed = do_garbage_collect_batched_block_granularity_pipelined(sbi, segno, &gc_list,
				   	gc_type, gc_control->should_migrate_blocks);
			//printk("%s: FG_GC node sec GC end", __func__);
		}	
#elif defined (GC_BLOCK_GRANULARITY_PIPELINE_BATCHED) 
		seg_freed = do_garbage_collect_batched_block_granularity_pipelined(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
#else
		seg_freed = do_garbage_collect_batched_pipelined(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
#endif
	} else {
		/* Allow normal BG_GC */
		seg_freed = do_garbage_collect(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	}
#else	/* else of GC_PIPELINE_BATCHED */
	if (gc_type == FG_GC) {
		seg_freed = do_garbage_collect_pipelined(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	} else {
		/* Allow normal BG_GC */
		seg_freed = do_garbage_collect(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	}
#endif // END of GC_PIPELINE_BATCHED
#else	/* else of GC_PIPELINE */
	if (gc_type == FG_GC) {
		seg_freed = do_garbage_collect_early_read(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	} else {
		/* Allow normal BG_GC */
		seg_freed = do_garbage_collect(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	}
#endif // END of GC_PIPELINE
#elif defined (GC_PAGE_POOL)
	if (gc_type == FG_GC) {
		seg_freed = do_garbage_collect_gc_pool(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	} else {
		/* Allow normal BG_GC */
		seg_freed = do_garbage_collect(sbi, segno, &gc_list, gc_type,
					gc_control->should_migrate_blocks);
	}
#else
#ifdef PGC
	seg_freed = do_garbage_collect_pgc(sbi, segno, &gc_list, gc_type,
  	      			gc_control->should_migrate_blocks);
#else
	seg_freed = do_garbage_collect(sbi, segno, &gc_list, gc_type,
				gc_control->should_migrate_blocks);
#endif
#endif

#ifdef FILTER_GC_REQ
	sbi->set_gc_req = false;
	sbi->gc_start_ = false;
	sbi->gc_end = false;
	sbi->gc_last_seg = false;
#endif

#ifdef GC_PAGE_POOL
	sbi->gc_page_holder_next_idx = 0;
#endif
#ifdef GC_PROFILE_TIMESERIAL	
	do_gc_cnt ++;
	if (cp_done) {
		cur_gcp->cp_lat	+= tmp_cp_latency;
	}
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	cur_gcp->sec_util_sum	+= sbi->tmp_cur_sec_util;
#endif
	if (seg_freed < 0)
		goto stop;

	total_freed += seg_freed;

	if (seg_freed == f2fs_usable_segs_in_sec(sbi, segno)) {
		sec_freed++;
		total_sec_freed++;
	} 
	// else {
		//printk("%s: WHY!!!!!. seg_free: %u", __func__, seg_freed);
	//}

	if (gc_type == FG_GC) {
		sbi->cur_victim_sec = NULL_SEGNO;

		if (has_enough_free_secs(sbi, sec_freed, 0)) {
			if (!gc_control->no_bg_gc &&
			    total_sec_freed < gc_control->nr_free_secs) {
				//printk("%s: gc more", __func__);
				goto go_gc_more;
			}
			//printk("%s: gc stop", __func__);
			goto stop;
		}
		if (sbi->skipped_gc_rwsem)
			skipped_round++;
		round++;
		if (skipped_round > MAX_SKIP_GC_COUNT &&
				skipped_round * 2 >= round) {
			stat_inc_cp_call_count(sbi, TOTAL_CALL);
			ret = f2fs_write_checkpoint(sbi, &cpc);
			goto stop;
		}
	} else if (has_enough_free_secs(sbi, 0, 0)) {
		goto stop;
	}

	__get_secs_required(sbi, NULL, &upper_secs, NULL);

	/*
	 * Write checkpoint to reclaim prefree segments.
	 * We need more three extra sections for writer's data/node/dentry.
	 */
	if (free_sections(sbi) <= upper_secs + NR_GC_CHECKPOINT_SECS &&
				prefree_segments(sbi)) {
		stat_inc_cp_call_count(sbi, TOTAL_CALL);
		ret = f2fs_write_checkpoint(sbi, &cpc);
		if (ret)
			goto stop;
		/* Reset due to checkpoint */
		sec_freed = 0;
	}
	//printk("%s: gc more 2", __func__);
go_gc_more:
	segno = NULL_SEGNO;
	goto gc_more;

stop:
#ifdef JW_COPY_COMMAND
#ifdef GC_PROFILE_TIMESERIAL	
	cp_start_t = OS_TimeGetNS_();
#endif
//	stat_inc_cp_call_count(sbi, TOTAL_CALL);
//	ret = f2fs_write_checkpoint(sbi, &cpc);
//	if (ret)
//		f2fs_bug_on(sbi, 1);
wait_copy:
	if ((tmptmp = (unsigned int ) get_page_copied(sbi))) {
		//printk("%s get_page_copied: %u", __func__, tmptmp);
		cond_resched();
		goto wait_copy;
	}
#ifdef GC_PROFILE_TIMESERIAL	
	cp_end_t = OS_TimeGetNS_();
	tmp_cp_latency = cp_end_t - cp_start_t;
	cur_gcp->post_copy_cp_lat	+= tmp_cp_latency;
#endif
#endif
	SIT_I(sbi)->last_victim[ALLOC_NEXT] = 0;
	SIT_I(sbi)->last_victim[FLUSH_DEVICE] = gc_control->victim_segno;

	if (gc_type == FG_GC)
		f2fs_unpin_all_sections(sbi, true);

	trace_f2fs_gc_end(sbi->sb, ret, total_freed, total_sec_freed,
				get_pages(sbi, F2FS_DIRTY_NODES),
				get_pages(sbi, F2FS_DIRTY_DENTS),
				get_pages(sbi, F2FS_DIRTY_IMETA),
				free_sections(sbi),
				free_segments(sbi),
				reserved_segments(sbi),
				prefree_segments(sbi));

#ifdef GC_PROFILE_TIMESERIAL	
	whole_gc_end_t = OS_TimeGetNS_();
	cur_gcp->whole_gc_lat += whole_gc_end_t - whole_gc_start_t;
	cur_gcp->whole_gc_cnt ++;
		
	if (do_gc_cnt == 0) {
		cur_gcp->whole_skipped_gc_cnt ++;
		cur_gcp->whole_skipped_gc_lat += whole_gc_end_t - whole_gc_start_t;
	} else if (do_gc_cnt == 1) {
		cur_gcp->whole_performed_gc_cnt ++;
		cur_gcp->whole_performed_gc_lat += whole_gc_end_t - whole_gc_start_t;
	} else if (do_gc_cnt > 1) {
		cur_gcp->whole_performed_gc_cnt += do_gc_cnt;
		cur_gcp->whole_performed_gc_lat += whole_gc_end_t - whole_gc_start_t;
		cur_gcp->whole_multi_gc_cnt ++;
		cur_gcp->whole_multi_gc_lat += whole_gc_end_t - whole_gc_start_t;
	}
#endif
#if defined(GC_PROFILE_TIMESERIAL) || defined(LIGHT_PROFILE)
	print_cur_gc_profiler(sbi);
	if (cur_gcp->do_gc_cnt > 0) {
		try_print_WAF_(sbi);
	}
	reset_cur_gc_profiler(sbi);
#endif

	f2fs_up_write(&sbi->gc_lock);

	put_gc_inode(&gc_list);
#ifdef PGC
  	for (int i = 0 ; i < NUM_PGC_THREADS ; i++) {
  	 	put_gc_inode(&p_gc_list[i]);
  	}
#endif
	if (gc_control->err_gc_skipped && !ret)
		ret = total_sec_freed ? 0 : -EAGAIN;

	//printk("%s end", __func__);
	//printk("%s end: thread: %s %u", __func__, current->comm, current->pid);
#ifdef CHECK_GC_RACE
	printk("%s end: thread: %s %u", __func__, current->comm, current->pid);
	atomic_set(&sbi->gcing, 0);
#endif
	//printk("%s return. ", __func__);
	return ret;
}

#ifdef JW_COPY_COMMAND
#ifdef COPY_ASYNC_META_UPDATE
int __init create_endio_copy_meta_cache(void)
{
	endio_copy_meta_slab = f2fs_kmem_cache_create("f2fs_endio_copy_meta_cache",
					sizeof(struct endio_copy_meta));
	return endio_copy_meta_slab ? 0 : -ENOMEM;
}

void destroy_endio_copy_meta_cache(void)
{
	kmem_cache_destroy(endio_copy_meta_slab);
}
#endif
#endif

int __init f2fs_create_garbage_collection_cache(void)
{
	victim_entry_slab = f2fs_kmem_cache_create("f2fs_victim_entry",
					sizeof(struct victim_entry));
	return victim_entry_slab ? 0 : -ENOMEM;
}

void f2fs_destroy_garbage_collection_cache(void)
{
	kmem_cache_destroy(victim_entry_slab);
}

#ifdef GC_PAGE_POOL
void *gc_page_array_alloc(struct f2fs_sb_info *sbi, int nr)
{
	unsigned int size = sizeof(struct page *) * nr;

	if (likely(size <= sbi->gc_page_array_slab_size))
		return f2fs_kmem_cache_alloc(sbi->gc_page_array_slab,
					GFP_F2FS_ZERO, false, sbi);
	return f2fs_kzalloc(sbi, size, GFP_NOFS);
}

void gc_page_array_free(struct f2fs_sb_info *sbi, void *pages, int nr)
{
	unsigned int size = sizeof(struct page *) * nr;

	if (!pages)
		return;

	if (likely(size <= sbi->gc_page_array_slab_size))
		kmem_cache_free(sbi->gc_page_array_slab, pages);
	else 
		kfree(pages);
}

void *gc_page_holder_array_alloc(struct f2fs_sb_info *sbi, int nr)
{
	unsigned int size = sizeof(struct page *) * nr;

	if (likely(size <= sbi->gc_page_holder_array_slab_size))
		return f2fs_kmem_cache_alloc(sbi->gc_page_holder_array_slab,
					GFP_F2FS_ZERO, false, sbi);
	return f2fs_kzalloc(sbi, size, GFP_NOFS);
}

void gc_page_holder_array_free(struct f2fs_sb_info *sbi, void *pages, int nr)
{
	unsigned int size = sizeof(struct page *) * nr;

	if (!pages)
		return;

	if (likely(size <= sbi->gc_page_holder_array_slab_size))
		kmem_cache_free(sbi->gc_page_holder_array_slab, pages);
	else 
		kfree(pages);
}

struct page *f2fs_gc_pool_alloc_page(void)
{
	struct page *page;

	page = mempool_alloc(gc_page_pool, GFP_NOFS);
	//lock_page(page);

	return page;
}

void f2fs_gc_pool_free_page(struct page *page)
{
	if (!page)
		return;
//	detach_page_private(page);
//	page->mapping = NULL;
//	unlock_page(page);
	mempool_free(page, gc_page_pool);
}


int __init f2fs_create_garbage_collection_page_pool(void)
{
#ifdef GC_PAGE_ARRAY
	return 0;
#else
	gc_page_pool = mempool_create_page_pool(N_GC_PAGES, 0);
	return gc_page_pool ? 0 : -ENOMEM;
#endif
}

void f2fs_destroy_garbage_collection_page_pool(void)
{
#ifdef GC_PAGE_ARRAY
	return;
#else
	mempool_destroy(gc_page_pool);
#endif
}

int f2fs_init_gc_page_array_cache(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	char slab_name[40];

	sprintf(slab_name, "f2fs_gc_page_array_entry-%u:%u", MAJOR(dev), MINOR(dev));

	//sbi->gc_page_array_slab_size = sizeof(struct page *) << LOG_N_GC_PAGES;
	sbi->gc_page_array_slab_size = sizeof(struct page *) * N_GC_PAGES;

	sbi->gc_page_array_slab = f2fs_kmem_cache_create(slab_name,
					sbi->gc_page_array_slab_size);
	return sbi->gc_page_array_slab ? 0 : -ENOMEM;
}

void f2fs_destroy_gc_page_array_cache(struct f2fs_sb_info *sbi)
{
	kmem_cache_destroy(sbi->gc_page_array_slab);
}

int f2fs_init_gc_page_holder_array_cache(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	char slab_name[50];

	sprintf(slab_name, "f2fs_gc_page_holder_array_entry-%u:%u", MAJOR(dev), MINOR(dev));

	sbi->gc_page_holder_array_slab_size = sizeof(struct page *) * N_GC_PAGES;

	sbi->gc_page_holder_array_slab = f2fs_kmem_cache_create(slab_name,
					sbi->gc_page_holder_array_slab_size);
	return sbi->gc_page_holder_array_slab ? 0 : -ENOMEM;
}

void f2fs_destroy_gc_page_holder_array_cache(struct f2fs_sb_info *sbi)
{
	kmem_cache_destroy(sbi->gc_page_holder_array_slab);
}

#endif

static void init_atgc_management(struct f2fs_sb_info *sbi)
{
	struct atgc_management *am = &sbi->am;

	if (test_opt(sbi, ATGC) &&
		SIT_I(sbi)->elapsed_time >= DEF_GC_THREAD_AGE_THRESHOLD)
		am->atgc_enabled = true;

	am->root = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&am->victim_list);
	am->victim_count = 0;

	am->candidate_ratio = DEF_GC_THREAD_CANDIDATE_RATIO;
	am->max_candidate_count = DEF_GC_THREAD_MAX_CANDIDATE_COUNT;
	am->age_weight = DEF_GC_THREAD_AGE_WEIGHT;
	am->age_threshold = DEF_GC_THREAD_AGE_THRESHOLD;
}

void f2fs_build_gc_manager(struct f2fs_sb_info *sbi)
{
	sbi->gc_pin_file_threshold = DEF_GC_FAILED_PINNED_FILES;

	/* give warm/cold data area from slower device */
	if (f2fs_is_multi_device(sbi) && !__is_large_section(sbi))
		SIT_I(sbi)->last_victim[ALLOC_NEXT] =
				GET_SEGNO(sbi, FDEV(0).end_blk) + 1;

	init_atgc_management(sbi);
}

int f2fs_gc_range(struct f2fs_sb_info *sbi,
		unsigned int start_seg, unsigned int end_seg,
		bool dry_run, unsigned int dry_run_sections)
{
	unsigned int segno;
	unsigned int gc_secs = dry_run_sections;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	for (segno = start_seg; segno <= end_seg; segno += SEGS_PER_SEC(sbi)) {
		struct gc_inode_list gc_list = {
			.ilist = LIST_HEAD_INIT(gc_list.ilist),
			.iroot = RADIX_TREE_INIT(gc_list.iroot, GFP_NOFS),
		};

		do_garbage_collect(sbi, segno, &gc_list, FG_GC,
						dry_run_sections == 0);
		put_gc_inode(&gc_list);

		if (!dry_run && get_valid_blocks(sbi, segno, true))
			return -EAGAIN;
		if (dry_run && dry_run_sections &&
		    !get_valid_blocks(sbi, segno, true) && --gc_secs == 0)
			break;

		if (fatal_signal_pending(current))
			return -ERESTARTSYS;
	}

	return 0;
}

static int free_segment_range(struct f2fs_sb_info *sbi,
				unsigned int secs, bool dry_run)
{
	unsigned int next_inuse, start, end;
	struct cp_control cpc = { CP_RESIZE, 0, 0, 0 };
	int gc_mode, gc_type;
	int err = 0;
	int type;

	/* Force block allocation for GC */
	MAIN_SECS(sbi) -= secs;
	start = MAIN_SECS(sbi) * SEGS_PER_SEC(sbi);
	end = MAIN_SEGS(sbi) - 1;

	mutex_lock(&DIRTY_I(sbi)->seglist_lock);
	for (gc_mode = 0; gc_mode < MAX_GC_POLICY; gc_mode++)
		if (SIT_I(sbi)->last_victim[gc_mode] >= start)
			SIT_I(sbi)->last_victim[gc_mode] = 0;

	for (gc_type = BG_GC; gc_type <= FG_GC; gc_type++)
		if (sbi->next_victim_seg[gc_type] >= start)
			sbi->next_victim_seg[gc_type] = NULL_SEGNO;
	mutex_unlock(&DIRTY_I(sbi)->seglist_lock);

	/* Move out cursegs from the target range */
	for (type = CURSEG_HOT_DATA; type < NR_CURSEG_PERSIST_TYPE; type++) {
		err = f2fs_allocate_segment_for_resize(sbi, type, start, end);
		if (err)
			goto out;
	}

	/* do GC to move out valid blocks in the range */
	err = f2fs_gc_range(sbi, start, end, dry_run, 0);
	if (err || dry_run)
		goto out;

	stat_inc_cp_call_count(sbi, TOTAL_CALL);
	err = f2fs_write_checkpoint(sbi, &cpc);
	if (err)
		goto out;

	next_inuse = find_next_inuse(FREE_I(sbi), end + 1, start);
	if (next_inuse <= end) {
		f2fs_err(sbi, "segno %u should be free but still inuse!",
			 next_inuse);
		f2fs_bug_on(sbi, 1);
	}
out:
	MAIN_SECS(sbi) += secs;
	return err;
}

static void update_sb_metadata(struct f2fs_sb_info *sbi, int secs)
{
	struct f2fs_super_block *raw_sb = F2FS_RAW_SUPER(sbi);
	int section_count;
	int segment_count;
	int segment_count_main;
	long long block_count;
	int segs = secs * SEGS_PER_SEC(sbi);

	f2fs_down_write(&sbi->sb_lock);

	section_count = le32_to_cpu(raw_sb->section_count);
	segment_count = le32_to_cpu(raw_sb->segment_count);
	segment_count_main = le32_to_cpu(raw_sb->segment_count_main);
	block_count = le64_to_cpu(raw_sb->block_count);

	raw_sb->section_count = cpu_to_le32(section_count + secs);
	raw_sb->segment_count = cpu_to_le32(segment_count + segs);
	raw_sb->segment_count_main = cpu_to_le32(segment_count_main + segs);
	raw_sb->block_count = cpu_to_le64(block_count +
			(long long)SEGS_TO_BLKS(sbi, segs));
	if (f2fs_is_multi_device(sbi)) {
		int last_dev = sbi->s_ndevs - 1;
		int dev_segs =
			le32_to_cpu(raw_sb->devs[last_dev].total_segments);

		raw_sb->devs[last_dev].total_segments =
						cpu_to_le32(dev_segs + segs);
	}

	f2fs_up_write(&sbi->sb_lock);
}

static void update_fs_metadata(struct f2fs_sb_info *sbi, int secs)
{
	int segs = secs * SEGS_PER_SEC(sbi);
	long long blks = SEGS_TO_BLKS(sbi, segs);
	long long user_block_count =
				le64_to_cpu(F2FS_CKPT(sbi)->user_block_count);

	SM_I(sbi)->segment_count = (int)SM_I(sbi)->segment_count + segs;
	MAIN_SEGS(sbi) = (int)MAIN_SEGS(sbi) + segs;
	MAIN_SECS(sbi) += secs;
	FREE_I(sbi)->free_sections = (int)FREE_I(sbi)->free_sections + secs;
	FREE_I(sbi)->free_segments = (int)FREE_I(sbi)->free_segments + segs;
	F2FS_CKPT(sbi)->user_block_count = cpu_to_le64(user_block_count + blks);

	if (f2fs_is_multi_device(sbi)) {
		int last_dev = sbi->s_ndevs - 1;

		FDEV(last_dev).total_segments =
				(int)FDEV(last_dev).total_segments + segs;
		FDEV(last_dev).end_blk =
				(long long)FDEV(last_dev).end_blk + blks;
#ifdef CONFIG_BLK_DEV_ZONED
		FDEV(last_dev).nr_blkz = FDEV(last_dev).nr_blkz +
					div_u64(blks, sbi->blocks_per_blkz);
#endif
	}
}

int f2fs_resize_fs(struct file *filp, __u64 block_count)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(filp));
	__u64 old_block_count, shrunk_blocks;
	struct cp_control cpc = { CP_RESIZE, 0, 0, 0 };
	unsigned int secs;
	int err = 0;
	__u32 rem;

	old_block_count = le64_to_cpu(F2FS_RAW_SUPER(sbi)->block_count);
	if (block_count > old_block_count)
		return -EINVAL;

	if (f2fs_is_multi_device(sbi)) {
		int last_dev = sbi->s_ndevs - 1;
		__u64 last_segs = FDEV(last_dev).total_segments;

		if (block_count + SEGS_TO_BLKS(sbi, last_segs) <=
								old_block_count)
			return -EINVAL;
	}

	/* new fs size should align to section size */
	div_u64_rem(block_count, BLKS_PER_SEC(sbi), &rem);
	if (rem)
		return -EINVAL;

	if (block_count == old_block_count)
		return 0;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
		f2fs_err(sbi, "Should run fsck to repair first.");
		return -EFSCORRUPTED;
	}

	if (test_opt(sbi, DISABLE_CHECKPOINT)) {
		f2fs_err(sbi, "Checkpoint should be enabled.");
		return -EINVAL;
	}

	err = mnt_want_write_file(filp);
	if (err)
		return err;

	shrunk_blocks = old_block_count - block_count;
	secs = div_u64(shrunk_blocks, BLKS_PER_SEC(sbi));

	/* stop other GC */
	if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
		err = -EAGAIN;
		goto out_drop_write;
	}

	/* stop CP to protect MAIN_SEC in free_segment_range */
	f2fs_lock_op(sbi);

	spin_lock(&sbi->stat_lock);
	if (shrunk_blocks + valid_user_blocks(sbi) +
		sbi->current_reserved_blocks + sbi->unusable_block_count +
		F2FS_OPTION(sbi).root_reserved_blocks > sbi->user_block_count)
		err = -ENOSPC;
	spin_unlock(&sbi->stat_lock);

	if (err)
		goto out_unlock;

	err = free_segment_range(sbi, secs, true);

out_unlock:
	f2fs_unlock_op(sbi);
	f2fs_up_write(&sbi->gc_lock);
out_drop_write:
	mnt_drop_write_file(filp);
	if (err)
		return err;

	err = freeze_super(sbi->sb);
	if (err)
		return err;

	if (f2fs_readonly(sbi->sb)) {
		thaw_super(sbi->sb);
		return -EROFS;
	}

	f2fs_down_write(&sbi->gc_lock);
	f2fs_down_write(&sbi->cp_global_sem);

	spin_lock(&sbi->stat_lock);
	if (shrunk_blocks + valid_user_blocks(sbi) +
		sbi->current_reserved_blocks + sbi->unusable_block_count +
		F2FS_OPTION(sbi).root_reserved_blocks > sbi->user_block_count)
		err = -ENOSPC;
	else
		sbi->user_block_count -= shrunk_blocks;
	spin_unlock(&sbi->stat_lock);
	if (err)
		goto out_err;

	set_sbi_flag(sbi, SBI_IS_RESIZEFS);
	err = free_segment_range(sbi, secs, false);
	if (err)
		goto recover_out;

	update_sb_metadata(sbi, -secs);

	err = f2fs_commit_super(sbi, false);
	if (err) {
		update_sb_metadata(sbi, secs);
		goto recover_out;
	}

	update_fs_metadata(sbi, -secs);
	clear_sbi_flag(sbi, SBI_IS_RESIZEFS);
	set_sbi_flag(sbi, SBI_IS_DIRTY);

	stat_inc_cp_call_count(sbi, TOTAL_CALL);
	err = f2fs_write_checkpoint(sbi, &cpc);
	if (err) {
		update_fs_metadata(sbi, secs);
		update_sb_metadata(sbi, secs);
		f2fs_commit_super(sbi, false);
	}
recover_out:
	clear_sbi_flag(sbi, SBI_IS_RESIZEFS);
	if (err) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		f2fs_err(sbi, "resize_fs failed, should run fsck to repair!");

		spin_lock(&sbi->stat_lock);
		sbi->user_block_count += shrunk_blocks;
		spin_unlock(&sbi->stat_lock);
	}
out_err:
	f2fs_up_write(&sbi->cp_global_sem);
	f2fs_up_write(&sbi->gc_lock);
	thaw_super(sbi->sb);
	return err;
}
