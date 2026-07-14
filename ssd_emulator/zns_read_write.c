// SPDX-License-Identifier: GPL-2.0-only

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"
#include <linux/ktime.h>

#include "jw_config.h"

#ifdef COPY_ENABLED
#include <linux/highmem.h>
#endif

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
unsigned long long OS_TimeGetUS( void )
{
    struct timespec64 lTime;
    ktime_get_coarse_real_ts64(&lTime);
    return (lTime.tv_sec * 1000000 + div_u64(lTime.tv_nsec, 1000) );

}

#define SEC_IN_USEC 1000000
#define MSEC_IN_USEC 1000
#define PRINT_TIME_SEC	1
#define WAF_TIME_INTERVAL	(500 * MSEC_IN_USEC)

extern bool copy_start;

static inline void print_CHIP_UTIL(struct nvmev_ns *ns)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	static bool first_passed = false;
	
	uint64_t avg_nand_idle_t, avg_nand_active_t;

	avg_nand_idle_t = ns->nand_idle_t_sum / (spp->nchs * spp->luns_per_ch);
	avg_nand_active_t = ns->nand_active_t_sum / (spp->nchs * spp->luns_per_ch);
	if (!first_passed && avg_nand_active_t == 0)
		return;
	first_passed = true;
	ns->avg_nand_idle_t_sum_total += avg_nand_idle_t;
	ns->avg_nand_active_t_sum_total += avg_nand_active_t;
	ns->nand_idle_t_sum = 0;
	ns->nand_active_t_sum = 0;
	if (avg_nand_idle_t + avg_nand_active_t > 0)
		printk("%s: util: %llu %% total util: %llu %% ", 
				__func__,  
				avg_nand_active_t * 100 / (avg_nand_idle_t + avg_nand_active_t), 
				ns->avg_nand_active_t_sum_total * 100 
					/ (ns->avg_nand_idle_t_sum_total + ns->avg_nand_active_t_sum_total)
		);	
}

static inline void try_print_CHIP_UTIL(struct nvmev_ns *ns) {
	unsigned long long cur_t = OS_TimeGetUS();

	if (cur_t - ns->last_t > WAF_TIME_INTERVAL) {
		print_CHIP_UTIL(ns);

		ns->last_t = cur_t;
	}
}

#endif

static inline uint32_t __nr_lbas_from_rw_cmd(struct nvme_rw_command *cmd)
{
	return cmd->length + 1;
}

static bool __check_boundary_error(struct zns_ftl *zns_ftl, uint64_t slba, uint32_t nr_lba)
{
	return lba_to_zone(zns_ftl, slba) == lba_to_zone(zns_ftl, slba + nr_lba - 1);
}

static void __increase_write_ptr(struct zns_ftl *zns_ftl, uint32_t zid, uint32_t nr_lba)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	uint64_t cur_write_ptr = zone_descs[zid].wp;
	uint64_t zone_capacity = zone_descs[zid].zone_capacity;

	cur_write_ptr += nr_lba;

	zone_descs[zid].wp = cur_write_ptr;

	if (cur_write_ptr == (zone_to_slba(zns_ftl, zid) + zone_capacity)) {
		//change state to ZSF
		release_zone_resource(zns_ftl, OPEN_ZONE);
		release_zone_resource(zns_ftl, ACTIVE_ZONE);

		if (zone_descs[zid].zrwav)
			ASSERT(0);

		change_zone_state(zns_ftl, zid, ZONE_STATE_FULL);
	} else if (cur_write_ptr > (zone_to_slba(zns_ftl, zid) + zone_capacity)) {
		NVMEV_ERROR("[%s] Write Boundary error!!\n", __func__);
	}
}

static inline struct ppa __lpn_to_ppa(struct zns_ftl *zns_ftl, uint64_t lpn)
{
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct znsparams *zpp = &zns_ftl->zp;
	uint64_t zone = lpn_to_zone(zns_ftl, lpn); // find corresponding zone
	uint64_t off = lpn - zone_to_slpn(zns_ftl, zone);

	uint32_t sdie = (zone * zpp->dies_per_zone) % spp->tt_luns;
	uint32_t die = sdie + ((off / spp->pgs_per_oneshotpg) % zpp->dies_per_zone);

	uint32_t channel = die_to_channel(zns_ftl, die);
	uint32_t lun = die_to_lun(zns_ftl, die);
	struct ppa ppa = {
		.g = {
			.lun = lun,
			.ch = channel,
			.pg = off % spp->pgs_per_oneshotpg,
		},
	};

	return ppa;
}

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
static bool __zns_write(struct nvmev_ns *ns, struct zns_ftl *zns_ftl, struct nvmev_request *req,
			struct nvmev_result *ret)
