#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

uint32_t NO_SECTOR = -1;

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE. If INODE does not contain data for a byte at 
   offset POS allocate it. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);

  block_sector_t *block_data;
  block_sector_t sector;

  size_t idx = pos / BLOCK_SECTOR_SIZE;
  struct cache *buff = byte_to_indirect_sector(buff, inode, &block_data, &sector, idx);
  if(sector == NO_SECTOR){
    return sector;
  }
  if (idx >= DIRECT_BLOCKS)
  {
    idx = pos - (BLOCK_SECTOR_SIZE * DIRECT_BLOCKS); 
    idx = idx / (BLOCKS_PER_SECTOR * BLOCK_SECTOR_SIZE);
    byte_to_double_indirect_sec(buff, &block_data, &sector, idx);
    if(sector == NO_SECTOR){
      return sector;
    }
    idx = pos - (BLOCK_SECTOR_SIZE * DIRECT_BLOCKS);
    idx = idx / BLOCK_SECTOR_SIZE % BLOCKS_PER_SECTOR;
    byte_to_double_indirect_sec(buff, &block_data, &sector, idx);  
  }
  return sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    disk_inode->length = length;
    disk_inode->is_dir = dir;

    struct cache *buff = set_block_data(sector, NULL, true, false);
    memcpy(buff->block_data, disk_inode, sizeof *disk_inode);
    free(disk_inode);
    success = true;
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  block_sector_t double_indirect_block;

  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
        set_inode_data(inode, false);
        block_sector_t single_indirect_block = inode->data->sectors[DIRECT_BLOCKS];
        for (int i = 0; i < DIRECT_BLOCKS; i++){
          if (inode->data->sectors[i] != 0)
            free_map_release(inode->data->sectors[i], 1);
        }

      if (single_indirect_block != 0)
      {
        for (int i = 0; i < BLOCKS_PER_SECTOR; i++){
          block_sector_t *block_data;
          struct cache *buff;
          buff = set_block_data(single_indirect_block, block_data, false, true);

          double_indirect_block = block_data[i];
          free_map_release(double_indirect_block, 1);
        }
        free_map_release(single_indirect_block, 1);
      }
    }
    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  struct cache *buff;
  
  if (offset >= inode_length(inode))
    return 0; 

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if (sector_idx == NO_SECTOR)
        break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      buff = set_block_data(sector_idx, NULL, false, false);
      memcpy(buffer + bytes_read, (uint8_t *)buff->block_data + sector_ofs,
            chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  start_read_ahead(inode, offset);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   file growth is done */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  struct cache *buff;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if (sector_idx == NO_SECTOR)
        break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = MAX_FILE_SIZE - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
    
      buff = set_block_data(sector_idx, NULL, true, false);
      memcpy((uint8_t *)buff->block_data + sector_ofs, buffer + bytes_written,
            chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  // update the length of the file if it was extended
  set_inode_data(inode, true);
  if (offset > inode->data->length)
    inode->data->length = offset;
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  set_inode_data(inode, false);
  return inode->data->length;
}

/* Allocate space for indirect sectors. */
struct cache *byte_to_indirect_sector(struct cache *buff, struct inode *inode, block_sector_t **block_data, block_sector_t *sector, size_t idx)
{
  if (idx >= DIRECT_BLOCKS)
    idx = DIRECT_BLOCKS;
  set_inode_data(inode, false);
  bool already_allocated = true;
  *sector = inode->data->sectors[idx];
  if (*sector == 0) {
    if (!free_map_allocate(1, sector)){
      sector = NO_SECTOR;
      return NULL;
    }
    already_allocated = false;
    inode->data->sectors[idx] = *(sector);
  }  
  set_block_data(*sector, block_data, false, true);
  if(!already_allocated)
    memset(*block_data, 0, BLOCK_SECTOR_SIZE);
  return buff;
}

/* Allocate space for double indirect sectors. */
void byte_to_double_indirect_sec(struct cache *buff, block_sector_t **block_data, block_sector_t *sector, size_t idx)
{
  block_sector_t new_sector;
  bool already_allocated = true;
  new_sector = *sector;
  *sector = (* block_data)[idx];
  if(*sector == 0){
      if (!free_map_allocate(1, sector)){
        sector = NO_SECTOR;
        return;
      } 
      already_allocated = false;
      set_block_data(new_sector, block_data, true, true);
      if(buff != NULL)
        *(*(block_data) + idx) = *(sector);
    }

  set_block_data(*sector, block_data, true, true);
  if(!already_allocated)
    memset(*block_data, 0, BLOCK_SECTOR_SIZE);
}

/* Set inode's data to the data that is inside the 
   buffer cache entry using the inode's disk sector. */
void set_inode_data(struct inode *inode, bool is_dirty){
  struct cache *buff = allocate_buffer(inode->sector);
  inode->data = (struct inode_disk *)buff->block_data;
  deallocate_buffer(buff, is_dirty);
}

/* Create buffer cache entry to hold data in SECTOR. If IS_DATA
   is true then BLOCK_DATA will hold the data in SECTOR. */
struct cache *set_block_data(block_sector_t sector, block_sector_t *block_data, bool is_dirty, bool is_data){
  struct cache *buff = allocate_buffer(sector);
  if (is_data)
    *block_data = (struct inode_disk *)buff->block_data;
  deallocate_buffer(buff, is_dirty);
  return buff;
}

/* Set the read next sector for the future read. */
void start_read_ahead(struct inode *inode, off_t offset) {
  if ((offset + BLOCK_SECTOR_SIZE - 1) < inode_length(inode))
  {
    block_sector_t sector_idx = byte_to_sector(inode, offset + BLOCK_SECTOR_SIZE - 1);
    if (sector_idx != NO_SECTOR)
      read_next_sector(sector_idx);
  }
}

/* Indicates if on disk inode represented by inode
   holds a directory. */
bool is_directory(struct inode *inode)
{
  set_inode_data(inode, false);
  return inode->data->is_dir;
}