/*
 * Compatibility functions which bloat the callers too much to make inline.
 * All of the callers of these functions should be converted to use folios
 * eventually.
 */

#include <linux/migrate.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include "internal.h"
#include <linux/jw_trace.h>

struct address_space *page_mapping(struct page *page)
{
	return folio_mapping(page_folio(page));
}
EXPORT_SYMBOL(page_mapping);

void unlock_page(struct page *page)
{
	return folio_unlock(page_folio(page));
}
EXPORT_SYMBOL(unlock_page);

void end_page_writeback(struct page *page)
{
	return folio_end_writeback(page_folio(page));
}
EXPORT_SYMBOL(end_page_writeback);

void wait_on_page_writeback(struct page *page)
{
	return folio_wait_writeback(page_folio(page));
}
EXPORT_SYMBOL_GPL(wait_on_page_writeback);

void wait_for_stable_page(struct page *page)
{
	return folio_wait_stable(page_folio(page));
}
EXPORT_SYMBOL_GPL(wait_for_stable_page);

bool page_mapped(struct page *page)
{
	return folio_mapped(page_folio(page));
}
EXPORT_SYMBOL(page_mapped);

void mark_page_accessed(struct page *page)
{
	folio_mark_accessed(page_folio(page));
}
EXPORT_SYMBOL(mark_page_accessed);

bool set_page_writeback(struct page *page)
{
	return folio_start_writeback(page_folio(page));
}
EXPORT_SYMBOL(set_page_writeback);

bool set_page_dirty(struct page *page)
{
	return folio_mark_dirty(page_folio(page));
}
EXPORT_SYMBOL(set_page_dirty);

int __set_page_dirty_nobuffers(struct page *page)
{
	return filemap_dirty_folio(page_mapping(page), page_folio(page));
}
EXPORT_SYMBOL(__set_page_dirty_nobuffers);

bool clear_page_dirty_for_io(struct page *page)
{
	return folio_clear_dirty_for_io(page_folio(page));
}
EXPORT_SYMBOL(clear_page_dirty_for_io);

bool redirty_page_for_writepage(struct writeback_control *wbc,
		struct page *page)
{
	return folio_redirty_for_writepage(wbc, page_folio(page));
}
EXPORT_SYMBOL(redirty_page_for_writepage);

void lru_cache_add(struct page *page)
{
	folio_add_lru(page_folio(page));
}
EXPORT_SYMBOL(lru_cache_add);

void lru_cache_add_inactive_or_unevictable(struct page *page,
		struct vm_area_struct *vma)
{
	folio_add_lru_vma(page_folio(page), vma);
}

int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
		pgoff_t index, gfp_t gfp)
{
	return filemap_add_folio(mapping, page_folio(page), index, gfp);
}
EXPORT_SYMBOL(add_to_page_cache_lru);

noinline
struct page *pagecache_get_page(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp)
{
	struct folio *folio;

	folio = __filemap_get_folio(mapping, index, fgp_flags, gfp);
	if ((fgp_flags & FGP_HEAD) || !folio || xa_is_value(folio))
		return &folio->page;
	return folio_file_page(folio, index);
}
EXPORT_SYMBOL(pagecache_get_page);

#ifdef GC_IO_PROFILE_TIMESERIAL
static inline unsigned long long OS_TimeGetNS_pcache_folio( void )
{
    //struct timespec64 lTime;
    ////ktime_get_coarse_real_ts64(&lTime);
    //ktime_get_real_ts64(&lTime);
    //return (lTime.tv_sec * 1000000000 + lTime.tv_nsec );
    return ktime_get_ns();
}
#endif

struct page *jw_pagecache_get_page(struct address_space *mapping, pgoff_t index,
#ifdef GC_IO_PROFILE_TIMESERIAL
		int fgp_flags, gfp_t gfp, struct lat_profiler *lat_profiler)
#else
		int fgp_flags, gfp_t gfp)