#else
static bool __zns_write(struct zns_ftl *zns_ftl, struct nvmev_request *req,
			struct nvmev_result *ret)
#endif
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct nvme_rw_command *cmd = &(req->cmd->rw);

	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);
	uint64_t slpn, elpn, lpn, zone_elpn;
	// get zone from start_lbai
	uint32_t zid = lba_to_zone(zns_ftl, slba);
	enum zone_state state = zone_descs[zid].state;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_xfer_completed = nsecs_start;
	uint64_t nsecs_latest = nsecs_start;
	uint32_t status = NVME_SC_SUCCESS;
	
	//copy_start = false;

	uint64_t pgs = 0;

	struct buffer *write_buffer;
#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
	static uint64_t first_req_nsecs_start_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
	static uint64_t succeed_req_nsecs_start_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
	static uint64_t nsecs_xfer_completed_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
	static int cmd_trial_cnt_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
#endif

#ifdef CHIP_UTIL
	if (zid > CHIP_UTIL_ZONE_COUNT - 1) {
		printk("%s: Need to increase CHIP_UTIL_ZONE_COUNT!!", __func__);
		NVMEV_ASSERT(0);
	}
	cmd_trial_cnt_of_zone[zid] ++;
	if (cmd_trial_cnt_of_zone[zid] == 1) {
		first_req_nsecs_start_of_zone[zid] = nsecs_start;
	}
#elif defined GC_REQ_CHIP_UTIL
	if (cmd->flags == 1 || cmd->flags == 3 || zns_ftl->ns->gcing) {
		if (zid > CHIP_UTIL_ZONE_COUNT - 1) {
			printk("%s: Need to increase CHIP_UTIL_ZONE_COUNT!!", __func__);
			NVMEV_ASSERT(0);
		}

		if (cmd->flags == 3) {
			printk("%s: REQ_GC_END", __func__);
		}

		cmd_trial_cnt_of_zone[zid] ++;
		if (cmd_trial_cnt_of_zone[zid] == 1) {
			first_req_nsecs_start_of_zone[zid] = nsecs_start;
		}
	}
#endif	

	if (cmd->opcode == nvme_cmd_zone_append) {
		slba = zone_descs[zid].wp;
		cmd->slba = slba;
	}

	slpn = lba_to_lpn(zns_ftl, slba);
	elpn = lba_to_lpn(zns_ftl, slba + nr_lba - 1);
	zone_elpn = zone_to_elpn(zns_ftl, zid);

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d opcode: %u \n", __func__, slba,
			nr_lba, zid, state, cmd->opcode);
