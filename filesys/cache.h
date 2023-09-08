#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdint.h>
#include <list.h>
#include "devices/block.h"
#include "threads/synch.h"

#define CACHE_SIZE 64           // # of caches in buffer cache list.
#define WRITE_INTERVAL 100      // How often (time in ms) write behind happens.

struct cache
{
    uint8_t block_data[BLOCK_SECTOR_SIZE];      // Data cache is storing.
    block_sector_t disk_sector;                 // Disk sector cache points to.
    bool accessed;                              // Indicates if cache has been accessed recently.
    bool is_available;                          // Indicates if cache is available for access.
    bool is_dirty;                              // Indicates if block_data has been modified 
    struct list_elem elem;                      // List elem used for iteration
};

void init_cache(void);
struct cache *find_buffer(block_sector_t);
struct cache *allocate_buffer(block_sector_t);
void deallocate_buffer(struct cache *, bool);
void read_next_sector(block_sector_t);
void read_ahead(void *);
void write_back(void *);
void write_dirty_to_disk(void);

#endif