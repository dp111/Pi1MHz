// cache.h

#ifndef CACHE_H
#define CACHE_H

// The first 2MB of memory is mapped at 4K pages so the 6502 Co Pro
// can play tricks with banks selection
#define NUM_4K_PAGES 512

void _clean_cache_area(void *start, unsigned int length);

void map_4k_page(unsigned int logical, unsigned int physical);

void enable_MMU_and_IDCaches(unsigned int num_4k_pages);

#endif