//	printk("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d opcode: %u \n", __func__, slba,
//			nr_lba, zid, state, cmd->opcode);

	if (zns_ftl->zp.zone_wb_size)
		write_buffer = &(zns_ftl->zone_write_buffer[zid]);
	else
		write_buffer = zns_ftl->ssd->write_buffer;

	if (buffer_allocate(write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba)) {
		//printk ("%s: buffer allocate fail. zone_wb_size: %u", __func__, 
		//		zns_ftl->zp.zone_wb_size);
		return false;
	}


	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}

	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d sLBA: 0x%llx \n",
			    __func__, slba, nr_lba, zid, zns_ftl->zone_descs[zid].wp, state, slba * 512/1024/4);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	switch (state) {
	case ZONE_STATE_EMPTY: {
		// check if slba == start lba in zone
		if (slba != zone_descs[zid].zslba) {
			status = NVME_SC_ZNS_INVALID_WRITE;
			goto out;
		}

		if (is_zone_resource_full(zns_ftl, ACTIVE_ZONE)) {
			status = NVME_SC_ZNS_NO_ACTIVE_ZONE;
			goto out;
		}
		if (is_zone_resource_full(zns_ftl, OPEN_ZONE)) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}
		acquire_zone_resource(zns_ftl, ACTIVE_ZONE);
		// go through
	}
	case ZONE_STATE_CLOSED: {
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}

		// change to ZSIO
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_IMPL);
		break;
	}
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL: {
		break;
	}
	case ZONE_STATE_FULL:
		status = NVME_SC_ZNS_ERR_FULL;
		goto out;
	case ZONE_STATE_READ_ONLY:
		status = NVME_SC_ZNS_ERR_READ_ONLY;
		goto out;
	case ZONE_STATE_OFFLINE:
		status = NVME_SC_ZNS_ERR_OFFLINE;
		goto out;
	}

	__increase_write_ptr(zns_ftl, zid, nr_lba);

	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(zns_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

#ifdef CHIP_UTIL
	succeed_req_nsecs_start_of_zone[zid] = nsecs_start;
	nsecs_xfer_completed_of_zone[zid] = nsecs_xfer_completed;
	cmd_trial_cnt_of_zone[zid] = 0;
#elif defined GC_REQ_CHIP_UTIL
	if (cmd->flags == 1 || cmd->flags == 3 || zns_ftl->ns->gcing) {
		succeed_req_nsecs_start_of_zone[zid] = nsecs_start;
		nsecs_xfer_completed_of_zone[zid] = nsecs_xfer_completed;
		cmd_trial_cnt_of_zone[zid] = 0;
	}
#endif

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		struct ppa ppa;
		uint64_t pg_off;

		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));

		/* Aggregate write io in flash page */
		if (((pg_off + pgs) == spp->pgs_per_oneshotpg) || ((lpn + pgs - 1) == zone_elpn)) {
			struct nand_cmd swr = {
				.type = USER_IO,
				.cmd = NAND_WRITE,
				.stime = nsecs_xfer_completed,
				.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz,
				.interleave_pci_dma = false,
				.ppa = &ppa,
			};
			size_t bufs_to_release;
			uint32_t unaligned_space =
				zns_ftl->zp.zone_size % (spp->pgs_per_oneshotpg * spp->pgsz);
#ifdef CHIP_UTIL
			uint64_t nsecs_completed = ssd_advance_nand_chip_util(zns_ftl->ssd, &swr, 
					&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
					first_req_nsecs_start_of_zone[zid], 
					succeed_req_nsecs_start_of_zone[zid], 
					nsecs_xfer_completed_of_zone[zid]
					);
#elif defined GC_REQ_CHIP_UTIL
			uint64_t nsecs_completed;
			if (cmd->flags == 1 || cmd->flags == 3) {
				nsecs_completed = ssd_advance_nand_chip_util(zns_ftl->ssd, &swr, 
						&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
						first_req_nsecs_start_of_zone[zid], 
						succeed_req_nsecs_start_of_zone[zid], 
						nsecs_xfer_completed_of_zone[zid]
						);
			} else if (zns_ftl->ns->gcing) {
				nsecs_completed = ssd_advance_nand_chip_util_not_gc_req(zns_ftl->ssd, &swr, 
						&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
						first_req_nsecs_start_of_zone[zid], 
						succeed_req_nsecs_start_of_zone[zid], 
						nsecs_xfer_completed_of_zone[zid]
						);
			} else {
				nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
			}
#else
			uint64_t nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
#endif

			nsecs_latest = max(nsecs_completed, nsecs_latest);
			NVMEV_ZNS_DEBUG("%s Flush slba 0x%llx nr_lba 0x%llx zone_id %d state %d\n",
					__func__, slba, nr_lba, zid, state);

			if (((lpn + pgs - 1) == zone_elpn) && (unaligned_space > 0))
				bufs_to_release = unaligned_space;
			else
				bufs_to_release = spp->pgs_per_oneshotpg * spp->pgsz;

			schedule_internal_operation(req->sq_id, nsecs_completed, write_buffer,
						    bufs_to_release);
		}
	}

out:
	ret->status = status;
	if ((cmd->control & NVME_RW_FUA) ||
	    (spp->write_early_completion == 0)) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;
	
#if defined GC_REQ_CHIP_UTIL
	if (cmd->flags == 3) {
		zns_ftl->ns->gcing = false;
	}
#endif

	return true;
}


