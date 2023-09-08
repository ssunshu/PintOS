#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"
#include <list.h>

#define MAX_FILE_SIZE 8127488
#define DIRECT_BLOCKS 124
#define BLOCKS_PER_SECTOR 125

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;         /* File size in bytes. */
  bool is_dir;          /* If inode represents a directory. */
  
  /* direct blocks(124), 1 indirect_block, 1 doubly indirect block. */
  block_sector_t sectors[DIRECT_BLOCKS + 2];
};

/* In-memory inode. */
struct inode
{
    struct list_elem elem;   /* Element in inode list. */
    block_sector_t sector;   /* Sector number of disk location. */
    int open_cnt;            /* Number of openers. */
    bool removed;            /* True if deleted, false otherwise. */
    int deny_write_cnt;      /* 0: writes ok, >0: deny writes. */
    struct inode_disk *data; /* Inode content. */
    struct lock lock;        /* Used for synchronization. */
};

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

struct cache *byte_to_indirect_sector(struct cache *, struct inode *, block_sector_t **, block_sector_t *, size_t);
void byte_to_double_indirect_sec(struct cache *, block_sector_t **, block_sector_t *, size_t);
void set_inode_data(struct inode *, bool);
struct cache *set_block_data(block_sector_t, block_sector_t *, bool, bool);
void start_read_ahead(struct inode *, off_t);
bool is_directory(struct inode *);

#endif /* filesys/inode.h */