#endif
{
	struct folio *folio;
	struct page *ret_page;
#ifdef GC_IO_PROFILE_TIMESERIAL
	unsigned long long lat1, lat2, lat3;
	lat1 = OS_TimeGetNS_pcache_folio();
	folio = __jw_filemap_get_folio(mapping, index, fgp_flags, gfp, lat_profiler);
	lat2 = OS_TimeGetNS_pcache_folio();
	lat_profiler->filemap_get_folio_lat += lat2 - lat1;
#else
	folio = __jw_filemap_get_folio(mapping, index, fgp_flags, gfp);
#endif
	if ((fgp_flags & FGP_HEAD) || !folio || xa_is_value(folio))
		return &folio->page;
	
#ifdef GC_IO_PROFILE_TIMESERIAL
	ret_page = jw_folio_file_page(folio, index, lat_profiler);
	lat3 = OS_TimeGetNS_pcache_folio();
	lat_profiler->folio_file_page_lat += lat3 - lat2;
#else
	ret_page = jw_folio_file_page(folio, index);
#endif
	return ret_page;
	//return jw_folio_file_page(folio, index, lat_profiler);
}
EXPORT_SYMBOL(jw_pagecache_get_page);

struct page *jw_pagecache_try_get_page(struct address_space *mapping, pgoff_t index,
#ifdef GC_IO_PROFILE_TIMESERIAL
		int fgp_flags, gfp_t gfp, struct lat_profiler *lat_profiler)
#else
		int fgp_flags, gfp_t gfp)
#endif
{
	struct folio *folio;
	struct page *ret_page;
#ifdef GC_IO_PROFILE_TIMESERIAL
	unsigned long long lat1, lat2, lat3;
	lat1 = OS_TimeGetNS_pcache_folio();
	folio = __jw_filemap_try_get_folio(mapping, index, fgp_flags, gfp, lat_profiler);
	lat2 = OS_TimeGetNS_pcache_folio();
	lat_profiler->filemap_get_folio_lat += lat2 - lat1;
#else
	folio = __jw_filemap_try_get_folio(mapping, index, fgp_flags, gfp);
#endif
	if (!folio)
		return NULL;
	//if ((fgp_flags & FGP_HEAD) || !folio || xa_is_value(folio))
	if ((fgp_flags & FGP_HEAD) || xa_is_value(folio))
		return &folio->page;
	
#ifdef GC_IO_PROFILE_TIMESERIAL
	ret_page = jw_folio_file_page(folio, index, lat_profiler);
	lat3 = OS_TimeGetNS_pcache_folio();
	lat_profiler->folio_file_page_lat += lat3 - lat2;
#else
	ret_page = jw_folio_file_page(folio, index);
#endif
	return ret_page;
	//return jw_folio_file_page(folio, index, lat_profiler);
}
EXPORT_SYMBOL(jw_pagecache_try_get_page);

struct page *grab_cache_page_write_begin(struct address_space *mapping,
					pgoff_t index)
{
	unsigned fgp_flags = FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE;

	return pagecache_get_page(mapping, index, fgp_flags,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(grab_cache_page_write_begin);

struct page *jw_grab_cache_page_write_begin(struct address_space *mapping,
#ifdef GC_IO_PROFILE_TIMESERIAL
					pgoff_t index, struct lat_profiler *lat_profiler)
#else
					pgoff_t index)
#endif
{
	unsigned fgp_flags = FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE;
	
	return jw_pagecache_get_page(mapping, index, fgp_flags,
#ifdef GC_IO_PROFILE_TIMESERIAL
			mapping_gfp_mask(mapping), lat_profiler);
#else
			mapping_gfp_mask(mapping));
#endif
}
EXPORT_SYMBOL(jw_grab_cache_page_write_begin);

struct page *jw_try_grab_cache_page_write_begin(struct address_space *mapping,
#ifdef GC_IO_PROFILE_TIMESERIAL
					pgoff_t index, struct lat_profiler *lat_profiler)
#else
					pgoff_t index)
#endif
{
	unsigned fgp_flags = FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE;
	
	return jw_pagecache_try_get_page(mapping, index, fgp_flags,
#ifdef GC_IO_PROFILE_TIMESERIAL
			mapping_gfp_mask(mapping), lat_profiler);
#else
			mapping_gfp_mask(mapping));
#endif
}
EXPORT_SYMBOL(jw_try_grab_cache_page_write_begin);

void delete_from_page_cache(struct page *page)
{
	return filemap_remove_folio(page_folio(page));
}

int try_to_release_page(struct page *page, gfp_t gfp)
{
	return filemap_release_folio(page_folio(page), gfp);
}
EXPORT_SYMBOL(try_to_release_page);

int isolate_lru_page(struct page *page)
{
	if (WARN_RATELIMIT(PageTail(page), "trying to isolate tail page"))
		return -EBUSY;
	return folio_isolate_lru((struct folio *)page);
}

void putback_lru_page(struct page *page)
{
	folio_putback_lru(page_folio(page));
}