#ifdef COPY_ENABLED
static bool __zns_copy(struct zns_ftl *zns_ftl, struct nvmev_request *req,
			struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	
	/* JW added */	
	struct nvme_copy_cmd *cmd = &(req->cmd->cp);
	//struct nvme_command *cmd = req->cmd;
	//uint64_t nranges = cmd->cp.nr + 1; /* zero-based */
	uint64_t nranges = cmd->nr + 1; /* zero-based */
	u64 paddr = cmd->prp1;
	//u64 paddr = cmd->cp.prp1;
	void *vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
	struct nvme_copy_range *copy_range = (struct nvme_copy_range *) vaddr;
	/* JW added DONE */	

	uint64_t slba, elba;
	uint64_t slpn, elpn, lpn, zone_elpn;
//	for (int tmp_i = 0; tmp_i < nranges; tmp_i ++){
//		uint64_t srclba = copy_range[tmp_i].slba;
//		uint64_t dstlba = copy_range[tmp_i].dlba;
//		uint64_t src_lpn = srclba / spp->secs_per_pg;
//		uint64_t dst_lpn = dstlba / spp->secs_per_pg;
//		//if (srclba % spp->secs_per_pg || dstlba % spp->secs_per_pg) {
//		//	printk("%s: not aligned !!!", __func__);
//		//	NVMEV_ASSERT(0);
//		//}
//#ifdef JW_DBG_COPY
//		if (tmp_i == 0)
//			printk("%s: first src_lpn: 0x%llx dst_lpn: 0x%llx slba: 0x%llx dlba: 0x%llx secs_per_pg: %d", 
//				__func__, src_lpn, dst_lpn, srclba, dstlba, spp->secs_per_pg);
//		else if (tmp_i == nranges - 1)
//			printk("%s: last src_lpn: 0x%llx dst_lpn: 0x%llx slba: 0x%llx dlba: 0x%llx secs_per_pg: %d", 
//				__func__, src_lpn, dst_lpn, srclba, dstlba, spp->secs_per_pg);
//#endif
//	}
	
	uint64_t nr_lba = nranges * zns_ftl->ssd->sp.secs_per_pg;
#ifdef JW_DBG_COPY
	//printk("%s: nr_lba: %llu", __func__, nr_lba);
#endif
	// for write buffer allocation space check.  
	slba = copy_range[0].dlba;
	elba = copy_range[nranges-1].dlba;
	slpn = lba_to_lpn(zns_ftl, slba);
	//printk("%s: zone_size: %lu", __func__, (zns_ftl->zp.zone_size / zns_ftl->ssd->sp.pgsz));
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);
	uint32_t read_zid = lpn_to_zone(zns_ftl, lba_to_lpn(zns_ftl, copy_range[0].slba));
	enum zone_state state = zone_descs[zid].state;
	enum zone_state read_state = zone_descs[read_zid].state;
	uint32_t status = NVME_SC_SUCCESS;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = nsecs_start, nsecs_latest = 0, nsecs_latest_read; /* TODO. Check */
	uint64_t pgs = 0, pg_off;
	struct ppa ppa;
	struct nand_cmd swr;
	struct buffer *write_buffer;
	
	/*TODO buffer allocation space check. determine whether bef read or bef write.  */

	if (zns_ftl->zp.zone_wb_size)
		write_buffer = &(zns_ftl->zone_write_buffer[zid]);
	else
		write_buffer = zns_ftl->ssd->write_buffer;

	if (buffer_allocate(write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba)) {
		//printk ("%s: buffer allocate fail. zone_wb_size: %u", __func__, 
		//		zns_ftl->zp.zone_wb_size);
		kunmap_atomic(vaddr);
		return false;
	}


	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		printk("%s: write unit not aligned", __func__);
		NVMEV_ASSERT(0);
		goto out;
	}

	/* TODO: Assume that host copy command does not overflow next zone. */
	if (__check_boundary_error(zns_ftl, slba, elba-slba+1) == false) {
		// return boundary error
		printk("%s: BOUNDARY ERROR!!!", __func__);
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}

	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d sLBA: 0x%llx \n",
			    __func__, slba, nr_lba, zid, zns_ftl->zone_descs[zid].wp, state, slba * 512/1024/4);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	copy_start = true;
//#ifdef JW_DBG_COPY
	static int jw_cnt = 0;
	jw_cnt ++;
	if (jw_cnt % 100 == 0) {
		//printk("%s: cnt: %llu thread: %s %u", __func__, jw_cnt, current->comm, current->pid);
	}
