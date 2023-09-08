#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "filesys/cache.h"
#include "filesys/inode.h"
#include "filesys/file.h"

const char *trace_path(const char *path, bool *too_long, char *dir_name);

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct file *file, char name[NAME_MAX + 1])
{
  struct dir_entry dir;
  bool success = false;

  if(file->pos == 0){
    // . should not be considered
    file->pos += sizeof dir;
    // .. should not be considered
    file->pos += sizeof dir;
  }

  while (inode_read_at(file->inode, &dir, sizeof dir, file->pos) == sizeof dir)
  {
    file->pos += sizeof dir;
    if (dir.in_use){
      strlcpy(name, dir.name, NAME_MAX + 1);
      success = true;
      break;
    }
  }
  return success;
}

/* Finds the first directory in the given path. DIR_NAME will be set to
   the directory's name. If the found directory name exceeds the max name
   length, then TOO_LONG will be set to true and DIR_NAME will be the first
   NAME_MAX values of the directory name.*/
const char *trace_path(const char *path, bool *too_long, char *dir_name)
{
  const char *traced_path = path;
  *too_long = false;
  int i = 0;
  if (traced_path[i] == '\0')
    return NULL;

  while (traced_path[i] == '/')
    i++;

  if (traced_path[i] == '\0')
    return NULL;
    
  int first_char_index = i;
  while (traced_path[i] != '/' && traced_path[i] != '\0'){
    i++;
  }

  if (&traced_path[i] - &traced_path[first_char_index] > NAME_MAX){
    *too_long = true;
    memcpy(dir_name, &traced_path[first_char_index], NAME_MAX);
    dir_name[NAME_MAX] = '\0';
  }else{
  memcpy(dir_name, &traced_path[first_char_index], &traced_path[i] - &traced_path[first_char_index]);
  dir_name[&traced_path[i] - &traced_path[first_char_index]] = '\0';
  }

  return &traced_path[i];
}

/* Returns the first directory found from the specified PATH.
   FILE_NAME will be set the the directory name. IS_PARENT indicates
   if the parent directory is calling the function.*/
struct dir *retrieve_dir_from_location(const char *path, char *file_name, bool is_parent)
{
  struct dir *dir;
  struct inode *temp = NULL;
  bool too_long = false;

  if(*path == '\0')
    return NULL;
  int path_empty = (*path == '/');
  switch(path_empty){
    case 1:
      dir = dir_open(inode_open(ROOT_DIR_SECTOR));
      break;
    default:
      dir = dir_open(inode_reopen(thread_current()->curr_inode));
      break;
  }

  while(true)
  {
    path = trace_path(path, &too_long, file_name);

    if(path == NULL)
      break;
    if(too_long)
      break;
    if (dir->inode->removed){
      return NULL;
    }
    else if (is_parent && *path == '\0')
      return dir;
    else{
      bool found = dir_lookup(dir, file_name, &temp);
      inode_close(dir->inode);

      if (!found)
        return NULL;
      dir->inode = temp;
    }
  }
  if (is_parent)
    return NULL;
  return dir;
}

/* Indicates if directory DIR is empty. A directory is
   empty if none of the associated inodes are in use. */
bool is_empty_dir(struct inode *dir)
{
  struct dir_entry e;
  for(size_t ofs = 2 * sizeof(e); inode_read_at(dir, &e, sizeof(e), ofs) == sizeof(e); ofs += sizeof e){
    if (e.in_use)
       return false;
  }
  return true;
}

/* Initializes root directory. */
void root_dir_init(void){
  struct dir *dir = dir_open(inode_open(ROOT_DIR_SECTOR));
  lock_acquire(&dir->inode->lock);
  dir_add(dir, ".", ROOT_DIR_SECTOR);
  dir_add(dir, "..", ROOT_DIR_SECTOR);
  lock_release(&dir->inode->lock);
}
