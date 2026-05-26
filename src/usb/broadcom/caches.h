/*
 * Broadcom BCM283x cache management for TinyUSB
 *
 * This file provides cache operations that map to the existing
 * Pi1MHz cache control API.
 */

#ifndef BROADCOM_CACHES_H
#define BROADCOM_CACHES_H

#include <stdint.h>
#include "../../rpi/cache.h"
#include "../../rpi/arm-start.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Clean (write-back) data cache for a memory region
 * @param addr Starting address of the memory region
 * @param size Size of the memory region in bytes
 *
 * This ensures any modified cache lines are written back to memory.
 * Use before DMA transfers from memory.
 */
static inline void data_clean(void* addr, size_t size)
{
    _clean_cache_area(addr, (unsigned int)size);
}

/**
 * @brief Invalidate data cache for a memory region
 * @param addr Starting address of the memory region
 * @param size Size of the memory region in bytes
 *
 * This discards cache lines without writing them back.
 * Use after DMA transfers to memory.
 */
static inline void data_invalidate(void* addr, size_t size)
{
    _invalidate_cache_area(addr, (unsigned int)size);
}

/**
 * @brief Clean and invalidate data cache for a memory region
 * @param addr Starting address of the memory region
 * @param size Size of the memory region in bytes
 *
 * This writes back modified cache lines and then discards them.
 * Use for bidirectional DMA or when cache state is uncertain.
 */
static inline void data_clean_and_invalidate(void* addr, size_t size)
{
    _clean_cache_area(addr, (unsigned int)size);
    _invalidate_cache_area(addr, (unsigned int)size);
}

/**
 * @brief Initialize cache subsystem
 *
 * This is typically called during board initialization.
 * Maps to existing Pi1MHz cache initialization if needed.
 */
static inline void init_caches(void)
{
    /* Cache initialization is handled by Pi1MHz startup code */
    /* enable_MMU_and_IDCaches() is called during boot */
}

/**
 * @brief Setup MMU with flat mapping
 *
 * This is typically called during board initialization.
 */
static inline void setup_mmu_flat_map(void)
{
    /* MMU setup is handled by Pi1MHz startup code */
    /* enable_MMU_and_IDCaches() configures the page tables */
}

#ifdef __cplusplus
}
#endif

#endif /* BROADCOM_CACHES_H */
