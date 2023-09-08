#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  init_cache();
  if (format) 
    do_format ();

  free_map_open ();
  root_dir_init ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  write_dirty_to_disk();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char *file_name = malloc(sizeof(char) * (NAME_MAX + 1));
  struct dir *dir = retrieve_dir_from_location(name, file_name, true);
  if(dir == NULL)
    return false;

  lock_acquire(&dir->inode->lock);
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, file_name, inode_sector));

  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  
  lock_release(&dir->inode->lock);
  free(file_name);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char *file_name = malloc(sizeof(char) * (NAME_MAX + 1));
  struct dir *dir = retrieve_dir_from_location(name, file_name, false);
  free(file_name);

  if(dir == NULL)
    return NULL;

  while (*(name + 1) != '\0'){
    name++;
  }

  if (*name == '/' && !is_directory(dir->inode)){
    dir_close(dir);
    return NULL;
  }

  return file_open(dir->inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  bool success = false;
  char *file_name = malloc(sizeof(char) * (NAME_MAX + 1));
  struct dir *dir = retrieve_dir_from_location(name, file_name, true);
  struct inode *file = NULL;

  if (dir == NULL){
    free(file_name);
    return false;
  }

  lock_acquire(&dir->inode->lock);

  if (!dir_lookup(dir, file_name, &file))
    goto done;

  if (is_directory(file) && !is_empty_dir(file))
    goto done;

  dir_remove(dir, file_name);
  success = true;

  done:
    lock_release(&dir->inode->lock);

    if (file != NULL)
      inode_close(file);
    dir_close(dir);
    free(file_name);
    return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