//#endif
	
	/* Parsing */
	uint64_t contig_range_slba[265];
	uint64_t contig_range_elba[265];
	uint64_t prev_slba = copy_range[0].slba;
	uint64_t cur_range_slba = prev_slba;
	int contig_range_cnt = 0;
	unsigned int i;
	for (i = 1; i < nranges; i ++){
		slba = copy_range[i].slba;
		//if (slba / spp->secs_per_pg - prev_slba / spp->secs_per_pg > 1) {
		if (lba_to_lpn(zns_ftl, slba) - lba_to_lpn(zns_ftl, prev_slba) > 1) {
			/* discrete case */
			contig_range_slba[contig_range_cnt] = cur_range_slba;
			contig_range_elba[contig_range_cnt] = prev_slba;	
			contig_range_cnt ++;
			cur_range_slba = slba;
		} else {
			/* continuous case */
		}
		prev_slba = slba;
	}
	contig_range_slba[contig_range_cnt] = cur_range_slba;
	contig_range_elba[contig_range_cnt] = prev_slba;	
	contig_range_cnt ++;
	int idx = 0;
	int cur_range_ofs = 0;
	uint64_t complete_time_array[256] = {0};
	int tmp_i;
	uint64_t real_start_lpn, prev_lpn;
	
	

	/* ssd_advance_write_buffer with nrLBA = 0. (PCIe Command transver overhead) */
    	nsecs_latest = nsecs_start;
    	nsecs_latest = ssd_advance_write_buffer(zns_ftl->ssd, nsecs_latest, 0);


	NVMEV_ZNS_DEBUG(
		"%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx last lba 0x%llx\n",
		__func__, slba, nr_lba, zid, zone_descs[zid].state, zone_descs[zid].wp,
		(slba + nr_lba - 1));

	if (zone_descs[read_zid].state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE;
	} 

	// get delay from nand model
	//nsecs_latest = nsecs_start;
	if (LBA_TO_BYTE(nr_lba) <= KB(4))
		nsecs_latest += spp->fw_4kb_rd_lat;
	else
		nsecs_latest += spp->fw_rd_lat;

	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	/* Read Phase */
	for (idx = 0; idx < contig_range_cnt; idx ++){
		slba = contig_range_slba[idx];
		elba = contig_range_elba[idx];
		slpn = lba_to_lpn(zns_ftl, slba);
		elpn = lba_to_lpn(zns_ftl, elba);
	
		for (lpn = slpn; lpn <= elpn; lpn += pgs) {
			ppa = __lpn_to_ppa(zns_ftl, lpn);
			pg_off = ppa.g.pg % spp->pgs_per_flashpg;
			pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_flashpg - pg_off));
			swr.xfer_size = pgs * spp->pgsz;
			swr.ppa = &ppa;
			nsecs_completed = ssd_advance_nand_copy(zns_ftl->ssd, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
		}
	}


	/* write phase */

	//prev_slba = copy_range[0].dlba;
	//cur_range_slba = prev_slba;
	//contig_range_cnt = 0;
	//for (i = 1; i < nranges; i ++){
	//	slba = copy_range[i].dlba;
	//	if (slba / spp->secs_per_pg - prev_slba / spp->secs_per_pg > 1) {
	//		/* discrete case */
	//		contig_range_slba[contig_range_cnt] = cur_range_slba + 0x100 * spp->secs_per_pg;
	//		contig_range_elba[contig_range_cnt] = prev_slba + 0x100 * spp->secs_per_pg;	
	//		contig_range_cnt ++;
	//		cur_range_slba = slba;
	//	} else {
	//		/* continuous case */
	//	}
	//	prev_slba = slba;
	//}
	//contig_range_slba[contig_range_cnt] = cur_range_slba + 0x100 * spp->secs_per_pg;
	//contig_range_elba[contig_range_cnt] = prev_slba + 0x100 * spp->secs_per_pg;	
	//contig_range_cnt ++;

//	for (i = 0; i < contig_range_cnt; i ++){
//		slba = contig_range_slba[i];
//		elba = contig_range_elba[i];
//		start_lpn = slba / spp->secs_per_pg;
//		end_lpn = elba / spp->secs_per_pg;
//#ifdef COPY_DBG
//		//printk("%s: bef write-phase start_lpn: 0x%llx end_lpn: 0x%llx", __func__, 
//		//		start_lpn, end_lpn);
//#endif	
//	}


///////////////////////////// write /////////////////////////////

	slba = copy_range[0].dlba;
	// get zone from start_lbai
	pgs = 0;

	slpn = lba_to_lpn(zns_ftl, slba);
	elpn = lba_to_lpn(zns_ftl, slba + nr_lba - zns_ftl->ssd->sp.secs_per_pg);
	
	if (slpn + nranges - 1 != lba_to_lpn(zns_ftl, copy_range[nranges - 1].dlba)) {
		printk("slba: %llu nranges: %llu copy_range_elba: %llu", slpn, nranges, lba_to_lpn(zns_ftl, copy_range[nranges - 1].dlba));
		NVMEV_ASSERT(slpn + nranges - 1 == lba_to_lpn(zns_ftl, copy_range[nranges - 1].dlba));
	}
	zone_elpn = zone_to_elpn(zns_ftl, zid);

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d opcode: %u \n", __func__, slba,
			nr_lba*zns_ftl->ssd->sp.secs_per_pg, zid, state, cmd->opcode);
#ifdef JW_DBG_COPY
	printk("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d opcode: %u \n", __func__, slba,
			nr_lba, zid, state, cmd->opcode);
