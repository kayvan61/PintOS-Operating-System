#include "filesys/inode.h"
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NEXT_BLOCKSIZE(x) (x % BLOCK_SECTOR_SIZE) ? (x + (BLOCK_SECTOR_SIZE - (x % BLOCK_SECTOR_SIZE))) : x;

bool inode_disk_add_sector(block_sector_t sector, struct inode_disk *inode, off_t offset);
void offset_to_arrIndex(off_t off, int* dirInd, int* indirOff, int* dIndirOff1, int* dIndirOff2);
bool fillZeros(block_sector_t, struct inode_disk *, int, int, int, int);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    int dir, indir, dIndir1, dIndir2;
    offset_to_arrIndex(pos, &dir, &indir, &dIndir1, &dIndir2);
    if(dir != -1) {
      return inode->data.direct[dir];
    }
    else if(indir != -1) {
      if(inode->data.indirect == 0xFFFFFFFF) {
	return -1;
      }      

      block_sector_t newBlock;
      block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
      if(!blocks) {
	PANIC("Heap ran out of space and couldnt allocate for indirect block buffer");
      }
      
      block_read(fs_device, inode->data.indirect, blocks);
      newBlock = blocks[indir];
	
      free(blocks);
      return newBlock;
    
    }
    else {
      block_sector_t newBlock;
      block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
      if(!blocks) {
	PANIC("Heap ran out of space and couldnt allocate for double indirect block buffer");
      }

      if(inode->data.doubleIndirect == 0xFFFFFFFF) {
	free(blocks);
	return -1;
      }
      
      block_sector_t secondLevelBlock;
      block_read (fs_device, inode->data.doubleIndirect, blocks);
      secondLevelBlock = blocks[dIndir1];
      if(secondLevelBlock == 0xFFFFFFFF) {
	free(blocks);
	return -1;
      }
      block_read (fs_device, secondLevelBlock, blocks);
      newBlock = blocks[dIndir2];
      free(blocks);
      return newBlock;
      
    }
  }
  else
    return -1;
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
inode_create (block_sector_t sector, off_t length, enum inode_type type)
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
      disk_inode->type   = type;
      disk_inode->magic  = INODE_MAGIC;
      disk_inode->indirect       = 0xFFFFFFFF;
      disk_inode->doubleIndirect = 0xFFFFFFFF;
      for(int i = 0; i < 10; i++) {
	disk_inode->direct[i] = 0xFFFFFFFF;
      }

      // new allocator
      for(off_t i = 0; i <= bytes_to_sectors(length); i++) {
	if(!inode_disk_add_sector(sector, disk_inode, i*BLOCK_SECTOR_SIZE)) {
	  success = false;
	  break;
	}
	else {
	  success = true;
	}
      }
      free(disk_inode);
    }
  return success;
}

/*
 * fill unalloced blocks with zero blocks up to 
 * the indexes provided
 */

