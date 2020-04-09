#include "vm/swap.h"
#include "vm/frame.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/exception.h"
#include "threads/synch.h"

#define SECTORS_PER_PAGE PGSIZE / BLOCK_SECTOR_SIZE

static struct bitmap* swap_used_vector;
static struct block* swapBlock;
static struct hash swapTable;
static struct lock swap_lock; 

unsigned swapHash(const struct hash_elem *p_, void* aux);
bool swapLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux);

void swapInit() {
  swapBlock = block_get_role(BLOCK_SWAP);
  // size is in sectors which are 512 bytes each
  // a frame is 4096 bytes so divide by 8 to get the total number of swap slots
  swap_used_vector = bitmap_create(block_size(swapBlock) / 8);
  bitmap_set_all(swap_used_vector, 0);

  // table to keep track of swap table entries
  hash_init(&swapTable, swapHash, swapLess, NULL);

  lock_init(&swap_lock);
}


/*
  evit the page that currently in the frame that contains address kpage into swap 
  If swap is full then we panick the Kernel  
 */
int putPageIntoSwap(void *kpage) {
  if (bitmap_all(swap_used_vector, 0, block_size(swapBlock) / 8)) {
    PANIC("SWAP is full");
  }
  SwapTableEntry* potentialSTE = malloc(sizeof(SwapTableEntry));
  
  // find index of first free table
  size_t index_of_free_swap = bitmap_scan (swap_used_vector, 0, 1, 0);  
  ASSERT(index_of_free_swap != BITMAP_ERROR);

  // check if there is already a swap slot for this.
  potentialSTE->index = index_of_free_swap;
  
  struct hash_elem *e = hash_find(&swapTable, &potentialSTE->hashElem);
  ASSERT(e == NULL);

  // update the Swap PTE
  UserFrameTableEntry* frameEntry = frame_find_userframe_entry(kpage);
  potentialSTE->owner_tid = frameEntry->owner_tid;
  potentialSTE->upage = frameEntry->page_ptr;

  hash_insert(&swapTable, &potentialSTE->hashElem);

  bitmap_set(swap_used_vector, index_of_free_swap, 1);

  // each frame is 8 sectors large. write them into swap sequentially
  lock_acquire(&swap_lock);
  unsigned int kpageOff = 0;
  for(unsigned int i = 0; i < SECTORS_PER_PAGE; i++, kpageOff += BLOCK_SECTOR_SIZE) {
    block_write (swapBlock,
		 potentialSTE->index*SECTORS_PER_PAGE + i,
		 (char*)kpage + kpageOff);
  }
  lock_release(&swap_lock);
  return index_of_free_swap;
}

/* 
   get the page that belongs SPTE and put it into a free frame
   This assumes the page is actually in swap
 */
void* getPageFromSwap(SupPageEntry* SPTE, void* kpage) {

  ASSERT(SPTE->location == SWAP);

  // each frame is 8 sectors large. read them out of swap one by one
  lock_acquire(&swap_lock);
  unsigned int kpageOff = 0;
  for(unsigned int i = 0; i < SECTORS_PER_PAGE; i++, kpageOff += BLOCK_SECTOR_SIZE) {
    block_read (swapBlock,
		 SPTE->locationInSwap*SECTORS_PER_PAGE + i,
		 (char*)kpage + kpageOff);
  }
  lock_release(&swap_lock);
  
  SwapTableEntry scratch;
  scratch.index = SPTE->locationInSwap;
  struct hash_elem* e = hash_find(&swapTable, &scratch.hashElem);
  SwapTableEntry *curSTE = hash_entry(e, SwapTableEntry, hashElem);
  
  bitmap_set(swap_used_vector, SPTE->locationInSwap, 0);
  hash_delete(&swapTable, &curSTE->hashElem);

  free(curSTE);
  
  return kpage;
}


unsigned swapHash(const struct hash_elem *p_, void* aux) {
  const SwapTableEntry *p = hash_entry(p_, SwapTableEntry, hashElem);
  return hash_bytes(&p->index, sizeof(p->index));
}

bool swapLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux) {
  const SwapTableEntry *a = hash_entry(a_, SwapTableEntry, hashElem);
  const SwapTableEntry *b = hash_entry(b_, SwapTableEntry, hashElem);
  
  return a->index < b->index;
}
