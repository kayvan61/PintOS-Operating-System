#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
static bool validName(const char*);
struct dir* walkPath(const char *name, const struct dir* pwd, char* final_name, bool*, bool*);

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

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size)
{
  if(!validName(name)) {
    return false;
  }
  block_sector_t inode_sector = 0;
  char final_name[NAME_MAX+1];
  bool isExist;
  struct dir *dir = walkPath(name, thread_current()->pwd, final_name, &isExist, NULL);
  bool success = (dir != NULL
		  && !isExist
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, FILE)
                  && dir_add (dir, final_name, inode_sector, FILE));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
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
  if(!validName(name)) {
    return NULL;
  }
  char final_name[NAME_MAX+1];
  struct dir *dir = walkPath(name, thread_current()->pwd, final_name, NULL, NULL);
  struct inode *inode = NULL;
  
  if (dir != NULL) {
    if(dir->inode->removed) {
      return NULL;
    }
    dir_lookup (dir, final_name, &inode);
  }
  else {
    return NULL;
  }
  
  dir_close (dir);
  
  return file_open (inode);
}

struct dir* filesys_open_dir(const char *name) {
   if(!validName(name)) {
    return NULL;
  }
  char final_name[NAME_MAX+1];
  bool isExist;
  struct dir *dir = walkPath(name, thread_current()->pwd, final_name, &isExist, NULL);
  if(!dir) {
    return NULL;
  }
  if(dir->inode->removed) {
    return NULL;
  }
  if(!isExist) {
    dir_close(dir);
    return NULL;
  }
  return dir;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  if(!validName(name)) {
    return false;
  }
  char final_name[NAME_MAX+1];
  bool isExist;
  bool isFile;
  bool success;
  struct inode *temp = malloc(sizeof(struct inode));
  struct dir *dir = walkPath(name, thread_current()->pwd, final_name, &isExist, &isFile);
  struct dir *prtDir = NULL;
  if(dir->inode->removed) {
    return false;
  }
  if(!dir_lookup(dir, final_name, &temp)) {
    prtDir = getParentDir(dir);
  }
  if(!isFile) {
    if(prtDir) {
      success = dir != NULL && isExist && dir_isEmpty(dir) && dir_remove (prtDir, final_name);
    }
    else {
      PANIC("WHAT");
    }
  }
  else {
    success = dir != NULL && isExist && dir_remove (dir, final_name);
  }
  dir_close (dir);

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

// walks a path and then returns a dir
// pointing to the last dir in that path
struct dir* walkPath(const char *name, const struct dir* pwd, char* final_name, bool* lastExist, bool* isFile) {
  char* currentToken = NULL;
  char* saveptr      = NULL;
  char* tempBuf      = malloc(sizeof(char) * (strlen(name) + 1));
  memcpy(tempBuf, name, sizeof(char) * (strlen(name) +1));
  struct dir* curDir = NULL;
  struct dir* tempDir = NULL;
  struct inode* temp = malloc(sizeof(struct inode));
  bool notExist = false;
  bool fileOnPath = false;
  bool usingPWD   = false;
  if (name[0] == '/' || pwd == NULL) {
    curDir = dir_open_root();
  }
  else {
    usingPWD = true;
    curDir = dir_reopen(pwd);
  }
  
  for (currentToken = strtok_r (tempBuf, "/", &saveptr); currentToken != NULL;
       currentToken = strtok_r (NULL, "/", &saveptr)) {

    // return the last token to the caller
    // since we cannot know the last one we
    // simply copy on every iteration
    if(final_name != NULL) {
      strlcpy(final_name, currentToken, NAME_MAX+1);
    }

    if(curDir->inode->removed) {
      notExist = true;
    }

    
    // something other than the last
    // token doesnt exist on the path
    // return null and handle the error in the caller
    if(notExist || fileOnPath) {
      if(final_name != NULL) {
	*final_name = '\0';
      }
      free(tempBuf);
      if(lastExist) {
	*lastExist = !notExist;
      }
      return NULL;
    }
    
    if(strcmp(currentToken, ".") == 0) {
      curDir = curDir;
    }
    else if(strcmp(currentToken, "..") == 0) {
      tempDir = getParentDir(curDir);
      if(usingPWD && curDir->inode->sector != pwd->inode->sector) {
	dir_close(curDir);
      }
      curDir = tempDir;
    }
    else {
      if(!dir_lookup (curDir, currentToken, &temp)) {
	// we already hit something that didnt exist and then we continued
	// return null
	// this means that something other than the last doesnt exist
	notExist = true;
      }
      else {
	if(temp->removed) {
	  notExist = true;
	}
	else if (temp->data.type == FILE) {
	  fileOnPath = true;
	}
	else {
	  if(usingPWD && curDir->inode->sector != pwd->inode->sector) {
	    dir_close(curDir);
	  }
	  curDir = dir_open( temp );
	}
      }
    }
  }

  // we didnt exit prematurely due to something not existing on
  // the path
  free(tempBuf);
  if(lastExist) {
    *lastExist = !notExist;
  }
  if(isFile) {
    *isFile  = fileOnPath;
  }
  return curDir;
}

struct dir* filesys_get_dir (const char *name, const struct dir* parent) {
  bool isExist;
  struct dir* ret = walkPath(name, parent, NULL, &isExist, NULL);
  if(!isExist) {
    dir_close(ret);
    return NULL;
  }
  return ret;
}

bool filesys_create_dir (const char *name, const struct dir* parent) {
  if(!validName(name)) {
    return false;
  }
  block_sector_t inode_sector = 0;
  char final_name[NAME_MAX+1]; // name at the end of tokenization
  struct dir* final_parent = walkPath(name, parent, final_name, NULL, NULL);
  bool success = (final_parent != NULL
                  && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, 16)
                  && dir_add (final_parent, final_name, inode_sector, DIR));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  
  dir_close(final_parent);
  return success;
}

static bool validName(const char* name) {
  if(*name == '\0') {
    return false;
  }
    
  char *currentToken = NULL;
  char *saveptr      = NULL;
  char *tempBuf      = malloc(sizeof(char) * (strlen(name) + 1));
  memcpy(tempBuf, name, (strlen(name) + 1) * sizeof(char));
  
  for (currentToken = strtok_r (tempBuf, "/", &saveptr); currentToken != NULL;
       currentToken = strtok_r (NULL, "/", &saveptr)) {
    if(strlen(currentToken) > NAME_MAX) {
      free(tempBuf);
      return false;
    }
  }
  free(tempBuf);
  return true;
}