bool fillZeros(block_sector_t sector, struct inode_disk *inode, int dir, int indir, int dIndir1, int dIndir2) {
  bool updated = false;
  int maxIndirInd;
  int maxDirInd;
  
  // which blocks need to be filled?
  if(dIndir1 != -1) {
    maxIndirInd = 128;
    maxDirInd   = 10;
  }
  else if(indir != -1) {
    maxIndirInd = indir;
    maxDirInd = 10;
  }
  else {
    maxDirInd = dir + 1;
  }

  // allocated temp buffer (bounce sorta)
  block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
  if(blocks == NULL) {
    PANIC("Ran out of heap for fillZeros");
  }

  // fill direct
  for(int i = 0; i < maxDirInd; i++) {
    if(inode->direct[i] == 0xFFFFFFFF) {
      if(free_map_allocate (1, &inode->direct[i])) {	  
	updated = true;
      }
      else {
	free(blocks);
	return false;
      }
    }
  }
  if(updated) {
    block_write (fs_device, sector, inode);
  }

  // fill indirect
  updated = false;
  if(maxIndirInd > 0) {
    if(inode->indirect == 0xFFFFFFFF) {
      if(free_map_allocate (1, &inode->indirect)) {	  	
	block_write (fs_device, sector, inode);
      }
      else {
	free(blocks);
	return false;
      }
    }
    block_read(fs_device, inode->indirect, blocks);
  }
  
  for (int i = 0; i < maxIndirInd; i++) {
    if(blocks[i] == 0xFFFFFFFF || blocks[i] == 0) {
      if(free_map_allocate (1, &blocks[i])) {
	updated = true;
      }
    }
  }
  
  if(updated) {
    block_write (fs_device, inode->indirect, blocks);
  }

  // fill double indirect
  for (int i = 0; i < dIndir1; i++) {
    updated = false;
    block_read(fs_device, inode->doubleIndirect, blocks);
    block_sector_t curBlock = blocks[i];

    if(curBlock == 0xFFFFFFFF) {
      if(free_map_allocate (1, &blocks[i])) {
	block_write (fs_device, inode->doubleIndirect, blocks);
	curBlock = blocks[i];
      }
      else {
	free(blocks);
	return false;
      }
    }
    ASSERT(curBlock != 0xFFFFFFFF);
    
    block_read(fs_device, curBlock, blocks);
    for(int j = 0; j < 128; j++) {
      if(blocks[j] == 0xFFFFFFFF || blocks[j] == 0) {
	if(free_map_allocate (1, &blocks[j])) {
	  updated = true;
	}
	else {
	  free(blocks);
	  return false;
	}
      }
    }
    
    if(updated) {
      block_write (fs_device, curBlock, blocks);
    }
  }

  // update last double indirect page
  updated = false;
  if(dIndir1 >= 0) {
    block_read(fs_device, inode->doubleIndirect, blocks);
    block_sector_t curBlock = blocks[dIndir1];
    ASSERT(curBlock != 0xFFFFFFFF);
  
    block_read(fs_device, curBlock, blocks);
    for(int i = 0; i < dIndir2; i++) {
      if(blocks[i] == 0xFFFFFFFF || blocks[i] == 0) {
	if(free_map_allocate (1, &blocks[i])) {
	  updated = true;
	}
	else {
	  free(blocks);
	  return false;
	}
      }
    }
  
    if(updated) {
      block_write (fs_device, curBlock, blocks);
    }
  }

  free(blocks);
  return true;
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
  block_read (fs_device, inode->sector, &inode->data);
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
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed) {
          free_map_release (inode->sector, 1);
	  
	  // iterate tables and release the blocks
	  // first direct 
	  for (int i = 0; i < 10; i++) {
	    if(inode->data.direct[i] != 0xFFFFFFFF) {
	      free_map_release (inode->data.direct[i], 1);
	    }
	    else {
	      free (inode);
	      return;
	    }
	  }

	  // check if any indirect exist
	  if(inode->data.indirect == 0xFFFFFFFF) {
	    free (inode);
	    return;
	  }

	  // iterate over indirects
	  block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
	  if(!blocks) {
	    PANIC("Heap ran out of space and couldnt allocate for indirect block buffer");
	  }
	  block_read(fs_device, inode->data.indirect, blocks);
	  
	  for (int i = 0; i < 128; i++) {
	    if(blocks[i] != 0xFFFFFFFF) {
	      free_map_release (blocks[i], 1);
	    }
	    else {
	      free (inode);
	      return;
	    }
	  }

	  // iterate over double indirect
	  block_sector_t* secondBlocks = malloc(sizeof(block_sector_t) * 128);
	  if(!blocks) {
	    PANIC("Heap ran out of space and couldnt allocate for indirect block buffer");
	  }
	  block_read(fs_device, inode->data.doubleIndirect, blocks);
	  
	  for (int i = 0; i < 128; i++) {
	    block_read(fs_device, blocks[i], secondBlocks);
	    for (int j = 0; j < 128; j++) {
	      if(secondBlocks[j] != 0xFFFFFFFF) {
		free_map_release (secondBlocks[j], 1);
	      }
	      else {
		free (inode);
		return;
	      }
	    }
	  }
      }
      free (inode);
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

void offset_to_arrIndex(off_t off, int* dirInd, int* indirOff, int* dIndirOff1, int* dIndirOff2) {
  if(off < BLOCK_SECTOR_SIZE * 10) {
    *dirInd = off / BLOCK_SECTOR_SIZE;
  }
  else if(off < (BLOCK_SECTOR_SIZE * 10) + BLOCK_SECTOR_SIZE * 128) {
    *dirInd = -1;
    *indirOff = (off - (BLOCK_SECTOR_SIZE * 10)) / BLOCK_SECTOR_SIZE;
  }
  else if(off < (BLOCK_SECTOR_SIZE * 10) + (BLOCK_SECTOR_SIZE * 128) + (BLOCK_SECTOR_SIZE * 128 * 128)) {
    *dirInd = -1;
    *indirOff = -1;
    *dIndirOff1 = (off - ((BLOCK_SECTOR_SIZE * 10) + (BLOCK_SECTOR_SIZE * 128))) / (BLOCK_SECTOR_SIZE * 128);
    *dIndirOff2 = (off - ((BLOCK_SECTOR_SIZE * 10) + (BLOCK_SECTOR_SIZE * 128) + *dIndirOff1 * 128)) / (BLOCK_SECTOR_SIZE);
  }
  else {
    PANIC("offset too large to get indexes");
  }
}

