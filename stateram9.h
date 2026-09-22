
#ifndef STATERAM9_H
#define STATERAM9_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*stateram9_recipe_fn)(
    uint64_t page_index,
    void *page_out,
    size_t page_size,
    void *user_ctx
);

typedef struct stateram9_region stateram9_region;

stateram9_region *stateram9_create(
    size_t logical_bytes,
    stateram9_recipe_fn recipe,
    void *user_ctx
);

void *stateram9_data(stateram9_region *r);
size_t stateram9_size(stateram9_region *r);

/*
 * Application must call checkpoint only while its own writers to this region
 * are quiescent/synchronized.
 *
 * For each dirty page:
 *   1) regenerate recipe base
 *   2) compare actual current page to recipe base
 *   3) encode one fresh compact delta (or full-page snapshot if dense)
 *   4) MADV_DONTNEED the raw page
 *
 * After this returns, the current logical value is represented by:
 *   recipe + at most one per-page delta/snapshot.
 */
int stateram9_checkpoint_deltas(stateram9_region *r);

/*
 * Drop recipe-clean or already-checkpointed pages.
 * Dirty uncheckpointed pages are preserved.
 */
int stateram9_dematerialize_safe(stateram9_region *r);

/*
 * Application has changed its recipe so it now reproduces the entire current
 * logical state. Clears all stored deltas and dirty bits.
 * Call only while writers are quiescent.
 */
int stateram9_rebase_recipe(stateram9_region *r);

/* Statistics */
uint64_t stateram9_missing_faults(stateram9_region *r);
uint64_t stateram9_write_faults(stateram9_region *r);
uint64_t stateram9_dirty_pages(stateram9_region *r);
uint64_t stateram9_delta_pages(stateram9_region *r);
uint64_t stateram9_snapshot_pages(stateram9_region *r);
uint64_t stateram9_representation_bytes(stateram9_region *r);



/*
 * StateRAM-12 range APIs.
 *
 * These operate on byte ranges but round safely to page boundaries.
 * They are the primitive used by segmented execution shadowing.
 */
int stateram9_materialize_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes
);

int stateram9_checkpoint_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes
);

int stateram9_dematerialize_safe_range(
    stateram9_region *r,
    size_t offset_bytes,
    size_t length_bytes
);

/* Force all logical pages resident by touching one byte per page. */
int stateram9_materialize_all(stateram9_region *r);

/* Approximate current resident page count using mincore(). */
uint64_t stateram9_resident_pages(stateram9_region *r);

/* Runtime page size. */
size_t stateram9_page_size(stateram9_region *r);

void stateram9_destroy(stateram9_region *r);

#ifdef __cplusplus
}
#endif
#endif