#endif

	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d sLBA: 0x%llx \n",
			    __func__, slba, nr_lba, zid, zns_ftl->zone_descs[zid].wp, state, slba * 512/1024/4);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	switch (state) {
	case ZONE_STATE_EMPTY: {
		// check if slba == start lba in zone
		if (slba != zone_descs[zid].zslba) {
			status = NVME_SC_ZNS_INVALID_WRITE;
			goto out;
		}

		if (is_zone_resource_full(zns_ftl, ACTIVE_ZONE)) {
			status = NVME_SC_ZNS_NO_ACTIVE_ZONE;
			goto out;
		}
		if (is_zone_resource_full(zns_ftl, OPEN_ZONE)) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}
		acquire_zone_resource(zns_ftl, ACTIVE_ZONE);
		// go through
	}
	case ZONE_STATE_CLOSED: {
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}

		// change to ZSIO
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_IMPL);
		break;
	}
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL: {
		break;
	}
	case ZONE_STATE_FULL:
		status = NVME_SC_ZNS_ERR_FULL;
		goto out;
	case ZONE_STATE_READ_ONLY:
		status = NVME_SC_ZNS_ERR_READ_ONLY;
		goto out;
	case ZONE_STATE_OFFLINE:
		status = NVME_SC_ZNS_ERR_OFFLINE;
		goto out;
	}

	__increase_write_ptr(zns_ftl, zid, nr_lba);

	// get delay from nand model
	nsecs_latest = ssd_advance_write_buffer_copy_write(zns_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	uint64_t nsecs_xfer_completed = nsecs_latest;

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		struct ppa ppa;
		uint64_t pg_off;

		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));

		/* Aggregate write io in flash page */
		if (((pg_off + pgs) == spp->pgs_per_oneshotpg) || ((lpn + pgs - 1) == zone_elpn)) {
			struct nand_cmd swr = {
				.type = USER_IO,
				.cmd = NAND_WRITE,
				.stime = nsecs_xfer_completed,
				.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz,
				.interleave_pci_dma = false,
				.ppa = &ppa,
			};
			size_t bufs_to_release;
			uint32_t unaligned_space =
				zns_ftl->zp.zone_size % (spp->pgs_per_oneshotpg * spp->pgsz);
			nsecs_completed = ssd_advance_nand_copy(zns_ftl->ssd, &swr);

			nsecs_latest = max(nsecs_completed, nsecs_latest);
			NVMEV_ZNS_DEBUG("%s Flush slba 0x%llx nr_lba 0x%llx zone_id %d state %d\n",
					__func__, slba, nr_lba, zid, state);

			if (((lpn + pgs - 1) == zone_elpn) && (unaligned_space > 0))
				bufs_to_release = unaligned_space;
			else
				bufs_to_release = spp->pgs_per_oneshotpg * spp->pgsz;

			schedule_internal_operation(req->sq_id, nsecs_completed, write_buffer,
						    bufs_to_release);
		}
	}

out:
	ret->status = status;
	if (spp->write_early_completion == 0) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = (nsecs_xfer_completed + req->nsecs_start) / 2;
		//ret->nsecs_target = nsecs_xfer_completed; //ori
	//printk("%s: __zns_copy done", __func__);
	kunmap_atomic(vaddr);
	return true;
}

#endif

static bool __zns_write_zrwa(struct zns_ftl *zns_ftl, struct nvmev_request *req,
			     struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct znsparams *zpp = &zns_ftl->zp;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);
	uint64_t elba = cmd->slba + nr_lba - 1;

	// get zone from start_lbai
	uint32_t zid = lba_to_zone(zns_ftl, slba);
	enum zone_state state = zone_descs[zid].state;

	uint64_t prev_wp = zone_descs[zid].wp;
	const uint32_t lbas_per_zrwa = zpp->lbas_per_zrwa;
	const uint32_t lbas_per_zrwafg = zpp->lbas_per_zrwafg;
	uint64_t zrwa_impl_start = prev_wp + lbas_per_zrwa;
	uint64_t zrwa_impl_end = prev_wp + (2 * lbas_per_zrwa) - 1;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = nsecs_start;
	uint64_t nsecs_xfer_completed = nsecs_start;
	uint64_t nsecs_latest = nsecs_start;
	uint32_t status = NVME_SC_SUCCESS;

	struct ppa ppa;
	struct nand_cmd swr;

	uint64_t nr_lbas_flush = 0, lpn, remaining, pgs = 0, pg_off;

	NVMEV_DEBUG(
		"%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx  buffer %lu\n",
		__func__, slba, nr_lba, zid, state, prev_wp, zrwa_impl_start, zrwa_impl_end,
		zns_ftl->zrwa_buffer[zid].remaining);

	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}

	// valid range : wp <=  <= wp + 2*(size of zwra) -1
	if (slba < zone_descs[zid].wp || elba > zrwa_impl_end) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp 0x%llx state %d\n",
			    __func__, slba, nr_lba, zid, zone_descs[zid].wp, state);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	switch (state) {
	case ZONE_STATE_CLOSED:
	case ZONE_STATE_EMPTY: {
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}

		if (!buffer_allocate(&zns_ftl->zrwa_buffer[zid], zpp->zrwa_size))
			NVMEV_ASSERT(0);

		// change to ZSIO
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_IMPL);
		break;
	}
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL: {
		break;
	}
	case ZONE_STATE_FULL:
		status = NVME_SC_ZNS_ERR_FULL;
		goto out;
	case ZONE_STATE_READ_ONLY:
		status = NVME_SC_ZNS_ERR_READ_ONLY;
		goto out;
	case ZONE_STATE_OFFLINE:
		status = NVME_SC_ZNS_ERR_OFFLINE;
		goto out;