/*
 * Adds a single sector to inode_disk. called when a write goes beyond the length of the inode
 */

bool inode_disk_add_sector(block_sector_t sector, struct inode_disk *inode, off_t offset) {
  int direct, indirect, doubleIndir1, doubleIndir2;
  offset_to_arrIndex(offset, &direct, &indirect, &doubleIndir1, &doubleIndir2);

  if(direct != -1) {
    // add sector to direct array

    // get a free sector
    if(free_map_allocate (1, &inode->direct[direct])) {
      // update the metadata on disk 
      block_write (fs_device, sector, inode);
      if(!fillZeros(sector, inode, direct, -1, -1, -1)) {	
	return false;
      }
      return true;
    }
    else {
      PANIC("Allocating new sector failed");
    }
  }
  else if(indirect != -1) {
    // add sector to single indirect array

    // create the single indirect list if it doesnt exist
    if(inode->indirect == 0xFFFFFFFF) {
      
      if(free_map_allocate (1, &inode->indirect)) {
	// update the metadata on disk 
	block_write (fs_device, sector, inode);
      }
      else {
	PANIC("Allocating new sector failed");
      }
      
    }

    block_sector_t newBlock;
    block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
    if(!blocks) {
      PANIC("Heap ran out of space and couldnt allocate for indirect block buffer");
    }
    
    // get a free sector
    if(free_map_allocate (1, &newBlock)) {
      // update the metadata on disk
      block_read(fs_device, inode->indirect, blocks);
      blocks[indirect] = newBlock;
      block_write (fs_device, inode->indirect, blocks);
      if(!fillZeros(sector, inode, -1, indirect, -1, -1)) {
	free(blocks);
	return false;
      }
      free(blocks);
      return true;
    }
    else {
      PANIC("Allocating new sector failed");
    }
    
  }
  else {
    // add sector to double indirect array

    block_sector_t firstLevelBlock;
    block_sector_t newBlock;
    block_sector_t* blocks = malloc(sizeof(block_sector_t) * 128);
    if(!blocks) {
      PANIC("Heap ran out of space and couldnt allocate for double indirect block buffer");
    }
    
    // create the first level double indirect list if it doesnt exist
    if(inode->doubleIndirect == 0xFFFFFFFF) {
      memset(blocks, 0xFF, sizeof(block_sector_t) * 128);
      if(free_map_allocate (1, &inode->doubleIndirect)) {
	// update the metadata on disk 
	block_write (fs_device, sector, inode);
	block_write (fs_device, inode->doubleIndirect, blocks); // ensure that unalloced blocks are marked as unalloced
      }
      else {
	PANIC("Allocating new sector failed");
      }
    }

    // create the second level double indirect list if it doesnt exist
    block_read(fs_device, inode->doubleIndirect, blocks);  // get the first level of the table
    firstLevelBlock = blocks[doubleIndir1]; 
    
    if(firstLevelBlock == 0xFFFFFFFF) {
      block_sector_t tempBlock;
      if(free_map_allocate (1, &tempBlock)) {
	// update the metadata on disk
	blocks[doubleIndir1] = tempBlock;
	block_write (fs_device, inode->doubleIndirect, blocks);  // update the first level of the table
      }
      else {
	PANIC("Allocating new sector failed");
      }
    }
    
    // get a free sector
    if(free_map_allocate (1, &newBlock)) {
      // update the metadata on disk
      block_sector_t secondLevelBlock;
      block_read (fs_device, inode->doubleIndirect, blocks);
      secondLevelBlock = blocks[doubleIndir1];
      block_read (fs_device, secondLevelBlock, blocks);
      blocks[doubleIndir2] = newBlock;
      block_write (fs_device, secondLevelBlock, blocks);
      if(!fillZeros(sector, inode, -1, -1, doubleIndir1, doubleIndir2)) {
	free(blocks);
	return false;
      }
      free(blocks);
      return true;
    }
    else {
      PANIC("Allocating new sector failed");
    }
    
  }

  // unreachable
  return false;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  bool addedSector = false;
  if(offset + size > inode->data.length) {
    inode->data.length += offset + size - inode->data.length;
  }

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if(sector_idx == 0xFFFFFFFF || sector_idx == 0) {
	if(!inode_disk_add_sector(inode->sector, &inode->data, offset)) {
	  return 0;
	}
	sector_idx = byte_to_sector (inode, offset);
	addedSector = true;
      }
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

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
  return inode->data.length;
}
