#include <stdio.h>
#include <limits.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/file.h"
#include "devices/timer.h"
#include "filesys/cache.h"

static struct list cache_list;                  // Buffer cache list.
static struct lock lock;                        // Lock used for synchronization.
block_sector_t next_sector_to_read;             // Next disk sector to read for read-ahead.
bool next_sector_to_read_present;               // Indicates if there is a sector for read-ahead
static struct condition cache_cond;             // Used to signal cache is available if some process is waiting.

/* Initializes the buffer cache list. Also creates 
   2 threads used for read-ahead and write-back. */
void init_cache(void)
{   
  struct cache *cache;
  list_init(&cache_list);
  for (int i = 0; i < CACHE_SIZE; i++){
    cache = malloc(sizeof(struct cache));
    cache->disk_sector = UINT_MAX;
    cache->is_available = true;
    list_push_back(&cache_list, &cache->elem);
  }

  lock_init(&lock);
  cond_init(&cache_cond);

  thread_create("read_ahead", 0, read_ahead, NULL);
  thread_create("write_back", 0, write_back, NULL);
}

/* Finds the buffer cache entry associated with the
   provided sector. If none exist, then an available
   cache entry will be returned. */
struct cache *find_buffer(block_sector_t sector)
{
  struct cache *buf;
  struct cache *temp_buf = NULL;

  while (temp_buf == NULL) {
    struct list_elem *e = list_begin(&cache_list);
    while (e != list_end(&cache_list))
    {
      buf = list_entry(e, struct cache, elem);
      if (sector == buf->disk_sector)
        return buf;
      else if (buf->is_available)
      {
        if (buf->accessed)
          buf->accessed = false;
        else {
          if (temp_buf == NULL)
            temp_buf = buf;
        }
      }
      e = list_next(e);
    }
  }
  return temp_buf;
}

/* Allocate a buffer cache entry for the following
   disk sector and read sector data into cache. */
struct cache *allocate_buffer(block_sector_t sector)
{
  struct cache *buf;

  lock_acquire(&lock);
  buf = find_buffer(sector);
  if (sector == buf->disk_sector) {
    if (!buf->is_available)
      cond_wait(&cache_cond, &lock);
  }
  else {
    buf->is_available = false;
    if (buf->is_dirty) {
      // Need to first update the disk.
      buf->is_dirty = false;
      block_write(fs_device, buf->disk_sector, buf->block_data);
      cond_signal(&cache_cond, &lock);
    }
    // Read disk data into cache.
    buf->disk_sector = sector;
    block_read(fs_device, sector, buf->block_data);
    buf->is_available = true;
  }
  lock_release(&lock);
  buf->accessed = true;
  return buf;
}

/* Mark the buffer cache entry as available for use. */
void deallocate_buffer(struct cache *buf, bool is_dirty)
{
  lock_acquire(&lock);
  buf->is_available = false;
  if (is_dirty)
    buf->is_dirty = is_dirty;

  list_remove(&buf->elem);
  list_push_back(&cache_list, &buf->elem);
  buf->is_available = true;
  cond_signal(&cache_cond, &lock);
  lock_release(&lock);
}

/* Update the next_sector_to_read variable
   with the provided sector and indicates
   that the next sector is available for
   read ahead. */
void read_next_sector(block_sector_t sector)
{
  lock_acquire(&lock);
  next_sector_to_read = sector;
  next_sector_to_read_present = true;
  cond_signal(&cache_cond, &lock);
  lock_release(&lock);
}

/* Read ahead function. */
void read_ahead(void *aux UNUSED)
{
  while (true){
    lock_acquire(&lock);

    if (!next_sector_to_read_present)
      cond_wait(&cache_cond, &lock);
    next_sector_to_read_present = false;
    
    lock_release(&lock);

    allocate_buffer(next_sector_to_read);
  }
}

/* Write back function. */
void write_back(void *aux UNUSED)
{
  while (true){
    timer_msleep(WRITE_INTERVAL);
    write_dirty_to_disk();
  }
}

/* Write all dirty cache entry data to the
   relevant disk locations. */
void write_dirty_to_disk(void)
{
  struct list_elem *e = list_begin(&cache_list);
  while (e != list_end(&cache_list))
  {
    struct cache *buf = list_entry(e, struct cache, elem);
    if (buf->is_dirty) {
      if (!buf->is_available)
        cond_wait(&cache_cond, &lock);

      buf->is_available = false;
      buf->is_dirty = false;

      block_write(fs_device, buf->disk_sector, buf->block_data);
      deallocate_buffer(buf, false);
      e = list_begin(&cache_list);
    } else
      e = list_next(e);
  }
}
