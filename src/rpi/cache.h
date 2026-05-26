/* cache.h */

#ifndef CACHE_H
#define CACHE_H

void _clean_cache_area(const void *start, unsigned int length);

void _invalidate_cache_area(const void * start, unsigned int length);

void map_4k_page(unsigned int logical, unsigned int physical);

void enable_MMU_and_IDCaches(unsigned int num_4k_pages);

#endif