#if 0
		case ZONE_STATE_EMPTY :
			return NVME_SC_ZNS_INVALID_ZONE_OPERATION;
#endif
	}

	if (elba >= zrwa_impl_start) {
		nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), lbas_per_zrwafg) *
				lbas_per_zrwafg;

		NVMEV_DEBUG("%s implicitly flush zid %d wp before 0x%llx after 0x%llx buffer %lu",
			    __func__, zid, prev_wp, zone_descs[zid].wp + nr_lbas_flush,
			    zns_ftl->zrwa_buffer[zid].remaining);
	} else if (elba == zone_to_elba(zns_ftl, zid)) {
		// Workaround. move wp to end of the zone and make state full implicitly
		nr_lbas_flush = elba - prev_wp + 1;

		NVMEV_DEBUG("%s end of zone zid %d wp before 0x%llx after 0x%llx buffer %lu",
			    __func__, zid, prev_wp, zone_descs[zid].wp + nr_lbas_flush,
			    zns_ftl->zrwa_buffer[zid].remaining);
	}

	if (nr_lbas_flush > 0) {
		if (!buffer_allocate(&zns_ftl->zrwa_buffer[zid], LBA_TO_BYTE(nr_lbas_flush)))
			return false;

		__increase_write_ptr(zns_ftl, zid, nr_lbas_flush);
	}
	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(zns_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	lpn = lba_to_lpn(zns_ftl, prev_wp);
	remaining = nr_lbas_flush / spp->secs_per_pg;
	/* Aggregate write io in flash page */
	while (remaining > 0) {
		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(remaining, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));

		if ((pg_off + pgs) == spp->pgs_per_oneshotpg) {
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = nsecs_xfer_completed;
			swr.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz;
			swr.interleave_pci_dma = false;
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed,
						    &zns_ftl->zrwa_buffer[zid],
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		lpn += pgs;
		remaining -= pgs;
	}

out:
	ret->status = status;

	if ((cmd->control & NVME_RW_FUA) ||
	    (spp->write_early_completion == 0)) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

bool zns_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slpn = lba_to_lpn(zns_ftl, cmd->slba);

	// get zone from start_lba
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);

	NVMEV_DEBUG("%s slba 0x%llx zone_id %d \n", __func__, cmd->slba, zid);

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
	try_print_CHIP_UTIL(ns);
#endif

	if (zone_descs[zid].zrwav == 0){
#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
		return __zns_write(ns, zns_ftl, req, ret);
#else
		return __zns_write(zns_ftl, req, ret);
#endif
	}
	else
		return __zns_write_zrwa(zns_ftl, req, ret);
}

#ifdef COPY_ENABLED
bool zns_copy(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slpn = lba_to_lpn(zns_ftl, cmd->slba);

	// get zone from start_lba
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);

	NVMEV_DEBUG("%s slba 0x%llx zone_id %d \n", __func__, cmd->slba, zid);

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
	try_print_CHIP_UTIL(ns);
#endif

	return __zns_copy(zns_ftl, req, ret);
}
#endif

bool zns_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct nvme_rw_command *cmd = &(req->cmd->rw);

	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);

	uint64_t slpn = lba_to_lpn(zns_ftl, slba);
	uint64_t elpn = lba_to_lpn(zns_ftl, slba + nr_lba - 1);
	uint64_t lpn;

	// get zone from start_lba
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);
	uint32_t status = NVME_SC_SUCCESS;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = nsecs_start, nsecs_latest = 0;
	uint64_t pgs = 0, pg_off;
	struct ppa ppa;
	struct nand_cmd swr;

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
	static uint64_t nsecs_start_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
	static uint64_t nsecs_fw_completed_of_zone[CHIP_UTIL_ZONE_COUNT] = {0};
#endif

#if defined CHIP_UTIL || defined GC_REQ_CHIP_UTIL
	try_print_CHIP_UTIL(ns);
#endif

#ifdef CHIP_UTIL
	if (zid > CHIP_UTIL_ZONE_COUNT - 1) {
		printk("%s: Need to increase CHIP_UTIL_ZONE_COUNT!!", __func__);
		NVMEV_ASSERT(0);
	}
	nsecs_start_of_zone[zid] = nsecs_start;
	
#elif defined GC_REQ_CHIP_UTIL
	if (cmd->flags == 2) {
		ns->gcing = true;
	}
	if (cmd->flags == 1 || cmd->flags == 2 || ns->gcing) {
		if (zid > CHIP_UTIL_ZONE_COUNT - 1) {
			printk("%s: Need to increase CHIP_UTIL_ZONE_COUNT!!", __func__);
			NVMEV_ASSERT(0);
		}
		nsecs_start_of_zone[zid] = nsecs_start;
		
		if (cmd->flags == 2) {
			printk("%s: REQ_GC_START", __func__);
		}
	}
#endif	

	NVMEV_ZNS_DEBUG(
		"%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx last lba 0x%llx\n",
		__func__, slba, nr_lba, zid, zone_descs[zid].state, zone_descs[zid].wp,
		(slba + nr_lba - 1));

	if (zone_descs[zid].state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE;
	} else if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
	}

	if (zone_descs[zid].state == ZONE_STATE_EMPTY) {
		/* read of a fully reset zone: host mapping points at a block whose
		 * zone was already reset -> stale-old-address read (scenario a) */
		printk("%s: read of EMPTY zone. zid: %u slba: 0x%llx nr_lba: 0x%llx wp: 0x%llx zslba: 0x%llx",
				__func__, zid, slba, nr_lba, zone_descs[zid].wp,
				zone_descs[zid].zslba);
	} else if (zone_descs[zid].wp < slba) {
		/* zid/wp/state distinguish stale-old-address reads (reset+rewritten
		 * victim zone) from reads of a GC destination zone whose data has
		 * not landed yet (scenario b) */
		printk("%s: zone read above write pointer. zid: %u slba: 0x%llx nr_lba: 0x%llx wp: 0x%llx zslba: 0x%llx state: %d",
				__func__, zid, slba, nr_lba, zone_descs[zid].wp,
				zone_descs[zid].zslba, zone_descs[zid].state);
	}

	// get delay from nand model
	nsecs_latest = nsecs_start;
	if (LBA_TO_BYTE(nr_lba) <= KB(4))
		nsecs_latest += spp->fw_4kb_rd_lat;
	else
		nsecs_latest += spp->fw_rd_lat;
	
#ifdef CHIP_UTIL
	nsecs_fw_completed_of_zone[zid] = nsecs_latest;
#elif defined GC_REQ_CHIP_UTIL
	if (cmd->flags == 1 || cmd->flags == 2 || ns->gcing) {
		nsecs_fw_completed_of_zone[zid] = nsecs_latest;
	}
#endif	

	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_flashpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_flashpg - pg_off));
		swr.xfer_size = pgs * spp->pgsz;
		swr.ppa = &ppa;
#ifdef CHIP_UTIL
		nsecs_completed = ssd_advance_nand_chip_util(zns_ftl->ssd, &swr, 
						&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
						nsecs_start_of_zone[zid], 
						nsecs_start_of_zone[zid], 
						nsecs_fw_completed_of_zone[zid]
						);
	
#elif defined GC_REQ_CHIP_UTIL
		if (cmd->flags == 1 || cmd->flags == 2) {
			nsecs_completed = ssd_advance_nand_chip_util(zns_ftl->ssd, &swr, 
							&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
							nsecs_start_of_zone[zid], 
							nsecs_start_of_zone[zid], 
							nsecs_fw_completed_of_zone[zid]
							);
		} else if (ns->gcing) {
			nsecs_completed = ssd_advance_nand_chip_util_not_gc_req(zns_ftl->ssd, &swr, 
							&(ns->nand_idle_t_sum), &(ns->nand_active_t_sum), 
							nsecs_start_of_zone[zid], 
							nsecs_start_of_zone[zid], 
							nsecs_fw_completed_of_zone[zid]
							);
		} else {
			nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
		}
#else
		nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
#endif
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	if (swr.interleave_pci_dma == false) {
		nsecs_completed = ssd_advance_pcie(zns_ftl->ssd, nsecs_latest, nr_lba * spp->secsz);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	ret->status = status;
	ret->nsecs_target = nsecs_latest;
	return true;
}
